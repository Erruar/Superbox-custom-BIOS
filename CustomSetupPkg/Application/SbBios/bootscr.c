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
  UINTN  Pos;
  UINTN  Idx;

  Label[0] = 0;
  /* Standard UEFI EFI_LOAD_OPTION: Attributes(4) + FilePathListLength(2) + CHAR16 Description[] */
  if (Size > 8) {
    Pos = 6;
    Idx = 0;
    while ((Pos + 1 < Size) && (Idx + 1 < LabelChars)) {
      CHAR16  Ch = (CHAR16)(Data[Pos] | (Data[Pos + 1] << 8));
      if (Ch == 0) {
        if (Idx > 0) {
          Label[Idx] = 0;
          return;
        }
        break;
      }
      if ((Ch < 0x20) || (Ch > 0x24FF)) {
        Idx = 0;
        break;
      }
      Label[Idx++] = Ch;
      Pos += 2;
    }
    Label[0] = 0;
  }

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
  if (!EFI_ERROR (ReadGlobalVar (L"BootOrder", &Data, &Size)) && (Size >= sizeof (UINT16))) {
    UINTN   OrderCount = Size / sizeof (UINT16);
    UINT16  *Order     = (UINT16 *)Data;

    for (I = 0; I < OrderCount && mEntryCount < MAX_ENTRIES; I++) {
      UINT8  *OptData = NULL;
      UINTN   OptSize = 0;

      Num = Order[I];
      UnicodeSPrint (Name, sizeof (Name), L"Boot%04X", Num);
      if (!EFI_ERROR (ReadGlobalVar (Name, &OptData, &OptSize))) {
        mEntries[mEntryCount].Num = Num;
        BootLabel (OptData, OptSize, mEntries[mEntryCount].Label, 64);
        SbLog ("[boot] lbl", Num, mEntries[mEntryCount].Label[0]);
        mEntryCount++;
        FreePool (OptData);
      }
    }
    FreePool (Data);
  }

  for (Num = 0; Num < 0x100 && mEntryCount < MAX_ENTRIES; Num++) {
    BOOLEAN  Seen = FALSE;

    for (I = 0; I < mEntryCount; I++) {
      if (mEntries[I].Num == Num) {
        Seen = TRUE;
        break;
      }
    }
    if (Seen) {
      continue;
    }
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04X", Num);
    if (!EFI_ERROR (ReadGlobalVar (Name, &Data, &Size))) {
      mEntries[mEntryCount].Num = Num;
      BootLabel (Data, Size, mEntries[mEntryCount].Label, 64);
      SbLog ("[boot] lbl", Num, mEntries[mEntryCount].Label[0]);
      mEntryCount++;
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


/*
 * Integer square root (Newton).  Inputs here are squared deltas of
 * screen coordinates, so a handful of iterations converges.
 */
STATIC
INTN
IntSqrt (
  IN INTN  V
  )
{
  INTN  G;
  INTN  T;

  if (V <= 0) {
    return 0;
  }
  G = V;
  while (TRUE) {
    T = (G + V / G) / 2;
    if (T >= G) {
      return G;
    }
    G = T;
  }
}

STATIC
INT32
DrawKeycap (
  IN INT32         X,
  IN INT32         BaseY,
  IN CONST CHAR16  *Key,
  IN BOOLEAN       Hi,
  IN BOOLEAN       UseSyneIcon
  )
{
  /*
   * Boot-page keycaps: the box rides on the caption baseline, sized by
   * the caption font; icons print in Syne (the arrows live there),
   * letters in the wide mono, with Syne as the compact fallback.
   */
  if (Hi) {
    if (UseSyneIcon) {
      return GfxKeycap (X, BaseY, Key,
                        FONTW (SbSyne34),
                        FONT (SbSyne34), FONT (SbSyne34),
                        14, 8, 8, 5, COL_CARD, COL_WHITE);
    }
    return GfxKeycap (X, BaseY, Key,
                      FONTW (SbSyne34),
                      FONT (SbMono44), FONT (SbSyne34),
                      14, 8, 8, 5, COL_CARD, COL_WHITE);
  }
  if (UseSyneIcon) {
    return GfxKeycap (X, BaseY, Key,
                      FONTW (SbMono19),
                      FONT (SbSyne22), FONT (SbSyne22),
                      7, 4, 4, 2, COL_CARD, COL_WHITE);
  }
  return GfxKeycap (X, BaseY, Key,
                    FONTW (SbMono19),
                    FONT (SbMono19), FONT (SbMono19),
                    7, 4, 4, 2, COL_CARD, COL_WHITE);
}

STATIC
EFI_STATUS
LaunchFsFile (
  IN EFI_HANDLE                       FsHandle,
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs,
  IN CONST CHAR16                     *FileName
  )
{
  EFI_FILE_PROTOCOL         *Root = NULL;
  EFI_FILE_PROTOCOL         *File = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Dp   = NULL;
  EFI_HANDLE                Child = NULL;
  EFI_STATUS                Status;
  UINT8                     InfoBuf[sizeof (EFI_FILE_INFO) + 256];
  UINTN                     InfoSize = sizeof (InfoBuf);
  UINTN                     Size = 0;
  UINT8                     *Buf = NULL;

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status) || (Root == NULL)) {
    return Status;
  }
  Status = Root->Open (Root, &File, (CHAR16 *)FileName, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (File == NULL)) {
    Root->Close (Root);
    return Status;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, InfoBuf);
  if (!EFI_ERROR (Status)) {
    Size = (UINTN)((EFI_FILE_INFO *)InfoBuf)->FileSize;
    Buf  = AllocatePool (Size);
    if (Buf != NULL) {
      Status = File->Read (File, &Size, Buf);
    } else {
      Status = EFI_OUT_OF_RESOURCES;
    }
  }
  File->Close (File);
  Root->Close (Root);

  if (EFI_ERROR (Status) || (Buf == NULL)) {
    if (Buf != NULL) {
      FreePool (Buf);
    }
    return Status;
  }

  Dp     = FileDevicePath (FsHandle, FileName);
  Status = gBS->LoadImage (FALSE, gImageHandle, Dp, Buf, Size, &Child);
  FreePool (Buf);
  if (Dp != NULL) {
    FreePool (Dp);
  }
  SbLog ("[boot] loadimg", (UINTN)Status, Size);
  if (!EFI_ERROR (Status)) {
    Status = gBS->StartImage (Child, NULL, NULL);
    SbLog ("[boot] startimg", (UINTN)Status, 0);
    GfxReapplyMode ();
  }
  return Status;
}

