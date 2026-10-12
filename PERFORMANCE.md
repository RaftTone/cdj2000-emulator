# Targeted emulator optimizations (2026-09-10)

The DSP read dispatcher checks shared RAM, enabled SDRAM, and global/local L2
before probing peripheral models. These ranges do not overlap MMIO. SDRAM's
live enable bit, alignment requirements, and bounds are preserved. Peripheral
reads retain their original dispatch order.

C674x packet fetch reuses a fetch-block header within that call only. It reads
a new header when crossing a 32-byte boundary and discards the cache on return.
No guest cycles or writes occur during fetch, and the read callback contract is
side-effect-free. Thus code changes between packets remain immediately visible;
there is no persistent decoded-code cache or checkpoint ABI change.

Blackfin patch 08 skips the full RGB comparison after unchanged scans; see
`patches/README.md`. The existing changed-line conversion and changed-frame
publication policies remain in effect.

## Validation and measurement

New tests exercise packet boundaries, compact instructions, updated instruction
memory, missing headers, RAM boundaries, the local L2 alias, SDRAM enablement,
MMIO fallback, and framebuffer publication. The local QEMU and Blackfin binaries
were rebuilt using the repository build scripts.

An isolated `-O3` C674x fetch benchmark on this host fetched ten million
8-instruction packets using a trivial side-effect-free memory callback. Across
three alternating before/after trials, the old code used 0.240-0.244 CPU seconds
and the new code used 0.203-0.208 seconds (about 15% less CPU time). Read callbacks
fell from 160 million to 90 million, or 16 to 9 per packet. This measures packet
fetch only, not instruction execution, real firmware, boot time, or overall
emulator speed. No end-to-end speedup is claimed.

The first pass left full CPU-state copies and persistent instruction predecoding
for separately measured work. The second pass below narrows packet execution's
copy without changing the CPU layout. Persistent instruction predecoding remains
unimplemented.


## Second pass: packet state and loop scheduling

`cdj_c674x_execute` now copies only the mutable scalar/pipeline prefix through
`offsetof(CdjC674x, loop)`: 3,024 bytes rather than 6,592 on this host. The
transactional copy and success-only commit remain, including atomic decode
failure. The loop schedule and retained instruction array are neither read nor
written by packet execution; loop setup/stepping still owns them. Branches can
idle a loop by changing prefix fields without clearing its retained storage.
The CPU layout and checkpoint ABI are unchanged. Future helpers called with the
partial working copy must not access its uncopied tail.

The loop scheduler visits only origins congruent to the current cycle modulo
the initiation interval, preserving their ascending order and all filtering,
capacity, finite-iteration, and predicate-epilog rules.

`tools/cdj_dsp/benchmark_core.c` supplies a reproducible synthetic benchmark
(build command in its header). Three alternating trials against the first-pass
sources, compiled with `-O3`, gave:

| Workload (3 million calls) | Before CPU seconds | After CPU seconds |
| --- | --- | --- |
| Single MVK packet execution | 0.405–0.408 | 0.181–0.187 |
| 48-origin loop scheduling, II=8 | 0.083–0.086 | 0.028–0.029 |

These are about 55% and 67% less CPU time for the selected isolated workloads,
respectively. They do not establish a whole-emulator or firmware speedup.

The loop regression test compares against the frozen original algorithm across
326,144 combinations, checking output order, rejection, and complete scheduler
state. CPU tests cover retained-tail preservation on branch retirement and
rollback after a conflicting parallel write. Core, circular-addressing, and
scheduler harnesses also passed AddressSanitizer and UndefinedBehaviorSanitizer.

## Dispatch table (2026-09-10)

`cdj_c674x_execute`'s if/else-if decode ladder became a table of 114 rows plus
one `arm_*` function each, and the function went from 1,679 lines to 805. The
transactional copy described above is unchanged - still one
`memcpy(&out, cpu, offsetof(CdjC674x, loop))` with commit only on success - and
the change was held to a byte-identical proof rather than to a passing suite:
all 65,536 compact verdicts, 1,854 decode verdicts, 32 control-register rows and
5 packet-atomicity cases are identical before and after.

It costs about **9% on the synthetic dispatch benchmark**: selection is now a
linear scan of 114 rows where the ladder short-circuited on its common cases.
That is a measured regression on an isolated workload, not an end-to-end one, and
it is recorded here rather than hidden because the obvious fix does not work as
stated - several rows constrain no bits in 6:2 (masks 0x0000000c, 0x0000001c,
0x0000003c, 0x0000010c, 0x0ffffffe, 0x0f830ffe), so they would belong in every
bucket of a `w & 0x7c` bucketing. Anyone optimising this should re-run
`tools/cdj_dsp/benchmark_core.c` and the probe sweeps together.

## Decode memo and bounded copy-in (2026-09-28)

A connected profile put the DSP
thread's time in the packet-entry state copy (70% of all memmove), the
114-row first-match arm scan (~17%), and fetch. Two exact changes:

* `cdj_c674x_arm_lookup` memoizes row selection per instruction word in a
  4,096-entry direct-mapped, thread-local table. Selection is a pure function
  of the word (`cdj_c674x_arm_table_predicates_are_word_only`), so a hit
  returns what the scan would.
* `cdj_c674x_execute` copies the scalar prefix, the live queue entries and
  8 spare slots per queue, not the whole 3,024-byte prefix. (Superseded by
  in-place execution below.)

Replay of checkpoint 400 (`tools/cdj_dsp/replay.c`, 20 M steps, `-O2`, three
alternating trials): 3.94–3.97 → 3.33–3.38 user CPU seconds, about 15% less.
Replay includes coverage/trace printing the live board does not do. The arm
scan fell from 15% of samples to 1.5%, and memmove from 23% to 17%. Traces
and final checkpoints from checkpoints 1, 25 and 250 (1 M steps each) are
byte-identical to HEAD's.

Connected: two QEMU builds from one QEMU tree and toolchain (HEAD vs this
change), four alternating 60-second headless NXS boots (`nxs_vm --seconds 60
--frame-interval 0.2`). The DSP executed 400.1M and 402.1M packets on HEAD
against 482.1M and 490.1M with the change, about 21% more in the same wall
time. The GUI exited 0 in all four, no run logged a DSP fault, and all four
final screens are byte-identical. The firmware-free QEMU integration tests
and the full pytest suite (934 passed, 33 skipped) pass on the new binary.

## In-place packet execution (2026-09-28)

After the change above, the connected profile still spent ~20% of the DSP
thread in the two per-packet memcpys, the larger of them the commit back into
the CPU. `cdj_c674x_execute` now backs up the scalar prefix and live queue
entries, runs the packet (`execute_packet`) directly on the CPU, and restores
the backup if the packet fails. Queue appends go through `append_load` and
`append_store`, which first save the slot's old bytes into the backup, so a
rollback leaves exactly the bytes a discarded scratch copy would. Bus and tick
callbacks read `cpu.cycles` and `cpu.packets` mid-packet (HPIC event records),
so those two are still written only when the packet retires. The DSP runs
under QEMU's global lock, so no other thread observes a half-executed packet.

The original scratch-copy transaction remains behind
`CDJ_C674X_COPY_TRANSACTIONS` and for an invalid incoming queue count.
`tests/test_c674x_transaction.py` builds it as the byte-exact reference, with
two new cases in which a queue append is followed by a rejected parallel
instruction. Removing either append-time save or the rollback fails the gate.

Replay traces and final checkpoints (checkpoints 1, 25, 250) are
byte-identical to the previous commit's; replay CPU 3.34–3.41 → 3.05–3.06 s.
Connected, alternating 60-second NXS boots against the previous commit:
480.1M and 484.1M DSP packets before, 561.1M and 566.1M after (~17% more;
~40% more than before the decode memo). The GUI exited 0 in every run, no run
logged a DSP fault, and every final screen is byte-identical. The full suite
passes on the new binary.

## Per-cycle path trims (2026-09-28)

Four exact changes to code that runs every DSP cycle or packet:

* The arm-lookup memo is a relaxed `_Atomic uint64_t` table (word and row in
  one entry) instead of a thread-local one; on macOS every thread-local access
  was a `_tlv_get_addr` call. The rollback extent is recorded once at the end
  of issue and passed as a parameter, instead of per-append thread-locals.
* `cdj_spi_core_tick` returns early for the valid idle SPI transfer state,
  which `cdj_c6747_spi_wm8740_advance` would only have validated. Any other
  state, invalid ones included, takes the full call.
* `cdj_c674x_loop_functional_timing()` is an inline read of an exported flag.
* `cdj_c6747_pll_tick` skips the division when no whole OSCIN period elapsed.

Replay output is byte-identical (checkpoints 1, 25, 250); replay CPU is
2.94–2.96 → 2.73–2.78 s. Connected alternating 60-second boots: 563.1M and
573.1M DSP packets before, 601.1M and 602.1M after (~6%), identical final
screens, no faults, full suite passing.

## Idle timers, SPI clock and fetch order (2026-09-28)

* `cdj_c6747_timers_tick` skips a timer whose ENAMODE12 and ENAMODE34 are
  both 0: every mode's counter, prescaler and compare path is gated off then.
  The NXS firmware configures both timers (TGCR) during boot but never writes
  TCR in a 60-second boot, so this per-cycle call did nothing.
* `cdj_spi_core_tick` replaces its per-cycle divide/modulo with subtraction;
  `cdj_spi_clock_ready` makes numerator equal the denominator, so it loops once.
* NXS `dsp_read` tests L2 (and its local alias) first. The windows are
  disjoint, and firmware executes from L2, which previously paid a cross-file
  L1D call per instruction fetch.

Replay output byte-identical; full suite passes. Connected alternating
60-second boots: 596.1M and 601.1M DSP packets before, 613.1M and 623.1M
after (~3%), identical final screens, no faults.

## Fetch-block fast path (2026-09-28)

`cdj_c674x_fetch` made one read callback for the fetch-packet header and one
per instruction word. Boards may now register `cdj_c674x_set_fetch_block`: a
side-effect-free hook returning a host pointer to a whole 32-byte-aligned
fetch block when it is plain memory the paired read callback would return
word for word, or NULL, which falls back to read. The pointer is used only
within one fetch call, so nothing is cached and code writes stay visible to
the next fetch. NXS (`dsp_memory_span`) and the replay board (`memory_span`)
register it; both helpers cover exactly their read path's RAM windows, which
are disjoint from every peripheral, and every window is 32-byte aligned.

Replay output byte-identical; replay CPU 2.65–2.72 → 2.05–2.09 s (replay's
read path probes every peripheral before RAM). Connected alternating 60-second
boots: 618.1M and 622.1M DSP packets before, 642.1M and 641.1M after (~3.5%),
identical final screens, no faults, full suite passing.

## Exact idle-loop skip and GUI head start (default in `nxs_vm`, 2026-09-28)

In the legacy scheduler a DSP activation runs until HINT or its 1,000,000
packet budget, synchronously inside the SH-4's MMIO write. After the DSP
acknowledges MAIN's command it sits in its polling loop, so ~95% of
activations ended by exhausting the budget while MAIN was frozen: 88% of the
SH-4 thread was DSP execution. (The since-removed `--fast-dsp`, i.e.
`--dsp-legacy-budget 65536`, does not help: 65,536 packets
often end before the DSP's acknowledgement, DSPINT never falls, and the DSP
is starved.)

The board (`CDJ_NXS_DSP_IDLE_SKIP=1`; `nxs_vm` sets it unless
`--no-dsp-idle-skip`) proves when the DSP spins: at a clean step (no queued
loads/stores, branch or SPLOOP) it anchors PC, registers, control registers
and delayed-control state. Any device access other than a pure GPIO read
(the idle loop polls the boot-phase inputs), an EDMA completion, a McASP
slot tick that moved data, or more than 64 distinct written RAM words voids
the anchor. When the DSP returns to the anchor PC with identical state, every
RAM word it wrote since holds its anchor value again, and no timer, PLL
countdown, SPI transfer or PSC transition can fire, the whole system state
has repeated with a period of P packets and C cycles. Running k more
periods is then exactly: packets += kP, cycles += kC,
`cdj_c6747_pll_ticks(kC)` (closed form, checked against k*C single ticks in
`tests/cstub/c6747-pll.c`), and a kC shift of each delayed-control cycle the
loop rewrites once per period. Dead load/store queue slots, which checkpoints
store verbatim, are handled by always leaving one whole period to execute
after a skip: it repeats every append of the loop at the same phase, and only
a checkpoint reads dead slots. The skip takes as many periods as fit before
the end of the activation budget and before the next McASP slot the DSP can
observe. With functional audio a slot is invisible when, probed on scratch
EDMA/McASP copies, it stages no RAM write, raises no EDMA completion and reads
no RAM word the loop transiently rewrites; invisible slots inside a skip are
then run in order at their exact packet counts (with TX capture, whose
records carry per-slot counters, every slot counts as visible).
`tests/test_dsp_idle_skip.py` compiles the board's helpers against the real
peripheral models; removing the memory, timer or delayed-control check fails
it.

