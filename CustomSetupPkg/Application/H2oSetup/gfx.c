/*
 * gfx.c - GOP framebuffer backend, scaled HII font rendering, software cursor.
 *
 * All drawing goes into a 0x00RRGGBB back buffer that is transferred with a
 * single GOP Blt call.  Glyphs come from the stock EFI_HII_FONT_PROTOCOL
 * system font and are nearest-neighbour scaled, which gives the UI readable
 * text on a 4K panel without shipping a private font package.
 */

#include "H2oSetup.h"

#define MAX_GLYPH_CACHE 256
#define CURSOR_H        16

typedef struct {
  BOOLEAN                          Loaded;
  BOOLEAN                          Found;
  UINT32                           Width;
  UINT32                           Height;
  UINT32                           Advance;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Pixels;
} GLYPH;

/* Row width of the pointer shape in glyph columns. */
STATIC CONST UINT8  mCursorShape[CURSOR_H] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 11, 11, 8, 6, 4
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop = NULL;
STATIC EFI_HII_FONT_PROTOCOL         *mFont = NULL;
STATIC UINT32                        *mBack = NULL;
STATIC UINT32                        *mStage = NULL;      /* used on R/B swap */
STATIC UINTN                          mPitch = 0;         /* pixels per line  */
STATIC UINT32                         mWidth = 0;
STATIC UINT32                         mHeight = 0;
STATIC UINT32                         mScale = 1;
STATIC UINT32                         mCellW = 8, mCellH = 16;
STATIC BOOLEAN                        mSwapRb = FALSE;
STATIC GLYPH                          mGlyph[MAX_GLYPH_CACHE];
STATIC INT32                          mCurX = 0, mCurY = 0;
STATIC BOOLEAN                        mCurVisible = FALSE;
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

STATIC
VOID
Blend (
  IN INT32   X,
  IN INT32   Y,
  IN UINT32  Color,
  IN UINT32  Alpha
  )
{
  UINT32  *Dst;
  UINT32  D;
  UINT32  R, G, B;

  if ((X < 0) || (Y < 0) || ((UINT32)X >= mWidth) || ((UINT32)Y >= mHeight)) {
    return;
  }

  Dst = &mBack[(UINTN)Y * mPitch + (UINTN)X];
  D   = mSwapRb ? SwapRb (*Dst) : *Dst;

  R = ((((Color >> 16) & 0xFF) * Alpha + ((D >> 16) & 0xFF) * (255 - Alpha)) / 255) << 16;
  G = ((((Color >> 8) & 0xFF) * Alpha + ((D >> 8) & 0xFF) * (255 - Alpha)) / 255) << 8;
  B = (((Color & 0xFF) * Alpha + (D & 0xFF) * (255 - Alpha)) / 255);

  *Dst = mSwapRb ? SwapRb (R | G | B) : (R | G | B);
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
  IN UINT32  Color
  )
{
  INT32  I, T = (INT32)mScale;

  for (I = 0; I < T; I++) {
    GfxFill (X, Y + I, W, 1, Color);
    GfxFill (X, Y + H - 1 - I, W, 1, Color);
    GfxFill (X + I, Y, 1, H, Color);
    GfxFill (X + W - 1 - I, Y, 1, H, Color);
  }
}

/* -------------------------------------------------------------- HII glyphs */

STATIC
GLYPH *
GetGlyph (
  IN CHAR16  Ch
  )
{
  GLYPH             *G;
  EFI_IMAGE_OUTPUT  *Blt;
  EFI_STATUS        Status;
  UINTN             Baseline;

  if ((Ch == 0) || (Ch >= MAX_GLYPH_CACHE)) {
    return NULL;
  }

  G = &mGlyph[Ch];
  if (G->Loaded) {
    return G->Found ? G : NULL;
  }

  G->Loaded = TRUE;
  if (mFont == NULL) {
    return NULL;
  }

  Blt      = NULL;
  Baseline = 0;
  Status   = mFont->GetGlyph (mFont, Ch, NULL, &Blt, &Baseline);
  if (EFI_ERROR (Status) || (Blt == NULL) || (Blt->Image == NULL)) {
    if (Blt != NULL) {
      FreePool (Blt);
    }
    return NULL;
  }

  /*
   * The bitmap is owned by the font protocol allocation.  Keeping the wrapper
   * alive caches the glyph for the life of the application; the few hundred
   * bytes per character are reclaimed at exit.
   */
  G->Found   = TRUE;
  G->Width   = Blt->Image->Width;
  G->Height  = Blt->Image->Height;
  G->Advance = Blt->Image->Width != 0 ? Blt->Image->Width : 6;
  G->Pixels  = Blt->Image->Pixels;

  return G;
}

