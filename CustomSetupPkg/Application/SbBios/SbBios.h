/*
 * SbBios - one graphical front-end for the Superbox firmware: a Saku
 * Overclock styled boot menu and a custom setup viewer, in a single
 * application.
 *
 * Everything is built on stock protocols only (GOP, HII database/string,
 * pointer protocols, RT time) and ships its own baked bitmap fonts, so the
 * look is identical in QEMU and on the box, and no vendor binary is touched.
 * The boot screen and the setup screen are plain state inside the event loop:
 * switching between them costs one redraw, not a 10-second LoadImage from
 * USB.
 */

#ifndef _SB_BIOS_H_
#define _SB_BIOS_H_

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
#include <Protocol/HiiConfigRouting.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/PciIo.h>
#include "fontdata.h"

/* ------------------------------------------------------------------ palette
 * Saku Overclock: grey paper, orange accent, ink text.  Per the design,
 * every text on the paper is 0x000000; only text sitting on dark cards
 * (inactive buttons, tab chips) is white.
 */

#define COL_BG        0x00d7d7d7u   /* paper                          */
#define COL_INK       0x00000000u   /* all paper text                 */
#define COL_ORANGE    0x00e05e12u   /* active accent                  */
#define COL_CARD      0x00212121u   /* inactive buttons / tab chips   */
#define COL_WHITE     0x00ffffffu   /* text on dark / orange          */
#define COL_LINE      0x00000000u   /* sketch strokes                 */

/* ----------------------------------------------------------------- field log
 * Tracing goes to the serial port (QEMU) and to sbxlog.txt on every FAT
 * volume, because the target box has no serial console.
 */

EFI_STATUS SbLogInit (VOID);
VOID       SbLogStr (IN CONST CHAR8  *Line);
VOID       SbLog (IN CONST CHAR8  *Tag, IN UINTN V1, IN UINTN V2);
VOID       SbLog6 (IN CONST CHAR8  *Tag, IN UINTN V1, IN UINTN V2,
                   IN UINTN V3, IN UINTN V4, IN UINTN V5, IN UINTN V6);

/* --------------------------------------------------------------------- gfx */

EFI_STATUS GfxInit (VOID);
VOID       GfxExit (VOID);
UINT32     GfxWidth (VOID);
UINT32     GfxHeight (VOID);
INT32      GfxUi (VOID);              /* layout scale: 1 small, 2 large panes */
UINT32    *GfxBackBuffer (VOID);

VOID       GfxClear (UINT32 Color);
VOID       GfxFill (INT32 X, INT32 Y, INT32 W, INT32 H, UINT32 Color);
VOID       GfxFrame (INT32 X, INT32 Y, INT32 W, INT32 H, INT32 T, UINT32 Color);

/* baked bitmap fonts: text is drawn on its baseline, no blending (fast) */
INT32      GfxText (INT32 X, INT32 BaseY, IN CONST SB_GLYPH *Tab, IN UINTN Count,
                    IN CONST UINT8 *Bits, IN CONST CHAR16 *S, IN UINT32 Color,
                    IN INT32 Scale);
INT32      GfxTextW (IN CONST SB_GLYPH *Tab, IN UINTN Count,
                     IN CONST CHAR16 *S, IN INT32 Scale);
INT32      GfxAscent (IN CONST SB_GLYPH *Tab, IN UINTN Count);
#define    FONT(t)   t##_Tab, sizeof (t##_Tab) / sizeof (t##_Tab[0]), t##_Bits
#define    FONTW(t)  t##_Tab, sizeof (t##_Tab) / sizeof (t##_Tab[0])

/* vector shapes (fixed point, no FPU in UEFI apps) */
VOID       GfxPolyFill (IN CONST INT32 (*Pts)[2], IN UINTN Count, IN UINT32 C);
VOID       GfxCircle (INT32 Cx, INT32 Cy, INT32 R, UINT32 Color, BOOLEAN Fill);
VOID       GfxGear (INT32 Cx, INT32 Cy, INT32 R, UINT32 Ink, UINT32 Hole);

/* hand-drawn strokes: deterministic wobble, same seed -> same line */
VOID       GfxSketchH (INT32 X, INT32 Y, INT32 Len, UINT32 Seed, UINT32 Color);
VOID       GfxSketchV (INT32 X, INT32 Y, INT32 Len, UINT32 Seed, UINT32 Color);
VOID       GfxSketchFrame (INT32 X, INT32 Y, INT32 W, INT32 H, UINT32 Seed,
                           UINT32 Color);
VOID       GfxCorners (INT32 X, INT32 Y, INT32 W, INT32 H, INT32 Arm,
                       INT32 T, UINT32 Color);
#define    PC_TL  0x1u
#define    PC_TR  0x2u
#define    PC_BL  0x4u
#define    PC_BR  0x8u
#define    PC_ALL (PC_TL | PC_TR | PC_BL | PC_BR)
VOID       GfxPencilCorners (INT32 X, INT32 Y, INT32 W, INT32 H, INT32 Arm,
                             INT32 T, UINT32 Seed, UINT32 Corners,
                             UINT32 Color);