Exactness on stock NXS firmware, full capture, same binary with and without
the skip, over the common prefix of a 45-second boot: all 120,717 event
records (every packets and cycles value included) and all 513 DSP checkpoints
are byte-identical; with `--functional-dsp-audio`, 118,238 events and 446
checkpoints are. The skip changes wall time only.

MAIN then reaches its GUI link before the slower simulated GUI has booted and
waits for a retry, so `--gui-head-start SECONDS` starts the GUI first (its
link connection is lazy and retried); with the skip it defaults to 2.0 s
(0.5 s measured too short, 1.0-2.0 s equivalent; the head start alone gives
nothing). It was 1.5 s until the SH-4 fast paths below made MAIN's early boot
twice as fast; 1.5 s then lost the race in 3 of 4 `--dsp-model` boots, each
costing a ~3 s retry.

Wall time from launch, `--lightweight`, same binary:

| Milestone | Full execution | Idle skip + head start |
| --- | ---: | ---: |
| Player screen (from GUI launch) | 9.9 s | 4.2 s |
| SD browser lists `TESTTONE.WAV` (`--test-track --source-key-when-ready`) | 150.5 s | 38.7 s |
| Track loaded, duration reply after ENTER, ENTER | 194.3 s | 49.4 s |

The loaded-track screen shows TRACK 01 with REMAIN 00m:10s. With
`--functional-dsp-audio` the DSP streams McASP slots continuously from early
boot and takes an EDMA completion interrupt every 32 slots (~32K packets);
each window still executes the handler plus two idle-loop periods, so the SD
listing improves less, 185 s to 152 s, and playback
advances at ~0.05-0.07x real time: replaying a mid-playback snapshot shows
only ~30% of packets in the idle loop, the rest in 3,197 distinct PCs of
audio work, and the functional model clocks one slot per 1,024 packets, so
real time needs ~90M packets/s against ~10M today. Playback is bound by raw
DSP execution speed.

## Loop-buffer transactions (2026-09-28)

Counting copies over 5M steps of a mid-playback snapshot (functional audio)
found `loop_step` moving 5.2 KB in and 5.2 KB out on every one of 571K
loop-buffer cycles, 5.9 GB against 4.2 GB for all per-packet backups
together, and SPLOOP setup and the IDLE/padding path copying the whole
6.5 KB CPU twice. Now:

* `loop_step` copies the scalar prefix and live queue entries, the loop
  schedule's scalar fields, and only for an unsealed (loading) loop its
  tags/count arrays and, when it fetches, the instruction buffer; a sealed
  step reads the committed schedule through `cdj_c674x_loop_issue_*_from`,
  which take the tags/count arrays from a separate struct. It commits queue
  slots up to the extent execute reports having written
  (`execute_transaction`), so untouched dead slots keep their bytes.
* SPLOOP setup copies and commits up to the instruction buffer, which it
  never writes.
* The IDLE/padding path executes in place (execute is transactional) and
  restores its idle count on failure.

Replay of the playback snapshot: identical trace and final checkpoint,
0.75-0.76 -> 0.71-0.72 s; boot checkpoints 1/25/250 identical; 45-second
full-capture boots against the previous binary give identical events and
DSP checkpoints (508, and 452 with functional audio).

The issue loop reuses the pre-scan's per-instruction NOP count and SPMASK
decode, the load/store overlap check is skipped with no store queued, and the
retire loop skips a load due more than two cycles out (neither its E3 read nor
its retirement can fall in the cycle) with one test: about 1.5% on the
playback replay, identical output.

## C674x interpreter performance (upstream summary)

The NXS DSP interpreter runs synchronously inside the SH-4's MMIO path, so its
speed bounds the whole emulated machine. These changes make it faster without
changing what it computes: every one keeps traces, events and DSP checkpoints
byte-identical to the previous build.

### Changes

* **Decode memo.** Row selection in the 114-row conditional dispatch table is
  a pure function of the instruction word (`also` predicates read only the
  word; see `cdj_c674x_arm_table_predicates_are_word_only`), so
  `cdj_c674x_arm_lookup` memoizes it per word in a direct-mapped table of
  relaxed 64-bit atomics (word and row in one entry).
* **In-place packet execution.** Each packet used to run against a scratch
  copy of the 3 KB CPU prefix that was copied back on success.
  `cdj_c674x_execute` now backs up the scalar prefix and live queue entries,
  runs the packet (`execute_packet`) directly on the CPU, and restores the
  backup if it fails. Queue appends go through `append_load`/`append_store`,
  which save the overwritten slot first, so a rollback leaves exactly the bytes
  a discarded copy would. `cycles` and `packets` still change only when the
  packet retires, because bus callbacks read them mid-packet. The old copy
  transaction remains behind `CDJ_C674X_COPY_TRANSACTIONS`, and
  `tests/test_c674x_transaction.py` builds it as the byte-exact reference
  (with cases where an append is followed by a rejected parallel instruction).
* **Loop-buffer transactions.** `loop_step` copied 5.2 KB in and out on every
  loop-buffer cycle. A sealed loop step now copies only the scalar prefix, live
  queue entries and the loop's scalar fields, reads the committed tags/count
  schedule through `cdj_c674x_loop_issue_*_from`, and commits queue slots up to
  the extent execute reports writing. SPLOOP setup stops short of the
  instruction buffer, and the IDLE/padding path executes in place.
* **Fetch block hook.** `cdj_c674x_set_fetch_block` lets a board return a host
  pointer to a whole 32-byte fetch block of plain memory, replacing one read
  callback per instruction word. Nothing is cached across fetches, so code
  writes stay visible. The NXS board and the replay tool register it.
* **Per-cycle and per-read trims.** Timers with both ENAMODE fields clear are
  skipped (the NXS boot never enables them); the SPI tick has an exact idle
  fast path and no per-cycle division; the PLL tick skips its division when no
  whole input period elapsed; `cdj_c674x_loop_functional_timing()` is inline;
  the NXS `dsp_read` tests L2 first (the windows are disjoint and firmware
  executes from L2); the issue loop reuses the pre-scan's NOP/SPMASK decode;
  and the queue scans skip entries that cannot act this cycle.

### Measurements (macOS, Apple silicon)

Same QEMU tree and toolchain, `upstream/main` against this branch, 60-second
full-capture NXS boots through `tools.cdj_main.nxs_vm`:

| Boot | DSP packets, upstream | DSP packets, this branch | Events / checkpoints compared |
| --- | ---: | ---: | --- |
| default | 406M | 654M | 118,238 / 446, all identical |
| `--functional-dsp-audio` | 377M | 603M | 117,165 / 417, all identical |

Standalone replay (`tools/cdj_dsp/replay.c`): boot checkpoints give identical
traces and final checkpoints; 5M steps of a mid-playback snapshot with
functional audio take 1.18-1.20 s upstream and 0.68-0.69 s here, with an
identical trace and final checkpoint.

The pytest suite gives the same result on both builds (680 passed; the same
26 checkpoint/deferred-replay tests fail on `upstream/main` in this
environment).

## Persistent packet cache and fast paths (2026-10-05)

Ideas after Stijn Jacobs' cdj-nxs2-qemu (its `c66x_step.c` packet cache and
`fast_cycles`, and its `make m1` decoder sweep), used with permission; credit
in THIRD_PARTY.md, no code copied. All of it is in `emulator/qemu/cdj_c674x.c`;
no board file changed. This supersedes "Nothing is cached across fetches"
above.

* **Decode record.** Everything the issue loop derived from an instruction
  before touching state (pre-scan NOP/SPMASK, compact lowering, CALLP,
  DINT/RINT, unconditional class, arm row, and the compact format, now
  `compact_form()` in the issue loop's test order) is one pure function,
  `decode_instruction`. Uncached packets decode on the fly with it.
* **Packet cache.** Per thread, direct-mapped on pc/2 (16K entries), for
  direct steps: fetched packet, decode records, fast-path eligibility.
  Invalidation is by content: an entry keeps the raw 32 bytes of every fetch
  block its fetch read, and each hit re-obtains them through the board's
  fetch-block hook and compares. DSP stores, HPI uploads, EDMA, L1D/SDRAM
  remaps are all seen at the next fetch with no hook in any writer; words
  that came through the read callback are never cached.
* **Fast path** (`execute_fast`) for cached packets that write no control
  register during issue: execute in place with no prefix copy. E1 register
  results are deferred until issue has passed every check
  (`CdjC674xDefer`), so a failing packet is declined byte-for-byte and the
  transactional path reproduces the fault. Retirement can fail only on a
  queue entry, so registers, control registers and live queue entries are
  snapshotted only when an entry can act within the packet; such a fault is
  restored as the transactional rollback would leave it (retirement faults
  now report the packet's entry pc, which is what they always reported).
* **Single-instruction path** (`execute_single`), ~90% of NXS packets: one
  register-only arm (MVK/MVKH/ADD/SUB/AND/OR/XOR/CMP/shifts/bit fields/MVC
  read...), B, B reg or NOP n, when no queue entry acts in its cycles.
* `CDJ_C674X_PACKET_CACHE=0` disables all of it, `=decode` keeps only the
  cache; `cdj_c674x_set_packet_cache()` does the same for tests.

Not done: the per-hit fetch-block call (`dsp_memory_span`, ~7% of the DSP
thread in a profile) could go if the board declared mapping-stable windows; McASP-paced
batching of NOP runs needs board cooperation (each cycle ticks the board).

### Exactness

* Same QEMU binary, `CDJ_C674X_PACKET_CACHE=0` against default, 45-second
  full-capture stock NXS boots: all 214,401 events and 3,045 DSP checkpoints
  of the common prefix byte-identical; with `--functional-dsp-audio`,
  121,309 events and 529 checkpoints. Final screens identical. Against the
  pre-change binary: 234,677 events and 3,593 checkpoints identical.
* Replay (`tools/cdj_dsp/replay.c`): checkpoints 1, 25, 250 (1M steps), 400
  (5M) and the first-play snapshot (5M, strict and functional audio) give
  identical traces and final checkpoints against the pre-change core, and
  cache off against on.
* `tests/cstub/c674x-packet-cache.c`: 3,000 random programs (self-modifying
  stores, host code rewrites between steps, fetch-window withdrawal, failing
  store commits and E3 reads) run in lockstep cached+fast vs uncached;
  whole CPU and memory compared after every step (3.4M packets, 1,977
  faults). Removing validation, the register/branch/slot rollback, the
  deferral, the entry-pc fault, the single path's window or predicate, or
  its branch break each fails it. Clean under ASan/UBSan.

### Measurements (Apple silicon, loaded shared host)

| Workload | Before | After |
| --- | ---: | ---: |
| `benchmark_core` step-alu, 30M packets | 21.3 M/s | 77 M/s |
| `benchmark_core` step-mem (LDW/STW loop) | 19.1 M/s | 37 M/s |
| replay ckpt 400, 20M steps, user s | 2.20-2.21 | 1.70-1.71 |
| replay first-play, functional audio, 5M | 0.67-0.68 | 0.56-0.57 |
| 60 s NXS boot, DSP packets | 649M, 648M | 844M, 857M |
| same, `--functional-dsp-audio` | 585M | 737M |
| same binary, cache off vs on | 603M | 852M |

Boots ran pairwise at the same time on a loaded host, with
`--lightweight`, so no capture cost. With the core cheaper, the DSP thread
is now roughly half board work (`dsp_cycle_tick`, timers, PLL, interrupt
delivery), which bounds further core-only gains.

## Decoder cross-check (2026-10-05)

`tools/cdj_dsp/decode_crosscheck.c` sweeps an address range from a DSP
checkpoint through `cdj_c674x_fetch` and `cdj_c674x_describe` (the family the
issue loop executes); `python -m tools.cdj_dsp.decode_crosscheck` compares
it with GNU objdump (`gobjdump -D -z -EL -b binary -m tic6x`): instruction
boundaries and sizes, parallel bits, family vs mnemonic, and every compact
instruction the core rewrites to 32 bits re-disassembled from the rewritten
word. Over the NXS stage1 (0x11801da0-0x11804d80) and stage2
(0xc0000000-0xc0058320) images: 90,273 instructions, 1,582 rewritten compact
operands, 0 disagreements; the only fetch rejections are 818 blocks of
0xffffffff fill that objdump also calls undefined. Operands of in-place
compact forms and of 32-bit arms are outside this check.

## DSP on its own thread (`nxs_vm --dsp-thread`, 2026-10-05; default since 2026-10-06)

`CDJ_NXS_DSP_THREAD=1` runs the C674x on its own host thread instead of inside
MAIN's HPI MMIO write (pattern after Stijn Jacobs' cdj-nxs2-qemu; see
THIRD_PARTY.md). The DSP runs at most one quantum (1 ms) ahead of
`QEMU_CLOCK_VIRTUAL` and gives up lag rather than catching up. MAIN, on every
HPI access, reset or boot-phase change, waits with the BQL released until the
DSP has executed `--dsp-thread-access-packets` (default 256) packets since
MAIN's previous access. The synchronisation argument is the comment above
`NxsDspThread` in `cdj2000_nxs_hpi.c`; `tests/test_dsp_thread.py` compiles that
code against a pthread shim and checks pacing, the BQL release, the lock
hand-over at MAIN's target, torn-mailbox freedom, HINT delivery and shutdown.
Threaded runs write no checkpoints and are not replay evidence.

The synchronous default is unchanged. Same 45 s boots, `develop` binary versus
this one: all 233,789 event records and 3,569 checkpoints are byte-identical
over the common prefix, and 122,123 events and 551 checkpoints with
`--functional-dsp-audio`.

Stock NXS firmware, a real rekordbox USB export, `--functional-dsp-audio
--lightweight`, Ethernet on an isolated `link_hub`, scenario driven through
`tools.cdj_main.dev`. Seconds of wall time, single runs, polled at about 1 s.
The host was shared with other emulator runs (load average 10-38).

| Milestone | synchronous | `--dsp-thread` |
| --- | ---: | ---: |
| Launch to `Not Loaded.` | 13.6 | 8.2 |
| USB press to the USB root list | 134.8 | 1.2 |
| Enter on [TRACK] to the track list | 1.8 | 0.7 |
| Enter on Bang Bang to the duration reply | 19.6 | 1.15 |
| Play to 150 counter frames (1 s of track) | 45 | 19 |
| Pro DJ Link keep-alives per minute (largest gap) | 1.1 (77 s) | 23.0 (2.8 s) |

Both loaded the track with its waveform and BPM and played. The threaded run
logged no DSP fault and its DSP-paced WAV has 128,042 frames, 44,311 of them
nonzero. Playback stays bound by DSP speed (one McASP slot per 1,024 packets).

Variants that lost:

- MAIN waiting for virtual-time parity with a 150 Mpackets/s DSP clock: MAIN
  waited 99.5% of the time and had not reached `Not Loaded.` after 280 s.
- No per-access wait (`--dsp-thread-access-packets 0`): MAIN's 3,000-read
  ready poll saw about 13 DSP packets per read and the deck showed E-7010.
- 64 and 512 packets per access also worked: play to 150 frames in 27 s and
  41 s.

## McASP slots on the DSP thread's virtual clock (`--audio-clock virtual`, opt-in, 2026-10-05)

After
Stijn Jacobs' cdj-nxs2-qemu `CDJ_C6X_MHZ` (peripherals on virtual time, a
configurable DSP clock); credit in THIRD_PARTY.md.
`nxs_vm --functional-dsp-audio --dsp-thread --audio-clock virtual`
(`CDJ_NXS_DSP_AUDIO_CLOCK=virtual`) replaces "one slot per 1,024 packets" with
slot deadlines at the configured McASP1 rate (88,200 slots/s for 44.1 kHz
stereo) on the DSP thread's clock, `epoch + packets / rate`
(`--dsp-clock-mpps`, default 150, i.e. 1,700.7 packets per slot). The DSP runs
at most a quantum ahead; when it is behind, the thread gives the lag up as
before, and the slots the slip jumped fire at once (chunks are 2,048 packets in
this mode, so a burst is a few dozen slots) and are counted as underruns: time
is never stalled. `cdj_dsp_audio_clock.h` holds the exact deadline arithmetic;
`tests/test_dsp_audio_clock.py` drives the board's `thread_audio_tick` and
`dsp_thread_slip` with a host faster and ten times slower than the clock
(every slot gets 1,700-1,701 packets and none is late; slots still follow
virtual time with >= 85% underruns; stop/restart; 48 kHz reconfiguration).
The idle skip stops before each slot edge. Host audio (`--host-dsp-audio-wav`)
is allowed with it.

