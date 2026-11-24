/*
 * H2oSetup - custom graphical UEFI setup UI (prototype).
 *
 * Shared types and module interfaces.  Everything is built on stock protocols
 * only (GOP, HII database/string/font, pointer protocols), so the module can
 * run as a plain UEFI application in QEMU and later be injected into the
 * InsydeH2O firmware image without touching vendor binaries.
 */

#ifndef _H2O_SETUP_H_
#define _H2O_SETUP_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiDatabase.h>
#include <Protocol/HiiImage.h>
#include <Protocol/HiiString.h>
#include <Protocol/HiiFont.h>
#include <Protocol/HiiConfigRouting.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/DevicePath.h>

/*
 * Tiny serial tracer: EDK2's AsciiSPrint is not pulled in by this module,
 * so log lines are assembled from fixed text plus decimal values.
 */
STATIC inline VOID
SbLog (
  IN CONST CHAR8  *Tag,
  IN UINTN        V1,
  IN UINTN        V2
  )
{
  CHAR8   Msg[96];
  UINTN   Pos = 0;
  UINTN   N;
  CHAR8   Num[24];

  while ((Pos < sizeof (Msg) - 8) && (Tag[Pos] != 0)) {
    Msg[Pos] = Tag[Pos];
    Pos++;
  }
  Msg[Pos++] = ' ';
  AsciiSPrint (Num, sizeof (Num), "%u 0x%x\r\n", (UINT32)V1, (UINT32)V2);
  for (N = 0; (N < sizeof (Num)) && (Num[N] != 0); N++) {
    Msg[Pos++] = Num[N];
  }
  SerialPortWrite ((UINT8 *)Msg, Pos);
}

/* ---------------------------------------------------------------- palette */

#define COL_BG        0x00101826u   /* window background        */
#define COL_BG_ALT    0x00162032u   /* alternating row          */
#define COL_SEL       0x002a5f9eu   /* selection highlight      */
#define COL_HDR       0x000a1018u   /* header / footer bar      */
#define COL_ACCENT    0x004ab3f4u   /* accent / cursor          */
#define COL_TEXT      0x00e8eef6u   /* normal text              */
#define COL_TEXT_DIM  0x008a97a8u   /* secondary text           */
#define COL_TEXT_HI   0x00ffffffffu /* text on selection        */
#define COL_VALUE     0x00ffd166u   /* current value            */
#define COL_LINE      0x002b3a4eu   /* separators               */

/* ------------------------------------------------------------- IFR model */

#define VS_CLASSIC   EFI_IFR_VARSTORE_OP        /* 0x24 GUID varstore  */
#define VS_NAMEVALUE EFI_IFR_VARSTORE_NAME_VALUE_OP
#define VS_EFI       EFI_IFR_VARSTORE_EFI_OP    /* 0x26 EFI variable   */

#define ST_NORMAL    0
#define ST_ONEOF     1
#define ST_CHECKBOX  2
#define ST_NUMERIC   3
#define ST_REF       4
#define ST_TEXT      5
#define ST_SUBTITLE  6
#define ST_ACTION    7
#define ST_LABEL     8
#define ST_PASSWORD  9

/* One runtime-visible row of the current form. */
typedef struct STMT {
  UINT8          Kind;
  UINT8          Op;
  UINT16         QFlags;              /* EFI_IFR_QUESTION_HEADER.Flags     */
  UINT16         VarStoreId;
  UINT16         Offset;              /* byte offset, bit offset if Bit    */
  UINT8          Width;               /* value width in bytes              */
  BOOLEAN        Bit;                 /* checkbox flag is a bit index      */
  CHAR16        *Prompt;
  CHAR16        *Text2;               /* EFI_IFR_TEXT TextTwo              */
  struct OPTION *Options;
  UINTN          OptionCount;
  UINT16         RefFormId;
  INT64          Min, Max, Step;
  UINT8          NumFlags;
  struct STMT   *Next;
} STMT;

typedef struct OPTION {
  UINT64          Value;
  UINT16          LabelId;
  CHAR16         *Label;
  UINT8           Flags;
} OPTION;

