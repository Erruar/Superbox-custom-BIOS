#!/usr/bin/env bash
# qexec.sh - type a command into the OVMF EFI Shell and show the serial echo.
#   bash qexec.sh 'dir fs0:\EFI\BOOT' [wait-seconds]
SRC="$(cd "$(dirname "$0")" && pwd)"
Q="$SRC/qprobe.sh"
"$Q" key ctrl+c > /dev/null
"$Q" key ret > /dev/null
sleep 1
"$Q" text "$1" > /dev/null
"$Q" key ret > /dev/null
sleep "${2:-6}"
"$Q" serial | sed 's/\x1b\[[0-9;=?]*[a-zA-Z]//g' | grep -av '^\s*$' | tail -n 14
