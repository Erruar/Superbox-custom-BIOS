/*
 * ui.c - screen layout, navigation and widgets of the graphical setup UI.
 *
 * Layout is expressed in font cells multiplied by the integer scale chosen in
 * gfx.c, so the same code gives a readable page on a 1366x768 laptop panel and
 * on the 4K display of the target box without hardcoded pixel coordinates.
 */

#include "H2oSetup.h"

#define MAX_ROWS      512
#define MAX_STACK     12
#define MAX_TABS      32

#define MODE_LIST     0
#define MODE_POPUP    1

typedef struct {
  FORMSET  *Fs;
  FORM     *Form;
  UINTN    Sel;
} STACK_ITEM;

STATIC FORMSET    *mFs       = NULL;
STATIC FORM       *mForm     = NULL;
STATIC STMT       *mRows[MAX_ROWS];
STATIC UINTN       mRowCount = 0;
STATIC UINTN       mSel      = 0;
STATIC UINTN       mTop      = 0;
STATIC INT32       mMode     = MODE_LIST;
STATIC STMT       *mPopup    = NULL;
STATIC UINTN       mPopupSel = 0;
STATIC STACK_ITEM  mStack[MAX_STACK];
STATIC UINTN       mDepth    = 0;
STATIC FORMSET    *mAll      = NULL;
STATIC BOOLEAN     mQuit     = FALSE;
STATIC BOOLEAN     mChanged  = FALSE;
STATIC CHAR16      mStatus[128];

/* Tab strip geometry, filled by DrawHeader, hit-tested by the mouse. */
STATIC INT32       mTabX[MAX_TABS];
STATIC INT32       mTabW[MAX_TABS];
STATIC FORMSET    *mTabFs[MAX_TABS];
STATIC UINTN       mTabCount = 0;

#define PAD()     ((INT32)(6 * GfxScale ()))
#define ROW_H()   ((INT32)GfxCellH () + (INT32)GfxScale ())
#define HDR_H()   ((INT32)GfxCellH () * 3)
#define FTR_H()   ((INT32)GfxCellH () + (INT32)GfxScale () * 2)

/* ------------------------------------------------------------- row model */

STATIC
BOOLEAN
IsSelectable (
  IN STMT  *S
  )
{
  return (S->Kind == ST_ONEOF) || (S->Kind == ST_CHECKBOX) ||
         (S->Kind == ST_NUMERIC) || (S->Kind == ST_REF) ||
         (S->Kind == ST_ACTION) || (S->Kind == ST_PASSWORD);
}

STATIC
VOID
RebuildRows (
  VOID
  )
{
  STMT   *S;
  UINTN  I;

  mRowCount = 0;
  if (mForm != NULL) {
    for (S = mForm->Statements; S != NULL; S = S->Next) {
      if (mRowCount >= MAX_ROWS) {
        break;
      }
      switch (S->Kind) {
        case ST_ONEOF:
        case ST_CHECKBOX:
        case ST_NUMERIC:
        case ST_REF:
        case ST_ACTION:
        case ST_PASSWORD:
        case ST_TEXT:
        case ST_SUBTITLE:
          mRows[mRowCount++] = S;
          break;
        default:
          break;
      }
    }
  }

  if (mSel >= mRowCount) {
    mSel = 0;
  }
  for (I = 0; (I < mRowCount) && (mRowCount > 0); I++) {
    if (IsSelectable (mRows[mSel])) {
      break;
    }
    mSel = (mSel + 1) % mRowCount;
  }
  mTop = 0;
}

STATIC
INT32
VisibleRows (
  VOID
  )
{
  INT32  H = (INT32)GfxHeight () - HDR_H () - FTR_H () - 2 * PAD ();
  INT32  N = H / ROW_H ();

  return N > 1 ? N : 1;
}

/* ------------------------------------------------------------ navigation */

STATIC
VOID
SelectForm (
  IN FORMSET  *Fs,
  IN FORM     *Form,
  IN BOOLEAN  Push
  )
{
  if (Push && (mForm != NULL) && (mDepth < MAX_STACK)) {
    mStack[mDepth].Fs   = mFs;
    mStack[mDepth].Form = mForm;
    mStack[mDepth].Sel  = mSel;
    mDepth++;
  }

  mFs   = Fs;
  mForm = Form;
  mSel  = 0;
  mMode = MODE_LIST;
  RebuildRows ();
  ModelLoad (mFs);
}

