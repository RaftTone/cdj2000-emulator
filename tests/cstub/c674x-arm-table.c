/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shadow check for the C674x conditional-instruction dispatch table.
 *
 * cdj_c674x.c selects an arm by scanning cdj_c674x_arms[] first-match-wins, so
 * a row whose mask/match overlaps an earlier row's never runs: it is silently
 * shadowed, the build says nothing, and the instruction it was added for keeps
 * reporting whatever the earlier row does.  The comment above the table warns
 * about exactly this and asks for a sweep before adding a row.  A sweep is not
 * needed - overlap has a closed form.
 *
 * Two rows can both match some word w iff they agree on every bit both of them
 * constrain:
 *
 *     ((match_i ^ match_j) & mask_i & mask_j) == 0
 *
 * If that holds and NEITHER row carries an `also` predicate, the later row is
 * unreachable for every word the pair shares - a hard shadow, reported as a
 * failure.  If either carries `also`, the overlap is the documented way two
 * rows are disambiguated (`also` is the rest of the ladder's condition), so it
 * is reported for visibility and not failed.
 *
 * A word claimed by a pair is printed so the offender is identifiable rather
 * than merely counted.  Output, in order:
 *
 *     rows <n>
 *     shadow <i> <j> mask=<hex> match=<hex> / mask=<hex> match=<hex> word=<hex>
 *     predicated <i> <j> ...
 *     hard-shadows <n>
 *     predicated-overlaps <n>
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cdj_c674x.h"

/* Any word both rows match: take the bits each row constrains from its own
 * match, which agree wherever both constrain, and leave the rest zero. */
static uint32_t shared_word(uint32_t m1, uint32_t x1, uint32_t m2, uint32_t x2)
{
    return (x1 & m1) | (x2 & m2);
}

int main(void)
{
    unsigned rows = cdj_c674x_arm_table_rows();
    unsigned hard = 0, predicated = 0;
    printf("rows %u\n", rows);
    for (unsigned i = 0; i < rows; ++i) {
        uint32_t mi, xi; bool ai;
        if (!cdj_c674x_arm_table_row(i, &mi, &xi, &ai)) {
            printf("row-query-failed %u\n", i);
            return 1;
        }
        for (unsigned j = i + 1; j < rows; ++j) {
            uint32_t mj, xj; bool aj;
            if (!cdj_c674x_arm_table_row(j, &mj, &xj, &aj)) {
                printf("row-query-failed %u\n", j);
                return 1;
            }
            if ((xi ^ xj) & mi & mj) continue;   /* cannot both match */
            uint32_t w = shared_word(mi, xi, mj, xj);
            /* cdj_c674x_arm_lookup's format rule resolves one whole class of
             * mask/match overlap without a predicate: a row whose mask leaves
             * bits 31-28 free is never selected for a word carrying the
             * nonconditional 0001 opcode field there.  So if either row pins
             * those bits to 0001, every word the pair shares is one the other
             * row is ineligible for unless it pins them too - the pair cannot
             * collide, and reporting it would be a false alarm that grows with
             * every nonconditional row added.  Mirrors
             * cdj_c674x_arm_table_row_claims, which applies the same rule. */
            bool i_nonconditional = (mi >> 28) == 0xfu && (xi >> 28) == 1u;
            bool j_nonconditional = (mj >> 28) == 0xfu && (xj >> 28) == 1u;
            if ((i_nonconditional && (mj >> 28) != 0xfu) ||
                (j_nonconditional && (mi >> 28) != 0xfu))
                continue;
            if (!ai && !aj) {
                ++hard;
                printf("shadow %u %u mask=%08" PRIx32 " match=%08" PRIx32
                       " / mask=%08" PRIx32 " match=%08" PRIx32
                       " word=%08" PRIx32 "\n", i, j, mi, xi, mj, xj, w);
            } else {
                ++predicated;
                printf("predicated %u %u mask=%08" PRIx32 " match=%08" PRIx32
                       " / mask=%08" PRIx32 " match=%08" PRIx32
                       " word=%08" PRIx32 "\n", i, j, mi, xi, mj, xj, w);
            }
        }
    }
    /* Out-of-range indices must report failure rather than read past the end,
     * or a miscounted loop above would silently check nothing. */
    if (cdj_c674x_arm_table_row(rows, NULL, NULL, NULL)) {
        printf("out-of-range-row-accepted\n");
        return 1;
    }
    printf("hard-shadows %u\n", hard);
    printf("predicated-overlaps %u\n", predicated);
    return 0;
}
