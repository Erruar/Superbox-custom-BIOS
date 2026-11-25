/*
 * gfx.c - framebuffer backend for SbBios.
 *
 * All drawing happens in a 0x00RRGGBB back buffer transferred with a single
 * GOP Blt per frame.  Text uses the baked 1bpp tables from fontdata.h: a
 * glyph pixel is one store, no alpha blending, so a full screen redraw is
 * cheap even on the box's slow GOP (the previous HII-font renderer blended
 * every pixel and crawled).
 *
 * "Hand drawn" strokes are polylines with a deterministic wobble (seeded
 * LCG), so the frame looks sketchy on paper but never shimmers between
 * redraws.
 */

#include "SbBios.h"

#define CURSOR_W  11
#define CURSOR_H  16

STATIC CONST UINT8  mCursorShape[CURSOR_H] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 11, 11, 8, 6, 4
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop = NULL;
STATIC UINT32                        *mBack = NULL;
STATIC UINT32                        *mStage = NULL;      /* R/B swap copy  */
STATIC UINTN                          mPitch = 0;
STATIC UINT32                         mWidth = 0;
STATIC UINT32                         mHeight = 0;
STATIC INT32                          mUi = 1;
STATIC BOOLEAN                        mSwapRb = FALSE;
STATIC INT32                          mCurX = 0, mCurY = 0;
STATIC BOOLEAN                        mCurVisible = FALSE;
STATIC INT32                          mCursorX0, mCursorY0, mCursorW, mCursorH;
STATIC INT32                          mCursorS = 1;
STATIC UINT32                         mCursorSave[4096];

/* ------------------------------------------------------------- primitives */

STATIC
UINT32
SwapRb (
  IN UINT32  Color
  )
{
  return (((Color & 0x00FF0000) >> 16) | (Color & 0x0000FF00) |
          ((Color & 0x000000FF) << 16));
}

STATIC
VOID
Plot (
  IN INT32   X,
  IN INT32   Y,
  IN UINT32  Color
  )
{
  if ((X >= 0) && (Y >= 0) && ((UINT32)X < mWidth) && ((UINT32)Y < mHeight)) {
    mBack[(UINTN)Y * mPitch + (UINTN)X] = mSwapRb ? SwapRb (Color) : Color;
  }
}

VOID
GfxClear (
  IN UINT32  Color
  )
{
  UINTN   Index;
  UINTN   Count = (UINTN)mWidth * mHeight;
  UINT32  C     = mSwapRb ? SwapRb (Color) : Color;

  for (Index = 0; Index < Count; Index++) {
    mBack[Index] = C;
  }
}

VOID
GfxFill (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   W,
  IN INT32   H,
  IN UINT32  Color
  )
{
  INT32  I, J;

  for (J = Y; J < Y + H; J++) {
    for (I = X; I < X + W; I++) {
      Plot (I, J, Color);
    }
  }
}

VOID
GfxFrame (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   W,
  IN INT32   H,
  IN INT32   T,
  IN UINT32  Color
  )
{
  INT32  I;

  for (I = 0; I < T; I++) {
    GfxFill (X, Y + I, W, 1, Color);
    GfxFill (X, Y + H - 1 - I, W, 1, Color);
    GfxFill (X + I, Y, 1, H, Color);
    GfxFill (X + W - 1 - I, Y, 1, H, Color);
  }
}

/* ------------------------------------------------------------ baked fonts */

STATIC
CONST SB_GLYPH *
FindGlyph (
  IN CONST SB_GLYPH  *Tab,
  IN UINTN           Count,
  IN UINT16          Code
  )
{
  UINTN  I;

  for (I = 0; I < Count; I++) {
    if (Tab[I].Code == Code) {
      return &Tab[I];
    }
  }
  return NULL;
}