STATIC
VOID
FirstFormOf (
  IN FORMSET  *Fs
  )
{
  FORM  *F;
  FORM  *Best = NULL;

  for (F = Fs->Forms; F != NULL; F = F->Next) {
    if ((Best == NULL) || (F->Id < Best->Id)) {
      Best = F;
    }
  }
  SelectForm (Fs, Best, FALSE);
}

STATIC
BOOLEAN
GoBack (
  VOID
  )
{
  if (mMode == MODE_POPUP) {
    mMode  = MODE_LIST;
    mPopup = NULL;
    return TRUE;
  }
  if (mDepth == 0) {
    return FALSE;
  }

  mDepth--;
  mFs   = mStack[mDepth].Fs;
  mForm = mStack[mDepth].Form;
  mSel  = mStack[mDepth].Sel;
  mMode = MODE_LIST;
  RebuildRows ();
  if (mSel >= mRowCount) {
    mSel = 0;
  }
  return TRUE;
}

STATIC
VOID
SwitchFormset (
  IN INT32  Direction
  )
{
  FORMSET  *F, *Target = NULL;
  UINTN    Count = 0, Index = 0, Current = 0, Want;

  for (F = mAll; F != NULL; F = F->Next) {
    if (F == mFs) {
      Current = Count;
    }
    Count++;
  }
  if (Count == 0) {
    return;
  }

  Want = (Current + (UINTN)Direction + Count) % Count;
  for (F = mAll; F != NULL; F = F->Next, Index++) {
    if (Index == Want) {
      Target = F;
      break;
    }
  }
  if (Target != NULL) {
    mDepth = 0;
    FirstFormOf (Target);
  }
}

STATIC
VOID
JumpToFormset (
  IN FORMSET  *Fs
  )
{
  if ((Fs != NULL) && (Fs != mFs)) {
    mDepth = 0;
    FirstFormOf (Fs);
  }
}

/* --------------------------------------------------------------- writes */

STATIC
VOID
Commit (
  IN CHAR16  *Text
  )
{
  EFI_STATUS  Status;

  Status   = ModelFlush (mFs);
  mChanged = TRUE;

  if (EFI_ERROR (Status)) {
    UnicodeSPrint (mStatus, sizeof (mStatus), L"write failed: %r", Status);
  } else {
    UnicodeSPrint (mStatus, sizeof (mStatus), L"%s", Text);
  }
}

STATIC
VOID
StepOneOf (
  IN STMT   *S,
  IN INTN   Direction
  )
{
  VARSTORE  *V;
  UINT64    Cur;
  BOOLEAN   Valid;
  UINTN     I, Hit = S->OptionCount;

  V = ModelVarstore (mFs, S->VarStoreId);
  if ((V == NULL) || (S->OptionCount == 0)) {
    return;
  }

  Cur = ModelRead (S, V, &Valid);
  for (I = 0; I < S->OptionCount; I++) {
    if (S->Options[I].Value == Cur) {
      Hit = I;
      break;
    }
  }
  Hit = (Hit >= S->OptionCount)
        ? 0
        : (Hit + (UINTN)Direction + S->OptionCount) % S->OptionCount;

  ModelWrite (S, V, S->Options[Hit].Value);
  Commit (S->Prompt != NULL ? S->Prompt : L"setting");
}

STATIC
VOID
StepNumeric (
  IN STMT  *S,
  IN INTN  Direction
  )
{
  VARSTORE  *V;
  UINT64    Cur;
  BOOLEAN   Valid;
  INT64     Next;
  INT64     Step = S->Step != 0 ? S->Step : 1;

  V = ModelVarstore (mFs, S->VarStoreId);
  if (V == NULL) {
    return;
  }

  Cur  = (INT64)ModelRead (S, V, &Valid);
  Next = Cur + (INT64)Direction * Step;

  if (Next < S->Min) {
    Next = S->Min;
  }
  if ((S->Max != 0) && (Next > S->Max)) {
    Next = S->Max;
  }
  if (Next < 0) {
    Next = 0;
  }

  ModelWrite (S, V, (UINT64)Next);
  Commit (S->Prompt != NULL ? S->Prompt : L"value");
}

