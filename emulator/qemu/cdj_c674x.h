/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_H
#define CDJ_C674X_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "cdj_c674x_loop.h"
/* Partial interpreter. Encodings and semantics come from TI SPRUFE8B; no
 * third-party decoder code. Coverage is far wider than the seven instructions
 * this comment used to name: DSP_ARCHITECTURE_COVERAGE.md holds the measured
 * position against the manual's 240 Table A-1 rows, and
 * analysis/dsp/isa_probe.json is regenerated from TI's own assembler. Do not
 * infer coverage from this header. */
typedef struct {
    uint64_t due, value;
    uint32_t address;
    /* Low byte: transfer size. High byte: issue-time circular address width
     * (0=linear, 1..32 bits) for nonaligned transfers. */
    unsigned size;
} CdjC674xStore;
typedef struct {
    uint64_t due, value;
    /* Low size byte 1/2/4/8 is a memory load; high byte retains circular
     * address width as in CdjC674xStore. Size 0 is an already-computed delayed
     * scalar result and size 16 an already-computed register-pair result.
     * Sizes 32/33 are delayed IFR set/clear effects; their address field is
     * the interrupt mask and they never write a general register.
     * For size 0, address may be a floating-point status-bit OR mask and
     * sign_extend selects FADCR (false) or FMCR (true).  These sentinels reuse
     * the ABI-stable writeback queue so schema-1 checkpoints retain every
     * in-flight four-cycle result. */
    uint32_t address;
    unsigned bank, dst, size;
    bool sign_extend;
} CdjC674xLoad;
#define CDJ_C674X_DELAYED_IFR_SET 32u
#define CDJ_C674X_DELAYED_IFR_CLEAR 33u
/* No GPR write: address is the SSR unit mask, with CSR.SAT set in parallel. */
#define CDJ_C674X_DELAYED_SAT 34u
/* No GPR write: address is an FAUCR status OR mask, already shifted into the
 * unit's half.  The DP compares write dst and FAUCR on the same later cycle
 * (SPRUFE8B 4.2.10, printed page 598), and sign_extend selects only between
 * FADCR and FMCR, so the FAUCR half of that pair needs its own entry. */
#define CDJ_C674X_DELAYED_FAUCR 35u
/* idle_cycles sentinel for the IDLE instruction's unbounded wait. */
#define CDJ_C674X_IDLE_FOREVER (~0u)
typedef struct {
    uint32_t word, pc, header;
    bool compact;
} CdjC674xInstruction;
typedef struct {
    CdjC674xInstruction instructions[8];
    unsigned count;
    uint32_t next_pc;
    bool single_cycle;
} CdjC674xPacket;
typedef struct {
    uint32_t r[2][32], control[32], pc;
    uint64_t cycles, packets, branch_due;
    /* Optional board clock edge, after advancing the cycle and before E3
     * bus effects. Runs for inserted NOPs too, never for a decode rejection.
     * Must not fail or modify CPU state. Rebind after reset/checkpoint restore.
     * Like bus commits, external effects cannot roll back a broken callback. */
    void (*cycle_tick)(void *);
    void *cycle_opaque;
    /* Delayed control-register availability. ID 31 is not a C674x control
     * register exposed by this core; its otherwise-unused slot preserves the
     * software-loop setup PC, interrupt-drain phase, and compact retained
     * buffer metadata in existing checkpoints without changing the CPU ABI. */
    uint64_t control_ready[32];
    uint32_t branch_target, fault_pc, fault_word;
    struct { uint64_t due; uint32_t target; } branch_queue[5];
    unsigned branch_count;
    const char *fault;
    CdjC674xStore stores[24];
    unsigned store_count;
    CdjC674xLoad loads[40];
    unsigned load_count;
    bool loop_active;
    /* idle_cycles counts issue cycles in which no packet is fetched: the
     * interrupt pipe-down interval and the padding of a single-cycle packet.
     * CDJ_C674X_IDLE_FOREVER is the IDLE instruction's unbounded wait
     * (SPRUFE8B printed page 274, "infinite multicycle NOP"), held in this
     * existing field so that sizeof(CdjC674x) - and with it the checkpoint
     * ABI, which stores this struct verbatim - does not change. */
    unsigned idle_cycles, loop_wait, loop_tags, loop_packets;
    unsigned loop_pred_bank, loop_pred_reg, loop_pred_history;
    bool loop_pred_invert;
    CdjC674xLoop loop;
    CdjC674xInstruction loop_instructions[112];
} CdjC674x;
/* Reads must be side-effect-free and remain mapped between E1 validation and
 * E3 sampling. Read-clear registers require a future bus transaction API. */
typedef bool (*CdjC674xRead)(void *, uint32_t, uint32_t *);
/* commit=false checks a write without effects. A successful check must
 * guarantee a later commit succeeds; callbacks must write the whole transfer.
 * Simple MMIO registers can apply effects at commit; mapping/acceptance must
 * remain stable, even if an earlier in-flight store changes register state. */
