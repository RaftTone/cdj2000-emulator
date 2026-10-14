# Vendored: the C66x DSP core of cdj-nxs2-qemu

The files in this directory are copied from
[Stijn-Jacobs/cdj-nxs2-qemu](https://github.com/Stijn-Jacobs/cdj-nxs2-qemu), `hw/cdj/c6x/`,
at commit `c24a89d523240641274dc2a06598fbb7f9ae71df` (10.10.2026, the last change to that directory in
`main` at `eea1e0f`). Author: Stijn Jacobs. Copied on 12.10.2026.

What it is: an interpreter for the TI C66x instruction set (a superset of the C674x and the C67x+ of
the CDJ-2000's C6727) with a profile-guided JIT that loads native modules (`C66X_JIT=<m.so>`). The
CDJ-2000 4.33 DSP program is pure C67x+ (no compact packets, no SPLOOP), so the core runs it as is.

Taken: `c66x.h`, `c66x_core_int.h`, `c66x_priv.h`, `c66x_decode.[ch]`, `c66x_ext_table.h`,
`c66x_core.c`, `c66x_mem.c`, `c66x_exec.c`, `c66x_step.c`, `c66x_jit.c`, `c66x_core.md` (its notes)
and `binutils/`. Not taken: the C6655 SoC models (`soc_*`, the CDJ-2000NXS2's chip), the offline
runner and replay tools, the tests and the JIT generator (`tools/c14_jitgen.py`); modules are built
with the generator in the cdj-nxs2-qemu checkout.

Do not edit these files here: changes go upstream first, then the directory is copied again. Our
own code around the core (the C6727 chip, its host port, dMAX, audio out) lives in `emulator/qemu/`.
The exceptions are the local changes below, each marked in the file; a new copy has to keep them
until upstream has its own fix.

## Local changes

14.10.2026, `c66x_exec.c` (merge of PR #47, after a first run on Windows/x86-64):

- `f_to_i32` (SPINT, SPTRUNC, DPINT, DPTRUNC, DSPINT): NaN, infinities and values out of range
  saturate by sign to 0x7FFFFFFF or 0x80000000, as the C67x does (and `cdj_c674x_sp_to_integer`
  in `emulator/qemu/`). They all gave 0x80000000, so a positive sample clipped through SPINT came
  out at negative full scale.
- `sp_out`/`dp_out`: a NaN result of the float operations is the C67x's NaN_out, 0x7FFFFFFF (DP:
  0x7FFFFFFF_FFFFFFFF), with the sign of the product for the multiplies. The host's default NaN
  was stored before, and x86 (0xFFC00000) and arm64 (0x7FC00000) disagree on it, so the same run
  gave different register contents on a Mac and on a PC.

JIT modules built by the upstream generator compile the upstream semantics; with these changes a
module and the interpreter can differ on NaN and out-of-range conversions.

## Licences

- The core files: GPL-2.0-or-later (SPDX headers in each file), the same as this repository.
- `binutils/`: verbatim tables from GNU binutils 2.42 (`include/opcode/tic6x*.h`),
  **GPL-3.0-or-later** (see `binutils/README`). The decoder (`c66x_decode.c`) includes them.
  A binary built with them combines GPL-3.0-or-later code with GPL-2.0-only QEMU and must not be
  distributed (`THIRD_PARTY.md`). Our own decoder (`emulator/qemu/cdj_c674x.c`) decodes all 79 824
  instructions of the 4.33 DSP program the same way and could replace the tables if needed.

The core is compiled as its own static library with `-ffp-contract=off -frounding-math`: its float
results must be exact per operation (the JIT modules are checked against it).
