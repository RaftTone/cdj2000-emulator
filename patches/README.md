# Patches

Patches against two upstreams. Each one exists because the CDJ's firmware
exercises something the upstream emulator gets wrong or does not implement, and
each says what was measured before and after. See `../THIRD_PARTY.md` for the
licence of each.

| patch | against | applied by |
|---|---|---|
| `qemu-sh-intc-priority-imask.patch` | QEMU 11.x | `scripts/build-qemu-sh4.sh` |
| `qemu-sh-intc-priority-order.patch` | QEMU 11.x, after the IMASK patch | `scripts/build-qemu-sh4.sh` |
| `qemu-sh-tmu-stop-reset.patch` | QEMU 11.x | `scripts/build-qemu-sh4.sh` |
| `01-gdb-17.2-bfin-parallel-dsp32alu.patch` | GDB 17.2 | `scripts/build-bfin-sim.sh` |
| `02-gdb-17.2-bfin-cdj2000-board.patch` | GDB 17.2 | `scripts/build-bfin-sim.sh` |

The two GDB patches are ordered and must be applied in that order. Both build
scripts apply with `--forward`, so re-running them on an already-patched tree is
a no-op.

---

## `qemu-sh-intc-priority-imask.patch` — QEMU, SH-4 interrupt semantics

Four related omissions in QEMU's SH-4 interrupt path. All are invisible to the
r2d/Linux guest the code was written for, and all are fatal to a uITRON RTOS
that masks with `SR.IMASK`.

1. **`sh_intc_get_pending_vector` ignored interrupt priority.** It returned the
   first pending source unless `IMASK` was `0x0f`, so raising `IMASK` masked
   nothing. The patch adds `prio` to `struct intc_source`, defaulting to `0x0f`
   so existing controllers behave exactly as before, and accepts a source only
   while `prio > imask`.

The follow-on `qemu-sh-intc-priority-order.patch` selects the highest-priority
eligible source, using source order to break ties. The first patch only filtered
by `IMASK` and still returned the first pending source. Both patches are needed
for the board's programmed priority levels to arbitrate simultaneous requests.

`qemu-sh-tmu-stop-reset.patch` clears a stopped timer's asserted IRQ and adds a
reset callback for each three-channel TMU block. A reset stops the underlying
ptimers, restores their count and limit, clears their interrupt levels, and
restores the visible registers. Without this, a pending timer line can survive
a guest reset and interrupt the new boot before it has initialized its handlers.

2. **`sh_intc_write` never recorded the level a priority register carries.** It
   only asked whether the field was non-zero, i.e. whether the source was
   enabled, so with (1) in place every source stayed at the default level of 15
   and outranked everything. The patch stores the field value into
   `source->prio`, clamped to 15, and traces it as `sh_intc_prio`. This is what
   lets a board model its real priority registers instead of hardcoding levels:
   the CDJ's guest programs 8 for its tick and 4 for its links, and both now
   come out of the registers rather than out of the board file.

3. **Accepting an interrupt did not raise `SR.IMASK` to that interrupt's
   level.** Real SH-4 does, which is what lets a handler save `SR` on entry and
   restore it on exit while staying masked against its own source until it has
   acknowledged the device.

4. **A declined interrupt was never offered again after an `rte`.** (1) makes
   declines possible for the first time — stock QEMU has no way to say "not
   now" — and QEMU only ever kicks the CPU out of a chained run of translation
   blocks on the *rising edge* of an interrupt line, in `cpu_interrupt()`. A
   periodic timer whose handler did not run keeps its underflow flag set, so
   there is no second edge. The one offer that does happen lands in the `rte`
   delay slot, where it is declined because delay slots are indivisible, and
   the block holding that delay slot then chains straight into the resumed code
   through `lookup_and_goto_ptr` without ever returning to the interrupt check.
   The patch makes `use_exit_tb` true for `TB_FLAG_DELAY_SLOT_RTE`, so an `rte`
   delay slot leaves the CPU loop and the restored `SR` is re-tested.