STATIC
VOID
BlitGlyph (
  IN INT32             X,
  IN INT32             BaseY,
  IN CONST SB_GLYPH    *G,
  IN CONST UINT8       *Bits,
  IN UINT32            Color,
  IN INT32             Scale
  )
{
  UINTN  RowBytes = (G->W + 7) / 8;
  UINTN  I, J;
  INT32  Ox = X + (INT32)G->Dx * Scale;
  INT32  Oy = BaseY + (INT32)G->Dy * Scale;

  for (I = 0; I < G->H; I++) {
    for (J = 0; J < G->W; J++) {
      if (Bits[G->Off + I * RowBytes + (J / 8)] & (0x80 >> (J % 8))) {
        if (Scale == 1) {
          Plot (Ox + (INT32)J, Oy + (INT32)I, Color);
        } else {
          GfxFill (Ox + (INT32)J * Scale, Oy + (INT32)I * Scale,
                   Scale, Scale, Color);
        }
      }
    }
  }
}

INT32
GfxTextW (
  IN CONST SB_GLYPH  *Tab,
  IN UINTN           Count,
  IN CONST CHAR16    *S,
  IN INT32           Scale
  )
{
  INT32  Total = 0;

  while (S != NULL && *S != 0) {
    CONST SB_GLYPH  *G = FindGlyph (Tab, Count, *S++);

    if (G != NULL) {
      Total += (INT32)G->Adv * Scale;
    }
  }
  return Total;
}

/* Draws S with its baseline at BaseY; returns the drawn width. */
INT32
GfxText (
  IN INT32             X,
  IN INT32             BaseY,
  IN CONST SB_GLYPH    *Tab,
  IN UINTN             Count,
  IN CONST UINT8       *Bits,
  IN CONST CHAR16      *S,
  IN UINT32            Color,
  IN INT32             Scale
  )
{
  INT32  Pen = X;

  while (S != NULL && *S != 0) {
    CONST SB_GLYPH  *G = FindGlyph (Tab, Count, *S++);

    if (G != NULL) {
      BlitGlyph (Pen, BaseY, G, Bits, Color, Scale);
      Pen += (INT32)G->Adv * Scale;
    }
  }
  return Pen - X;
}

/* Max ink height above the baseline over the whole table (one scan). */
INT32
GfxAscent (
  IN CONST SB_GLYPH  *Tab,
  IN UINTN           Count
  )
{
  INT32  I, Best = 0;

  for (I = 0; I < (INT32)Count; I++) {
    if ((INT32)-Tab[I].Dy > Best) {
      Best = -(INT32)Tab[I].Dy;
    }
  }
  return Best;
}

/* ---------------------------------------------------------- vector shapes */

VOID
GfxPolyFill (
  IN CONST INT32  (*Pts)[2],
  IN UINTN        Count,
  IN UINT32       Color
  )
{
  INTN   I, K, J, MinY, MaxY, N;
  INTN   Xs[32];

  if (Count < 3 || Count > 32) {
    return;
  }
  MinY = MaxY = Pts[0][1];
  for (I = 0; I < (INTN)Count; I++) {
    if (Pts[I][1] < MinY) {
      MinY = Pts[I][1];
    }
    if (Pts[I][1] > MaxY) {
      MaxY = Pts[I][1];
    }
  }

  for (J = MinY; J <= MaxY; J++) {
    N = 0;
    for (K = 0; K < (INTN)Count; K++) {
      INTN  Y1 = Pts[K][1];
      INTN  Y2 = Pts[(K + 1) % Count][1];

      if (((Y1 <= J) && (Y2 > J)) || ((Y2 <= J) && (Y1 > J))) {
        Xs[N++] = Pts[K][0] +
                  (J - Y1) * (Pts[(K + 1) % Count][0] - Pts[K][0]) / (Y2 - Y1);
      }
    }
    for (I = 0; I < N; I++) {
      for (K = I + 1; K < N; K++) {
        if (Xs[K] < Xs[I]) {
          INTN  T = Xs[I];

          Xs[I] = Xs[K];
          Xs[K] = T;
        }
      }
    }
    if (N >= 2) {
      for (I = Xs[0]; I <= Xs[N - 1]; I++) {
        Plot ((INT32)I, J, Color);
      }
    }
  }
}

