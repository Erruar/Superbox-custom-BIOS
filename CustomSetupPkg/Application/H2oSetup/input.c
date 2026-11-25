/*
 * input.c - pointer and keyboard front-end.
 *
 * The UI consumes the standard pointer protocols:
 *   EFI_ABSOLUTE_POINTER_PROTOCOL  - absolute devices (touch panel, QEMU
 *                                    virtio tablet)
 *   EFI_SIMPLE_POINTER_PROTOCOL    - relative devices (USB/PS2 mouse)
 *
 * Firmware often publishes several pointer instances (built-in virtio input,
 * USB drivers bound late, broken stubs from the BDS).  LocateProtocol picks
 * an arbitrary one, so the module enumerates all instances, probes them, and
 * uses the healthy ones, falling over to the next instance when a device
 * starts returning EFI_DEVICE_ERROR.
 *
 * Keyboard input comes from the standard ConIn SimpleTextIn instance.  The
 * instance handed to an application started from the shell is a text-input
 * agent whose WaitForKey event reports EFI_DEVICE_ERROR for CheckEvent, so
 * the keystroke queue is polled directly.
 */

#include "H2oSetup.h"

#define MAX_POINTER_INSTANCES  8

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  *mAbs[MAX_POINTER_INSTANCES];
STATIC UINTN                          mAbsCount;
STATIC UINTN                          mAbsActive;

STATIC EFI_SIMPLE_POINTER_PROTOCOL    *mRel[MAX_POINTER_INSTANCES];
STATIC UINTN                          mRelCount;
STATIC UINTN                          mRelActive;

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
 * Relative devices report raw counts; a modern 1600dpi mouse produces
 * thousands of counts per swipe, so applying them 1:1 makes the cursor fly
 * across the screen and stick to the edges.  Scale counts to the display
 * width and divide by eight (a typical BIOS setup pointer speed),
 * accumulating the remainder so slow movements still move the cursor.
 */
STATIC INT32  mMoveAccX;
STATIC INT32  mMoveAccY;

STATIC
VOID
MoveCursorRelative (
  IN INT32  CountsX,
  IN INT32  CountsY
  )
{
  INT32  Scale = (INT32)GfxWidth () / 640;

  if (Scale < 1) {
    Scale = 1;
  }
  mMoveAccX += CountsX * Scale;
  mMoveAccY += CountsY * Scale;

  InputSetCursor (mX + mMoveAccX / 8, mY + mMoveAccY / 8);
  mMoveAccX %= 8;
  mMoveAccY %= 8;
}

/*
 * Collect every instance of a pointer protocol, probing each once.  An
 * instance answering EFI_SUCCESS or EFI_NOT_READY is usable; anything else
 * (DEVICE_ERROR from an unconnected stub) is skipped.
 */
STATIC
VOID
CollectPointers (
  IN  EFI_GUID      *Guid,
  IN  BOOLEAN       IsAbsolute,
  OUT VOID          **Slots,
  OUT UINTN         *Count
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  UINTN       Index, Total;
  VOID        *Proto;

  *Count = 0;
  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Total, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; (Index < Total) && (*Count < MAX_POINTER_INSTANCES); Index++) {
    Proto = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], Guid, &Proto)) ||
        (Proto == NULL)) {
      continue;
    }

    if (IsAbsolute) {
      EFI_ABSOLUTE_POINTER_STATE  St;

      Status = ((EFI_ABSOLUTE_POINTER_PROTOCOL *)Proto)->GetState (
                                                        (EFI_ABSOLUTE_POINTER_PROTOCOL *)Proto, &St);
    } else {
      EFI_SIMPLE_POINTER_STATE  St;

      Status = ((EFI_SIMPLE_POINTER_PROTOCOL *)Proto)->GetState (
                                                       (EFI_SIMPLE_POINTER_PROTOCOL *)Proto, &St);
    }

    if ((Status == EFI_SUCCESS) || (Status == EFI_NOT_READY)) {
      Slots[(*Count)++] = Proto;
    }
  }

  FreePool (Handles);
}

/*
 * Pointer protocols can be published before their backing device is
 * connected (ConSplitter aggregates a virtio/USB device that BDS never
 * started).  Connect every controller that appears in a pointer device path
 * so real hardware drivers bind before we probe the instances.
 */
STATIC
VOID
ConnectPointerControllers (
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
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Dp = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                       &gEfiDevicePathProtocolGuid, (VOID **)&Dp)) &&
        (Dp != NULL)) {
      gBS->ConnectController (Dp, NULL, NULL, TRUE);
    }
  }
  FreePool (Handles);
}

EFI_STATUS
InputInit (
  VOID
  )
{
  mX = (INT32)GfxWidth () / 2;
  mY = (INT32)GfxHeight () / 2;

  ConnectPointerControllers (&gEfiAbsolutePointerProtocolGuid);
  ConnectPointerControllers (&gEfiSimplePointerProtocolGuid);

  mAbsActive = 0;
  mRelActive = 0;
  CollectPointers (&gEfiAbsolutePointerProtocolGuid, TRUE,
                   (VOID **)mAbs, &mAbsCount);
  CollectPointers (&gEfiSimplePointerProtocolGuid, FALSE,
                   (VOID **)mRel, &mRelCount);

  SbLog ("[input] abs ok", mAbsCount, (UINTN)(mAbsCount ? mAbs[0] : 0));
  SbLog ("[input] rel ok", mRelCount, (UINTN)(mRelCount ? mRel[0] : 0));

  if ((mAbsCount == 0) && (mRelCount == 0)) {
    Print (L"[input] no usable pointer - keyboard only\n");
  }

  return EFI_SUCCESS;
}