5. **A declined interrupt was reported as taken.** `superh_cpu_exec_interrupt`
   called `superh_cpu_do_interrupt` and returned `true` whenever
   `CPU_INTERRUPT_HARD` was set, even when the interrupt was then declined
   inside -- masked by `SR.BL`, or, with (1), by `IMASK`. TCG treats `true` as
   "state changed": it drops the block it was about to chain from
   (`*last_tb = NULL`) and takes the big lock at the next block boundary. The
   controller is level sensitive, so a pending-but-masked timer keeps the flag
   up for the whole run and every basic block of the guest went back through
   the slow loop. The patch tests the delay slot, `SR.BL` and the pending
   vector first and returns `false` for a decline; the line is re-tested at the
   next exit to the loop, which every `SR` write and every `rte` forces (4).

6. **The main loop could not wait for less than a millisecond.** On Windows
   QEMU waits in `g_poll`, which takes whole milliseconds and rounds up, and
   the scheduler adds its own slack. The RTOS tick is a 1 ms TMU period; a
   tick that fires late enough to overlap the next is coalesced by the level
   sensitive line and the guest loses it. Measured ~820 of 1000 a second with
   the board raising the process timer resolution to 0.5 ms, 720 without. The
   patch arms a high-resolution waitable timer for any wait under 4 ms and
   hands its handle to `g_poll` alongside the other wait objects, so the wait
   ends when the timer fires or when anything else becomes ready. Measured
   with it: 1000 a second, alone and with the GUI board attached.
   `CDJ_MAIN_LOOP_HIRES=0` switches it off. At the full rate the GUI board,
   whose interpreter is thirty times slower than the real chip on real work,
   overflowed its exception stack -- the long-standing double fault at
   `0x00b99196`, a push with `SP` already in MMR space -- in three runs of
   three, until its simulator started carrying link announcements over
   (`BFIN_LINK_ANNOUNCE_STICKY`, set by the launchers); with that, three of
   three boots at the full rate were clean.

### What each one costs

Without (3) the CDJ's RTOS never survives its first timer tick. Its interrupt
prologue at image `0x2e6426` installs an `SR` with `IMASK = 10`, does its work,
then restores the entry `SR` at `0x2e644a` — and with `IMASK` back at 0 the
still-asserted timer re-enters on the very next instruction, the `rts` at
`0x2e644c`. Measured: 170 645 interrupts in 12 s, every one of them interrupting
that same `rts`, and the device flag never cleared because the handler body was
never reached.

After the patch the same run reaches its tasks: the trap log grows from 300
lines to 4026 and MAIN initialises its GUI link.

Without (4) the RTOS system time at `0x04fc45ec` freezes for good within the
first thirty seconds — measured stalls at 203 and at 6851 ticks — while the
timer keeps underflowing and `CPU_INTERRUPT_HARD` stays set. Measured with all
four: 105 185 ticks over 150 s, a steady ~700/s, and the GuiCom mode word at
`0x04c06fb0` advances from 0 to 1 for the first time.

The interrupt priority level itself is not the lever, and raising it does not
substitute for (4). `TMU0_PRIO` must stay in 1..10: measured at 11, 14 and 15
the tick outranks the prologue's own `IMASK = 10`, re-enters it, and the guest
resets after ~2 s; at 10, 8 and 4 the guest runs, and 8 is the level the
firmware itself programs. The stall happens at every level in the working range,
because the decline itself — not which level caused it — is what wedges the CPU.

---

## `01-gdb-17.2-bfin-parallel-dsp32alu.patch` — one instruction, one packet

GNU sim's `decode_dsp32alu_0` commits one packed 16-bit ALU result directly to
its destination data register. That is incorrect when the DSP32 ALU operation
occupies slot 0 of a Blackfin 64-bit multi-issue packet: later slots must read
the register values from before the packet, and all register writes commit only
after all three slots execute.