STATIC
VOID
ToggleCheckbox (
  IN STMT  *S
  )
{
  VARSTORE  *V;
  BOOLEAN   Valid;
  UINT64    Cur;

  V = ModelVarstore (mFs, S->VarStoreId);
  if (V == NULL) {
    return;
  }
  Cur = ModelRead (S, V, &Valid);
  ModelWrite (S, V, Cur != 0 ? 0 : 1);
  Commit (S->Prompt != NULL ? S->Prompt : L"option");
}

STATIC
VOID
OpenPopup (
  IN STMT  *S
  )
{
  VARSTORE  *V;
  UINT64    Cur;
  BOOLEAN   Valid;
  UINTN     I;

  mMode     = MODE_POPUP;
  mPopup    = S;
  mPopupSel = 0;

  if (S->Kind == ST_ONEOF) {
    V   = ModelVarstore (mFs, S->VarStoreId);
    Cur = ModelRead (S, V, &Valid);
    for (I = 0; I < S->OptionCount; I++) {
      if (S->Options[I].Value == Cur) {
        mPopupSel = I;
        break;
      }
    }
  }
}

STATIC
VOID
Activate (
  IN STMT  *S
  )
{
  FORM  *F;

  switch (S->Kind) {
    case ST_REF: {
      FORMSET  *Fs = mFs;
      F = ModelForm (Fs, S->RefFormId);
      if (F == NULL) {
        /* Cross formset reference: find the formset that owns the form. */
        for (Fs = mAll; Fs != NULL; Fs = Fs->Next) {
          F = ModelForm (Fs, S->RefFormId);
          if (F != NULL) {
            break;
          }
        }
      }
      SbLog ("[ui] open form", S->RefFormId, (UINTN)(F != NULL));
      if (F != NULL) {
        SelectForm (Fs, F, TRUE);
      } else {
        UnicodeSPrint (mStatus, sizeof (mStatus),
                       L"form 0x%04x not in model", S->RefFormId);
      }
      break;
    }

    case ST_ONEOF:
    case ST_NUMERIC:
      OpenPopup (S);
      break;

    case ST_CHECKBOX:
      ToggleCheckbox (S);
      break;

    default:
      break;
  }
}

/* ------------------------------------------------------------- rendering */

STATIC
VOID
DrawHeader (
  VOID
  )
{
  INT32    W = (INT32)GfxWidth ();
  INT32    S = (INT32)GfxScale ();
  INT32    Y = PAD ();
  FORMSET  *F;
  INT32    TabX = PAD ();

  GfxFill (0, 0, W, HDR_H (), COL_HDR);

  mTabCount = 0;
  for (F = mAll; F != NULL; F = F->Next) {
    CHAR16  *Title = F->Title != NULL ? F->Title : L"(unnamed)";
    INT32   Tw     = GfxTextWidth (Title) + 8 * S;
    BOOLEAN Active = (F == mFs);

    if ((TabX + Tw > W - PAD ()) || (mTabCount >= MAX_TABS)) {
      break;
    }
    mTabX[mTabCount]  = TabX;
    mTabW[mTabCount]  = Tw;
    mTabFs[mTabCount] = F;
    mTabCount++;

    if (Active) {
      GfxFill (TabX, Y - S, Tw, ROW_H (), COL_SEL);
    }
    GfxText (TabX + 3 * S, Y, Title, Active ? COL_TEXT_HI : COL_TEXT_DIM, Tw);
    TabX += Tw + 2 * S;
  }

  Y += ROW_H () + S;
  if (mForm != NULL) {
    GfxText (PAD (), Y, mForm->Title != NULL ? mForm->Title : L"", COL_ACCENT,
             W - 2 * PAD ());
  }
  if (mStatus[0] != 0) {
    INT32  Sw = GfxTextWidth (mStatus);
    GfxText (W - PAD () - Sw, Y, mStatus, COL_VALUE, W - PAD ());
  }

  GfxFill (0, HDR_H () - S, W, S, COL_LINE);
}

