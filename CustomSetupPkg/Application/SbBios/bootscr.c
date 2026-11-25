/*
 * bootscr.c - the Saku Overclock boot menu screen.
 *
 * Enumerates real Boot#### entries, keeps a "last / second-to-last booted"
 * history in NV variables, boots via BootNext + cold reset, launches the
 * EFI shell file for diagnostics, and hands over to the setup screen on
 * F10 (in-process: no LoadImage round-trip to the USB stick).
 *
 * Boot#### payload is  { UINT32 Attributes; UINT32 Timeout;
 *                        EFI_DEVICE_PATH_PROTOCOL; CHAR16 Description[] }.
 * The description sits AFTER the whole device path, so we walk the path
 * node by node (3-byte node lengths) before reading it; some vendor
 * blobs carry four extra bytes up front, and both layouts are tried.
 */

#include "SbBios.h"
#include <Guid/FileInfo.h>

STATIC EFI_GUID  mSbBootGuid = {
  0x4a7c2e91, 0x6f05, 0x4d18,
  { 0xb3, 0xea, 0x8c, 0x92, 0xf0, 0x4d, 0x71, 0x5c }
};

#define ATTR_NV  (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE)

typedef struct {
  UINT16  Num;
  CHAR16  Label[64];
} ENTRY;

typedef struct {
  UINT16  Num;
  CHAR16  Label[64];
} SB_LASTBOOT;

#define MAX_ENTRIES  16
#define ITEMS        3

STATIC ENTRY    mEntries[MAX_ENTRIES];
STATIC UINTN    mEntryCount;
STATIC CHAR16   mLastLabel[64] = L"";
STATIC UINT16   mLastNum = 0xFFFF;
STATIC CHAR16   mPrevLabel[64] = L"";
STATIC UINT16   mPrevNum = 0xFFFF;

STATIC UINTN    mSel = 0;
STATIC BOOLEAN  mWantSetup = FALSE;
STATIC CHAR16   mStatusMsg[96];

/* card geometry for mouse hit-testing (valid after BootDraw) */
STATIC INT32    mCardX, mCardY0, mCardW, mCardH, mCardGap;

#define TITLE_TXT  L"superbox"
#define PRESS_TXT  L"PRESS \x21B5 TO CONTINUE"          /* ↩ */
#define HINTS_TXT  L"\x21C5 NAVIGATE   E EDIT   C CONSOLE   F10 SETUP"
#define SIGN_TXT   L"2026 @Serzhik Saku"
#define POWER_TXT  L"powered by Saku Overclock"

/* Russian prefixes, escapes keep the source pure ASCII */
#define LBL_LAST  L"\x041F\x043E\x0441\x043B\x0435\x0434\x043D\x0438\x0439: "   /* Последний: */
#define LBL_PREV  L"\x041F\x0440\x0435\x0434\x043F\x043E\x0441\x043B\x0435\x0434\x043D\x0438\x0439: "  /* Предпоследний: */
#define LBL_DIAG  L"\x0414\x0438\x0430\x0433\x043D\x043E\x0441\x0442\x0438\x043A\x0430: "  /* Диагностика: */

/* ------------------------------------------------------------- boot vars */