typedef bool (*CdjC674xWrite)(void *, uint32_t, uint64_t, unsigned, bool commit);
/* Fetch and execution are separate so loop-buffer instructions retain their
 * original PC/header and share one architectural commit with overlaid code.
 * Fetch reads only pc and fault from the CPU; rejection writes fault,
 * fault_pc and fault_word. Observers may supply a scratch CPU with only
 * pc/fault initialized. Registers, pipeline and loop state are not accessed. */
bool cdj_c674x_fetch(CdjC674x *, CdjC674xRead, void *, CdjC674xPacket *);
/* The reserved gap between L2 RAM and L1P, 0x11840000-0x11DFFFFF.  Stock's
 * bit reversal (MP4AACDEC_TIJ_scramble, a plain branch loop at 0xC0030D80)
 * has LDDW *+A4[A9] at 0xC0030E80 in a delay slot of [A1] B at 0xC0030E6C;
 * on the last iteration it prefetches pair (16,0), one entry past its
 * table, into the gap.  SPRUFE8B: a branch's 5 delay slots execute, so the
 * load really happens on hardware.  TI leaves reserved-address accesses
 * undefined (SPRS377F section 3.4).  Stock's AAC runs on silicon, so the
 * load completes without a fatal exception.  Its value is never consumed
 * (verified: identical output for 0 or 0xDEADBEEF), so 0 is an arbitrary
 * choice.  With the gap enabled (default) a DATA load of an unmapped word
 * there completes with zero and is counted once per word read
 * (cdj_c674x_data_gap_reads); stores and instruction fetches of the gap
 * still fault.  The gap's local alias 0x00840000-0x00DFFFFF is not handled
 * (it still faults).  Process-wide. */
#define CDJ_C674X_DATA_GAP_BASE 0x11840000u
#define CDJ_C674X_DATA_GAP_END  0x11e00000u
void cdj_c674x_set_data_gap(bool on);
uint64_t cdj_c674x_data_gap_reads(void);

/* Optional fetch fast path.  Returns a host pointer to the 32 bytes at the
 * 32-byte-aligned address `block` when that whole block is plain memory which
 * the paired read callback would return word for word (little-endian), else
 * NULL, and then fetch falls back to the read callback.  Must be side-effect
 * free like read.  The pointer is used only within one call; the packet
 * cache below keeps copies of the bytes, never the pointer, so code writes
 * stay immediately visible.  Register once at board setup; the hook applies
 * only when fetch or step is given `read`. */
typedef const uint8_t *(*CdjC674xFetchBlock)(void *opaque, uint32_t block);
void cdj_c674x_set_fetch_block(CdjC674xRead read, CdjC674xFetchBlock block);
/* Optional: a counter the board moves (from the thread that runs the DSP,
 * or with it stopped) whenever any fetch block could map to other host
 * memory or stop mapping - a device write that may remap memory, a reset,
 * a restore.  While it stands still, the packet cache checks its entries'
 * bytes at the host pointers fetch_block returned before, without calling
 * it again (the content check itself is unchanged). */
void cdj_c674x_set_fetch_epoch(const uint64_t *epoch);
/* Optional: the board's plain-RAM windows, for compiled execution's data
 * accesses.  window(opaque, address, &lo, &hi, &host) returns true when
 * address lies in a window [lo, hi) of memory that the read callback (the
 * one registered with the fetch-block hook) returns byte for byte from
 * host (host[0] is lo's byte) and whose layout changes only where the
 * fetch epoch moves; false otherwise.  While *direct is true, the write
 * callback `write` would do nothing for an aligned 1/2/4/8-byte store into
 * a window but check (commit=false: succeed) or store the bytes
 * little-endian (commit=true), so compiled paths do that themselves.  The
 * board updates *direct from the thread that runs the DSP.  Needs the
 * fetch epoch (cdj_c674x_set_fetch_epoch). */
typedef bool (*CdjC674xRamWindow)(void *opaque, uint32_t address,
                                  uint32_t *lo, uint32_t *hi, uint8_t **host);
void cdj_c674x_set_ram_window(CdjC674xWrite write, CdjC674xRamWindow window,
                              const bool *direct);
/* Direct steps keep a per-thread cache of fetched and decoded packets, valid
 * only while the bytes it was built from are unchanged: every hit re-reads
 * them through the fetch-block hook, so code writes by anyone are seen at the
 * next fetch.  Packets that write no control register then run on a fast
 * path without the transactional prefix copy; results, faults and rollback
 * are byte-identical to the uncached path.  Mode 0 off, 1 cache only,
 * 2 cache and fast path (default; CDJ_C674X_PACKET_CACHE=0|decode|... sets it
 * at first use).  Process-wide; for tests and A/B measurement. */