Exactness: the synchronous modes do not reach the new code. Same 45 s
full-capture stock boots, `develop` (6d7b58a) binary against this one: all
257,469 events and 4,209 DSP checkpoints of the common prefix byte-identical;
with `--functional-dsp-audio`, 125,453 events and 641 checkpoints.

**Result: the clock works, real-time playback does not.** Stock NXS, the
a real rekordbox USB export, `--lightweight`, `link_hub`, same scenario as above,
shared host:

| | packets (default) | `--audio-clock virtual` |
| --- | ---: | ---: |
| USB press to the USB root list | 1.2 s | 35.6 s |
| Enter on Bang Bang to the duration reply | 0.9 s | 1.9 s |
| After PLAY | 150 frames in 17 s, then 1.15 frames/s (0.008x) | E-8302 CANNOT PLAY TRACK (300A) |
| McASP slots per virtual second | ~5,000 | 88,200.5 |
| DSP packets/s | ~5.0 M | ~3.4 M |
| slots short of their budget (underruns) | n/a | 97.8% (39 of 1,700.7 packets per slot) |
| MAIN blocked in the per-access DSP wait (whole run) | 80% | 78% |
| QEMU CPU | 113% | 117% |
| DSP-paced WAV | 415,566 frames, 69,309 nonzero | 18,373,237 frames (416.6 s), all zero |

The slot clock tracks virtual time exactly (8,820,051 slots in 100.000
virtual s), but from the moment McASP1 starts in boot the DSP never gets
ahead of it again: no pacing wait, no idle skip, 391 of 400 s slipped. The
firmware's real-time audio duty (an EDMA completion ISR every 32 slots, plus
decode once a track plays) needs more packets per second than the interpreter
delivers on that code (3.4-5 M/s; the playback-snapshot replay above put
playback demand near 63 M packets/s), so the ISRs starve the background
tasks, MAIN's stream and command traffic waits on the DSP, and the load fails.
Fewer packets per MAIN access (`--dsp-thread-access-packets 32`) gives E-7010
in boot; a 4 Mpps DSP clock (what the host sustains) gives E-7010 before
McASP even starts.

So the mode stays opt-in and is not proposed as the `--dsp-thread` default:
real time needs a DSP roughly 15-20x faster on audio code. The profile of the
starved DSP thread points at the first levers: 59% of step samples in the
transactional (non-fast) packet path, and every RAM store walking the full peripheral
probe chain in `dsp_write` before reaching L2/SDRAM (twice: check and commit).

## RAM store dispatch and loop-buffer fast path (2026-10-06)

Profiled first: stock NXS, a real rekordbox USB export, `--functional-dsp-audio
--dsp-thread`, macOS `sample` on the DSP thread during playback, and a
per-path counter (a temporary build, not kept) over 10 M packets replayed
from a real-USB playback checkpoint (Bang Bang, 15 s after PLAY, taken from a
synchronous run with `dev checkpoint`):

| packets by step path (functional audio) | share |
| --- | ---: |
| SPLOOP loop-buffer cycles (`loop_step`, all transactional) | 52.0% |
| direct, `execute_fast` | 32.6% |
| direct, `execute_single` | 8.5% |
| direct, not fast-eligible (CMPSP 90%, CALLP 8%) | 2.3% |
| IDLE / SPLOOP setup | 0.7% |

The audio kernels run from the loop buffer, and every loop-buffer cycle paid
a ~1 KB scratch copy of the CPU in and out, a second ~1 KB backup inside
`execute_transaction`, and a fresh decode of each buffered instruction: a
loop cycle cost about eight direct packets. In the live DSP thread,
`dsp_write` was 6.2% inclusive (each RAM store probing ~20 models twice) and
the transactional path 13.4%.

* **RAM store dispatch** (`cdj2000_nxs_hpi.c`). `dsp_write` asks
  `dsp_memory_span` first: it is exactly the union of the RAM windows the
  function accepted at its end (L1D SRAM, L2 and its local alias, shared RAM,
  SDRAM while EMIFB enables it), none of which overlaps a register window of
  any model (0x01800000-0x01efffff, EMIFB registers at 0xb0000000). A RAM
  store lands, with the idle-proof write log, without probing any model; an
  MMIO address, an odd size, a store straddling a window end or SDRAM with
  EMIFB disabled falls through to the chain unchanged. `dsp_memory_span`
  tests L2 first (as `dsp_read` does). `CDJ_NXS_DSP_RAM_FAST=0` restores the
  chain for A/B.
* **Loop-buffer cycles on the fast path** (`cdj_c674x.c`). A sealed,
  non-reloading loop with no post-loop fetch, interrupt drain, IDLE or
  retained interrupted loop - 93% of loop cycles in the playback replay - runs
  in place (`loop_step_in_place`): issue, TSR.SPLX, the predicate history,
  `execute_fast` and the ILC/termination updates, with a three-field undo; a
  declined packet leaves the CPU untouched and `loop_step` runs it. The other
  loop cycles keep `loop_step`, which now also executes its scratch copy with
  `execute_fast` instead of under a second backup. Buffered instructions'
  decodes and fast-path eligibility are memoized per thread by instruction
  (`loop_decode`). The fast path's commit no longer refuses single-cycle
  packets (direct packets never are). All of it is mode 2 only:
  `CDJ_C674X_PACKET_CACHE=decode` or `0` runs the old path.

### Exactness

* Same QEMU binary, `CDJ_NXS_DSP_RAM_FAST=0 CDJ_C674X_PACKET_CACHE=decode`
  (both new paths and the existing fast paths off) against default, 45-second
  full-capture stock boots: all 243,298 events and 3,826 DSP checkpoints of
  the common prefix byte-identical; with `--functional-dsp-audio`, 122,530
  events and 562 checkpoints. Against the pre-change binary (fc8385b): 260,133
  events and 4,281 checkpoints; functional audio 125,638 and 646. Final
  screens identical in all six.
* Replay: checkpoints 1, 25, 250 (1M steps), 400 (5M), the earlier
  first-play snapshot (5M, strict and functional audio) and the real-USB
  playback checkpoint (10M, strict and functional audio; 4.68M and 2.78M
  trace lines) give identical traces and final checkpoints against the
  pre-change core, and cache off against on in the new one.
* `tests/cstub/c674x-packet-cache.c` adds 3,000 random SPLOOP programs (ILC
  1-12, II 1-14, bodies of loads, stores, NOP n and parallel packets, failing
  store commits and E3 reads) run in lockstep mode 2 against mode 0: 6.9M
  packets, 5.6M loop-buffer cycles, 688 faults, whole CPU and memory equal
  after every step. Removing the retirement snapshot or the loop-cycle undo,
  dropping the ILC update or reading another instruction's memo entry each
  fails it. Clean under ASan/UBSan.
* `tests/test_dsp_ram_fast_path.py` compiles the board's `dsp_write` with the
  real models and runs 200,000 random stores (window edges, every model's
  registers, odd sizes, SDRAM gating toggled, every L1D partition, the idle
  log armed) on two boards, fast and `ram_slow`, comparing results, memory
  and model state, then a real EDMA transfer staged into SDRAM. Dropping the
  idle-log call, mis-sizing a store or widening L2 by a word each fails it.

### Measurements (Apple silicon, shared host, load 12-14)

| Workload | Before | After |
| --- | ---: | ---: |
| `benchmark_core` step-sploop (new: SPLOOP, ILC 64, II 4) | 9.5-9.8 M/s | 19.9-20.0 M/s |
| `benchmark_core` step-alu / step-mem | 79-81 / 37 M/s | unchanged |
| replay real-USB playback, 10M, functional audio, user s | 2.07-2.08 | 1.74-1.76 |
| same, strict | 2.54-2.56 | 2.02-2.03 |
| replay ckpt 400 (boot), 20M | 1.72-1.74 | 1.70-1.71 |
| `--audio-clock virtual` playback run, DSP packets/s | 3.3 M | 5.2 M |
| same, underrun slots | 97.85% | 96.6% |
| same, USB press to root list / Enter to duration | 25.1 / 2.4 s | 12.1 / 1.2 s |

Replay times include `replay.c`'s own write trace and peripheral chain
(unchanged), so the core gain on audio code is larger than they show. The
virtual-clock runs are the scenario of the previous section (`--dsp-thread
--lightweight --audio-clock virtual`, link_hub, same key presses); neither
plays (the counter never advances) and the DSP still gets 59 of its 1,700.7
packets per slot. In the live DSP thread after the change, sched_yield and
the lock (the per-access hand-over to MAIN) are 24% of samples, the
transactional path 2%, `dsp_write` 2%, `advance_functional_mcasp_slots`
(three ~4 KB EDMA state copies per slot) 4%, and the interpreter's issue
loop (`execute_packet`) the largest single cost.

