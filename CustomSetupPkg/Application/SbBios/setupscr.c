/*
 * setupscr.c - the setup viewer screen: tabs, dashboard, form rows.
 *
 * Layout follows the design mock: a left status column (headline, clock,
 * date, CPU / memory / iGPU blocks divided by hand-drawn rules), a top tab
 * strip (orange = active, dark chip = inactive, white text), the real form
 * content on paper, and a hotkey footer.  The form data itself is never
 * invented here: prompts and values come from the firmware's HII IFR model
 * (model.c) exactly as the vendor publishes them.
 *
 * Navigation and writes reuse the proven prototype logic: rows -> REF opens
 * subforms, ONE_OF pops a list, values are committed live to the cached
 * varstore and flushed on F10 (save + reboot) or F7 (back to the boot
 * menu).
 */

#include "SbBios.h"

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
STATIC BOOLEAN     mHelp     = FALSE;      /* F1 overlay visible         */
STATIC BOOLEAN     mWantBoot = FALSE;
STATIC CHAR16      mStatus[128];
STATIC HW_INFO     mHw;
STATIC UINTN       mHwMinute = 0xFFFFFFFF;
STATIC BOOLEAN     mModelDone = FALSE;
STATIC EFI_STATUS  mModelStatus = EFI_NOT_STARTED;

/* tab strip geometry, hit-tested by the mouse (Last entry = Save&Exit) */
STATIC INT32     mTabX[MAX_TABS];
STATIC INT32     mTabW[MAX_TABS];
STATIC INT32     mTabY;
STATIC INT32     mBoxAEnd;      /* bottom of the "superbox" bracket box */
STATIC INT32     mTabH;
STATIC FORMSET  *mTabFs[MAX_TABS];
STATIC UINTN     mTabCount = 0;

#define S()      GfxUi ()
#define PADX()   ((INT32)(18 * S ()))
#define A(f)     GfxAscent (FONTW (f))
#define T(f,x,b,s,c)       GfxText (x, b, FONT (f), s, c, S ())
#define TW(f,s)            GfxTextW (FONTW (f), s, S ())
#define RH()               ((INT32)(22 * S ()))
#define CONTENT_X()        ((INT32)(GfxWidth () * 24 / 100))
#define CONTENT_W()        ((INT32)GfxWidth () - CONTENT_X () - PADX ())

/*
 * Fit a string into MaxPx pixels, appending ".." when clipped.  Vendor
 * formset titles ("UEFI Platform Initialization Configuration") are far
 * wider than a tab chip, and overlapping chips looked broken.
 */
STATIC
VOID
FitText (
  IN  CONST SB_GLYPH  *Tab,
  IN  UINTN           Count,
  IN  CONST CHAR16    *S,
  IN  INT32           MaxPx,
  OUT CHAR16          *Out,
  IN  UINTN           OutChars
  )
{
  INT32  I;

  if (GfxTextW (Tab, Count, S, S ()) <= MaxPx) {
    StrCpyS (Out, OutChars, S);
    return;
  }
  for (I = (INTN)OutChars - 4; I > 0; I--) {
    CopyMem (Out, S, I * sizeof (CHAR16));
    Out[I]     = '.';
    Out[I + 1] = '.';
    Out[I + 2] = 0;
    if (GfxTextW (Tab, Count, Out, S ()) <= MaxPx) {
      return;
    }
  }
  Out[0] = 0;
}

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
ContentTop (
  VOID
  )
{
  return mTabY + mTabH + (INT32)(44 * S ());
}

STATIC
INT32
FooterTop (
  VOID
  )
{
  return (INT32)GfxHeight () - (INT32)(46 * S ());
}

/*
 * The concept's panel grid.  Every panel is bounded by pencil corner
 * brackets only.  The tab row and the form list live in their own box
 * on the right (ContentMid is its bottom line), the Help Info box sits
 * under it, and the dashboard box on the left ends on the same bottom
 * line as Help Info, with a clear gap between the two boxes.  There is
 * no frame around the whole viewport.
 */
STATIC
INT32
ContentBottom (
  VOID
  )
{
  return FooterTop () - (INT32)(18 * S ());
}

STATIC
INT32
HelpTop (
  VOID
  )
{
  return ContentBottom ()
         - (A (SbSyne22) + A (SbMono16) + (INT32)(46 * S ()));
}

STATIC
INT32
ContentMid (
  VOID
  )
{
  return HelpTop () - (INT32)(30 * S ());
}

STATIC
INT32
RowsBottom (
  VOID
  )
{
  return ContentMid () - (INT32)(14 * S ());
}