void cdj_c674x_set_packet_cache(int mode);
bool cdj_c674x_execute(CdjC674x *, const CdjC674xPacket *, CdjC674xRead,
                      CdjC674xWrite, void *);
void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry);
/* Present already-selected CPU interrupt requests at an execute-packet
 * boundary. Bits 4..15 correspond to INT4..INT15; all other bits are
 * rejected. Requests latch in IFR even while masked. A recognized interrupt
 * redirects the next fetch to its IST entry without advancing CPU time. */
bool cdj_c674x_interrupt(CdjC674x *cpu, uint32_t pending);
/* True when cdj_c674x_interrupt(cpu, 0) would return true and change
 * nothing, so a board with no request to present may skip the call. */
bool cdj_c674x_interrupt_quiet(const CdjC674x *cpu);
bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque);
/* Optional direct source packet observation, before loop-setup transformations.
 * Consume only when the step succeeds. count=0 for loop/idle steps; this does
 * not observe loop-buffer source fetches. No extra bus reads or CPU effects. */
bool cdj_c674x_step_capture_direct(CdjC674x *, CdjC674xRead, CdjC674xWrite,
                                  void *, CdjC674xPacket *);

/* Compiled execution (the "JIT"; see "Compiled SPLOOP kernels" in
 * cdj_c674x.c).  cdj_c674x_run executes up to `limit` packets from compiled
 * code, each with exactly the effects cdj_c674x_step would have had, and
 * calls between(between_opaque) after every packet except the limit-th
 * (and except where the board horizon below lets it skip one).
 * between must do everything the caller does between two steps - its
 * post-step work for the packet just run and its pre-step work for the next,
 * interrupt presentation through cdj_c674x_interrupt included - and returns
 * false where the caller's own loop would stop.  The return value is the
 * number of packets executed; *status says what follows the last one:
 *   0                          nothing: the caller does its post-step work
 *                              (and steps itself when the return is 0);
 *   CDJ_C674X_RUN_BETWEEN      between ran and returned true: the caller is
 *                              positioned just before a step;
 *   CDJ_C674X_RUN_STOPPED      between returned false: stop as the caller's
 *                              loop would;
 *   CDJ_C674X_RUN_FAULT        packet n+1 failed exactly as cdj_c674x_step
 *                              would have (cpu->fault set).
 * Off unless CDJ_C674X_JIT=1 (or cdj_c674x_set_jit(1)); =loops compiles
 * loop-buffer cycles only.  Needs packet-cache mode 2.  Process-wide, like the packet cache. */
typedef bool (*CdjC674xBetween)(void *opaque);
/* With run steps on (default off), cdj_c674x_run steps the interpreter
 * itself where no compiled path takes a packet - exactly cdj_c674x_step -
 * and goes on with between() (or the horizon's skip of it) after it, as
 * after any packet; it then returns 0 only with nothing run at all (never
 * for that reason), and a fault of the interpreter's step returns
 * CDJ_C674X_RUN_FAULT. */
void cdj_c674x_set_run_steps(bool on);
enum {
    CDJ_C674X_RUN_BETWEEN = 1,
    CDJ_C674X_RUN_STOPPED = 2,
    CDJ_C674X_RUN_FAULT = 3,
};
/* While a run is in a steady-state kernel the queue arrays (stores[],
 * loads[]) are rebuilt only when it ends; their counts and everything else
 * stay exact.  between() must therefore not read the queue arrays - the
 * boards' between-step work does not - or call cdj_c674x_sync first, which
 * rebuilds them (and costs the speed).  Every return from cdj_c674x_run
 * leaves them exact. */
void cdj_c674x_sync(CdjC674x *cpu);
/* *out = *cpu with the queue arrays as cdj_c674x_sync would rebuild them,
 * without ending steady execution (for tests comparing every packet). */
void cdj_c674x_view(const CdjC674x *cpu, CdjC674x *out);
unsigned cdj_c674x_run(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write,
                       void *opaque, unsigned limit, CdjC674xBetween between,
                       void *between_opaque, unsigned *status);
/* Board horizon (optional, per thread): a stretch of packets over which the
 * board has proved its between() a no-op that returns true, apart from
 * counting the step.  cdj_c674x_run then skips between() after a packet
 * when cpu->packets < until, cpu->pc != break_pc (0: none) and
 * cdj_c674x_interrupt_quiet(cpu), and counts each skip in `skipped` for the
 * board to add to its step count before it next uses it.  The board lowers
 * `until` (to 0) from any callback after which between() could act - a
 * device access, a tick that raised or could raise an event - and may do so
 * from another thread with an atomic store; the core reads it atomically
 * after every packet. */
