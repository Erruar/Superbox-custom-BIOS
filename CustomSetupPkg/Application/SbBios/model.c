/*
 * model.c - HII form model.
 *
 * Enumerates every package list in the stock EFI_HII_DATABASE_PROTOCOL,
 * exports the IFR and builds an in-memory tree of formsets/forms/statements.
 * No vendor binary is patched: the standard ExportPackageLists() view of the
 * database is the only input, exactly like the stock SetupUtilityApp uses.
 *
 * Settings are cached per formset from the EFI variable described by the
 * IFR varstore opcode and written back with SetVariable(), which keeps the
 * prototype independent of the (vendor specific) display engine.
 */

#include "SbBios.h"
#include <Protocol/HiiConfigAccess.h>

#define SCOPE_FORMSET   1
#define SCOPE_FORM      2
#define SCOPE_QUESTION  3
#define SCOPE_OTHER     4

#define MAX_SCOPE_DEPTH 24

typedef struct {
  UINT8     Kind;
  FORMSET   *Fs;
  FORM      *Form;
  STMT      *Stmt;
} SCOPE;

STATIC EFI_HII_DATABASE_PROTOCOL       *mHiiDb  = NULL;
STATIC EFI_HII_STRING_PROTOCOL         *mHiiStr = NULL;
STATIC EFI_HII_CONFIG_ROUTING_PROTOCOL *mCfg    = NULL;
STATIC FORMSET                         *mFormsets = NULL;

/* Insyde H2O EFI_SETUP_UTILITY_APPLICATION_PROTOCOL_GUID */
STATIC EFI_GUID  mSetupAppGuid = {
  0xE7A97370, 0x00FF, 0x4BBA, { 0xBB, 0xA0, 0xD0, 0xBC, 0x20, 0x0C, 0xCE, 0x98 }
};
STATIC EFI_GUID  mFormBrowser2Guid = {
  0xB9D4C360, 0xBCFB, 0x4F9B, { 0x92, 0x98, 0x53, 0xC1, 0x36, 0x98, 0x22, 0x58 }
};
STATIC EFI_GUID  mHiiConfigAccessGuid = {
  0x330D4706, 0xF2A0, 0x4E4F, { 0xA3, 0x69, 0xB6, 0x6F, 0xA8, 0xD5, 0x43, 0x85 }
};

typedef
EFI_STATUS
(EFIAPI *SB_SEND_FORM) (
  IN CONST VOID  *This,
  IN VOID        *Handles,
  IN UINTN       HandleCount,
  IN EFI_GUID    *FormSetGuid,
  IN UINT16      FormId,
  IN CONST VOID  *ScreenDimensions,
  OUT UINTN      *ActionRequest
  );

typedef
EFI_STATUS
(EFIAPI *SB_BROWSER_CALLBACK) (
  IN CONST VOID      *This,
  IN OUT UINTN       *ResultsDataSize,
  IN OUT EFI_STRING  ResultsData,
  IN BOOLEAN         RetrieveData,
  IN CONST EFI_GUID  *VariableGuid,
  IN CONST CHAR16    *VariableName
  );

typedef struct {
  SB_SEND_FORM         SendForm;
  SB_BROWSER_CALLBACK  BrowserCallback;
} SB_FORM_BROWSER2;

STATIC EFI_HANDLE       mSetupAppHandle = NULL;
STATIC UINT32          *mSetupAppPhase  = NULL;
STATIC EFI_RAISE_TPL    mOrigRaiseTpl   = NULL;
STATIC SB_SETUP_RUNNER  mActiveRunner   = NULL;
STATIC BOOLEAN          mRunnerRan      = FALSE;
STATIC VARSTORE        *mActiveFlushVar = NULL;

STATIC
EFI_TPL
EFIAPI
SafeRaiseTpl (
  IN EFI_TPL  NewTpl
  )
{
  if (NewTpl == 6) {
    return 7;
  }
  return mOrigRaiseTpl (NewTpl);
}