VOID
GfxCircle (
  IN INT32    Cx,
  IN INT32    Cy,
  IN INT32    R,
  IN UINT32   Color,
  IN BOOLEAN  Fill
  )
{
  INT32  I, J;

  for (J = -R; J <= R; J++) {
    for (I = -R; I <= R; I++) {
      INT32  D2 = I * I + J * J;

      if (Fill ? (D2 <= R * R)
               : (D2 <= R * R && D2 > (R - 2) * (R - 2))) {
        Plot (Cx + I, Cy + J, Color);
      }
    }
  }
}

/*
 * Gear icon: 8-tooth polygon in Ink with a disc of Hole color punched into
 * the middle (the mockup's cog = black gear, paper-colored center).
 * sin/cos come from a 10-degree table scaled by 1000 - UEFI apps run
 * without FPU state, so all math here is integer.
 */
STATIC CONST INT32  mSin1000[10] = {
  0, 174, 342, 500, 643, 766, 866, 940, 985, 1000
};

STATIC
INT32
SinX1000 (
  IN INT32  Deg
  )
{
  INT32  Q, K, Interp, Sign = 1;

  Deg %= 360;
  if (Deg < 0) {
    Deg += 360;
  }
  Q     = Deg / 90;
  K     = Deg % 90;
  Interp = mSin1000[K / 10] +
           (mSin1000[(K / 10) + 1 > 9 ? 9 : (K / 10) + 1] - mSin1000[K / 10]) *
           (K % 10) / 10;
  if (Q == 2) {
    Sign = -1;
  }
  if (Q & 1) {
    return Sign * mSin1000[(90 - K) / 10];
  }
  return Sign * Interp;
}

VOID
GfxGear (
  IN INT32   Cx,
  IN INT32   Cy,
  IN INT32   R,
  IN UINT32  Ink,
  IN UINT32  Hole
  )
{
  INT32  Pts[32][2];
  UINTN  N = 0;
  UINTN  T;

  /* 8 teeth x 4 points: root, tip, tip, root per 45-degree sector */
  for (T = 0; T < 8 && N < 32; T++) {
    INT32  A0 = (INT32)T * 45;
    INT32  Ang[4] = { A0 - 16, A0 - 8, A0 + 8, A0 + 16 };
    INT32  Rad[4] = { R * 72 / 100, R, R, R * 72 / 100 };
    UINTN  I;

    for (I = 0; I < 4; I++) {
      Pts[N][0] = Cx + Rad[I] * SinX1000 (Ang[I] + 90) / 1000;
      Pts[N][1] = Cy - Rad[I] * SinX1000 (Ang[I]) / 1000;
      N++;
    }
  }
  GfxPolyFill (Pts, N, Ink);
  GfxCircle (Cx, Cy, R * 38 / 100, Hole, TRUE);
}

/* ------------------------------------------------------ sketchy strokes */

/*
 * Seeded 16-bit LCG; same seed -> same wobble, so the "hand" is steady
 * across redraws (a screen-wide flicker every keypress would be awful).
 */
STATIC
UINT32
Noise (
  IN OUT UINT32  *State
  )
{
  *State = (*State * 1103515245u + 12345u) & 0x7FFFFFFFu;
  return *State;
}

