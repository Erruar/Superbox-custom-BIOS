/*
 * gopdiag.c - dump every GOP in the system, its mode list and the EDID that
 * is attached to it.  Everything goes to sbxlog.txt via SbLogStr, so you can
 * boot once cold, once warm (after a Windows boot) and diff the two logs.
 *
 * What to look for in the diff:
 *   - number of GOP handles differs        -> second GOP appears / disappears
 *   - handle count same, MaxMode differs   -> same GOP, mode list built from
 *                                             a different (fallback) source
 *   - FrameBufferSize: cold shows 0x500000 (5 MiB = exactly 1280x1024*4)
 *     while warm shows 0x1FA4000 (4K*4): the mode list is gated by the
 *     video window the GPU believes it owns, not by the EDID (identical
 *     in both boots).  Compare CONFIG_MEMSIZE: if cold is ~5 MiB and warm
 *     hundreds, the UMA window is the filter.
 *
 * Call GopDiag ("tag") right after SbLogInit () and again after GfxInit ().
 */

#include "SbBios.h"
#include <Protocol/EdidDiscovered.h>
#include <Protocol/EdidActive.h>
#include <IndustryStandard/Acpi10.h>

/* set to 1 to try re-running the display driver binding when the mode list
 * looks like the fallback one.  Experimental: the text console may blink. */
#define GOPDIAG_RECONNECT  0

STATIC
VOID
LogFmt (
  IN CONST CHAR8  *Fmt,
  ...
  )
{
  CHAR8    Line[256];
  VA_LIST  Args;

  VA_START (Args, Fmt);
  AsciiVSPrint (Line, sizeof (Line), Fmt, Args);
  VA_END (Args);
  AsciiStrCatS (Line, sizeof (Line), "\r\n");
  SbLogStr (Line);
}

STATIC
VOID
DumpPath (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;
  CHAR16                    *Text;
  CHAR8                     Ascii[200];

  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid,
                                      (VOID **)&Dp)) || (Dp == NULL)) {
    LogFmt ("    path: <none>");
    return;
  }
  Text = ConvertDevicePathToText (Dp, FALSE, FALSE);
  if (Text == NULL) {
    LogFmt ("    path: <convert failed>");
    return;
  }
  UnicodeStrToAsciiStrS (Text, Ascii, sizeof (Ascii));
  LogFmt ("    path: %a", Ascii);
  FreePool (Text);
}

STATIC
VOID
DumpEdidBlob (
  IN CONST CHAR8  *Name,
  IN UINT32       Size,
  IN UINT8        *E
  )
{
  UINT32  Hact, Vact, Clk, Index;
  CHAR8   Hex[3 * 16 + 1];

  if ((E == NULL) || (Size == 0)) {
    LogFmt ("    %a: absent (size %u)", Name, Size);
    return;
  }
  if (Size < 128) {
    LogFmt ("    %a: short blob, %u bytes", Name, Size);
    return;
  }

  for (Index = 0; Index < 16; Index++) {
    AsciiSPrint (&Hex[Index * 3], 4, "%02x ", E[Index]);
  }
  LogFmt ("    %a: %u bytes, ext blocks %u, head %a", Name, Size, E[126], Hex);

  /* first detailed timing descriptor = preferred mode */
  Clk  = (UINT32)(E[54] | (E[55] << 8));
  Hact = (UINT32)(E[56] | ((E[58] & 0xF0) << 4));
  Vact = (UINT32)(E[59] | ((E[61] & 0xF0) << 4));
  LogFmt ("    %a: preferred DTD %ux%u pclk %u kHz", Name, Hact, Vact, Clk * 10);

  /*
   * Full base block (and the CEA extension when present): standard
   * timings sit at 0x24..0x34, the descriptors at 0x48..0x7F.  Needed
   * to prove the cold and warm EDIDs match byte for byte.
   */
  for (Index = 0; Index < Size; Index += 16) {
    UINTN  Count16 = ((UINTN)Size - Index) > 16 ? 16 : ((UINTN)Size - Index);
    UINTN  J;

    for (J = 0; J < Count16; J++) {
      AsciiSPrint (&Hex[J * 3], 4, "%02x ", E[Index + J]);
    }
    Hex[Count16 * 3] = 0;
    LogFmt ("    %a: %03x: %a", Name, Index, Hex);
  }
}

