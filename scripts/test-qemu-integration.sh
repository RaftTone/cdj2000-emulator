#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the custom board and run firmware-free, actual-QEMU regressions.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
QEMU_SRC=${QEMU_SRC:-"$REPO/build/qemu"}
PYTHON=${PYTHON:-python3}

sh "$REPO/scripts/build-qemu-sh4.sh" "$QEMU_SRC"
case $(uname -s 2>/dev/null) in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
    *)                    EXE= ;;
esac
QEMU_BINARY="$QEMU_SRC/build/qemu-system-sh4$EXE"
test -x "$QEMU_BINARY"
cd "$REPO"
CDJ_QEMU="$QEMU_BINARY" CDJ_TEST_QEMU="$QEMU_BINARY" "$PYTHON" -m pytest -q \
    tests/test_nxs_hpi.py \
    tests/test_qemu_sh4_irq_timer.py \
    tests/test_main_dmac.py \
    tests/test_sh7764_ata.py
