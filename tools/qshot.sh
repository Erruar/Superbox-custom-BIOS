#!/usr/bin/env bash
# qshot.sh - screendump + PNG conversion: bash qshot.sh name
SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="${2:-/c/Users/Sakura/AppData/Local/Temp/opencode}"
N="${1:-shot}"
"$SRC/qprobe.sh" dump "$OUT/$N.ppm" > /dev/null
"$SRC/pysh.sh" "$SRC/ppm2png.py" "$OUT/$N.ppm"
