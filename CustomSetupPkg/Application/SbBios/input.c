/*
 * input.c - pointer and keyboard front-end.
 *
 * Two pointer flavors are consumed:
 *   EFI_SIMPLE_POINTER_PROTOCOL    - relative devices (USB/PS2 mouse)
 *   EFI_ABSOLUTE_POINTER_PROTOCOL  - absolute devices (touch panel, QEMU
 *                                    virtio tablet)
 *
 * On the target (an HP ENVY x360 with InsydeH2O) the touchscreen publishes
 * an absolute device that parks the cursor in the top corners and emits
 * reports without any touch - that is what "teleports" the cursor.  Policy:
 *
 *   1. A relative device wins: while it has produced input recently, plain
 *      (no-touch) absolute reports are ignored.
 *   2. Absolute reports that (a) carry no TouchActive/AltActive and (b) sit
 *      in the top corner parking zones are dropped unconditionally.
 *   3. Touches (TouchActive) are honoured as clicks everywhere.
 *   4. The box's PS/2 touchpad stream emits garbage bursts (hundreds of
 *      counts per report) that fling the cursor into the corners.  Every
 *      report is clamped to REL_CLAMP counts, and an instance that bursts
 *      REL_BURSTS times is banned permanently in favour of the next
 *      relative device (the USB mouse).  If every relative instance is
 *      banned, the priority window goes quiet and the touchscreen
 *      (absolute) drives the cursor.
 *
 * Keyboard input comes from ConIn SimpleTextIn; the agent instance handed
 * to shell-launched apps reports EFI_DEVICE_ERROR on CheckEvent, so the
 * queue is polled directly.
 */

#include "SbBios.h"

#define MAX_POINTER_INSTANCES  8
#define REL_PRIORITY_TICKS     120      /* ~2s of 16ms polls            */
#define CORNER_PARK_PCT        4        /* top-corner parking zone, %   */
#define REL_CLAMP              64       /* per-report movement cap      */
#define REL_BURST              240      /* beyond: PS/2 touchpad garbage */
#define REL_BURSTS             3        /* bursts that ban an instance  */

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  *mAbs[MAX_POINTER_INSTANCES];
STATIC UINTN                          mAbsCount;
STATIC UINTN                          mAbsActive;

STATIC EFI_SIMPLE_POINTER_PROTOCOL    *mRel[MAX_POINTER_INSTANCES];
STATIC UINTN                          mRelCount;
STATIC UINTN                          mRelActive;

STATIC INT32    mX, mY;
STATIC BOOLEAN  mLeftDown  = FALSE;
STATIC BOOLEAN  mRightDown = FALSE;
STATIC BOOLEAN  mTouchDown = FALSE;
STATIC BOOLEAN  mAltDown   = FALSE;
STATIC INT32    mWheel     = 0;
STATIC UINT64   mPollTick  = 0;
STATIC UINT64   mLastRelTick = 0;
STATIC UINTN    mParkDrops = 0;
STATIC UINTN    mAbsLogged = 0;
STATIC UINTN    mRelBurst[MAX_POINTER_INSTANCES];  /* per-instance garbage */
STATIC UINTN    mRelBanMask = 0;                   /* bit set: instance dead */

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
 * thousands of counts per swipe, so scale counts to display width and
 * divide by eight, accumulating the remainder for slow movements.
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
  INT32  Scale = (INT32)GfxWidth () / (GfxWidth () >= 2560 ? 480 : 640);

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
 * Collect every instance of a pointer protocol, probing each once;
 * instances answering SUCCESS/NOT_READY are usable, broken stubs (a
 * DEVICE_ERROR on the very first probe) are dropped.
 */