CDJ-2000 GUI firmware relies on this:

```text
R0 = R0 -|- R0 || [P5] = R0 || NOP
```

With `R0 = 7`, real packet semantics store `7` through `P5` and then commit the
ALU result `R0 = 0`. Unpatched GNU sim commits zero early, so the parallel store
incorrectly writes zero and the firmware enters a fatal loop.

The patch uses the simulator's existing deferred `STORE` queue, matching the
other DSP32 ALU cases in the same decoder. A runtime trace after applying it
must show the memory store receiving `0x00000007` followed by the queued
register write committing `R0 = 0`.

This one is an ordinary correctness fix and is suitable upstream on its own,
which is why it is a patch of its own rather than part of the board work.

---

## `02-gdb-17.2-bfin-cdj2000-board.patch` — the GUI board

Sixteen files, twelve under `sim/bfin/` and four under `sim/common/`, adding no
new files. Everything it adds is off unless an environment variable switches it
on, so an unconfigured simulator behaves as it does upstream. The patch's own
header lists every piece; two are worth knowing about before you build.

### `sim/common/dv-cfi.c` — the AMD command set, and a dump of the part on exit

Upstream implements CFI command set 1 (Intel) only. The CDJ's 2 MiB parallel
flash is an AMD part, so without this the board file is rejected with

```text
/core/bfin_ebiu_amc/cfi@0: cmdset 2 not supported
```

the simulator carries on with no flash behind the EBIU, and the firmware
double-faults a fraction of a second later at `0xffb00000`.

`BFIN_CFI_DUMP=<path>` writes the flash contents to `<path>` when the simulator
exits, with a count of the sector erases and program commands the AMD path
saw. The board file opens the image read-only and write-back through the file
needs `mmap()`, which this host lacks, so a firmware update the GUI applies to
itself -- fed over the link by MAIN's updater -- would otherwise leave nothing
to compare. It is an `atexit` hook: the simulator has to exit on its own
(`BFIN_EXIT_AFTER_WALL`), a `run_headless` budget that terminates it does not
trigger it. The visible symptom
is a crash that says nothing about a missing device — worth recognising, because
it is what you get if you build the simulator without this patch.

### `sim/bfin/dv-bfin_dma.c` — DMA interrupt delivery

The upstream channel model raises its interrupt before setting `DMA_DONE`,
asserts after every row of a 2D transfer, and never deasserts. In the CDJ
firmware this makes the DMA3 ISR read a zero status and, once status ordering
alone is corrected, re-enter EVT8 for ever before it can acknowledge the
channel.

The patch publishes completion before delivery, interrupts only when the final
2D row completes, and treats the output as level sensitive: asserted while
`DONE`/`ERR` is pending, dropped when firmware acknowledges those bits in
`IRQ_STATUS`. A zero-length pulse could otherwise be lost before the CEC
services the SIC, leaving a receive task asleep for good.

`BFIN_PPI_DMA_DELAY` paces the display DMA. A value of `5000` approximates
scanline time well enough to stop the LCD consuming one hardware event per
simulated cycle.

`BFIN_SPORT_RX_US` (off) lands a freshly armed 64-byte SPORT receive's
record that many microseconds after the arm instead of on the next tick;
the housekeeping loop's 200-byte polls are left alone. It was built because
the GUI arms fifty 64-byte status receives a second and parses one in ten,
losing them between the arm and the handler -- the shape of a completion
that lands before the task is waiting for it. Measured: at 500 us on every
receive the GUI sent no request for 45 s; at 200 us on the record receives
alone the screen stayed black until t94. The comment above the knob has the
numbers; the first such arm also logs the SPORT's clock registers
(`bfin sport: ...`), which say the transmit clock is internal at SCLK/34 and
the receive clock is MAIN's.

### `sim/bfin/dv-bfin_ppi.c` — the link's frame ceiling

