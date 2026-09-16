#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF
#
# Build the CDJ-2000 GUI board simulator: GNU sim from GDB 17.2, plus the
# patches in patches/.
#
#   sh scripts/build-bfin-sim.sh [options] [GDB_TARBALL_OR_DIR]
#
# With no argument the script looks for gdb-17.2.tar.xz beside the repository
# and in build/, and downloads it if neither is there.  The result is installed
# as bin/cdj-run.
#
# Options:
#   --march=ARCH     -march for the simulator; default "native", because the
#                    binary is a local tool.  Pass --march=nocona (the MSYS2
#                    baseline) for a binary that has to run elsewhere.
#   --opt=FLAG       optimisation level, default -O3.
#   --profile        build with -pg into a separate object tree and install
#                    as bin/cdj-run-pg, for gprof.  Give the run
#                    BFIN_EXIT_AFTER_WALL=<seconds> so the simulator exits
#                    normally and gmon.out is written.
#   --reconfigure    throw the object tree away first.  configure runs only
#                    when there is no config.status, so a change of flags
#                    does nothing without this.
#
# CDJ_SIM_CFLAGS overrides the compiler flags outright (it replaces the
# -O/-march/-g set, and --profile still appends -pg).
#
# Prerequisites: a C toolchain, make, patch, tar.  On MSYS2 MINGW64:
#   pacman -S --needed base-devel mingw-w64-x86_64-toolchain
#
# Everything upstream is GPLv3; see THIRD_PARTY.md.

set -e

MARCH=native
OPT=-O3
PROFILE=
RECONFIGURE=
while [ $# -gt 0 ]; do
    case $1 in
        --march=*)     MARCH=${1#--march=} ;;
        --opt=*)       OPT=${1#--opt=} ;;
        --profile)     PROFILE=1 ;;
        --reconfigure) RECONFIGURE=1 ;;
        --help|-h)     sed -n '2,40p' "$0"; exit 0 ;;
        --*)           echo "unknown option: $1" >&2; exit 2 ;;
        *)             break ;;
    esac
    shift
done

GDB_VERSION=17.2
GDB_TARBALL=gdb-$GDB_VERSION.tar.xz
GDB_URL=https://ftp.gnu.org/gnu/gdb/$GDB_TARBALL

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${CDJ_BUILD_DIR:-$REPO/build}
SRC=$WORK/gdb-$GDB_VERSION
OBJ=$WORK/sim-build
INSTALL_NAME=cdj-run
if [ -n "$PROFILE" ]; then
    OBJ=$WORK/sim-build-pg
    INSTALL_NAME=cdj-run-pg
fi

# The flags the simulator is compiled with.  -O3 -march=native because the
# interpreter's hot loop is the whole cost of a run; -g costs nothing at run
# time and keeps the binary debuggable.  --enable-sim-inline lets sim-core's
# and sim-events' small functions inline into their callers instead of being
# out-of-line calls on every guest memory access; --disable-sim-assert drops
# the three ASSERTs that otherwise run per byte in sim_core_find_mapping.
SIM_CFLAGS=${CDJ_SIM_CFLAGS:-"$OPT -march=$MARCH -g"}
[ -n "$PROFILE" ] && SIM_CFLAGS="$SIM_CFLAGS -pg"

HOST_OS=$(uname -s 2>/dev/null)
MAKE=${MAKE:-make}
case $HOST_OS in
    Darwin) MAKE=${CDJ_MAKE:-gmake} ;;
esac
command -v "$MAKE" >/dev/null 2>&1 || {
    echo "missing $MAKE; on macOS install Homebrew make, gmp and mpfr" >&2
    exit 1
}

case $HOST_OS in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
    *)                    EXE= ;;
esac

# MSYS2's shell drops TMP from the environment it hands to native processes, and
# GCC then tries to write to the Windows directory.  Every compile fails with a
# permission error that reads like a broken toolchain and is nothing of the sort.
if [ "$EXE" = .exe ] && [ -z "$TMP" ]; then
    TMP=$(cygpath -w "${TMPDIR:-/tmp}" 2>/dev/null) || TMP='C:\Windows\Temp'
    TEMP=$TMP
    export TMP TEMP
