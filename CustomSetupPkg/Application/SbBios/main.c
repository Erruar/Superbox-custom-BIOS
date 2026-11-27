/*
 * main.c - SbBios entry point and screen router.
 *
 * One application, two screens: the boot menu and the setup viewer.  The
 * transition between them is a state change - the boot menu used to
 * LoadImage the setup from the USB stick, which cost 10 seconds on the
 * box's firmware; in-process it costs one model scan (once) and a redraw.
 */

#include "SbBios.h"

#define FRAME_STALL_US  8000

/* one idle lap stalls 8 ms plus poll overhead (~10 ms); 100 laps ~= 2 s */
#define SENSOR_TICK_LAPS  100u

#define SCR_BOOT   0
#define SCR_SETUP  1

EFI_STATUS
EFIAPI
SbBiosMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  INPUT_EVENT   Event;
  INT32         Screen = SCR_BOOT;
  INT32         CursorX, CursorY;
  UINTN         Laps = 0;

  SbLogInit ();
  SbLog ("[sbios] enter", 0, 0);

  GopDiag ("before GfxInit");
  Status = GfxInit ();
  if (EFI_ERROR (Status)) {
    Print (L"SbBios: no usable graphics output (%r)\n", Status);
    return Status;
  }

  GopDiag ("after GfxInit");
  InputInit ();
  BootEnter ();
  InputGetCursor (&CursorX, &CursorY);
  GfxSetCursor (CursorX, CursorY, TRUE);
  GfxPresent ();

  for ( ; ;) {
    UINT32  Type = InputPoll (&Event);

    if (Type == EV_NONE) {
      gBS->Stall (FRAME_STALL_US);
      if (++Laps >= SENSOR_TICK_LAPS) {
        Laps = 0;
        if (Screen == SCR_SETUP) {
          /* clock + sensors keep moving while nobody touches the box */
          SetupTick ();
        } else {
          BootTick ();
        }
        InputGetCursor (&CursorX, &CursorY);
        GfxSetCursor (CursorX, CursorY, TRUE);
        GfxPresent ();
      }
      continue;
    }

    if (Type == EV_MOVE) {
      /* cursor-only update: cheap present, no full redraw */
      InputGetCursor (&CursorX, &CursorY);
      GfxSetCursor (CursorX, CursorY, TRUE);
      GfxPresentCursor ();
      continue;
    }

    Laps = 0;

    if (Screen == SCR_BOOT) {
      BootEvent (&Event);
      if (BootWantSetup ()) {
        Screen = SCR_SETUP;
        SetupEnter ();
        if (SetupWantBoot ()) {
          Screen = SCR_BOOT;
          GfxReapplyMode ();
          BootEnter ();
        }
      } else {
        BootDraw ();
      }
    } else {
      SetupEvent (&Event);
      if (SetupWantBoot ()) {
        Screen = SCR_BOOT;
        BootEnter ();
      } else {
        SetupDraw ();
      }
    }

    InputGetCursor (&CursorX, &CursorY);
    GfxSetCursor (CursorX, CursorY, TRUE);
    GfxPresent ();
  }
}