**Real time needs a compiler, not more of this.** The previous section put
playback demand near 63 M packets/s on this code; the interpreter now
delivers ~5 M in the live board and ~20 M on a pure SPLOOP microbenchmark,
and what remains is spread over the generic issue loop (operand fetch, arm
dispatch, the E1/E3/E5 queues, a board tick per cycle) with no single cost
left that an exact change removes an order of magnitude from. Stijn Jacobs'
cdj-nxs2-qemu reaches real time on its C66x only with a block compiler:
`c66x_jit.c` (860 lines in the core) loads regions that
`tools/c14_jitgen.py` (2,650 lines) generates as C from a run profile, with
the pipeline resolved at compile time (each write's landing cycle and the
branches in flight are static state; conditional branches fork it; loops
close by merging states), interpreter handlers called with captured writes
for anything without a native form, and exits that put in-flight writes back
where the interpreter keeps them. Its decoder, state layout and write ring
are its own, so it is a design to follow rather than code to drop in; here a
compiled region would also have to call the board's per-cycle tick, stop at
every interrupt-recognition boundary and McASP slot edge, re-check stores
against the fetch blocks it was built from, and reproduce the E1/E3/E5
queues and fault rollback byte for byte to keep the checkpoint evidence.

## Compiled SPLOOP kernels (`CDJ_C674X_JIT=1`, `nxs_vm --dsp-jit`, default on, 2026-10-06)