fi

mkdir -p "$WORK"

# ---------------------------------------------------------------- sources ---
if [ -n "$1" ] && [ -d "$1" ]; then
    echo "using the GDB tree at $1"
    SRC=$1
else
    tarball=$1
    if [ -z "$tarball" ]; then
        for candidate in "$WORK/$GDB_TARBALL" "$REPO/../$GDB_TARBALL" "$REPO/$GDB_TARBALL"; do
            [ -f "$candidate" ] && tarball=$candidate && break
        done
    fi
    if [ -z "$tarball" ]; then
        tarball=$WORK/$GDB_TARBALL
        echo "downloading $GDB_URL"
        if command -v curl >/dev/null 2>&1; then
            curl -fL -o "$tarball" "$GDB_URL"
        elif command -v wget >/dev/null 2>&1; then
            wget -O "$tarball" "$GDB_URL"
        else
            echo "no curl or wget; download $GDB_URL to $tarball yourself" >&2
            exit 1
        fi
    fi

    if [ ! -d "$SRC" ]; then
        echo "unpacking $tarball"
        # --force-local: a Windows path starting "C:" otherwise reads as a
        # remote host and tar tries to open an rsh connection.
        case $EXE in
            .exe) tar --force-local -xJf "$tarball" -C "$WORK" ;;
            *) tar -xJf "$tarball" -C "$WORK" ;;
        esac
    fi
fi

[ -d "$SRC/sim/bfin" ] || { echo "not a GDB source tree: $SRC" >&2; exit 1; }

# ---------------------------------------------------------------- patches ---
# --forward makes re-running the script on an already-patched tree a no-op.
for patch in "$REPO"/patches/[0-9][0-9]-gdb-*.patch; do
    [ -e "$patch" ] || continue
    name=$(basename "$patch")
    stamp=$SRC/.cdj-$name.applied
    checksum=$(cksum < "$patch")
    if [ -f "$stamp" ]; then
        if [ "$(cat "$stamp")" != "$checksum" ]; then
            echo "$name changed; use a fresh GDB source tree" >&2
            exit 1
        fi
        echo "$name already applied"
        continue
    fi
    if patch -d "$SRC" -p1 --forward --silent --dry-run < "$patch" >/dev/null 2>&1; then
        echo "applying $name"
        patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$patch"
    elif patch -d "$SRC" -p1 --reverse --force --silent --dry-run < "$patch" >/dev/null 2>&1; then
        echo "$name already applied"
    else
        echo "patch does not apply cleanly: $name" >&2
        exit 1
    fi
    echo "$checksum" > "$stamp"
done

# ------------------------------------------------------------------ build ---
if [ -n "$RECONFIGURE" ] && [ -d "$OBJ" ]; then
    echo "removing $OBJ"
    rm -rf "$OBJ"
fi
if [ ! -f "$OBJ/config.status" ]; then
    echo "configuring with CFLAGS=\"$SIM_CFLAGS\""
    mkdir -p "$OBJ"
    # The simulator only.  Building gdb itself takes an order of magnitude
    # longer and nothing here uses it.  CFLAGS in the environment reaches
    # bfd, opcodes and libiberty as well, which is harmless.
    set -- --with-system-zlib
    if [ "$HOST_OS" = Darwin ]; then
        for dependency in gmp mpfr; do
            prefix=$(brew --prefix "$dependency")
            set -- "$@" "--with-$dependency=$prefix"
        done
    fi
    (cd "$OBJ" && CFLAGS="$SIM_CFLAGS" "$SRC/configure" \
        --target=bfin-elf \
        --disable-gdb --disable-binutils --disable-gas --disable-ld \
        --disable-gprof --disable-gprofng --disable-nls --disable-werror \
        --enable-sim --enable-sim-inline --disable-sim-assert "$@")
    echo "$SIM_CFLAGS" > "$OBJ/cdj-cflags"
fi