MAIN's records reach the simulator over TCP as `"CDJL"` + length + body, and
the SPORT model keeps them in slots keyed by exact frame length, handing one
over when the firmware arms a receive of that length. Two limits in that model
were set from the records seen at the time, 64-byte status records and
224-byte payloads: a frame longer than **512 bytes** was thrown away together
with the whole staging buffer, and there were **eight** slots that were never
freed. The first playlist's track list broke both. MAIN sends a track list as
one 896-byte frame (its rows are UTF-16 titles), and every page of a track list
has a different length. The QEMU side had the same 512-byte ceiling in its
transmit path, and there it also swallowed the completion, which is why MAIN
went silent for the rest of every run that opened a playlist
(`runs/nxs-swap/twoboard-10-load`, 2026-09-02: the 896-byte answer announced in
9 545 status records, never delivered).

The ceiling is now 4096 bytes on both boards -- the firmware validates an
announced payload against 1..2048 halfwords at `0xb7f8d2`, and the DMA model
reads a receive in chunks of at most 4096 bytes, so that is the protocol's own
limit -- there are sixteen slots, a slot whose ring has been read out is
recycled for a new length (never the 64-byte slot, whose newest record the
model repeats), and both losses say so once on stderr (`link: unusable frame
length`, `link: no slot for a ...-byte frame`). `BFIN_LINK_RX_CENSUS` prints
`recycled=` and `noslot=` with the rest. Behaviour for the lengths seen before
is unchanged: same slot assignment in the same order, depth 1.

One more rule came out of the same runs. A payload was handed over once and
then refused as stale, because a payload is one transmission -- but MAIN's
status record keeps announcing its last answer after it went over, the
firmware arms the receive for it again, and on the wire a fetch poll brings
that answer back. Refusing wedged the GUI in its boot-time browse loop in
half the runs: `runs/nxs-swap/trackload-6-jogenter` armed the announced
48-byte receive 45 277 times and was handed 42, with MAIN answering every
poll with a status record, and the SD key never got past it. Now, while
MAIN's own newest record announces exactly that length, the newest payload
of the length is handed over again (`repeat_payload=` in the census);
`BFIN_LINK_REPEAT_ANNOUNCED=0` restores the refusal.

### `sim/bfin/interp.c` — guest time on the wall clock

Upstream ticks the event queue once per instruction (plus a few for slow
ones), so guest time is whatever speed the interpreter happens to run at. Here
that was 8 MIPS against a 400 MHz core: every firmware delay, time-out and
animation stretched elevenfold and a boot took five minutes. The patch delivers
ticks at the rate the wall clock advances, `BFIN_CCLK_HZ` per second, and puts
a parked CPU -- the firmware idles `main()` in a one-instruction loop that
jumps to itself, or executes `IDLE` -- to sleep until the next event is due or
a record arrives from MAIN. Guest time never runs ahead of the wall clock, so
the two boards, QEMU having always been on the wall clock, see the same time.

The comment this replaces said the idle loop must not be skipped because two
attempts made boot slower. Both attempts let guest time run *faster* than the
wall clock while parked, so the display DMA and the link retries fired at
whatever rate the host could manage and MAIN, on real time, fell behind. The
cap is what was missing, not the idea.

What goes with it: the event queue's poll event is silenced
(`SIM_EVENTS_POLL_RATE` in `sim-main.h`; at 400 MHz it would wake a sleeping
run every ten microseconds), the display DMA is paced per frame at
`BFIN_PPI_FPS` instead of one scanline per simulated cycle, and a receive with
nothing to hand over retries after `BFIN_SPORT_RETRY_US` of guest time rather
than after 5000 ticks. Measured on the GUI board alone: guest ticks track the
wall clock at exactly 400 M/s, the display scans at 57-60 fps, and the
simulator sleeps two thirds of the time. The full boot reaches the player
screen with `NO DISC` in about 35 s where the old time base still showed the
`Wait` spinner at 150 s.

