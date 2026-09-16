/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_MULTICYCLE_H
#define CDJ_C674X_MULTICYCLE_H
#include <stdbool.h>

/* Execute-packet multicycle-NOP accounting, taken from SPRUFE8B:
 *
 *  - 3.8.10, printed page 82: "Two instructions that generate multicycle NOPs
 *    cannot share the same execute packet.  Instructions that generate a
 *    multicycle NOP are: NOP n (where n > 1); IDLE; BNOP target, n (for all
 *    values of n, regardless of predication); ADDKPC label, reg, n (for all
 *    values of n, regardless of predication)."
 *  - 3.8.11.5, printed page 83: "A NOP n (with n > 1) instruction cannot be
 *    placed in parallel with other multicycle NOP counts (ADDKPC, BNOP,
 *    CALLP) with the exception of another NOP n where the NOP count is the
 *    same."
 *  - 3.8.11.4, printed page 83: an IDLE "can be placed in parallel with the
 *    NOP instruction", and with nothing else that generates a multicycle NOP.
 *  - 3.10.2.2, printed page 93: "When PROT is 1, four cycles of NOP are added
 *    after each LD instruction within the fetch packet whether the LD is in
 *    16-bit compact format or 32-bit format."  The SPKERNEL restrictions on
 *    printed page 481 count protected loads among the instructions that
 *    initiate multicycle NOPs.
 *  - IDLE, printed page 274: "Performs an infinite multicycle NOP that
 *    terminates upon servicing an interrupt, or a branch occurs due to an IDLE
 *    instruction being in the delay slots of a branch."  Delay slots: 0.
 *
 * Pure: counts in, packet duration and an accept/reject decision out.  No
 * CdjC674x access, so these are safe to call against a transactional copy.
 * Header-only because each function is a few lines and the repository lists
 * translation units explicitly in a dozen build recipes.
 */
typedef struct {
    unsigned cycles;  /* Execute-packet duration in cycles, at least one. */
    unsigned nop;     /* NOP n count already accepted, zero when none. */
    bool idle;        /* An IDLE occupies this packet. */
    bool protect;     /* PROT already added its four cycles to this packet. */
} CdjC674xPacketTiming;

#define CDJ_C674X_PACKET_TIMING_INIT {1, 0, false, false}

/* SPRUFE8B 3.8.10, printed page 82: a second multicycle-NOP generator may not
 * share the execute packet.  A packet longer than one cycle, or one holding an
 * IDLE, already has one. */
static inline bool cdj_c674x_packet_generator(const CdjC674xPacketTiming *p)
{
    return p->cycles > 1 || p->idle;
}

/* NOP count, 1..9 after the encoding's count - 1 translation. */
static inline bool cdj_c674x_packet_nop(CdjC674xPacketTiming *p, unsigned count)
{
    if (count > 1) {
        /* SPRUFE8B 3.8.11.5, printed page 83: the sole permitted companion is
         * "another NOP n where the NOP count is the same".  A protected load,
         * a BNOP, an ADDKPC or a CALLP leaves nop at zero and so rejects, and
         * 3.8.11.4 forbids sharing with IDLE outright. */
        if (p->idle || (p->cycles > 1 && p->nop != count)) return false;
        p->nop = count;
    }
    if (count > p->cycles) p->cycles = count;
    return true;
}

/* IDLE: issues in the packet's one cycle, then waits without bound. */
static inline bool cdj_c674x_packet_idle(CdjC674xPacketTiming *p)
{
    /* SPRUFE8B 3.8.11.4, printed page 83, and the IDLE entry on printed page
     * 274: zero delay slots, so IDLE issues in the packet's one cycle.  The
     * unbounded wait that follows is the caller's to represent; it is not a
     * cycle count. */
    if (cdj_c674x_packet_generator(p)) return false;
    p->idle = true;
    return true;
}

/* One protected LD.  Called once per load, counted once per packet. */
static inline bool cdj_c674x_packet_protected_load(CdjC674xPacketTiming *p)
{
    /* SPRUFE8B 3.10.2.2, printed page 93, says verbatim: "When PROT is 1, four
     * cycles of NOP are added after each LD instruction within the fetch
     * packet whether the LD is in 16-bit compact format or 32-bit format."
     *
     * That sentence is per-LD, and it does not say what two LDs in the SAME
     * execute packet cost.  Counting once per packet is an INFERENCE, not a
     * quote: parallel loads share the packet's single issue cycle, so their
     * E3/E5 phases coincide and one four-cycle shadow covers both.  A literal
     * per-LD reading would give 4 + 4.  The manual does not settle it, and the
     * audit records it as an open question; what is certain is that rejecting
     * the packet outright - the previous behaviour - is wrong either way,
     * because TI codegen emits dual .D loads in protected fetch packets.
     */
    if (p->protect) return true;
    if (cdj_c674x_packet_generator(p)) return false;
    p->protect = true;
    p->cycles = 5;
    return true;
}

/* BNOP target,n / ADDKPC label,reg,n / CALLP, as a total cycle count. */
static inline bool cdj_c674x_packet_multicycle(CdjC674xPacketTiming *p,
                                               unsigned cycles)
{
    /* 3.8.11.5's equal-count exception is stated only for NOP n, so these
     * admit no companion generator at all. */
    if (cycles > 1 && cdj_c674x_packet_generator(p)) return false;
    if (cycles > p->cycles) p->cycles = cycles;
    return true;
}
#endif