# The top-level configure does not descend; sim/Makefile is written by the
# top-level make.  Without this step the next line fails with "sim: No such
# file or directory", which reads like a broken tarball and is not one.
#
# MAKEINFO=true throughout: makeinfo builds the manuals and nothing else, it is
# often not installed, and without this bfd stops on doc/bfd.info with an error
# that says nothing about the simulator.
echo "configuring sim"
"$MAKE" -C "$OBJ" MAKEINFO=true configure-sim

# bfin/run links against libbfd, libopcodes and libiberty.  Only the top-level
# makefile knows how to build them, and it is not reached by building in sim/.
#
# The failure that is tolerated here is the translation catalogues: po/ wants
# msgfmt, which has nothing to do with the simulator, and it takes the whole
# recursive target down with it.  So the exit status is ignored and the three
# libraries are checked for directly instead -- a real failure still stops the
# script, one line later and with a clearer message.
echo "building the libraries the simulator links against"
"$MAKE" -C "$OBJ" MAKEINFO=true all-bfd all-libiberty all-opcodes || true

missing=
for lib in bfd/libbfd.la libiberty/libiberty.a opcodes/libopcodes.la; do
    [ -f "$OBJ/$lib" ] || missing="$missing $lib"
done
if [ -n "$missing" ]; then
    echo "these libraries were not built:$missing" >&2
    echo "re-run with the output visible; the error above is the real one" >&2
    exit 1
fi

# The viewer consumes PPM frames. Autodetected SDL selects another backend,
# so remove an old SDL-built object when upgrading an existing build tree.
if [ "$(cat "$OBJ/cdj-display-backend" 2>/dev/null)" != file ]; then
    rm -f "$OBJ/sim/bfin/gui.o"
fi
echo "building"
# The makefile that knows about bfin/run is sim/Makefile, not sim/bfin/.
# -lws2_32 is the socket library the MAIN link needs on Windows.
case $EXE in
    .exe) "$MAKE" -C "$OBJ/sim" MAKEINFO=true "bfin/run$EXE" SDL_CFLAGS= LIBS=-lws2_32 ;;
    *)    "$MAKE" -C "$OBJ/sim" MAKEINFO=true "bfin/run$EXE" SDL_CFLAGS= ;;
esac

echo file > "$OBJ/cdj-display-backend"

# ---------------------------------------------------------------- install ---
# libtool leaves a ~50 KB wrapper stub at sim/bfin/run and the real ~17 MB
# binary in sim/bfin/.libs/.  Copying the stub produces something that exits
# 127 with no output, which is a confusing way to spend an afternoon.
real=$OBJ/sim/bfin/.libs/run$EXE
[ -f "$real" ] || real=$OBJ/sim/bfin/run$EXE
[ -f "$real" ] || { echo "no simulator was built" >&2; exit 1; }

mkdir -p "$REPO/bin"
# Replace the inode instead of overwriting a previously executed Mach-O.
# macOS can retain its old code-signature pages and kill the new image on launch
# even when codesign verifies the bytes on disk. Stage on the same filesystem
# so the rename is atomic and a failed copy leaves the installed binary intact.
install_tmp=$(mktemp "$REPO/bin/.$INSTALL_NAME.XXXXXX")
trap 'rm -f "$install_tmp"' EXIT
cp "$real" "$install_tmp"
chmod +x "$install_tmp"
mv -f "$install_tmp" "$REPO/bin/$INSTALL_NAME$EXE"
trap - EXIT

size=$(wc -c < "$REPO/bin/$INSTALL_NAME$EXE")
echo
echo "built: $REPO/bin/$INSTALL_NAME$EXE ($size bytes, CFLAGS $(cat "$OBJ/cdj-cflags" 2>/dev/null))"
if [ "$size" -lt 1000000 ]; then
    echo "that is far too small to be the simulator -- it is libtool's wrapper" >&2
    exit 1
fi

# A rebuilt tree with the old binary still installed is the most expensive
# silent error in the project (BUILD.md), so say when the copy is fresh.
echo "installed $(date +%Y-%m-%dT%H:%M:%S%z)"