VOID       GfxSetCursor (INT32 X, INT32 Y, BOOLEAN Visible);
EFI_STATUS GfxPresent (VOID);
VOID       GfxReapplyMode (VOID);
EFI_STATUS GfxStepMode (INTN Direction);
EFI_STATUS GfxScreenshot (CHAR16 *Result, UINTN ResultChars);  /* BMP to FAT */

/* ------------------------------------------------------------------- input */

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

/* ------------------------------------------------------------------- model */

#define VS_CLASSIC   EFI_IFR_VARSTORE_OP
#define VS_NAMEVALUE EFI_IFR_VARSTORE_NAME_VALUE_OP
#define VS_EFI       EFI_IFR_VARSTORE_EFI_OP

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

typedef struct STMT {
  UINT8          Kind;
  UINT8          Op;
  UINT16         QuestionId;
  UINT16         QFlags;
  UINT16         VarStoreId;
  UINT16         Offset;
  UINT8          Width;
  BOOLEAN        Bit;
  CHAR16        *Prompt;
  CHAR16        *Text2;
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

typedef struct VARSTORE {
  UINT8           Type;
  EFI_GUID        Guid;
  CHAR16          Name[72];
  UINT16          Id;
  UINT16          Size;
  UINT8          *Data;
  UINTN           DataSize;
  UINT32          Attributes;
  BOOLEAN         Present;
  BOOLEAN         ReadOnly;
  BOOLEAN         Dirty;
  struct VARSTORE *Next;
} VARSTORE;

typedef struct FORMSET {
  EFI_GUID        Guid;
  EFI_GUID        ClassGuids[4];
  UINTN           ClassGuidCount;
  CHAR16         *Title;
  EFI_HII_HANDLE  Handle;
  CHAR8           Lang[8];
  VARSTORE       *Vars;
  UINTN           VarCount;
  FORM           *Forms;
  struct FORMSET *Next;
} FORMSET;

typedef BOOLEAN (*SB_SETUP_RUNNER) (VOID);
EFI_STATUS ModelInit (VOID);
EFI_STATUS ModelRunSetup (IN SB_SETUP_RUNNER Runner);
VOID       ModelNotifyChanged (IN FORMSET *Fs, IN STMT *S, IN UINT64 Value);
VOID       ModelExit (VOID);
FORMSET   *ModelFormsets (VOID);
FORM      *ModelForm (FORMSET *Fs, UINT16 FormId);
VARSTORE  *ModelVarstore (FORMSET *Fs, UINT16 VarStoreId);
EFI_STATUS ModelLoad (FORMSET *Fs);
EFI_STATUS ModelFlush (FORMSET *Fs);
UINT64     ModelRead (STMT *S, VARSTORE *V, BOOLEAN *Valid);
UINT64     NormValue (STMT *S, UINT64 Value);
EFI_STATUS ModelWrite (STMT *S, VARSTORE *V, UINT64 Value);
CHAR16    *ModelValueText (STMT *S, VARSTORE *V);
CHAR16    *ModelGetString (FORMSET *Fs, EFI_STRING_ID Id);

/* ------------------------------------------------------------------ hwinfo */

typedef struct {
  CHAR16  Brand[64];        /* CPUID brand string, ASCII part           */
  CHAR16  Time[16];         /* "HH:MM"                                  */
  CHAR16  Date[16];         /* "MM/DD/YYYY"                             */
  CHAR16  Weekday[16];      /* English weekday name                     */
  CHAR16  CpuFreq[16];      /* "3.6 GHz"  (1 decimal)                   */
  CHAR16  CpuTemp[16];      /* "44C"                                    */
  CHAR16  CpuBclk[16];      /* "100 MHz"                                */
  CHAR16  CpuVolt[16];      /* "1.25V"                                  */
  CHAR16  MemFreq[16];      /* "2133 MHz"                               */
  CHAR16  MemVolt[16];      /* "1.2V"                                   */
  CHAR16  GpuFreq[16];      /* "960 MHz"                                */
  CHAR16  GpuVolt[16];      /* "1.0V"                                   */
  BOOLEAN CpuReal;          /* TRUE: MSR-derived values, FALSE: stubs   */
  BOOLEAN TempReal;         /* TRUE: SMU temperature read OK            */
} HW_INFO;

VOID HwUpdate (HW_INFO *Hw);   /* refresh time (per second) + sensors */

/* ------------------------------------------------------------------ screens */

/* boot menu (screen 0) */
VOID BootEnter (VOID);                       /* collect entries, draw */
VOID BootEvent (INPUT_EVENT *Event);         /* returns, may switch  */
VOID BootDraw (VOID);
BOOLEAN BootWantSetup (VOID);                /* F10 pressed */

/* setup viewer (screen 1) */
EFI_STATUS SetupEnter (VOID);                /* lazy ModelInit, draw  */
VOID       SetupEvent (INPUT_EVENT *Event);
VOID       SetupDraw (VOID);
VOID       SetupTick (VOID);
BOOLEAN    SetupWantBoot (VOID);             /* F7 pressed */

#endif /* _SB_BIOS_H_ */
