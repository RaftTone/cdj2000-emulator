/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c674x_loop.h"

static bool functional_timing;

void cdj_c674x_loop_set_functional_timing(bool enabled)
{
    functional_timing = enabled;
}

bool cdj_c674x_loop_functional_timing(void)
{
    return functional_timing;
}

bool cdj_c674x_loop_init(CdjC674xLoop *loop, unsigned ii, uint32_t iterations)
{
    if (!ii || ii > 16) return false;
    memset(loop, 0, sizeof(*loop));
    loop->ii = ii;
    loop->iterations = iterations;
    return true;
}

bool cdj_c674x_loop_load(CdjC674xLoop *loop, const uint32_t *tags,
                        unsigned count, bool finish, unsigned delay)
{
    if (!loop->ii || loop->sealed || loop->cycle >= 48 || count > 8 ||
        (count && !tags) || loop->length > loop->cycle) return false;
    unsigned index = loop->cycle;
    if (count) memcpy(loop->tags[index], tags, count * sizeof(*tags));
    loop->count[index] = count;
    loop->length = index + 1;
    if (finish) {
        loop->sealed = true;
        if (loop->predicate_loop) {
            loop->post_cycle = loop->end_cycle = UINT64_MAX;
            return true;
        }
        /* Post-loop fetching cannot precede the final loading boundary. */
        uint64_t loading_end = ((loop->length + loop->ii - 1) / loop->ii) * loop->ii;
        loop->post_cycle = (uint64_t)loop->iterations * loop->ii + delay;
        /* Functional run-ahead only: the reached II=1 SPLOOPD epilog needs
         * two additional cycles before direct fetch to avoid issuing a live
         * buffered .L2 move beside the following .L2 MVK.  This is a bounded
         * development approximation, not an architectural timing claim. */
        if (functional_timing && loop->delayed_count) loop->post_cycle += 2;
        if (loop->post_cycle < loading_end) loop->post_cycle = loading_end;
        loop->end_cycle = loop->iterations ?
            (uint64_t)(loop->iterations - 1) * loop->ii + loop->length : loading_end;
        if (loop->post_cycle > loop->end_cycle) loop->post_cycle = loop->end_cycle;
    }
    return true;
}

bool cdj_c674x_loop_issue(CdjC674xLoop *loop, uint32_t tags[8], unsigned *count,
                         bool *post_fetch, bool *drained)
{
    return cdj_c674x_loop_issue_filtered(loop, tags, count, post_fetch, drained, NULL, NULL);
}

bool cdj_c674x_loop_issue_filtered(CdjC674xLoop *loop, uint32_t tags[8], unsigned *count,
                                  bool *post_fetch, bool *drained,
                                  bool (*allow)(void *, uint32_t), void *opaque)
{
    uint32_t result[8];
    unsigned n = 0;
    if (!loop->ii || (!loop->sealed && loop->cycle >= 48)) return false;
    /* Only origins congruent to this cycle modulo II can issue. Preserve
     * ascending origin/tag order, but skip all the other schedule entries.
     * Use a wide origin so the increment cannot wrap for a large II. */
    bool finite = !loop->predicate_loop ||
                  (loop->sealed && loop->end_cycle != UINT64_MAX);
    for (uint64_t origin = loop->cycle % loop->ii;
         origin < loop->length && origin <= loop->cycle;
         origin += loop->ii) {
        uint64_t age = loop->cycle - origin;
        /* Predicate loops become finite when interrupt draining begins. */
        if (finite && age / loop->ii >= loop->iterations)
            continue;
        for (unsigned j = 0; j < loop->count[origin]; ++j) {
            uint32_t tag = loop->tags[origin][j];
            if (allow && !allow(opaque, tag)) continue;
            if (n == 8) return false;
            result[n++] = tag;
        }
    }
    if (n) memcpy(tags, result, n * sizeof(*tags));
    *count = n;
    *post_fetch = loop->sealed && loop->cycle >= loop->post_cycle;
    *drained = loop->sealed && loop->cycle >= loop->end_cycle;
    ++loop->cycle;
    return true;
}

bool cdj_c674x_loop_interrupt_drain(CdjC674xLoop *loop)
{
    if (!loop || !loop->sealed || !loop->ii ||
        loop->cycle % loop->ii || !loop->cycle)
        return false;
    uint64_t launched = loop->cycle / loop->ii;
    if (launched > UINT32_MAX) return false;
    uint64_t end = (launched - 1) * loop->ii + loop->length;
    if (end < loop->cycle) return false;
    loop->iterations = launched;
    /* Interrupt draining never fetches the post-SPKERNEL program stream. */
    loop->post_cycle = loop->end_cycle = end;
    return true;
}