STATIC
VOID
DrawList (
  VOID
  )
{
  INT32      W  = (INT32)GfxWidth ();
  INT32      S  = (INT32)GfxScale ();
  INT32      Top = HDR_H () + PAD ();
  INT32      Bottom = (INT32)GfxHeight () - FTR_H () - PAD ();
  INT32      Vis = VisibleRows ();
  UINTN      I;
  INT32      Y;

  if (mForm == NULL) {
    GfxText (PAD (), Top, L"This formset publishes no forms.",
             COL_TEXT_DIM, W - 2 * PAD ());
    return;
  }
  if (mRowCount == 0) {
    GfxText (PAD (), Top, L"Empty form.", COL_TEXT_DIM, W - 2 * PAD ());
    return;
  }

  if (mSel >= mTop + (UINTN)Vis) {
    mTop = mSel - (UINTN)Vis + 1;
  }
  if (mSel < mTop) {
    mTop = mSel;
  }

  Y = Top;
  for (I = mTop; (I < mRowCount) && (Y + ROW_H () <= Bottom); I++) {
    STMT      *S2  = mRows[I];
    BOOLEAN   Sel  = (I == mSel);
    BOOLEAN   Pick = IsSelectable (S2);
    CHAR16    *Prompt = S2->Prompt != NULL ? S2->Prompt : L"(no prompt)";
    CHAR16    *Value;
    VARSTORE  *V;
    INT32     VW;

    if (Sel) {
      GfxFill (PAD (), Y, W - 2 * PAD (), ROW_H (), COL_SEL);
      GfxFill (PAD (), Y, 3 * S, ROW_H (), COL_ACCENT);
    } else if (Pick && (((I - mTop) & 1) != 0)) {
      GfxFill (PAD (), Y, W - 2 * PAD (), ROW_H (), COL_BG_ALT);
    }

    GfxText (PAD () + 5 * S, Y, Prompt,
             Sel ? COL_TEXT_HI : (Pick ? COL_TEXT : COL_TEXT_DIM),
             W - 6 * PAD ());

    V = ModelVarstore (mFs, S2->VarStoreId);
    if ((S2->Kind == ST_TEXT) || (S2->Kind == ST_SUBTITLE)) {
      Value = S2->Text2 != NULL ? S2->Text2 : L"";
      VW    = GfxTextWidth (Value);
      GfxText (W - PAD () - VW, Y, Value, COL_TEXT_DIM, W - 2 * PAD ());
    } else {
      Value = ModelValueText (S2, V);
      VW    = GfxTextWidth (Value);
      GfxText (W - PAD () - VW - 3 * S, Y, Value,
               Sel ? COL_TEXT_HI : COL_VALUE, W - 3 * PAD ());
      if (S2->Kind == ST_REF) {
        GfxText (W - PAD () - 2 * S, Y, L">", Sel ? COL_TEXT_HI : COL_ACCENT, 0);
      }
      if ((Value != NULL) && (Value != S2->Text2)) {
        FreePool (Value);
      }
    }

    Y += ROW_H ();
  }

  if (mRowCount > (UINTN)Vis) {
    INT32  BarH  = Bottom - Top;
    INT32  Thumb = BarH * Vis / (INT32)mRowCount;
    INT32  Pos   = BarH * (INT32)mTop / (INT32)mRowCount;

    if (Thumb < 8 * S) {
      Thumb = 8 * S;
    }
    GfxFill (W - 2 * S, Top, S, BarH, COL_LINE);
    GfxFill (W - 2 * S, Top + Pos, S, Thumb, COL_ACCENT);
  }
}

STATIC
INT32
PopupGeometry (
  VOID
  )
{
  INT32  H = (INT32)GfxHeight ();
  INT32  Count;
  INT32  PanH;

  if (mPopup == NULL) {
    return 0;
  }
  Count = mPopup->Kind == ST_ONEOF ? (INT32)mPopup->OptionCount : 2;
  PanH  = (Count + 3) * ROW_H () + 2 * PAD ();
  if ((H - PanH) / 2 < HDR_H ()) {
    PanH = H - FTR_H () - HDR_H ();
  }
  return PanH;
}

STATIC
INT32
PopupTop (
  VOID
  )
{
  INT32  PanH = PopupGeometry ();
  INT32  PanY = ((INT32)GfxHeight () - PanH) / 2;

  return PanY < HDR_H () ? HDR_H () : PanY;
}

