#!/usr/bin/env bash
# edk2_build.sh - build CustomSetupPkg inside the space-free workspace.
#
# Called by tools/build.ps1 (which mirrors the sources) and usable directly:
#   C:\msys64\usr\bin\bash.exe -l tools/edk2_build.sh
set -e

MSYS2_BIN=/c/msys64/ucrt64/bin
PY=/c/Users/Sakura/AppData/Local/Programs/Python/Python312

export PATH="$MSYS2_BIN:$PY:$PY/Scripts:/usr/local/bin:/usr/bin:/bin:$PATH"
mkdir -p /usr/local/bin
# native (non-MSYS) processes cannot follow MSYS symlinks, and the edk2
# makefiles invoke plain "make" from native cmd, so install a real copy
[ -x /usr/local/bin/make.exe ] || cp -f "$MSYS2_BIN/mingw32-make.exe" /usr/local/bin/make.exe

# The build tools run under native Windows Python, so WORKSPACE and friends
# must be Windows paths (C:\dev\sbx), not MSYS paths (/c/dev/sbx).
WS_UNIX="${SBX_WS:-/c/dev/sbx}"
WS_WIN="$(cygpath -w "$WS_UNIX")"
MSYS_WIN="$(cygpath -w "$MSYS2_BIN")"

export WORKSPACE="$WS_WIN"
PY_WIN="$(cygpath -w "$PY")"
# ucrt64 also ships an MSYS python; the edk2 tools must run under native
# Windows python with Windows-style WORKSPACE/PACKAGES_PATH.
export PYTHON_COMMAND="$PY_WIN\\python.exe"
export PYTHON_HOME="$PY_WIN"
export PYTHONIOENCODING=utf-8
# module GNUmakefiles run commands through cmd.exe, so the WindowsLike
# (.bat) tool wrappers and the native Bin\Win64 tools must come first on PATH
export PATH="$WS_UNIX/edk2/BaseTools/Bin/Win64:$WS_UNIX/edk2/BaseTools/BinWrappers/WindowsLike:$PATH"
export BASE_TOOLS_PATH="$WS_WIN\\edk2\\BaseTools"
# current edk2 dropped the GCC5 tag; a mingw (PE-target) GCC cannot link the
# ELF output of the GCC toolchain, so use the native clang/LLVM flow
export CLANG_BIN="$MSYS_WIN\\"
export EDK_TOOLS_PATH="$WS_WIN\\edk2\\BaseTools"
export PACKAGES_PATH="$WS_WIN\\edk2;$WS_WIN"

cd "$WS_UNIX/edk2"

if [ ! -f BaseTools/Bin/Win64/tiano.exe ]; then
  echo "[edk2_build] building BaseTools"
  make -C BaseTools -j8 EXTRA_OPTFLAGS=-Wno-error > /tmp/basetools.log 2>&1 || {
    tail -n 25 /tmp/basetools.log
    exit 1
  }
fi

# stock build.py prints nothing when a build fails; make the tracebacks visible
"$PYTHON_COMMAND" "$(cygpath -u "$(dirname "$0")")/edk2_patch_buildpy.py" \
  BaseTools/Source/Python/build/build.py

# edksetup.sh rewrites WORKSPACE to a POSIX path; restore the Windows form,
# and pre-create Conf/ so it is not regenerated inside C:/msys64.
mkdir -p "$WS_UNIX/Conf"
source ./edksetup.sh > /dev/null 2>&1 || true
export WORKSPACE="$WS_WIN"

echo "[edk2_build] build -a X64 -t CLANGPDB -p CustomSetupPkg/CustomSetupPkg.dsc"
build -a X64 -t CLANGPDB -p CustomSetupPkg/CustomSetupPkg.dsc -b DEBUG "$@"