Three corrections found with the profiler once the time base was in. The
PLL model (`dv-bfin_pll.c`) had no lock event, so the `IDLE` the firmware
executes for the PLL -- the first thing it does after reset -- slept until
the simulator's next internal event, 2.68 s on the wall clock at the top of
every boot; a `PLL_CTL` write now relocks after `PLL_LOCKCNT` system clocks.
An `IDLE` whose wake-up has already happened (the lock, 8 us after the write,
always has by the time the catch-up delivers it) returns at once, as the chip
does when `SIC_IWR` already shows the source, and a parked sync returns as
soon as any event fired during its catch-up. And a deliberate wait is not
lag: its ticks are delivered in full on top of the cap, so an event further
away than the cap fires once, at its time, instead of being chased in 50 ms
slices -- before this the first `IDLE` slept 2.68 s and then had 2.63 s of it
dropped.

`BFIN_TIME_BASE=insn` restores the upstream behaviour for reproducing a run
instruction for instruction, and `BFIN_EXIT_AFTER_TICKS=<n>` halts at a guest
time so two builds can be compared picture for picture: two runs of the same
build to 2e9 ticks execute exactly the same number of instructions
(410 517 504 with the fast MMU check below).

### The interpreter's own speed

Three changes to the per-instruction path, each measured on the GUI board
alone (`run_headless --packet packets/status.bin --seconds 60`, MIPS and the
wall clock to reach 1e9 guest ticks, i7-13700H):

* The ~2000 lines of environment-gated probes that ran before and after every
  instruction, and the three trace hooks on every memory access, sit behind
  one flag decided once from the environment (`bfin_probes_active`). A probe
  run is unchanged; a plain run pays one predicted branch.
* Guest loads, stores and fetches use a direct-mapped cache of host pointers,
  one entry per 4 KiB page and per map, for pages that are plain memory --
  no device, wholly inside one mapping, not straddling a modulo alias. The
  core's own path was one linked-list walk of ~40 mappings *per byte*. The
  cache is stamped with `sim_core_generation` and flushed when a mapping is
  attached or detached, which the EBIU model does on every AMGCTL write.
  Together with the probe gate: 8.4 → 18.7 MIPS, 32.7 s → 10.9 s.
* The CPLB walk -- sixteen entries per access -- keeps a memo of the last
  single hit per table; the same page with a granted permission skips the
  walk, anything else takes it, any MMU register write empties the memo.
  `getenv()` left the DMA, PPI, SPORT and CFI hot paths. 18.7 → 31.7 MIPS,
  10.9 s → 6.6 s.
* A memo hit still made two calls per access, a quarter of the interpreter's
  time in the profile. `dv-bfin_mmu.c` now publishes, per table, the page of
  its last single hit with one bit per (supervisor, write) pair the entry
  grants -- the implicit rules for MMR space and the L1 banks folded in at
  publication, or all of memory below L1 while the CPLBs are off -- and
  `bfin_mmu_fast_ok` in `bfin-sim.h` answers an aligned, granted access with
  a compare and a shift; the supervisor test reads the CEC's `IPEND` through
  a pointer. Same firmware timeline to 2e9 ticks: 28 → 40 MIPS.
* Upstream's decoders build the operand strings of every instruction they
  execute -- `val_str = imm16_str (hword)`, one `sprintf` each -- and the
  only reader is `TRACE_INSN`, off in any run that is not a trace. Found
  with gdb attached to a plain run: five breakpoints on `__mingw_pformat`,
  five hits from `fmtconst_str` under a decoder; 8 % of the run thread in
  the sampling profile. `fmtconst_str` returns an empty string unless
  `TRACE_INSN_P` says the text is wanted. Wall-clock run, GUI alone: 48 →
  45.7 ns of busy time per instruction; the instruction count to 2e9 ticks
  is unchanged.

