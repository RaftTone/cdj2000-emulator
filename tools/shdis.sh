#!/bin/sh
# shdis.sh 'x /40i 0x04215180' ['x /8wx 0x...' ...]: MAIN code through QEMU's
# own SH-4 disassembler, on a paused machine with no ports and nothing running.
cd "$(dirname "$0")/.." || exit 1
{ sleep 0.5; for c in "$@"; do echo "$c"; done; echo quit; } | \
  build/qemu/build/qemu-system-sh4 -M cdj2000-main -bios firmware/main-firmware.bin -S \
    -display none -serial null -serial null -serial null -monitor stdio -nodefaults \
    -device loader,file=firmware/main-unpacked.bin,addr=0x04000000,force-raw=on 2>&1 | \
  LC_ALL=C tr -d '\r' | LC_ALL=C grep -a -v '^(qemu)\|^QEMU [0-9]' | LC_ALL=C sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
