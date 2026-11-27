# One-shot QEMU cycle with the 8 GiB stick: rebuild image, fresh serial,
# launch. Mirrors tools/qemu_run.ps1 exactly, plus disk1.
param(
    [switch]$Stop
)

$Repo = 'C:\Users\Sakura\CLionProjects\Superbox custom BIOS'
$Work = 'C:\dev\sbx\qemu'
$Qemu = 'C:\msys64\ucrt64\bin\qemu-system-x86_64.exe'

Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" |
    Where-Object { $_.CommandLine -like '*sbx\qemu*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped pid $($_.ProcessId)" }
if ($Stop) { exit 0 }

Copy-Item "$Repo\build\SbBios.efi" "$Work\SbBios.efi" -Force
$env:MKFAT_NSH = "$Repo\tools\startup.nsh"
Start-Sleep -Seconds 1
& python "$Repo\tools\mkfat.py" "$Work\efi.img" `
    "$Repo\build\SbBios.efi=BOOTX64.EFI" `
    "$Repo\build\UsbMouseDxe.efi=MOUSEDXE.EFI" `
    "$Repo\build\UsbMouseAbsolutePointerDxe.efi=ABSDXE.EFI" `
    "$Repo\build\SbPointerInfo.efi=SBPINFO.EFI" `
    "$Repo\build\VirtioInputDxe.efi=VIRTIN.EFI"
if (-not (Test-Path "$Work\efi.img")) { throw 'mkfat failed' }
Remove-Item "$Work\serial.log" -EA SilentlyContinue

$args = @(
    '-machine', 'q35',
    '-cpu', 'max',
    '-m', '2048',
    '-smp', '2',
    '-drive', "if=pflash,format=raw,readonly=on,file=$Work\OVMF_CODE.fd",
    '-drive', "if=pflash,format=raw,file=$Work\OVMF_VARS.fd",
    '-drive', "format=raw,if=none,id=disk0,file=$Work\efi.img",
    '-device', 'ich9-ahci,id=ahci',
    '-device', 'ide-hd,bus=ahci.0,drive=disk0',
    '-drive', "format=raw,if=none,id=disk1,file=$Work\stick.img",
    '-device', 'ide-hd,bus=ahci.1,drive=disk1',
    '-device', 'ich9-usb-ehci1,id=usb',
    '-device', 'ich9-usb-uhci1,masterbus=usb.0,firstport=2,multifunction=on',
    '-device', 'ich9-usb-uhci1,masterbus=usb.0,firstport=4,multifunction=on',
    '-device', 'usb-tablet,bus=usb.0,port=2',
    '-device', 'virtio-tablet-pci',
    '-serial', "file:$Work\serial.log",
    '-net', 'none',
    '-monitor', 'none',
    '-display', 'none',
    '-qmp', 'tcp:127.0.0.1:4545,server,nowait'
)

Start-Process -FilePath $Qemu -ArgumentList $args -WindowStyle Hidden
Start-Sleep -Seconds 2
$proc = Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" |
    Where-Object { $_.CommandLine -like '*sbx\qemu*' } |
    Select-Object -First 1
if (-not $proc) {
    Write-Host '[qemu] failed to start'
    exit 1
}
Write-Host "[qemu] pid $($proc.ProcessId) running with stick.img"
