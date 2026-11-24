/*
 * SbPointerInfo - tiny firmware pointer inspector (test harness helper).
 *
 * Enumerates every EFI_SIMPLE_POINTER_PROTOCOL and
 * EFI_ABSOLUTE_POINTER_PROTOCOL instance in the platform, prints the device
 * path node types, the drivers that claim the handle, and a GetState probe.
 * Used to understand QEMU/OVMF pointer stacks; the same walk later helps the
 * setup UI pick a working device.
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/SimplePointer.h>

STATIC
VOID
DumpPath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp
  )
{
  UINTN  Guard = 0;

  while (!IsDevicePathEnd (Dp) && (Guard++ < 10)) {
    Print (L"%04x ", (DevicePathType (Dp) << 8) | DevicePathSubType (Dp));
    Dp = NextDevicePathNode (Dp);
  }
}

/* Ask every driver binding in the platform whether it owns the handle. */
STATIC
VOID
DescribeDrivers (
  IN EFI_HANDLE  Child
  )
{
  EFI_HANDLE                  *Handles;
  EFI_STATUS                  Status;
  UINTN                       Index, Count;
  EFI_DRIVER_BINDING_PROTOCOL *Binding;
  EFI_COMPONENT_NAME2_PROTOCOL *Cn2;
  EFI_COMPONENT_NAME_PROTOCOL *Cn;
  CHAR16                      *Wide;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol, &gEfiDriverBindingProtocolGuid, NULL,
                   &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"       no driver bindings in platform\n");
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Binding = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                       &gEfiDriverBindingProtocolGuid, (VOID **)&Binding)) ||
        (Binding == NULL)) {
      continue;
    }
    if (EFI_ERROR (Binding->Supported (Binding, Child, NULL))) {
      continue;
    }

    Wide  = NULL;
    Cn2   = NULL;
    Cn    = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
           &gEfiComponentName2ProtocolGuid, (VOID **)&Cn2)) && (Cn2 != NULL)) {
      Cn2->GetDriverName (Cn2, "eng", &Wide);
    } else if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
           &gEfiComponentNameProtocolGuid, (VOID **)&Cn)) && (Cn != NULL)) {
      Cn->GetDriverName (Cn, "eng", &Wide);
    }

    if (Wide != NULL) {
      Print (L"       supported by: %s\n", Wide);
    } else {
      Print (L"       supported by binding %p\n", Handles[Index]);
    }
  }
  FreePool (Handles);
}

STATIC
VOID
Probe (
  IN EFI_GUID      *Guid,
  IN CONST CHAR8   *Tag
  )
{
  EFI_HANDLE    *Handles;
  EFI_STATUS    Status;
  UINTN         Index, Count;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
  Print (L"[sbpointer] %a: %u instance(s) status=%r\n", Tag, Count, Status);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Print (L"  [%u] handle %p path ", Index, Handles[Index]);
    Dp = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                       &gEfiDevicePathProtocolGuid, (VOID **)&Dp)) &&
        (Dp != NULL)) {
      DumpPath (Dp);
    } else {
      Print (L"(none)");
    }
    Print (L"\n");

    DescribeDrivers (Handles[Index]);

    if (CompareGuid (Guid, &gEfiAbsolutePointerProtocolGuid)) {
      EFI_ABSOLUTE_POINTER_PROTOCOL  *Ap = NULL;

      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], Guid, (VOID **)&Ap)) &&
          (Ap != NULL)) {
        EFI_ABSOLUTE_POINTER_STATE  St;

        Print (L"       abs %p range %Ld..%Ld / %Ld..%Ld getstate=%r\n",
               Ap,
               Ap->Mode->AbsoluteMinX, Ap->Mode->AbsoluteMaxX,
               Ap->Mode->AbsoluteMinY, Ap->Mode->AbsoluteMaxY,
               Ap->GetState (Ap, &St));
      }
    } else {
      EFI_SIMPLE_POINTER_PROTOCOL  *Sp = NULL;

      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], Guid, (VOID **)&Sp)) &&
          (Sp != NULL)) {
        EFI_SIMPLE_POINTER_STATE  St;

        Print (L"       rel res %Ld..%Ld getstate=%r\n",
               Sp->Mode->ResolutionX, Sp->Mode->ResolutionY,
               Sp->GetState (Sp, &St));
      }
    }
  }
  FreePool (Handles);
}

EFI_STATUS
EFIAPI
SbPointerInfoMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  Print (L"[sbpointer] start\n");
  Probe (&gEfiAbsolutePointerProtocolGuid, "abs");
  Probe (&gEfiSimplePointerProtocolGuid, "rel");
  SerialPortWrite ((UINT8 *)"[sbpointer] done\r\n", 20);
  return EFI_SUCCESS;
}