STATIC
EFI_STATUS
ReadGlobalVar (
  IN  CONST CHAR16  *Name,
  OUT UINT8         **Data,
  OUT UINTN         *Size
  )
{
  EFI_STATUS  Status;
  UINTN       Sz = 0;

  Status = gRT->GetVariable ((CHAR16 *)Name, &gEfiGlobalVariableGuid, NULL,
                             &Sz, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_NOT_FOUND;
  }
  *Data = AllocatePool (Sz);
  if (*Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = gRT->GetVariable ((CHAR16 *)Name, &gEfiGlobalVariableGuid, NULL,
                             &Sz, *Data);
  if (EFI_ERROR (Status)) {
    FreePool (*Data);
    return Status;
  }
  *Size = Sz;
  return EFI_SUCCESS;
}

/* Is the range a sane, printable UCS-2 description? */
STATIC
BOOLEAN
SaneDescription (
  IN UINT8   *Desc,
  IN UINTN   Bytes
  )
{
  UINTN  I;

  if ((Bytes < 2) || (Bytes % 2) != 0) {
    return FALSE;
  }
  for (I = 0; I + 1 < Bytes; I += 2) {
    UINT16  Ch = (UINT16)(Desc[I] | (Desc[I + 1] << 8));

    if (Ch == 0) {
      return I > 0;
    }
    if (Ch < 0x20 || Ch > 0x24FF) {
      return FALSE;
    }
  }
  return TRUE;
}

/*
 * Extract the human label of a Boot#### payload.  header is the fixed
 * attribute/timeout prefix length: the UEFI spec layout (8) and the
 * legacy with the extra EFI_STATUS field (12) are both attempted.
 */
STATIC
BOOLEAN
BootLabelAt (
  IN  UINT8   *Data,
  IN  UINTN   Size,
  IN  UINTN   Header,
  OUT CHAR16  *Label,
  IN  UINTN   LabelChars
  )
{
  UINT8   *Ptr = Data + Header;
  UINT8   *End = Data + Size;
  UINTN    DpLen;
  UINTN    Bytes;

  Label[0] = 0;
  if (Header >= Size) {
    return FALSE;
  }

  while (Ptr + 4 <= End) {
    if ((Ptr[0] == END_DEVICE_PATH_TYPE) &&
        (Ptr[1] == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
      break;
    }
    DpLen = (UINTN)Ptr[2] | ((UINTN)Ptr[3] << 8) | ((UINTN)Ptr[4] << 16);
    if (DpLen < 4) {
      return FALSE;
    }
    Ptr += DpLen;
  }
  if (Ptr + 4 > End) {
    return FALSE;
  }

  Bytes = (UINTN)(End - Ptr);
  if (!SaneDescription (Ptr, Bytes)) {
    return FALSE;
  }
  Bytes /= sizeof (CHAR16);
  if (Bytes >= LabelChars) {
    Bytes = LabelChars - 1;
  }
  CopyMem (Label, Ptr, Bytes * sizeof (CHAR16));
  Label[Bytes] = 0;
  return TRUE;
}

STATIC
VOID
BootLabel (
  IN  UINT8   *Data,
  IN  UINTN   Size,
  OUT CHAR16  *Label,
  IN  UINTN   LabelChars
  )
{
  if (BootLabelAt (Data, Size, 8, Label, LabelChars)) {
    return;
  }
  if (BootLabelAt (Data, Size, 12, Label, LabelChars)) {
    return;
  }
  Label[0] = 0;
}

STATIC
VOID
LoadLastVar (
  IN  CONST CHAR16  *Name,
  OUT CHAR16        *Label,
  OUT UINT16        *Num
  )
{
  SB_LASTBOOT  Info;
  UINTN        Size = sizeof (Info);

  if (!EFI_ERROR (gRT->GetVariable ((CHAR16 *)Name, &mSbBootGuid, NULL,
                                    &Size, &Info)) &&
      (Size == sizeof (Info)) && (Info.Label[0] != 0)) {
    StrCpyS (Label, 64, Info.Label);
    *Num = Info.Num;
  }
}

STATIC
VOID
SaveLastVar (
  IN CONST CHAR16  *Name,
  IN CONST CHAR16  *Label,
  IN UINT16        Num
  )
{
  SB_LASTBOOT  Info;

  ZeroMem (&Info, sizeof (Info));
  Info.Num = Num;
  StrCpyS (Info.Label, 64, Label);
  gRT->SetVariable ((CHAR16 *)Name, &mSbBootGuid, ATTR_NV, sizeof (Info),
                    &Info);
}

STATIC
VOID
CollectEntries (
  VOID
  )
{
  UINTN   I;
  UINT16  Num;
  CHAR16  Name[10];
  UINT8   *Data;
  UINTN   Size;

  mEntryCount = 0;
  for (Num = 0; Num < 0x100; Num++) {
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Num);
    if (!EFI_ERROR (ReadGlobalVar (Name, &Data, &Size))) {
      if (mEntryCount < MAX_ENTRIES) {
        mEntries[mEntryCount].Num = Num;
        BootLabel (Data, Size, mEntries[mEntryCount].Label, 64);
        SbLog ("[boot] lbl", Num, mEntries[mEntryCount].Label[0]);
        mEntryCount++;
      }
      FreePool (Data);
    }
  }
  SbLog ("[boot] entries", mEntryCount, 0);

  /* "last booted": the firmware's BootCurrent when present */
  if (!EFI_ERROR (ReadGlobalVar (L"BootCurrent", &Data, &Size)) &&
      (Size >= sizeof (UINT16))) {
    Num = *(UINT16 *)Data;
    for (I = 0; I < mEntryCount; I++) {
      if (mEntries[I].Num == Num) {
        StrCpyS (mLastLabel, 64, mEntries[I].Label);
        mLastNum = Num;
        break;
      }
    }
    FreePool (Data);
  }

  /* persisted history wins over the simple BootCurrent heuristic */
  LoadLastVar (L"SbBootLast", mLastLabel, &mLastNum);
  LoadLastVar (L"SbBootPrev", mPrevLabel, &mPrevNum);

  if ((mLastLabel[0] == 0) && (mEntryCount > 0)) {
    StrCpyS (mLastLabel, 64, mEntries[0].Label);
    mLastNum = mEntries[0].Num;
  }
  if ((mPrevLabel[0] == 0) && (mEntryCount > 1)) {
    StrCpyS (mPrevLabel, 64, mEntries[1].Label);
    mPrevNum = mEntries[1].Num;
  }
  SbLog ("[boot] last", mLastNum, mPrevNum);
}

STATIC
VOID
BootTo (
  IN UINT16        Num,
  IN CONST CHAR16  *Label
  )
{
  CHAR16  PrevLabel[64];
  UINT16  PrevNum;
  UINT16  Next = Num;

  StrCpyS (PrevLabel, 64, mLastLabel);
  PrevNum = mLastNum;

  if (PrevLabel[0] != 0) {
    SaveLastVar (L"SbBootPrev", PrevLabel, PrevNum);
  }
  SaveLastVar (L"SbBootLast", Label, Num);

  gRT->SetVariable (L"BootNext", &gEfiGlobalVariableGuid, ATTR_NV,
                    sizeof (Next), &Next);

  StrCpyS (mStatusMsg, 96, L"rebooting...");
  SbLog ("[boot] bootnext", Num, 0);
  BootDraw ();
  gBS->Stall (200000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

/*
 * Load and start an EFI file from any mounted FAT volume (the EFI shell
 * for the diagnostics entry).  LoadImage needs a real device path, and
 * file size comes from GetInfo because FAT drivers answer Read(NULL) with
 * SUCCESS and zero length.
 */
EFI_STATUS
RunFile (
  IN CONST CHAR16  *Name1,
  IN CONST CHAR16  *Name2
  )
{
  EFI_HANDLE    *Handles;
  EFI_STATUS    Status;
  UINTN         Index, Count, N;
  BOOLEAN       Found = FALSE;
  CONST CHAR16  *Names[2];

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Names[0] = Name1;
  Names[1] = Name2;

  for (Index = 0; (Index < Count) && !Found; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
    EFI_FILE_PROTOCOL                *Root, *File;
    EFI_DEVICE_PATH_PROTOCOL         *FilePath;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
           &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) || (Fs == NULL)) {
      continue;
    }
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    for (N = 0; N < 2; N++) {
      EFI_STATUS  S1;
      UINT8       *Buf;
      UINTN       Size = 0;
      UINT8       InfoBuf[sizeof (EFI_FILE_INFO) + 256];
      UINTN       InfoSize = sizeof (InfoBuf);

      File = NULL;
      S1 = Root->Open (Root, &File, (CHAR16 *)Names[N], EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR (S1)) {
        continue;
      }
      S1 = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, InfoBuf);
      if (!EFI_ERROR (S1)) {
        Size = ((EFI_FILE_INFO *)InfoBuf)->FileSize;
      }
      if (Size != 0) {
        Buf = AllocatePool (Size);
        if (Buf != NULL) {
          File->SetPosition (File, 0);
          S1 = File->Read (File, &Size, Buf);
          if (!EFI_ERROR (S1)) {
            EFI_HANDLE  Img = NULL;

            FilePath = FileDevicePath (Handles[Index], (CHAR16 *)Names[N]);
            if (FilePath != NULL) {
              S1 = gBS->LoadImage (FALSE, gImageHandle, FilePath,
                                   Buf, Size, &Img);
              if (!EFI_ERROR (S1)) {
                gBS->StartImage (Img, NULL, NULL);
                gBS->UnloadImage (Img);
              }
              FreePool (FilePath);
            }
          }
          FreePool (Buf);
        }
      }
      File->Close (File);
      Found = TRUE;
      break;
    }
    Root->Close (Root);
  }

  FreePool (Handles);
  return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
VOID
RunShell (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = RunFile (L"\\EFI\\BOOT\\SHELLX64.EFI", L"\\SHELLX64.EFI");
  if (Status == EFI_NOT_FOUND) {
    StrCpyS (mStatusMsg, 96, L"SHELLX64.EFI not found on any volume");
    BootDraw ();
  }
}

/* ------------------------------------------------------------- activation */

STATIC
VOID
Activate (
  VOID
  )
{
  mStatusMsg[0] = 0;
  if (mSel == 2) {
    RunShell ();
  } else if ((mSel == 0) && (mLastNum != 0xFFFF)) {
    BootTo (mLastNum, mLastLabel);
  } else if ((mSel == 1) && (mPrevNum != 0xFFFF)) {
    BootTo (mPrevNum, mPrevLabel);
  } else {
    StrCpyS (mStatusMsg, 96, L"nothing selected to boot");
    BootDraw ();
  }
}

/* ------------------------------------------------------------------ draw */

VOID
BootDraw (
  VOID
  )
{
  INTN          W = (INTN)GfxWidth ();
  INTN          H = (INTN)GfxHeight ();
  INTN          Mar = W / 16;
  INTN          Xc  = Mar;
  INTN          Wc  = W * 46 / 100;
  INTN          Ch  = H * 7 / 100;
  INTN          Gap = Ch / 3;
  INTN          Y0  = H * 22 / 100;
  INTN          I, Y;
  CONST CHAR16  *Pre[ITEMS];
  CONST CHAR16  *Val[ITEMS];
  CHAR16        Line[160];

  GfxClear (COL_BG);

  /* headline */
  GfxText (Mar, H * 13 / 100, FONT (SbSyne64), TITLE_TXT, COL_INK, 1);

  /* card block corner brackets from the mockup */
  {
    INT32  Bx = Xc - 10, By = Y0 - 10;
    INT32  Bw = Wc + 20, Bh = ITEMS * Ch + (ITEMS - 1) * Gap + 20;

    GfxCorners (Bx, By, Bw, Bh, 14, 3, COL_INK);
  }

  Pre[0] = LBL_LAST;
  Pre[1] = LBL_PREV;
  Pre[2] = LBL_DIAG;
  Val[0] = mLastLabel[0] != 0 ? mLastLabel : L"(none)";
  Val[1] = mPrevLabel[0] != 0 ? mPrevLabel : L"(none)";
  Val[2] = L"EfiShell";

  mCardX = Xc;
  mCardY0 = Y0;
  mCardW = Wc;
  mCardH = Ch;
  mCardGap = Gap;

  for (I = 0; I < ITEMS; I++) {
    Y = Y0 + I * (Ch + Gap);
    GfxFill (Xc, Y, Wc, Ch, (UINTN)I == mSel ? COL_ORANGE : COL_CARD);
    UnicodeSPrint (Line, sizeof (Line), L"%s%s", Pre[I], Val[I]);
    GfxText (Xc + 14, Y + Ch / 2 + 9, FONT (SbMono24), Line, COL_WHITE, 1);
  }

  /* press to continue */
  GfxText (Mar, H * 62 / 100, FONT (SbSyne34), PRESS_TXT, COL_INK, 1);

  /* footer */
  GfxText (Mar, H * 78 / 100, FONT (SbMono19), HINTS_TXT, COL_INK, 1);
  GfxText (Mar, H * 86 / 100, FONT (SbMono19), SIGN_TXT, COL_INK, 1);

  /* brand mark: the lightning polygon + power line */
  {
    INTN  BoltX = W * 64 / 100;
    INTN  BoltW = H * 36 / 100;
    INTN  BoltH = H * 48 / 100;
    INTN  Tw;
    INT32 Pts[6][2];
    INTN  Kx[6] = { 600, 100, 400, 250, 900, 550 };
    INTN  Ky[6] = { 0, 500, 500, 1000, 400, 400 };

    for (I = 0; I < 6; I++) {
      Pts[I][0] = BoltX + Kx[I] * BoltW / 1000;
      Pts[I][1] = H * 8 / 100 + Ky[I] * BoltH / 1000;
    }
    GfxPolyFill (Pts, 6, COL_ORANGE);

    Tw = GfxTextW (FONTW (SbSyne26), POWER_TXT, 1);
    GfxText (BoltX + BoltW / 2 - Tw / 2, H * 64 / 100, FONT (SbSyne26),
             POWER_TXT, COL_INK, 1);
  }

  if (mStatusMsg[0] != 0) {
    GfxText (Mar, H * 93 / 100, FONT (SbMono19), mStatusMsg, COL_CARD, 1);
  }
}

/* ---------------------------------------------------------------- events */

STATIC
INTN
CardAt (
  IN INT32  X,
  IN INT32  Y
  )
{
  INTN  I;

  for (I = 0; I < ITEMS; I++) {
    INT32  Cy = mCardY0 + (INT32)I * (mCardH + mCardGap);

    if ((X >= mCardX) && (X < mCardX + mCardW) && (Y >= Cy) &&
        (Y < Cy + mCardH)) {
      return I;
    }
  }
  return -1;
}

VOID
BootEnter (
  VOID
  )
{
  mWantSetup = FALSE;
  mStatusMsg[0] = 0;
  CollectEntries ();
  BootDraw ();
}

VOID
BootEvent (
  IN INPUT_EVENT  *E
  )
{
  mStatusMsg[0] = 0;

  if (E->Type == EV_LCLICK) {
    INTN  Card = CardAt (E->X, E->Y);

    if (Card >= 0) {
      if ((UINTN)Card == mSel) {
        Activate ();                       /* second click = Enter */
      } else {
        mSel = (UINTN)Card;
        BootDraw ();
      }
    }
    return;
  }

  if (E->Type != EV_KEY) {
    return;
  }

  switch (E->Key.ScanCode) {
    case SCAN_UP:
      mSel = (mSel + ITEMS - 1) % ITEMS;
      BootDraw ();
      return;
    case SCAN_DOWN:
      mSel = (mSel + 1) % ITEMS;
      BootDraw ();
      return;
    case SCAN_F10:
      mWantSetup = TRUE;
      return;
    default:
      break;
  }

  if (E->Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
    Activate ();
    return;
  }
  if ((E->Key.UnicodeChar == L'c') || (E->Key.UnicodeChar == L'C')) {
    RunShell ();
    return;
  }
  if ((E->Key.UnicodeChar == L'e') || (E->Key.UnicodeChar == L'E')) {
    StrCpyS (mStatusMsg, 96, L"EDIT: not implemented yet");
    BootDraw ();
  }
}

BOOLEAN
BootWantSetup (
  VOID
  )
{
  return mWantSetup;
}
