/*
 * main.c - entry point and event loop of the custom setup UI.
 *
 * The application is deliberately standalone: it consumes the stock HII
 * database and GOP, and never calls the Insyde display engine.  That keeps the
 * QEMU prototype and the eventual injected module identical.
 */

#include "H2oSetup.h"

#define FRAME_STALLMicroseconds 8000
#define PAD_TEXT()              ((INT32)(8 * GfxScale ()))

EFI_STATUS
EFIAPI
H2oSetupMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  INPUT_EVENT   Event;
  FORMSET       *Fs;
  BOOLEAN       Redraw = TRUE;
  INT32         CursorX, CursorY;
  UINTN         Index;

  SbLog ("[h2osetup] enter", 0, 0);

  Status = GfxInit ();
  if (EFI_ERROR (Status)) {
    Print (L"H2oSetup: no usable graphics output (%r)\n", Status);
    return Status;
  }

  InputInit ();

  Status = ModelInit ();
  {
    UINTN  Count = 0;
    FORMSET  *F;

    for (F = ModelFormsets (); F != NULL; F = F->Next) {
      Count++;
    }
    SbLog ("[h2osetup] gfx", GfxWidth (), GfxHeight ());
    SbLog ("[h2osetup] formsets", Count, (UINTN)Status);
  }
  if (EFI_ERROR (Status) || (ModelFormsets () == NULL)) {
    GfxClear (COL_BG);
    GfxText (PAD_TEXT (), PAD_TEXT (),
             L"H2oSetup: the HII database published no formsets.",
             COL_TEXT, (INT32)GfxWidth () - 2 * PAD_TEXT ());
    GfxText (PAD_TEXT (), PAD_TEXT () * 3,
             L"Press any key to exit.", COL_TEXT_DIM,
             (INT32)GfxWidth () - 2 * PAD_TEXT ());
    GfxPresent ();

    Index = 0;
    while (Index++ < 1000) {
      if (InputPoll (&Event) == EV_KEY) {
        break;
      }
      gBS->Stall (50000);
    }
    GfxExit ();
    return EFI_NOT_FOUND;
  }

  Fs = ModelFormsets ();
  Status = UiInit (Fs);
  if (EFI_ERROR (Status)) {
    GfxExit ();
    return Status;
  }

  UiDraw ();
  GfxPresent ();

  while (!UiQuitRequested ()) {
    UINT32  Type = InputPoll (&Event);

    if (Type == EV_NONE) {
      gBS->Stall (FRAME_STALLMicroseconds);
      continue;
    }

    if (Type == EV_MOVE) {
      Redraw = TRUE;                 /* cursor moved, UI state unchanged */
    } else {
      UiEvent (&Event);
      Redraw = TRUE;
    }

    InputGetCursor (&CursorX, &CursorY);
    GfxSetCursor (CursorX, CursorY, TRUE);

    if (Redraw) {
      UiDraw ();
      GfxPresent ();
      Redraw = TRUE;                 /* next move event needs a present too */
    }
  }

  /* Persist every formset touched during the session. */
  for (Fs = ModelFormsets (); Fs != NULL; Fs = Fs->Next) {
    ModelFlush (Fs);
  }

  ModelExit ();
  InputExit ();
  GfxExit ();

  return EFI_SUCCESS;
}