STATIC
VOID
BlitGlyph (
  IN GLYPH   *G,
  IN INT32   X,
  IN INT32   Y,
  IN UINT32  Color
  )
{
  UINT32  I, J, K, L;

  for (J = 0; J < G->Height; J++) {
    for (I = 0; I < G->Width; I++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *P = &G->Pixels[J * G->Width + I];
      UINT32                         A = P->Reserved;

      if (A == 0) {
        /* Some font packages keep the ink in RGB and leave alpha unused. */
        A = ((UINT32)P->Red + P->Green + P->Blue) / 3;
      }
      if (A == 0) {
        continue;
      }

      for (L = 0; L < mScale; L++) {
        for (K = 0; K < mScale; K++) {
          Blend (X + (INT32)(I * mScale) + (INT32)K,
                 Y + (INT32)(J * mScale) + (INT32)L, Color, A);
        }
      }
    }
  }
}

INT32
GfxTextWidth (
  IN CONST CHAR16  *S
  )
{
  INT32  Width = 0;

  while (S != NULL && *S != 0) {
    GLYPH  *G = GetGlyph (*S);
    Width += (INT32)(G->Advance * mScale) + (INT32)mScale;
    S++;
  }

  return Width;
}

/*
 * Draws S at (X, Y).  When MaxW is non-zero the string is clipped and marked
 * with ".." so a long IFR prompt never runs into the value column.
 */
INT32
GfxText (
  IN INT32         X,
  IN INT32         Y,
  IN CONST CHAR16  *S,
  IN UINT32        Color,
  IN INT32         MaxW
  )
{
  INT32  Start = X;
  INT32  Limit = MaxW > 0 ? X + MaxW : 0x7FFFFFFF;

  while (S != NULL && *S != 0) {
    GLYPH  *G    = GetGlyph (*S);
    INT32  Adv   = (INT32)((G != NULL ? G->Advance : 6) * mScale) + (INT32)mScale;

    if (X + Adv > Limit) {
      GfxText (Limit - 3 * (INT32)mScale, Y, L"..", Color, 0);
      return Limit - Start;
    }
    if (G != NULL) {
      BlitGlyph (G, X, Y, Color);
    }
    X += Adv;
    S++;
  }

  return X - Start;
}

/* ------------------------------------------------------------ soft cursor */

#define CURSOR_W  11

STATIC INT32  mCursorX0, mCursorY0, mCursorW, mCursorH;
STATIC INT32  mCursorS = 1;

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
      /* White arrow with a dark outline so it stays visible on any panel. */
      GfxFill (mCurX + I * S, mCurY + J * S, S, S,
               ((I + 1 == mCursorShape[J]) || (J == CURSOR_H - 1)) ?
               0x00000000u : COL_TEXT_HI);
    }
  }
}

STATIC
VOID
SaveCursor (
  VOID
  )
{
  INT32  S  = mCursorS;
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
  INT32  S = mCursorS;
  INT32  I, J;

  for (J = 0; J < CURSOR_H * S; J++) {
    for (I = 0; I < CURSOR_W * S; I++) {
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

  gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mFont);
  if (mFont == NULL) {
    Print (L"[gfx] EFI_HII_FONT_PROTOCOL not found - text will be boxes\n");
  }

  /* Native panel mode: pick the largest resolution the GOP offers. */
  MaxMode  = mGop->Mode->MaxMode;
  Best     = mGop->Mode->Mode;
  BestArea = (UINTN)mGop->Mode->Info->HorizontalResolution *
             mGop->Mode->Info->VerticalResolution;

  for (Index = 0; Index < MaxMode; Index++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFO  *Info = NULL;

    SizeOfInfo = 0;
    Status     = mGop->QueryMode (mGop, Index, &SizeOfInfo, &Info);
    if (EFI_ERROR (Status) || (Info == NULL)) {
      continue;
    }
    if ((UINTN)Info->HorizontalResolution * Info->VerticalResolution > BestArea) {
      BestArea = (UINTN)Info->HorizontalResolution * Info->VerticalResolution;
      Best     = Index;
    }
  }

  if (Best != mGop->Mode->Mode) {
    Status = mGop->SetMode (mGop, Best);
    if (EFI_ERROR (Status)) {
      Print (L"[gfx] SetMode(%d) failed: %r\n", Best, Status);
    }
  }

  mWidth  = mGop->Mode->Info->HorizontalResolution;
  mHeight = mGop->Mode->Info->VerticalResolution;
  mPitch  = mWidth;               /* our back buffer is packed, one pixel/column */
  mSwapRb = (mGop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor);

  mScale = 1;
  if (mHeight >= 2000) {
    mScale = 3;
  } else if (mHeight >= 900) {
    mScale = 2;
  }

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

  mCellH = 19 * mScale + 2;
  mCellW = 7 * mScale;

  mCurX = (INT32)(mWidth / 2);
  mCurY = (INT32)(mHeight / 2);

  GfxClear (COL_BG);
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

UINT32
GfxScale (
  VOID
  )
{
  return mScale;
}

UINT32
GfxCellW (
  VOID
  )
{
  return mCellW;
}

UINT32
GfxCellH (
  VOID
  )
{
  return mCellH;
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
    mCursorS = mScale > 4 ? 4 : (INT32)mScale;
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
    /* Put the saved rectangle back so the next frame starts clean. */
    RestoreCursor ();
  }

  return Status;
}