STATIC
VOID
DumpModes (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop
  )
{
  UINT32  Index;

  LogFmt ("    modes: max %u, current %u, fb 0x%lx size 0x%lx",
          Gop->Mode->MaxMode, Gop->Mode->Mode,
          (UINT64)Gop->Mode->FrameBufferBase,
          (UINT64)Gop->Mode->FrameBufferSize);

  for (Index = 0; Index < Gop->Mode->MaxMode; Index++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info = NULL;
    UINTN                                 Size  = 0;

    if (EFI_ERROR (Gop->QueryMode (Gop, Index, &Size, &Info)) || (Info == NULL)) {
      LogFmt ("      #%u: query failed", Index);
      continue;
    }
    LogFmt ("      #%u: %ux%u fmt %u pitch %u", Index,
            Info->HorizontalResolution, Info->VerticalResolution,
            (UINT32)Info->PixelFormat, Info->PixelsPerScanLine);
    FreePool (Info);
  }
}

STATIC
VOID
DumpPciOwner (
  IN EFI_HANDLE  Handle
  )
{
  EFI_PCI_IO_PROTOCOL  *Pci = NULL;
  UINTN                Seg, Bus, Dev, Fn;
  UINT16               Ven = 0xFFFF, Did = 0xFFFF;

  /* the GOP handle is normally the PCI IO handle of the GPU itself */
  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiPciIoProtocolGuid,
                                      (VOID **)&Pci)) || (Pci == NULL)) {
    LogFmt ("    pci: GOP handle has no PciIo (child/virtual handle)");
    return;
  }
  Pci->GetLocation (Pci, &Seg, &Bus, &Dev, &Fn);
  Pci->Pci.Read (Pci, EfiPciIoWidthUint16, 0, 1, &Ven);
  Pci->Pci.Read (Pci, EfiPciIoWidthUint16, 2, 1, &Did);
  LogFmt ("    pci: %02x:%02x.%x  %04x:%04x", Bus, Dev, Fn, Ven, Did);
}

/*
 * iGPU register probe: the cold-boot mode list stops exactly where a
 * 5 MiB framebuffer ends, so ask the GPU how much video window it
 * thinks it has.  CONFIG_MEMSIZE is GFX8 register 0x150A (dword index),
 * byte offset 0x5428 in the MMIO BAR; amdgpu uses BAR5 on APUs.  The
 * value decodes as MiB in bits 21:12.  All reads, no writes.
 */
STATIC
VOID
GpuDiag (
  VOID
  )
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0, Index;
  UINTN       Bar;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return;
  }
  for (Index = 0; Index < Count; Index++) {
    EFI_PCI_IO_PROTOCOL  *Pci = NULL;
    UINT32               Id = 0, Cmd = 0, MemSize = 0;
    UINT8                Class[3];

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid,
                                        (VOID **)&Pci)) || (Pci == NULL)) {
      continue;
    }
    Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id);
    Pci->Pci.Read (Pci, EfiPciIoWidthUint8, 9, 3, Class);
    if ((Class[2] != 0x03) || ((Id & 0xFFFF) != 0x1002)) {
      continue;                   /* not an AMD display controller */
    }

    Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 4, 1, &Cmd);
    LogFmt ("  gpu %04x:%04x command 0x%08x (mem-enable %u)",
            Id & 0xFFFF, Id >> 16, Cmd, (Cmd & BIT1) ? 1 : 0);

    for (Bar = 0; Bar < 6; Bar++) {
      VOID  *Res = NULL;

      if (EFI_ERROR (Pci->GetBarAttributes (Pci, (UINT8)Bar, NULL, &Res)) ||
          (Res == NULL)) {
        continue;
      }
      {
        EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *D = Res;

        LogFmt ("  gpu bar%u: %a base 0x%lx len 0x%lx", Bar,
                (D->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) ? "mem" :
                (D->ResType == ACPI_ADDRESS_SPACE_TYPE_IO)  ? "io"  : "bus",
                D->AddrRangeMin, D->AddrLen);
      }
      FreePool (Res);
    }

    if (!EFI_ERROR (Pci->Mem.Read (Pci, EfiPciIoWidthUint32, 5, 0x5428,
                                   1, &MemSize))) {
      LogFmt ("  gpu CONFIG_MEMSIZE 0x%08x = %u MiB",
              MemSize, (MemSize >> 12) & 0x3F);
    } else {
      LogFmt ("  gpu CONFIG_MEMSIZE read failed");
    }
  }
  FreePool (Handles);
}

