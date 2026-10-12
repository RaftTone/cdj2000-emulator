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

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${CDJ_BUILD_DIR:-$REPO/build}
BIN=${CDJ_BIN_DIR:-$REPO/bin}

make -C "$REPO/emulator/bfin" O="$WORK/cdj-gui-run" ${CC:+CC="$CC"} \
    ${CFLAGS:+CFLAGS="$CFLAGS"}
mkdir -p "$BIN"
# A new file, not an overwrite: macOS kills a signed binary rewritten in place.
rm -f "$BIN/cdj-gui-run"
cp "$WORK/cdj-gui-run/cdj-gui-run" "$BIN/cdj-gui-run"
echo "installed $BIN/cdj-gui-run"
