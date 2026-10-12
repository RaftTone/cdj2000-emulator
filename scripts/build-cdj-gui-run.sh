#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build cdj-gui-run, the GUI board on the vendored Blackfin core in
# emulator/bfin/ (from Stijn Jacobs' cdj-nxs2-qemu; see THIRD_PARTY.md), and
# install it as bin/cdj-gui-run.  GUI-only for now: no MAIN link.
#
#   sh scripts/build-cdj-gui-run.sh
#
# Objects go to $CDJ_BUILD_DIR/cdj-gui-run (default build/).  CC and CFLAGS
# are passed through to make; the default is -O3 -g.

set -e

# Windows builds (MSYS2 MINGW64) get an .exe suffix, and MSYS2's shell hands
# GCC no TMP; see scripts/build-qemu-sh4.sh.
case $(uname -s 2>/dev/null) in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
    *)                    EXE= ;;
esac
if [ "$EXE" = .exe ] && [ -z "$TMP" ]; then
    TMP=$(cygpath -w "${TMPDIR:-/tmp}" 2>/dev/null) || TMP='C:\Windows\Temp'
    TEMP=$TMP
    export TMP TEMP
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${CDJ_BUILD_DIR:-$REPO/build}
BIN=${CDJ_BIN_DIR:-$REPO/bin}

make -C "$REPO/emulator/bfin" O="$WORK/cdj-gui-run" ${CC:+CC="$CC"} \
    ${CFLAGS:+CFLAGS="$CFLAGS"}
mkdir -p "$BIN"
# A new file, not an overwrite: macOS kills a signed binary rewritten in place.
rm -f "$BIN/cdj-gui-run$EXE"
cp "$WORK/cdj-gui-run/cdj-gui-run$EXE" "$BIN/cdj-gui-run$EXE"
echo "installed $BIN/cdj-gui-run$EXE"
