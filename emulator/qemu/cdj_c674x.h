/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_H
#define CDJ_C674X_H
#include <stdbool.h>
#include <stdint.h>
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
bool cdj_c674x_execute(CdjC674x *, const CdjC674xPacket *, CdjC674xRead,
                      CdjC674xWrite, void *);
void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry);
/* Present already-selected CPU interrupt requests at an execute-packet
 * boundary. Bits 4..15 correspond to INT4..INT15; all other bits are
 * rejected. Requests latch in IFR even while masked. A recognized interrupt
 * redirects the next fetch to its IST entry without advancing CPU time. */
bool cdj_c674x_interrupt(CdjC674x *cpu, uint32_t pending);
bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque);
/* Optional direct source packet observation, before loop-setup transformations.
 * Consume only when the step succeeds. count=0 for loop/idle steps; this does
 * not observe loop-buffer source fetches. No extra bus reads or CPU effects. */
bool cdj_c674x_step_capture_direct(CdjC674x *, CdjC674xRead, CdjC674xWrite,
                                  void *, CdjC674xPacket *);

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
#endif