STATIC
EFI_STATUS
EFIAPI
NoopClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  )
{
  (VOID)This;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
HookSendForm (
  IN CONST VOID  *This,
  IN VOID        *Handles,
  IN UINTN       HandleCount,
  IN EFI_GUID    *FormSetGuid,
  IN UINT16      FormId,
  IN CONST VOID  *ScreenDimensions,
  OUT UINTN      *ActionRequest
  )
{
  BOOLEAN  WantBoot = TRUE;

  (VOID)This;
  (VOID)Handles;
  (VOID)HandleCount;
  (VOID)FormSetGuid;
  (VOID)FormId;
  (VOID)ScreenDimensions;

  mRunnerRan = TRUE;
  if (mActiveRunner != NULL) {
    WantBoot = mActiveRunner ();
  }

  if (ActionRequest != NULL) {
    *ActionRequest = WantBoot ? 3 : 1; /* 3 = EXIT, 1 = RESET */
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
HookBrowserCallback (
  IN CONST VOID      *This,
  IN OUT UINTN       *ResultsDataSize,
  IN OUT EFI_STRING  ResultsData,
  IN BOOLEAN         RetrieveData,
  IN CONST EFI_GUID  *VariableGuid,
  IN CONST CHAR16    *VariableName
  )
{
  VARSTORE    *V = mActiveFlushVar;
  EFI_STRING  CfgResp = NULL;
  EFI_STRING  Prog    = NULL;
  CHAR16      *OffPtr;
  UINTN       NeedBytes;
  EFI_STATUS  Status;

  (VOID)This;
  (VOID)VariableGuid;
  (VOID)VariableName;

  if (!RetrieveData) {
    return EFI_SUCCESS;
  }
  if (ResultsDataSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((V == NULL) || (V->Data == NULL) || (mCfg == NULL)) {
    return EFI_NOT_FOUND;
  }

  {
    CHAR16  ReqBuf[128];

    UnicodeSPrint (
      ReqBuf, sizeof (ReqBuf),
      L"GUID=00000000000000000000000000000000&NAME=0000&PATH=00&OFFSET=0&WIDTH=%04X",
      (UINTN)V->DataSize
      );
    Status = mCfg->BlockToConfig (
                     mCfg,
                     ReqBuf,
                     V->Data,
                     V->DataSize,
                     &CfgResp,
                     &Prog
                     );
  }
  if (EFI_ERROR (Status) || (CfgResp == NULL)) {
    return EFI_NOT_FOUND;
  }

  OffPtr = CfgResp;
  while (*OffPtr != 0) {
    if (StrnCmp (OffPtr, L"OFFSET=", 7) == 0) {
      break;
    }
    OffPtr++;
  }
  if (*OffPtr == 0) {
    FreePool (CfgResp);
    return EFI_NOT_FOUND;
  }

  NeedBytes = (StrLen (OffPtr) + 1) * sizeof (CHAR16);
  if ((*ResultsDataSize < NeedBytes) || (ResultsData == NULL)) {
    *ResultsDataSize = NeedBytes;
    FreePool (CfgResp);
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (ResultsData, OffPtr, NeedBytes);
  *ResultsDataSize = NeedBytes;
  FreePool (CfgResp);
  return EFI_SUCCESS;
}

STATIC
EFI_HII_CONFIG_ACCESS_PROTOCOL *
GetConfigAccess (
  IN FORMSET  *Fs
  )
{
  EFI_HANDLE                      DriverHandle = NULL;
  EFI_HII_CONFIG_ACCESS_PROTOCOL  *Ca          = NULL;
  EFI_STATUS                      Status;

  if ((Fs == NULL) || (Fs->Handle == NULL) || (mHiiDb == NULL)) {
    return NULL;
  }
  Status = mHiiDb->GetPackageListHandle (mHiiDb, Fs->Handle, &DriverHandle);
  if (EFI_ERROR (Status) || (DriverHandle == NULL)) {
    return NULL;
  }
  Status = gBS->HandleProtocol (DriverHandle, &mHiiConfigAccessGuid, (VOID **)&Ca);
  if (EFI_ERROR (Status)) {
    return NULL;
  }
  return Ca;
}

STATIC
CHAR16 *
DupConfigHdr (
  IN CONST CHAR16  *FullResp
  )
{
  CONST CHAR16  *P;
  UINTN         Len;
  CHAR16        *Hdr;

  if (FullResp == NULL) {
    return NULL;
  }
  P = FullResp;
  while (*P != 0) {
    if ((P[0] == L'&') && (StrnCmp (P, L"&OFFSET=", 8) == 0)) {
      break;
    }
    P++;
  }
  Len = (UINTN)(P - FullResp);
  Hdr = AllocateZeroPool ((Len + 1) * sizeof (CHAR16));
  if (Hdr != NULL) {
    CopyMem (Hdr, FullResp, Len * sizeof (CHAR16));
  }
  return Hdr;
}

/* ------------------------------------------------------------- utilities */

STATIC
CHAR16 *
StrDup (
  IN CONST CHAR16  *S
  )
{
  CHAR16  *D;
  UINTN   Bytes;

  if (S == NULL) {
    return NULL;
  }
  Bytes = (StrLen (S) + 1) * sizeof (CHAR16);
  D     = AllocatePool (Bytes);
  if (D != NULL) {
    CopyMem (D, S, Bytes);
  }
  return D;
}

STATIC
UINT64
ReadLe (
  IN CONST UINT8  *P,
  IN UINTN        Width
  )
{
  UINT64  V = 0;
  UINTN   I;

  for (I = 0; I < Width && I < 8; I++) {
    V |= (UINT64)P[I] << (8 * I);
  }
  return V;
}

STATIC
VOID
WriteLe (
  IN UINT8  *P,
  IN UINTN  Width,
  IN UINT64 Value
  )
{
  UINTN  I;

  for (I = 0; I < Width && I < 8; I++) {
    P[I] = (UINT8)((Value >> (8 * I)) & 0xFF);
  }
}

STATIC
UINTN
WidthOfFlags (
  IN UINT8  Flags
  )
{
  switch (Flags & EFI_IFR_NUMERIC_SIZE) {
    case EFI_IFR_NUMERIC_SIZE_8: return 8;
    case EFI_IFR_NUMERIC_SIZE_4: return 4;
    case EFI_IFR_NUMERIC_SIZE_2: return 2;
    default:                     return 1;
  }
}

/* ---------------------------------------------------------------- strings */

CHAR16 *
ModelGetString (
  IN FORMSET        *Fs,
  IN EFI_STRING_ID  Id
  )
{
  CHAR16     *Buf;
  UINTN      Size;
  EFI_STATUS Status;

  if ((Fs == NULL) || (Id == 0) || (mHiiStr == NULL)) {
    return NULL;
  }

  Size = 512 * sizeof (CHAR16);
  Buf  = AllocateZeroPool (Size);
  if (Buf == NULL) {
    return NULL;
  }

  Status = mHiiStr->GetString (
                      mHiiStr, Fs->Lang, Fs->Handle, Id, Buf, &Size, NULL);
  if (EFI_ERROR (Status)) {
    /* Fall back to the generic language tag used by the platform. */
    Size = 512 * sizeof (CHAR16);
    Status = mHiiStr->GetString (mHiiStr, "en-us", Fs->Handle, Id, Buf, &Size, NULL);
  }
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return NULL;
  }
  return Buf;
}

/* -------------------------------------------------------------- varstores */

STATIC
VARSTORE *
AddVarstore (
  IN FORMSET  *Fs,
  IN UINT8    *Op,
  IN UINT8    OpCode,
  IN UINT8    Len
  )
{
  VARSTORE          *V;
  EFI_IFR_VARSTORE  *Classic;
  EFI_IFR_VARSTORE_EFI  *Efi;
  UINTN             Index;

  V = AllocateZeroPool (sizeof (VARSTORE));
  if (V == NULL) {
    return NULL;
  }

  if (OpCode == EFI_IFR_VARSTORE_EFI_OP) {
    Efi = (EFI_IFR_VARSTORE_EFI *)Op;
    V->Type = VS_EFI;
    V->Id   = Efi->VarStoreId;
    V->Guid = Efi->Guid;
    V->Size = Efi->Size;
    V->Attributes = Efi->Attributes;
    V->Name[71] = 0;
    for (Index = 0; Index < 71 && Efi->Name[Index] != 0; Index++) {
      V->Name[Index] = (CHAR16)Efi->Name[Index];
    }
  } else if (OpCode == EFI_IFR_VARSTORE_OP) {
    Classic = (EFI_IFR_VARSTORE *)Op;
    V->Type = VS_CLASSIC;
    V->Id   = Classic->VarStoreId;
    V->Guid = Classic->Guid;
    V->Size = Classic->Size;
    for (Index = 0; Index < 71 && Classic->Name[Index] != 0; Index++) {
      V->Name[Index] = (CHAR16)Classic->Name[Index];
    }
  } else {
    EFI_IFR_VARSTORE_NAME_VALUE  *Nv = (EFI_IFR_VARSTORE_NAME_VALUE *)Op;
    V->Type     = VS_NAMEVALUE;
    V->Id       = Nv->VarStoreId;
    V->Guid     = Nv->Guid;
    V->ReadOnly = TRUE;
  }

  (VOID)Len;

  V->Next = Fs->Vars;
  Fs->Vars = V;
  Fs->VarCount++;
  return V;
}

VARSTORE *
ModelVarstore (
  IN FORMSET  *Fs,
  IN UINT16   VarStoreId
  )
{
  VARSTORE  *V;

  if (Fs == NULL) {
    return NULL;
  }
  for (V = Fs->Vars; V != NULL; V = V->Next) {
    if (V->Id == VarStoreId) {
      return V;
    }
  }
  return NULL;
}

EFI_STATUS
ModelLoad (
  IN FORMSET  *Fs
  )
{
  VARSTORE                        *V;
  UINTN                           Size;
  UINT32                          Attributes;
  EFI_STATUS                      Status;
  EFI_HII_CONFIG_ACCESS_PROTOCOL  *Ca;
  EFI_STRING                      FullResp = NULL;
  EFI_STRING                      Progress = NULL;

  if (Fs == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Ca = GetConfigAccess (Fs);
  if ((Ca != NULL) && (mCfg != NULL)) {
    Status = Ca->ExtractConfig (Ca, NULL, &Progress, &FullResp);
    if (EFI_ERROR (Status)) {
      FullResp = NULL;
    }
  }

  for (V = Fs->Vars; V != NULL; V = V->Next) {
    CONST CHAR16  *NvName;

    if ((V->Type != VS_EFI) && (V->Type != VS_CLASSIC)) {
      continue;
    }
    if (V->Dirty && (V->Data != NULL)) {
      continue;
    }

    Size = V->Size != 0 ? V->Size : 4;
    if ((V->Data == NULL) || (V->DataSize < Size)) {
      if (V->Data != NULL) {
        FreePool (V->Data);
      }
      V->Data = AllocateZeroPool (Size);
      if (V->Data == NULL) {
        if (FullResp != NULL) {
          FreePool (FullResp);
        }
        return EFI_OUT_OF_RESOURCES;
      }
      V->DataSize = Size;
    }

    if ((FullResp != NULL) && (mCfg != NULL) &&
        (StrCmp (V->Name, L"SystemConfig") == 0)) {
      UINTN  BlockSize = V->DataSize;
      Status = mCfg->ConfigToBlock (mCfg, FullResp, V->Data, &BlockSize, &Progress);
      if (!EFI_ERROR (Status)) {
        V->Present  = TRUE;
        V->ReadOnly = FALSE;
        continue;
      }
    }

    NvName     = V->Name;
    Size       = 0;
    Attributes = 0;

    Status = gRT->GetVariable ((CHAR16 *)NvName, &V->Guid, &Attributes, &Size, NULL);
    if ((Status == EFI_NOT_FOUND) && (V->Type == VS_CLASSIC) &&
        (StrCmp (V->Name, L"SystemConfig") == 0)) {
      NvName = L"Setup";
      Size   = 0;
      Status = gRT->GetVariable ((CHAR16 *)NvName, &V->Guid, &Attributes, &Size, NULL);
    }
    if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
      if (V->Type == VS_CLASSIC) {
        V->ReadOnly = TRUE;
        continue;
      }
      Size = V->Size;
    }
    if (Size < V->Size) {
      Size = V->Size;
    }
    if (Size == 0) {
      Size = 4;
    }
    if (V->DataSize < Size) {
      FreePool (V->Data);
      V->Data = AllocateZeroPool (Size);
      if (V->Data == NULL) {
        if (FullResp != NULL) {
          FreePool (FullResp);
        }
        return EFI_OUT_OF_RESOURCES;
      }
    }

    Status = gRT->GetVariable ((CHAR16 *)NvName, &V->Guid, &Attributes, &Size, V->Data);
    if (!EFI_ERROR (Status)) {
      V->Present    = TRUE;
      V->Attributes = Attributes;
      V->DataSize   = Size;
      if ((UINT16)Size > V->Size) {
        V->Size = (UINT16)Size;
      }
    } else {
      V->Present  = TRUE;
      V->DataSize = V->Size;
      V->Attributes = EFI_VARIABLE_NON_VOLATILE |
                      EFI_VARIABLE_BOOTSERVICE_ACCESS |
                      EFI_VARIABLE_RUNTIME_ACCESS;
    }
  }

  if (FullResp != NULL) {
    FreePool (FullResp);
  }
  return EFI_SUCCESS;
}

EFI_STATUS
ModelFlush (
  IN FORMSET  *Fs
  )
{
  VARSTORE                        *V;
  EFI_STATUS                      Status;
  EFI_STATUS                      First = EFI_SUCCESS;
  EFI_HII_CONFIG_ACCESS_PROTOCOL  *Ca;
  EFI_STRING                      FullResp = NULL;
  CHAR16                          *Hdr     = NULL;
  EFI_STRING                      Progress = NULL;

  if (Fs == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Ca = GetConfigAccess (Fs);
  if ((Ca != NULL) && (mCfg != NULL)) {
    if (!EFI_ERROR (Ca->ExtractConfig (Ca, NULL, &Progress, &FullResp)) &&
        (FullResp != NULL)) {
      Hdr = DupConfigHdr (FullResp);
      FreePool (FullResp);
    }
  }

  for (V = Fs->Vars; V != NULL; V = V->Next) {
    CONST CHAR16  *NvName;

    if (!V->Dirty || (V->Data == NULL)) {
      continue;
    }

    Status = EFI_UNSUPPORTED;
    if ((Ca != NULL) && (mCfg != NULL) && (Hdr != NULL) &&
        (StrCmp (V->Name, L"SystemConfig") == 0)) {
      UINTN       ReqChars   = StrLen (Hdr) + 32;
      CHAR16      *Req       = AllocateZeroPool (ReqChars * sizeof (CHAR16));
      EFI_STRING  ConfigResp = NULL;

      if (Req != NULL) {
        UnicodeSPrint (Req, ReqChars * sizeof (CHAR16), L"%s&OFFSET=0&WIDTH=%04X",
                       Hdr, (UINTN)V->DataSize);
        Status = mCfg->BlockToConfig (mCfg, Req, V->Data, V->DataSize, &ConfigResp, &Progress);
        SbLog ("[model] btc", (UINTN)Status, (UINTN)V->DataSize);
        FreePool (Req);
      }
      if (!EFI_ERROR (Status) && (ConfigResp != NULL)) {
        mActiveFlushVar = V;
        Status = Ca->RouteConfig (Ca, ConfigResp, &Progress);
        mActiveFlushVar = NULL;
        SbLog ("[model] route", (UINTN)Status, 0);
        FreePool (ConfigResp);
      }
    } else {
      SbLog6 ("[model] flush skip", (UINTN)(Ca != NULL), (UINTN)(mCfg != NULL),
              (UINTN)(Hdr != NULL), (UINTN)V->DataSize, 0, 0);
    }

    if (EFI_ERROR (Status)) {
      NvName = (V->Type == VS_CLASSIC && StrCmp (V->Name, L"SystemConfig") == 0)
               ? L"Setup" : V->Name;
      Status = gRT->SetVariable (
                      (CHAR16 *)NvName, &V->Guid,
                      V->Attributes != 0 ? V->Attributes :
                        (EFI_VARIABLE_NON_VOLATILE |
                         EFI_VARIABLE_BOOTSERVICE_ACCESS |
                         EFI_VARIABLE_RUNTIME_ACCESS),
                      V->DataSize, V->Data);
    }

    if (EFI_ERROR (Status) && !EFI_ERROR (First)) {
      First = Status;
    }
    if (!EFI_ERROR (Status)) {
      V->Dirty = FALSE;
    }
  }

  if (Hdr != NULL) {
    FreePool (Hdr);
  }
  return First;
}

VOID
ModelNotifyChanged (
  IN FORMSET  *Fs,
  IN STMT     *S,
  IN UINT64   Value
  )
{
  EFI_HII_CONFIG_ACCESS_PROTOCOL  *Ca;
  EFI_IFR_TYPE_VALUE              Val;
  EFI_BROWSER_ACTION_REQUEST      Req = EFI_BROWSER_ACTION_REQUEST_NONE;
  UINT8                           Type;

  if ((Fs == NULL) || (S == NULL) || (S->QuestionId == 0)) {
    return;
  }
  Ca = GetConfigAccess (Fs);
  if ((Ca == NULL) || (Ca->Callback == NULL)) {
    return;
  }

  ZeroMem (&Val, sizeof (Val));
  if (S->Kind == ST_CHECKBOX) {
    Type  = EFI_IFR_TYPE_BOOLEAN;
    Val.b = (BOOLEAN)(Value != 0);
  } else {
    switch (S->Width) {
      case 8:  Type = EFI_IFR_TYPE_NUM_SIZE_64; Val.u64 = Value; break;
      case 4:  Type = EFI_IFR_TYPE_NUM_SIZE_32; Val.u32 = (UINT32)Value; break;
      case 2:  Type = EFI_IFR_TYPE_NUM_SIZE_16; Val.u16 = (UINT16)Value; break;
      default: Type = EFI_IFR_TYPE_NUM_SIZE_8;  Val.u8  = (UINT8)Value; break;
    }
  }

  Ca->Callback (Ca, EFI_BROWSER_ACTION_CHANGED, S->QuestionId, Type, &Val, &Req);
}

/*
 * Reorder or toggle a dynamic boot-order action item (Op: 1=up, 0=down,
 * 2=toggle).  Tries the standard changed-action dispatch with synthetic
 * command QuestionIds first, then falls back to the 0x8001 item callback.
 */
EFI_STATUS
ModelBootOrderOp (
  IN FORMSET  *Fs,
  IN STMT     *S,
  IN UINT8    Op
  )
{
  EFI_HII_CONFIG_ACCESS_PROTOCOL  *Ca;
  EFI_IFR_TYPE_VALUE              Val;
  EFI_BROWSER_ACTION_REQUEST      Req = EFI_BROWSER_ACTION_REQUEST_NONE;
  UINT16                          CmdQId;
  EFI_STATUS                      Status;

  if ((Fs == NULL) || (S == NULL) || (S->QuestionId == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Ca = GetConfigAccess (Fs);
  if ((Ca == NULL) || (Ca->Callback == NULL)) {
    return EFI_UNSUPPORTED;
  }

  CmdQId = (Op == 1) ? 0x103Au : (Op == 0) ? 0x103Bu : 0x103Cu;

  ZeroMem (&Val, sizeof (Val));
  Val.u32 = (UINT32)S->QuestionId;
  Status = Ca->Callback (
                 Ca,
                 EFI_BROWSER_ACTION_CHANGED,
                 CmdQId,
                 EFI_IFR_TYPE_NUM_SIZE_32,
                 &Val,
                 &Req
                 );
  if (EFI_ERROR (Status)) {
    ZeroMem (&Val, sizeof (Val));
    Val.u8 = Op;
    Status = Ca->Callback (
                   Ca,
                   0x8001,
                   S->QuestionId,
                   EFI_IFR_TYPE_NUM_SIZE_8,
                   &Val,
                   &Req
                   );
  }

  if (!EFI_ERROR (Status)) {
    VARSTORE  *V;
    for (V = Fs->Vars; V != NULL; V = V->Next) {
      if (StrCmp (V->Name, L"SystemConfig") == 0) {
        V->Dirty = TRUE;
        break;
      }
    }
  }

  SbLog6 ("[model] bootop", (UINTN)Status, S->QuestionId, Op, CmdQId, 0, 0);
  return Status;
}

/* ------------------------------------------------------- value access */

UINT64
ModelRead (
  IN  STMT      *S,
  IN  VARSTORE  *V,
  OUT BOOLEAN   *Valid
  )
{
  UINT64  Value = 0;

  if (Valid != NULL) {
    *Valid = FALSE;
  }
  if ((S == NULL) || (V == NULL) || (V->Data == NULL)) {
    return 0;
  }

  if (S->Bit) {
    if ((S->Offset / 8) < V->DataSize) {
      Value = (V->Data[S->Offset / 8] >> (S->Offset % 8)) & 1;
      if (Valid != NULL) {
        *Valid = TRUE;
      }
    }
    return Value;
  }

  if (((UINTN)S->Offset + S->Width) <= V->DataSize) {
    Value = ReadLe (&V->Data[S->Offset], S->Width);
    if (Valid != NULL) {
      *Valid = TRUE;
    }
  }
  return Value;
}

EFI_STATUS
ModelWrite (
  IN STMT      *S,
  IN VARSTORE  *V,
  IN UINT64    Value
  )
{
  if ((S == NULL) || (V == NULL) || (V->Data == NULL) || V->ReadOnly) {
    return EFI_ACCESS_DENIED;
  }

  if (S->Bit) {
    if ((S->Offset / 8) >= V->DataSize) {
      return EFI_INVALID_PARAMETER;
    }
    if ((Value & 1) != 0) {
      V->Data[S->Offset / 8] |= (UINT8)(1u << (S->Offset % 8));
    } else {
      V->Data[S->Offset / 8] &= (UINT8)~(1u << (S->Offset % 8));
    }
  } else {
    if (((UINTN)S->Offset + S->Width) > V->DataSize) {
      return EFI_INVALID_PARAMETER;
    }
    WriteLe (&V->Data[S->Offset], S->Width, Value);
  }

  V->Dirty = TRUE;
  return EFI_SUCCESS;
}

/*
 * Normalize a raw varstore value for ONEOF comparison.  Insyde packs
 * many selects into 2-byte fields while the ONE_OF_OPTION entries carry
 * 1-byte values (e.g. stored 0x103, option 0x03), so compare the
 * significant bytes of both sides.
 */
UINT64
NormValue (
  IN STMT   *S,
  IN UINT64  Value
  )
{
  UINTN  W = S->Width != 0 ? S->Width : 1;

  if (W >= 8) {
    return Value;
  }
  return Value & (((UINT64)1 << (8 * W)) - 1);
}

CHAR16 *
ModelValueText (
  IN STMT      *S,
  IN VARSTORE  *V
  )
{
  CHAR16  *Text;
  UINT64   Value;
  BOOLEAN  Valid;
  UINTN    I;

  Text = StrDup (L"");
  if ((S == NULL) || (V == NULL)) {
    return Text;
  }

  Value = ModelRead (S, V, &Valid);
  if (!Valid) {
    FreePool (Text);
    return StrDup (L"n/a");
  }

  switch (S->Kind) {
    case ST_ONEOF:
      Value = NormValue (S, Value);
      for (I = 0; I < S->OptionCount; I++) {
        if (NormValue (S, S->Options[I].Value) == Value) {
          return StrDup (S->Options[I].Label != NULL ? S->Options[I].Label : L"?");
        }
      }
      if (S->OptionCount > 0) {
        /*
         * The stored value is outside the published option set.  Show
         * it as a plain number and log the raw comparison once per
         * question: the field's bytes tell exactly why the match
         * failed (width, flags, option values).
         */
        STATIC STMT  *DbgSeen[24];
        UINTN        K;
        BOOLEAN      Fresh = TRUE;

        for (K = 0; K < sizeof (DbgSeen) / sizeof (DbgSeen[0]); K++) {
          if (DbgSeen[K] == S) {
            Fresh = FALSE;
            break;
          }
        }
        if (Fresh) {
          for (K = 0; K < sizeof (DbgSeen) / sizeof (DbgSeen[0]); K++) {
            if (DbgSeen[K] == NULL) {
              DbgSeen[K] = S;
              break;
            }
          }
          SbLog6 ("[val] nm", (UINTN)S->Prompt,
                  ((UINTN)S->Width << 8) | (UINTN)S->NumFlags,
                  Value, S->OptionCount,
                  S->Options[0].Value,
                  S->OptionCount > 1 ? S->Options[1].Value : 0);
        }
        FreePool (Text);
        Text = AllocateZeroPool (48 * sizeof (CHAR16));
        if (Text != NULL) {
          UnicodeSPrint (Text, 48 * sizeof (CHAR16), L"%lu", Value);
        }
        return Text;
      }
      {
        STATIC UINTN  DbgLeft = 24;   /* field debug budget, then quiet */
        if (DbgLeft-- > 0) {
          SbLog ("[val] oneof0", (UINTN)S->Op,
                 ((UINTN)S->Width << 8) | (UINTN)S->NumFlags);
        }
      }
      break;

    case ST_CHECKBOX:
      return StrDup (Value != 0 ? L"Enabled" : L"Disabled");

    case ST_NUMERIC:
      FreePool (Text);
      Text = AllocateZeroPool (48 * sizeof (CHAR16));
      if (Text != NULL) {
        if ((S->NumFlags & EFI_IFR_DISPLAY) == EFI_IFR_DISPLAY_UINT_HEX) {
          /*
           * The vendor marked the field hex-displayed, so the stock
           * setup shows "0x1" and we used to copy that.  A plain
           * number is the readable text equivalent; keep the hex in
           * brackets for the fields that are genuinely bit masks.
           */
          UnicodeSPrint (Text, 48 * sizeof (CHAR16), L"%lu (0x%lX)",
                         Value, Value);
        } else if ((S->NumFlags & EFI_IFR_DISPLAY) == EFI_IFR_DISPLAY_INT_DEC) {
          UnicodeSPrint (Text, 48 * sizeof (CHAR16), L"%ld", (INT64)Value);
        } else {
          UnicodeSPrint (Text, 48 * sizeof (CHAR16), L"%lu", Value);
        }
      }
      return Text;

    default:
      break;
  }

  FreePool (Text);
  Text = AllocateZeroPool (48 * sizeof (CHAR16));
  if (Text != NULL) {
    UnicodeSPrint (Text, 48 * sizeof (CHAR16), L"0x%lX", Value);
  }
  return Text;
}

/* --------------------------------------------------------------- IFR parse */

STATIC
STMT *
NewStatement (
  IN FORMSET  *Fs,
  IN FORM     *Form,
  IN UINT8    Kind,
  IN UINT8    Op
  )
{
  STMT  *S;

  S = AllocateZeroPool (sizeof (STMT));
  if (S == NULL) {
    return NULL;
  }
  S->Kind = Kind;
  S->Op   = Op;

  if (Form != NULL) {
    /* Append so that the on-screen order matches the IFR order. */
    if (Form->Statements == NULL) {
      Form->Statements = S;
    } else {
      STMT  *T = Form->Statements;
      while (T->Next != NULL) {
        T = T->Next;
      }
      T->Next = S;
    }
  }
  (VOID)Fs;
  return S;
}

STATIC
FORM *
NewForm (
  IN FORMSET  *Fs,
  IN UINT16   Id,
  IN UINT16   TitleId
  )
{
  FORM  *F;

  F = AllocateZeroPool (sizeof (FORM));
  if (F == NULL) {
    return NULL;
  }
  F->Id    = Id;
  F->Title = ModelGetString (Fs, TitleId);
  F->Next  = Fs->Forms;
  Fs->Forms = F;
  return F;
}

STATIC
FORMSET *
NewFormset (
  IN UINT8          *Op,
  IN UINT8           Len,
  IN EFI_HII_HANDLE  Handle,
  IN CHAR8           *Lang
  )
{
  EFI_IFR_FORM_SET  *Fs = (EFI_IFR_FORM_SET *)Op;
  FORMSET           *N;
  UINTN             ClassCount;
  UINTN             Index;

  N = AllocateZeroPool (sizeof (FORMSET));
  if (N == NULL) {
    return NULL;
  }
  N->Guid   = Fs->Guid;
  N->Handle = Handle;
  AsciiStrnCpyS (N->Lang, sizeof (N->Lang), Lang, sizeof (N->Lang) - 1);

  if (Len >= 23) {
    ClassCount = Op[22] & 0x03;
    if (ClassCount > 4) {
      ClassCount = 4;
    }
    for (Index = 0; Index < ClassCount && (23 + (Index + 1) * sizeof (EFI_GUID)) <= Len; Index++) {
      CopyMem (&N->ClassGuids[Index], Op + 23 + Index * sizeof (EFI_GUID), sizeof (EFI_GUID));
      N->ClassGuidCount++;
    }
  }
  return N;
}

STATIC
VOID
FillQuestion (
  IN STMT               *S,
  IN EFI_IFR_QUESTION_HEADER  *Q,
  IN FORMSET            *Fs
  )
{
  S->QuestionId = Q->QuestionId;
  S->QFlags     = Q->Flags;
  S->VarStoreId = Q->VarStoreId;
  S->Offset     = Q->VarStoreInfo.VarOffset;
  S->Prompt     = ModelGetString (Fs, Q->Header.Prompt);
}

STATIC
VOID
AddOption (
  IN STMT     *S,
  IN FORMSET  *Fs,
  IN UINT8    *Op,
  IN UINT8    Len
  )
{
  OPTION  *List;
  OPTION  *O;
  UINTN   Width = S->Width != 0 ? S->Width : 1;

  List = AllocateZeroPool ((S->OptionCount + 1) * sizeof (OPTION));
  if (List == NULL) {
    return;
  }
  if (S->Options != NULL) {
    CopyMem (List, S->Options, S->OptionCount * sizeof (OPTION));
    FreePool (S->Options);
  }
  S->Options = List;
  O          = &List[S->OptionCount++];

  O->LabelId = (UINT16)(Op[2] | (Op[3] << 8));

  if (Len >= 7) {
    /* UEFI 2.x layout: Header(2) Option(2) Flags(1) Type(1) Value(1..8) */
    UINT8  ValType = Op[5];
    UINTN  Avail   = Len - 6;

    O->Flags = Op[4];
    switch (ValType) {
      case EFI_IFR_TYPE_NUM_SIZE_8:
        O->Value = Op[6];
        break;
      case EFI_IFR_TYPE_NUM_SIZE_16:
        O->Value = ReadLe (&Op[6], Avail >= 2 ? 2 : Avail);
        break;
      case EFI_IFR_TYPE_NUM_SIZE_32:
        O->Value = ReadLe (&Op[6], Avail >= 4 ? 4 : Avail);
        break;
      case EFI_IFR_TYPE_NUM_SIZE_64:
        O->Value = ReadLe (&Op[6], Avail >= 8 ? 8 : Avail);
        break;
      case EFI_IFR_TYPE_BOOLEAN:
        O->Value = Op[6] & 1;
        break;
      default:
        O->Value = ReadLe (&Op[6], Avail >= Width ? Width : Avail);
        break;
    }
  } else {
    O->Value = ReadLe (&Op[4], Width);
    O->Flags = Op[4 + Width];
  }

  O->Label = ModelGetString (Fs, O->LabelId);
}

/*
 * Insyde H2O legacy quirk: its ONE_OF_OPTION payload does not place the
 * Value at the offset the standard layouts use, so every option parses
 * as 0 (field log: stored 3, 5 options, values 0,0).  The vendor's
 * option sets are plain enums listed in order (0,1,2...), so when all
 * parsed values collide at 0 and there are at least three options,
 * number them in publication order.
 */
STATIC
VOID
FixupOneof (
  IN STMT  *S
  )
{
  UINTN  I;
  UINTN  W = S->Width != 0 ? S->Width : 1;
  UINT64 Mask = W >= 8 ? 0 : (((UINT64)1 << (8 * W)) - 1);

  if ((S->Kind != ST_ONEOF) || (S->OptionCount < 3)) {
    return;
  }
  for (I = 0; I < S->OptionCount; I++) {
    if ((S->Options[I].Value & Mask) != 0) {
      return;
    }
  }
  for (I = 1; I < S->OptionCount; I++) {
    S->Options[I].Value = I;
  }
}

STATIC
VOID
ParseIfr (
  IN     UINT8           *Base,
  IN     UINTN           Size,
  IN     EFI_HII_HANDLE  Handle,
  IN     CHAR8           *Lang,
  IN OUT FORMSET         **ListHead
  )
{
  UINT8              *Ptr, *End;
  SCOPE              Stack[MAX_SCOPE_DEPTH];
  INTN               Depth = 0;
  FORMSET            *Fs = NULL;
  FORM               *Form = NULL;
  STMT               *St = NULL;
  UINTN              TraceLeft = 0;   /* field debugging budget */

  Ptr = Base;
  End = Base + Size;

  while (Ptr + 2 <= End) {
    EFI_IFR_OP_HEADER  *H   = (EFI_IFR_OP_HEADER *)Ptr;
    UINT8              Len  = (UINT8)(Ptr[1] & 0x7F);
    BOOLEAN            Scope = (Ptr[1] & 0x80) != 0;

    /*
     * Raw opcode trace, logged to the USB stick: the hardware's Insyde H2O
     * packs reference subforms our parser has never seen, so capture the
     * exact byte stream and analyze it offline.
     */
    if (TraceLeft > 0) {
      TraceLeft--;
      SbLog6 ("[model] op", (UINTN)(Ptr - Base), H->OpCode, Len,
              Scope, (UINTN)Ptr[2] | ((UINTN)Ptr[3] << 8) |
              ((UINTN)Ptr[4] << 16) | ((UINTN)Ptr[5] << 24),
              (UINTN)Ptr[6] | ((UINTN)Ptr[7] << 8) | ((UINTN)Ptr[8] << 16) |
              ((UINTN)Ptr[9] << 24));
    }

    if (Len < 2) {
      break;
    }

    switch (H->OpCode) {
      case EFI_IFR_FORM_SET_OP:
        Fs = NewFormset (Ptr, Len, Handle, Lang);
        if (Fs != NULL) {
          Fs->Title = ModelGetString (Fs, ((EFI_IFR_FORM_SET *)Ptr)->FormSetTitle);
          Fs->Next  = *ListHead;
          *ListHead = Fs;
        }
        Form = NULL;
        St   = NULL;
        break;

      case EFI_IFR_FORM_OP:
        if (Fs != NULL) {
          Form = NewForm (Fs,
                          (UINT16)(Ptr[2] | (Ptr[3] << 8)),
                          (UINT16)(Ptr[4] | (Ptr[5] << 8)));
        }
        St = NULL;
        break;

      case EFI_IFR_VARSTORE_OP:
      case EFI_IFR_VARSTORE_EFI_OP:
      case EFI_IFR_VARSTORE_NAME_VALUE_OP:
        if (Fs != NULL) {
          AddVarstore (Fs, Ptr, H->OpCode, Len);
        }
        break;

      case EFI_IFR_ONE_OF_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_ONEOF, H->OpCode);
          if (St != NULL) {
            FillQuestion (St, (EFI_IFR_QUESTION_HEADER *)(Ptr + 2), Fs);
            St->Width    = (UINT8)WidthOfFlags (Ptr[2 + sizeof (EFI_IFR_QUESTION_HEADER)]);
            St->NumFlags = Ptr[2 + sizeof (EFI_IFR_QUESTION_HEADER)];
          }
        }
        break;

      case EFI_IFR_NUMERIC_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_NUMERIC, H->OpCode);
          if (St != NULL) {
            EFI_IFR_NUMERIC  *N = (EFI_IFR_NUMERIC *)Ptr;
            FillQuestion (St, &N->Question, Fs);
            St->Width    = (UINT8)WidthOfFlags (N->Flags);
            St->NumFlags = N->Flags;
            St->Min      = N->data.u64.MinValue;
            St->Max      = N->data.u64.MaxValue;
            St->Step     = N->data.u64.Step;
          }
        }
        break;

      case EFI_IFR_CHECKBOX_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_CHECKBOX, H->OpCode);
          if (St != NULL) {
            EFI_IFR_CHECKBOX  *C = (EFI_IFR_CHECKBOX *)Ptr;
            FillQuestion (St, &C->Question, Fs);
            St->Width = 1;
            if ((C->Flags & 0x80) != 0) {   /* EFI_IFR_CHECKBOX_MASK */
              St->Bit = TRUE;
            }
          }
        }
        break;

      case EFI_IFR_REF_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_REF, H->OpCode);
          if (St != NULL) {
            EFI_IFR_REF  *R = (EFI_IFR_REF *)Ptr;
            FillQuestion (St, &R->Question, Fs);
            St->RefFormId = R->FormId;
          }
        }
        break;

      case EFI_IFR_ACTION_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_ACTION, H->OpCode);
          if (St != NULL) {
            EFI_IFR_ACTION  *A = (EFI_IFR_ACTION *)Ptr;
            FillQuestion (St, &A->Question, Fs);
          }
        }
        break;

      case EFI_IFR_PASSWORD_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_PASSWORD, H->OpCode);
          if (St != NULL) {
            FillQuestion (St, (EFI_IFR_QUESTION_HEADER *)(Ptr + 2), Fs);
          }
        }
        break;

      case EFI_IFR_ONE_OF_OPTION_OP:
        if ((St != NULL) && (St->Kind == ST_ONEOF)) {
          AddOption (St, Fs, Ptr, Len);
        }
        break;

      case EFI_IFR_SUBTITLE_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_SUBTITLE, H->OpCode);
          if (St != NULL) {
            St->Prompt = ModelGetString (Fs, ((EFI_IFR_SUBTITLE *)Ptr)->Statement.Prompt);
          }
        }
        break;

      case EFI_IFR_TEXT_OP:
        if (Form != NULL) {
          St = NewStatement (Fs, Form, ST_TEXT, H->OpCode);
          if (St != NULL) {
            EFI_IFR_TEXT  *T = (EFI_IFR_TEXT *)Ptr;
            St->Prompt = ModelGetString (Fs, T->Statement.Prompt);
            St->Text2  = ModelGetString (Fs, T->TextTwo);
          }
        }
        break;

      default:
        if ((Depth == 0) || (Stack[Depth - 1].Kind != SCOPE_QUESTION)) {
          St = NULL;
        }
        break;
    }

    if (Scope && (Depth < MAX_SCOPE_DEPTH)) {
      Stack[Depth].Kind = (H->OpCode == EFI_IFR_FORM_SET_OP) ? SCOPE_FORMSET :
                          (H->OpCode == EFI_IFR_FORM_OP) ? SCOPE_FORM :
                          (St != NULL) ? SCOPE_QUESTION : SCOPE_OTHER;
      Stack[Depth].Fs   = Fs;
      Stack[Depth].Form = Form;
      Stack[Depth].Stmt = St;
      Depth++;
    }

    if (H->OpCode == EFI_IFR_END_OP && (Depth > 0)) {
      Depth--;
      switch (Stack[Depth].Kind) {
        case SCOPE_QUESTION: FixupOneof (Stack[Depth].Stmt);
                             St   = NULL;
                             break;
        case SCOPE_FORM:     Form = Stack[Depth].Fs ? Stack[Depth].Form : NULL;
                             St   = NULL;
                             break;
        case SCOPE_FORMSET:  Fs   = Stack[Depth].Fs;
                             Form = NULL;
                             St   = NULL;
                             break;
        default:             St = Stack[Depth].Stmt; break;
      }
    }

    Ptr += Len;
  }
}

