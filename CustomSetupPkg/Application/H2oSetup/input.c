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

/*
 * A pointer protocol can be published by a bus driver whose child device is
 * not connected yet (OVMF/QEMU enumerate USB late).  GetState then returns
 * EFI_DEVICE_ERROR, so walk the protocol's device path back to its controller
 * and connect it once.
 */
STATIC
VOID
ConnectPointerDevice (
  IN EFI_GUID  *ProtocolGuid
  )
{
  EFI_HANDLE                *Handles;
  EFI_STATUS                Status;
  UINTN                     Index, Count;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol, ProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Count == 0)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Dp = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                       &gEfiDevicePathProtocolGuid, (VOID **)&Dp)) &&
        (Dp != NULL)) {
      gBS->ConnectController (Dp, NULL, NULL, TRUE);

      /* identify the device by its path node types (type<<8|subtype) */
      {
        EFI_DEVICE_PATH_PROTOCOL  *Node = Dp;
        UINTN                     Guard = 0;

        while (!IsDevicePathEnd (Node) && (Guard++ < 8)) {
          SbLog ("[input] dp node",
                 (DevicePathType (Node) << 8) | DevicePathSubType (Node),
                 Guard);
          Node = NextDevicePathNode (Node);
        }
      }
    } else {
      SbLog ("[input] connect nodevpath", 0, Index);
    }
  }
  FreePool (Handles);
}

EFI_STATUS
InputInit (
  VOID
  )
{
  EFI_STATUS  Status;

  mX = (INT32)GfxWidth () / 2;
  mY = (INT32)GfxHeight () / 2;

  ConnectPointerDevice (&gEfiSimplePointerProtocolGuid);
  ConnectPointerDevice (&gEfiAbsolutePointerProtocolGuid);

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

  /* One-shot probe: report what each published instance says about itself. */
  {
    EFI_SIMPLE_POINTER_STATE    SpState;
    EFI_ABSOLUTE_POINTER_STATE  ApState;

    if (mSp != NULL) {
      SbLog ("[input] sp probe", (UINTN)mSp->GetState (mSp, &SpState), 0);
    }
    if (mAp != NULL) {
      SbLog ("[input] ap probe", (UINTN)mAp->GetState (mAp, &ApState), 0);
      SbLog ("[input] ap range", (UINTN)mAp->Mode->AbsoluteMaxX,
             (UINTN)mAp->Mode->AbsoluteMaxY);
    }
  }

  SbLog ("[input] init sp/ap", (UINTN)mSp, (UINTN)mAp);

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
  STATIC UINTN                mApErrLog = 0;
  STATIC UINTN                mApTrace  = 0;

  if (mAp == NULL) {
    return EV_NONE;
  }

  Status = mAp->GetState (mAp, &State);
  if (EFI_ERROR (Status)) {
    if (mApErrLog++ < 4) {
      SbLog ("[input] abs getstate", (UINTN)Status, 0);
    }
    return EV_NONE;
  }

  if (mApTrace++ < 40) {
    SbLog ("[input] abs state", (UINTN)State.CurrentX, (UINTN)State.CurrentY);
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

  /*
   * Keyboard has priority so that key repeat feels responsive.  The ConIn
   * instance handed to an application is a shell text-input agent whose
   * WaitForKey event may report EFI_DEVICE_ERROR for CheckEvent, so the
   * keystroke queue is polled directly.
   */
  if (gST->ConIn != NULL) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Event->Key);
    if (!EFI_ERROR (Status)) {
      SbLog ("[input] key", Event->Key.ScanCode, Event->Key.UnicodeChar);
      Event->Type = EV_KEY;
      return EV_KEY;
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