STATIC
INT32
PopupWidth (
  VOID
  )
{
  return (INT32)GfxWidth () / 2;
}

STATIC
VOID
DrawPopup (
  VOID
  )
{
  INT32      W = (INT32)GfxWidth ();
  INT32      S = (INT32)GfxScale ();
  INT32      PanW = PopupWidth ();
  INT32      PanX = (W - PanW) / 2;
  INT32      PanH = PopupGeometry ();
  INT32      PanY = PopupTop ();
  INT32      Y;
  UINTN      I;
  VARSTORE   *V;

  if (mPopup == NULL) {
    return;
  }

  GfxFill (PanX, PanY, PanW, PanH, COL_HDR);
  GfxFrame (PanX, PanY, PanW, PanH, COL_ACCENT);

  GfxText (PanX + PAD (), PanY + PAD (),
           mPopup->Prompt != NULL ? mPopup->Prompt : L"Select",
           COL_ACCENT, PanW - 2 * PAD ());

  V = ModelVarstore (mFs, mPopup->VarStoreId);
  Y = PanY + PAD () + ROW_H ();

  if (mPopup->Kind == ST_ONEOF) {
    UINT64  Cur;
    BOOLEAN Valid;

    Cur = ModelRead (mPopup, V, &Valid);
    for (I = 0; I < mPopup->OptionCount; I++) {
      BOOLEAN  Sel   = (I == mPopupSel);
      CHAR16   *Mark = (mPopup->Options[I].Value == Cur) ? L"* " : L"  ";
      CHAR16   *Label = mPopup->Options[I].Label;

      if (Sel) {
        GfxFill (PanX + PAD () - S, Y, PanW - 2 * PAD () + 2 * S, ROW_H (), COL_SEL);
      }
      GfxText (PanX + PAD (), Y, Mark, COL_ACCENT, 0);
      GfxText (PanX + PAD () + 3 * S, Y, Label != NULL ? Label : L"(option)",
               Sel ? COL_TEXT_HI : COL_TEXT, PanW - 3 * PAD ());
      Y += ROW_H ();
    }
  } else {
    CHAR16  *Value = ModelValueText (mPopup, V);
    INT32   Mid    = PanX + PanW / 2;

    GfxText (Mid - GfxTextWidth (Value) / 2, Y, Value, COL_VALUE,
             PanW - 2 * PAD ());
    Y += ROW_H () * 2;
    GfxText (PanX + PAD (), Y, L"[-]  decrease", COL_TEXT, PanW / 2 - 2 * PAD ());
    GfxText (Mid, Y, L"[+]  increase", COL_TEXT, PanW / 2 - 2 * PAD ());
    if (Value != NULL) {
      FreePool (Value);
    }
  }

  GfxText (PanX + PAD (), PanY + PanH - PAD () - ROW_H (),
           L"Enter pick, Esc close, Left/Right step", COL_TEXT_DIM,
           PanW - 2 * PAD ());
}

STATIC
VOID
DrawFooter (
  VOID
  )
{
  INT32    W = (INT32)GfxWidth ();
  INT32    H = (INT32)GfxHeight ();
  INT32    S = (INT32)GfxScale ();
  INT32    Y = H - FTR_H ();

  GfxFill (0, Y, W, FTR_H (), COL_HDR);
  GfxFill (0, Y, W, S, COL_LINE);
  {
    /*
     * Hints are drawn as separate segments so a narrow display drops whole
     * words instead of cutting one of them in half at the right margin.
     */
    STATIC CONST CHAR16  *Hints[] = {
      L"Up/Down move", L"Enter change", L"L/R step", L"Tab formset",
      L"Esc back", L"+/- mode"
    };
    INT32  X = PAD ();
    UINTN  I;

    for (I = 0; I < sizeof (Hints) / sizeof (Hints[0]); I++) {
      INT32  Tw = GfxTextWidth (Hints[I]);

      if (X + Tw > W - PAD ()) {
        break;
      }
      GfxText (X, Y + S, Hints[I], COL_TEXT_DIM, W - PAD () - X);
      X += Tw + ((INT32)GfxCellW () * 2);
    }
  }
  if (mChanged) {
    /* writes are committed live; the marker just reminds about the reboot */
    CONST CHAR16  *Mark = L"* F10 reboot";
    INT32          Tw  = GfxTextWidth (Mark);

    GfxText (W - PAD () - Tw, Y + S, Mark, COL_VALUE, Tw);
  }
}