STATIC
BOOLEAN
TryDirectBootFromDp (
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp,
  IN UINTN                     DpMaxLen
  )
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count    = 0;
  UINTN       Index;
  UINT8       *Raw     = (UINT8 *)Dp;
  CHAR16      FilePath[260];
  UINTN       FpPos    = 0;
  BOOLEAN     IsHd     = FALSE;
  UINT32      HdPart   = 0;
  UINT8       HdSig[16];
  UINTN       DpPrefixLen = 0;

  if ((Dp == NULL) || (DpMaxLen < 4)) {
    return FALSE;
  }

  FilePath[0] = 0;
  ZeroMem (HdSig, sizeof (HdSig));

  {
    UINTN  Pos = 0;
    while (Pos + 4 <= DpMaxLen) {
      UINT8   Type    = Raw[Pos];
      UINT8   SubType = Raw[Pos + 1];
      UINT16  NodeLen = (UINT16)(Raw[Pos + 2] | (Raw[Pos + 3] << 8));

      if (NodeLen < 4 || Pos + NodeLen > DpMaxLen) {
        break;
      }
      if ((Type == END_DEVICE_PATH_TYPE) && (SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
        DpPrefixLen = Pos;
        break;
      }
      if ((Type == MEDIA_DEVICE_PATH) && (SubType == MEDIA_HARDDRIVE_DP) && (NodeLen >= 42)) {
        IsHd   = TRUE;
        HdPart = *(UINT32 *)(Raw + Pos + 4);
        CopyMem (HdSig, Raw + Pos + 24, 16);
      } else if ((Type == MEDIA_DEVICE_PATH) && (SubType == MEDIA_FILEPATH_DP) && (NodeLen > 4)) {
        UINTN  Chars = (NodeLen - 4) / sizeof (CHAR16);
        UINTN  K;
        for (K = 0; K < Chars && FpPos + 1 < 260; K++) {
          CHAR16  Ch = (CHAR16)(Raw[Pos + 4 + K * 2] | (Raw[Pos + 5 + K * 2] << 8));
          if (Ch == 0) {
            break;
          }
          FilePath[FpPos++] = Ch;
        }
        FilePath[FpPos] = 0;
      }
      Pos += NodeLen;
    }
  }

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol,
                     &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return FALSE;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
    EFI_DEVICE_PATH_PROTOCOL         *Hdp = NULL;
    BOOLEAN                          Match = FALSE;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
           &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) || (Fs == NULL)) {
      continue;
    }
    Hdp = DevicePathFromHandle (Handles[Index]);
    if (Hdp == NULL) {
      continue;
    }

    if (IsHd && (FilePath[0] != 0)) {
      UINT8  *Hp = (UINT8 *)Hdp;
      while (!(Hp[0] == END_DEVICE_PATH_TYPE && Hp[1] == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
        UINT16  NLen = (UINT16)(Hp[2] | (Hp[3] << 8));
        if (NLen < 4) {
          break;
        }
        if ((Hp[0] == MEDIA_DEVICE_PATH) && (Hp[1] == MEDIA_HARDDRIVE_DP) && (NLen >= 42)) {
          UINT32  PNum = *(UINT32 *)(Hp + 4);
          if ((PNum == HdPart) && (CompareMem (Hp + 24, HdSig, 16) == 0)) {
            Match = TRUE;
            break;
          }
        }
        Hp += NLen;
      }
      if (Match && !EFI_ERROR (LaunchFsFile (Handles[Index], Fs, FilePath))) {
        FreePool (Handles);
        return TRUE;
      }
    } else if (!IsHd && (DpPrefixLen >= 4)) {
      if (CompareMem (Hdp, Raw, DpPrefixLen) == 0) {
        CONST CHAR16  *Target = FilePath[0] != 0 ? FilePath : L"\\EFI\\BOOT\\BOOTX64.EFI";
        if (!EFI_ERROR (LaunchFsFile (Handles[Index], Fs, Target))) {
          FreePool (Handles);
          return TRUE;
        }
      }
    }
  }

  FreePool (Handles);
  return FALSE;
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

  /* Try direct LoadImage + StartImage from Boot#### DevicePath first. */
  {
    CHAR16  Name[10];
    UINT8   *Data = NULL;
    UINTN   Size  = 0;

    UnicodeSPrint (Name, sizeof (Name), L"Boot%04X", Num);
    if (!EFI_ERROR (ReadGlobalVar (Name, &Data, &Size)) && (Size > 8)) {
      UINT16  FpLen = (UINT16)(Data[4] | (Data[5] << 8));
      UINTN   Pos   = 6;

      while (Pos + 1 < Size) {
        UINT16  Ch = (UINT16)(Data[Pos] | (Data[Pos + 1] << 8));
        Pos += 2;
        if (Ch == 0) {
          break;
        }
      }
      if ((FpLen >= 4) && (Pos + FpLen <= Size)) {
        EFI_DEVICE_PATH_PROTOCOL  *Dp = (EFI_DEVICE_PATH_PROTOCOL *)(Data + Pos);
        if (TryDirectBootFromDp (Dp, FpLen)) {
          FreePool (Data);
          return;
        }
      }
      FreePool (Data);
    }
  }

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

  /* card block: hand-drawn corner brackets, thin pencil stroke */
  {
    INT32  Ui = GfxUi ();
    INT32  Pad = 10 * Ui;
    INT32  Bx = Xc - Pad, By = Y0 - Pad;
    INT32  Bw = Wc + Pad * 2, Bh = ITEMS * Ch + (ITEMS - 1) * Gap + Pad * 2;

    GfxPencilCorners (Bx, By, Bw, Bh, 16 * Ui, Ui >= 2 ? 2 : 1,
                      (UINT32)(Bx + By * 7), PC_ALL, COL_INK);
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

  {
    BOOLEAN  Hi = (GfxUi () >= 2);
    INT32    CardAsc = Hi ? GfxAscent (FONTW (SbMono44)) : GfxAscent (FONTW (SbMono24));
    INT32    PadX    = Hi ? 24 : 14;

    for (I = 0; I < ITEMS; I++) {
      Y = Y0 + I * (Ch + Gap);
      GfxFill (Xc, Y, Wc, Ch, (UINTN)I == mSel ? COL_ORANGE : COL_CARD);
      UnicodeSPrint (Line, sizeof (Line), L"%s%s", Pre[I], Val[I]);
      if (Hi) {
        GfxText (Xc + PadX, Y + (Ch + CardAsc) / 2, FONT (SbMono44), Line, COL_WHITE, 1);
      } else {
        GfxText (Xc + PadX, Y + (Ch + CardAsc) / 2, FONT (SbMono24), Line, COL_WHITE, 1);
      }
    }

    /* press [↩] to continue */
    {
      INT32  Py = H * 62 / 100;
      INT32  Px = Mar;
      INT32  GapK = Hi ? 18 : 9;

      if (Hi) {
        Px += GfxText (Px, Py, FONT (SbSyne64), L"PRESS", COL_INK, 1) + GapK;
        Px  = DrawKeycap (Px, Py - 6, L"\x21B5", TRUE, TRUE) + GapK;
        GfxText (Px, Py, FONT (SbSyne64), L"TO CONTINUE", COL_INK, 1);
      } else {
        Px += GfxText (Px, Py, FONT (SbSyne34), L"PRESS", COL_INK, 1) + GapK;
        Px  = DrawKeycap (Px, Py - 2, L"\x21B5", FALSE, TRUE) + GapK;
        GfxText (Px, Py, FONT (SbSyne34), L"TO CONTINUE", COL_INK, 1);
      }
    }

    /* footer keycap hints + signature */
    {
      INT32  Fy   = H * 78 / 100;
      INT32  Fx   = Mar;
      INT32  Sp   = Hi ? 14 : 7;
      INT32  Sep  = Hi ? 34 : 18;

      if (Hi) {
        Fx = DrawKeycap (Fx, Fy, L"\x21C5", TRUE, TRUE) + Sp;
        Fx += GfxText (Fx, Fy, FONT (SbMono44), L"NAVIGATE", COL_INK, 1) + Sep;
        Fx = DrawKeycap (Fx, Fy, L"E", TRUE, FALSE) + Sp;
        Fx += GfxText (Fx, Fy, FONT (SbMono44), L"EDIT", COL_INK, 1) + Sep;
        Fx = DrawKeycap (Fx, Fy, L"C", TRUE, FALSE) + Sp;
        Fx += GfxText (Fx, Fy, FONT (SbMono44), L"CONSOLE", COL_INK, 1) + Sep;
        Fx = DrawKeycap (Fx, Fy, L"F10", TRUE, FALSE) + Sp;
        GfxText (Fx, Fy, FONT (SbMono44), L"SETUP", COL_INK, 1);
        GfxText (Mar, H * 87 / 100, FONT (SbMono44), SIGN_TXT, COL_INK, 1);
      } else {
        Fx = DrawKeycap (Fx, Fy, L"\x21C5", FALSE, TRUE) + Sp;
        Fx += GfxText (Fx, Fy, FONT (SbMono19), L"NAVIGATE", COL_INK, 1) + Sep;
        Fx = DrawKeycap (Fx, Fy, L"E", FALSE, FALSE) + Sp;
        Fx += GfxText (Fx, Fy, FONT (SbMono19), L"EDIT", COL_INK, 1) + Sep;
        Fx = DrawKeycap (Fx, Fy, L"C", FALSE, FALSE) + Sp;
        Fx += GfxText (Fx, Fy, FONT (SbMono19), L"CONSOLE", COL_INK, 1) + Sep;
        Fx = DrawKeycap (Fx, Fy, L"F10", FALSE, FALSE) + Sp;
        GfxText (Fx, Fy, FONT (SbMono19), L"SETUP", COL_INK, 1);
        GfxText (Mar, H * 87 / 100, FONT (SbMono19), SIGN_TXT, COL_INK, 1);
      }
    }

    /*
     * Brand mark: lightning polygon rounded like a fat marker stroke:
     * every convex corner gets a constant-radius arc (the tips read as
     * round caps), concave notches stay sharp.  Per corner the two cut
     * points sit at radius distance along the edges and three conic
     * samples bulge through the vertex, all in integer math.
     */
    {
      INTN   BoltX = W * 64 / 100;
      INTN   BoltW = H * 36 / 100;
      INTN   BoltH = H * 48 / 100;
      INTN   Tw, X, R, Area;
      INT32  Base[6][2];
      INT32  P0[64][2];
      INTN   N = 0, J, T;
      INTN   Kx[6] = { 600, 100, 400, 250, 900, 550 };
      INTN   Ky[6] = { 0, 500, 500, 1000, 400, 400 };

      for (I = 0; I < 6; I++) {
        Base[I][0] = BoltX + Kx[I] * BoltW / 1000;
        Base[I][1] = H * 8 / 100 + Ky[I] * BoltH / 1000;
      }

      Area = 0;
      for (I = 0; I < 6; I++) {
        J = (I + 1) % 6;
        Area += Base[I][0] * Base[J][1] - Base[J][0] * Base[I][1];
      }
      R = BoltW / 8;

      for (I = 0; I < 6; I++) {
        INT32  *Pv = Base[I];
        INT32  *Pa = Base[(I + 5) % 6];
        INT32  *Pb = Base[(I + 1) % 6];
        INTN    Ax = Pa[0] - Pv[0];
        INTN    Ay = Pa[1] - Pv[1];
        INTN    Bx = Pb[0] - Pv[0];
        INTN    By = Pb[1] - Pv[1];
        INTN    La = IntSqrt (Ax * Ax + Ay * Ay);
        INTN    Lb = IntSqrt (Bx * Bx + By * By);
        INTN    Ra = R;
        INTN    Rb = R;
        INT32   In0, In1, Out0, Out1;

        /* screen space is Y-down, so convexity means opposite signs */
        if ((Ax * By - Ay * Bx) * Area >= 0) {
          /* concave: keep the notch sharp */
          P0[N][0] = Pv[0];
          P0[N][1] = Pv[1];
          N++;
          continue;
        }
        if (Ra * 2 > La) {
          Ra = La / 2;
        }
        if (Rb * 2 > Lb) {
          Rb = Lb / 2;
        }
        In0  = Pv[0] + Ax * 1000 / La * Ra / 1000;
        In1  = Pv[1] + Ay * 1000 / La * Ra / 1000;
        Out0 = Pv[0] + Bx * 1000 / Lb * Rb / 1000;
        Out1 = Pv[1] + By * 1000 / Lb * Rb / 1000;
        P0[N][0] = In0;
        P0[N][1] = In1;
        N++;
        for (T = 2; T <= 6; T += 2) {
          INTN  W0 = (8 - T) * (8 - T);
          INTN  W1 = 2 * T * (8 - T);
          INTN  W2 = T * T;

          P0[N][0] = (INT32)((W0 * In0 + W1 * Pv[0] + W2 * Out0) / 64);
          P0[N][1] = (INT32)((W0 * In1 + W1 * Pv[1] + W2 * Out1) / 64);
          N++;
        }
        P0[N][0] = Out0;
        P0[N][1] = Out1;
        N++;
      }
      GfxPolyFill (P0, (UINTN)N, COL_ORANGE);

      /* centered under the bolt, clamped to the page margins */
      if (Hi) {
        Tw = GfxTextW (FONTW (SbSyne64), POWER_TXT, 1);
        X  = BoltX + BoltW / 2 - Tw / 2;
        if (X + Tw > W - Mar) {
          X = W - Mar - Tw;
        }
        if (X < Mar) {
          X = Mar;
        }
        GfxText (X, H * 64 / 100, FONT (SbSyne64), POWER_TXT, COL_INK, 1);
      } else {
        Tw = GfxTextW (FONTW (SbSyne26), POWER_TXT, 1);
        X  = BoltX + BoltW / 2 - Tw / 2;
        if (X + Tw > W - Mar) {
          X = W - Mar - Tw;
        }
        if (X < Mar) {
          X = Mar;
        }
        GfxText (X, H * 64 / 100, FONT (SbSyne26), POWER_TXT, COL_INK, 1);
      }
    }

    if (mStatusMsg[0] != 0) {
      if (Hi) {
        GfxText (Mar, H * 94 / 100, FONT (SbMono44), mStatusMsg, COL_CARD, 1);
      } else {
        GfxText (Mar, H * 94 / 100, FONT (SbMono19), mStatusMsg, COL_CARD, 1);
      }
    }
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