STATIC
VOID
SketchLine (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   Len,
  IN BOOLEAN Horizontal,
  IN UINT32  Seed,
  IN UINT32  Color
  )
{
  UINT32  State = Seed | 1;
  INT32   Step  = 9;
  INT32   I, Node, P1, O0, O1;
  INT32   Nodes[128][3];
  INT32   Count = 0;

  if (Len <= 0) {
    return;
  }
  /* Nodes[] holds 128 nodes; long strokes coarsen the step to fit */
  if (Len / Step >= 127) {
    Step = Len / 127 + 1;
  }
  O0 = (INT32)(Noise (&State) % 5) - 2;
  for (I = Step; ; I += Step) {
    P1 = I < Len ? I : Len;
    O1 = (INT32)(Noise (&State) % 5) - 2;
    /* keep the ends near the nominal line, wobble in the middle only */
    if (P1 >= Len) {
      O1 = 0;
    }
    if (Count >= 128) {
      break;
    }
    Nodes[Count][0] = P1;
    Nodes[Count][1] = O1;
    Nodes[Count][2] = O0;
    Count++;
    O0 = O1;
    if (P1 >= Len) {
      break;
    }
  }

  /* draw the 2px polyline between consecutive nodes */
  for (Node = 0; Node < Count; Node++) {
    INT32  Pa = Node == 0 ? 0 : Nodes[Node - 1][0];
    INT32  Oa = Node == 0 ? Nodes[0][2] : Nodes[Node - 1][1];
    INT32  Pb = Nodes[Node][0];
    INT32  Ob = Nodes[Node][1];
    INT32  Span = Pb - Pa;
    INT32  K;

    for (K = 0; K <= Span; K++) {
      INT32  O = Oa + (Ob - Oa) * K / (Span == 0 ? 1 : Span);

      if (Horizontal) {
        Plot (X + Pa + K, Y + O, Color);
        Plot (X + Pa + K, Y + O + 1, Color);
      } else {
        Plot (X + O, Y + Pa + K, Color);
        Plot (X + O + 1, Y + Pa + K, Color);
      }
    }
  }
}

VOID
GfxSketchH (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   Len,
  IN UINT32  Seed,
  IN UINT32  Color
  )
{
  SketchLine (X, Y, Len, TRUE, Seed, Color);
}

VOID
GfxSketchV (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   Len,
  IN UINT32  Seed,
  IN UINT32  Color
  )
{
  SketchLine (X, Y, Len, FALSE, Seed, Color);
}

VOID
GfxSketchFrame (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   W,
  IN INT32   H,
  IN UINT32  Seed,
  IN UINT32  Color
  )
{
  GfxSketchH (X, Y, W, Seed, Color);
  GfxSketchH (X, Y + H - 2, W, Seed + 7, Color);
  GfxSketchV (X, Y, H, Seed + 13, Color);
  GfxSketchV (X + W - 2, Y, H, Seed + 19, Color);
}

VOID
GfxCorners (
  IN INT32   X,
  IN INT32   Y,
  IN INT32   W,
  IN INT32   H,
  IN INT32   Arm,
  IN INT32   T,
  IN UINT32  Color
  )
{
  GfxFill (X, Y, Arm, T, Color);
  GfxFill (X, Y, T, Arm, Color);
  GfxFill (X + W - Arm, Y, Arm, T, Color);
  GfxFill (X + W - T, Y, T, Arm, Color);
  GfxFill (X, Y + H - T, Arm, T, Color);
  GfxFill (X, Y + H - Arm, T, Arm, Color);
  GfxFill (X + W - Arm, Y + H - T, Arm, T, Color);
  GfxFill (X + W - T, Y + H - Arm, T, Arm, Color);
}

/* -------------------------------------------------------------- screenshot */

#define SHOT_NAME  L"sbxshot"

/*
 * Print Screen: write the back buffer as a 24bpp BMP to the first FAT
 * volume that accepts writes (the USB stick).  Used to grab hardware
 * screenshots, where no other debug output exists.
 */
