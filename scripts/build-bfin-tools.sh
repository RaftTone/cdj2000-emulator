#!/bin/sh
# Build only the assembler/linker used by the Blackfin guest regressions.
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# MSYS2 drops TMP for native processes; GCC then writes to C:\WINDOWS.
# --force-local is GNU tar on Windows only: BSD tar on macOS has no such flag,
# and a C: path would otherwise be treated as a remote host.
case $(uname -s 2>/dev/null) in
    MINGW*|MSYS*|CYGWIN*)
        if [ -z "$TMP" ]; then
            TMP=$(cygpath -w "${TMPDIR:-/tmp}" 2>/dev/null) || TMP='C:\Windows\Temp'
            TEMP=$TMP
            export TMP TEMP
            echo "TMP was unset by the shell; using $TMP"
        fi
        TAR_EXTRACT='tar --force-local -xf'
        ;;
    *)
        TAR_EXTRACT='tar -xf'
        ;;
esac

version=2.44
archive=${1:-"$REPO/build/binutils-$version.tar.xz"}
mkdir -p "$REPO/build"
if [ ! -f "$archive" ]; then
    curl -fL "https://ftp.gnu.org/gnu/binutils/binutils-$version.tar.xz" -o "$archive"
fi
python3 - "$archive" <<'PY'
import hashlib, sys
from pathlib import Path
expected = 'ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237'
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() != expected:
    raise SystemExit('binutils archive checksum mismatch')
PY
if [ ! -d "$REPO/build/binutils-$version" ]; then
    $TAR_EXTRACT "$archive" -C "$REPO/build"
fi
mkdir -p "$REPO/build/bfin-binutils"
cd "$REPO/build/bfin-binutils"
if [ ! -f Makefile ]; then
    "../binutils-$version/configure" --target=bfin-elf --disable-nls \
        --disable-werror --disable-gdb --disable-gprof --disable-gprofng \
        --disable-gold --disable-sim --with-system-zlib
fi
make -j"${JOBS:-4}" MAKEINFO=true all-gas all-ld
printf 'Blackfin test tools ready in %s/build/bfin-binutils\n' "$REPO"
