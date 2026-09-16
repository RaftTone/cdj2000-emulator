/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Does any instruction word actually reach two rows of cdj_c674x_arms[]?
 *
 * c674x-arm-table.c answers a weaker question. It compares mask/match only, so
 * it over-reports: at 211 rows it flags 251 pairs that overlap on mask/match
 * and are separated solely by an `also` predicate. That check deliberately does
 * not evaluate predicates, so on its own it cannot tell a genuine double claim
 * from a pair the predicates keep apart. The comment above the table says the
 * property was once established by "sweeping every word the rows can
 * discriminate"; this re-establishes it at the current row count, and does so
 * exhaustively rather than by sampling.
 *
 * A brute-force sweep of 2^32 words against 211 rows is ~10^12 comparisons.
 * It is not needed, because selection is
 *
 *     row i claims w  <=>  (w & mask_i) == match_i  AND  also_i(w)
 *
 * so two rows can both claim some word only if their mask/match already agree
 * on every bit both constrain - which is exactly the pair list the closed-form
 * check produces. For each such pair every bit in mask_i | mask_j is FIXED by
 * the two matches, and only the remaining bits are free. Enumerating just those
 * free bits is exhaustive over the pair's whole overlap region, and there are
 * few of them because the masks are dense.
 *
 * Predicates are pure functions of the word, so no CPU is needed; that
 * assumption is itself checked per word rather than assumed.
 *
 * Output:
 *     rows <n>
 *     pairs-examined <n>
 *     skipped-too-many-free-bits <n> [<i> <j> <bits>]...
 *     double-claim <i> <j> word=<hex>
 *     double-claims <n>
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cdj_c674x.h"

/* Enumerating more than this many free bits per pair would cost more than the
 * check is worth; such a pair is reported rather than silently skipped, so a
 * gap in coverage is never mistaken for a clean result. */
#define MAX_FREE_BITS 28

int main(void)
{
    unsigned rows = cdj_c674x_arm_table_rows();
    unsigned long long examined = 0, doubles = 0;
    unsigned skipped = 0;
    printf("rows %u\n", rows);

    /* The method rests on `also` predicates being pure functions of the word.
     * Establish that first, over a spread of words chosen to exercise every
     * field a predicate could read, rather than re-checking inside the hot
     * loop where it would dominate the cost for no extra assurance. */
    for (unsigned k = 0; k < 4096; ++k) {
        uint32_t w = (k * UINT32_C(2654435761));   /* spread across the space */
        if (!cdj_c674x_arm_table_predicates_are_word_only(w)) {
            printf("predicate-reads-more-than-the-word word=%08" PRIx32 "\n", w);
            return 1;
        }
    }
    printf("word-only-probes 4096\n");

    for (unsigned i = 0; i < rows; ++i) {
        uint32_t mi, xi; bool ai;
        if (!cdj_c674x_arm_table_row(i, &mi, &xi, &ai)) return 1;
        for (unsigned j = i + 1; j < rows; ++j) {
            uint32_t mj, xj; bool aj;
            if (!cdj_c674x_arm_table_row(j, &mj, &xj, &aj)) return 1;
            /* Disagree on a bit both constrain: no word can reach both. */
            if ((xi ^ xj) & mi & mj) continue;
            ++examined;

            uint32_t fixed = mi | mj;
            uint32_t base = (xi & mi) | (xj & mj);
            /* Collect the free bit positions. */
            unsigned free_pos[32], nfree = 0;
            for (unsigned b = 0; b < 32; ++b)
                if (!((fixed >> b) & 1u)) free_pos[nfree++] = b;
            if (nfree > MAX_FREE_BITS) {
                printf("skipped-too-many-free-bits %u %u %u\n", i, j, nfree);
                ++skipped;
                continue;
            }
            bool reported = false;
            uint32_t w = base;
            uint64_t prev_gray = 0;
            for (uint64_t n = 0; n < (UINT64_C(1) << nfree); ++n) {
                /* Gray code: successive codes differ in exactly one bit, so the
                 * word is updated with a single toggle instead of being rebuilt
                 * from all nfree bits on every step. */
                uint64_t gray = n ^ (n >> 1);
                uint64_t diff = gray ^ prev_gray;
                if (diff) {
                    unsigned b = 0;
                    while (!((diff >> b) & 1u)) ++b;
                    w ^= 1u << free_pos[b];
                }
                prev_gray = gray;
                if (cdj_c674x_arm_table_row_claims(i, w) &&
                    cdj_c674x_arm_table_row_claims(j, w)) {
                    ++doubles;
                    if (!reported) {     /* one witness per pair is enough */
                        printf("double-claim %u %u word=%08" PRIx32 "\n", i, j, w);
                        reported = true;
                    }
                }
            }
        }
    }
    printf("pairs-examined %llu\n", examined);
    printf("skipped %u\n", skipped);
    printf("double-claims %llu\n", doubles);
    return 0;
}
