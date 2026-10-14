#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF
#
# Build qemu-system-sh4 with the CDJ-2000 MAIN board.
#
# The board sources live in this repository (emulator/qemu/) and are mirrored
# into a QEMU checkout, which is kept outside it.  The two build-system hooks are
# appended idempotently rather than shipped as a patch: a patch against
# hw/sh4/meson.build fuzzes on every QEMU release, while "add the line if it is
# not there" does not.
#
#   sh scripts/build-qemu-sh4.sh [QEMU_SRC]     (default /c/qemu-src)
#
# Prerequisites (MSYS2 MINGW64 shell):
#   pacman -S --needed base-devel mingw-w64-x86_64-toolchain \
#       mingw-w64-x86_64-{glib2,pixman,meson,ninja,pkgconf,python}

set -e

# MSYS2's shell drops TMP, TEMP and USERPROFILE from the environment it hands to
# native processes.  GCC then falls back to GetTempPath(), which answers the
# Windows directory when all three are unset, and every single compile fails
# with "Cannot create temporary file in C:\WINDOWS\: Permission denied" — a
# message that reads like a broken toolchain and is nothing of the sort.
if [ -z "$TMP" ] && command -v cygpath >/dev/null 2>&1; then
    TMP=$(cygpath -w "${TMPDIR:-/tmp}" 2>/dev/null) || TMP='C:\Windows\Temp'
    TEMP=$TMP
    export TMP TEMP
    echo "TMP was unset by the shell; using $TMP"
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
QEMU_SRC=${1:-${QEMU_SRC:-/c/qemu-src}}
BUILD=$QEMU_SRC/build

# Windows builds get an .exe suffix and every other platform does not.
case $(uname -s 2>/dev/null) in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
    *)                    EXE= ;;
esac
TARGET=qemu-system-sh4$EXE

if [ ! -f "$QEMU_SRC/hw/sh4/meson.build" ]; then
    echo "not a QEMU source tree: $QEMU_SRC" >&2
    echo "git clone --depth 1 https://gitlab.com/qemu-project/qemu.git $QEMU_SRC" >&2
    exit 1
fi

# SH-4 interrupt and timer fixes, then the TCG fast paths; see
# patches/README.md. They form a stack:
# the priority-order patch edits lines the IMASK patch wrote, so once it is in,
# the IMASK patch checks neither forward nor reverse. Find the last patch that
# is already applied, take everything before it as applied too, and apply the
# rest in order, so re-running the script on a patched tree is a no-op.
set -- \
    "$REPO/patches/qemu-sh-intc-priority-imask.patch" \
    "$REPO/patches/qemu-sh-intc-priority-order.patch" \
    "$REPO/patches/qemu-sh-tmu-stop-reset.patch" \
    "$REPO/patches/qemu-sh4-tcg-fast-paths.patch" \
    "$REPO/patches/qemu-sh-tmu-catch-up.patch" \
    "$REPO/patches/qemu-main-loop-macos-hires.patch"
applied=0
index=0
for patch in "$@"; do
    index=$((index + 1))
    if patch -d "$QEMU_SRC" -p1 --reverse --force --silent --dry-run < "$patch" >/dev/null 2>&1; then
        applied=$index
    fi
done
# The reverse check cannot tell an older version of a patch from the current
# one, so record what was applied and refuse a tree whose record differs.
stamp=$QEMU_SRC/.cdj2000-patches
patch_id() { printf '%s %s\n' "$(basename "$1")" "$(cksum < "$1" | cut -d' ' -f1-2)"; }
index=0
for patch in "$@"; do
    index=$((index + 1))
    [ "$index" -le "$applied" ] || break
    if ! grep -qxF "$(patch_id "$patch")" "$stamp" 2>/dev/null; then
        echo "$QEMU_SRC carries a different or unrecorded version of $(basename "$patch")." >&2
        echo "Restore a clean tree (files the patches added too) and re-run, e.g.:" >&2
        echo "  git -C $QEMU_SRC checkout -- . && git -C $QEMU_SRC clean -fd -e build" >&2
        exit 1
    fi