/* ---------------------------------------------------------------- scanning */

STATIC
VOID
ParsePackageList (
  IN     EFI_HII_HANDLE  Handle,
  IN     UINT8           *Buf,
  IN     UINTN           Size,
  IN OUT FORMSET         **ListHead
  )
{
  EFI_HII_PACKAGE_LIST_HEADER  *List;
  EFI_HII_PACKAGE_HEADER       *Pkg;
  UINTN                        Off;
  CHAR8                        Lang[8];

  List = (EFI_HII_PACKAGE_LIST_HEADER *)Buf;
  Off  = sizeof (EFI_HII_PACKAGE_LIST_HEADER);

  /* Language of the string package is needed to resolve prompts. */
  AsciiStrCpyS (Lang, sizeof (Lang), "en-us");
  while (Off + sizeof (EFI_HII_PACKAGE_HEADER) <= Size && Off < List->PackageLength) {
    Pkg = (EFI_HII_PACKAGE_HEADER *)(Buf + Off);
    if (Pkg->Length < sizeof (EFI_HII_PACKAGE_HEADER)) {
      break;
    }
    if (Pkg->Type == EFI_HII_PACKAGE_STRINGS) {
      EFI_HII_STRING_PACKAGE_HDR  *S = (EFI_HII_STRING_PACKAGE_HDR *)(Buf + Off);
      AsciiStrnCpyS (Lang, sizeof (Lang), (CHAR8 *)S->Language, sizeof (Lang) - 1);
      break;
    }
    Off += Pkg->Length;
  }

  Off = sizeof (EFI_HII_PACKAGE_LIST_HEADER);
  while (Off + sizeof (EFI_HII_PACKAGE_HEADER) <= Size && Off < List->PackageLength) {
    Pkg = (EFI_HII_PACKAGE_HEADER *)(Buf + Off);
    if (Pkg->Length < sizeof (EFI_HII_PACKAGE_HEADER)) {
      break;
    }
    if (Pkg->Type == EFI_HII_PACKAGE_FORMS) {
      ParseIfr (Buf + Off + sizeof (EFI_HII_PACKAGE_HEADER),
                Pkg->Length - sizeof (EFI_HII_PACKAGE_HEADER), Handle, Lang, ListHead);
    }
    Off += Pkg->Length;
  }
}