typedef struct {
    uint64_t until;
    uint32_t break_pc;
    uint64_t skipped;
    /* While count_ticks (the board's choice: its cycle_tick would only
     * count the cycle), the compiled paths add each cycle to `ticks`
     * instead of calling cycle_tick; the board takes them from there
     * wherever it applies its counted ticks. */
    bool count_ticks;
    uint64_t ticks;
} CdjC674xHorizon;
void cdj_c674x_set_horizon(CdjC674xHorizon *horizon);
/* A board that counts its own writes into DSP memory that may reach code
 * - every write it makes other than the core's direct stores: its write
 * callback's commits, DMA, the host port - lets the compiled paths keep
 * their code checks across runs and between(): it registers the counter
 * here and increments it (atomically, from any thread) after each write
 * for which cdj_c674x_may_hold_code(host bytes, size) is true, and after
 * anything else that rewrites memory wholesale.  Without one, every run
 * and between() rechecks the code it runs. */
void cdj_c674x_set_code_writes(const uint64_t *counter);
bool cdj_c674x_may_hold_code(const void *host, size_t size);
/* Ahead-of-time compilation input (tools/cdj_dsp/aot_gen.py): count lean
 * packets and their successors on the calling thread, and write the hot
 * ones (at least min_runs runs) to f.  Diagnostic; off by default. */
void cdj_c674x_aot_profile(bool on);
void cdj_c674x_aot_profile_dump(FILE *f, uint64_t min_runs);
/* And as cache entries are refilled, while profiling, to f (NULL: off). */
void cdj_c674x_aot_profile_to(FILE *f, uint64_t min_runs);
void cdj_c674x_set_jit(int enabled);
bool cdj_c674x_jit_enabled(void);
/* Counters of the calling thread's compiled execution, for reports and
 * tests: runs that executed a packet, loop-buffer cycles by path (native
 * jit_exec, execute_fast fallback, steady-state kernel), loop compiles;
 * direct packets run from plans, direct runs, plans built and packets
 * found not traceable. */
typedef struct {
    uint64_t runs, native, generic, compiles, steady;
    uint64_t direct, direct_runs, direct_plans, direct_untraceable;
    /* Direct packets run from a static schedule, schedules built, and
     * shaped packets that fitted none. */
    uint64_t static_hits, static_builds, static_misses;
    uint64_t static_lean;               /* of those, on the lean path */
    uint64_t aot;                       /* of those, compiled ahead of time */
    /* Region calls by how they ended: declined, limit, between, cont,
     * fault, redo. */
    uint64_t aot_exit[6];
} CdjC674xJitStats;
void cdj_c674x_jit_stats(CdjC674xJitStats *stats);

/* Introspection of the conditional-instruction dispatch table, for the one
 * test that proves no two rows can claim the same instruction word.
 *
 * Selection is first-match-wins over cdj_c674x_arms[], so a new row whose
 * mask/match overlaps an existing row's is silently shadowed by whichever comes
 * first and nothing in the build complains.  That is the failure mode these two
 * functions exist to make mechanical instead of a matter of careful reading:
 * rows i and j can both match some word exactly when
 *
 *     ((match_i ^ match_j) & mask_i & mask_j) == 0
 *
 * which is a closed-form check over every pair, needing no instruction sweep.
 * Only the three scalars a shadow check needs are exposed - never the row's
 * predicate or arm pointers, and nothing that reaches CPU state.  `has_also`
 * reports whether the row carries an `also` predicate, which is the documented
 * way two overlapping rows are legitimately disambiguated.  Row order is the
 * table's own.  Returns false for an out-of-range index. */
unsigned cdj_c674x_arm_table_rows(void);
bool cdj_c674x_arm_table_row(unsigned index, uint32_t *mask, uint32_t *match,
                             bool *has_also);
/* Whether row `index` actually claims `word` - mask/match AND its `also`
 * predicate.  This is what the closed-form check above cannot see: 251 pairs
 * overlap on mask/match alone and are separated only by a predicate, so the
 * mask/match check over-reports and something has to decide whether any word
 * really reaches two rows.  Every predicate is a pure function of the word, so
 * evaluating one needs no CPU; predicates_are_word_only() re-establishes that
 * for a given word, and must be true for a claims() result to mean anything. */
bool cdj_c674x_arm_table_row_claims(unsigned index, uint32_t word);
bool cdj_c674x_arm_table_predicates_are_word_only(uint32_t word);
/* The semantic family the core routes one instruction to ("ldw" is
 * "scalar_memory", compact forms are "compact-<form>", rejections name the
 * rejection), from the decode it executes with.  For decoder cross-checks
 * against an independent disassembler; returns a static string.  lowered,
 * when given, receives the word the issue loop executes: a compact
 * instruction rewritten to its 32-bit equivalent, else the word itself. */
const char *cdj_c674x_describe(const CdjC674xInstruction *insn,
                               uint32_t *lowered);
#endif