STATIC
VOID
CollectPointers (
  IN  EFI_GUID  *Guid,
  IN  BOOLEAN   IsAbsolute,
  OUT VOID      **Slots,
  OUT UINTN     *Count
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
 * connected; connect every controller that appears in a pointer device path
 * so real hardware drivers bind before we probe.
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
  ZeroMem (mRelBurst, sizeof (mRelBurst));
  mRelBanMask = 0;
  CollectPointers (&gEfiAbsolutePointerProtocolGuid, TRUE,
                   (VOID **)mAbs, &mAbsCount);
  CollectPointers (&gEfiSimplePointerProtocolGuid, FALSE,
                   (VOID **)mRel, &mRelCount);

  SbLog ("[input] abs ok", mAbsCount, (UINTN)(mAbsCount ? mAbs[0] : 0));
  SbLog ("[input] rel ok", mRelCount, (UINTN)(mRelCount ? mRel[0] : 0));
  if (mAbsCount > 0 && mAbs[0] != NULL) {
    SbLog6 ("[input] abs range", mAbs[0]->Mode->AbsoluteMinX,
            mAbs[0]->Mode->AbsoluteMaxX, mAbs[0]->Mode->AbsoluteMinY,
            mAbs[0]->Mode->AbsoluteMaxY, mAbs[0]->Mode->Attributes, 0);
  }

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
    if (mRelBanMask & (1u << mRelActive)) {
      mRelActive = (mRelActive + 1) % mRelCount;   /* skip banned stubs  */
      continue;
    }
    do {
      INT32  Dx = 0;
      INT32  Dy = 0;

      Status = Sp->GetState (Sp, &State);
      if (Status == EFI_NOT_READY) {
        break;
      }
      if (EFI_ERROR (Status)) {
        // broken instance: try the next one on the next round
        mRelActive = (mRelActive + 1) % mRelCount;
        break;
      }

      Dx = State.RelativeMovementX;
      Dy = State.RelativeMovementY;
      if (((Dx > REL_BURST) || (Dx < -REL_BURST)) ||
          ((Dy > REL_BURST) || (Dy < -REL_BURST))) {
        /* PS/2 touchpad garbage: it flings the cursor into the corners.
         * Drop the report; persistent offenders get the instance banned. */
        Moved = TRUE;
        mLastRelTick = mPollTick;      /* keep the abs priority window */
        mRelBurst[mRelActive]++;
        if (mRelBurst[mRelActive] <= 4) {
          SbLog ("[input] rel burst", mRelActive, mRelBurst[mRelActive]);
        }
        if (mRelBurst[mRelActive] >= REL_BURSTS) {
          /* Banned for good: HID reports cap at 127 counts, a device
           * that streams >240 is the PS/2 touchpad's garbage.  If every
           * relative device is banned, the window goes quiet and the
           * touchscreen (absolute) takes over. */
          mRelBanMask |= 1u << mRelActive;
          mRelActive = (mRelActive + 1) % mRelCount;
        }
        Status = EFI_NOT_READY;        /* end this instance's drain loop */
        break;
      }

      if (Dx > REL_CLAMP) {
        Dx = REL_CLAMP;
      }
      if (Dx < -REL_CLAMP) {
        Dx = -REL_CLAMP;
      }
      if (Dy > REL_CLAMP) {
        Dy = REL_CLAMP;
      }
      if (Dy < -REL_CLAMP) {
        Dy = -REL_CLAMP;
      }

      if ((Dx != 0) || (Dy != 0)) {
        MoveCursorRelative (Dx, Dy);
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

  if (Moved || Type != EV_NONE) {
    mLastRelTick = mPollTick;
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

/*
 * Poll the active absolute device with the anti-teleport policy described
 * in the file header.  Coordinates are mapped from the device range into
 * pixels; a "parked" report (no touch, top corner) never moves the cursor.
 */
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
  BOOLEAN                        Touch;
  BOOLEAN                        Alt;

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
      /* some firmware pointer stacks go stale; a reset revives them */
      Ap->Reset (Ap, TRUE);
      mAbsActive = (mAbsActive + 1) % mAbsCount;
      continue;
    }

    Touch = (State.ActiveButtons & EFI_ABSP_TouchActive) != 0;
    Alt   = (State.ActiveButtons & EFI_ABS_AltActive) != 0;

    RangeX = Ap->Mode->AbsoluteMaxX - Ap->Mode->AbsoluteMinX;
    RangeY = Ap->Mode->AbsoluteMaxY - Ap->Mode->AbsoluteMinY;

    if (!Touch && !Alt) {
      INT64  PctX = RangeX != 0 ?
                    (INT64)(State.CurrentX - Ap->Mode->AbsoluteMinX) * 100 /
                    (INT64)RangeX : 0;
      INT64  PctY = RangeY != 0 ?
                    (INT64)(State.CurrentY - Ap->Mode->AbsoluteMinY) * 100 /
                    (INT64)RangeY : 0;

      /* the x360 panel parks reports in the top corners: drop them */
      if (PctY <= CORNER_PARK_PCT &&
          (PctX <= CORNER_PARK_PCT || PctX >= 100 - CORNER_PARK_PCT)) {
        mParkDrops++;
        if ((mParkDrops % 64) == 1) {
          SbLog6 ("[input] park drop", (UINTN)PctX, (UINTN)PctY,
                  State.CurrentX, State.CurrentY, mParkDrops, 0);
        }
        break;
      }
      /* while a real mouse is being used, ignore the touch panel */
      if (mRelCount > 0 && (mPollTick - mLastRelTick) < REL_PRIORITY_TICKS) {
        break;
      }
    }

    if (mAbsLogged < 40) {
      mAbsLogged++;
      SbLog6 ("[input] abs st", State.CurrentX, State.CurrentY,
              State.ActiveButtons, Touch, Alt, mAbsActive);
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

    if (Touch && !mTouchDown) {
      mTouchDown = TRUE;
      Type       = EV_LCLICK;
    } else if (Alt && !mAltDown) {
      mAltDown = TRUE;
      Type     = EV_RCLICK;
    } else if (Touch || Alt) {
      Type = EV_MOVE;
    } else {
      mTouchDown = FALSE;
      mAltDown   = FALSE;
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

  mPollTick++;
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