EFI_STATUS
GfxScreenshot (
  OUT CHAR16  *Result,
  IN  UINTN   ResultChars
  )
{
  EFI_HANDLE        *Handles;
  EFI_STATUS        Status;
  UINTN             Index, Count;
  STATIC UINTN      mShotSeq = 0;
  UINT32            FileSize;
  UINTN             RowBytes;
  UINT8             Header[54];
  UINT8             *Row;
  EFI_FILE_PROTOCOL *File  = NULL;
  EFI_FILE_PROTOCOL *Root  = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  CHAR16            Name[24];

  RowBytes = ((mWidth * 3 + 3) / 4) * 4;
  FileSize = 54 + RowBytes * mHeight;
  Row      = AllocatePool (RowBytes);
  if (Row == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    FreePool (Row);
    return Status;
  }

  UnicodeSPrint (Name, sizeof (Name), L"%s%03u.bmp", SHOT_NAME,
                 (UINT32)(mShotSeq++ % 1000));

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
           &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) || (Fs == NULL)) {
      continue;
    }
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }
    Status = Root->Open (Root, &File, Name,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR (Status)) {
      break;
    }
    Root->Close (Root);
    Root = NULL;
  }
  FreePool (Handles);

  if ((File == NULL) || EFI_ERROR (Status)) {
    FreePool (Row);
    return EFI_NO_MEDIA;
  }

  ZeroMem (Header, sizeof (Header));
  Header[0]  = 'B';
  Header[1]  = 'M';
  Header[2]  = (UINT8)(FileSize);
  Header[3]  = (UINT8)(FileSize >> 8);
  Header[4]  = (UINT8)(FileSize >> 16);
  Header[5]  = (UINT8)(FileSize >> 24);
  Header[10] = 54;                                   /* pixel data offset  */
  Header[14] = 40;                                   /* DIB size           */
  Header[18] = (UINT8)(mWidth);
  Header[19] = (UINT8)(mWidth >> 8);
  Header[20] = (UINT8)(mWidth >> 16);
  Header[21] = (UINT8)(mWidth >> 24);
  Header[22] = (UINT8)(mHeight);
  Header[23] = (UINT8)(mHeight >> 8);
  Header[24] = (UINT8)(mHeight >> 16);
  Header[25] = (UINT8)(mHeight >> 24);
  Header[26] = 1;                                    /* planes             */
  Header[28] = 24;                                   /* bpp                */
  {
    UINTN  HdrLen = sizeof (Header);

    File->Write (File, &HdrLen, Header);
  }

  /* BMP rows go bottom-up; back buffer is 0x00RRGGBB (already display order) */
  for (Index = 0; Index < mHeight; Index++) {
    UINT32  *Src = &mBack[(UINTN)(mHeight - 1 - Index) * mPitch];
    UINTN   X;

    for (X = 0; X < mWidth; X++) {
      Row[X * 3 + 0] = (UINT8)(Src[X]);
      Row[X * 3 + 1] = (UINT8)(Src[X] >> 8);
      Row[X * 3 + 2] = (UINT8)(Src[X] >> 16);
    }
    File->Write (File, &RowBytes, Row);
  }

  File->Flush (File);
  File->Close (File);
  Root->Close (Root);
  FreePool (Row);

  if (Result != NULL && ResultChars > 0) {
    StrCpyS (Result, ResultChars, Name);
  }
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------ soft cursor */

STATIC
VOID
DrawCursor (
  VOID
  )
{
  INT32  S = mCursorS;
  INT32  I, J;

  for (J = 0; J < CURSOR_H; J++) {
    for (I = 0; I < mCursorShape[J]; I++) {
      /* white arrow, dark outline: visible on paper, cards and popups */
      GfxFill (mCurX + I * S, mCurY + J * S, S, S,
               ((I + 1 == mCursorShape[J]) || (J == CURSOR_H - 1)) ?
               COL_INK : COL_WHITE);
    }
  }
}

STATIC
VOID
SaveCursor (
  VOID
  )
{
  INT32  S = mCursorS;
  INT32  I, J;

  mCursorX0 = mCurX;
  mCursorY0 = mCurY;
  mCursorW  = CURSOR_W * S;
  mCursorH  = CURSOR_H * S;

  for (J = 0; J < mCursorH; J++) {
    for (I = 0; I < mCursorW; I++) {
      if ((mCursorX0 + I < 0) || (mCursorY0 + J < 0) ||
          ((UINT32)(mCursorX0 + I) >= mWidth) ||
          ((UINT32)(mCursorY0 + J) >= mHeight)) {
        continue;
      }
      mCursorSave[J * mCursorW + I] =
        mBack[((UINTN)(mCursorY0 + J)) * mPitch + (UINTN)(mCursorX0 + I)];
    }
  }
}

