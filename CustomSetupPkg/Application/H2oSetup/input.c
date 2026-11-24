/*
 * input.c - pointer and keyboard front-end.
 *
 * The firmware already ships USB/PS2 mouse and I2C touch drivers, so the UI
 * only consumes the standard pointer protocols:
 *   EFI_SIMPLE_POINTER_PROTOCOL    - relative devices (USB/PS2 mouse)
 *   EFI_ABSOLUTE_POINTER_PROTOCOL  - absolute devices (touch panel)
 * Keyboard input comes from the standard ConIn SimpleTextIn instance.
 */

#include "H2oSetup.h"

STATIC EFI_SIMPLE_POINTER_PROTOCOL   *mSp   = NULL;
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL *mAp   = NULL;

STATIC INT32   mX, mY;
STATIC BOOLEAN mLeftDown  = FALSE;
STATIC BOOLEAN mRightDown = FALSE;
STATIC BOOLEAN mTouchDown = FALSE;
STATIC INT32   mWheel     = 0;

VOID
InputSetCursor (
  IN INT32  X,
  IN INT32  Y
  )
{
  mX = X;
  mY = Y;
  if (mX < 0) {
    mX = 0;
  }
  if (mY < 0) {
    mY = 0;
  }
  if (mX >= (INT32)GfxWidth ()) {
    mX = (INT32)GfxWidth () - 1;
  }
  if (mY >= (INT32)GfxHeight ()) {
    mY = (INT32)GfxHeight () - 1;
  }
}

VOID
InputGetCursor (
  OUT INT32  *X,
  OUT INT32  *Y
  )
{
  *X = mX;
  *Y = mY;
}

EFI_STATUS
InputInit (
  VOID
  )
{
  EFI_STATUS  Status;

  mX = (INT32)GfxWidth () / 2;
  mY = (INT32)GfxHeight () / 2;

  Status = gBS->LocateProtocol (&gEfiSimplePointerProtocolGuid, NULL, (VOID **)&mSp);
  if (EFI_ERROR (Status)) {
    mSp = NULL;
  }

  Status = gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&mAp);
  if (EFI_ERROR (Status)) {
    mAp = NULL;
  }

  if ((mSp == NULL) && (mAp == NULL)) {
    Print (L"[input] no pointer protocol found - keyboard only\n");
  }

  return EFI_SUCCESS;
}

VOID
InputExit (
  VOID
  )
{
  mSp = NULL;
  mAp = NULL;
}

/* Poll the relative device (USB/PS2 mouse). */
STATIC
UINT32
PollSimplePointer (
  OUT INPUT_EVENT  *Event
  )
{
  EFI_SIMPLE_POINTER_STATE  State;
  EFI_STATUS                Status;
  UINT32                    Type  = EV_NONE;
  BOOLEAN                   Moved = FALSE;
  INT32                     Wheel = 0;

  if (mSp == NULL) {
    return EV_NONE;
  }

  while (TRUE) {
    Status = mSp->GetState (mSp, &State);
    if (EFI_ERROR (Status)) {
      break;
    }

    if ((State.RelativeMovementX != 0) || (State.RelativeMovementY != 0)) {
      InputSetCursor (mX + State.RelativeMovementX, mY + State.RelativeMovementY);
      Moved = TRUE;
    }
    Wheel += State.RelativeMovementZ;

    if (State.LeftButton && !mLeftDown) {
      Type       = EV_LCLICK;
    } else if (State.RightButton && !mRightDown) {
      Type       = EV_RCLICK;
    }
    mLeftDown  = State.LeftButton;
    mRightDown = State.RightButton;

    if (Type != EV_NONE) {
      break;
    }
  }

  mWheel += Wheel;
  Event->X = mX;
  Event->Y = mY;

  if (Type == EV_LCLICK) {
    return Type;
  }
  if (Type == EV_RCLICK) {
    return Type;
  }
  if ((mWheel >= 2) || (mWheel <= -2)) {
    Event->Wheel = mWheel > 0 ? 1 : -1;
    mWheel       = 0;
    return EV_WHEEL;
  }
  if (Moved) {
    return EV_MOVE;
  }

  return EV_NONE;
}

/* Poll the absolute device (touch panel). */
STATIC
UINT32
PollAbsolutePointer (
  OUT INPUT_EVENT  *Event
  )
{
  EFI_ABSOLUTE_POINTER_STATE  State;
  EFI_STATUS                  Status;
  UINT64                      RangeX, RangeY;

  if (mAp == NULL) {
    return EV_NONE;
  }

  Status = mAp->GetState (mAp, &State);
  if (EFI_ERROR (Status)) {
    return EV_NONE;
  }

  RangeX = mAp->Mode->AbsoluteMaxX - mAp->Mode->AbsoluteMinX;
  RangeY = mAp->Mode->AbsoluteMaxY - mAp->Mode->AbsoluteMinY;

  if (RangeX != 0) {
    mX = (INT32)(((State.CurrentX - mAp->Mode->AbsoluteMinX) * (GfxWidth () - 1)) / RangeX);
  }
  if (RangeY != 0) {
    mY = (INT32)(((State.CurrentY - mAp->Mode->AbsoluteMinY) * (GfxHeight () - 1)) / RangeY);
  }

  Event->X = mX;
  Event->Y = mY;

  if ((State.ActiveButtons & EFI_ABSP_TouchActive) != 0) {
    if (!mTouchDown) {
      mTouchDown = TRUE;
      return EV_LCLICK;
    }
  } else {
    mTouchDown = FALSE;
  }

  return EV_MOVE;
}

UINT32
InputPoll (
  OUT INPUT_EVENT  *Event
  )
{
  EFI_STATUS  Status;
  UINT32      Type;

  ZeroMem (Event, sizeof (*Event));

  /* Keyboard has priority so that key repeat feels responsive. */
  if ((gST->ConIn != NULL) && (gST->ConIn->WaitForKey != NULL)) {
    Status = gBS->CheckEvent (gST->ConIn->WaitForKey);
    if (!Status) {
      if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Event->Key))) {
        return EV_KEY;
      }
    }
  }

  Type = PollSimplePointer (Event);
  if (Type != EV_NONE) {
    Event->Type = Type;
    return Type;
  }

  Type = PollAbsolutePointer (Event);
  if (Type != EV_NONE) {
    Event->Type = Type;
    return Type;
  }

  return EV_NONE;
}
