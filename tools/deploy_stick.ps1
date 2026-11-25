# Deploy the built EFI app to a USB stick for hardware testing.
#
# Layout on the stick (FAT32):
#   \EFI\BOOT\BOOTX64.EFI    <- SbBios.efi (boot menu + setup, one app)
#   \SHELLX64.EFI            <- optional EFI shell for the "Diagnostika" entry
#
# Usage:  tools\deploy_stick.ps1 -Drive D
param(
    [Parameter(Mandatory = $true)][char]$Drive
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
$Root = "${Drive}:\"
if (-not (Test-Path $Root)) { throw "drive $Root not found" }

$Dir = Join-Path $Root 'EFI\BOOT'
New-Item -ItemType Directory -Force -Path $Dir | Out-Null

Copy-Item "$Repo\build\SbBios.efi" (Join-Path $Dir 'BOOTX64.EFI') -Force

# optional: an EFI shell for the diagnostics entry (root and EFI\BOOT)
foreach ($cand in "$env:USERPROFILE\Downloads\Shell.efi", "$Repo\tools\shellx64.efi") {
    if (Test-Path $cand) {
        Copy-Item $cand (Join-Path $Root 'SHELLX64.EFI') -Force
        Copy-Item $cand (Join-Path $Dir 'SHELLX64.EFI') -Force
        Write-Host "[deploy] shell from $cand"
        break
    }
}

# drop a previous field log so the new run is clean
Remove-Item (Join-Path $Root 'sbxlog.txt')  -EA SilentlyContinue
Remove-Item (Join-Path $Root 'sbxboot.txt') -EA SilentlyContinue

Write-Host "[deploy] staged:"
Get-ChildItem $Dir | Select-Object Name, Length, LastWriteTime
Write-Host "[deploy] after the run on the box, send sbxboot.txt / sbxlog.txt from the stick"