* The display path converted every scanline of every 60 Hz scan to RGB and
  scored every pixel for the best/detail outputs whether or not those were
  in use -- 27 % of the simulator's wall clock while the firmware idled, as
  the built-in sampling profiler (`BFIN_SAMPLE_PROFILE`, interp.c) showed.
  A line is converted only when its raw pixels changed, and scored only
  while a best/detail output is set. The instruction-counted regression run
  to 2e9 ticks dropped from 15.0 s to 7.1 s.

The regression test for all of this is `BFIN_TIME_BASE=insn
BFIN_EXIT_AFTER_TICKS=2000000000` from `packets/status.bin`: every build must
halt with a byte-identical frame (409 927 680 instructions before the poll
event was silenced, 191 758 336 after, because `IDLE` now warps to the next
real event).

### Two diagnostics that are there because they found something

`SIM_LOAD_TRACE` prints requested against actually-written length per loaded
section. The loop in `sim-load.c` discarded `do_write`'s short count, so a
section that was only partly written looked exactly like one fully written, and
nothing downstream could tell the difference.

`SIM_CORE_CENSUS=<lo>:<hi>` names which mapping served each stretch of a write.
A hardware device that accepts bytes and drops them still returns a full count,
so "written == requested" does not mean "stored".

### Not included

The tree this came from also carries changes under `sim/sh/` — an attempt to run
the CDJ's MAIN board on the SH simulator rather than on QEMU. That line of work
is not part of this emulator and is deliberately left out.

### Reproducing the patch

The patch is generated, not hand-written, and it is checked the same way: unpack
a pristine `gdb-17.2`, apply `01` then `02`, and the result must match the tree
it was taken from byte for byte. `.github/workflows/patches.yml` runs the
applying half on every push and fails on any fuzz or reject.

## 05: opt-in fresh-only live-link diagnostic

`BFIN_LINK_FRESH_ONLY=1` prevents the live SPORT frame cache from delivering a
previously consumed record again. It also bypasses the cached announcing-record
hold and the last-status fallback. New MAIN records retain their original bytes;
this does not rewrite announcement fields or CRCs. Legacy behavior remains the
default. The 200-byte housekeeping model and the independently timed emulators
are unchanged, so this is **not** a hardware clock model or a boot fix.

`tests/test_bfin_link_cache.py` compiles the actual patched cache function from
the local simulator build and verifies fresh/legacy consumption, replenishment,
length matching and repeated-payload gating. It requires that build and a C
compiler. The first 90-second NXS test with this flag showed E-8709 and never
consumed the announced 240-byte payload; it did not reach the later database
request phase. Do not enable this flag in normal launches on that evidence.

## 06: opt-in DMA register timeline

`BFIN_DMA_MMR_TRACE=1` logs CONFIG and IRQ_STATUS accesses with the guest PC,
simulator time, and pre-write channel state. It does not change DMA behavior.
This distinguishes payload cancellation from a five-second communication
timeout; logging can perturb scheduling, so compare untraced controls too.

## 07: publish the SIC mask before forwarding interrupts

All four SIC register layouts previously forwarded pending interrupts using
the old mask, then stored the new mask. Masking a serviced DMA interrupt could
therefore latch it again before firmware acknowledged DMA_DONE; unmasking a
pending source could fail to deliver it until some later event.

Store the mask first. The BF531 NXS GUI uses the **bf537** register-layout path.
`tests/test_bfin_sic_mask.py` executes each actual IMASK case body with a
forwarding stub, covering masking, unmasking, pending shared sources and no
pending source. It requires the locally patched simulator source and a compiler.
This is a mask-ordering fix, not a complete SIC/CEC pulse/acknowledgment model.

## 08: skip full-frame comparisons after unchanged scans

Track whether a scanline was converted during the current frame. If none was,
the RGB buffer cannot have changed, so skip comparing it with the previously
published frame. A changed scan still compares RGB before publishing: changes
in unused raw pixel bits must not create duplicate images. Initial publication,
forced publication, scoring, raw output, scan timing, and DMA consumption retain
their existing behavior.