VOID
InputExit (
  VOID
  )
{
  mAbsCount = 0;
  mRelCount = 0;
}

/* Poll the active relative device; hop to the next instance when it dies. */
STATIC
UINT32
PollSimplePointer (
  OUT INPUT_EVENT  *Event
  )
{
  EFI_SIMPLE_POINTER_PROTOCOL  *Sp;
  EFI_SIMPLE_POINTER_STATE     State;
  EFI_STATUS                   Status;
  UINT32                       Type  = EV_NONE;
  BOOLEAN                      Moved = FALSE;
  INT32                        Wheel = 0;
  UINTN                        Try;

  if (mRelCount == 0) {
    return EV_NONE;
  }

  for (Try = 0; Try < mRelCount; Try++) {
    Sp = mRel[mRelActive];
    do {
      Status = Sp->GetState (Sp, &State);
      if (Status == EFI_NOT_READY) {
        break;
      }
      if (EFI_ERROR (Status)) {
        // broken instance: try the next one on the next round
        mRelActive = (mRelActive + 1) % mRelCount;
        break;
      }

      if ((State.RelativeMovementX != 0) || (State.RelativeMovementY != 0)) {
        MoveCursorRelative (State.RelativeMovementX, State.RelativeMovementY);
        Moved = TRUE;
      }
      Wheel += State.RelativeMovementZ;

      if (State.LeftButton && !mLeftDown) {
        Type = EV_LCLICK;
      } else if (State.RightButton && !mRightDown) {
        Type = EV_RCLICK;
      }
      mLeftDown  = State.LeftButton;
      mRightDown = State.RightButton;
    } while (Type == EV_NONE && !EFI_ERROR (Status));

    if ((Type != EV_NONE) || Moved) {
      break;
    }
  }

  mWheel += Wheel;
  Event->X = mX;
  Event->Y = mY;

  if ((mWheel >= 2) || (mWheel <= -2)) {
    Event->Wheel = mWheel > 0 ? 1 : -1;
    mWheel       = 0;
    return EV_WHEEL;
  }
  if (Type != EV_NONE) {
    return Type;
  }
  if (Moved) {
    return EV_MOVE;
  }

  return EV_NONE;
}

/* Poll the active absolute device; hop to the next instance when it dies. */
STATIC
UINT32
PollAbsolutePointer (
  OUT INPUT_EVENT  *Event
  )
{
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Ap;
  EFI_ABSOLUTE_POINTER_STATE     State;
  EFI_STATUS                     Status;
  UINT64                         RangeX, RangeY;
  UINTN                          Try;
  UINT32                         Type = EV_NONE;

  if (mAbsCount == 0) {
    return EV_NONE;
  }

  for (Try = 0; Try < mAbsCount; Try++) {
    Ap = mAbs[mAbsActive];
    Status = Ap->GetState (Ap, &State);
    if (Status == EFI_NOT_READY) {
      break;
    }
    if (EFI_ERROR (Status)) {
      /* Some firmware pointer stacks go stale; a reset revives them. */
      Ap->Reset (Ap, TRUE);
      mAbsActive = (mAbsActive + 1) % mAbsCount;
      continue;
    }

    RangeX = Ap->Mode->AbsoluteMaxX - Ap->Mode->AbsoluteMinX;
    RangeY = Ap->Mode->AbsoluteMaxY - Ap->Mode->AbsoluteMinY;

    /*
     * Insyde ships a parked touch-panel stub that keeps reporting the corner
     * position; moving the cursor from it teleports the pointer.  Ignore a
     * corner report without an actual touch.
     */
    if ((State.ActiveButtons & EFI_ABSP_TouchActive) == 0) {
      if ((State.CurrentX <= Ap->Mode->AbsoluteMinX) &&
          (State.CurrentY <= Ap->Mode->AbsoluteMinY)) {
        break;
      }
    }

    if (RangeX != 0) {
      mX = (INT32)(((State.CurrentX - Ap->Mode->AbsoluteMinX) *
                    (INT64)(GfxWidth () - 1)) / (INT64)RangeX);
    }
    if (RangeY != 0) {
      mY = (INT32)(((State.CurrentY - Ap->Mode->AbsoluteMinY) *
                    (INT64)(GfxHeight () - 1)) / (INT64)RangeY);
    }
    InputSetCursor (mX, mY);
    Event->X = mX;
    Event->Y = mY;

    if ((State.ActiveButtons & EFI_ABSP_TouchActive) != 0) {
      if (!mTouchDown) {
        mTouchDown = TRUE;
        Type       = EV_LCLICK;
      } else {
        Type = EV_MOVE;
      }
    } else {
      mTouchDown = FALSE;
      Type       = EV_MOVE;
    }
    break;
  }

  return Type;
}

UINT32
InputPoll (
  OUT INPUT_EVENT  *Event
  )
{
  EFI_STATUS  Status;
  UINT32      Type;

  ZeroMem (Event, sizeof (*Event));

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
