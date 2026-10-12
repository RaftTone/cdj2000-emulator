# Third-party software

This repository contains one piece of vendored third-party source (the
Blackfin core in `emulator/bfin/`, below). Otherwise it contains **patches
against** third-party source, and build scripts that fetch it. Each upstream
keeps its own licence; the patches inherit the licence of what they patch.

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
| our changes | `patches/qemu-sh-intc-priority-imask.patch`, `qemu-sh-intc-priority-order.patch`, `qemu-sh-tmu-stop-reset.patch`, `qemu-sh4-tcg-fast-paths.patch` |

That patch corrects four omissions in QEMU's SH-4 interrupt path
(`hw/intc/sh_intc.c`, `target/sh4/translate.c`); `patches/README.md` explains
each one and what it costs to leave it out. Being derivative of QEMU, **it is
GPL-2.0**.

`qemu-sh4-tcg-fast-paths.patch` (SH-4 TB-exit fast paths, the store-to-code
fast reject and the jump-cache profiler) is adapted from
[cdj-nxs2-qemu](https://github.com/Stijn-Jacobs/cdj-nxs2-qemu) by Stijn
Jacobs, GPL-2.0-or-later, used with the author's permission; the patched files
carry a credit line. It is GPL-2.0 like the rest of the QEMU patches.

The board model in `emulator/qemu/` is our own code, not a patch. The build
script copies it into a QEMU checkout and wires it into meson and Kconfig
there, so nothing in this repository is a modified QEMU file.

QEMU 11.x or newer is required: the board includes `hw/core/boards.h` and
`system/address-spaces.h`, which are the post-reorganisation header paths.


## cdj-nxs2-qemu -- the Blackfin core behind `cdj-gui-run`

| | |
|---|---|
| what | `hw/cdj/bfin/` `bfin_core.c`, `bfin_exec.c`, `bfin_dsp.c`, `bfin_dsp.h`, `bfin_priv.h`, `bfin.h`, `bf531.c`, `bf531.h`; `emulator/bfin/cdj_gui_run.c` is based on its `bfinrun.c` |
| where from | Stijn Jacobs, `https://github.com/Stijn-Jacobs/cdj-nxs2-qemu`, commit `08d5cb1` (2026-10-03) |
| licence | GPL-2.0-or-later (SPDX line in every file, kept), used with the author's permission |
| our changes | dated notice at the top of each file; `bf531.c`/`bf531.h` (LDR boot split from the update loader, flash accessor, PF ready toggle, async bank 3 latch, IRQ-less GP timers no longer end a step), `bfin.h`/`bfin_core.c` (`bfin_code_lines_run`, GNU sim's accumulator words); `bfin_priv.h`/`bfin_exec.c` (GNU sim's bundle order and store queue, flags, reserved forms); `bfin_dsp.c` replaced, see below |

### GNU sim's semantics in `emulator/bfin/bfin_dsp.c`

| | |
|---|---|
| what | the DSP32 ALU/shift/MAC groups, ALU2op, CCflag and their helpers from `sim/bfin/bfin-sim.c`, and field layouts from `include/opcode/bfin.h`, of GDB 17.2 as built here (patches 01-14), verbatim under a shim onto the fast core's state |
| where from | GDB 17.2, Copyright (C) 2005-2025 Free Software Foundation, Inc., contributed by Analog Devices, Inc. |
| licence | GPL-3.0-or-later (SPDX line in the file); the combined `cdj-gui-run` is therefore GPL-3.0-or-later |
| why | `bin/cdj-run` is the reference the fast core must match bit for bit (`tools/cdj_gui/bfin_diff.py`) |

None of the binutils-derived C66x decoder tables of that repository are
included; the Blackfin files have no third-party tables.

## Credit: cdj-nxs2-qemu (Stijn Jacobs)

| | |
|---|---|
| what | ideas from the C66x interpreter of `https://github.com/Stijn-Jacobs/cdj-nxs2-qemu` (commit `08d5cb1`), used with the author's permission |
| used in | `emulator/qemu/cdj_c674x.c` (persistent per-PC decoded-packet cache and a fast path for common packets, after its `c66x_step.c` packet cache and `fast_cycles`; compiled SPLOOP kernels, `cdj_c674x_run` (2026-10-06), after the design of its `tools/c14_jitgen.py`/`c66x_jit.c` region compiler - schedule and pipeline resolved ahead of time, interpreter semantics for each operation, in-flight writes restored into the interpreter's structures on exit), `tools/cdj_dsp/decode_crosscheck.{c,py}` (decoder-vs-objdump sweep over the whole DSP image, after its `make m1`) |
| code copied | none: both were written against this core; files carry a courtesy credit line |
| also | ahead-of-time regions (2026-10-06): `tools/cdj_dsp/aot_gen.py` and the "ahead-of-time regions" section of `emulator/qemu/cdj_c674x.c`, after the approach of its `tools/c14_jitgen.py` + `c66x_jit.c` - profile the hot code, generate C for it with the pipeline state fixed at generation time, key each compiled packet by its code bytes, fall back to the interpreter where the static state does not hold, and keep the generated code with the build rather than the repository (it derives from the firmware); written for this core's lean-path pieces, no code copied |

## Adapted design: cdj-nxs2-qemu (Stijn Jacobs)

| | |
|---|---|
| what | the lockstep DSP-thread pattern (DSP on its own host thread, at most one quantum ahead of QEMU virtual time; MAIN releases the BQL while it waits) |
| where from | `https://github.com/Stijn-Jacobs/cdj-nxs2-qemu` at `08d5cb1`: `hw/cdj/boards/cdj2000/dsp_host.c`, `hw/cdj/boards/nxs2/dsp_c6x.c` (`CDJ_C6X_THREAD=2`) |
| licence | GPL-2.0-or-later; used with the author's permission |
| our use | `emulator/qemu/cdj2000_nxs_hpi.c`, `CDJ_NXS_DSP_THREAD=1` (2026-10-05): the pattern re-implemented for our C674x and HPI model, no code copied verbatim |
| also | the virtual-time peripheral clock with a configurable DSP clock (`CDJ_C6X_MHZ` in `nxs2/dsp_c6x.c`): `emulator/qemu/cdj_dsp_audio_clock.h`, `CDJ_NXS_DSP_AUDIO_CLOCK=virtual` (2026-10-05), McASP slots on the DSP thread's clock; re-implemented, no code copied |

## Nothing else is vendored

No other third-party source is copied into this tree. The only binaries this
repository will ever hold are none.

## What this project's own code is licensed under

`GPL-2.0-or-later` -- `emulator/`, `tools/`, `tests/`, `scripts/`, and the
documentation. See `LICENSE`. GPL-2.0-or-later was chosen because the board
model is compiled into QEMU, which is GPL-2.0-only: a GPLv3-only board could
not legally be linked into it, and a permissive licence would let the work be
taken closed.

The one exception is `emulator/bfin/`: the `cdj-gui-run` program built from
it is GPL-3.0-or-later as a whole, because `bfin_dsp.c` carries GNU sim code
(above). Its other files keep their GPL-2.0-or-later SPDX lines;
`emulator/bfin/LICENSE.md` explains the combination and `emulator/bfin/COPYING3`
has the licence text. Nothing in `emulator/bfin/` is linked into QEMU.
