@echo -off
echo [startup] pointer inventory
fs0:\EFI\BOOT\SBPINFO.EFI
pause
echo [startup] loading pointer drivers
load fs0:\EFI\BOOT\MOUSEDXE.EFI
load fs0:\EFI\BOOT\ABSDXE.EFI
load fs0:\EFI\BOOT\VIRTIN.EFI
echo [startup] connecting controllers
connect -r
echo [startup] pointer inventory after drivers
fs0:\EFI\BOOT\SBPINFO.EFI
pause
echo [startup] launching setup UI
fs0:\EFI\BOOT\BOOTX64.EFI
echo [startup] setup UI exited
