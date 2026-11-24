#!/usr/bin/env bash
# qcmds.sh - clear the shell line, then run each argument as a shell command.
#   bash qcmds.sh 'cd fs0:\EFI\BOOT' 'dir'
# Trailing numeric argument is the final wait in seconds (default 5).
SRC="$(cd "$(dirname "$0")" && pwd)"
Q="$SRC/qprobe.sh"
WAIT=5
LAST="${@: -1}"
if [[ "$LAST" =~ ^[0-9]+$ ]]; then
  WAIT="$LAST"
  set -- "${@:1:$#-1}"
fi
"$Q" key ctrl+c > /dev/null; "$Q" key ret > /dev/null; sleep 1
for c in "$@"; do
  "$Q" text "$c" > /dev/null
  "$Q" key ret > /dev/null
  sleep 2
done
sleep "$WAIT"
"$Q" serial | sed 's/\x1b\[[0-9;=?]*[a-zA-Z]//g' | grep -av '^\s*$' | tail -n 16