VOID
UiDraw (
  VOID
  )
{
  GfxClear (COL_BG);
  DrawHeader ();
  DrawList ();
  DrawFooter ();
  if (mMode == MODE_POPUP) {
    DrawPopup ();
  }
}

/* --------------------------------------------------------------- events */

STATIC
INT32
RowAt (
  IN INT32  Y
  )
{
  INT32  Top = HDR_H () + PAD ();
  INT32  I;

  if ((Y < Top) || (mRowCount == 0)) {
    return -1;
  }
  I = (Y - Top) / ROW_H ();
  if ((I < 0) || (mTop + (UINTN)I >= mRowCount)) {
    return -1;
  }
  return (INT32)(mTop + (UINTN)I);
}

STATIC
INT32
TabAt (
  IN INT32  X,
  IN INT32  Y
  )
{
  UINTN  I;

  if (Y > HDR_H ()) {
    return -1;
  }
  for (I = 0; I < mTabCount; I++) {
    if ((X >= mTabX[I]) && (X < mTabX[I] + mTabW[I])) {
      return (INT32)I;
    }
  }
  return -1;
}

STATIC
INT32
PopupRowAt (
  IN INT32  Y
  )
{
  INT32  I;

  if ((mPopup == NULL) || (mPopup->Kind != ST_ONEOF)) {
    return -1;
  }
  I = (Y - (PopupTop () + PAD () + ROW_H ())) / ROW_H ();
  if ((I < 0) || ((UINTN)I >= mPopup->OptionCount)) {
    return -1;
  }
  return I;
}

STATIC
VOID
PickPopup (
  VOID
  )
{
  VARSTORE  *V;

  if ((mPopup == NULL) || (mPopup->Kind != ST_ONEOF)) {
    mMode = MODE_LIST;
    return;
  }
  if (mPopupSel < mPopup->OptionCount) {
    V = ModelVarstore (mFs, mPopup->VarStoreId);
    ModelWrite (mPopup, V, mPopup->Options[mPopupSel].Value);
    Commit (mPopup->Prompt != NULL ? mPopup->Prompt : L"setting");
  }
  mMode  = MODE_LIST;
  mPopup = NULL;
}

STATIC
VOID
MoveSelection (
  IN INTN  Direction
  )
{
  UINTN  Guard;

  if (mRowCount == 0) {
    return;
  }
  Guard = 0;
  do {
    mSel = (mSel + (UINTN)Direction + mRowCount) % mRowCount;
    Guard++;
  } while (!IsSelectable (mRows[mSel]) && (Guard < mRowCount));
}

