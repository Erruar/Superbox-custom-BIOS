#!/usr/bin/env bash
# pysh.sh - run Windows python with a stable PATH from MSYS bash.
#   bash pysh.sh script.py [args...]
export PATH="/c/Users/Sakura/AppData/Local/Programs/Python/Python312:/c/msys64/ucrt64/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
exec python "$@"
