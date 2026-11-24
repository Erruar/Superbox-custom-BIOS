# Launch OVMF + QEMU headless, boot the FAT image containing H2oSetup.efi,
# capture a screen dump and the serial log.  The QMP socket stays open for
# tools/qemu_probe.py to drive input.
#
# Usage:  tools\qemu_run.ps1 [-Stop]
param(
    [switch]$Stop
)

$Repo = Split-Path -Parent $PSScriptRoot
$Work = 'C:\dev\sbx\qemu'
$Qemu = 'C:\msys64\ucrt64\bin\qemu-system-x86_64.exe'
$OvmfCode = 'C:\msys64\ucrt64\share\qemu\edk2-x86_64-code.fd'
$OvmfVars = 'C:\msys64\ucrt64\share\qemu\edk2-i386-vars.fd'

if ($Stop) {
    Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" |
        Where-Object { $_.CommandLine -like '*sbx\qemu*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped pid $($_.ProcessId)" }
    exit 0
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null

if (-not (Test-Path "$Repo\build\H2oSetup.efi")) { throw 'no build\H2oSetup.efi, run tools\build.ps1 first' }
& python "$Repo\tools\mkfat.py" "$Work\efi.img" "$Repo\build\H2oSetup.efi"
if (-not (Test-Path "$Work\efi.img")) { throw 'mkfat failed' }

Copy-Item $OvmfCode "$Work\OVMF_CODE.fd" -Force
if (-not (Test-Path "$Work\OVMF_VARS.fd")) {
    Copy-Item $OvmfVars "$Work\OVMF_VARS.fd" -Force
}
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
    '-device', 'ich9-usb-ehci1,id=usb',
    '-device', 'ich9-usb-uhci1,masterbus=usb.0,firstport=0,multifunction=on',
    '-device', 'usb-tablet,bus=usb.0,port=1',
    '-serial', "file:$Work\serial.log",
    '-net', 'none',
    '-monitor', 'none',
    '-display', 'none',
    '-qmp', 'tcp:127.0.0.1:4545,server,nowait'
)

Start-Process -FilePath $Qemu -ArgumentList $args -WindowStyle Hidden
Write-Host "[qemu] running; QMP on 127.0.0.1:4545, serial at $Work\serial.log"
