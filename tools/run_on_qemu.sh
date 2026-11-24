#!/usr/bin/env bash
# run_on_qemu.sh - type a shell command into the OVMF EFI Shell, wait, dump screen
SRC="$(cd "$(dirname "$0")" && pwd)"
Q="$SRC/qprobe.sh"
P="$SRC/pysh.sh"
OUT="${3:-/c/Users/Sakura/AppData/Local/Temp/opencode}"
N="${4:-shot}"
sleep 1
"$Q" text "$1" > /dev/null
"$Q" key ret > /dev/null
sleep "${2:-8}"
"$Q" dump "$OUT/$N.ppm" > /dev/null
"$P" "$SRC/ppm2png.py" "$OUT/$N.ppm"
