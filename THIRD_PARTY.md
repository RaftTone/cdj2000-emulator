# Third-party software

This repository contains no third-party source. It contains **patches against**
third-party source, and build scripts that fetch it. Each upstream keeps its own
licence; the patches inherit the licence of what they patch.

## GNU GDB 17.2 -- the Blackfin GUI board

| | |
|---|---|
| what | `sim/`, the GNU simulator, built for `bfin-elf` |
| where from | `https://ftp.gnu.org/gnu/gdb/gdb-17.2.tar.xz`, fetched by `scripts/build-bfin-sim.sh` |
| licence | GPL-3.0-or-later (see `COPYING3` in the tarball) |
| our changes | `patches/01-gdb-17.2-bfin-parallel-dsp32alu.patch` , `patches/02-gdb-17.2-bfin-cdj2000-board.patch`, `patches/03-gdb-17.2-macos-lp64.patch`, and `patches/04-gdb-17.2-posix-framebuffer.patch` |

Patches 01 and 02 touch sixteen files -- twelve under `sim/bfin/` and four under
`sim/common/` -- and add no new ones. Because they are derivative of GPLv3 FSF
code, **all four patch files are GPL-3.0-or-later**, whatever the rest of this
repository is licensed under. Every file they modify keeps its FSF copyright
header intact.

Patch 04 ports the file-backed framebuffer to POSIX and adds guest PC to
periodic simulator statistics.

Patch 03 corrects LP64 sign extension and BSD sed syntax in the simulator
source and its generated build rules, and adds the POSIX header required by
SPORT socket handling.

Patch 01 is an ordinary correctness fix and is upstream-suitable on its own.
Patch 02 is the CDJ-specific board work and is not: it is a large body of
device modelling that only makes sense for this player.

## QEMU -- the SH-4 MAIN board

| | |
|---|---|
| what | `qemu-system-sh4`, version 11.x or newer |
| where from | `https://gitlab.com/qemu-project/qemu.git`, cloned by you; `scripts/build-qemu-sh4.sh` builds it |
| licence | GPL-2.0-only for the emulator as a whole |
| our changes | `patches/qemu-sh-intc-priority-imask.patch` |

That patch corrects four omissions in QEMU's SH-4 interrupt path
(`hw/intc/sh_intc.c`, `target/sh4/translate.c`); `patches/README.md` explains
each one and what it costs to leave it out. Being derivative of QEMU, **it is
GPL-2.0**.

The board model in `emulator/qemu/` is our own code, not a patch. The build
script copies it into a QEMU checkout and wires it into meson and Kconfig
there, so nothing in this repository is a modified QEMU file.

QEMU 11.x or newer is required: the board includes `hw/core/boards.h` and
`system/address-spaces.h`, which are the post-reorganisation header paths.

## Adapted design: cdj-nxs2-qemu (Stijn Jacobs)

| | |
|---|---|
| what | the lockstep DSP-thread pattern (DSP on its own host thread, at most one quantum ahead of QEMU virtual time; MAIN releases the BQL while it waits) |
| where from | `https://github.com/Stijn-Jacobs/cdj-nxs2-qemu` at `08d5cb1`: `hw/cdj/boards/cdj2000/dsp_host.c`, `hw/cdj/boards/nxs2/dsp_c6x.c` (`CDJ_C6X_THREAD=2`) |
| licence | GPL-2.0-or-later; used with the author's permission |
| our use | `emulator/qemu/cdj2000_nxs_hpi.c`, `CDJ_NXS_DSP_THREAD=1` (2026-10-05): the pattern re-implemented for our C674x and HPI model, no code copied verbatim |

## Nothing else is vendored

No third-party source is copied into this tree. The only binaries this
repository will ever hold are none.

## What this project's own code is licensed under

`GPL-2.0-or-later` -- `emulator/`, `tools/`, `tests/`, `scripts/`, and the
documentation. See `LICENSE`. GPL-2.0-or-later was chosen because the board
model is compiled into QEMU, which is GPL-2.0-only: a GPLv3-only board could
not legally be linked into it, and a permissive licence would let the work be
taken closed.
