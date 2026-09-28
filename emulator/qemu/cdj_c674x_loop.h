/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_LOOP_H
#define CDJ_C674X_LOOP_H
#include <stdbool.h>
#include <stdint.h>
/* SPLOOP/SPLOOPD/SPLOOPW scheduling model, TI SPRUFE8B chapter 7.
 * Tags identify decoded instructions; the CPU owns their contents. SPLOOP's
 * own packet is outside this timeline. The caller handles SPMASK through the
 * candidate filter. SPLOOPD's initial four-cycle count delay and SPLOOPW's
 * predicate history are caller-owned. The caller supplies each program-memory packet at its original
 * cycle while loading; NOP cycles have no tags but still advance time. */
typedef struct {
    uint32_t tags[48][8];
    unsigned count[48], ii, length;
    uint32_t iterations;
    uint64_t cycle, post_cycle, end_cycle;
    bool sealed, predicate_loop, delayed_count;
} CdjC674xLoop;
/* Process-wide development mode for the single emulated DSP.  Strict timing
 * remains the default.  Functional mode delays SPLOOPD post-loop fetch by two
 * cycles so later firmware hardware dependencies can be inventoried while the
 * unresolved epilog timing is investigated separately. */
void cdj_c674x_loop_set_functional_timing(bool enabled);
bool cdj_c674x_loop_functional_timing(void);
bool cdj_c674x_loop_init(CdjC674xLoop *, unsigned ii, uint32_t iterations);
/* Add the current cycle's instructions; finish marks SPKERNEL. delay is the
 * decoded fstg*ii+fcyc. Control marker instructions are not included as tags. */
bool cdj_c674x_loop_load(CdjC674xLoop *, const uint32_t *, unsigned count,
                       bool finish, unsigned delay);
/* Return this cycle's simultaneous instructions and post-loop fetch permission.
 * No mutation on failure (e.g. more than eight simultaneous instructions). */
bool cdj_c674x_loop_issue(CdjC674xLoop *, uint32_t tags[8], unsigned *count,
                        bool *post_fetch, bool *drained);
/* Filter candidates before the eight-operation capacity check. SPMASK must
 * suppress buffered operations even when the unmasked issue would overflow. */
bool cdj_c674x_loop_issue_filtered(CdjC674xLoop *, uint32_t tags[8], unsigned *count,
                                 bool *post_fetch, bool *drained,
                                 bool (*allow)(void *, uint32_t), void *opaque);
/* Immediate SPKERNELR reload uses the same source buffer for a current
 * invocation and the preceding invocation's draining epilog. All cycle
 * arguments are relative to the first cycle after the SPLOOP packet. */
bool cdj_c674x_loop_issue_reload(CdjC674xLoop *, uint64_t current_start,
                                 uint64_t old_start, uint64_t old_end,
                                 uint64_t post_end, uint32_t tags[8],
                                 unsigned *count, bool *post_fetch,
                                 bool *drained,
                                 bool (*allow)(void *, uint32_t), void *opaque);
/* At a legal stage boundary, stop launching iterations and convert the
 * existing SPLOOP/SPLOOPD/SPLOOPW schedule into its interrupt epilog. The
 * caller is responsible for the architectural eligibility checks and for
 * vectoring after drain. */
bool cdj_c674x_loop_interrupt_drain(CdjC674xLoop *);
#endif
