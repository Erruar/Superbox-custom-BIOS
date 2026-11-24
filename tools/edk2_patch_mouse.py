#!/usr/bin/env python
"""edk2_patch_mouse.py - add serial diagnostics to the stock USB mouse drivers.

The workspace copy of MdeModulePkg's USB mouse drivers (built by our DSC for
the QEMU harness) reports only "EFI_DEVICE_ERROR" through the abs pointer
protocol.  This patch prints the raw USB transfer result codes in the
interrupt completion callbacks so the QEMU emulation failure can be pinned
down.  Applied to the disposable workspace mirror on every build, so it
must be idempotent.
"""
import sys
from pathlib import Path

MARK = "[sbx-mouse]"

FILES = [
    "MdeModulePkg/Bus/Usb/UsbMouseAbsolutePointerDxe/UsbMouseAbsolutePointer.c",
    "MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouse.c",
]

# unique anchors: the error branch of each driver's interrupt callback
ANCHORS = [
    ("""  if (Result != EFI_USB_NOERROR) {
    //
    // Some errors happen during the process
    //
    REPORT_STATUS_CODE_WITH_DEVICE_PATH (
      EFI_ERROR_CODE | EFI_ERROR_MINOR,
      (EFI_PERIPHERAL_MOUSE | EFI_P_EC_INPUT_ERROR),
      UsbMouseAbsolutePointerDevice->DevicePath""",
     """  if (Result != EFI_USB_NOERROR) {
    DEBUG ((DEBUG_INFO, "[sbx-mouse] abs int result=%x len=%x\\n", Result, DataLength));
    //
    // Some errors happen during the process
    //
    REPORT_STATUS_CODE_WITH_DEVICE_PATH (
      EFI_ERROR_CODE | EFI_ERROR_MINOR,
      (EFI_PERIPHERAL_MOUSE | EFI_P_EC_INPUT_ERROR),
      UsbMouseAbsolutePointerDevice->DevicePath"""),
]


def patch(ws: str) -> int:
    root = Path(ws)
    touched = 0
    for rel in FILES:
        f = root / rel
        if not f.exists():
            print(f"[sbx-mouse] missing {f}")
            continue
        s = f.read_text(encoding="utf-8")
        if MARK in s:
            continue
        for old, new in ANCHORS:
            if old in s:
                s = s.replace(old, new, 1)
                touched += 1
        f.write_text(s, encoding="utf-8")
    print(f"[sbx-mouse] patched {touched} site(s)")
    return 0


if __name__ == "__main__":
    sys.exit(patch(sys.argv[1]))