STATIC
VOID
HandleKey (
  IN EFI_INPUT_KEY  *K
  )
{
  mStatus[0] = 0;

  if (mMode == MODE_POPUP) {
    switch (K->ScanCode) {
      case SCAN_UP:
        if (mPopupSel > 0) {
          mPopupSel--;
        }
        break;
      case SCAN_DOWN:
        if ((mPopup != NULL) && (mPopupSel + 1 < mPopup->OptionCount)) {
          mPopupSel++;
        }
        break;
      case SCAN_ESC:
        mMode  = MODE_LIST;
        mPopup = NULL;
        break;
      case SCAN_LEFT:
        if ((mPopup != NULL) && (mPopup->Kind == ST_NUMERIC)) {
          StepNumeric (mPopup, -1);
        }
        break;
      case SCAN_RIGHT:
        if ((mPopup != NULL) && (mPopup->Kind == ST_NUMERIC)) {
          StepNumeric (mPopup, 1);
        }
        break;
      default:
        break;
    }
    if ((K->UnicodeChar == CHAR_CARRIAGE_RETURN) ||
        (K->UnicodeChar == L' ')) {
      PickPopup ();
    }
    return;
  }

  if (K->UnicodeChar == CHAR_TAB) {
    SwitchFormset (1);
    return;
  }

  switch (K->ScanCode) {
    case SCAN_UP:
      MoveSelection (-1);
      return;
    case SCAN_DOWN:
      MoveSelection (1);
      return;
    case SCAN_HOME:
      mSel = 0;
      return;
    case SCAN_END:
      mSel = mRowCount > 0 ? mRowCount - 1 : 0;
      return;
    case SCAN_ESC:
      if (!GoBack ()) {
        mQuit = TRUE;
      }
      return;
    case SCAN_PAGE_UP:
      mSel = mSel > (UINTN)VisibleRows () ? mSel - (UINTN)VisibleRows () : 0;
      return;
    case SCAN_PAGE_DOWN:
      mSel = mSel + (UINTN)VisibleRows ();
      if (mSel >= mRowCount) {
        mSel = mRowCount > 0 ? mRowCount - 1 : 0;
      }
      return;
    case SCAN_F10:
      ModelFlush (mFs);
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      return;
    case SCAN_LEFT:
      if (mSel < mRowCount) {
        if (mRows[mSel]->Kind == ST_ONEOF) {
          StepOneOf (mRows[mSel], -1);
        } else if (mRows[mSel]->Kind == ST_NUMERIC) {
          StepNumeric (mRows[mSel], -1);
        }
      }
      return;
    case SCAN_RIGHT:
      if (mSel < mRowCount) {
        if (mRows[mSel]->Kind == ST_ONEOF) {
          StepOneOf (mRows[mSel], 1);
        } else if (mRows[mSel]->Kind == ST_NUMERIC) {
          StepNumeric (mRows[mSel], 1);
        }
      }
      return;
    default:
      break;
  }

  if (K->UnicodeChar == L'+') {
    if (!EFI_ERROR (GfxStepMode (1))) {
      UnicodeSPrint (mStatus, sizeof (mStatus), L"mode %ux%u",
                     (UINTN)GfxWidth (), (UINTN)GfxHeight ());
    }
    return;
  }
  if (K->UnicodeChar == L'-') {
    if (!EFI_ERROR (GfxStepMode (-1))) {
      UnicodeSPrint (mStatus, sizeof (mStatus), L"mode %ux%u",
                     (UINTN)GfxWidth (), (UINTN)GfxHeight ());
    }
    return;
  }

  if (K->UnicodeChar == CHAR_CARRIAGE_RETURN) {
    if (mSel < mRowCount) {
      Activate (mRows[mSel]);
    }
  } else if (K->UnicodeChar == L'q') {
    mQuit = TRUE;
  }
}

VOID
UiEvent (
  IN INPUT_EVENT  *E
  )
{
  if (E->Type == EV_MOVE) {
    return;
  }

  if (E->Type == EV_WHEEL) {
    if ((mMode == MODE_LIST) && (mRowCount > 0)) {
      INT32  Vis = VisibleRows ();
      INT32  Max = (INT32)mRowCount - Vis;
      INT32  New = (INT32)mTop + E->Wheel;

      if (Max < 0) {
        Max = 0;
      }
      if (New < 0) {
        New = 0;
      }
      if (New > Max) {
        New = Max;
      }
      mTop = (UINTN)New;
    }
    return;
  }

  if (E->Type == EV_LCLICK) {
    INT32  Tab;

    Tab = TabAt (E->X, E->Y);
    if (Tab >= 0) {
      JumpToFormset (mTabFs[Tab]);
      return;
    }

    if (mMode == MODE_POPUP) {
      INT32  R = PopupRowAt (E->Y);
      if (R >= 0) {
        mPopupSel = (UINTN)R;
        PickPopup ();
      } else {
        mMode  = MODE_LIST;
        mPopup = NULL;
      }
      return;
    }

    {
      INT32  R = RowAt (E->Y);
      if (R >= 0) {
        mSel = (UINTN)R;
        if (IsSelectable (mRows[mSel])) {
          Activate (mRows[mSel]);
        }
      }
    }
    return;
  }

  if (E->Type == EV_RCLICK) {
    GoBack ();
    return;
  }

  if (E->Type == EV_KEY) {
    HandleKey (&E->Key);
  }
}

BOOLEAN
UiQuitRequested (
  VOID
  )
{
  return mQuit;
}

EFI_STATUS
UiInit (
  IN FORMSET  *First
  )
{
  mAll  = First;
  mMode = MODE_LIST;
  mStatus[0] = 0;
  FirstFormOf (First);

  return (mForm != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}