FORMSET *
ModelFormsets (
  VOID
  )
{
  return mFormsets;
}

FORM *
ModelForm (
  IN FORMSET  *Fs,
  IN UINT16   FormId
  )
{
  FORM  *F;

  if (Fs == NULL) {
    return NULL;
  }
  for (F = Fs->Forms; F != NULL; F = F->Next) {
    if (F->Id == FormId) {
      return F;
    }
  }
  return NULL;
}

STATIC
VOID
FreeForms (
  IN FORM  *Forms
  )
{
  FORM  *F;
  FORM  *NextF;

  for (F = Forms; F != NULL; F = NextF) {
    STMT  *S;
    STMT  *NextS;

    NextF = F->Next;
    for (S = F->Statements; S != NULL; S = NextS) {
      NextS = S->Next;
      if (S->Prompt != NULL) {
        FreePool (S->Prompt);
      }
      if (S->Text2 != NULL) {
        FreePool (S->Text2);
      }
      if (S->Options != NULL) {
        UINTN  I;
        for (I = 0; I < S->OptionCount; I++) {
          if (S->Options[I].Label != NULL) {
            FreePool (S->Options[I].Label);
          }
        }
        FreePool (S->Options);
      }
      FreePool (S);
    }
    if (F->Title != NULL) {
      FreePool (F->Title);
    }
    FreePool (F);
  }
}