done
index=0
for patch in "$@"; do
    index=$((index + 1))
    if [ "$index" -le "$applied" ]; then
        echo "$(basename "$patch") already applied"
    elif patch -d "$QEMU_SRC" -p1 --forward --silent --dry-run < "$patch" >/dev/null 2>&1; then
        echo "applying $(basename "$patch")"
        patch -d "$QEMU_SRC" -p1 --forward < "$patch"
    else
        echo "patch does not apply cleanly: $patch" >&2
        exit 1
    fi
done
for patch in "$@"; do patch_id "$patch"; done > "$stamp"

echo "mirroring board sources into $QEMU_SRC/hw/sh4"
for source in "$REPO"/emulator/qemu/*.c "$REPO"/emulator/qemu/*.h; do
    [ -e "$source" ] || continue
    # Preserve mtimes for unchanged inputs so Ninja can reuse their objects.
    # Compare bytes, not timestamps: checkout/restore may retain old mtimes.
    destination=$QEMU_SRC/hw/sh4/$(basename "$source")
    if cmp -s "$source" "$destination"; then
        continue
    fi
    cp -v "$source" "$QEMU_SRC/hw/sh4/"
done

# Ahead-of-time DSP regions (tools/cdj_dsp/aot_gen.py): generated from the
# local DSP image, so never in the repository; CDJ_C674X_AOT_SOURCE names
# the file, and without it any earlier one is removed.
aot=$QEMU_SRC/hw/sh4/cdj_c674x_aot.inc
if [ -n "${CDJ_C674X_AOT_SOURCE:-}" ]; then
    cmp -s "$CDJ_C674X_AOT_SOURCE" "$aot" || cp -v "$CDJ_C674X_AOT_SOURCE" "$aot"
elif [ -e "$aot" ]; then
    rm -v "$aot"
fi

# Every .c we mirror, as a meson files() argument list.
sources=$(cd "$REPO/emulator/qemu" && ls *.c | sed "s/.*/'&'/" | paste -sd, -)

meson_build=$QEMU_SRC/hw/sh4/meson.build
if ! grep -q CONFIG_CDJ2000_MAIN "$meson_build"; then
    echo "wiring the board sources into hw/sh4/meson.build"
    # Must be added before the "hw_arch +=" line that consumes the source set.
    tmp=$(mktemp)
    awk -v files="$sources" '/^hw_arch \+=/ && !done {
             print "sh4_ss.add(when: '\''CONFIG_CDJ2000_MAIN'\'', if_true: files(" files "))";
             print "";
             done = 1
         } { print }' "$meson_build" > "$tmp"
    mv "$tmp" "$meson_build"
elif ! grep -q "files($sources)" "$meson_build"; then
    # A tree wired before a source file was added needs the list refreshed;
    # rewriting just that line keeps this idempotent across future additions.
    echo "refreshing the board source list in hw/sh4/meson.build"
    tmp=$(mktemp)
    sed "s|^sh4_ss.add(when: 'CONFIG_CDJ2000_MAIN', if_true: files(.*|sh4_ss.add(when: 'CONFIG_CDJ2000_MAIN', if_true: files($sources))|" \
        "$meson_build" > "$tmp"
    mv "$tmp" "$meson_build"
fi

