#!/usr/bin/env bash
# qkeys.sh - send key events by name: bash qkeys.sh tab down down ret
SRC="$(cd "$(dirname "$0")" && pwd)"
Q="$SRC/qprobe.sh"
for k in "$@"; do
  "$Q" key "$k" > /dev/null
  sleep 0.4
done