EFI_STATUS
ModelReloadFormset (
  IN FORMSET  *Fs
  )
{
  EFI_STATUS  Status;
  UINT8       *Cfg     = NULL;
  UINTN       CfgSize  = 0;
  FORMSET     *Tmp     = NULL;
  FORMSET     *T;
  FORMSET     *NextT;
  BOOLEAN     Replaced = FALSE;

  if ((Fs == NULL) || (Fs->Handle == NULL) || (mHiiDb == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = mHiiDb->ExportPackageLists (mHiiDb, Fs->Handle, &CfgSize, NULL);
  if (CfgSize == 0) {
    return EFI_NOT_FOUND;
  }
  Cfg = AllocatePool (CfgSize);
  if (Cfg == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = mHiiDb->ExportPackageLists (
                     mHiiDb,
                     Fs->Handle,
                     &CfgSize,
                     (EFI_HII_PACKAGE_LIST_HEADER *)Cfg
                     );
  if (!EFI_ERROR (Status) && (CfgSize > 0)) {
    ParsePackageList (Fs->Handle, Cfg, CfgSize, &Tmp);
  }
  FreePool (Cfg);

  for (T = Tmp; T != NULL; T = NextT) {
    VARSTORE  *V;
    VARSTORE  *NextV;

    NextT = T->Next;
    if (!Replaced && CompareGuid (&T->Guid, &Fs->Guid)) {
      FORM  *OldForms = Fs->Forms;
      Fs->Forms = T->Forms;
      T->Forms  = NULL;
      FreeForms (OldForms);
      Replaced = TRUE;
    }
    FreeForms (T->Forms);
    if (T->Title != NULL) {
      FreePool (T->Title);
    }
    for (V = T->Vars; V != NULL; V = NextV) {
      NextV = V->Next;
      if (V->Data != NULL) {
        FreePool (V->Data);
      }
      FreePool (V);
    }
    FreePool (T);
  }

  return Replaced ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
ModelInit (
  VOID
  )
{
  EFI_STATUS      Status;
  EFI_HII_HANDLE  *Handles;
  UINTN           Count, Index;
  UINT8           *Cfg;
  UINTN           CfgSize;

  Status = gBS->LocateProtocol (&gEfiHiiDatabaseProtocolGuid, NULL, (VOID **)&mHiiDb);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->LocateProtocol (&gEfiHiiStringProtocolGuid, NULL, (VOID **)&mHiiStr);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  gBS->LocateProtocol (&gEfiHiiConfigRoutingProtocolGuid, NULL, (VOID **)&mCfg);

  /* Two-call enumeration: first the size, then the handle array. */
  Count   = 0;
  CfgSize = 0;
  Status  = mHiiDb->ListPackageLists (
                      mHiiDb, EFI_HII_PACKAGE_TYPE_ALL, NULL, &CfgSize, NULL);
  if (EFI_ERROR (Status) && (Status != EFI_OUT_OF_RESOURCES) && (CfgSize == 0)) {
    return Status;
  }
  Handles = AllocatePool (CfgSize);
  if (Handles == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = mHiiDb->ListPackageLists (
                    mHiiDb, EFI_HII_PACKAGE_TYPE_ALL, NULL, &CfgSize, Handles);
  if (EFI_ERROR (Status)) {
    FreePool (Handles);
    return Status;
  }
  Count = CfgSize / sizeof (EFI_HII_HANDLE);

  for (Index = 0; Index < Count; Index++) {
    Cfg     = NULL;
    CfgSize = 0;
    Status  = mHiiDb->ExportPackageLists (
                        mHiiDb, Handles[Index], &CfgSize, NULL);
    if (CfgSize == 0) {
      continue;
    }
    Cfg = AllocatePool (CfgSize);
    if (Cfg == NULL) {
      break;
    }
    Status = mHiiDb->ExportPackageLists (
                       mHiiDb, Handles[Index], &CfgSize,
                       (EFI_HII_PACKAGE_LIST_HEADER *)Cfg);
    if (!EFI_ERROR (Status) && (CfgSize > 0)) {
      ParsePackageList (Handles[Index], Cfg, CfgSize, &mFormsets);
    }
    FreePool (Cfg);
  }
  FreePool (Handles);

  /*
   * Field diagnostic: one line per formset (guid + sizes) and per form, so a
   * log taken off the boot stick shows exactly which H2O form parsed to which
   * row count.  Cheap: only runs once at startup.
   */
  {
    FORMSET   *Fs;
    FORM      *F;
    VARSTORE  *V;
    STMT      *S;
    UINTN     Fc, Sc;
    CHAR8     Line[160];

    for (Fs = mFormsets; Fs != NULL; Fs = Fs->Next) {
      Fc = 0;
      Sc = 0;
      for (F = Fs->Forms; F != NULL; F = F->Next) {
        Fc++;
        for (S = F->Statements; S != NULL; S = S->Next) {
          Sc++;
        }
      }
      AsciiSPrint (Line, sizeof (Line), "[model] fs %g forms %u stmts %u vars %u\r\n",
                   &Fs->Guid, (UINT32)Fc, (UINT32)Sc, (UINT32)Fs->VarCount);
      SbLogStr (Line);
      for (F = Fs->Forms; F != NULL; F = F->Next) {
        Sc = 0;
        for (S = F->Statements; S != NULL; S = S->Next) {
          Sc++;
        }
        SbLog ("[model] form", F->Id, Sc);
      }
      for (V = Fs->Vars; V != NULL; V = V->Next) {
        AsciiSPrint (Line, sizeof (Line), "[model] var %g id %u size %u type %u present %d\r\n",
                     &V->Guid, (UINT32)V->Id, (UINT32)V->Size, (UINT32)V->Type,
                     (UINT32)V->Present);
        SbLogStr (Line);
      }
    }
  }

  return mFormsets != NULL ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
ModelRunSetup (
  IN SB_SETUP_RUNNER  Runner
  )
{
  VOID                   *Existing     = NULL;
  SB_FORM_BROWSER2       *Fb2          = NULL;
  SB_SEND_FORM           OrigSendForm  = NULL;
  SB_BROWSER_CALLBACK    OrigCallback  = NULL;
  EFI_TEXT_CLEAR_SCREEN  OrigClear     = NULL;
  EFI_STATUS             Status;

  mActiveRunner = Runner;
  mRunnerRan    = FALSE;

  if (!EFI_ERROR (gBS->LocateProtocol (&mSetupAppGuid, NULL, &Existing))) {
    if (Runner != NULL) {
      Runner ();
    }
    mActiveRunner = NULL;
    return EFI_SUCCESS;
  }

  mSetupAppPhase = AllocateZeroPool (sizeof (UINT32));
  if (mSetupAppPhase == NULL) {
    if (Runner != NULL) {
      Runner ();
    }
    mActiveRunner = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  if (!EFI_ERROR (gBS->LocateProtocol (&mFormBrowser2Guid, NULL, (VOID **)&Fb2)) &&
      (Fb2 != NULL)) {
    OrigSendForm         = Fb2->SendForm;
    OrigCallback         = Fb2->BrowserCallback;
    Fb2->SendForm        = HookSendForm;
    Fb2->BrowserCallback = HookBrowserCallback;
  }

  if ((gST->ConOut != NULL) && (gST->ConOut->ClearScreen != NULL)) {
    OrigClear                = gST->ConOut->ClearScreen;
    gST->ConOut->ClearScreen = NoopClearScreen;
  }

  mOrigRaiseTpl = gBS->RaiseTPL;
  gBS->RaiseTPL = SafeRaiseTpl;

  *mSetupAppPhase = 0;
  Status = gBS->InstallProtocolInterface (
                  &mSetupAppHandle,
                  &mSetupAppGuid,
                  EFI_NATIVE_INTERFACE,
                  mSetupAppPhase
                  );

  if (!mRunnerRan && (Runner != NULL)) {
    Runner ();
  }

  gBS->RaiseTPL = mOrigRaiseTpl;
  if ((gST->ConOut != NULL) && (OrigClear != NULL)) {
    gST->ConOut->ClearScreen = OrigClear;
  }
  if (Fb2 != NULL) {
    if (OrigSendForm != NULL) {
      Fb2->SendForm = OrigSendForm;
    }
    if (OrigCallback != NULL) {
      Fb2->BrowserCallback = OrigCallback;
    }
  }

  if (!mRunnerRan && (mSetupAppHandle != NULL) && (mSetupAppPhase != NULL)) {
    *mSetupAppPhase = 1;
    gBS->ReinstallProtocolInterface (
           mSetupAppHandle,
           &mSetupAppGuid,
           mSetupAppPhase,
           mSetupAppPhase
           );
  }

  if (mSetupAppHandle != NULL) {
    gBS->UninstallProtocolInterface (mSetupAppHandle, &mSetupAppGuid, mSetupAppPhase);
    mSetupAppHandle = NULL;
  }
  if (mSetupAppPhase != NULL) {
    FreePool (mSetupAppPhase);
    mSetupAppPhase = NULL;
  }

  mActiveRunner = NULL;
  return Status;
}

VOID
ModelExit (
  VOID
  )
{


  /*
   * The model lives in boot services memory for the whole life of the
   * application; exit boot services frees it.  Keeping the tree avoids
   * dangling pointers in the UI while the last frame is presented.
   */
  mFormsets = NULL;
}