STATIC
INT32
VisibleRows (
  VOID
  )
{
  /* leave room for the Help Info block at the bottom of the content */
  INT32  H = RowsBottom () - ContentTop ();
  INT32  N = H / RH ();

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

  Status = ModelFlush (mFs);

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

/*
 * F9: fill the current formset with the defaults the vendor encoded in the
 * IFR: ONE_OF options carry the DEFAULT option flag (UEFI spec: bit 0 of
 * EFI_IFR_ONE_OF_OPTION.Flags), NUMERIC questions mark a default with
 * question flag 0x08 and pick min/optimal/max in bits 5:4.  CHECKBOX has
 * no default encoding in IFR at all, so checkboxes are left alone.
 */
#define SB_OPT_FLAG_DEFAULT  0x01
#define SB_Q_FLAG_DEFAULT    0x08

STATIC
VOID
LoadDefaults (
  VOID
  )
{
  FORM      *F;
  STMT      *S;
  VARSTORE  *V;
  UINTN     I;

  for (F = mFs != NULL ? mFs->Forms : NULL; F != NULL; F = F->Next) {
    for (S = F->Statements; S != NULL; S = S->Next) {
      V = ModelVarstore (mFs, S->VarStoreId);
      if (V == NULL) {
        continue;
      }
      if (S->Kind == ST_ONEOF) {
        for (I = 0; I < S->OptionCount; I++) {
          if ((S->Options[I].Flags & SB_OPT_FLAG_DEFAULT) != 0) {
            ModelWrite (S, V, S->Options[I].Value);
            break;
          }
        }
      } else if ((S->Kind == ST_NUMERIC) &&
                 ((S->QFlags & SB_Q_FLAG_DEFAULT) != 0)) {
        INT64  Val;

        switch ((S->QFlags >> 4) & 0x3) {
          case 0:  Val = S->Min; break;
          case 2:  Val = S->Max; break;
          default: Val = (S->Min + S->Max) / 2; break;
        }
        ModelWrite (S, V, (UINT64)Val);
      }
    }
  }
  Commit (L"defaults loaded");
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
  SbLog ("[ui] popup", (UINTN)S->Kind, (UINTN)S->OptionCount);

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

/*
 * Tiny case-insensitive substring test, enough for prompt matching
 * (BaseLib in this tree has no SearchString).
 */
STATIC
BOOLEAN
PromptHas (
  IN CONST CHAR16  *Prompt,
  IN CONST CHAR16  *Needle
  )
{
  CONST CHAR16  *P;

  if ((Prompt == NULL) || (Needle == NULL) || (*Needle == 0)) {
    return FALSE;
  }
  for (P = Prompt; *P != 0; P++) {
    CONST CHAR16  *Q = P;
    CONST CHAR16  *N = Needle;

    for ( ; ; Q++, N++) {
      CHAR16  A = *Q;
      CHAR16  B = *N;

      if (B == 0) {
        return TRUE;                     /* needle exhausted: matched   */
      }
      if (A == 0) {
        return FALSE;
      }
      if (A >= L'a' && A <= L'z') {
        A -= 32;
      }
      if (B >= L'a' && B <= L'z') {
        B -= 32;
      }
      if (A != B) {
        break;
      }
    }
  }
  return FALSE;
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
        /* cross formset reference: find the formset that owns the form */
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
      } else if (PromptHas (S->Prompt, L"Boot")) {
        /*
         * Front Page's "Boot Manager" references a form the vendor never
         * publishes in HII; the stock browser serves it from its own boot
         * page.  Ours is the SbBios boot menu - go there (flushing vars
         * first, exactly like F7).
         */
        ModelFlush (mFs);
        mWantBoot = TRUE;
      } else {
        UnicodeSPrint (mStatus, sizeof (mStatus),
                       L"form 0x%04x not published by firmware",
                       S->RefFormId);
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

/*
 * A tab chip: square, colored fill, small vector icon, Syne label.
 * The icon shape follows the mockup: home / square / diamond / triangle,
 * chosen by the formset title's first letter.
 */
STATIC
VOID
DrawTabIcon (
  IN INT32    X,
  IN INT32    Top,
  IN CHAR16   Kind,
  IN UINT32   Color
  )
{
  INT32  S    = S ();
  INT32  Cx   = X + 7 * S;
  INT32  Cy   = Top + mTabH / 2;
  INT32  R    = 5 * S;
  INT32  Pts[4][2];

  switch (Kind) {
    case L'M':  /* home: roof triangle + body square */
      Pts[0][0] = Cx - R - S; Pts[0][1] = Cy - S;
      Pts[1][0] = Cx;         Pts[1][1] = Cy - R - 2 * S;
      Pts[2][0] = Cx + R + S; Pts[2][1] = Cy - S;
      GfxPolyFill (Pts, 3, Color);
      GfxFill (Cx - R + S, Cy - S, 2 * R - 2 * S, 3 * S, Color);
      break;
    case L'A':  /* square */
      GfxFill (Cx - R, Cy - R, 2 * R + S, 2 * R + S, Color);
      break;
    case L'B':  /* triangle */
      Pts[0][0] = Cx - R; Pts[0][1] = Cy + R;
      Pts[1][0] = Cx;     Pts[1][1] = Cy - R;
      Pts[2][0] = Cx + R; Pts[2][1] = Cy + R;
      GfxPolyFill (Pts, 3, Color);
      break;
    default:    /* diamond (Sys Config, Save&Exit) */
      Pts[0][0] = Cx;     Pts[0][1] = Cy - R;
      Pts[1][0] = Cx + R; Pts[1][1] = Cy;
      Pts[2][0] = Cx;     Pts[2][1] = Cy + R;
      Pts[3][0] = Cx - R; Pts[3][1] = Cy;
      GfxPolyFill (Pts, 4, Color);
      break;
  }
}

STATIC
VOID
DrawTabs (
  VOID
  )
{
  INT32     W = (INT32)GfxWidth ();
  INT32     Sc = S ();
  FORMSET   *F;
  INT32     X = CONTENT_X ();
  INT32     Icon = 20 * Sc;
  INT32     Gap = 6 * Sc;
  INT32     Avail = W - CONTENT_X () - PADX ();
  INT32     Slots = 1;               /* +1 for the Save&Exit chip   */
  INT32     SlotW;
  CHAR16    Label[64];

  for (F = mAll; F != NULL; F = F->Next) {
    Slots++;
  }
  SlotW = (Avail - (Slots - 1) * Gap) / Slots;
  if (SlotW < Icon + (INT32)(20 * Sc)) {
    SlotW = Icon + (INT32)(20 * Sc);   /* tiny sliver: labels clip out */
  }

  mTabY = PADX ();
  mTabH = A (SbSyne16) + (INT32)(12 * Sc);
  mTabCount = 0;

  for (F = mAll; F != NULL && mTabCount < MAX_TABS - 1; F = F->Next) {
    CHAR16  *Title = F->Title != NULL ? F->Title : L"(unnamed)";

    FitText (FONTW (SbSyne16), Title, SlotW - Icon - (INT32)(12 * Sc),
             Label, 64);
    mTabX[mTabCount]  = X;
    mTabW[mTabCount]  = SlotW;
    mTabFs[mTabCount] = F;
    mTabCount++;

    GfxFill (X, mTabY, SlotW, mTabH, (F == mFs) ? COL_ORANGE : COL_CARD);
    DrawTabIcon (X + (INT32)(6 * Sc), mTabY, Title[0], COL_WHITE);
    T (SbSyne16, X + Icon + (INT32)(4 * Sc),
       mTabY + (mTabH + A (SbSyne16)) / 2, Label, COL_WHITE);
    X += SlotW + Gap;
  }

  /* the Save&Exit chip (always last, dark like the inactive tabs) */
  if (mTabCount < MAX_TABS) {
    CHAR16  *Title = L"Save&Exit";

    mTabX[mTabCount]  = X;
    mTabW[mTabCount]  = SlotW;
    mTabFs[mTabCount] = NULL;              /* NULL marks Save&Exit   */
    mTabCount++;

    GfxFill (X, mTabY, SlotW, mTabH, COL_CARD);
    GfxCircle (X + Icon / 2, mTabY + mTabH / 2, 5 * Sc, COL_WHITE, TRUE);
    T (SbSyne16, X + Icon + (INT32)(4 * Sc),
       mTabY + (mTabH + A (SbSyne16)) / 2, Title, COL_WHITE);
  }
}

STATIC
VOID
StatPair (
  IN INT32      X,
  IN INT32      X2,
  IN OUT INT32  *Y,
  IN CHAR16     *L1,
  IN CHAR16     *V1,
  IN CHAR16     *L2,
  IN CHAR16     *V2
  )
{
  INT32  Sc = S ();

  T (SbSyne16, X, *Y, L1, COL_INK);
  T (SbSyne16, X2, *Y, L2, COL_INK);
  *Y += (INT32)(26 * Sc);
  T (SbMono16, X, *Y, V1, COL_INK);
  T (SbMono16, X2, *Y, V2, COL_INK);
  *Y += (INT32)(30 * Sc);
}

STATIC
VOID
DrawLeft (
  VOID
  )
{
  INT32  W = (INT32)GfxWidth ();
  INT32  Sc = S ();
  INT32  X = PADX ();
  INT32  LCW = CONTENT_X () - PADX () - (INT32)(20 * Sc);
  INT32  Pt = Sc >= 2 ? 2 : 1;    /* pencil line: thinner than before */
  INT32  Arm = (INT32)(16 * Sc);
  INT32  BoxB;
  INT32  Y, Tw;

  Y = PADX () + (INT32)(14 * Sc) + A (SbSyne30);  /* baseline, glyph tops clear the box corners */

  /* superbox / clock + gear / date / weekday */
  T (SbSyne30, X, Y, L"superbox", COL_INK);
  Y += A (SbSyne30) + (INT32)(14 * Sc);

  T (SbMono44, X, Y, mHw.Time, COL_INK);
  GfxGear (X + TW (SbMono44, mHw.Time) + (INT32)(22 * Sc),
           Y - A (SbMono44) / 2, 15 * Sc, COL_INK, COL_BG);
  Y += A (SbMono44) + (INT32)(10 * Sc);

  T (SbMono24, X, Y, mHw.Date, COL_INK);
  Y += A (SbMono24) + (INT32)(6 * Sc);

  T (SbSyne26, X, Y, mHw.Weekday, COL_INK);

  /* The "superbox" bracket box: its top line sits on the same row as
   * the tab chips, its bottom just clears the weekday.  Both left
   * boxes are narrower than the column so they never touch the right
   * column's boxes. */
  mBoxAEnd = Y + (INT32)(16 * Sc);
  GfxPencilCorners (X - (INT32)(10 * Sc), mTabY - (INT32)(10 * Sc),
                    LCW + (INT32)(6 * Sc),
                    mBoxAEnd - mTabY + (INT32)(10 * Sc),
                    Arm, Pt, 0x9B41Du, PC_ALL, COL_INK);

  /* The dashboard bracket box: starts below box A and ends on the
   * same bottom line as the Help Info box (see DrawContent) */
  BoxB = mBoxAEnd + (INT32)(30 * Sc);
  Y = BoxB + (INT32)(20 * Sc);

  /* ▣ Monitor */
  GfxFill (X, Y, (INT32)(12 * Sc), (INT32)(12 * Sc), COL_INK);
  GfxFill (X + 3 * Sc, Y + 3 * Sc, (INT32)(6 * Sc), (INT32)(6 * Sc), COL_BG);
  T (SbSyne22, X + (INT32)(20 * Sc), Y + 6 * Sc + A (SbSyne22) / 2,
     L"Monitor", COL_INK);
  Y += A (SbSyne26) + (INT32)(30 * Sc);   /* clear the Monitor baseline */

  /* CPU */
  T (SbSyne26, X, Y, L"CPU", COL_INK);
  Y += A (SbSyne26) + (INT32)(16 * Sc);
  StatPair (X, X + LCW * 46 / 100, &Y, L"Frequency", mHw.CpuFreq,
            L"Temperature", mHw.CpuTemp);
  StatPair (X, X + LCW * 46 / 100, &Y, L"BCLK", mHw.CpuBclk,
            L"Voltage", mHw.CpuVolt);

  GfxSketchH (X, Y, LCW, (UINT32)(X * 131 + Y), COL_INK);
  Y += A (SbSyne26) + (INT32)(10 * Sc);   /* clear the rule, then the glyph tops */

  T (SbSyne26, X, Y, L"MEMORY", COL_INK);
  Y += A (SbSyne26) + (INT32)(16 * Sc);
  StatPair (X, X + LCW * 46 / 100, &Y, L"Frequency", mHw.MemFreq,
            L"Voltage", mHw.MemVolt);

  GfxSketchH (X, Y, LCW, (UINT32)(X * 977 + Y * 7), COL_INK);
  Y += A (SbSyne26) + (INT32)(10 * Sc);

  T (SbSyne26, X, Y, L"iGPU", COL_INK);
  Y += A (SbSyne26) + (INT32)(16 * Sc);
  StatPair (X, X + LCW * 46 / 100, &Y, L"Frequency", mHw.GpuFreq,
            L"Voltage", mHw.GpuVolt);

  /* Close the dashboard box on the Help Info bottom line */
  GfxPencilCorners (X - (INT32)(10 * Sc), BoxB,
                    LCW + (INT32)(6 * Sc), ContentBottom () - BoxB,
                    Arm, Pt, 0x1F77Au, PC_ALL, COL_INK);

  /* CPU brand under the dashboard when it fits */
  if (mHw.Brand[0] != 0) {
    Tw = TW (SbMono16, mHw.Brand);
    if ((Tw <= LCW) && (Y + (INT32)(10 * Sc) + A (SbMono16) < ContentBottom ())) {
      T (SbMono16, X, Y + (INT32)(10 * Sc), mHw.Brand, COL_INK);
    }
  }
  (void)W;
}

STATIC
VOID
DrawContent (
  VOID
  )
{
  INT32      X = CONTENT_X ();
  INT32      CW = CONTENT_W ();
  INT32      W = (INT32)GfxWidth ();
  INT32      Sc = S ();
  INT32      Y, Bottom, I;
  INT32      ValX = X + CW * 47 / 100;
  INT32      Pt = Sc >= 2 ? 2 : 1;    /* pencil line: thinner than before */
  INT32      Arm = (INT32)(16 * Sc);

  /*
   * The concept's grid, corner brackets only, one box per panel, no
   * frame around the viewport: the tab row and the form list share a
   * box whose top line is the "superbox" box's top line; the Help Info
   * box below it ends on the same bottom line as the dashboard box,
   * with a visible gap between the two boxes.
   */
  {
    INT32  BX = X - (INT32)(10 * Sc);
    INT32  BY = mTabY - (INT32)(10 * Sc);

    GfxPencilCorners (BX, BY,
                      W - PADX () - BX, ContentMid () - BY,
                      Arm, Pt, 0xB0B8Du, PC_ALL, COL_INK);
  }

  if (EFI_ERROR (mModelStatus) || (mAll == NULL)) {
    T (SbSyne22, X, ContentTop (),
       L"The HII database published no formsets.", COL_INK);
    T (SbMono16, X, ContentTop () + (INT32)(30 * Sc),
       L"Press F7 to return to the boot menu.", COL_INK);
    return;
  }

  /* form title: the vendor's FORM title, not a fixed label.  The
   * baseline keeps the glyph tops below the tab chips' bottom edge */
  if (mForm != NULL && mForm->Title != NULL) {
    T (SbSyne22, X, ContentTop () - (INT32)(14 * Sc), mForm->Title, COL_INK);
  }
  if (mStatus[0] != 0) {
    INT32  Sw = TW (SbMono16, mStatus);

    T (SbMono16, X + CW - Sw, ContentTop () - (INT32)(14 * Sc), mStatus,
       COL_INK);
  }

  Y = ContentTop ();
  Bottom = RowsBottom ();

  if (mForm == NULL) {
    T (SbMono16, X, Y, L"This formset publishes no forms.", COL_INK);
    return;
  }
  if (mRowCount == 0) {
    T (SbMono16, X, Y, L"Empty form.", COL_INK);
    return;
  }

  if (mSel >= mTop + (UINTN)VisibleRows ()) {
    mTop = mSel - (UINTN)VisibleRows () + 1;
  }
  if (mSel < mTop) {
    mTop = mSel;
  }

  for (I = (INT32)mTop; (I < (INT32)mRowCount) &&
       (Y + RH () <= Bottom); I++) {
    STMT     *R   = mRows[I];
    BOOLEAN  Selr = ((UINTN)I == mSel);
    CHAR16   *Prompt = R->Prompt != NULL ? R->Prompt : L"(no prompt)";
    CHAR16   *Value;
    VARSTORE *V;

    if (Selr) {
      GfxFill (X - 4 * Sc, Y - 3 * Sc, CW + 8 * Sc, RH (), COL_ORANGE);
    }

    Value = NULL;
    V = ModelVarstore (mFs, R->VarStoreId);
    if ((R->Kind == ST_TEXT) || (R->Kind == ST_SUBTITLE)) {
      Value = R->Text2 != NULL ? R->Text2 : L"";
      T (SbMono16, X, Y + A (SbMono16) - 2 * Sc, Value, COL_INK);
      Y += RH ();
      continue;
    } else if (R->Kind == ST_REF) {
      CHAR16  Line[256];
      /* the arrow sits right behind the name, like the concept's
       * "System Log >" row; the active row is the orange fill, and
       * nothing gets an extra outline */
      UnicodeSPrint (Line, sizeof (Line), L"%s  >", Prompt);
      T (SbMono16, X, Y + A (SbMono16) - 2 * Sc, Line,
         Selr ? COL_WHITE : COL_INK);
      Y += RH ();
      continue;
    } else {
      Value = ModelValueText (R, V);
    }

    T (SbMono16, X, Y + A (SbMono16) - 2 * Sc, Prompt,
       Selr ? COL_WHITE : COL_INK);
    T (SbMono16, ValX, Y + A (SbMono16) - 2 * Sc,
       Value != NULL ? Value : L"", Selr ? COL_WHITE : COL_INK);

    if ((Value != NULL) && (Value != R->Text2)) {
      FreePool (Value);
    }
    Y += RH ();
  }

  /* scrollbar: a thin sketch rail with an orange thumb */
  if (mRowCount > (UINTN)VisibleRows ()) {
    INT32  BarH  = Bottom - ContentTop ();
    INT32  Thumb = BarH * VisibleRows () / (INT32)mRowCount;
    INT32  Pos   = BarH * (INT32)mTop / (INT32)mRowCount;

    if (Thumb < (INT32)(8 * Sc)) {
      Thumb = (INT32)(8 * Sc);
    }
    GfxFill (W - PADX () / 2, ContentTop (), Sc, BarH, COL_CARD);
    GfxFill (W - PADX () / 2, ContentTop () + Pos, Sc, Thumb, COL_ORANGE);
  }

  /* Help Info block: corner brackets only, bottom aligned with the
   * Monitor box (both end at ContentBottom) */
  {
    INT32  Hy = HelpTop ();
    INT32  Ty = Hy + (INT32)(14 * Sc) + A (SbSyne22);

    GfxPencilCorners (X - (INT32)(10 * Sc), Hy,
                      CW + (INT32)(20 * Sc), ContentBottom () - Hy,
                      Arm, Pt, 0x5E12C0DEu, PC_ALL, COL_INK);
    T (SbSyne22, X, Ty, L"Help Info", COL_INK);
    if (mSel < mRowCount && mRows[mSel] != NULL &&
        mRows[mSel]->Prompt != NULL) {
      T (SbMono16, X, Ty + A (SbMono16) + (INT32)(16 * Sc),
         mRows[mSel]->Prompt, COL_INK);
    } else {
      T (SbMono16, X, Ty + A (SbMono16) + (INT32)(16 * Sc),
         L"Select option to see help string", COL_INK);
    }
  }
}

STATIC
VOID
DrawFooter (
  VOID
  )
{
  INT32   W = (INT32)GfxWidth ();
  INT32   H = (INT32)GfxHeight ();
  INT32   Sc = S ();
  INT32   Y = FooterTop ();
  INT32   X = CONTENT_X ();
  UINTN   I;
  STATIC CONST CHAR16  *Hints[] = {
    L"General Help (F1)", L"Boot Menu (F7)", L"Print Screen (F12)",
    L"Load Defaults (F9)", L"Save and Exit (F10)"
  };
  CONST CHAR16  *Sign = L"2026 @Serzhik Saku, Insyde Corp.";
  INT32         Sw;

  for (I = 0; I < sizeof (Hints) / sizeof (Hints[0]); I++) {
    INT32  Tw = TW (SbMono16, Hints[I]);

    if (X + Tw > W - PADX ()) {
      break;
    }
    T (SbMono16, X, Y + A (SbMono16), Hints[I], COL_INK);
    X += Tw + (INT32)(28 * Sc);
  }

  Sw = TW (SbMono16, Sign);
  T (SbMono16, (W - Sw) / 2, Y + A (SbMono16) + (INT32)(22 * Sc), Sign,
     COL_INK);
  (void)H;
}

/* ---------------------------------------------------------------- popup */

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
  PanH  = (Count + 5) * RH () + (INT32)(24 * S ());
  if (PanH > H - (INT32)(60 * S ())) {
    PanH = H - (INT32)(60 * S ());
  }
  return PanH;
}

STATIC
INT32
PopupTop (
  VOID
  )
{
  return ((INT32)GfxHeight () - PopupGeometry ()) / 2;
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
  INT32      W  = (INT32)GfxWidth ();
  INT32      Sc = S ();
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

  /* paper panel, corner brackets only like every other panel */
  GfxFill (PanX, PanY, PanW, PanH, COL_BG);
  GfxPencilCorners (PanX, PanY, PanW, PanH, (INT32)(18 * Sc),
                    Sc >= 2 ? 2 : 1, (UINT32)(PanX * 31 + PanY * 7 + PanW),
                    PC_ALL, COL_INK);

  T (SbSyne22, PanX + (INT32)(16 * Sc),
     PanY + (INT32)(14 * Sc) + A (SbSyne22),
     mPopup->Prompt != NULL ? mPopup->Prompt : L"Select", COL_INK);

  V = ModelVarstore (mFs, mPopup->VarStoreId);
  Y = PanY + (INT32)(14 * Sc) + A (SbSyne22) + (INT32)(16 * Sc);

  if (mPopup->Kind == ST_ONEOF) {
    UINT64  Cur;
    BOOLEAN Valid;

    Cur = ModelRead (mPopup, V, &Valid);
    for (I = 0; I < mPopup->OptionCount; I++) {
      BOOLEAN  Sel   = (I == mPopupSel);
      CHAR16   *Mark = (mPopup->Options[I].Value == Cur) ? L"* " : L"  ";
      CHAR16   *Label = mPopup->Options[I].Label;

      if (Y + RH () > PanY + PanH - (INT32)(34 * Sc)) {
        break;               /* never paint over the key hint */
      }
      if (Sel) {
        GfxFill (PanX + (INT32)(10 * Sc), Y - 2 * Sc,
                 PanW - (INT32)(20 * Sc), RH (), COL_ORANGE);
      }
      T (SbMono16, PanX + (INT32)(16 * Sc), Y + A (SbMono16) - 2 * Sc,
         Mark, Sel ? COL_WHITE : COL_INK);
      T (SbMono16, PanX + (INT32)(16 * Sc) + (INT32)(20 * Sc),
         Y + A (SbMono16) - 2 * Sc, Label != NULL ? Label : L"(option)",
         Sel ? COL_WHITE : COL_INK);
      Y += RH ();
    }
  } else {
    CHAR16  *Value = ModelValueText (mPopup, V);
    INT32   Mid    = PanX + PanW / 2;

    T (SbMono16, Mid - TW (SbMono16, Value) / 2, Y + A (SbMono16),
       Value, COL_INK);
    Y += RH () * 2;
    T (SbMono16, PanX + (INT32)(16 * Sc), Y, L"[-]  decrease", COL_INK);
    T (SbMono16, Mid, Y, L"[+]  increase", COL_INK);
    if (Value != NULL) {
      FreePool (Value);
    }
  }

  T (SbMono16, PanX + (INT32)(16 * Sc),
     PanY + PanH - (INT32)(14 * Sc) - A (SbMono16),
     L"Enter pick, Esc close, Left/Right step", COL_INK);
}

/* --------------------------------------------------------- help overlay */

STATIC
VOID
DrawHelp (
  VOID
  )
{
  INT32   W = (INT32)GfxWidth ();
  INT32   H = (INT32)GfxHeight ();
  INT32   Sc = S ();
  INT32   PanW = W * 44 / 100;
  INT32   PanX = (W - PanW) / 2;
  INT32   PanH = (INT32)(15 * 22 * Sc);
  INT32   PanY = (H - PanH) / 2;
  INT32   Y;
  UINTN   I;
  STATIC CONST CHAR16  *Lines[] = {
    L"\x21C5      navigate entries",
    L"Enter     open / change",
    L"\x2190 \x2192  step value",
    L"Tab       next tab",
    L"Click     pick, wheel scrolls",
    L"F1, Esc   toggle this help",
    L"F7        boot menu",
    L"F9        load defaults",
    L"F10       save and reboot",
    L"F12       screenshot to USB stick",
    L"+ / -     video mode"
  };

  Y = PanY + (INT32)(14 * Sc) + A (SbSyne22);   /* baseline below the corners */
  GfxFill (PanX, PanY, PanW, PanH, COL_BG);
  GfxPencilCorners (PanX, PanY, PanW, PanH, (INT32)(18 * Sc),
                    Sc >= 2 ? 2 : 1, (UINT32)(PanX * 17 + PanY), PC_ALL,
                    COL_INK);
  T (SbSyne22, PanX + (INT32)(16 * Sc), Y, L"Hot keys", COL_INK);
  Y += A (SbSyne22) + (INT32)(16 * Sc);
  for (I = 0; I < sizeof (Lines) / sizeof (Lines[0]); I++) {
    T (SbMono16, PanX + (INT32)(16 * Sc), Y, Lines[I], COL_INK);
    Y += (INT32)(22 * Sc);
  }
}

VOID
SetupDraw (
  VOID
  )
{
  if (mHwMinute != (UINTN)mHw.Time[0] * 100 + mHw.Time[1]) {
    HwUpdate (&mHw);
    mHwMinute = (UINTN)mHw.Time[0] * 100 + mHw.Time[1];
  }

  GfxClear (COL_BG);
  DrawTabs ();
  DrawLeft ();
  DrawContent ();
  DrawFooter ();
  if (mMode == MODE_POPUP) {
    DrawPopup ();
  }
  if (mHelp) {
    DrawHelp ();
  }
}

/*
 * Timed refresh while the main loop idles: re-read the clock and the
 * sensors and repaint only the left panel (the "superbox" and
 * dashboard boxes).  The form list, tabs, popup and footer are left
 * untouched, so the update stays cheap and never fights a click.
 */
VOID
SetupTick (
  VOID
  )
{
  if ((mMode == MODE_POPUP) || mHelp) {
    HwUpdate (&mHw);
    mHwMinute = (UINTN)mHw.Time[0] * 100 + mHw.Time[1];
    return;
  }
  HwUpdate (&mHw);
  mHwMinute = (UINTN)mHw.Time[0] * 100 + mHw.Time[1];
  /*
   * Repaint the dashboard column only.  The fill boundary must not cross
   * any ink: the right content box's left corner arms span
   * CONTENT_X-10*Sc .. CONTENT_X+6*Sc, so anything reaching past
   * CONTENT_X-16*Sc clips them into bare dashes for one tick.  Stop at
   * CONTENT_X-40*Sc, ~30px clear of the left boxes' own corner tips
   * (CONTENT_X-14*Sc, redrawn identically by DrawLeft, wobble is seeded
   * and ends are pinned to the nominal line).
   */
  {
    INT32  Sc = S ();
    GfxFill (0, 0, CONTENT_X () - (INT32)(40 * Sc), (INT32)GfxHeight (), COL_BG);
  }
  DrawLeft ();
}

/* --------------------------------------------------------------- events */

STATIC
INT32
RowAt (
  IN INT32  Y
  )
{
  INT32  Top = ContentTop ();
  INT32  I;

  if ((Y < Top) || (mRowCount == 0)) {
    return -1;
  }
  I = (Y - Top) / RH ();
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

  if ((Y < mTabY) || (Y > mTabY + mTabH)) {
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
  I = (Y - (PopupTop () + (INT32)(14 * S ()) + A (SbSyne22) +
            (INT32)(16 * S ()))) / RH ();
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

  /* F1 toggles; Esc closes the overlay before doing anything else */
  if (K->ScanCode == SCAN_F1) {
    mHelp = !mHelp;
    return;
  }
  if (mHelp && ((K->ScanCode == SCAN_ESC) ||
                (K->UnicodeChar == CHAR_CARRIAGE_RETURN))) {
    mHelp = FALSE;
    return;
  }

  if (K->ScanCode == SCAN_F7) {
    ModelFlush (mFs);
    mWantBoot = TRUE;
    return;
  }
  if (K->ScanCode == SCAN_F12) {
    CHAR16  Name[24];

    if (!EFI_ERROR (GfxScreenshot (Name, 24))) {
      UnicodeSPrint (mStatus, sizeof (mStatus), L"saved %s", Name);
    } else {
      StrCpyS (mStatus, 128, L"no writable volume for screenshot");
    }
    return;
  }
  if (K->ScanCode == SCAN_F9) {
    LoadDefaults ();
    return;
  }
  if (K->ScanCode == SCAN_F10) {
    ModelFlush (mFs);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    return;
  }

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
      /* root Esc returns to the boot menu - there is nowhere "up" to go */
      if (!GoBack ()) {
        ModelFlush (mFs);
        mWantBoot = TRUE;
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
  }
}

VOID
SetupEvent (
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

    if (mHelp) {
      mHelp = FALSE;                     /* click anywhere closes it */
      return;
    }

    Tab = TabAt (E->X, E->Y);
    if (Tab >= 0) {
      if (mTabFs[Tab] == NULL) {         /* Save&Exit chip */
        ModelFlush (mFs);
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      } else {
        JumpToFormset (mTabFs[Tab]);
      }
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
    if (mHelp) {
      mHelp = FALSE;
      return;
    }
    GoBack ();
    return;
  }

  if (E->Type == EV_KEY) {
    HandleKey (&E->Key);
  }
}

BOOLEAN
SetupWantBoot (
  VOID
  )
{
  return mWantBoot;
}

EFI_STATUS
SetupEnter (
  VOID
  )
{
  FORMSET  *F;
  UINTN    Count = 0;

  mHelp     = FALSE;
  mWantBoot = FALSE;
  mStatus[0] = 0;

  if (!mModelDone) {
    mModelDone  = TRUE;
    mModelStatus = ModelInit ();
    for (F = ModelFormsets (); F != NULL; F = F->Next) {
      Count++;
    }
    SbLog ("[setup] formsets", Count, (UINTN)mModelStatus);
    mAll = ModelFormsets ();
    if (!EFI_ERROR (mModelStatus) && (mAll != NULL)) {
      mMode = MODE_LIST;
      FirstFormOf (mAll);
    }
  }
  HwUpdate (&mHw);
  mHwMinute = (UINTN)mHw.Time[0] * 100 + mHw.Time[1];
  SetupDraw ();
  return EFI_SUCCESS;
}