STATIC
VOID
RestoreCursor (
  VOID
  )
{
  INT32  I, J;

  for (J = 0; J < mCursorH; J++) {
    for (I = 0; I < mCursorW; I++) {
      if ((mCursorX0 + I < 0) || (mCursorY0 + J < 0) ||
          ((UINT32)(mCursorX0 + I) >= mWidth) ||
          ((UINT32)(mCursorY0 + J) >= mHeight)) {
        continue;
      }
      mBack[((UINTN)(mCursorY0 + J)) * mPitch + (UINTN)(mCursorX0 + I)] =
        mCursorSave[J * mCursorW + I];
    }
  }
}

VOID
GfxSetCursor (
  IN INT32    X,
  IN INT32    Y,
  IN BOOLEAN  Visible
  )
{
  mCurX       = X;
  mCurY       = Y;
  mCurVisible = Visible;
}

/* -------------------------------------------------------------- init/exit */

STATIC
VOID
GfxMeasure (
  VOID
  )
{
  mWidth  = mGop->Mode->Info->HorizontalResolution;
  mHeight = mGop->Mode->Info->VerticalResolution;
  mPitch  = mWidth;
  mSwapRb = (mGop->Mode->Info->PixelFormat ==
             PixelRedGreenBlueReserved8BitPerColor);
  mUi     = mHeight >= 1500 ? 2 : 1;
}

EFI_STATUS
GfxInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index, SizeOfInfo, Best, BestArea, MaxMode;

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status) || (mGop == NULL) || (mGop->Mode == NULL)) {
    return EFI_NOT_FOUND;
  }

  /* native panel mode: the largest resolution the GOP offers */
  MaxMode  = mGop->Mode->MaxMode;
  Best     = mGop->Mode->Mode;
  BestArea = (UINTN)mGop->Mode->Info->HorizontalResolution *
             mGop->Mode->Info->VerticalResolution;

  for (Index = 0; Index < MaxMode; Index++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info = NULL;

    SizeOfInfo = 0;
    Status     = mGop->QueryMode (mGop, Index, &SizeOfInfo, &Info);
    if (EFI_ERROR (Status) || (Info == NULL)) {
      continue;
    }
    if ((UINTN)Info->HorizontalResolution * Info->VerticalResolution > BestArea) {
      BestArea = (UINTN)Info->HorizontalResolution * Info->VerticalResolution;
      Best     = Index;
    }
    FreePool (Info);
  }

  if (Best != mGop->Mode->Mode) {
    Status = mGop->SetMode (mGop, Best);
    if (EFI_ERROR (Status)) {
      Print (L"[gfx] SetMode(%d) failed: %r\n", Best, Status);
    }
  }

  GfxMeasure ();

  mBack = AllocateZeroPool ((UINTN)mWidth * mHeight * sizeof (UINT32));
  if (mBack == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (mSwapRb) {
    mStage = AllocatePool ((UINTN)mWidth * mHeight * sizeof (UINT32));
    if (mStage == NULL) {
      FreePool (mBack);
      mBack = NULL;
      return EFI_OUT_OF_RESOURCES;
    }
  }

  mCurX = (INT32)(mWidth / 2);
  mCurY = (INT32)(mHeight / 2);

  SbLog ("[gfx] mode list", MaxMode, Best);
  SbLog ("[gfx] at start", mWidth, mHeight);
  GfxClear (COL_BG);
  return EFI_SUCCESS;
}