# The C66x DSP core (emulator/c6x/, vendored from cdj-nxs2-qemu, see its
# VENDORED.md) is a static library of its own: its float results must be exact
# per operation, so it gets -ffp-contract=off -frounding-math and the board
# sources do not. The board includes it as "c6x/c66x.h".
echo "mirroring the C66x core into $QEMU_SRC/hw/sh4/c6x"
mkdir -p "$QEMU_SRC/hw/sh4/c6x/binutils"
for source in "$REPO"/emulator/c6x/*.[ch] "$REPO"/emulator/c6x/binutils/*.h; do
    destination=$QEMU_SRC/hw/sh4/c6x/${source#"$REPO"/emulator/c6x/}
    cmp -s "$source" "$destination" || cp -v "$source" "$destination"
done
c6x_sources=$(cd "$REPO/emulator/c6x" && ls *.c | sed "s/.*/'c6x\/&'/" | paste -sd, -)
# The JIT's auto-generation (C66X_JIT_AUTO) runs cdj-nxs2-qemu's generator
# against its offline core library; both paths are baked in as strings.
c6x_tools=${C66X_JIT_TOOLS:-$HOME/Developer/cdj-nxs2-qemu/hw/cdj/c6x/tools}
c6x_libdir=${C66X_JIT_LIBDIR:-$HOME/build/c6x}
c6x_block="cdjc6x = static_library('cdjc6x', files($c6x_sources), c_args: ['-O3', '-ffp-contract=off', '-frounding-math', '-Wno-error', '-DC66X_JIT_TOOLS=\"$c6x_tools\"', '-DC66X_JIT_LIBDIR=\"$c6x_libdir\"'], build_by_default: false)"
if ! grep -qF "$c6x_block" "$meson_build" || ! grep -q 'link_whole: cdjc6x' "$meson_build"; then
    echo "wiring the C66x core library into hw/sh4/meson.build"
    tmp=$(mktemp)
    awk -v block="$c6x_block" '/^# cdjc6x begin/ { skip = 1 } !skip { print } /^# cdjc6x end/ { skip = 0 }' \
        "$meson_build" > "$tmp"
    awk -v block="$c6x_block" '/^hw_arch \+=/ && !done {
             print "# cdjc6x begin"
             print block
             print "sh4_ss.add(when: '\''CONFIG_CDJ2000_MAIN'\'', if_true: declare_dependency(link_whole: cdjc6x))"
             print "# cdjc6x end"
             print ""
             done = 1
         } { print }' "$tmp" > "$meson_build"
    rm -f "$tmp"
fi

kconfig=$QEMU_SRC/hw/sh4/Kconfig
if ! grep -q CDJ2000_MAIN "$kconfig"; then
    echo "wiring CONFIG_CDJ2000_MAIN into hw/sh4/Kconfig"
    cat >> "$kconfig" <<'EOF'

config CDJ2000_MAIN
    bool
    default y
    depends on SH4
    select PFLASH_CFI02
    select SD
    select IDE_MMIO
    select USB
EOF
fi

# Selections added after the Kconfig block was first written have to be
# appended to an already-wired tree rather than rewritten into it.  Each one
# arrived with a device: SD with the card, IDE_MMIO with the disc drive.  The
# loop is exact-match anchored, so IDE_MMIO is not mistaken for IDE_MMIO_FOO
# and SD is not satisfied by SDHCI.
for want in SD IDE_MMIO USB; do
    if sed -n '/^config CDJ2000_MAIN/,/^$/p' "$kconfig" |
           grep -qx "    select $want"; then
        continue
    fi
    echo "adding 'select $want' to CONFIG_CDJ2000_MAIN"
    tmp=$(mktemp)
    awk -v want="$want" '{ print }
         /^    select PFLASH_CFI02$/ { print "    select " want }' \
        "$kconfig" > "$tmp"
    mv "$tmp" "$kconfig"
done

if [ ! -f "$BUILD/build.ninja" ]; then
    echo "configuring (sh4-softmmu only)"
    mkdir -p "$BUILD"
    # --disable-fdt: SH-4 has no device tree, and the dtc subproject would
    # otherwise be cloned with git, which MSYS2 does not ship by default.
    (cd "$BUILD" && ../configure \
        --target-list=sh4-softmmu \
        --disable-gtk --disable-sdl --disable-vnc --disable-spice \
        --disable-docs --disable-guest-agent --disable-tools \
        --disable-fdt --disable-werror)
fi

echo "building"
# Only the emulator, never the default target.  Building everything also builds
# the TCG plugin DLLs, which need a writable temporary directory the toolchain
# does not always have -- and as a fallback after a real failure it replaces the
# compiler error that matters with a screenful of unrelated link errors.
ninja -C "$BUILD" "$TARGET"

"$BUILD/$TARGET" -M help
echo
echo "built: $BUILD/$TARGET"
