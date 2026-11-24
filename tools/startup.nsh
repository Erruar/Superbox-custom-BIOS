@echo -off
echo [startup] loading pointer drivers
load fs0:\EFI\BOOT\MOUSEDXE.EFI
load fs0:\EFI\BOOT\ABSDXEDXE.EFI
echo [startup] launching setup UI
fs0:\EFI\BOOT\BOOTX64.EFI
echo [startup] setup UI exited
