/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_APPROX_H
#define CDJ_C674X_APPROX_H
#include <stdbool.h>
#include <stdint.h>
/* The four .S-unit reciprocal APPROXIMATION instructions, SPRUFE8B (July 2010):
 * RCPSP 411-412, RCPDP 409-410, RSQRSP 420-421, RSQRDP 418-419.
 *
 * THESE ARE THE ONE FAMILY IN THIS CORE WHOSE RESULT IS DELIBERATELY NOT
 * BIT-EXACT, AND THAT IS WHAT THE MANUAL SPECIFIES.  Every entry says the same
 * thing of the normal-number case: the instruction "provides the correct
 * exponent, and the mantissa is accurate to the eighth binary position
 * (therefore, mantissa error is less than 2-8)".  A TOLERANCE, not a value.
 * The pages then frame the result as a Newton-Raphson seed - "x[0], the seed
 * value for the algorithm, is given by RCPDP.  For each iteration, the accuracy
 * doubles.  Thus, with one iteration, accuracy is 16 bits in the mantissa; with
 * the second iteration, the accuracy is 32 bits; with the third iteration, the
 * accuracy is the full 52 bits" - and never give the delivered bits, a
 * polynomial, or a table.  Two conforming implementations can therefore differ
 * below the eighth mantissa bit.
 *
 * WHAT THIS IMPLEMENTATION GUARANTEES.  The exact special cases the notes
 * enumerate (NaN, denormal, signed zero, signed infinity, negative square-root
 * source, reciprocal underflow), bit for bit with their status flags; the
 * correct exponent; and a mantissa within the stated 2^-8.  It reproduces each
 * entry's worked example exactly, because those examples use inputs whose
 * result is exactly representable.
 *
 * WHAT IT DOES NOT GUARANTEE.  That the mantissa bits below the eighth position
 * match silicon.  Firmware that REFINES the seed (the documented use) converges
 * to the same answer regardless, since a Newton-Raphson iteration's fixed point
 * does not depend on the seed.  Firmware that consumes the seed DIRECTLY,
 * without refinement, may diverge from hardware in those low bits.  That is the
 * one real consequence and it is declared in the replay and board manifests
 * rather than left for a reader to discover.
 *
 * Pure value-in/value-out: nothing here touches CdjC674x. */
typedef enum {
    CDJ_C674X_RCPSP = 0,
    CDJ_C674X_RCPDP,
    CDJ_C674X_RSQRSP,
    CDJ_C674X_RSQRDP,
} CdjC674xApproxKind;

typedef struct {
    /* Single-precision results occupy the low 32 bits. */
    uint64_t value;
    /* Warning mask to OR into FAUCR, already in the low (.1 unit) bit
     * positions; the caller shifts by 16 for a .2 unit.  Bit numbering is
     * Table 2-26's (printed page 61): NAN1 0, NAN2 1, DEN1 2, DEN2 3, INVAL 4,
     * INFO 5, OVER 6, INEX 7, UND 8, UNORD 9, DIV0 10. */
    uint32_t status;
    /* True for the two double-precision forms, whose src2 and dst are register
     * pairs.  Reported from the kind alone, so a caller can learn the shape
     * before it has operands. */
    bool pair;
    bool valid;
} CdjC674xApproxResult;

CdjC674xApproxResult cdj_c674x_approx(CdjC674xApproxKind kind, uint64_t src2);
#endif