Stage 1 of the compiler the previous section asked for: the loop-buffer
cycles. All of it is in `emulator/qemu/cdj_c674x.c` ("Compiled SPLOOP
kernels", "steady-state kernels"); design after Stijn Jacobs'
cdj-nxs2-qemu region compiler (credit in THIRD_PARTY.md), no code copied.

**Mechanism: in-process, ahead of each loop's first use, no host machine
code.** A sealed SPLOOP buffer is a complete, self-contained program (it
issues from the buffer, never from memory), so it is compiled the moment it
is sealed, from the CPU's own buffer, for any firmware, and identified by a
byte comparison with that buffer at the start of every run (a restore, a new
SPLOOP or a reload can never run a stale compile). Offline-generated C (the
nxs2 route) would need a profile-and-rebuild loop per firmware and a second
implementation of our semantics in a generator; runtime native code would
need a backend per host (arm64 macOS with MAP_JIT, x86-64 MSYS2) and W^X
handling. Instead the compiled form is data plus specialised C: every
operation runs this core's own arm semantics (`arm_*`, `cdj_c674x_sp.c`), so
"our semantics win" by construction, and what is compiled away is the
generic machinery around them.

Three layers, each falling back to the one below for a cycle it does not
handle (the CPU untouched, so the fallback reproduces any fault itself):

1. **Compiled schedule** (`jit_cycle`). Cycle c issues the origins
   `o` of its phase with `c - iterations * II < o <= c`, a contiguous run,
   so each (phase, first, last) has one prebuilt combined packet with its
   decodes. The cycle is `loop_step_in_place` with that lookup instead of
   `cdj_c674x_loop_issue_filtered_from`, `loop_decode` and the packet copy.
2. **Native issue** (`jit_exec`). Loads/stores (every short-offset form),
   MPYSP and ADDSP/SUBSP from precompiled fields, other reviewed register
   arms through their own functions; queue appends saved in an 8-entry undo;
   the cycle's bus operations (tick, due store commits, due E3 reads) run
   before any register or queue changes, so a failing commit needs no
   snapshot; retirement compacts each queue in one pass that leaves exactly
   the bytes of `execute_packet`'s one-at-a-time `memmove`, dead slots
   included (the vacated slots all hold the pre-cycle last entry).
3. **Steady-state kernels** (`jk_compile`, `jk_exec`): the pipeline resolved
   at compile time. For a loop of unpredicated loads/stores and SP
   arithmetic, a simulation of `execute_packet`'s issue/retire rules gives,
   per phase, the queue shape (which (operation, age) is at each index),
   which entries commit, read at E3 and retire, which same-cycle load/store
   pairs need the overlap check, and the tail that retirement copies into
   vacated slots; a loop where any data-independent issue check could fire
   is not compiled. A steady cycle does only the data-dependent work, keeps
   in-flight entries in a model by (operation, issue cycle), writes only the
   vacated slots, and rebuilds the live prefix of `stores[]`/`loads[]`
   (`jk_sync`) when steady execution ends. It starts only from a queue that
   matches the compiled shape entry by entry.

`cdj_c674x_run` strings cycles together and calls the caller's
`between()` - the board's whole post-step and pre-step work, split out of
`execute_dsp` (`dsp_pre_step`/`dsp_post_step`) and of `replay.c`
(`quota_pre`/`quota_post`) - after every packet, so interrupts, McASP slot
edges, timers and MAIN's hand-over happen exactly where the step loop put
them, and the board tick is still called every cycle. The one new rule:
`between()` must not read the queue arrays while a kernel is steady (their
counts are exact; interrupt recognition with an active loop never reads
them, and steady execution ends before an idle loop's interrupt entry).

Tried and dropped: compiling direct (non-loop) packets with the same
executor (stage 2's simplest form) - 5.3 M of 20 M playback steps ran
through it, no faster than `execute_fast` (lean replay 2.07 s against
2.06 s), so it was removed.

### Exactness

* `tests/cstub/c674x-packet-cache.c` gains a JIT phase: 3,000 SPLOOP
  programs (a third with kernel-shaped bodies, a quarter under functional
  timing), loop bodies now including MPYSP, every ADDSP/SUBSP encoding and
  loads/stores of every width and addressing mode through pointers that are
  circular under a random AMR, random FP rounding modes, random interrupt
  requests with GIE set, random run limits and `between()` refusals. System
  B steps the uncached interpreter inside A's `between()`, and A's rebuilt
  view (`cdj_c674x_view`) and memory must equal B's after every packet:
  1.29 M compiled packets (452 K steady, 784 K native, 55 K generic) in
  101 K runs, 375 loop interrupts, 2,220 faults. Clean under ASan/UBSan.
* Mutations: 22 single-line breaks of the issue range, undo, compaction
  fill, E3 values, overlap and collision checks, tick, span reads, buffer
  match, ILC, FP rounding, AMR, and of the kernel's tail copies, sync
  timing, FP status, pair high word and exits - each fails the test. Three
  defensive checks are unreachable by it: the all-pairs overlap check on a
  run's first cycle (every packet path already checked older pairs), the
  due check on kernel entry, and the sync before `between()` after a loop
  ends in a full cycle (only SPLOOPW can).
* Replay (`tools/cdj_dsp/replay.c`), JIT off against on: checkpoints 1,
  25, 250 (1 M steps), 400 (5 M), the first-play snapshot (5 M, strict and
  functional audio) and the real-USB playback checkpoint (10 M and 60 M
  steps, strict and functional audio; 28.6 M and 17.2 M trace lines at
  60 M) give identical traces and final checkpoints, which at 10 M also
  equal the pre-change core's. Of the boot checkpoints only the playback
  ones reach loops in their window (checkpoint 400: none in 5 M steps).
* 45-second full-capture stock boots, `develop` (ccc0c79) binary against
  this one with the JIT on: all 262,797 events and 4,353 DSP checkpoints of
  the common prefix byte-identical; with `--functional-dsp-audio`, 130,522
  events and 778 checkpoints. JIT off against on in this binary: 262,797 /
  4,353 and 130,041 / 765. Final screens identical in all.

### Measurements (Apple M4 Max, shared host, load 8-15)

| Workload | JIT off | JIT on |
| --- | ---: | ---: |
| `benchmark_core` step-sploop (LDW/ADD/MVK/STW, II 4) | 19.2 M/s | 30.4 M/s |
| `benchmark_core` step-memcpy (the stage-1 memcpy kernel, ILC 64) | 15.6 M/s | 30.0 M/s |
| `benchmark_core` step-kernel (same, ILC 30000, steady) | 17.2 M/s | 43.5 M/s |
| `benchmark_core` step-alu / step-mem (no loops) | 80 / 37 M/s | unchanged |
| replay real-USB playback, 10 M, functional audio, user s | 1.76-1.78 | 1.32 |
| same, strict | 2.02-2.03 | 1.34-1.35 |
| same without `replay.c`'s trace output, 20 M steps | 2.75 | 1.83 |
| live `--dsp-thread --audio-clock virtual`, packets by 140 virtual s | 929 M | 1,069 M |
| same, underrun slots | 95.7% | 95.0% |

In the 20 M-step playback replay, 10.5 M steps are loop cycles; 10.1 M of
them run steady, 0.33 M native, 14 K generic, from 14 compiles. Loop
cycles went from about half the core's time to 43% of the (smaller)
total; the direct packets are now the larger half.

**Real time: not reached.** The live run is the virtual-clock scenario of
the sections above (stock NXS, a real rekordbox USB export, link_hub, same key
presses): it loads the track and the counter still does not advance after
PLAY, in both modes. The DSP gets ~15% more packets per virtual second
(7.6 M/s), against the ~63 M/s playback needs. In the DSP thread with the
JIT, waiting for MAIN's HPI hand-over is 23% of samples, the board's
per-cycle tick and per-step work 15%, and the core 55%, of which the
compiled loops are now a small part (`jk_exec` 3%, `cdj_c674x_run` 2%) and
direct packets the bulk (`execute_packet` 13%, `memmove` 8%,
`cdj_c674x_step_capture_direct` 8%). 80% of the remaining direct packets
fall in seven code ranges (`dsp_output_cursor_advance` 23%, four pieces of
`dsp_varispeed_resample` 46%, `dsp_pcm_queue_discard` 7%), mostly
predicated ALU code with data-dependent branches, where a per-packet
executor saves nothing over `execute_fast`.

What real time would still need, in order of size (stage 2 took the
first three steps; see "C674x JIT stage 2" below):

1. **Compiled direct code** (stage 2 proper): traces/superblocks over those
   ranges with the pipeline resolved across packets and branches (loads in
   flight and branch delay slots as static state, side exits on predicate
   or branch outcomes), i.e. the steady-kernel technique generalised to
   control flow. The per-packet costs left are the generic issue loop, the
   fetch-block revalidation and the dispatch, a few ns each.
2. **Batched board work**: the board tick (PLL, timers, SPI) every cycle and
   the full between-step work every packet are now about a sixth of the DSP
   thread. A board horizon - how many cycles/packets until a timer event, a
   PSC transition, a McASP slot edge, a pending EDMA notification or a MAIN
   request - would let compiled code run that far with one batched tick,
   flushing before any non-RAM bus access.
3. **MAIN/DSP hand-over**: MAIN waits for the DSP 73% of the time and the
   DSP yields to MAIN 23%; this shrinks only as the DSP gets faster.

**Default:** `nxs_vm` now turns `--dsp-jit` on (`CDJ_C674X_JIT=1`); every
exactness gate above passes, and it is a 1.3-1.5x on playback replay and
2-2.5x on loop-buffer code with no measurable cost elsewhere.
`CDJ_C674X_JIT=0` / `--no-dsp-jit` keeps the interpreter for A/B.

## Virtual time held while MAIN waits for the DSP (`--dsp-thread`, 2026-10-06)

**Symptom.** Stock NXS, a real rekordbox USB export, `--functional-dsp-audio
--dsp-thread`, load Obey, PLAY: the remain counter advanced about 150 half
frames (07:11 02 to 07:10 33) and froze while the transport still reported
play requested. Five of five runs (`runs/loadfix` stock-long/stock-late,
`runs/ps` base1, usb1, smp1).

**Mechanism** (DSP event transcripts, `CDJ_USBH_TRACE=scsi`, MAIN PC samples).
MAIN streams the track to the DSP in 0x28-frame stream buffers (header
0x11838140 = 0x1010100 for a new stream, 0x1020100 to continue, data blocks
handed over with 0x118381c4). Before play it buffers 0x78 frames; the DSP then
counts 0x11837cd0 (frames ahead) down to 0 while its position 0x11837c10
counts up to 0x77, and MAIN delivered only four blocks of the next buffer in
the following 150 s (one per ~290M DSP cycles against one per ~3M before
play). USB reads of the file slowed to one 4 KB read per 0.7-1.5 s: nothing in
the USB, EDMA, McASP or interrupt models was waiting, MAIN simply had no time.
Every HPI access on the DSP thread waits until the DSP has run 256 packets
(`--dsp-thread-access-packets`); the interpreter delivers 6-15 M packets/s
against a 150 Mpps clock, so the wait is ~20-40 us of host time per word, and
QEMU_CLOCK_VIRTUAL kept running through it. After PLAY MAIN's tick-driven
status polls read ~100 HPI words every few ms: MAIN spent 80-85% of each
virtual second in the per-access wait (`main-wait` 8.0 s per 10 s), PC
samples were almost all in `dspregif_read32_arg`/`sub_041c77b8_hpi`, and the
file task feeding the stream starved. Without a PLAY press (the firmware
auto-plays a loaded track) the same old binary still fed slowly enough to play
(one 0x28-frame buffer per ~130M cycles); the press's extra MAIN work tipped it
into the stall. No REQ 3 (pause) was written in any of these runs.

**Fix** (`cdj2000_nxs_hpi.c`, `main_lock`). On the real board an HPI access
stalls the SH7764 bus well under a microsecond while the C674x runs on in
parallel; the host time the interpreter needs to catch up is not board time.
MAIN now stops the virtual clock (`cpu_disable_ticks`, as a VM stop does) for
the duration of its wait for the DSP thread, and restarts it with the BQL held
only if the VM is still running. Lockstep, access_packets and the DSP's
slip-to-virtual-time pacing are unchanged. `CDJ_NXS_DSP_HOST_TIME=1` restores
the old charging for A/B. `tests/test_dsp_thread.py` checks the clock is held
while MAIN waits (seen from another thread with the BQL free), restarted after,
and left stopped when the VM was stopped meanwhile.

**Result** (same scenario, shared host, `runs/ps`):

| | old (`CDJ_NXS_DSP_HOST_TIME=1` / develop) | held clock |
| --- | --- | --- |
| PLAY pressed after load | frozen after ~150 half frames (5 of 5) | 07:11 34 → 07:06 01 in 90 s, still advancing (fix1, fix2) |
| no PLAY press (auto-play) | 07:11 02 → 07:04 59 in 90 s (np-old) | not needed |
| stream buffers after play | 4 blocks in 150 s | one 0x28-frame buffer per ~20M DSP cycles, steady |
| launch to `Not Loaded.` / USB list / duration reply | 8.2 / 1.2 / 1.15 s | unchanged |

Playback stays DSP-bound (~0.06x real time) and MAIN's virtual time now runs
slower than the wall clock while it waits on the DSP (about 55 virtual s in
150 s after PLAY); the wall-clock GUI board tolerated it in these runs.

**Not changed: the synchronous modes.** Holding the clock while the DSP runs
inside MAIN's DSPINT write (free, or charged at 150 Mpps) slowed MAIN's
virtual time 7-14x against the GUI board and Enter on [TRACK] never opened the
track list (sync1, sync2), so `--no-dsp-thread` keeps host-time charging. Its
"stall" in `runs/loadfix/stock-sync` was not one: the load took until the
end of the run (NOW LOADING on the screenshot before the last), and the DSP
transcript shows the position still advancing (0x90) when the run stopped.
Full-capture 45 s `--no-dsp-thread --functional-dsp-audio` boots, develop
binary against this one: all 125,638 common events and 646 checkpoints
byte-identical, same final screen.

**`--audio-clock virtual`** still ends in E-8302 CANNOT PLAY TRACK (3184):
92% of the McASP slots fire short of their packets, the known DSP-speed limit
of that mode (previous section), not this stall.

## C674x JIT stage 2: batched board work, direct traces, MAIN hand-over (`dsp-jit2`, 2026-10-06)

Three steps after the stage-1 roadmap above, each measured on its own,
plus a boot fix the faster DSP needed.  Benchmarks: `benchmark_core`, and a
board-like replay - `tools/cdj_dsp/replay.c` with its output compiled out,
RAM first on both bus callbacks as the QEMU board does, coverage off, and
QEMU's own hardening flags (`-ftrivial-auto-var-init=zero
-fzero-call-used-regs=used-gpr -fstack-protector-strong`, which zero every
uninitialized local) - over 20 M steps of the real-USB playback checkpoint
with functional audio and the JIT on.  Live: stock NXS, the confirmed
rekordbox USB, `link_hub`, `--lightweight --dsp-thread --audio-clock
virtual --no-dsp-idle-skip` (develop needs the last flag to boot, see
below), no PLAY press (stock auto-plays), DSP packets per wall second from
the end of the load, from the stamped 10-virtual-second reports; shared
host, two series an hour apart.

| | replay, user s | live packets/s (series 1 / 2) | per virtual s (2) | underrun slots (2) |
| --- | ---: | ---: | ---: | ---: |
| develop (e93d4f1) | 1.56 | boot failed / 10.3 M | 15.1 M | 89.9% |
| 1. batched ticks | 1.37 | 12.8 M / - | - | - |
| 2. direct traces (+ boot fix) | 1.20 | 15.4 M / - | - | - |
| + sampled EDMA/McASP logging | 1.20 | 16.6 M / - | - | - |
| 3. hand-over at MAIN's target | 1.20 | 16.4 M / 14.9 M | 20.6 M | 86.6% |
| + no-op per-step work skipped | 1.20 | - / 15.7 M | 21.8 M | 85.8% |

With the idle skip on (nxs_vm's default, which now boots) the final binary
ran 15.8 M packets/s.  `benchmark_core` (JIT on, develop -> final):
step-alu 77 -> 112 M packets/s, step-mem 35 -> 50 M, step-sploop,
step-memcpy and step-kernel unchanged (29, 29, 42-44 M).

**Real time: not reached.**  In every live run, develop's and the final
binary's alike, the counter advances about 150 half frames (one second of
track) at ~9 frames per wall second and E-8302 CANNOT PLAY TRACK (3184)
appears 15-30 s after the auto-play: the DSP still gets ~22 M packets per
virtual second against the ~63 M playback needs, and 86% of the McASP slots
fire short of their packets.  The DSP thread is now ~95% execution (the
hand-over spin was 8-25% of it); what is left is the core itself (direct
traces and loop kernels ~65%) and per-packet board work (~15%).

### 1. Batched board ticks (`cdj_dsp_ticks.h`)

The core calls the board's `cycle_tick` every DSP cycle; it clocked the
SPI1/WM8740 shift logic, the PLL's OSCIN counter and both Timer64Ps.  When
both timers are stopped and the SPI transfer is idle in
`cdj_spi_core_tick`'s fast path (or functional timing), n ticks are exactly
one SPI tick (idempotent there) and `cdj_c6747_pll_ticks(n)` (closed form,
countdowns included), and no tick can raise an event.  The QEMU board and
the replay then only count ticks and apply them (`dsp_ticks_flush`) before
any non-RAM bus access, at the end of each activation and before
checkpoints; they re-test the steady state after each per-cycle tick and
drop back to per-cycle ticking at every flush.  `CDJ_NXS_DSP_TICK_BATCH=0`
ticks every cycle.  The board's per-step PSC tick is skipped while no
power transition is in flight.

`tests/cstub/dsp-ticks.c` checks 200,000 random PLL/timer/SPI states
(genuine WM8740 transfers among them) state by state, and 3,000 random
sequences of ticks and register writes in lockstep, a batched system
against a per-cycle one (337 M ticks, 231 M of them only counted): equal
state at every flush and the same timer events at the same cycles.
Mutations - dropping either timer condition or the SPI-idle condition,
the SPI tick in the flush, one tick of the flush, or treating functional
timing as non-ticking - each fail it; dropping the replay's flush on a
register read or on a register write each changes a replay trace.

### Held clock and the DSP's pacing (boot fix)

With the held virtual clock (previous section) a DSP fast enough to reach
its pacing limit fails the stock boot in `--dsp-thread`.  With
`--audio-clock virtual`, develop failed its boot with the idle skip and 1
of 3 without it, step 1's binary both of its boots, and the stage-2 binary
3 of 4 without the idle skip (the one that passed wrote a full event
transcript, which slows MAIN).  MAIN's per-access
lockstep makes the DSP run 256 packets per HPI access while MAIN holds the
clock, so an 8,192-word firmware upload (2.1 M packets) leaves the DSP clock
~14 ms ahead of virtual time; the pacing then stops the DSP except for
those 256 packets per MAIN access, and MAIN's stage-1 handshake, which waits
~10 ms of virtual time for HINT after setting boot phase 2, gives up and
restarts the download with DSPINT set - which the DSP's read-modify-write of
HPIC (OR HINT, write back) then clears: HPIC 0x14e, and the boot never
leaves stage 1 (control-event traces with virtual timestamps: HINT 11.6 ms
of virtual time after phase 2, 8,281 DSP packets later).  On the board the
DSP runs on during an HPI access, so packets run for MAIN with the clock
held are not the DSP running ahead: `dsp_thread_credit` moves the DSP clock
by the part past the pacing limit (`credited=` in the thread report; ~17 M
packets by the end of boot, ~30 M by 100 virtual s of playback).  All ten
boots since the fix finished stage 1 (13 HINT handshakes, no 0x14e), with
and without the idle skip.  `tests/test_dsp_thread.py` checks the
credit; removing the call or the epoch move fails it.

### 2. Direct traces (`cdj_c674x.c`, "direct traces")

`cdj_c674x_run` now runs direct (non-loop) code as it runs loop cycles:
each packet with the effects `cdj_c674x_step` would have had, `between()`
after each, and no return to the board's step loop; the board and the
replay call it for direct code too.  A packet runs from a plan built once
per packet-cache entry (so per fetched bytes: changed code refills the
entry and drops the plan), checked against memory with the same byte
comparison as `fetch_cached`.  `dt_exec`:

- issues in instruction order with this core's own semantics: the stage-1
  JitOp memory and SP operations, the register-only arms (packet_single's
  list) and branch arms through a prebuilt arm, every other fast arm in full
  (`dt_arm`, as `jit_arm`), CMPSP, the long ADDA forms, and the compact
  value, BNOP, B15/Dpp and MVC-to-ILC forms, which `execute_packet` now
  shares through `compact_value`, `compact_bnop`, `compact_memory` and
  `compact_mvc_ilc`; E1 results are deferred, appended queue slots keep their
  old bytes, branch state and the two control registers written in place
  (FAUCR, ILC) are saved, so a declined packet leaves the CPU untouched;
- then runs every bus operation of every cycle of the packet in
  `execute_packet`'s order - tick, due store commits, due E3 reads - up to
  the cycle a branch matures in, before any register, queue entry or branch
  changes: a failing commit or E3 read needs only the issue undone, as
  `jit_exec` does for one cycle, and no snapshot;
- then retires cycle by cycle with `execute_packet`'s compaction
  (`JIT_COMPACT`, dead slots included), only in the cycles where an entry
  acts.

Between packets the queues are the CPU's own arrays, so an exit (interrupt,
unsupported packet, code change, limit) is a return.  A one-instruction
packet in `execute_single`'s shapes takes that path instead.
`CDJ_C674X_JIT=loops` compiles loop-buffer cycles only.  In the playback
replay 8.5 M of the ~9.5 M direct packets per 20 M steps run traced, in
runs of ~50 packets; the rest are SPLOOP setup, control-register writes and
loop loading.  The packet cache index now folds the high address bits, and
the hot paths' large scratch locals (`JitUndo`, which grew to whole queues,
the E3 data, the defer list) skip QEMU's zero-initialization, which had
cost 9.5% of the DSP thread in `memset`.

What it does not do: resolve the queue shape statically across packets.
The per-packet floor is now issue, the bus callbacks and the
byte comparison; a plan per (packet, queue shape) chained across packets,
and validation skipped while no write can have reached the code, are the
next levers on this path.

### 3. MAIN hand-over

The DSP thread used to end its chunk as soon as MAIN asked for the lock, so
MAIN found the DSP short of its 256-packet target, waited on `progress` and
let the DSP retake the lock - three hand-overs per access.  It now runs on
to the target first (`dsp_thread_main_due`); MAIN gets the lock at the same
packet.  On a playing DSP thread the yield spin fell from 8% to 3% of
samples (25% in stage 1's profile); packets per second within noise.  Also
board work: the EDMA and McASP write logging the audio ISRs triggered every
few hundred packets (3.5% of the thread, ~380 K stderr lines in four
minutes) is sampled (the first 1,024 of each kind, then every 65,536th
with its count), and the per-packet EDMA notification poll, interrupt
presentation and McASP slot check are skipped when they cannot act
(`cdj_c674x_interrupt_quiet`, the core's own no-op test).

### Exactness

- Replays (`replay.c`), develop against each step's commit and JIT off
  against on: checkpoints 1, 25, 250 (1 M steps), 400 (5 M), the first-play
  snapshot (5 M, strict and functional audio) and the real-USB playback
  checkpoint (10 M, strict and functional audio) identical; the playback
  checkpoint to 60 M steps (28,631,258 and 17,189,136 trace lines) gives
  identical traces and final checkpoints for develop and dsp-jit2, JIT off
  and on.
- 45-second full-capture stock boots (synchronous, JIT on), develop against
  dsp-jit2: all 266,423 events and 4,451 DSP checkpoints of the common
  prefix byte-identical; with `--functional-dsp-audio` 130,226 events and
  770 checkpoints.  dsp-jit2 JIT off against on: 286,292 / 4,988 and
  134,481 / 885.  (dsp-jit2 reaches 300,722 events in the same 45 s against
  develop's 266,423.)
- `tests/cstub/c674x-packet-cache.c` adds 24,000 random direct programs
  (build()'s mix with MPYSP, ADDSP/SUBSP, every load/store form, CMPSP,
  16x16 and half-word multiplies, ADDA, the long ADDAW form and ADDKPC)
  run through `cdj_c674x_run` against the uncached interpreter after every
  packet, with random interrupts, limits, refusals, code uploads, a
  withdrawn code window and failing commits and E3 reads (5.4 M traced
  packets, 22,256 faults), two directed declines after in-place state
  (a queue append, FAUCR, ILC; a DP compare's delayed FAUCR effect against
  a CMPSP), and checks `cdj_c674x_interrupt_quiet` against a real
  presentation at 9.6 M packets.  Clean under ASan/UBSan.
- Mutations of the direct traces: 18 of 21 fail it (the E1/E5 and FAUCR
  conflict checks, the overlap pairs, every undo and restore, tick order,
  the branch stop, E3 sign extension and order, the tail copy, the cycle
  count, the deferred results, NOP timing, predicates, the generic arm's
  undo and the code byte check).  Three are conservative checks the test
  cannot reach: the first packet's all-pairs overlap check (as in stage 1),
  refusing slow arms (the only one generated, MVC, is declined anyway) and
  PROT timing (no compact packets; the replays run them).
- Full suite: 947 passed; the two failures are the known TMU test and the
  orphan-watchdog flake.

## C674x JIT stage 3 (`dsp-jit3`, 2026-10-06)

Stage 2's levers in order, each measured on its own.  Benchmarks:
`build/j2/mkbench.sh`-style lean replays (RAM-first bus, no trace output,
QEMU's hardening flags) of two checkpoints, user seconds: the real-USB
playback checkpoint (20 M steps, functional audio) and a new **live**
checkpoint, `play-live`, taken from a `--dsp-thread --audio-clock virtual`
playback 8 s after the auto-play with a diagnostic build that allows
threaded checkpoints (6 M steps; its first ~3 M steps keep the live mix -
71% direct packets, as the live runs' 78% - before the replay's
packet-paced audio turns it into the memcpy-heavy mix of the older
checkpoint, 44% of steps in one 8-byte SPLOOP copy).

### 1. Board horizon (`CdjC674xHorizon`)

Most between-step work does nothing: no interrupt request, no slot edge,
MAIN not waiting.  `cdj_c674x_set_horizon` lets a board say so for a
stretch: the core skips `between()` after a packet while `cpu->packets <
until`, the PC is not `break_pc` and `cdj_c674x_interrupt_quiet` holds (the
CPU's own state can make a latched request eligible), and counts the skips
for the board's step count.  The board bounds `until` by everything
`dsp_post_step`/`dsp_pre_step` act on at a packet count - the activation
quota, the McASP slot deadline, MAIN's per-access target, 64 packets while
the idle skip looks for a loop (whose anchor PC becomes `break_pc`) - opens
none while something acts every step (an INTC request, an EDMA
notification, a PSC transition, unbatched ticks, the fault history), and
closes it (`until = 0`) from every callback after which between() could
act: device reads and writes, an unbatched tick, and MAIN raising
`host_waiting` (an atomic store from MAIN's thread, re-checked by the DSP
after it publishes a new bound).  The replay does the same under
`CDJ_DSP_REPLAY_HORIZON=1` (its per-step coverage summary is then left
out).  ~92% of between() calls are skipped on the live checkpoint.

### 2. Fetch epoch and a 64 K-entry packet cache

The packet cache checked each entry's fetch blocks through the board's
fetch-block hook every packet.  `cdj_c674x_set_fetch_epoch` registers a
counter the board moves on every committed device write and on DSP reset
(any of those could remap a block: EMIFB, the L1D partition); while it
stands still the entry's bytes are compared at the host pointers the hook
returned before.  The content check itself is unchanged.  The cache grows
from 16 K to 64 K entries (464 bytes each, 30 MB per stepping thread):
on the live checkpoint 48 K of 78 K misses per 3 M steps were conflicts,
9.8 K after.

| replay, user s | real-USB 20 M | live 6 M |
| --- | ---: | ---: |
| develop (251ed30) | 1.20 | 0.54 |
| 1. horizon | 1.01 | 0.48 |
| 2. fetch epoch, 64 K entries | 0.98 | 0.46 |

Exactness: `tests/cstub/c674x-packet-cache.c` runs four fifths of its JIT
and direct-trace seeds with a horizon: after a request-free presentation,
half the time a bound of 1-12 packets and sometimes a random break PC,
with a request the "board" presents exactly at the bound or the break PC;
system B catches up the skipped steps (each followed by the no-op
presentation the skip stood for, or by that request where B reaches it).
2.8 M between() calls are skipped.  Mutations - dropping the quiet check,
the break PC, or skipping at the bound itself - each fail it.  The fetch
epoch gets a swappable code window (0x1100-0x11ff maps to other code) the
direct-trace phase toggles; leaving one toggle's epoch move out, or
trusting stale host pointers, each fails it.  Replays (develop against
this, JIT on with the horizon against JIT on without; JIT off against JIT
off) identical apart from the left-out coverage lines: checkpoints 1, 25,
250 (1 M steps), 400 (5 M), the real-USB playback checkpoint (10 M, strict
and functional audio) and the live checkpoint (3 M, both).  The first-play
snapshot of the earlier stages (`runs/agent-play-checkpoints`) was deleted during this work and is no longer in the set.  45 s
full-capture stock boots (synchronous; the horizon is open there whenever
functional audio is off), develop against each step: all 298,354 events
and 5,314 DSP checkpoints of the common prefix byte-identical (each step's
binary gets further in the 45 s: 304-311 K events).  A first version
without the quota bound ran an activation 4 packets past its budget after
an idle skip; the boot comparison caught it.

### 3. Static schedules (`cdj_c674x.c`, "static schedules", "the lean path")

Compiled C674x code runs the same pipeline at the same PC each time: on
the live checkpoint 1.87 M traced packets showed 6,239 distinct (PC, queue
shape) pairs for 5,548 PCs.  A **shape** (`DtsShape`, interned per thread)
is what `dt_exec`'s decisions depend on in the queues - each entry's due
cycle relative to the CPU's, its kind (size low byte), register and sign
extension, and the pending branches' dues.  From the entry shape come the
landing registers and the write-conflict masks at start + 4 and + 5 (which
replace the per-operation queue scans); from the post-issue shape (entry
shape plus what the issue appended) the cycle in which each entry commits,
reads at E3 and retires, the maturing branch, the overlap-check pairs, and
two straight-line programs (`dts_program`): the bus phase, and retirement
with every compaction move and tail fill at the indices `dt_exec`'s
compaction uses, so dead slots get the same bytes.  Such a **variant**
belongs to its cache entry and records the shape the packet leaves, which
is the next packet's entry shape: within a run packets chain shape to shape
without looking at the queues (a real `between()` call ends the chain).
The queues stay the CPU's own arrays, so any packet can fall back to the
generic path and an exit is a return.

`dt_exec` finds a variant by entry shape and checks the issue's actual
appends (and cycle count, branches) entry by entry, building one when none
matches.  The **lean path** (`dts_lean`) then runs a packet whose plan is
lean - every instruction with a lean form - from its variant for the entry
shape and the predicates of its memory, SP and branch instructions: the
register-only arms NXS code consists of are computed in line (each its
arm's expression: ADD/SUB in every listed encoding, MVK/MVKH/ADDK, AND/OR/
XOR/ANDN, CMPxx(U)), memory and SP operations append without the scans
the variant settled, generic arms (`dt_arm`) and compact memory operations
are called as in `dt_exec` with their appends then checked against the
variant, and the bus phase and retirement run the variant's programs.
Everything data-dependent is still checked: addresses, alignment and bus
mapping, the candidate pairs' overlap, writes to registers landing in the
first cycle, two writes to one register, FAUCR conflicts.  A decline puts
back what the issue changed (appended slots, FAUCR, branch state, fault)
and hands the packet to `dt_exec`.  `CDJ_C674X_STATIC=0` turns both off.
With nothing in flight `execute_single` still runs one-instruction packets
(its result shape is known: empty, or one queued branch, cached per
entry).

### 4. ABSSP/CMPSP in compiled loops

Loop-buffer cycles holding ABSSP or CMPxxSP (FAUCR written in place) were
left to the transactional interpreter, ~1.5% of the live checkpoint's
steps at ~10x a compiled cycle's cost.  Their shapes now run on
`jit_exec` with FAUCR saved and put back on a decline, the arms sharing
one `controls` array (so two FAUCR writers in a packet conflict as in
`execute_packet`) and `execute_packet`'s delayed FP-status check; a
decline goes to the interpreter, never `execute_fast` (which cannot roll
FAUCR back).

### 5. Per-packet board trims

From the live DSP thread's profile: the fetch epoch now moves only on
cache-controller and EMIFB writes (the two models `dsp_memory_span` reads;
every device write had made the host pointers stale every few hundred
packets); while the board's ticks are only counted, the compiled paths add
cycles to `CdjC674xHorizon.ticks` instead of calling `cycle_tick` (the
board folds them into its debt wherever it applies it, the replay too); the
idle skip's anchoring bound is 1,024 packets (64 re-anchored, and logged
up to 64 RAM words, far too often); and the HPIC-write and WM8740-latch
reports (28 K and 18 K lines in a 4-minute run) are sampled like the EDMA
ones, the event-transcript path looked up once.

### Measurements so far

| | develop | 1-2 | 3-5 |
| --- | ---: | ---: | ---: |
| replay real-USB 20 M, user s | 1.20 | 0.98 | 0.94 |
| replay live 6 M, user s | 0.54 | 0.46 | 0.38 |
| `benchmark_core` step-mem (horizon) | 47 M/s | - | 54 M/s |
| `benchmark_core` step-alu (horizon) | 102 M/s | - | 78 M/s |
| live, DSP packets per wall s | 13.7 M | - | 16.7-17.5 M |
| live, per virtual s | 20.8 M | - | 24.0 M |
| live, underrun slots | 86.2-86.5% | - | 83.9-84.4% |

(Live: the virtual-clock scenario, A/B alternated, two runs each, shared
host at load ~10; E-8302 on screen in every run.)  step-alu is a loop of
one-instruction ALU packets and a branch, where `execute_single` was
already lean; the static path costs it the branch's packets.

### 6. RAM windows, tick batches in schedules, idle re-anchoring

`cdj_c674x_set_ram_window` lets a board describe its plain-RAM windows
(the QEMU board: L2 and its alias, the L1D SRAM partition, shared RAM,
enabled SDRAM and its mirrors, exactly `dsp_memory_span`'s); compiled
paths then read E3 values and check and commit stores in host memory
through a four-entry per-thread window cache (valid while the fetch epoch
stands), instead of the board's callbacks - stores only aligned, and only
while the board's `ram_direct` says its RAM write path would just store
the bytes (`CDJ_NXS_DSP_RAM_FAST=0` and a running idle-skip write log turn
it off).  The replay offers the windows for reads; direct stores only under
`CDJ_DSP_REPLAY_RAM_DIRECT=1`, since they leave out the write records its
trace prints.  The lockstep test's system gets windows (and a write log
its callback keeps only while stores may not bypass it); dropping the
epoch, the direct flag, the alignment test or the commit each fails it, as
does widening a window.  Static schedules' bus programs batch the ticks up
to each cycle with a bus operation, and an empty retirement program is
skipped; the lean path keeps the last variant per cache entry.  The idle
skip re-anchors a broken proof at most every 4,096 steps (each anchor
copies the CPU's registers and logs up to 64 RAM writes; in busy code
every horizon end used to take one).  Synchronous 45 s boots are faster
for it: 404,618 events in 45 s against develop's 298,354, the common
prefix byte-identical as before.

### 7. Code checks skipped between code-reaching writes; McASP slots in place

Inside `cdj_c674x_run` a packet-cache entry checked since the last write
that could have reached code is not compared again: a per-thread code
generation moves at every run's start, every `between()` a run calls,
every store committed through the board's callback and every store
committed directly into a host page holding a checked fetch block (a
hashed bit per 4 KB host page, set when a block is checked).  The
lockstep test gets a `between()` that sometimes writes code, as an EDMA
transfer could, and some long horizons (up to 2,000 packets, as on the
board); dropping any of the four moves or the page marking fails it.  The
QEMU board's functional McASP slot now runs on the board's own EDMA and
McASP state with a backup put back on failure, one 4 KB EDMA copy per
slot instead of two (all 142,177 events and 1,093 DSP checkpoints of a 45 s
full-capture `--functional-dsp-audio` boot identical to develop's).  RAM
window lookups compare an offset against the window's span and load and
store whole little-endian words.

### 8. The steady kernels' model, one record per entry

`benchmark_core` step-kernel ran at either ~35 M or ~19.6 M cycles/s from
run to run of one binary.  The slow runs had the JIT model page-aligned:
its address, value and size arrays kept each entry's three fields exactly
4 KB (or 8 KB, 12 KB) apart, which Apple silicon punishes in a loop that
stores one and loads another.  The model now keeps one record per
(operation, age); step-kernel runs at 34-35 M every time.

### 9. CALLP in traces

CALLP packets (8 K per 3 M steps of the live checkpoint) went to the
interpreter; `dt_callp` issues them as `execute_packet` does (B3 gets the
return address, a branch six cycles out, a six-cycle packet; a packet with
a parallel control instruction stays untraceable, as the interpreter
stops on it), in `dt_exec` and on the lean path.  The lockstep generator
now emits CALLP; dropping the pending-branch, multicycle or write-conflict
check, or moving the return address or the due cycle, each fails it.

### Stage 3 (C-level) exactness and results

- Replays, `dt-jit3` against develop (JIT on, and JIT off against JIT on,
  and this binary JIT off against JIT on with the horizon): checkpoints 1,
  25, 250, 400, the real-USB playback checkpoint (10 M, strict and
  functional audio) and the live checkpoint (3 M, both) identical (the
  horizon runs omit only the coverage lines).  The real-USB playback
  checkpoint to 60 M steps: develop and this binary, JIT off and on, give
  the same traces (28,631,258 and 17,189,136 lines) and final checkpoints;
  with the horizon the final checkpoints are identical and the traces too
  once the coverage lines are left out of both.
- 45 s full-capture stock boots against develop: all 298,354 events and
  5,314 DSP checkpoints of the common prefix byte-identical (this binary
  reaches 383,676 events in the 45 s); with `--functional-dsp-audio`,
  142,177 events and 1,093 checkpoints.  This binary JIT off against on:
  379,347 events and 7,503 checkpoints.
- `tests/cstub/c674x-packet-cache.c` clean under ASan/UBSan.
- Live, same session, alternated (two runs each): **25.1-25.6 M DSP packets
  per virtual second against develop's 21.0-21.3 M (+20%)**, 16.7-19.9 M
  per wall second against 15.5 M, underrun slots 83.3-83.5% against
  86.0-86.1%.  E-8302 on screen in every run: **real time is not reached**;
  the ~63 M per virtual second playback needs is ~2.5x away.  The DSP
  thread is now ~92% core (traces, loops, the interpreter at ~11%); the
  per-packet costs left are spread over issue, the queue programs, the
  cache checks and thread-local accesses, so the next stage compiles hot
  regions ahead of time (below).

## C674x stage 4: ahead-of-time regions (2026-10-06)

The stock DSP image is fixed, so its hot code can be compiled once, ahead
of time, instead of interpreted (or replayed from static schedules) on
every pass.  The approach follows Stijn Jacobs' `c14_jitgen.py` /
`c66x_jit.c` for cdj-nxs2-qemu (THIRD_PARTY.md): profile, generate C for
the hot code with the pipeline state fixed at generation time, key every
compiled piece by its code bytes, fall back to the existing paths where
the static state does not hold.  It is written against this core: the
generated code is the lean path's own pieces with constants.

### Design

- **Unit: a lean variant.**  A static schedule (stage 3) already fixes
  everything about a packet except its data for one (code bytes, entry
  queue shape, pmask predicates) triple.  `dts_lean` was split into
  always-inline pieces (`lean_begin`, `lean_issue`, `lean_check`,
  `lean_decline`, `lean_tick`/`lean_commit`/`lean_e3`, `lean_apply`,
  `lean_lret`, `lean_branch`, `lean_end`); `dts_lean` runs them for any
  variant, a generated node calls them with the variant's constants (op
  fields, predicates, append slots, landing masks, the bus and retirement
  programs unrolled), so the compiler specializes each packet and both
  paths share one implementation.
- **Profile.**  `cdj_c674x_aot_profile` counts lean runs per variant and
  the two commonest lean successors; `cdj_c674x_aot_profile_dump` writes
  the hot ones (fetch blocks and bytes, plan ops, shapes, programs,
  edges).  Replay: `CDJ_DSP_AOT_PROFILE=path`; the lockstep test:
  `PCTEST_AOT_PROFILE=path` (entries are dumped as they are refilled, so
  programs reusing addresses keep theirs).
- **Generator.**  `tools/cdj_dsp/aot_gen.py OUT.c PROFILE...` merges
  profiles, numbers the queue shapes, and emits one C function per group
  of up to 256 connected nodes: a node is its packet's code, then dt_run's
  post-packet steps (the run limit, the horizon), then its successors -
  profiled ones in line (next pc, the cache entry current and holding the
  successor's bytes, the predicates select the node, whose entry shape is
  this node's exit shape by construction; `goto` within a function, a
  guaranteed tail call across), any other compiled node through
  `aot_next` (the same checks against the node table).
- **Binding.**  A cache fill looks its bytes up (`aot_packet_of`: pc and
  every byte of its fetch blocks), a variant its node (`aot_node_of`:
  outcome and entry shape), so code other than the profiled image never
  runs compiled.  `dt_run` enters a region where it would run `dts_lean`;
  the region returns where dt_run must act (declined first packet, the
  limit, the horizon's end, a packet it cannot run, a fault).
- **Build.**  The generated file derives from the firmware, so it stays
  with the build (as nxs2 keeps its cache), not in the repository:
  `CDJ_C674X_AOT_SOURCE=file scripts/build-qemu-sh4.sh` copies it into
  the QEMU tree as `cdj_c674x_aot.inc` (and removes it when unset), the
  replay and test builds take `-DCDJ_C674X_AOT_FILE=...`.  Without a file
  the core builds as before; with one, `CDJ_C674X_AOT=1` turns it on
  (default off).

Rebuilding for a new profile: replay the checkpoints with
`CDJ_DSP_AOT_PROFILE`, run `aot_gen.py`, rebuild.  The profile used here
merged five replays (boot checkpoint 400 for 5 M steps; the real-USB
playback checkpoint and the live playback checkpoint, 20 M / 6 M steps,
strict and functional audio): 4,398 nodes, compiled with the QEMU build
in about a minute.

### Alongside

- **Code checks across runs.**  Every run start and every between()
  moved `code_gen`, so every packet's cache entry compared its 32-64 bytes
  again after each one (96% of the 1.25 G entry checks of a live run did,
  `entry_current` 6-7% of the DSP thread).  The board now counts its own
  writes into DSP memory that may reach code (`cdj_c674x_set_code_writes`
  / `cdj_c674x_may_hold_code`: its write callback's RAM commits, EDMA,
  the host port, the model window, the L1D clear at reset; a page holds
  code once a fetch block there was checked), and the core moves
  `code_gen` only when that count or the fetch epoch moved - after a run's
  start, after between(), and after a compiled store committed through the
  callback.  Without a counter (the replay tool, other boards) it moves
  as before.  `CDJ_NXS_DSP_CODE_WRITES=0` turns it off.
- **MVC to CSR and IER in traces.**  A live census of the interpreter
  steps the activation loop took found 112 M direct steps in 140 s, 94 M
  of them three packets of one routine writing IER/CSR (the
  `MVC CSR,B4 / AND / MVC B4,CSR` pattern).  `dt_plan` now traces MVC to
  CSR and IER (`dt_mvc_ctl`, written in place exactly as
  `execute_packet`'s commit writes them, restored on a decline), refusing
  a packet that also reads what it writes or writes one twice (the
  interpreter's fault); interrupt recognition stays where it was (the
  horizon's interrupt check after the packet, then between()).  Direct
  interpreter steps fell to 16 M, direct runs from 103 M to 9 M.
- **Thread-local state in one variable.**  The compiled paths' per-thread
  state (`code_gen`, the packet cache, the horizon, the RAM TLB, the
  counters) is one `_Thread_local` struct (`CDJ_C674X_TLS`), so a
  function locates it once; `horizon_skip` is always inlined.

### Direct form, run steps and the rest (473f10d)

- **Direct form** (`aot_gen.py`, `emit_direct`).  Where a packet holds
  only register-only, memory, SP, compact-value, compact stack (B15 word,
  Dpp) and B/BNOP displacement or register operations, and no write can
  conflict (no two writes to one register, none to a register landing that
  cycle), the node checks everything first without touching the CPU (a
  decline needs no undo), runs the bus phase (which then cannot act on its
  own appends - checked at generation), appends, writes registers and
  retires.  79% of the compiled runs of the stock profile take it; the rest
  keep the lean form (generic arms, compact BNOP, CALLP, MVC).  Profiles
  carry the core's enum numbering and the generated file asserts it.
- **Run steps** (`cdj_c674x_set_run_steps`, on in the NXS board,
  `CDJ_NXS_DSP_RUN_STEPS=0` off): a run steps the interpreter itself where
  no compiled path takes a packet and goes on, with between() or the
  horizon's skip of it after the step as after any packet.
- **Compact stack transfers in line** (`LOP_CB15`, `LOP_CDPP`) instead of
  `compact_memory`'s generic path.
- **RAM window fast paths and the interrupt check always inlined**, the
  RAM TLB move-to-front; the board stores directly again as soon as the
  idle proof breaks.

Exactness (all on 473f10d): the lockstep test (now also with compact fetch
packets, register branches, directed Dpp, duplicate-write,
self-modifying-code and CSR/IER programs, tick counting and run-count
checks) clean, also under ASan/UBSan; the AOT build of it (profile of the
test's own programs, every variant dumped, forced declines) clean, also
under ASan/UBSan; mutation checks on every new path (core and generator) -
the remaining misses are equivalent or unreachable by construction (a
disabled access's AMR-ready decline, the compact-value fault restore of a
form that cannot fault, aliasing PCs the harness cannot place, the
loop/idle test in `aot_next`, profiled edges whose shapes always match);
the eight checkpoint replays identical (develop JIT on vs this with the
horizon, develop JIT off vs this JIT on, this JIT off vs AOT on with the
horizon); the 60 M-step replays identical in trace and final checkpoint
(develop, JIT off, AOT on; with the horizon identical but for coverage
lines); 45 s full-capture boots byte-identical over the common prefix
against develop (298,354 events, 5,314 checkpoints; functional audio
142,177 / 1,093) and JIT off against AOT on (373,168 / 7,336); the full
suite passes but the known TMU test and the orphan-watchdog flake.

Live, real USB, `--lightweight --dsp-thread --audio-clock virtual`
(**preliminary**: the machine was shared with other emulator runs,
which moves single runs by ±10% and more):

| build | trials | per virtual s | per wall s | underrun slots |
|---|---|---|---|---|
| develop | 3 (interleaved) | 20.8-21.2 M | 15.0-16.3 M | 86.0-86.4% |
| stage 3 (b2eb06b) | 3 (interleaved) | 26.1-26.7 M | 19.0-20.8 M | 82.5-83.1% |
| this, AOT on | 2 (interleaved, MAIN slowed: virtual/wall 0.57-0.59) | 27.8 / 38.7 M | 15.9 / 23.0 M | 81.6 / 72.7% |
| this, AOT on | 4 (two earlier sessions, virtual/wall 0.73-0.78) | 39.5-44.1 M | 31-34 M | 73.1-75.5% |

E-8302 stays on screen: **real time is not reached**.  At ~40-44 M per
virtual second the gap to the ~63 M playback needs is ~1.5x.  The DSP
thread now spends ~40% in the compiled regions themselves, ~10% in loops,
~7% in stores the board logs for the idle proof, ~4% in the board's 4 KB
EDMA backup per McASP slot, ~5% in between().  What would close the gap:
regions that keep in-flight writes across packets (no queue array copies:
~4.5 struct copies per packet now), compiled SPLOOP kernels and loading
cycles, fewer horizon ends (each ~55 packets), an EDMA slot rollback that
copies only what a slot touches, and a cheaper idle-proof policy while
playing.  AOT stays off by default (`CDJ_C674X_AOT=1`).

## C674x stage 5 (`dsp-jit5`, 2026-10-06/07)

Goal: real-time stock playback under `--audio-clock virtual` (~63 M DSP
packets per virtual second, no E-8302).  Live numbers below are the
virtual-clock scenario of stage 4 (stock firmware, a real rekordbox
USB export, `--lightweight --dsp-thread --audio-clock virtual`, auto-play after
load), measured from 5 s after the load to the end of a 150 s run, on a
host shared with other emulator runs (load 11-17 on 16 cores).
Baseline (develop f3fe256 with the stage-4 AOT file): **50.6-52.8 M
packets per virtual second** (5 runs), underrun slots 66-69%.

### 1. Board: MAIN's hand-over, the idle skip, the slot's EDMA copies

A live profile of the DSP thread (`sample`, 10 s of playback) and
temporary counters (horizon ends by cause, interpreter steps by kind) gave:

- **MAIN's waits ended every horizon.**  85 M of ~120 M between() calls
  in a run came from `dsp_horizon_open` closing the horizon whenever
  `host_waiting` was set: MAIN tries the DSP lock on every HPI access
  (~400 K/s while a track plays), and while it waited for the DSP to reach
  `main_last + access_packets` the DSP called between() after *every*
  packet up to that point.  But `until` is already bounded by that packet
  count, and `dsp_thread_main_due` ends the chunk there: only a DSP past
  it must stop at once.  `dsp_horizon_open` now closes only then, and
  `main_lock` closes only when the published `until` lies past it
  (a Dekker pair: MAIN raises the flag, barrier, reads `until`; the DSP
  publishes `until`, barrier, reads the flag).  AOT region exits at a
  horizon end fell from 41.6 M to 5.5 M per run.  Live alone: +2%
  (52.5-52.8 M), within the noise.
- **The idle skip only costs under the virtual audio clock.**  During
  playback it never skipped (`idle-skipped` flat after boot: the next slot
  edge is always due for a DSP behind virtual time), but every activation
  re-anchored (a state copy), RAM stores went through the logging
  callback instead of the direct path, and every pass of the anchor PC
  was a between().  `--no-dsp-idle-skip` alone: **58.8-59.9 M** against
  ~51.5 M, and `Not Loaded.` 1.1 s sooner (no 2 s GUI head start, which
  only exists for the skip).  A back-off of the anchoring instead (fewer
  anchors after unproductive ones, the GUI head start kept) put 4 of 6
  runs into a different firmware regime: the DSP spent most of its time
  in the 8-byte SPLOOP memcpy at 0x11804838 (2.0 G steady kernel cycles
  per run against ~0.37 G) at ~33 M per virtual second, as did 2 of 5
  runs with the skip off in the board but the head start kept.  Skipping
  is exact either way (45 s boots byte-identical), so this is timing
  sensitivity of the firmware, not a fault; the launcher setting that
  never showed it is the one adopted: `nxs_vm` now defaults to
  `--no-dsp-idle-skip` (and so no GUI head start) with `--audio-clock
  virtual`; `--dsp-idle-skip` still turns it on.
- **The McASP slot's EDMA copies.**  Each slot copied the 4 KB EDMA state
  twice: the board's undo copy and `cdj_c6747_edma_event`'s own trial run
  on a copy (`preview`).  Every board caller of `service_mcasp_axevt`
  already discards or restores both models on failure, so it now uses
  `cdj_c6747_edma_event_unchecked` (the commit pass alone, which fails
  exactly where the trial would) with a journal of the PaRAM sets the
  events change (an event changes only its own set; each is saved once,
  before its first change), and the slot copies only the ~250 bytes of
  registers outside `param[]`.  `tests/cstub/c6747-edma.c` checks the
  unchecked events against the checked ones (state and bus) and the undo
  after a failure in a chained transfer.
- **QoS.**  The DSP thread asks macOS for the performance cores
  (`QOS_CLASS_USER_INTERACTIVE`): on the busy host an occasional run had
  shown about half the packet rate per wall second with the normal kernel
  mix, as on an efficiency core.  Not measured on its own.

Exactness: 45 s full-capture stock boots against develop, all 298,354
events and 5,314 checkpoints of the common prefix byte-identical (this
binary reaches 395,405 events), with `--functional-dsp-audio` 142,177 and
1,093 (the slot path with the journal); `tests/test_dsp_thread.py` (MAIN's
hand-over under the shim) and the EDMA test pass.

Live, three runs: **61.8 / 60.8 / 53.3 M per virtual second** (the last
at virtual/wall 0.70 under load 12-17), underrun slots 61-64%.  E-8302
stays on screen: real time is not reached yet.

**Where E-8302 comes from.**  In every virtual-clock run, at 26 to 67 M
packets per virtual second and with DSP clocks of 60, 100 and 150 Mpps, the
counter stops at elapsed frame 239 (1.6 s into the track) and E-8302
follows; with the packets clock the same build plays on (slowly, 0.09x,
pause/resume/seek/cue all work).  A temporary log of MAIN's HPI writes to
the stream header (0x11838140) and data hand-over (0x118381c4), and of
the DSP's frames-ahead/position words (0x11837cd0/0x11837c10) at DSPINT,
shows why: before play MAIN hands over a block every ~0.1 virtual s and
preloads 0x78 frames; once playing, the DSP consumes them at about real
time (0x76 frames in 2.8 virtual s) and then waits with one frame ahead,
while MAIN hands over a block only every ~2-2.5 virtual s (a 0x28-frame
buffer per ~6.5 s against ~1 s needed).  The feed is starved, not the
decode: the DSP's background tasks, which serve MAIN's stream traffic,
get what the audio interrupts leave.  Weighting packets by CSR.GIE at
each between(): of ~57 M packets per virtual second, ~36 M ran with
interrupts disabled (the ISRs, mostly) and ~21 M in the background.  So
"~63 M packets per virtual second" understates what real time needs: the
background must be several times faster, i.e. a DSP well above 100 M
packets per virtual second.

**Measurement.**  The DSP thread's report now includes its CPU time
(`thread-cpu=`), so `rate.py` gives packets per DSP-thread CPU second,
which the host's load moves far less than packets per wall second.

### 2. One copy of the core's compiled-path state

The compiled paths' per-thread state (`core_tls`: code generation, packet
cache, horizon, RAM TLB, counters; the loop tables, the steady-kernel
model, the shape table) was `_Thread_local`; on Mach-O each access is a
`_tlv_get_addr` call, and the AOT regions' chain checks (`aot_current`,
`horizon_skip`, the tick counting) made them 9% of the DSP thread
(`aot_current` 5.3% self, `_tlv_get_addr` 3.6%).  Every user steps one
core on one thread at a time (the board holds its DSP lock around every
call, from either thread; replay and tests are single-threaded), so the
state is now one process-wide copy; `-DCDJ_C674X_TLS=_Thread_local`
restores per-thread copies for a client that needs them.  (As a side
effect MAIN's HPI writes now see the code pages the DSP thread marked,
which `cdj_c674x_may_hold_code` reads; before, MAIN's own empty
thread-local copy answered.)

Exactness: the eight checkpoint replays identical (develop JIT on against
this with the horizon; this JIT off against AOT on with the horizon), 45 s
full-capture boot byte-identical over the common prefix (298,354 events,
5,314 checkpoints), C674x and DSP test files pass.

Live, alternated with step 1 (two each): **51.1 / 53.1 M packets per
DSP-thread CPU second against 43.7 / 45.1 M (+17%)**; per virtual second
63.1 / 66.5 M against 55.7 / 56.7 M.  E-8302 as before.

### 3. What real time needs: the playback phase, not the run average

The live numbers above are run averages, and most of a 150 s run is spent
after E-8302, where the DSP sits in fast polling code.  A temporary board
diagnostic that splits packets by the stream's state (frames ahead
`0x11837cd0`, position `0x11837c10`, header `0x11838140` = `0xffffffff`
after the error) and by CSR.GIE gives, per virtual second (HEAD of step 2,
150 Mpps DSP clock, five runs on a shared host):

| state | packets | ISRs (GIE 0) | background | underrun slots |
|---|---:|---:|---:|---:|
| before play | 52-81 M | 32-49 M | 20-32 M | 46-66% |
| playing, frames ahead | 26-37 M | 26-36 M | 0.5-1.2 M | 81-86% |
| playing, buffer dry | 23-31 M | 22-29 M | 0.6-1.7 M | 79-85% |
| after E-8302 | 54-76 M | 33-45 M | 21-31 M | 49-64% |

While a track plays, the audio interrupts take everything the DSP gets and
the background (which copies MAIN's stream blocks: `0x118381c4` is cleared
by the block copy at `0x11804854`) gets ~1 M packets per virtual second.  A
log of `0x118381c4`/`0x11838140` shows the hand-over is DSP-paced: MAIN
writes the next block ~20 ms after the DSP clears the flag, every ~0.15
virtual s before play and every ~2.5 virtual s once playing.  PC histograms
put the playing ISRs in `dsp_varispeed_resample` (`0xc00100c0`-`0xc0010e00`,
SPLOOPs, about half), `dsp_output_cursor_advance` (`0xc0006f18`, ~14%) and
`0xc00057c0`.

**The demand**, with MAIN unconstrained: `--audio-clock packets` with the
slot interval made configurable (diagnostic build) plays on past frame 450
at 1,024 packets per slot, stops at frame 239 (as under the virtual clock)
at 768, and never starts at 512.  At 88,200 slots per second the stock
playback needs more than 68 M and at most 90 M DSP packets per virtual
second, delivered on time.  The ~63 M of stage 4 was a run average; in the
playing phase the live DSP delivers ~30 M.

**Where the playing DSP's time goes.**  A checkpoint taken 3 s into
playback (`build/ck/play-wet`, diagnostic: a DSP-thread run) replays like
the other playback checkpoints; on those the time goes to loops: steady
kernels (`jk_exec`, `jit_cycle`, ~40%: the stage-1 memcpy alone is ~40% of
all packets, the AAC decoder copying through it), loading cycles through
`loop_step` (~20%: 3% of the packets, each with ~7 KB of scratch copies),
then the direct/AOT code.  Live, the DSP thread also waits for MAIN's HPI
accesses (`sched_yield`/mutex, ~15% of its time while playing).

### 4. Loops: bursts, AOT kernels, loading cycles in place, bulk copies

- **Bursts** (`jk_burst`).  After a steady cycle whose between() the
  horizon skips, the next full steady cycles of the kernel run in one loop:
  only `cycle < post_cycle` of `jit_ready` can change between them, and
  each phase's shape is looked up once.  Bursts average ~730 cycles.
- **AOT kernels.**  The profile dumps every hot loop's `JitKernel` bytes (J
  lines, `KJ` for the layout); `aot_gen.py` emits them as constant
  `JitKernel`s with `jk_burst_t`/`jk_exec_t` over them, so the compiler
  folds each phase.  A loop uses one when its own kernel has the same bytes
  (`CDJ_C674X_AOT_KERNELS=0` turns them off).  Six kernels from the seven
  replays (`--kernel-min`, default 10,000 steady cycles).
- **Loading cycles in place** (`loop_load_in_place`).  A loading cycle of a
  plain SPLOOP/SPLOOPD - no reload, interrupt, retained buffer, SPMASK,
  SPLOOPW or IDLE - parses its packet first, then writes in place with a
  small undo instead of `loop_step`'s scratch copies; anything `loop_step`
  would stop on, and any packet `execute_fast` declines, goes to `loop_step`
  untouched.
- **Bulk copies** (`jk_bulk`).  A kernel of one 8-byte post-increment load
  and one store of the same register pair (the stage-1 memcpy) is advanced
  N periods at once at a period start: every in-flight value is checked
  against the memory it came from, the source and destination ranges must
  not overlap and lie in RAM windows with direct stores, the horizon, the
  limit, the full stretch and `post_cycle` must leave room for N periods
  and one more; then the committed stores are one `memcpy`, the model, the
  registers, the queue tails, ILC, the loop condition history and the
  counters are set as N * II steady cycles leave them.  In the replays it
  takes all of the memcpy's steady cycles (11.35 M of 30 M packets on
  `play-wet`).

Exactness: the ten checkpoint replays (boot 1/25/250/400, play-real,
play-live, play-wet, each with and without functional audio) identical
between the interpreter (JIT off) and AOT + kernels + horizon (steps
burst, kernels, loading) and, with direct RAM writes on both sides,
identical to HEAD (all four steps); lockstep plain / AOT (kernels from the
test's own profile) / ASan / ASan+AOT; mutations of the burst's ILC, the
loading cycle's wait and packet count, the bulk copy's length and its
store-base update are caught (mutations of the bulk's queue-tail due cycle
and pair register are not: both are overwritten by the period that always
follows a bulk step before anything can read them); 45 s full-capture
boots byte-identical to develop over the common prefix (298,354 events /
5,314 checkpoints; functional audio 142,177 / 1,093).

Lean replay, user s (stock-AOT HEAD build -> this):

| replay | HEAD | bursts | + AOT kernels | + loading in place | + bulk copy |
|---|---:|---:|---:|---:|---:|
| play-real 20 M | 0.76 | 0.67 | 0.63 | 0.57 | 0.52 |
| play-wet 60 M | 2.24 | - | 1.78 | 1.62 | 1.46 |

**Live: no change in the playing phase.**  Same diagnostic, four runs of
this branch's builds alternated with four of HEAD on a shared host: playing
32-35 M per virtual second against 32-37 M, buffer dry 31-32 M against
30-31 M, the run average 31-78 M against 31-75 M (the host's load moves
single runs by 2x).  One run (AOT kernels, before the bulk copy) stayed 51
virtual s with the buffer dry but the background busy (67 M of 75 M per
virtual second with GIE 1), without recovering playback.  The playing DSP is ISR-bound
(97% GIE 0) and the ISR mix (varispeed SPLOOPs with SP arithmetic, the
cursor-advance loop, the board's per-slot work, the MAIN hand-over) did not
get faster; the replay gains are in the memcpy and the loop machinery the
decoder uses, which only run once the background gets time.  E-8302 stays.
Real time needs ~2.5x on the playing ISR mix as well as the background.
