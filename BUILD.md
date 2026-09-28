# Building

Host recipes for both Windows and macOS. Diagnostic notes from NXS/DSP work
follow; they do not replace the build steps.

**Windows (MSYS2 MINGW64, x86-64):** install the packages under
[What you need](#what-you-need), then:

```sh
sh scripts/build-bfin-sim.sh
git clone --depth 1 https://gitlab.com/qemu-project/qemu.git /c/qemu-src
sh scripts/build-qemu-sh4.sh /c/qemu-src
export CDJ_QEMU=/c/qemu-src/build/qemu-system-sh4
```

The NXS launcher resolves `bin/cdj-run.exe` and `CDJ_QEMU`. `--debug` and
`--qemu-sync-profile` use TCP/telnet chardevs; MinGW QEMU has no `unix:` sockets.

**macOS (Homebrew, including Apple Silicon):** see
[macOS migration](#macos-migration). `scripts/build-bfin-sim.sh` uses `gmake`
and Homebrew `gmp`/`mpfr`. In-tree QEMU stays
`build/qemu/build/qemu-system-sh4`. `--debug` keeps Unix QMP/monitor sockets
and the relative-path `sockaddr_un` workaround.

Check the captured NXS MAIN/DSP ready/clear/ack sequence without executing or
modifying firmware:

```sh
.venv/bin/python -m tools.cdj_dsp.boot_handshake runs/RUN/dsp-events.jsonl
```

Exit 0 means the final reset epoch contains an ordered MAIN read of exactly 1
at 0x1183fff4, clear of 0x1183ffec, then acknowledgement of 0x1183fff0. Exit 1
means that evidence is absent; malformed/incomplete records fail with exit 2.
The tool hashes the transcript and names the firmware address-map hash, but
does not authenticate its origin or establish boot, GUI liveness, or absence
of a later error. Use it alongside strict replay and the connected visual gate.

Deferred scheduling is an opt-in diagnostic, not a boot-validation mode:

```sh
.venv/bin/python -m pytest -q tests/test_dsp_scheduler.py \
  tests/test_dsp_deferred_replay.py tests/test_dsp_checkpoint_replay.py
sh scripts/build-qemu-sh4.sh build/qemu
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_DEFERRED_DIAGNOSTIC \
  --seconds 20 --frame-interval 5 --port 6080 \
  --qemu build/qemu/build/qemu-system-sh4 --deferred-dsp-scheduling
```

Schema 11 appends serializable activation/slice state. Legacy schemas import
with legacy scheduling. Deferred replay requires the captured transcript when
continuation is pending; never infer missing host interleaving. The explicit
mode remains architecturally ineligible, and short callbacks do not prove MAIN
receives sufficient execution time. The first longer GUI diagnostic still
stalls; do not promote this flag to the strict default.

Add `--qemu-sync-profile` to either legacy or deferred diagnostics to enable
QEMU lock-wait profiling. Before teardown the launcher captures
`info sync-profile -n 30` and `info sync-profile -m -n 30` through its run-local
Unix monitor socket, saving `qemu-sync-profile.txt` and collection status/hash
in `run.json`. Profiling introduces observer overhead; compare both policies
with identical inputs, and do not interpret lock waits as firmware cycle timing.

Connected replay limits are diagnostic, not firmware events. If a step,
packet or cycle ceiling prevents reaching the next recorded boundary, replay
exits nonzero with `outcome=event_budget_exhausted` in `failure.json` and its
manifest. The optional gate fails, and no final checkpoint is written because
pending HPI/cooperative scheduling state is not serialized. Resume from a
prior captured connected checkpoint, not this partial trace. A valid boundary
at the exact limit can still verify; contradictory state remains a mismatch.

Standalone SH7764 IIC empty-bus controller regression:

```sh
.venv/bin/python -m pytest -q tests/test_sh7764_iic.py
```

The NXS board now connects this controller to a bounded identity-only endpoint.
Run `tests/test_mfi_identity.py` alongside the controller tests. Apple HomeKitADK
documents 2.0C device/firmware versions in registers 0/1; these are independently
specified constants, not inferred success responses. All other identity/device
operations remain unsupported. No certificates, signing or authentication
success are synthesized. The QEMU adapter aggregates nine SCL periods per byte
and one per STOP using 53.950MHz Pck; START/STOP and pin-level timing remain
approximations, and the board's 53.930MHz alternative documentation is unresolved.
Two corrected 120-second cold runs now reach normal player/UTILITY without
E-7010 or E-7206 and respond to controls. This establishes clean startup, not
storage/audio functionality or full fidelity.

Strict timed SPI/schema-10 focused regression:

```sh
.venv/bin/python -m pytest -q tests/test_c6747_spi_timed.py \
  tests/test_c6747_spi_clock.py tests/test_dsp_checkpoint_replay.py
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-spkernel-h7-strict-1 \
  runs/NEW_SPI_STRICT_REPLAY --steps 1000000 --verify-repeat
```

The current 40-byte transfer state preserves pending RX, chip-select latch,
queued TX and mandatory inter-transfer gap across checkpoints. Schema 9
migration retains all prior peripheral state and initializes only this new tail.
The strict endpoint supports the observed WM8740 format/configuration, not
arbitrary SPI devices, IRQ/DMA or chip-select-hold modes. Unsupported accesses
fail closed. The original instantaneous exploratory endpoint remains separate.

TI-backed compact SPKERNEL regression (user-installed C6000 CGT 8.5.0.LTS):

```sh
C6X_TI_BIN=/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin \
  .venv/bin/python -m pytest -q tests/test_c674x.py tests/test_c674x_spkernel_fields.py
```

Without `C6X_TI_BIN`, only the independent external-tool test skips; the full
field/schedule test still runs. The TI assembler fixture is
`tests/ti/spkernel-oracle.asm`. Do not use GNU libopcodes to validate its compact
SPKERNEL operands: the checked GNU version has a field-scatter discrepancy.

Boot-error milestone evidence (strict, no exploratory DSP flags):

```sh
sh scripts/build-qemu-sh4.sh build/qemu
.venv/bin/python -m pytest -q tests/test_nxs_boot_evidence.py tests/test_dsp_replay.py
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_STRICT_BOOT \
  --seconds 120 --frame-interval 5 --ui \
  --qemu build/qemu/build/qemu-system-sh4
```

Use a new run directory each time. Closing the UI ends the run. The command
collects evidence; a zero launcher exit is NOT a successful-boot result. Inspect
the frame timeline and DSP fault/handshake state, record basic interactions,
and require at least 60 seconds after the normal player appears without
E-7010 before repeating a cold boot. Two strict 120-second captures and a
late encoder response have been captured. E-7206 AUTH CHIP ERROR remains; this
evidence does not establish clean full boot, full DSP parity or working audio.
`frames/manifest.json` records observation times, hashes and incomplete/missing
frames. Unchanged images do not prove liveness. `run.json` records the actual
binary/firmware hashes before launch and after exit. No frame capture or input
hashing changes firmware traffic, error conditions, or DSP timing semantics.

Exact-address mnemonic/operand inventory (macOS): the GNU source tree prepared
by the dependency build is reused; these ignored libraries are not in Git.
From a fresh `build/tic6x-binutils` directory, configure and build:

```sh
mkdir -p build/tic6x-binutils
cd build/tic6x-binutils
../gdb-17.2/configure --target=tic6x-elf --disable-nls --disable-gdb \
  --disable-gas --disable-ld --disable-gprof --disable-gold \
  --disable-libdecnumber --disable-readline --disable-sim --disable-werror
make -j4 all-bfd all-opcodes all-libiberty
cd ../..
cc -std=c11 -Wall -Wextra -Werror -Ibuild/tic6x-binutils/bfd \
  -Ibuild/gdb-17.2/include -Ibuild/gdb-17.2/bfd \
  tools/cdj_dsp/tic6x_disasm.c \
  build/tic6x-binutils/opcodes/.libs/libopcodes.a \
  build/tic6x-binutils/bfd/.libs/libbfd.a \
  build/tic6x-binutils/libiberty/libiberty.a \
  -lz -L/opt/homebrew/opt/zstd/lib -lzstd -o /tmp/cdj-tic6x-disasm
C6X_DISASSEMBLER=/tmp/cdj-tic6x-disasm .venv/bin/python -m pytest -q \
  tests/test_dsp_semantic_inventory.py tests/test_dsp_coverage.py tests/test_dsp_inventory.py
.venv/bin/python -m tools.cdj_dsp.semantic_inventory \
  runs/dsp-circular-transcript-replay-1/final.cdjdsp \
  runs/dsp-circular-transcript-replay-1/coverage.json \
  runs/NEW_SEMANTIC_INVENTORY.json --disassembler /tmp/cdj-tic6x-disasm
```

The link flags above match this host's Homebrew zstd-enabled BFD build; use the
equivalent library path for another host. On MSYS2 MINGW64 that is typically
`-lz -lzstd` without `-L/opt/homebrew/...`, writing the binary under `build/`
instead of `/tmp` if you prefer. Report output must be new. The
frontend batches exact addresses via stdin instead of linear sweeping across
data. Reports preserve provenance hashes and remain non-validating: canonical
disassembly does not prove execution, semantic correctness or test completeness.

AMR/circular-addressing validation:

```sh
.venv/bin/python -m pytest -q tests/test_c674x.py tests/test_c674x_circular.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iemulator/qemu tests/cstub/c674x-circular.c \
  emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c \
  -o /tmp/cdj-circular-family-san
/tmp/cdj-circular-family-san
sh scripts/build-qemu-sh4.sh build/qemu
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_CIRCULAR_CONNECTED \
  --seconds 15 --qemu build/qemu/build/qemu-system-sh4 \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_CIRCULAR_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_CIRCULAR_REPLAY --steps 100000000 \
  --events runs/NEW_CIRCULAR_CONNECTED/dsp-events.jsonl \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx --verify-repeat
```

Use current sources for pending circular transfers: the existing queue's size
high byte now retains circular width at issue. AMR-use interlocks remain
fail-closed after executed MVC AMR; exact stall prediction and loop missed-stall
exceptions are not implemented.

Reanalyze a captured predicate inventory without rerunning firmware:

```sh
.venv/bin/python -m tools.cdj_dsp.coverage \
  runs/dsp-saturation-transcript-replay-1/final.cdjdsp \
  runs/dsp-saturation-transcript-replay-1/trace.jsonl \
  runs/NEW_SOURCE_PREDICATE_REPORT.json \
  --formats build/gdb-17.2/include/opcode/tic6x-insn-formats.h
```

Standalone analysis is not validation-eligible: it does not evaluate inherited
execution-mode approximations. Replay still combines coverage with its own
provenance and equivalence gates. Classified source predicates are not proof
of buffered issue or instruction-family correctness.

Saturating arithmetic batch validation:

```sh
.venv/bin/python -m pytest -q tests/test_c674x.py tests/test_c674x_saturation.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iemulator/qemu tests/cstub/c674x-saturation.c \
  emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c \
  -o /tmp/cdj-saturation-family-san
/tmp/cdj-saturation-family-san
sh scripts/build-qemu-sh4.sh build/qemu
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_SAT_CONNECTED \
  --seconds 15 --qemu build/qemu/build/qemu-system-sh4 \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_SAT_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_SAT_REPLAY --steps 100000000 \
  --events runs/NEW_SAT_CONNECTED/dsp-events.jsonl \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx --verify-repeat
```

Local socket permission is required for connected runs and full pytest. The
initial sandboxed `nxs-saturation-connected-1` failed before firmware execution
because it could not bind serial sockets; `nxs-saturation-connected-2` is the
successful authorized run. In-flight saturation status uses sentinel size 34
in the existing checkpoint writeback queue. Use current sources to resume it.

Source-predicate audit regression (tool-only; no QEMU rebuild needed):

```sh
.venv/bin/python -m pytest -q tests/test_dsp_coverage.py tests/test_dsp_replay.py
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-return-bnop-connected-replay-1/final.cdjdsp \
  runs/NEW_PREDICATE_AUDIT --steps 100000 \
  --functional-dsp-timing --functional-dsp-audio --verify-repeat
```

Inspect `coverage.json`'s `source_predicate_audit` and per-instruction
`source_predicate_outcomes`. These are pre-source-fetch observations, not proof
of predicate-body execution in buffered loops or under SPMASK. Exploratory
ancestry remains ineligible for architectural validation.

The current validation checkpoint is in the final section. Earlier milestones
below are historical and retain the limitations measured then.

Two emulators, built separately, from sources that live outside this repository.
Neither build needs firmware.

## What you need

* **Python 3.10 or newer**, with `tkinter`. On Debian and Ubuntu that is a
  separate package, `python3-tk`.
* **A C toolchain**, `make`, `patch`, `tar`.
* For the MAIN board: **meson**, **ninja**, **pkg-config**, **glib2**,
  **pixman** -- QEMU's own dependencies.

On Windows this is developed and tested in an **MSYS2 MINGW64 shell**:

```sh
pacman -S --needed base-devel mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-{glib2,pixman,meson,ninja,pkgconf,python}
```

Use the MINGW64 shell, not Git Bash. Git Bash's `make` resolves `/bin/sh` to a
path containing a space, and GDB's build system does not quote it -- the build
then fails with `C:/Program: No such file or directory`, which looks like a
broken toolchain and is not one.

Python packages:

```sh
pip install -r requirements.txt        # Pillow, numpy
pip install -r requirements-dev.txt    # ... plus pytest
```

## The GUI board -- Blackfin, from GNU sim

```sh
sh scripts/build-bfin-sim.sh
```

The script downloads `gdb-17.2.tar.xz` if it is not already beside the
repository or in `build/`, unpacks it, applies `patches/0*-gdb-*.patch`,
configures the simulator only, builds it, and installs it as `bin/cdj-run`.
Re-running it is a no-op on an already-patched tree.

Point it at a tarball or an unpacked tree if you have one:

```sh
sh scripts/build-bfin-sim.sh /path/to/gdb-17.2.tar.xz
sh scripts/build-bfin-sim.sh /path/to/gdb-17.2
```

The script builds `libbfd`, `libiberty` and `libopcodes` first, because
`bfin/run` links against them and only GDB's top-level makefile knows to build
them. It passes `MAKEINFO=true` throughout -- `makeinfo` builds the manuals and
nothing else, is often not installed, and without this the build stops on
`doc/bfd.info` with an error that says nothing about the simulator. A failure in
`po/` (translation catalogues, which want `msgfmt`) is likewise ignored, and the
three libraries are then checked for directly, so a real failure still stops the
script.

The result is around 17 MB. If you end up with something near 50 KB you have
libtool's wrapper script rather than the simulator; the script checks for
exactly this and refuses, because the wrapper exits 127 with no output and is a
confusing way to spend an afternoon.

The simulator is compiled `-O3 -march=native -g` with `--enable-sim-inline`
and `--disable-sim-assert`, because the interpreter's inner loop is the whole
cost of a run. `--march=nocona` gives a binary that runs on any x86-64,
`--opt=-O2` the upstream level, `--profile` a `-pg` build installed as
`bin/cdj-run-pg` for gprof, and `--reconfigure` throws the object tree away
first -- configure only runs when there is no `config.status`, so a change of
flags does nothing without it. `CDJ_SIM_CFLAGS` replaces the flags outright.

## The MAIN board -- SH-4, from QEMU

QEMU is not vendored here. Clone it wherever you like:

```sh
git clone --depth 1 https://gitlab.com/qemu-project/qemu.git /c/qemu-src
sh scripts/build-qemu-sh4.sh /c/qemu-src
```

The script applies `patches/qemu-sh-intc-priority-imask.patch`, copies
`emulator/qemu/*` into `hw/sh4/`, adds the meson and Kconfig lines if they are
not there, configures for `sh4-softmmu` only, and builds. All of it is
idempotent, so re-run it after every change to `emulator/qemu/`.

QEMU **11.x or newer** is required.

Put the result on `PATH`, or point `CDJ_QEMU` at it:

```sh
export CDJ_QEMU=/c/qemu-src/build/qemu-system-sh4
```

Check the board is there:

```sh
qemu-system-sh4 -M help | grep cdj2000
```

### Rebuild immediately, and check the timestamp

If a build of the board fails, the **old** `qemu-system-sh4` is still on disk
and every later run keeps measuring the old behaviour, while looking exactly as
though your change did nothing. This is the most expensive silent error in the
project. After each build, check the binary is newer than the source you
changed.

## Tests

```sh
pytest -q                 # or: python -m pytest -q
```

Without firmware, roughly two dozen tests skip and the rest run. The skips are
the ones that read a real firmware image; see FIRMWARE.md. `tests/cstub/`
compiles `emulator/qemu/cdj2000_input.c` on the host against stub QEMU headers,
so the panel input protocol is tested without QEMU at all.

## Where things land

| directory | holds | in git? |
|---|---|---|
| `bin/` | the Blackfin simulator you built | no |
| `build/` | the unpacked GDB tree and its object tree | no |
| `firmware/` | boot images built from your own firmware | no |
| `packets/` | stimulus the tools generate | no |
| `runs/` | frames, logs and captures from runs | no |

All five are in `.gitignore` and all five can be moved: `CDJ_BIN_DIR`,
`CDJ_BUILD_DIR`, `CDJ_FIRMWARE_DIR`, `CDJ_PACKETS_DIR`, `CDJ_RUNS_DIR`.

With one exception. The board files in `emulator/` name their flash image as
`firmware/gui-flash-image.bin`, and the simulator resolves that itself, against
the working directory the launchers set -- the repository root. So
`CDJ_FIRMWARE_DIR` moves everything the Python side reads but not that one file.
Copy the board file and pass `--board` if you need it elsewhere.

## macOS migration

The `codex/macos-nxs` branch is bringing this project to macOS. Install Xcode
Command Line Tools and Homebrew `make`, `gmp`, and `mpfr` before running
`scripts/build-bfin-sim.sh`. On macOS the script uses `gmake`, BSD-compatible
tar arguments, system zlib and Homebrew's arithmetic libraries. `CDJ_MAKE`
can select another GNU make executable. Patch 03 fixes BSD sed module
registration, Blackfin sign extension on LP64 hosts, and a missing POSIX
header in SPORT socket handling. Applied-patch checksums are stored in the
GDB source tree; changing a patch requires a fresh source tree.

Extract a locally supplied combined NXS updater:

```sh
python3 -m tools.cdj_gui.nxs_container /path/to/C2KNXS.UPD firmware/nxs/updates
python3 -m tools.cdj_gui.extract firmware/nxs/updates/C2KGUI.UPD firmware/nxs
```

The splitter validates the four manifest lengths, NXS component identities,
and all CRCs before writing byte-identical component files. `container.json`
records the source and component SHA-256 hashes. All generated files remain
under ignored `firmware/`.

This does **not** establish NXS MAIN compatibility. The QEMU board and proxy
still contain original-CDJ-2000 firmware assumptions. The NXS MAIN register
map, native MAIN–GUI link, flash geometry and real C674x DSP execution remain
migration work. GUI ELF loading bypasses the resident flash bootloader.

Initial input verification on this Mac: all four extracted component files
match the earlier independent NXS parser byte-for-byte. The five GUI ELF
regions also match its materialized LDR data. The earlier harness additionally
staged four INIT bytes at `0xff800060`; this fork omits them. That boot-stage
assumption needs reconciliation before claiming equivalent startup behavior.

Verified on Apple Silicon macOS (2026-09-08): `bin/cdj-run` builds as a
Mach-O arm64 executable, and rerunning the build succeeds. A five-second
stock NXS GUI run reaches core-timer and SPORT initialization and exits at
the configured wall-clock limit without a reported CPU exception. It reports
unconnected GPIO ports 2 and 4; this is not a full boot or display validation.
Host suite: 156 passed, 42 skipped. Skipped tests need additional firmware or
runtime fixtures. QEMU has not yet been built on this Mac for this fork.

### Diagnosing GUI startup on macOS

Patch 04 enables the file-backed framebuffer on POSIX hosts. The build
explicitly clears `SDL_CFLAGS`: SDL autodetection otherwise selects a backend
that does not publish the PPM file consumed by the Python viewer. Existing
SDL-built `gui.o` is rebuilt when switching to the file backend.

Useful viewer overrides:

```sh
--env BFIN_FAST_LZSS= --env BFIN_STATS=5 --env BFIN_EXCEPTION_TRACE=1 \
--env BFIN_GUI_TRACE=1 --env BFIN_GUI_FRAME_TRACE=1
```

Empty values remove an environment variable. Statistics include guest PC,
instruction count, display DMA, completed/published frames and received link
bytes. The native NXS GUI-only run now publishes frames and displays
`E-8709: COMMUNICATION ERROR`, with `link_rx=0`, because MAIN is not connected.
Some image regions are visibly corrupt; framebuffer publication is verified,
but image fidelity is not. This run does not demonstrate full-system boot.

### Experimental full NXS board pair

QEMU source tested on macOS: `55347990687e7bc5b6b0d624f290025726e8fbfa`
(version 11.1.50). The interrupt trace-event patch context was refreshed for
that revision. Build with:

```sh
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
```

`cdj2000nxs-main` has 128 MiB SDRAM, matching the NXS boot stack at
`0xac000000`. The original `cdj2000-main` retains 64 MiB. With 64 MiB, stock
NXS reset code entered the error loop at `0xa00004e6`; with 128 MiB it reaches
the application and sends GUI status records. Peripheral fidelity and DSP
execution remain incomplete; this profile is explicitly experimental.

The GUI launchers now enable `BFIN_PARALLEL_WRITEBACK=1` by default. Without
it, a parallel arithmetic/store packet writes the newly calculated register
value instead of the old value, corrupting resource relocation. The
instruction-level regression in `tests/bfin/parallel-store.s` fails with
stored value 11 instead of 7 when the fix is disabled, and passes when enabled.
The NXS GUI-only screen becomes readable with this fix.

Prepare `firmware/nxs/main-firmware.bin` using the validated NXS loader, then:

```sh
python -m tools.cdj_main.nxs_vm runs/nxs-check --seconds 60
```

This diagnostic runner connects QEMU's request and status channels on ports
5980 and 5982, with panel control on 5984. It uses stock firmware traffic,
without a link proxy or replay file. Each new run directory contains commands,
explicit environment overrides, logs, traffic and the framebuffer. A zero
exit means the bounded run completed and produced a frame; it does not certify
full boot, input fidelity or actual C674x DSP execution.

Direct-link verification (45 seconds, `runs/nxs-linked-two-channels`):
stock NXS MAIN and GUI exchanged bidirectional traffic and the GUI rendered
`Not Loaded.` with `E-7010: DSP DEVICE ERROR`. The communication error cleared.
This identifies the next blocker as DSP support; it does not validate audio
or the original model's behavioral DSP against NXS. Host suite including the
new Blackfin instruction regression: 157 passed, 42 skipped.

### NXS DSP host port

The NXS profile now uses `cdj2000_nxs_hpi.c` instead of the original player's
behavioral DSP/window. It models the verified 32-bit host accesses: HPIC at
`0x0c000000`, HPIA at `0x0c040000`, incrementing HPID at `0x0c080000`, and
fixed HPID at `0x0c0c0000`. The supported target is global L2 RAM
`0x11800000..0x1183ffff`, with HWOB set and byte addressing. The DMAC preserves
the host data-register address while the target address increments inside UHPI.

The stock boot writes 13,781 words: the 55,120-byte stage-1 program plus its
entry pointer. The captured program at `0x11801da0` matches independent MAIN
extraction byte-for-byte, SHA-256
`b6d4e3237a0409d13a523b1e445cc40113dda31467ac5e18dbc1ff6b2a83f736`.
This was repeated with `CDJ_REQ_STATUS_FRESH=0`; the NXS runner now explicitly
disables that upstream request-rewriting option and records its environment.

DSPINT records the handoff and writes `dsp-l2.bin` in the run directory.
**Full C674x execution and the boot ROM remain incomplete** (see the partial
core milestone below). No fake DSP-ready
reply is generated. Additional memory regions, DSP-side registers, reset-line
behavior and interrupt-driven host transfers remain work. The current host
port supports the observed initial upload, not arbitrary DSP firmware yet.

`tests/test_nxs_hpi.py` drives the actual QEMU device and its DMAC via qtest,
using synthetic memory and a stopped CPU; no Pioneer firmware is required.

### Partial C674x execution core

`cdj_c674x.c` decodes instructions independently from TI SPRUFE8B. It currently
supports the observed 32-bit startup forms of MVK/MVKH, AND, floating-point
control-register MVC (including ILC/RILC setup), register/relative branches and full/compact CALLP, ADDKPC, ADD/SUB (.L/.S), OR (.L), MVK (.D), CMPEQ/CMPGT, scalar loads/stores, LDNW/STNW, LDDW/STDW and LDNDW/STNDW, SHL/SHR/SHRU (.S, 32-bit) and NOP. Mixed fetch
packets use the header layout and halfword p bits; compact .L ADD/SUB
support the low/high register set and cross path. Compact MVK, immediate
CMPEQ and all L2c logic/comparison forms are decoded; predicate destinations
remain in the low register set even when operands use the high subset.
Compact register BNOP uses B0-B15 regardless of RS. Compact register moves
implement both directions between a full register index and the selected
subset, including cross-bank operands. Compact BNOP supports signed
7-bit and unsigned 8-bit halfword displacements, optional A0/B0 predicates,
and unconditional NOP insertion even when the branch predicate is false. Compact Dpp STW/STDW
stack pushes update B15 in E1 and queue little-endian RAM writes for E3.
The memory callback currently supports checked L2 RAM writes, not device
transactions. Full-width STB/STH/STW also use the E3 write queue and
preserve neighboring bytes for narrow stores. LDNW/STNW permit unaligned
word addresses, including transfers crossing a bus-word boundary, with
scaled offsets. The core rejects parallel memory accesses in a packet with
an active nonaligned access, as required by the ISA. Doubleword transfers
use even/odd register pairs in little-endian order. LDNDW/STNDW support
scaled and unscaled offsets; aligned forms require eight-byte alignment.
Both load-result registers participate in E5 write-hazard checks. Compact
MVC writes ILC from the selected B register subset; full-width MVC can write
ILC/RILC. Their four-cycle availability timestamps are recorded for the future
loop engine. Unconditional SPLOOP execution is supported below; other loop
forms and control-register reads remain incomplete. Full-width LDW, LDB/LDBU and LDH/LDHU support linear immediate/register offsets and
pre/post pointer updates: address generation in E1, RAM sampling in E3 and
register writeback in E5. Narrow loads select little-endian byte/halfword
lanes, apply signed or unsigned extension, and scale offsets by element size. PROT inserts four NOP cycles. Pending memory
operations freeze with the core on an unsupported packet. Circular addressing,
RAM arbitration for simultaneous overlapping accesses, and register-result
collisions stop explicitly. Read callbacks currently require stable, side-effect-free RAM. Every execute packet
reads the pre-packet state; writes commit together. Branches take effect after
five delay slots, including inserted NOP cycles. Up to six taken branches
can be in flight, with targets captured at issue; multiple taken branches in
one packet are rejected. A taken branch cancels the active non-reloading loop
buffer, including when issued before SPLOOP (TI section 7.14). Unsupported instructions stop with PC and opcode, without committing part of the
failed packet. This is not a complete ISA, pipeline, privilege or interrupt model.

On DSPINT the host-port device reads the uploaded entry pointer at global L2
base and starts this core. **That is an explicit boot-ROM handoff abstraction;
the unavailable boot ROM is not executed.** Initial core state is deterministic
zero initialization, not a measured ROM register snapshot. The implemented
startup path initializes the registers it uses.

The connected stock MAIN/Blackfin run in `runs/nxs-c674x-pinmux-bank` uploads
13,781 words and executes 597 packets / 699 cycles. It commits both SYSCFG
unlock keys and all 20 PINMUX register writes, ending with PINMUX19 = 2
(the documented UHPI_HRDY output selection). Execution stops on unsupported
instruction `0x4683e000` at `0x11801f20`. This decodes as `[B1] SPLOOPW 14`
(TI SPRUFE8B section 7.10); predicate-controlled loop scheduling remains missing. Standalone replay
and the connected run agree: `B15=0x11805ae8`, `B14=0x11806900` and
`B3=0x118027c0`. The bounded GUI run exits 0 and produces a frame; this is
not proof of a completed firmware boot.

Compact MVK.S now decodes the scattered unsigned eight-bit constant
(Figure F-24), and compact ADD.L handles the signed immediate encoding
(Figure D-5, where zero encodes +8). Tests cover all 256 constant values,
both register banks/subsets, all immediate offsets, cross-path reads and
32-bit wrapping.

Compact immediate-offset loads and stores (Figures C-8/C-9) use the same
E1/E3/E5 pipeline as full-width instructions. The decoder handles the header's
primary/secondary data sizes, signed narrow loads, register subsets, fixed
A/B4-7 pointer selection, and byte-scaled nonaligned doubleword offsets.
Protected loads retain their four NOPs. Faults report the original firmware
opcode. Synthetic tests exercise every data-size selection, both register
subsets, delayed stores, protected loads, unaligned doubleword layout, and
out-of-bounds failure. Other compact memory addressing forms remain incomplete.

`cdj_c6747_syscfg.c` implements KICK0R/KICK1R reset, readback, ordered unlock,
and relock on a wrong key, following TI SPRUH91D sections 10.2.1.2 and 10.5.5.
PINMUX0-19 provide protected configuration storage with zero reset values
(section 10.5.10). Locked writes leave configuration unchanged. Writes
must be aligned 32-bit transfers and apply at the CPU store's E3 phase;
checking a queued store has no effects. PINMUX19 reserved bits 31:4 must be
zero; nonzero programming stops as unsupported, rather than inventing behavior
for reserved bits. Tests cover each register's reset, protection and readback,
the bank boundary and reserved-bit rejection. Physical pin routing, other SYSCFG
registers, PSC, and clock hardware remain unimplemented. Privilege enforcement
is not implemented: this startup path assumes supervisor access, pending a
complete CPU privilege model. No physical pin behavior or readiness is implied
by configuration storage. MMIO read-clear and other side-effecting reads
require a future bus transaction interface.

Synthetic tests cover reset/readback, reversed keys, wrong-key relocking,
unsupported writes/addresses, protected pinmux writes, and adjacent CPU stores
unlocking only at E3. The complete suite reports 161 passed / 42 skipped;
the CPU and SYSCFG/pipeline harnesses also pass AddressSanitizer and
UndefinedBehaviorSanitizer.

An earlier run (`runs/nxs-c674x-compact`) recorded a Blackfin GUI double fault
at `0x00d290a6` after illegal instructions at `0x00d0cf42`. Its immediate
repeat completed 15 seconds with GUI exit 0. This intermittent issue remains
open. The older prototype's listing omits register-extension bits, some cross
paths and extended memory selectors; it must not be an execution oracle.
DSP peripherals, remaining ISA/control behavior, interrupts and audio are
still incomplete. No DSP-ready result is fabricated.

`tests/test_c674x.py` compiles an independent, synthetic instruction harness.
It checks sign extension, pre-packet reads, predicates, branch/NOP timing,
ADDKPC return addresses, mixed-width packet boundaries, compact register
banks and arithmetic, full-index compact moves, relative and compact BNOP branches, ADD/SUB wraparound and operand order, signed comparison boundaries, CMPEQ, stack-store timing and captured source values,
doubleword order, byte/halfword lanes and sign extension, scalar-store byte preservation, unaligned transfers across word boundaries,
second-word bounds failures, doubleword register pairs and offset scaling,
upper-register hazards, nonaligned packet restrictions, ILC/RILC setup,
register-subset selection and parallel control-write conflicts,
shift counts through and beyond 32 bits, load E1/E3/E5 timing, PROT, memory/register hazards,
alignment/bounds failures, and atomic unsupported-packet
stops. The same harness
also passes Clang address and undefined-behavior sanitizers.

### Unconditional SPLOOP execution

`emulator/qemu/cdj_c674x_loop.c` schedules overlapping iterations as original
packets are fetched. The core decodes full-width unconditional SPLOOP and
SPKERNEL, tracks ILC, expands body NOPs into cycles, replays buffered
instructions and merges post-loop instructions during the epilog. Register
BNOP supports a return during draining: its NOP cycles still allow buffered
instructions and delayed memory effects to finish. SPKERNEL stage bits are
reversed per TI Table 3-29; excessive fetch delay is capped at the epilog end.

Composite packets preserve each instruction's original PC/header, read one
pre-cycle register snapshot and commit together. Faults retain their originating
PC and leave the composite packet uncommitted. Loading is limited to 14
original packets, 48 cycles and eight simultaneous instructions. Complete
functional-unit conflict checks are still missing; register and memory hazards
covered by the interpreter are checked. SPMASK, reload/nested loops,
SPLOOPD/W, protected/control instructions in the body, interrupt draining and
restart remain unsupported and must not be counted as complete loop emulation.

The independent schedule test matches all 14 cycles of TI SPRUFE8B Table 7-1.
CPU tests additionally decode the complete copy program through normal
`cdj_c674x_step`, verify eight copied words and pointer updates, and verify
zero iterations perform no memory operations. Tests cover ILC readiness,
composite faults, CALLP return addresses and pending-branch restrictions,
false-predicate BNOP timing, six simultaneous in-flight
branches, captured targets, and branch cancellation of SPLOOP. The connected NXS run
above verifies the firmware's loop and return path. Sanitizers and the
160-test host suite pass (42 platform/dependency tests skipped).

### Deterministic DSP replay diagnostics

Replay the pre-execution 256 KiB L2 dump captured by a connected NXS run:

```sh
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-c674x-pinmux-bank/dsp-l2.bin runs/dsp-debug
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-c674x-pinmux-bank/dsp-l2.bin runs/dsp-break --break-pc 0x11801f20
```

Output directories must be new. The tool compiles the current C674x and SYSCFG
models with the system C compiler into a temporary directory. It snapshots and
hashes the dump, records source/header hashes in `manifest.json`, and writes
`trace.jsonl`: step PCs/cycles and loop/branch state, committed/rejected writes,
and a final report containing all 64 general registers and pending memory counts.
`--steps` sets the maximum number of core steps (default 10000). `--break-pc`
stops before execution at that PC; a breakpoint in a buffered loop refers to
the core program counter, not every buffered instruction's original address.
Fault, step-limit and breakpoint outcomes are distinct. Exit zero means the
report was produced; it never means firmware booted.

This is headless DSP replay from an explicit ROM-handoff abstraction. It does
not run MAIN/Blackfin concurrently, execute the boot ROM, or supply missing
peripheral responses. Both global and local L2 addresses are supported.
`runs/dsp-replay-pinmux` reproduces 597 packets / 699 cycles and the same
SPLOOPW stop as the connected run. `runs/dsp-replay-before-sploopw` stops just
before it, with two pending stores and no CPU fault. The real-firmware replay
also passes AddressSanitizer/UndefinedBehaviorSanitizer. Synthetic tests check
byte-identical repeat traces, local L2 aliases, breakpoint/limit semantics, and
malformed-input rejection. This replaces the untracked development probe as
the repeatable diagnostic entry point; interactive stepping/resume and general
memory inspection remain future work.

Latest development checkpoint: compact NOP 1..8 is implemented, including
loop-buffer dynamic length. Replay and a rebuilt connected MAIN/Blackfin run
(`runs/nxs-compact-nop-connected`) agree at 606 packets / 708 cycles, stopping
at compact `SPMASK S1`, `0x2d66` at `0x11801f26`. Repeated replay traces are
byte-identical. The connected GUI exits 0 after 15 seconds and publishes a
frame; full boot remains incomplete. The full suite passes 164 tests with
43 skips; the CPU harness passes address/undefined-behavior sanitizers.

### Static format inventory for batch implementation

The GNU format table comes with the GDB 17.2 source extracted by the Blackfin
build. Scan the uploaded stage-1 range, or narrow to the current loop:

```sh
.venv/bin/python -m tools.cdj_dsp.inventory runs/nxs-compact-nop-connected/dsp-l2.bin runs/formats.json --formats build/gdb-17.2/include/opcode/tic6x-insn-formats.h --range 0x11801da0:0x1180f4f0 --trace runs/dsp-compact-nop-replay-2/trace.jsonl
.venv/bin/python -m tools.cdj_dsp.inventory runs/nxs-compact-nop-connected/dsp-l2.bin runs/loop-formats.json --formats build/gdb-17.2/include/opcode/tic6x-insn-formats.h --range 0x11801f20:0x11801f70
```

The output file must be new. Ranges are global L2 addresses with exclusive
ends; `--range` can repeat. The scanner honors compact layout, skips fetch
headers and applies SAT/BR/DSZ bits before matching the most-specific GNU
format masks. It records hashes of input, format table, tool and optional trace.
It reads the table as data and accepts only numeric OR expressions and the
three documented macros; it never evaluates arbitrary code from the table.

This discovery-only inventory scans beyond execution blockers without changing
firmware state. Matches are format candidates, not validated mnemonics or
reachability. The broad range includes data. Replay PC visits include idle and
loading cycles, so they are not executed-instruction counts or coverage proof.
Use the nearby families to plan a coherent implementation batch, consult TI
operand/timing rules, run focused CPU tests and deterministic replay, then run
the connected boards after material progress. Unsupported execution still stops.

### SPMASK and predicate batch

Immediate `SPKERNELR` reload is supported for predicated full-width `SPLOOP`
when the initial ILC and each true-reload RILC can finish the loading stage.
The first termination boundary must occur at buffered cycle 3 or later so its
four-cycle-old predicate sample is available.
The core samples the outer predicate four cycles before the last kernel stage
boundary, starts a new buffer invocation on the following cycle, and issues
its prolog alongside the preceding epilog. Post-body fetch ends after the
reloading interval or a taken branch's final delay slot, while the buffered
invocation continues. The implementation uses the existing checkpointed CPU
layout. `tests/cstub/c674x-loop.c` checks the seven-cycle TI Example 7-15
schedule also exercised by ghidra-c6000; `tests/cstub/c674x.c` checks one and
two reloads, an II=2 overlap, ILC/RILC state, a post-body branch and the
unsupported-count boundary. Predicated `SPLOOPD`, `SPMASKR`, zero or short
RILC values, and interrupt restart across reload still stop explicitly.

Full and compact SPMASK decode to the eight unit-mask bits (compact has six).
During loading, masked program-memory instructions execute once and are not
buffered. During loading/draining, masked buffered instructions are suppressed
before the scheduler's eight-operation limit, and program-memory replacements
merge into the same architectural commit. Zero masks and masks outside the
loop are supported. Misplaced masks fail atomically. Unit classification uses
TI appendices C-G; unknown formats stop when masking requires their unit.
The full-width SPMASK opcode follows GNU's correction to SPRUFE8B; compact
encoding follows Figure H-8. Behavior follows sections 7.11 and 7.15.

Compact predicated MVK (Figure G-3) supports L/S/D, both banks/subsets,
constants zero/one, and A0/!A0/B0/!B0. Tests cover all combinations. Further
tests verify masked versus unmasked operations across all six compact mask
bits, replacement during loading and draining, zero/idle masks, misplaced-mask
rollback, and filtering a 16-candidate issue down to eight before capacity
checking. Reload/SPMASKR, interrupt restart and masked multicycle operations
remain unsupported. General functional-unit conflict checking remains incomplete.

Replays `runs/dsp-mask-batch-1` and `-2` match byte-for-byte. The rebuilt
connected run `runs/nxs-mask-batch-connected` agrees at 608 packets / 710 cycles,
PC `0x11801f34`, opcode `0x42140264`, stopping on an unmapped LDW from
PSC0 PTSTAT (`0x01c10128`). See SPRUH91D Table 8-6 and section 8.6.10.
GUI exit 0/frame publication after 15 seconds is not full boot. PSC transition
modeling is the next peripheral batch. Full suite: 164 passed, 43 skipped;
the CPU harness passes address/undefined-behavior sanitizers.

### PSC register-transition batch

The shared `cdj_c6747_psc.c` model implements both controllers' MDCTL/MDSTAT,
PTCMD and PTSTAT, following SPRUH91D chapter 8. The firmware's requested NEXT
states are latched at GO, PTSTAT becomes busy, then MDSTAT changes when the
transition finishes. Populated modules and restricted interconnect modules use
Tables 8-1/8-2. Reads are side-effect-free, and write validation never mutates
state. Tests exercise deferred transitions, GO snapshotting, both controllers,
status bits, absent modules, restricted states and invalid accesses.

Explicit approximations: a transition takes eight successful DSP steps, with
one tick after each step including the committing step; this is not physical
clock timing. Intermediate MDSTAT states are not modeled. Repeated GO while
busy is ignored. PTCMD readback is assumed zero for the observed read/OR/write
sequence; the manual labels GO write-only. Physical clock/reset routing is not
implemented. DSP LRST starts deasserted under the ROM-handoff abstraction;
self-reset, FORCE, emulation interrupt enables, auto-sleep/wake and domain
power-down stop as unsupported. PSC state is not proof of peripheral readiness.

The same batch adds linear ADDAB/H/W and SUBAB/H/W, register/immediate forms,
with same-bank operands and modular scaled arithmetic. Circular addressing
remains unsupported. The loop source-packet limit was corrected: storage is
indexed by LBC (TI 7.7.3.3), so NOP/setup fetches can exceed 14 while dynamic
length, total tags and simultaneous issue remain bounded.

Repeated firmware replay reaches 833 packets / 958 cycles, stopping at
`0x118021a0`, opcode `0x031c3ec0`. Full suite: 165 passed / 43 skipped;
CPU and PSC harnesses pass address/undefined-behavior sanitizers.
The rebuilt connected MAIN/Blackfin run `runs/nxs-psc-batch-connected` agrees
at the same stop and logs firmware MDCTL/GO writes for EDMA, GPIO and HPI.
Its GUI exits 0 with a frame after 15 seconds; full boot remains incomplete.

### Address/logic and fetch-header batch

ADDAD register/immediate forms extend address scaling to eight bytes (TI
SPRUFE8B page 117 explicitly specifies no SUBAD). OR and XOR cover the
full-width L/S/D register and signed-immediate variants with cross paths.
Tests cover both banks, cross paths, sign extension and wrapping.

Loop setup/body and CALLP companion checks now distinguish actual compact
branches from unrelated instructions sharing a BR header. PROT checks apply
to loads rather than all instructions in the fetch packet (TI section 3.10).
Mixed-header tests verify normal arithmetic/MVK can execute with PROT/BR set.
Protected loads within the loop body remain explicitly unsupported.

Identical deterministic replay traces (`runs/dsp-logic-batch-1` and `-repeat`)
reach 1,003 packets / 1,156 cycles, PC `0x11802ca8`, word `0x020c0264`.
The unmapped read is McASP0 PDIR at `0x01d00014` (C6747 datasheet memory map;
SPRUH91D section 24.1.4). This begins the next peripheral initialization batch;
it is not evidence of audio operation or full boot. Suite: 165 passed / 43
skipped; CPU address/undefined-behavior sanitizer passes.
The rebuilt connected run `runs/nxs-logic-batch-connected` agrees at the same
PC/opcode and packet/cycle counts; its GUI exits 0 with a frame after 15 seconds.

### McASP pin-register and bit-field batch

`cdj_c6747_mcasp.c` models PFUNC/PDIR/PDOUT latches and PDSET/PDCLR write
aliases for all three ports, following SPRUH91D 24.1.3-8. Serializer counts
are 16/12/4 (SPRS377F Table 6-43), not the ambiguous summary's 16/9.
Reset is zero. Check-phase writes have no effects; commit updates the latch.
Changing direction/function does not modify PDOUT. PDIN, PDCLR reads,
clock/reset release, physical routing, FIFO/DMA, serializer status and audio
remain unsupported. Nonzero reserved writes stop rather than being ignored.

Full-width CLR/SET/EXT/EXTU implement the eight constant/register variants
from SPRUFE8B, with both banks/cross paths. Independent bit-by-bit tests cover
all 1,024 parameter pairs, including full-width masks, reversed CLR/SET fields,
zero shifts and sign extension. Invalid register count upper bits stop;
this is conservative for CLR, whose text is less explicit than SET/EXT/EXTU.
False predicates do not fault on unused invalid counts. Compact forms remain
future work. Inventory `runs/mcasp-next-inventory.json` scans
`0x11802ca0:0x11802f00`: 180 candidates in 24 format families, not proof that
all candidates are code or supported instructions.

Reproduce this checkpoint (choose new output directories):

```sh
.venv/bin/pytest -q
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-logic-batch-connected/dsp-l2.bin runs/NEW_REPLAY
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_CONNECTED --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Suite: 166 passed / 43 skipped. CPU and McASP C harnesses pass Clang
`-fsanitize=address,undefined`. Replays `runs/dsp-mcasp-fields-1` and
`runs/dsp-mcasp-fields-repeat` are byte-identical, reaching 1,051 packets /
1,214 cycles. Rebuilt connected run `runs/nxs-mcasp-fields-connected` matches
the stop at `0x11802d5c`, word `0x02140264`, LDW from GPIO DIR45 at
`0x01e26060` (SPRUH91D Table 20-1). All three McASP ports receive firmware
configuration writes. This proves neither physical audio operation nor full boot.

### GPIO and compact bit-field batch

`cdj_c6747_gpio.c` supplies DIR/OUT_DATA/SET_DATA/CLR_DATA, rising/falling
trigger masks and BINTEN configuration to both DSP execution paths. Eight
16-bit banks are present (C6747 datasheet Table 6-8); generic-manual bank 8
is absent. Reset DIR is all ones, other supported latches zero. Aliases share
readback and implement write-one set/clear, with no effect during validation.
See SPRUH91D sections 20.3.2-11 and register map Table 20-2 (the earlier GPIO
checkpoint's Table 20-1 citation identifies bit mapping, not register offsets).

Physical input reads, INTSTAT, edge detection, pinmux and DSP/EDMA interrupt
delivery remain unsupported. BINTEN/trigger readback is configuration only.
The model retains output-latch writes with DIR=input, interpreting the manual's
"writes do not affect pins" as a drive restriction, not a latch-write mask;
that interpretation needs hardware confirmation. Reserved BINTEN bits stop.

Compact Sc5 CLR/SET/EXTU and S2ext signed/unsigned byte/halfword extracts
follow SPRUFE8B Figures F-27/F-28. Tests cover all 32 Sc5 constants, both banks
and register subsets, and all S2ext variants. EXTU Sc5 writes A0/B0 even with
the high operand subset. These extend the preceding full-width family.

Suite: 167 passed / 43 skipped. CPU and GPIO sanitizer harnesses pass.
Replays `runs/dsp-gpio-fields-1` and `runs/dsp-gpio-fields-repeat` are identical:
1,064 packets / 1,238 cycles, stopping at PC `0x11802d90`, compact `0x105d`,
loading I2C0 ICMDR at `0x01c22024` (SPRUH91D 22.3.9). Use the preceding
replay/build/connected commands with fresh output paths to reproduce.
Rebuilt connected run `runs/nxs-gpio-fields-connected` matches this stop,
records GPIO DIR45=`0xffffbfff`, and exits GUI 0 with a frame after 15 seconds.
This is not a completed firmware boot or working audio.

### I2C GPIO-mode and long-offset memory batch

`cdj_c6747_i2c.c` implements reset-held ICMDR and ICPFUNC/ICPDIR/ICPDOUT
plus write-only PDSET/PDCLR aliases, following SPRUH91D 22.3.9 and 22.3.16-21.
It additionally permits IRS=1 in idle GPIO mode (PFUNC=1, slave STT=0,
optional FREE). TI specifies constant-one internal SCL/SDA in GPIO mode.
All transfer modes, status/data reads, external input and interrupt behavior
remain unsupported. Changing pin function requires IRS=0. Reserved writes
stop; PDSET/PDCLR reads stop because TI labels readback indeterminate.
The firmware actually configures this GPIO/idle sequence; it does not perform
an I2C transfer in the newly covered path. Research notes suggest no DSP I2C
slaves, but that has not been independently reverified from the schematic here.

Full-width long-offset LDB/LDBU/LDH/LDHU/LDW/STB/STH/STW use Figure C-5
and section 3.9.3 of SPRUFE8B: B14/B15 base, unsigned scaled 15-bit offset,
no pointer update, .D2 execution with either data register bank. Tests cover
all eight forms, both data banks/bases, offsets 0/1/31/256/32767, wraparound
address calculation, signed loads, E3/E5 delay and narrow-store preservation.

Suite: 168 passed / 43 skipped. CPU and I2C address/undefined sanitizers pass.
Replays `runs/dsp-i2c-long-1` and `runs/dsp-i2c-long-repeat` are identical:
1,088 packets / 1,289 cycles, PC `0x11802dc8`, compact `0x114d`, loading
PLLCTL at `0x01c11100`. Use the earlier commands with fresh output paths.
PLL clock/reset timing is the next peripheral batch; full boot/audio remain
incomplete. No I2C ACK or clock-ready response was fabricated.
Rebuilt connected run `runs/nxs-i2c-long-connected` matches the same stop and
records the I2C0 GPIO/idle configuration. GUI exits 0 with a frame after the
15-second bound. QEMU's binary timestamp was checked against changed sources.

### Automated replay equivalence gates

Use one compilation for two fresh-process runs, with an optional saved baseline:

```sh
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-i2c-long-connected/dsp-l2.bin runs/NEW_GATE --verify-repeat --expect-trace runs/dsp-i2c-long-1/trace.jsonl
```

`trace.jsonl` and `repeat.jsonl` are retained alongside `gate.json`, which
records SHA-256 hashes, individual comparison outcomes and the aggregate result.
Either mismatch exits 1, without changing the baseline. Omit `--expect-trace`
when intentionally advancing firmware execution; keep `--verify-repeat` as
the deterministic gate. Exact equality includes writes and final state, not
just packet counts or the stopping PC. It is equivalence evidence, not an
architectural oracle, full boot test or proof of audio. Matching fault traces
can pass. The original no-gate diagnostic exit behavior remains unchanged.

The compiler reads snapshotted source/header bytes and those same bytes supply
manifest hashes, preventing a concurrent worktree edit from mislabeling the
compiled input. Two executions share only the binary and immutable input dump;
each starts a fresh process. This is not a partial CPU-state resume checkpoint.

`runs/dsp-pll-baseline-gate` passes repeat and baseline checks with trace hash
`5b7056642605e60cb33cf2761e01ab1a8a3df304ffc481c20a1a4dc135fd7fc8`.
Tests verify matching faults, baseline mismatch exit 1, artifact retention,
missing-baseline rejection and the existing breakpoint/step-limit distinctions.
No new connected run is needed for this tool-only change; the last connected
firmware evidence remains `runs/nxs-i2c-long-connected`.
Full suite after the gate change: 169 passed / 43 skipped.

### PLL reset-held configuration and reserved-bit conflict

`cdj_c6747_pll.c` stores 13 configuration registers per SPRUH91D 7.4.3-15.
PLLCTL starts at POR `0xf2`; divider defaults are explicitly tested. Using
POR defaults at the missing-ROM handoff is an approximation, not measured
handoff state. Read-only reserved-one bits 7:6 remain one. Reserved bit 4 must
be written one per Table 7-5. PLLEN and PLLRST release stop until clock
transition semantics exist. GO/status/clock outputs remain unmapped. Programmed
divider latches do not imply active clock ratios. PLL_MASTER_LOCK starts
unlocked; attempts to set it through unimplemented SYSCFG registers still stop.

```sh
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-i2c-long-connected/dsp-l2.bin runs/NEW_PLL --verify-repeat
```

`runs/dsp-pll-config-1` passes repeat equivalence at 1,117 packets / 1,351
cycles, PC `0x11802e14`, compact `0x1144`. Firmware writes PLLCTL=`0x1c0`,
clearing bit 4. The model rejects that write under the current TI manual.
Investigate older device documentation/vendor initialization code and the
instruction sequence; do not silently mask or accept the discrepancy.
The current reference is [TI SPRUH91D, Table 7-5](https://www.ti.com/lit/ug/spruh91d/spruh91d.pdf).
Tests cover reset values, validation without mutation, configuration readback,
reserved fields, unsupported clock release and unmapped GO/status. The PLL
address/undefined sanitizer harness passes. Full boot and audio are incomplete.
Full suite: 170 passed / 43 skipped. Rebuilt connected run
`runs/nxs-pll-config-connected` agrees at 1,117 packets / 1,351 cycles and
the same stop; its bounded GUI exits 0 with a frame. QEMU timestamp checked.

### PLL legacy-bit and synthetic divider GO

The previous strict bit-4 stop is superseded by a flagged writable-latch
assumption. [Linux v6.1's primary PLL driver source](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/clk/davinci/pll.c)
defines bit 4 as PLLDIS and clears it in `davinci_pllen_rate_change`. Its
platform registration includes DA830. This supports legacy software usage,
not C6747 hardware equivalence; TI Table 7-5 still calls it reserved-one.
The C6747 forum example also clears it but is user-supplied, not authoritative
silicon documentation. No physical PLLDIS effect is claimed. Replay emits
`pll_legacy_bit4_used` and connected writes log `legacy-bit4-assumption`.

PLLCMD/PLLSTAT follow the command/status interface of SPRUH91D 7.4.16-17.
GO snapshots PLLDIV1-7, reports busy and commits the captured ratios after
eight successful DSP steps, including the committing step. This is explicitly
synthetic timing. Configuration changes and another GO while busy stop;
writing command zero does not cancel the transition. STABLE assumes the
oscillator counter completed before the missing-ROM handoff, not PLL lock.
Physical phase alignment, clock output, PLL enable/reset release and peripheral
frequency coupling remain unimplemented. Replay manifests list these assumptions.

`runs/dsp-pll-go-final --verify-repeat --expect-trace runs/dsp-pll-go-1/trace.jsonl`
passes both equivalence gates: 1,142 packets / 1,392 cycles, unsupported word
`0x3021a121` at `0x11802e94`. Suite: 170 passed / 43 skipped; PLL sanitizer
passes. Tests cover bit-4 validation/commit diagnostics, GO snapshot/latency,
busy-state rejection, command clear without cancellation and reset behavior.
Full boot/audio are incomplete; no PLL lock/ready response is fabricated.
Rebuilt connected run `runs/nxs-pll-go-connected` agrees at the same stop and
packet/cycle counts. Its GUI exits 0 with a frame at the 15-second bound.
The build exposed a missing brace around the shared peripheral-tick loop;
it was fixed and rebuilt before this connected run. QEMU timestamp verified.

### Immediate BNOP branch timing

The full-width displacement BNOP follows SPRUFE8B pp165-167: signed 12-bit
displacement from the containing fetch-packet base, scaled by two when the
fetch packet has a compact header and by four otherwise. Predicates control
the branch, not NOP insertion. Counts 6/7 truncate at the taken transfer but
run fully for false predicates. Tests cover all counts, both units, both fetch
layouts and signed offset extrema. The ordinary branch queue/pipeline handles
the five delay slots. CALLP and loop control guards recognize the new form.

`runs/dsp-bnop-immediate-1 --verify-repeat` passes at 1,147 packets / 1,402
cycles, PC `0x11802ea8`, word `0x21940264`, a protected LDW from PLLSTAT
inside a software-pipelined loop. This is the next scheduler work, not a
missing standalone load decoder. Preserve its PROT delay and predicate behavior
when extending loop loading/replay. Suite: 170 passed / 43 skipped; CPU
address/undefined sanitizer passes. Full boot/audio remain incomplete and
the existing PLL/PSC timing assumptions still apply.
Rebuilt connected run `runs/nxs-bnop-immediate-connected` matches the stop
and packet/cycle counts; GUI exits 0 with a frame at the 15-second bound.

### Protected loads during software-loop loading

PROT contributes four empty loading cycles, not four cycles freezing the loop
buffer. Lowered loads no longer carry PROT when buffered/reissued. The focused
CPU harness compares against explicit LD; NOP 4 each cycle across II=1..7,
full/compact forms, compact low/high register sets, SPMASK, and false full-width
predicates. Changing RAM checks E3 capture/E5 completion. Invalid parallel
SPKERNEL and multiple-multicycle packets reject without CPU-state changes.
Primary basis: SPRUFE8B 3.10, 7.7.3.3, SPKERNEL p481. This is model testing,
not measured silicon timing; interrupt restart and the preceding-packet
SPKERNEL restriction remain incomplete.

```sh
.venv/bin/pytest -q
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I emulator/qemu tests/cstub/c674x.c emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-prot-loop-san
/tmp/cdj-prot-loop-san
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-bnop-immediate-connected/dsp-l2.bin runs/dsp-protected-loop-1 --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-protected-loop-connected --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Use fresh output directories when repeating commands. Suite: 170 passed /
43 skipped; CPU sanitizers pass. Replay exact-repeat gate passes; connected
execution agrees at 1,162 packets / 1,420 cycles, PC `0x11802ecc`, compact
`0x0134`. The PLLCTL=`0x1c8` store attempts reset release and remains rejected.
Trace SHA-256 `65dba25926a35dc30506d2cb4bbf955a47dafb5e23c53bdfbecab93c2df5a1c0`.
Connected GUI exit 0/frame is not boot completion. No working audio or physical
PLL lock/clock propagation has been established.

### Cycle-edge PLL clock integration

Both replay and connected DSP execution now advance PLL state through the
CPU's per-cycle callback, before E3 bus effects. Multi-cycle NOP/protected-load
calls no longer count as just one PLL tick. A GO store starts its eight-cycle
synthetic delay at E3; all eight subsequent cycles must elapse. PSC remains
step-based for now. This is clock-delivery infrastructure, not physical PLL
phase alignment, lock detection, or clock propagation to devices.

The combined `tests/cstub/c6747-pll-clock.c` harness checks E3 start, split and
multi-cycle waits, and load sampling when GO completes during PROT insertion.
The core harness checks per-cycle delivery through loops, branch truncation,
no ticking on decode rejection and callback clearing at reset. Rebind callbacks
after restoring any future serialized CPU checkpoint.

```sh
.venv/bin/pytest -q
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-protected-loop-connected/dsp-l2.bin runs/dsp-cycle-clock-1 --verify-repeat --expect-trace runs/dsp-protected-loop-1/trace.jsonl
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-cycle-clock-connected --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Suite: 171 passed / 43 skipped; core and PLL/CPU address/undefined sanitizer
harnesses pass. Replay passes both equivalence gates, hash unchanged at
`65dba25926a35dc30506d2cb4bbf955a47dafb5e23c53bdfbecab93c2df5a1c0`.
PLLRST release remains unsupported at 1,162 packets / 1,420 cycles.
Rebuilt `runs/nxs-cycle-clock-connected` matches these counts and the stop;
GUI exits 0 with a frame at the 15-second bound, not full boot.

For subsequent physical timing work, visually checked SPRS377F Table 6-4 p73
specifies 1000 ns minimum PLLRST assertion and 2000*N/sqrt(M) OSCIN cycles
maximum lock wait. The radical is missing from plain-text extraction.
RRV4356-derived parent hardware research gives OSCIN=16.9344 MHz, N=1,M=23:
rounding upward gives 17 assertion periods and 418 lock-wait periods. Do not
use DSP step counts as oscillator periods after clock division/multiplication,
or expose a made-up lock status bit. Full boot/audio remain unverified.

### Bypass oscillator time and guarded reset release

PLLRST release now validates powered square-wave operation, software-selected
bypass, a 17-OSCIN-period reset minimum and catalog PLL operating ranges.
OSCIN=16.9344 MHz is board-specific; bypass CPU cycles scale by active PLLDIV1,
using the old ratio for the period ending at a GO transition. The integer
lock-wait bound is ceil(2000*N/sqrt(M)); tests cover all multipliers, boundary
rounding, invalid source/power/reference settings, reassertion cancellation,
readback writes not restarting the timer, and divider scaling. PLLSTAT is
unchanged by this wait; PLLEN/physical output clocks remain unsupported.

```sh
.venv/bin/pytest -q
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I emulator/qemu tests/cstub/c6747-pll.c emulator/qemu/cdj_c6747_pll.c -o /tmp/cdj-pll-reset-san
/tmp/cdj-pll-reset-san
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-cycle-clock-connected/dsp-l2.bin runs/dsp-pll-release-1 --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-pll-release-connected --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Suite: 171 passed / 43 skipped; PLL sanitizer passes. Repeat gate passes,
trace hash `eb9d086289e93c47dba8ac9dcbf4f4746b7d54382b1dd5c825863bd183040fdb`.
Replay and connected execution stop at 1,166 packets / 1,427 cycles,
`0x11802ed8` / `0x0134`, attempting PLLCTL=`0x1c9`. Both report 414 lock-wait
periods remaining from a 418-period bound. Only four modeled periods have
elapsed since the reset-release store committed. The attempted enable would
commit two cycles after E1 validation if accepted. This short wait is an
unresolved firmware/model/catalog discrepancy, not permission to bypass timing
or invent lock status. Connected
GUI exits 0/frame exists; full boot and audio remain incomplete.

### PLL enable latch and fractional oscillator time

PLLEN is a control latch, not a lock-status transaction. The model accepts it
only with valid source/power/reset and operating-point fields. If software sets
it before the conservative catalog wait expires, sticky `early_enable` records
the timing violation while the countdown continues. No lock bit or successful
analog acquisition is fabricated. PLL-mode DSP cycles advance oscillator time
by the exact rational `N*POSTDIV*SYSCLK1/M`; phase remainder survives across
cycles and clears on a clock-mux transition. PLLDIV1-7 can be staged for GO
after reset release; multiplier/reference/post-divider writes remain guarded.

```sh
.venv/bin/pytest -q
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I emulator/qemu tests/cstub/c6747-pll.c emulator/qemu/cdj_c6747_pll.c -o /tmp/cdj-pll-enable-san
/tmp/cdj-pll-enable-san
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-pll-release-connected/dsp-l2.bin runs/dsp-pll-enable-1 --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-pll-enable-connected --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Suite: 171 passed / 43 skipped; sanitizer passes. Repeat trace hash is
`51c7b92af18570445973ea76e14d4b5883547db6ff91795eb88680ab9a2d1047`.
Replay and connected execution agree at 1,179 packets / 1,450 cycles,
`0x11802fac` / compact `0x117d`, a read of CFGCHIP1 (`0x01c14180`). PLL
diagnostics agree at OSCIN=1430, reset-age=17, lock-wait=411,
early-enable=true. The 15-second connected GUI exits 0 and has a frame; this is
not full boot. Analog lock, physical clocks and working audio remain unverified.

### SYSCFG CFGCHIP family

CFGCHIP0-4 at `0x01c1417c..0x01c1418c` implement documented reset values,
field validity and kicker protection. CFGCHIP0's PLL lock causes mapped PLL
writes to complete without effects. CFGCHIP1 stores routing/HPI controls;
CFGCHIP2 masks read-only PHY status; CFGCHIP3 preserves reserved-one `0xff00`;
CFGCHIP4 reads zero and records AMUTE clear pulses diagnostically. Downstream
pin, clock, USB, eCAP, HPI and McASP effects remain unconnected except PLL write
lockout. Privilege checking is not modeled.

```sh
.venv/bin/pytest -q
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I emulator/qemu tests/cstub/c6747-syscfg.c emulator/qemu/cdj_c6747_syscfg.c emulator/qemu/cdj_c6747_pll.c emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-cfgchip-san
/tmp/cdj-cfgchip-san
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-pll-enable-connected/dsp-l2.bin runs/dsp-cfgchip-1 --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-cfgchip-connected --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Suite: 171 passed / 43 skipped; sanitizer passes. Repeat trace SHA-256 is
`e4143aed27d7a25d38001782924b05dab01cec7681cb6f16b118cb93f96a0469`.
Replay and connected runs agree at 1,190 packets / 1,469 cycles,
`0x11802fd0` / `0x020c0264`, reading DSP-side HPIC `0x01e10030` after firmware
sets CFGCHIP1=`0x18000` and relocks KICK. Connected GUI exit 0/frame exists is
not boot completion. HPI flow control, full boot and audio remain incomplete.

### Connected HPI chunk flow and phase-3 entry

DSP-side HPIC now shares state with the SH4 UHPI path. MAIN HPI control,
HINT/DSPINT transitions, PTDAT_H-derived GPIO4 boot phases, EMIFB register
configuration and 32 MiB SDRAM storage are exercised by focused tests. The
standalone runner accepts an explicit `--boot-phase`; this is deliberately not
an invented host-event stream.

```sh
.venv/bin/pytest -q
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-boot-phase-connected-3/dsp-l2.bin runs/dsp-phase2-callp-fixed --boot-phase 2 --verify-repeat --steps 20000
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-pack4-connected --seconds 15 --qemu build/qemu/build/qemu-system-sh4
```

Suite: 173 passed / 43 skipped. Phase-2 replay stops at the genuine HINT
host-event boundary at 1,315 packets / 1,749 cycles and repeats exactly.
Connected execution completes the observed 32 KiB HPI chunk flow, reaches MAIN
phase 3, and fails closed at 2,599,580 packets / 6,132,079 cycles,
`0x11804468` / `0xc09868c0`. The GUI exit 0 and frame only prove bounded
frontend execution. Full boot and audio remain incomplete. SDRAM command
timing/arbitration, physical HPI pins/HRDY/FIFO behavior and DSP interrupt
delivery remain unmodeled; the portable register state must not be described as
those physical effects.

### Versioned DSP checkpoints and HPI event replay

Schema-1 `.cdjdsp` files contain ABI-checked native CPU/peripheral state, full
L2 and lossless sparse 4 KiB SDRAM pages. The loader rejects wrong magic,
schema, byte order, component sizes, checksum, page counts, trailing data and
truncation. Run manifests bind every checkpoint, the ordered HPI transcript,
the genuine firmware inputs and relevant source files with SHA-256. Source
changes are recorded but remain loadable when the explicit ABI contract still
matches; this is what permits implementing the next opcode and retrying its
atomic fault packet.

```sh
.venv/bin/pytest -q
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm runs/nxs-checkpoint-connected-2 --seconds 15 --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.event_replay runs/nxs-checkpoint-connected-2 runs/dsp-event-replay-1 --verify-repeat
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-checkpoint-connected-2/dsp-checkpoints/00000000000000000065.cdjdsp runs/dsp-checkpoint-next --steps 100000 --verify-repeat
.venv/bin/python -m tools.cdj_dsp.replay runs/nxs-checkpoint-connected-2/dsp-checkpoints/00000000000000000001.cdjdsp runs/dsp-checkpoint-full-events-5 --steps 10000000 --events runs/nxs-checkpoint-connected-2/dsp-events.jsonl --verify-repeat
```

Suite: 179 passed / 43 skipped. The connected run creates 65 checkpoints in
28 MiB. Event replay gates 110,210 ordered state-changing events, 104,093 HPI
words in 14 chunks, 14 host HINT acknowledgements, 13 DSP HINT edges, one
DSPINT edge and exact initial-L2 equivalence. Checkpoint replay gates identical
traces, faults, packet/cycle counts, final serialized state, L2 SHA-256 and
logical 32 MiB SDRAM SHA-256.

The first checkpoint-driven ISA batch implements scalar ADD/SUB .D register,
constant and cross-path forms plus scalar CMPLTU. Replay and rebuilt connected
execution agree on the next fail-closed stop at 2,599,589 packets / 6,132,090
cycles, `0x118044c0` / compact `0x0c66`.

With `--events`, standalone replay can now start at any pre-fault connected
checkpoint and inject the remaining captured MAIN/HPI transcript. From the
DSP-start checkpoint it reproduces all 39 connected stops through the 14 HPI
chunks in about 2.3 seconds. Every host transaction is checked for ordered
offset/address/value/size and resulting boot-phase/HPI state. DSP-side HPIC
writes are checked at their original packet/cycle count, and every connected
stop must match PC, fault word, packet/cycle counts and HPI state. The exact
repeat gate additionally compares trace bytes, final serialized state, L2 and
logical SDRAM. `runs/dsp-checkpoint-full-events-5` passes with trace SHA-256
`da4f1ba46e3d27949a7c124b7521c1689aa76f4da3925dc4b82abc1d318b6ac5`.

Full boot/audio remain incomplete. Schema 1 is not cross-ABI portable and DSP
interrupt delivery remains absent. Event replay only injects the captured
transcript; it neither predicts new MAIN behavior nor validates physical HPI
timing, HRDY/FIFO behavior or pin-level effects.

### Compact loop, memory and external-stage replay batch

Compact SPLOOP/SPKERNEL and the C-8 through C-15 compact .D memory families
are implemented from SPRUFE8B Figures H-5, H-7 and C-8 through C-15. SPLOOPD
is recognized and remains fail-closed. The scalar .L comparison batch covers
CMPEQ, CMPGT, CMPGTU, CMPLT and CMPLTU register/immediate forms. Tests cover
all compact loop intervals, stage/cycle field reconstruction, every scalar DSZ
interpretation, aligned/nonaligned doublewords, address-update timing, RS and
pointer selection, predicates, signedness, delayed queues and rollback.

An exact-repeat replay final checkpoint can be chained as the next input. Its
gate must prove repeat trace equality and identical final state/memory; the new
manifest records hashes of the input manifest and gate and preserves original
connected firmware/source provenance. Inventory accepts either raw 256 KiB L2
or schema-1 checkpoints, checks the checkpoint FNV-1a payload checksum, restores
sparse SDRAM pages, and scans explicit L2 or SDRAM ranges.

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-compact-loop-connected/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_CONNECTED_EVENT_REPLAY --steps 10000000 \
  --events runs/nxs-compact-loop-connected/dsp-events.jsonl --verify-repeat
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_CONNECTED_EVENT_REPLAY/final.cdjdsp runs/NEW_CHAINED_REPLAY \
  --steps 1000000 --verify-repeat
.venv/bin/python -m tools.cdj_dsp.inventory \
  runs/NEW_CONNECTED_EVENT_REPLAY/final.cdjdsp runs/NEW_SDRAM_INVENTORY.json \
  --formats build/gdb-17.2/include/opcode/tic6x-insn-formats.h \
  --range 0xc000e800:0xc000ec00 \
  --trace runs/NEW_CONNECTED_EVENT_REPLAY/trace.jsonl
```

Current evidence: `runs/dsp-compact-loop-connected-events-1` gates all 39
connected DSP stops and exact repeat state/memory, then stops at
`0xc000ea94` / `0x018c0958`, `INTSP .L1 A3,A3`, after 2,600,603 packets /
6,133,200 cycles. Its trace SHA-256 is
`e9e265bbda0b8ac8217b2b813bcfaa47adc970aafbc3fa3b0f354fbf7a4f9df6`.
The rebuilt connected `runs/nxs-compact-loop-connected` records the same
boundary. Suite: 180 passed / 43 skipped; C674x address/undefined sanitizer
passes. Full boot and audio remain incomplete. Next implement the reachable
external-stage floating-point/conversion cluster beginning with INTSP as a
documented family, using checkpoint replay for iteration and another connected
run only after a meaningful batch.

### Scalar floating-point conversion and multiplication batch

`INTSP`, `INTSPU`, `SPINT`, `SPTRUNC` and `MPYSP` now share a four-cycle
E1-read/E4-write path. Their bit-exact helpers do not use host floating point.
FADCR controls conversions, FMCR controls multiply, and sticky status is
committed with the delayed result. TI rounding, denormal, underflow, overflow,
NaN, infinity, signed-zero and saturation behavior is covered by table-driven
tests. `ADDSP`, `SUBSP` and other scalar/vector floating-point instructions
remain fail-closed.

Computed E4 results use `size == 0` entries in the existing delayed-load queue;
the entry's value, due cycle, destination and pending FADCR/FMCR OR mask are
therefore already included in schema-1 checkpoints. The C structure layout and
checkpoint component sizes did not change. Tests round-trip a checkpoint with
an in-flight computed result and verify E4/E1 and delayed-status conflicts are
rejected atomically.

Reproduce focused and complete validation:

```sh
.venv/bin/python -m pytest -q tests/test_c674x.py tests/test_dsp_replay.py tests/test_dsp_inventory.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I emulator/qemu tests/cstub/c674x.c emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-fp-batch-san
/tmp/cdj-fp-batch-san
.venv/bin/python -m pytest -q
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
```

Focused tests report 18 passed; the full suite reports 180 passed / 43 skipped;
the sanitizer harness passes. The rebuilt QEMU binary was checked newer than
the C674x source. Use fresh run directories for replay and connected execution:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-compact-loop-connected-events-1/final.cdjdsp \
  runs/NEW_FP_REPLAY --steps 1000000 --verify-repeat
.venv/bin/python -m tools.cdj_main.nxs_vm runs/NEW_FP_CONNECTED \
  --seconds 15 --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_FP_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_FP_EVENT_REPLAY --steps 10000000 \
  --events runs/NEW_FP_CONNECTED/dsp-events.jsonl --verify-repeat
```

Current artifacts are `runs/nxs-fp-convert-connected` and
`runs/dsp-fp-connected-events-1`. They agree at PC `0xc0012644`, word
`0x020c9572`, after 2,600,629 packets / 6,133,250 cycles, with one in-flight E4
result retained. The replay verifies all 39 connected stops. Trace SHA-256 is
`9f75a60dbe1d4e094ae0fef9ec078219a5662b751e75c8cba0550c0a59497613` and
final checkpoint SHA-256 is
`6787287c81eafa09bf86eb32b283d16a19f0261cd0a0719fd4db61fdd6a27e10`.
The current blocker is an `.L2X` `ADDU` extended-result form, not another
floating-point instruction. Implement its associated extended integer family
as the next batch. Exact replay does not prove full boot, working audio or all
floating-point semantics; those remain incomplete.

### Reachable DSP path through SPLOOPD and shared RAM

The earlier `0x020c9572` classification above is superseded: it is a compound
MPYLI/MPYIL packet, not scalar `ADDU`. The current batch covers its compound
multiply family, scalar floating add/subtract, extended 40-bit integer
arithmetic, ANDN, and the reachable compact compare, LSDx1, non-saturating
arithmetic, and shift families. Unsupported saturating compact operations
remain fail-closed because their CSR.SAT update is delayed.

Full and compact `SPLOOPD` implement SPRUFE8B's delayed initial testing rule.
The first three loop cycles neither terminate nor decrement ILC, so the
scheduler launches `ILC + ceil(4/II)` iterations. Full setup packets may load
ILC in parallel. Compact II 1..16 is accepted. Figure H-6 conditional reload,
interrupt return, and termination while loading remain unsupported where
applicable.

SPRS377F Table 3-4 maps 128 KiB shared RAM at
`0x80000000..0x8001ffff`. Both connected and standalone DSP buses now preserve
that storage, and UHPI fixed/auto-increment data ports can address it. Schema-2
checkpoints contain complete L2 and shared RAM plus lossless sparse EMIFB
SDRAM. A schema-1 input remains readable, but its previously uncaptured shared
RAM is explicitly restored as zero; use a schema-2 checkpoint after the first
shared-RAM access.

Reproduce the focused and complete checks:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c674x.c emulator/qemu/cdj_c674x.c \
  emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-c674x-batch-san
/tmp/cdj-c674x-batch-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/dsp-checkpoint.c \
  emulator/qemu/cdj_dsp_checkpoint.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c \
  -o /tmp/cdj-checkpoint-v3-san
/tmp/cdj-checkpoint-v3-san /tmp/cdj-checkpoint-v3-san.cdjdsp
.venv/bin/python -m pytest -q
```

The full suite reports 181 passed / 43 skipped; both sanitizer harnesses pass.
The tests requiring localhost/qtest sockets need an environment which permits
local socket binds.

Deterministic first-arrival replay:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-compact-shifts-1/final.cdjdsp \
  runs/NEW_SHARED_WAIT_BOUNDARY --steps 1000000 \
  --break-pc 0x11804904 --verify-repeat
```

The recorded run is `runs/dsp-shared-wait-boundary-1`: PC `0x11804904`,
2,864,814 packets / 6,486,387 cycles, exact-repeat trace SHA-256
`62e8d267bac31019d1d03412fb0c092a4321b8938662d70099fb129438f12477`.
The instruction there is `0x0001a120`, an unconditional self-branch. It is a
stable non-ISA boundary, not proof of full boot or an identified application
wait.

Rebuild and collect connected evidence:

```sh
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_SPLOOPD_SHARED_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_SPLOOPD_SHARED_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_SPLOOPD_SHARED_EVENTS --steps 100000000 \
  --events runs/NEW_SPLOOPD_SHARED_CONNECTED/dsp-events.jsonl --verify-repeat
```

The recorded connected run is `runs/nxs-sploopd-shared-connected-3`. It has 67
schema-2 checkpoints, 110,209 ordered events, 40 DSP stops, a status-zero GUI
exit and a frame. The shared cooperative quantum is one million DSP packets;
that is a host scheduling setting and does not model wall-clock concurrency.
`runs/dsp-sploopd-shared-connected-events-1` verifies all 40 stops and repeats
trace/state/L2/shared-RAM/SDRAM exactly. Its trace SHA-256 is
`92ea9014bcec73588b3477f43927b2b51401225b2a53de912d2db10585314639`.

Full boot and working audio are still unproven. The next engineering boundary
is the control-flow-aware confirmed-code coverage report, followed by DSP
interrupt-controller/delivery work if the stable branch is an interrupt wait.

### Control-flow-aware DSP coverage gate

Replay now captures exact fetch-time packet encodings only after a DSP step
completes. It distinguishes direct and software-loop source fetches from parked
software-loop scheduler cycles and idle cycles. The automatic `coverage.json`
groups confirmed instructions by format family, raw encoding, functional unit,
side, compact/full form, cross path, predication, branch delay slots and broad
architectural state. It also records dynamic source transitions, direct
branch/call targets, loop sources, probable uncompleted targets, unsupported
faults and packet/cycle deltas. This is execution evidence, not proof of TI
semantic equivalence; unvisited memory remains unclassified.

The GNU format header is read from the GDB build by default. Pass `--formats`
only for a nonstandard build location. A directory in the input position makes
replay select its newest structurally valid, provenance-bearing checkpoint;
corrupt, incomplete and ungated replay checkpoints are ignored.

Reproduce focused, complete and sanitizer validation:

```sh
.venv/bin/python -m pytest -q \
  tests/test_dsp_inventory.py tests/test_dsp_coverage.py tests/test_dsp_replay.py
.venv/bin/python -m pytest -q
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c674x.c emulator/qemu/cdj_c674x.c \
  emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-c674x-coverage-san
/tmp/cdj-c674x-coverage-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/dsp-checkpoint.c \
  emulator/qemu/cdj_dsp_checkpoint.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c \
  -o /tmp/cdj-checkpoint-coverage-san
/tmp/cdj-checkpoint-coverage-san /tmp/cdj-checkpoint-coverage-san.cdjdsp
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tools/cdj_dsp/replay.c emulator/qemu/cdj_c674x.c \
  emulator/qemu/cdj_c674x_loop.c emulator/qemu/cdj_c6747_syscfg.c \
  emulator/qemu/cdj_c6747_psc.c emulator/qemu/cdj_c6747_mcasp.c \
  emulator/qemu/cdj_c6747_gpio.c emulator/qemu/cdj_c6747_i2c.c \
  emulator/qemu/cdj_c6747_pll.c emulator/qemu/cdj_c6747_hpi.c \
  emulator/qemu/cdj_c6747_emifb.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c \
  emulator/qemu/cdj_dsp_checkpoint.c \
  -o /tmp/cdj-replay-coverage-san
/tmp/cdj-replay-coverage-san \
  runs/nxs-sploopd-shared-connected-3/dsp-checkpoints/00000000000000000001.cdjdsp \
  10000 0 0 /tmp/cdj-replay-coverage-san.cdjdsp
```

Canonical connected-transcript replay:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-sploopd-shared-connected-3/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_DSP_COVERAGE --steps 100000000 \
  --events runs/nxs-sploopd-shared-connected-3/dsp-events.jsonl \
  --verify-repeat
```

`runs/dsp-sploopd-shared-coverage-3` passes all 40 connected-stop, trace,
coverage and final state/memory repeat gates. It contains 1,525 confirmed source
packets, 1,957 instruction addresses, 1,721 distinct encodings, 1,581 dynamic
source transitions, 114 direct targets, 56 software-loop source packets, five
probable uncompleted targets and zero unsupported faults. Trace SHA-256 is
`c74366da1e1b0d13f78ea5be70f6ee284c8fcebfd460df0a4f70a7e6a49d8562` and
coverage SHA-256 is
`4d3faa11813fd445747af2803d87863bfb9aa55a4a182865b2e6741873f0684f`.
The suite reports 184 passed / 43 skipped and all three sanitizer checks pass.
The high visit counts include stable wait/service loops; they are not full-boot
or audio evidence. Interrupt delivery and DMA/EDMA are the next DSP subsystem
boundary.

### C6747 interrupt-controller and schema-3 gate

SPRUFK5A chapter 7 backs the C6747 megamodule INTC model: four event banks,
event set/clear and masks, derived event/exception combiner views, and INTMUX1-3.
UHPI DSPINT latches device event 34. Event-to-CPU recognition/vectoring,
acknowledgement, exception/drop state, AEG, and same-cycle external-event
arbitration remain unimplemented and fail closed at their integration boundary.
Schema-3 checkpoints append this INTC state; schema-1/2 checkpoints remain
readable and initialize the absent controller to documented reset values after
the legacy payload checksum has been verified.

Reproduce the focused tests and sanitizer harnesses:

```sh
.venv/bin/python -m pytest -q \
  tests/test_c674x.py tests/test_dsp_inventory.py \
  tests/test_dsp_coverage.py tests/test_dsp_event_replay.py \
  tests/test_dsp_checkpoint_replay.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c6747-intc.c \
  emulator/qemu/cdj_c6747_intc.c -o /tmp/cdj-c6747-intc-san
/tmp/cdj-c6747-intc-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/dsp-checkpoint.c \
  emulator/qemu/cdj_dsp_checkpoint.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c \
  -o /tmp/cdj-checkpoint-v3-san
/tmp/cdj-checkpoint-v3-san /tmp/cdj-checkpoint-v3-san.cdjdsp
```

Rebuild and validate connected execution plus deterministic repeat:

```sh
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_INTC_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_INTC_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_INTC_REPLAY --steps 100000000 \
  --events runs/NEW_INTC_CONNECTED/dsp-events.jsonl --verify-repeat
```

Recorded evidence is `runs/nxs-intc-connected-1` and
`runs/dsp-intc-connected-replay-1`. The connected run has 65 schema-3
checkpoints, 110,208 events and 39 DSP stops, ending at the unchanged
25,364,865-packet / 60,779,972-cycle SPLOOPD resource conflict. Repeat gates
trace, coverage, state, L2, shared RAM, SDRAM and the appended INTC state. The
suite reports 186 passed / 43 skipped. A non-validating run-ahead reaches
Timer64P0 TGCR at `0x01c20024`; Timer64P0/1 are the next peripheral family.

### C6747 Timer64P and schema-4 gate

SPRUH91D chapter 28 backs the complete register-level Timer64P0/1 model.
Implemented semantics include documented reset/masks, coherent 64-bit counter
reads, Plus-mode counter read-reset, reset controls, interrupt-status W1C,
reload/capture and compare storage. Internal/external clock progression,
physical pins/output, watchdog reset, DMA events, and INTC delivery remain
unimplemented rather than assigned an invented timing ratio.

Reproduce focused and sanitizer validation:

```sh
.venv/bin/python -m pytest -q tests/test_c674x.py \
  tests/test_dsp_inventory.py tests/test_dsp_coverage.py \
  tests/test_dsp_event_replay.py tests/test_dsp_checkpoint_replay.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c6747-timer.c \
  emulator/qemu/cdj_c6747_timer.c -o /tmp/cdj-c6747-timer-san
/tmp/cdj-c6747-timer-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/dsp-checkpoint.c \
  emulator/qemu/cdj_dsp_checkpoint.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c \
  -o /tmp/cdj-checkpoint-v4-san
/tmp/cdj-checkpoint-v4-san /tmp/cdj-checkpoint-v4-san.cdjdsp
.venv/bin/python -m pytest -q
```

Schema-3 migration and the rebuilt connected/repeat gate:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-intc-connected-1/dsp-checkpoints/00000000000000000064.cdjdsp \
  /tmp/dsp-timer-schema4-migration --steps 1000000 \
  --events runs/nxs-intc-connected-1/dsp-events.jsonl --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_TIMER_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_TIMER_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_TIMER_REPLAY --steps 100000000 \
  --events runs/NEW_TIMER_CONNECTED/dsp-events.jsonl --verify-repeat
```

Recorded evidence is `runs/nxs-timer-connected-1` and
`runs/dsp-timer-connected-replay-1`. The latter gates 1,504 confirmed source
packets, 1,943 instruction addresses, 1,705 encodings, 1,556 dynamic edges,
five probable addresses, zero unsupported faults, and byte-identical CPU,
peripheral, Timer64P, L2/shared/SDRAM state. It ends at the unchanged validated
25,364,865-packet conflict. Exploratory run-ahead reaches SPI1 SPIGCR0 at
`0x01e12000`; SPI0/1 are the next peripheral family. Neither the exploratory
path nor a GUI frame proves boot or audio.

### C6747 SPI and schema-5 gate

SPRUH91D chapter 27 and SPRS377F section 6.17 back the SPI0/SPI1 register
model. It implements GCR reset/mode control, interrupt enable/level/flags and
vector side effects, GPIO pin function/direction/output set/clear, transmit
configuration, receive-buffer read clearing, all delay/default/format
registers, and documented reset/reserved-bit values. External input pin reads
remain unavailable until a caller supplies observed pin values. Enabled
SPIDAT0/SPIDAT1 writes remain fail-closed because no physical SPI slave,
transfer clock, receive timing, DMA request, or INTC delivery is modeled.

Reproduce focused, sanitizer, migration, connected and repeat validation:

```sh
.venv/bin/python -m pytest -q tests/test_c674x.py \
  tests/test_dsp_inventory.py tests/test_dsp_coverage.py \
  tests/test_dsp_event_replay.py tests/test_dsp_checkpoint_replay.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c6747-spi.c \
  emulator/qemu/cdj_c6747_spi.c -o /tmp/cdj-c6747-spi-san
/tmp/cdj-c6747-spi-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/dsp-checkpoint.c \
  emulator/qemu/cdj_dsp_checkpoint.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c \
  -o /tmp/cdj-checkpoint-v5-san
/tmp/cdj-checkpoint-v5-san /tmp/cdj-checkpoint-v5-san.cdjdsp
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-timer-connected-1/dsp-checkpoints/00000000000000000064.cdjdsp \
  runs/NEW_SPI_SCHEMA4_MIGRATION --steps 1000000 \
  --events runs/nxs-timer-connected-1/dsp-events.jsonl --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_SPI_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_SPI_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_SPI_REPLAY --steps 100000000 \
  --events runs/NEW_SPI_CONNECTED/dsp-events.jsonl --verify-repeat
.venv/bin/python -m pytest -q
```

Recorded evidence is `runs/nxs-spi-connected-1` and
`runs/dsp-spi-connected-replay-2`. The connected run has 65 schema-5
checkpoints, 110,208 ordered events and 39 DSP stops. Replay verifies all 39
stops and exact repeat state/memory, with trace SHA-256
`e87f6d32b6cadc3760cd733776071dbf7b41a8f73ba8e0216b9dc2cf87c1eab3`,
coverage SHA-256
`43ec202de4627962042ac0a48988ab7d48cc0ea4392eeee055fe0a52f1074695`,
and final checkpoint SHA-256
`ce921817e55e21a936c30a02d1a3d327ba14347a1344a4e25e60151ac0195464`.
Coverage remains 1,504 confirmed packets, 1,943 instruction addresses, 1,705
encodings, 1,556 edges, five probable addresses and zero unsupported faults.
The complete suite reports 188 passed / 43 skipped; SPI and checkpoint
ASan/UBSan harnesses pass.

The schema-4 migration run is `runs/dsp-spi-connected-replay-1`; its schema-5
state is 8,448 bytes, final component size 456, and exact-repeat checkpoint
SHA-256 is
`c7975769c91f1d2fcec1f8752545cbc573f4a398034296719c5d890a7e9323d7`.
A separately labeled non-validating run-ahead used the unproven +2-cycle
SPLOOPD drain hypothesis only for inventory. It observed the complete SPI1
initialization sequence (`SPIGCR0=1`, `SPIGCR1=3`, `SPIPC0=0xe01`,
`SPIDAT1=0`, `SPIFMT0=0x00021810`, `SPIDELAY=0x02020408`, interrupt/level
zero, then enable) and next reached L1PCFG at `0x01840020`, followed by
L1DCFG at `0x01840040`. The exploratory scheduler edit was removed and QEMU
rebuilt before final validation. Cache state/effects are the next peripheral
family; none of this establishes the disputed SPLOOPD timing, boot, or audio.

### Functional DSP breadth mode, schema-6 cache state and scalar multiplies

Strict timing remains the default and its validated boundary is unchanged:
25,364,865 packets / 60,779,972 cycles, PC `0xc004f306`, compact `0x2627`, at
the II=1 `SPLOOPD` L2 resource conflict. For breadth-first firmware inventory,
set `CDJ_NXS_DSP_FUNCTIONAL_TIMING=1` or pass
`--functional-dsp-timing` to `tools.cdj_main.nxs_vm` and
`tools.cdj_dsp.replay`. This opt-in mode adds two cycles to the delayed-loop
post-loop fetch. It is an explicit approximation of the unresolved epilog
timing, and generated manifests record `dsp_timing_mode` as
`functional-runahead` and `architectural_validation_eligible` as false. Never
use functional-mode packet/cycle counts as strict timing evidence.

The cache/memory-system control block now handles the C6747-valid register
surface at `0x01840000`: L2CFG, L1PCFG/L1PCC, L1DCFG/L1DCC, the documented
block-operation base/count registers, global writeback/invalidate commands,
and the valid MAR64-103, MAR128 and MAR192-223 ranges. Reset cache sizes follow
the C6747 device documentation. Reserved or unmodeled addresses fail closed,
including EDMAWEIGHT and L2ALLOC0-3. Cache contents/tags, miss and command
latency, privilege, and arbitration are not modeled. Memory is a unified,
always-coherent backing store, so cache operations complete immediately and
their status/count registers read zero; this is register-level functional
behavior, not a cache timing model.

Schema-6 checkpoints append cache-control state. Schemas 1-5 remain readable;
their original payload/checksum is validated before missing cache state is
reset. The current native state is 9,520 bytes and the final peripheral-tail
component is 1,528 bytes. Any direct checkpoint/replay harness link must now
include `emulator/qemu/cdj_c6747_cache.c`.

The full-width `.M` non-saturating scalar 16-by-16 multiply batch covers all 18
signed, unsigned, mixed-sign low/high-halfword and signed-immediate opcode
forms, on both sides and cross paths. Results publish at E2 and use the common
delayed-result conflict machinery. Saturating `SMPY` remains fail closed until
its delayed CSR.SAT effects are modeled.

Reproduce focused and sanitizer checks:

```sh
.venv/bin/python -m pytest -q \
  tests/test_c674x.py::test_c6747_cache_registers \
  tests/test_c674x.py::test_dsp_checkpoint_round_trip \
  tests/test_c674x.py::test_c674x_loop_schedule \
  tests/test_dsp_checkpoint_replay.py tests/test_dsp_inventory.py \
  tests/test_dsp_coverage.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c6747-cache.c \
  emulator/qemu/cdj_c6747_cache.c -o /tmp/cdj-c6747-cache-san
/tmp/cdj-c6747-cache-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/c674x.c emulator/qemu/cdj_c674x.c \
  emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-c674x-mpy-san
/tmp/cdj-c674x-mpy-san
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I emulator/qemu tests/cstub/dsp-checkpoint.c \
  emulator/qemu/cdj_dsp_checkpoint.c emulator/qemu/cdj_c6747_intc.c \
  emulator/qemu/cdj_c6747_timer.c emulator/qemu/cdj_c6747_spi.c \
  emulator/qemu/cdj_c6747_cache.c -o /tmp/cdj-checkpoint-v6-san
/tmp/cdj-checkpoint-v6-san /tmp/cdj-checkpoint-v6-san.cdjdsp
```

The focused Python gate reports 13 passed. A restricted-sandbox complete run
reports 181 passed / 43 skipped, with four failures and four setup errors caused
by denied localhost socket binds. Rerun the complete suite in a socket-enabled
environment before recording a final full-suite result.

Validate schema-5 migration and the unmodified strict scheduler, then rebuild
QEMU and collect a fresh strict connected run:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-spi-connected-1/dsp-checkpoints/00000000000000000064.cdjdsp \
  runs/NEW_CACHE_SCHEMA5_MIGRATION --steps 1000000 \
  --events runs/nxs-spi-connected-1/dsp-events.jsonl --verify-repeat
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_CACHE_MPY_STRICT_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4
```

Use the functional mode only for downstream discovery, and preserve its label
through standalone repeat:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_CACHE_MPY_FUNCTIONAL_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4 --functional-dsp-timing
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_CACHE_MPY_FUNCTIONAL_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_CACHE_MPY_FUNCTIONAL_REPLAY --steps 100000000 \
  --events runs/NEW_CACHE_MPY_FUNCTIONAL_CONNECTED/dsp-events.jsonl \
  --functional-dsp-timing --verify-repeat
```

Recorded connected functional evidence is deliberately non-architectural.
`runs/nxs-cache-functional-1` observes SPI1 plus L1PCFG/L1DCFG setup and stops
at 25,378,653 packets / 60,793,894 cycles, PC `0xc0038a40`, word
`0x01a86e80` (`MPYUS .M1 A3,A10,A3`). With the multiply batch,
`runs/nxs-mpy-functional-1` passes that blocker and stops at 25,380,302 packets
/ 60,796,809 cycles, PC `0xc003b3a0`, compact `0xccf7`
(`SUBAW .D2 B15,6,B15`). Each run records 65 schema-6 checkpoints, 110,208
ordered events and 39 DSP stops. Both have a status-zero bounded GUI exit and a
frame, which does not prove boot. The next breadth-oriented ISA batch is the
compact `.D` address-arithmetic family including the nearby `ADDAW`. Strict
repeat/connected validation is still required after that batch; full boot,
cache timing, interrupt delivery and audio remain unproven.

### Schema-8 DSP batch, connected gates and bounded replay

The current integrated DSP state is checkpoint schema 8.  Direct checkpoint
and replay harnesses must link `cdj_c6747_syscfg.c`, `cdj_c6747_intc.c`,
`cdj_c6747_timer.c`, `cdj_c6747_spi.c`, `cdj_c6747_cache.c`,
`cdj_c6747_mcasp.c`, and `cdj_c6747_edma.c`.  The native state size is 15,704
bytes with component sizes `6592,112,784,36,84,32,144,6,7712`.

Replay has three independent ceilings. `--steps` (also
`--instructions`) bounds successful core calls; `--packets` and `--cycles`
bound deltas from the input checkpoint. Limits are checked between successful
steps, so a multicycle instruction may cross a cycle ceiling. Manifests and
gates record the limits, progress deltas, stop state, functional
approximations, and architectural-validation eligibility. A fault also writes
`failure.json` and a resumable checkpoint. The broader fault-encoding list may
contain valid instructions rejected for resource/timing conflicts; only the
separate unsupported-encoding list is an opcode-support claim.

Run the focused and complete gates:

```sh
.venv/bin/python -m pytest -q \
  tests/test_c674x.py tests/test_dsp_replay.py \
  tests/test_dsp_checkpoint_replay.py
.venv/bin/python -m pytest -q
sh scripts/build-qemu-sh4.sh "$PWD/build/qemu"
```

The focused gate reports 25 passed.  With localhost sockets available, the
complete suite reports 192 passed / 43 optional skips.  CPU, INTC, McASP,
EDMA, and checkpoint C stubs also pass when compiled with
`-fsanitize=address,undefined -Wall -Wextra -Werror`.

Recheck old checkpoint migration with new output directories:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-addk-functional-1/final.cdjdsp \
  runs/NEW_SCHEMA6_TO_8 --steps 1 \
  --functional-dsp-timing --verify-repeat
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-mcasp-functional-2/final.cdjdsp \
  runs/NEW_SCHEMA7_TO_8 --steps 1 \
  --functional-dsp-timing --verify-repeat
```

Recorded migrations are `runs/dsp-schema6-to-8-migration-2` and
`runs/dsp-schema7-to-8-migration-2`. Both produce schema-8 checkpoints and
pass trace, coverage, state and memory repeat gates.

Collect strict connected evidence and replay all MAIN/HPI events:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_DSP_STRICT --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_DSP_STRICT/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_DSP_STRICT_REPLAY --steps 100000000 \
  --events runs/NEW_DSP_STRICT/dsp-events.jsonl --verify-repeat
```

`runs/nxs-dsp-splx-strict-1` and `runs/dsp-splx-strict-replay-1`
verify 39 connected stops and exact repeat state/memory. Strict execution
fails closed at 25,364,865 packets / 60,779,972 cycles, PC `0xc004f306`, word
`0x2627`, on the known SPLOOPD epilog register conflict. Trace SHA-256 is
`e87f6d32b6cadc3760cd733776071dbf7b41a8f73ba8e0216b9dc2cf87c1eab3`;
coverage SHA-256 is
`b95bc2267ace92d75590ca1b1473db6b76b1ff9c6f30f105eb3a237861c7f8fb`.
The repeat gate passing proves reproducibility of this failure, not boot.

For downstream breadth and coarse audio-flow discovery, preserve both labels:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_DSP_FUNCTIONAL --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4 \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_DSP_FUNCTIONAL/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_DSP_FUNCTIONAL_REPLAY --steps 100000000 \
  --events runs/NEW_DSP_FUNCTIONAL/dsp-events.jsonl \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx \
  --verify-repeat
```

`runs/nxs-dsp-tx-capture-3` and
`runs/dsp-tx-capture-replay-3` verify all 40 connected stops, exact repeat
state/memory, and a byte-identical transmit capture. The run ends by budget at 27,099,500 packets / 66,177,094
cycles. Coverage has 3,447 confirmed source packets, 4,276 confirmed
instruction addresses, 3,654 distinct encodings, 3,560 dynamic edges, 13
probable direct targets, and zero unsupported faults. Trace SHA-256 is
`18e075624287cc0f0a2cfaedb1c2bbf8f5125ea9a718f84c6abfd0f4305f971b`;
coverage SHA-256 is
`5b3874f0f2697e424be30516564694af816eea8635eac864e63c0129b3a4c80f`.

Functional audio advances active McASP1/2 slots once per 1,024 successful DSP
packets and services the resulting EDMA work atomically. This is a deterministic
coarse scheduler, not a sample clock. It consumes only genuine accepted XBUF
words, records underruns, and does not synthesize PCM, input, a host sink, or
rate matching. Both functional switches make the run ineligible for
architectural timing claims.

`--capture-dsp-tx` requires `--functional-dsp-audio` and writes canonical
JSONL only when a genuine accepted XBUF word is newly latched into XRSR.  It
records unsigned serializer words, not interpreted PCM.  The connected capture
has 3,348 records: 1,674 for McASP1 serializer 0 and 1,674 for McASP2 serializer
3.  SHA-256 is
`72c70733a4b7016cde086e322d754cdd17dbc4e13ca652a79f06e5667ab0e3b2`.
All words are zero, and no nonzero source-ring payload was observed.  This is
evidence that DMA-to-McASP transport is active but the upstream firmware path
has not supplied audio; it is not working-audio evidence.  Replay validates
record structure and sequence and compares the capture byte-for-byte on
`--verify-repeat`.

The strict schedule around `0xc004f2ec..0xc004f306` matches SPRUFE8B's
SPLOOPD formulas: II=1, 128 iterations, dynamic length 7, and
`SPKERNEL 3,0`. The post-loop `MVK .L2 1,B4` overlaps a buffered
`MV .L2X A3,B4`. A two-cycle delay avoids the conflict but is not supported by
the ISA documentation. Keep it functional-only pending C6747 ISS/EVM or
D810K013 IERR/LBX evidence. TSR.SPLX now tracks ordinary loop active/idle state,
and the implemented B IRP return subset handles returned SPLOOPD counting and
parallel-operation suppression. Returned SPMASK reversal, SPLOOPW return, full
retained-buffer state, and interrupt-time loop drain still fail closed or remain
unimplemented. Do not relax the strict resource check or describe the
functional run as full DSP parity, full boot, or working audio.

## Schema-9 SPI1, SPLOOPW and interrupt-return batch

Current-source connected regression replay:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-return-bnop-connected-1/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_RETURN_CONNECTED_REPLAY --steps 100000000 \
  --events runs/nxs-return-bnop-connected-1/dsp-events.jsonl \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx --verify-repeat
```

Recorded `runs/dsp-return-bnop-connected-replay-1` verifies all 71 connected
stops, repeat trace, state/memory and transmit capture. It ends at 58,099,500
packets / 120,392,283 cycles with zero execution faults. Trace SHA-256:
`06cfce913e51f11843c1109526a3cedf9c0aed3c608f6433beab6223500ec325`;
coverage SHA-256:
`a2ee57e7136f977630ac064b78c3d03bc7a456b0639193ea58db152581b1e792`.
Coverage contains 4,469 source packets, 5,396 instruction addresses, 4,585
encodings and 33 probable addresses. Coverage is explicitly ineligible for
architectural validation because the run uses exploratory timing/audio.

The returned immediate BNOP regression (SPRUFE8B 7.13.2) can be repeated with:

```sh
.venv/bin/python -m pytest -q tests/test_c674x.py
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-wm8740-connected-replay-3/final.cdjdsp runs/NEW_RETURN_BNOP \
  --steps 100000 --functional-dsp-timing --functional-dsp-audio --verify-repeat
```

Recorded `runs/dsp-return-bnop-replay-1` passes exact repeat with zero faults,
ending at 56,199,500 packets / 117,064,013 cycles. Trace SHA-256:
`6b1f1766c1a0d0295f2b394ba4110913ec58eb3fca09d9c159f7f22d98b3fc3f`.
The rebuilt 15-second connected regression is
`runs/nxs-return-bnop-connected-1`; it retains all functional timing labels.

Explicit mode transitions inherit the input checkpoint's exploratory status.
Even with strict execution selected, the manifest, gate and coverage remain
ineligible for architectural validation if the checkpoint has exploratory
ancestry. Approximation descriptions propagate across repeated resumes.
Automatic strict selection excludes these checkpoints. Use a strict-origin
checkpoint for strict evidence; repeat equivalence alone cannot establish it.

For automatic checkpoint selection, pass a run directory as the first replay
argument. Candidates are ordered by modification time, then path. Selection
validates checkpoint checksum/provenance and matches the requested DSP timing,
audio mode, and (when supplied) event-transcript hash before accepting a
candidate. Newer incompatible candidates are skipped. An explicit checkpoint
path remains available for deliberate mode-transition investigations.

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/nxs-dsp-wm8740-sploopw-functional-3 runs/NEW_AUTO_CONTINUATION \
  --steps 1000000 --functional-dsp-timing --functional-dsp-audio --verify-repeat
```

Coverage reporting distinguishes decoder rejections from other execution
faults. Reanalyze existing traces without rerunning firmware:

```sh
.venv/bin/python -m tools.cdj_dsp.coverage \
  runs/dsp-splx-strict-replay-1/final.cdjdsp \
  runs/dsp-splx-strict-replay-1/trace.jsonl /tmp/NEW_STRICT_COVERAGE.json \
  --formats build/gdb-17.2/include/opcode/tic6x-insn-formats.h
```

This reports 1,504 confirmed source packets, 1,943 instruction addresses,
six probable addresses, one execution fault, and zero unsupported encodings.
The terminal register conflict remains in `faults` and makes coverage
`validation_eligible` false. A repeat gate passing is independent of successful
execution. Reserved predicates and unsupported modes likewise remain execution
faults rather than automatically becoming missing-opcode inventory.

The historical schema-8 limitations immediately above are superseded by this
section.  Rebuild QEMU and run the focused/full validation before collecting
new evidence:

```sh
sh scripts/build-qemu-sh4.sh build/qemu
cc -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iemulator/qemu tests/cstub/c674x.c \
  emulator/qemu/cdj_c674x.c emulator/qemu/cdj_c674x_loop.c \
  -o /tmp/cdj-c674x-san
/tmp/cdj-c674x-san
.venv/bin/python -m pytest -q
```

Schema 9 adds the board-specific WM8740 state to the checkpoint.  Exercise the
one-step schema-8 migration with a fresh output directory:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-tx-capture-replay-3/final.cdjdsp \
  runs/NEW_SCHEMA8_TO_9 --steps 1 \
  --functional-dsp-timing --functional-dsp-audio --verify-repeat
```

`runs/dsp-schema8-to-9-migration-2` records the current passing migration gate.
The new functional timing behavior is deliberately bounded: SPI1 accepts only
the observed WM8740 configuration, interrupt-return SPMASK rebuild requires
exact stable program bytes, ISR-local loops may replace retained validation
metadata, and interrupt entry drains already-issued effects with a minimum
empty interval.  The generated manifests label every approximation; strict
mode remains fail closed where exact timing is absent.

For a bounded continuation from the post-SPLOOPW checkpoint:

```sh
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/dsp-wm8740-sploopw-nested-replay-1/final.cdjdsp \
  runs/NEW_POST_INTERRUPT_REPLAY --steps 5000000 \
  --functional-dsp-timing --functional-dsp-audio --verify-repeat
```

The recorded `runs/dsp-post-interrupt-pipedown-5m-replay-2` gate passes exact
repeat state/memory and ends by its five-million-step limit at 33,100,114
packets / 76,600,256 cycles with no fault.  Trace SHA-256 is
`5121630448324766fed2c412853a65b212d66bb85f6feb1bfcfe4b71260066d1`.

Collect and independently replay a complete connected transcript:

```sh
.venv/bin/python -m tools.cdj_main.nxs_vm \
  runs/NEW_SCHEMA9_CONNECTED --seconds 15 \
  --qemu build/qemu/build/qemu-system-sh4 \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx
.venv/bin/python -m tools.cdj_dsp.replay \
  runs/NEW_SCHEMA9_CONNECTED/dsp-checkpoints/00000000000000000001.cdjdsp \
  runs/NEW_SCHEMA9_CONNECTED_REPLAY --steps 100000000 \
  --events runs/NEW_SCHEMA9_CONNECTED/dsp-events.jsonl \
  --functional-dsp-timing --functional-dsp-audio --capture-dsp-tx \
  --verify-repeat
```

Recorded evidence is `runs/nxs-dsp-wm8740-sploopw-functional-3` plus
`runs/dsp-wm8740-connected-replay-3`.  It verifies all 69 connected stops and
ends by phase budget at 56,099,500 packets / 116,888,772 cycles.  The replay
passes exact trace, coverage, state/memory and transmit-capture repeat checks.
Trace SHA-256 is
`7fe32d6bda3f0dce5c285c9b116f299e328cf8ae842a5ac0690159c1417537a8`;
coverage SHA-256 is
`f5809e49118533611c3c125b7493fa33506f02519ea1886222163511c217de39`;
the 59,988-record transmit SHA-256 is
`f84639275492f868d73c8bff2009bcbaaaa7a76b911ff133dd36b230489a17b4`.
All captured serializer words remain zero.  These are functional breadth and
reproducibility results, not cycle accuracy, full boot, or working audio.