typedef struct FORM {
  UINT16          Id;
  CHAR16         *Title;
  STMT           *Statements;
  struct FORM    *Next;
} FORM;

/* Backing storage of a formset, cached in memory. */
typedef struct VARSTORE {
  UINT8           Type;               /* VS_CLASSIC / VS_NAMEVALUE / VS_EFI */
  EFI_GUID        Guid;               /* varstore GUID, variable GUID        */
  CHAR16          Name[72];           /* EFI variable name, ASCII for classic */
  UINT16          Id;
  UINT16          Size;
  UINT8          *Data;
  UINTN           DataSize;
  UINT32          Attributes;
  BOOLEAN         Present;            /* variable exists in NVram           */
  BOOLEAN         ReadOnly;           /* cannot be accessed directly        */
  BOOLEAN         Dirty;
  struct VARSTORE *Next;
} VARSTORE;

typedef struct FORMSET {
  EFI_GUID        Guid;
  CHAR16         *Title;
  EFI_HII_HANDLE  Handle;
  CHAR8           Lang[8];            /* language of the string package     */
  VARSTORE       *Vars;
  UINTN           VarCount;
  FORM           *Forms;
  struct FORMSET *Next;
} FORMSET;

/* ------------------------------------------------------------------ gfx */

EFI_STATUS GfxInit (VOID);
VOID       GfxExit (VOID);
UINT32     GfxWidth (VOID);
UINT32     GfxHeight (VOID);
UINT32     GfxScale (VOID);            /* integer font scale, 1..3          */
UINT32     GfxCellW (VOID);
UINT32     GfxCellH (VOID);
UINT32    *GfxBackBuffer (VOID);
VOID       GfxClear (UINT32 Color);
VOID       GfxFill (INT32 X, INT32 Y, INT32 W, INT32 H, UINT32 Color);
VOID       GfxFrame (INT32 X, INT32 Y, INT32 W, INT32 H, UINT32 Color);
INT32      GfxText (INT32 X, INT32 Y, CONST CHAR16 *S, UINT32 Color, INT32 MaxW);
INT32      GfxTextWidth (CONST CHAR16 *S);
VOID       GfxSetCursor (INT32 X, INT32 Y, BOOLEAN Visible);
EFI_STATUS GfxPresent (VOID);

/* ---------------------------------------------------------------- input */

#define EV_NONE     0
#define EV_KEY      1
#define EV_MOVE     2
#define EV_LCLICK   3
#define EV_RCLICK   4
#define EV_WHEEL    5

typedef struct {
  UINT32        Type;
  EFI_INPUT_KEY Key;
  INT32         X, Y;
  INT32         Wheel;
} INPUT_EVENT;

EFI_STATUS InputInit (VOID);
VOID       InputExit (VOID);
UINT32     InputPoll (INPUT_EVENT *Event);   /* EV_NONE when idle */
VOID       InputGetCursor (INT32 *X, INT32 *Y);
VOID       InputSetCursor (INT32 X, INT32 Y);

/* ---------------------------------------------------------------- model */

EFI_STATUS ModelInit (VOID);
VOID       ModelExit (VOID);
FORMSET   *ModelFormsets (VOID);
FORM      *ModelForm (FORMSET *Fs, UINT16 FormId);
VARSTORE  *ModelVarstore (FORMSET *Fs, UINT16 VarStoreId);
EFI_STATUS ModelLoad (FORMSET *Fs);
EFI_STATUS ModelFlush (FORMSET *Fs);
UINT64     ModelRead (STMT *S, VARSTORE *V, BOOLEAN *Valid);
EFI_STATUS ModelWrite (STMT *S, VARSTORE *V, UINT64 Value);
CHAR16    *ModelValueText (STMT *S, VARSTORE *V);   /* allocated, caller frees */
CHAR16    *ModelGetString (FORMSET *Fs, EFI_STRING_ID Id);

/* ------------------------------------------------------------------- ui */

EFI_STATUS UiInit (FORMSET *First);
VOID       UiEvent (INPUT_EVENT *Event);
VOID       UiDraw (VOID);
BOOLEAN    UiQuitRequested (VOID);

#endif /* _H2O_SETUP_H_ */
