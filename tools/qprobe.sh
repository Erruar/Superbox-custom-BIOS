#!/usr/bin/env bash
# qprobe.sh - thin wrapper so probing from PowerShell/MSYS keeps quoting sane
export PATH="/c/Users/Sakura/AppData/Local/Programs/Python/Python312:/c/msys64/ucrt64/bin:$PATH"
exec python "C:/Users/Sakura/CLionProjects/Superbox custom BIOS/tools/qemu_probe.py" "$@"