`tests/test_bfin_frame_change.py` compiles the actual locally patched file
backend and feeds black, unchanged, changed, and restored scanlines. It checks
publication counts and that unchanged scans avoid the full RGB comparison.
Rebuild with `sh scripts/build-bfin-sim.sh` to apply the patch.

## 09: move the LZSS accelerator off the instruction stack

Extract the optional decompressor into a non-inlined helper. Its 4 KiB window
no longer enlarges every `interp_insn_bfin` call, including NXS runs where the
accelerator is disabled. Preserve all bank checks, memory writes, return-PC
updates, and the original pre-probe bypass when acceleration declines.

`tests/test_bfin_cold_lzss.py` exercises the built helper with a synthetic
space-producing bank and an unsupported source. Fixed-tick firmware benchmarks
and the generated dispatcher prologue are recorded in `ITERATION_ANALYSIS.md`.
Rebuild with `sh scripts/build-bfin-sim.sh` to apply the patch.

## 10: retain the SPORT receive capture descriptor

Keep the run-local receive capture file open, flushing each complete SPRX
record immediately. Normal exit closes the descriptor; abrupt termination
still retains already flushed records. Write/close failures are reported.
The capture path is a process-lifetime setting, as used by the NXS launcher.
`tests/test_bfin_sport_capture.py` checks descriptor reuse, record visibility,
and byte-exact output after both normal and abrupt process exit.

## 11: retain the SPORT transmit capture descriptor

Keep the `BFIN_SPORT_TX_OUTPUT` capture file open for the process, flushing each
complete `SPTX` record immediately and closing it at normal exit. The packet
header, SPORT base, payload bytes, append behavior and abrupt-termination
visibility are unchanged. The destination is read once, matching the
receive-capture lifetime contract. `tests/test_bfin_sport_tx_capture.py`
checks one-open descriptor reuse, visibility before close, and byte-exact
normal/abrupt output. `tools/cdj_gui/benchmark_sport_tx.py` measures the
capture path against the previous open/write/close implementation.

## 13: no canned bootstrap records once the live link has spoken

