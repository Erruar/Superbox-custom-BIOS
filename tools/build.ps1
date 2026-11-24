# Build the custom setup application in a space-free EDK2 workspace.
#
# EDK2 makefiles cannot handle paths with spaces, so the sources are mirrored
# into $Workspace (default C:\dev\sbx) and the produced .efi is copied back
# into the repository under build\.
#
# Usage:  tools\build.ps1 [-Clean] [-Verbose]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$Repo      = Split-Path -Parent $PSScriptRoot
$Workspace = if ($env:SBX_WS) { $env:SBX_WS } else { 'C:\dev\sbx' }
$Bash      = 'C:\msys64\usr\bin\bash.exe'

Write-Host "[build] workspace: $Workspace"

New-Item -ItemType Directory -Force -Path $Workspace | Out-Null

Write-Host '[build] mirroring edk2 (incremental)'
robocopy "$Repo\vendor\edk2" "$Workspace\edk2" /E /XJ /XD .git EmulatorPkg /XF .git /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw "edk2 mirror failed: $LASTEXITCODE" }

Write-Host '[build] mirroring CustomSetupPkg'
robocopy "$Repo\CustomSetupPkg" "$Workspace\CustomSetupPkg" /MIR /XJ /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw "package mirror failed: $LASTEXITCODE" }

# Build dir is regenerated on every run: keep it out of the mirror.
$BuildDir = Join-Path $Workspace 'Build'
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host '[build] cleaning Build/'
    Remove-Item -Recurse -Force $BuildDir
}

& $Bash -lc "'$Repo/tools/edk2_build.sh'"
if ($LASTEXITCODE -ne 0) {
    Write-Host '[build] build failed'
    exit 1
}

$Out = Get-ChildItem "$Workspace\Build" -Recurse -Filter 'H2oSetup.efi' -EA SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($Out) {
    New-Item -ItemType Directory -Force -Path "$Repo\build" | Out-Null
    Copy-Item $Out.FullName "$Repo\build\H2oSetup.efi" -Force
    $Size = (Get-Item "$Repo\build\H2oSetup.efi").Length
    Write-Host "[build] OK: build\H2oSetup.efi ($Size bytes)"
} else {
    Write-Host '[build] no H2oSetup.efi produced - see the build log above'
    exit 1
}

# QEMU helper drivers (the stock OVMF image has no USB mouse support)
foreach ($name in 'UsbMouseDxe.efi', 'UsbMouseAbsolutePointerDxe.efi', 'SbPointerInfo.efi', 'VirtioInputDxe.efi') {
    $Drv = Get-ChildItem "$Workspace\Build" -Recurse -Filter $name -EA SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($Drv) {
        Copy-Item $Drv.FullName "$Repo\build\$name" -Force
        Write-Host "[build] OK: build\$name"
    }
}