VOID
GopDiag (
  IN CONST CHAR8  *Tag
  )
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0, Index;
  EFI_STATUS  Status;

  LogFmt ("==== GopDiag [%a] ====", Tag);

  /* 1. all GOP instances, not just the first one LocateProtocol returns */
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                    NULL, &Count, &Handles);
  LogFmt ("GOP handles: %u (%r)", Count, Status);

  for (Index = 0; !EFI_ERROR (Status) && (Index < Count); Index++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;
    EFI_EDID_DISCOVERED_PROTOCOL    *Disc = NULL;
    EFI_EDID_ACTIVE_PROTOCOL        *Act  = NULL;

    LogFmt ("  [%u] handle %p", Index, Handles[Index]);
    DumpPath (Handles[Index]);
    DumpPciOwner (Handles[Index]);

    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index],
          &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) &&
        (Gop != NULL) && (Gop->Mode != NULL)) {
      {
        EFI_GRAPHICS_OUTPUT_PROTOCOL  *First = NULL;

        gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                             (VOID **)&First);
        LogFmt ("    LocateProtocol picks this one: %a",
                (First == Gop) ? "YES" : "no");
      }
      DumpModes (Gop);
    }

    /* 2. EDID the driver itself saw, and EDID it actually applied */
    gBS->HandleProtocol (Handles[Index], &gEfiEdidDiscoveredProtocolGuid,
                         (VOID **)&Disc);
    gBS->HandleProtocol (Handles[Index], &gEfiEdidActiveProtocolGuid,
                         (VOID **)&Act);
    DumpEdidBlob ("EdidDiscovered",
                  Disc ? Disc->SizeOfEdid : 0, Disc ? Disc->Edid : NULL);
    DumpEdidBlob ("EdidActive",
                  Act ? Act->SizeOfEdid : 0, Act ? Act->Edid : NULL);
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  /* 3. EDID protocols that live on other handles (child / connector nodes) */
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiEdidDiscoveredProtocolGuid,
                                     NULL, &Count, &Handles);
  LogFmt ("EdidDiscovered handles: %u (%r)", Count, Status);
  for (Index = 0; !EFI_ERROR (Status) && (Index < Count); Index++) {
    EFI_EDID_DISCOVERED_PROTOCOL  *Disc = NULL;

    LogFmt ("  edid owner %p", Handles[Index]);
    DumpPath (Handles[Index]);
    gBS->HandleProtocol (Handles[Index], &gEfiEdidDiscoveredProtocolGuid,
                         (VOID **)&Disc);
    DumpEdidBlob ("EdidDiscovered",
                  Disc ? Disc->SizeOfEdid : 0, Disc ? Disc->Edid : NULL);
  }
  if (Handles != NULL) {
    FreePool (Handles);
  }

  /* 4. AMD iGPU: BAR sizes and the configured video window */
  GpuDiag ();

  /* 5. ConOut GOP, so we can tell whether text console and LocateProtocol
   *    agree */
  LogFmt ("ConOut handle: %p", gST->ConsoleOutHandle);
  LogFmt ("==== end [%a] ====", Tag);
}

/*
 * Experimental: re-run the display driver binding on every PCI display
 * controller so it re-reads EDID.  Only useful if the log shows that the
 * EDID is missing at cold boot but a plain re-read works.
 */
VOID
GopDiagReconnect (
  VOID
  )
{
#if GOPDIAG_RECONNECT
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0, Index;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return;
  }
  for (Index = 0; Index < Count; Index++) {
    EFI_PCI_IO_PROTOCOL  *Pci = NULL;
    UINT8                Class[3];

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid,
                                        (VOID **)&Pci)) || (Pci == NULL)) {
      continue;
    }
    Pci->Pci.Read (Pci, EfiPciIoWidthUint8, 9, 3, Class);
    if (Class[2] != 0x03) {          /* base class 3 = display controller */
      continue;
    }
    LogFmt ("reconnect display controller handle %p", Handles[Index]);
    gBS->DisconnectController (Handles[Index], NULL, NULL);
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }
  FreePool (Handles);
#endif
}