EFI_STATUS
GfxStepMode (
  IN INTN  Direction
  )
{
  EFI_STATUS  Status;
  INTN        Index, MaxMode, Guard;
  UINTN       SizeOfInfo;
  UINT32      *NewBack, *NewStage;

  if (mGop == NULL) {
    return EFI_NOT_READY;
  }

  MaxMode = (INTN)mGop->Mode->MaxMode;
  Index   = (INTN)mGop->Mode->Mode + Direction;

  for (Guard = 0; Guard < MaxMode; Guard++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info = NULL;

    Index += (Index >= MaxMode) ? -MaxMode : 0;
    Index += (Index < 0) ? MaxMode : 0;
    SizeOfInfo = 0;
    Status     = mGop->QueryMode (mGop, (UINT32)Index, &SizeOfInfo, &Info);
    if (Info != NULL) {
      FreePool (Info);
      break;
    }
    Index += Direction;
  }
  if (Guard >= MaxMode) {
    return EFI_UNSUPPORTED;
  }

  Status = mGop->SetMode (mGop, (UINT32)Index);
  if (EFI_ERROR (Status)) {
    SbLog ("[gfx] SetMode fail", (UINTN)Index, (UINTN)Status);
    return Status;
  }

  GfxMeasure ();

  NewBack = AllocateZeroPool ((UINTN)mWidth * mHeight * sizeof (UINT32));
  if (NewBack == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  NewStage = NULL;
  if (mSwapRb) {
    NewStage = AllocatePool ((UINTN)mWidth * mHeight * sizeof (UINT32));
    if (NewStage == NULL) {
      FreePool (NewBack);
      return EFI_OUT_OF_RESOURCES;
    }
  }

  if (mBack != NULL) {
    FreePool (mBack);
  }
  if (mStage != NULL) {
    FreePool (mStage);
  }
  mBack  = NewBack;
  mStage = NewStage;

  mCurX = (INT32)(mWidth / 2);
  mCurY = (INT32)(mHeight / 2);
  GfxClear (COL_BG);
  SbLog ("[gfx] mode", mWidth, mHeight);
  return EFI_SUCCESS;
}

VOID
GfxExit (
  VOID
  )
{
  if (mBack != NULL) {
    FreePool (mBack);
    mBack = NULL;
  }
  if (mStage != NULL) {
    FreePool (mStage);
    mStage = NULL;
  }
}

UINT32
GfxWidth (
  VOID
  )
{
  return mWidth;
}

UINT32
GfxHeight (
  VOID
  )
{
  return mHeight;
}

INT32
GfxUi (
  VOID
  )
{
  return mUi;
}

UINT32 *
GfxBackBuffer (
  VOID
  )
{
  return mBack;
}

EFI_STATUS
GfxPresent (
  VOID
  )
{
  EFI_STATUS  Status;
  BOOLEAN     CurOn = FALSE;

  if ((mGop == NULL) || (mBack == NULL)) {
    return EFI_NOT_READY;
  }

  if (mCurVisible) {
    mCursorS = mUi;
    if ((CURSOR_W * mCursorS) * (CURSOR_H * mCursorS) <=
        (INT32)(sizeof (mCursorSave) / sizeof (mCursorSave[0]))) {
      SaveCursor ();
      DrawCursor ();
      CurOn = TRUE;
    }
  }

  if (mSwapRb && (mStage != NULL)) {
    UINTN  Index;
    UINTN  Count = (UINTN)mWidth * mHeight;

    for (Index = 0; Index < Count; Index++) {
      mStage[Index] = mBack[Index];
    }
    Status = mGop->Blt (
                    mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mStage,
                    EfiBltBufferToVideo, 0, 0, 0, 0, mWidth, mHeight,
                    (UINTN)mWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  } else {
    Status = mGop->Blt (
                    mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mBack,
                    EfiBltBufferToVideo, 0, 0, 0, 0, mWidth, mHeight, 0);
  }

  if (CurOn) {
    RestoreCursor ();
  }
  return Status;
}