The file-based peer and the `--packet` stream exist to start the GUI before MAIN
has sent anything. Under `BFIN_LINK_FRESH_ONLY=1` the live path returns nothing
whenever MAIN has sent nothing new, and the read then fell through to those
canned sources: the GUI firmware received the bootstrap status record ("nothing
mounted", halfwords 29/30 zero) on top of a live one, and thousands of canned
deliveries for announced payload lengths. A record without the announcement
landing while the firmware waits for the announced payload is exactly the fault
at `0x00b7cb50` (halfword 30 reads 0, `(0-1)*2`, the checksum walks off the CPLB
map) and the double fault at `0x00b99196` behind it: `BFIN_SPORT_RX_ADDR_TRACE`
showed 5 033 224-byte deliveries into `0x00f00040` for one or two MAIN
transmissions, and the fault dump showed a record the live trace never saw.

Once the link is open and has delivered a record, a read with nothing fresh now
returns 0 -- a quiet wire -- and the DMA waits for MAIN. Legacy delivery never
reaches that point (it repeats the last live record first), so its behaviour is
unchanged. With this patch the stock CDJ-2000 GUI 4.20 browses natively on
fresh-only delivery (encoder click 100-150 ms: categories, playlists, a
playlist's track list with artist, length, BPM and key) without the double
fault; the earlier NXS observation under patch 05 (E-8709, the announced payload
never consumed) may share this cause and has not been re-run.

## 14: one guest time with MAIN, a virtual time base, frame sync

The GUI simulator executes about thirty million instructions a second where
the BF531 executes four hundred, and on the wall-clock time base that speed was
part of the protocol: MAIN gives the GUI 3 ms to answer a record, a GUI that is
late on the wall clock is late in MAIN's time too, and MAIN falls back to
forced sends and finally resets the GUI (E-8709). This patch makes the GUI's
time its own and keeps it together with MAIN's (the QEMU half is
`emulator/qemu/cdj2000_cosim.c`, which describes the protocol and the wire):

* `BFIN_TIME_BASE=virtual` (`interp.c`): every instruction advances guest time
  by its cycles, as `insn` does, and a parked CPU (the self-jump idle loop,
  `IDLE`) jumps straight to its next event in one delivery. The display and
  the link retry are paced as on the wall clock (`bfin_wall_active` now means
  "a paced time base"): `insn` keeps upstream's scanline-per-tick display,
  which is a hardware event, a malloc and a line compare per cycle and ran
  this board at 3.3 MIPS (profiled with macOS `sample`: 10 % of the time in
  the interpreter, the rest in `memcmp`, malloc/free and `memmove` under the
  DMA). Standalone on an M1 Pro: 32 MIPS busy, 96 % of guest time skipped,
  guest time at 3.1x real time, the same picture as on the wall clock.
* `BFIN_COSIM=host:port` (`interp.c`, `dv-bfin_ppi.c`): implies the virtual
  time base, connects to MAIN's co-simulation port, and carries the link over
  that one connection -- MAIN's records are handed to the SPORT model's
  existing split into slots at their time plus the link latency, the GUI's
  requests leave stamped with its time -- and the run loop never goes past
  MAIN's reported time plus the latency. `BFIN_COSIM_QUANTUM_US` (100) is the
  latency; `STATS cosim` reports steps, waits and the wall time spent waiting.
* Frame sync (`dv-bfin_dma.c`, `dv-bfin_ppi.c`, `gui.c`): the first line of a
  2D block of the display DMA is the top of the picture. Lines were placed by
  a counter that only wrapped at the picture's height, so the picture sat
  wherever the first line after start-up landed -- 15 lines high on the
  virtual time base, the browser's title row off the top.
* `BFIN_GPIO_STRAP=<mask>:<value>` (`dv-bfin_gpio.c`): board straps on the PF
  port. PF3 is the panel revision the GUI reads after its board init; `0x8:0x8`
  runs the late "/2" board's path. Measured with GUI 4.20: the strap is read,
  the path runs and draws the same picture as the original board's. Its
  display DMA is a 250-line block where the original's is 255, which is why
  the frame sync publishes a frame at the start of every block, not only when
  the picture's height is reached (a first cut did only the latter and the
  "/2" board never published a frame).
* The GUI exits when MAIN's side of the co-simulation link goes away (closed,
  reset or failed): a normal exit, so `BFIN_CFI_DUMP` writes the flash --
  what `tools/cdj_main/gui_update_check.py` needs after an update run.
* A 64-byte receive is a status record only when it goes into the record
  buffer 0x00f00000; into 0x00f00040 it is a payload, delivered untouched and
  closing the announcement. A 32-word list answer is 64 bytes, and taken as a
  record it stalled the link for good (TAG LIST; the first browse on a big card).
* Diagnostics: `bfin_guest_seconds()` stamps `BFIN_SPORT_RX_ADDR_TRACE`, the
  `link-split` trace and `BFIN_CALL_WATCH` with guest time, and the receive
  trace also says what each receive was given (`sport rx gave N of M`). That
  is what found the three-second pause below.

Not a patch, but found with it: `tools/cdj_gui/run_headless.py` sets
`BFIN_SPORT_RX_ZERO_200=1`, which answers the GUI's 200-byte reads with zeros.
Those reads are the GUI draining the line after every record (`0xb7ef40`: arm
200 bytes, re-arm while they keep completing, stop when the DMA count stands
still), and zeros every millisecond made the line never go quiet: after each
payload the GUI drained for about three seconds before it armed the payload's
receive, while MAIN waited for the answer -- the "3.7 s per step" of every
native browse, and a key press made in that window was lost or read as a long
press. `boot_vm --cosim` removes it; the live-link runs without co-simulation
still have it.
