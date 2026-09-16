/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_PACKED16_H
#define CDJ_C674X_PACKED16_H
#include <stdbool.h>
#include <stdint.h>
/* Packed 16-bit arithmetic, packed compares, packed shifts and the two
 * variable 32-bit shifts, SPRUFE8B (July 2010).  Printed pages: ABS2 103,
 * ADD2 137, SUB2 548, SADD2 425, SSUB2 502, SADDUS2 433 (SADDSU2 431 is a
 * pseudo-operation of it), MAX2 306, MIN2 311, AVG2 147, SHR2 453,
 * SHRU2 459, CMPEQ2 179, CMPGT2 191 (CMPLT2 205 is a pseudo-operation of
 * it), SPACK2 472, SSHVL 495, SSHVR 497.
 *
 * Pure value-in/value-out functions: no CdjC674x access, so the execute
 * loop's transactional copy is never reachable from here.  Every operand is
 * the raw 32-bit register word; the halfword split and the signedness of
 * each half are the instruction's own, per the pages cited above. */

typedef struct {
    uint32_t value;
    bool saturated;
} CdjC674xPacked16Sat;

uint32_t cdj_c674x_abs2(uint32_t src2);
uint32_t cdj_c674x_add2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_sub2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_sadd2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_ssub2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_saddus2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_max2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_min2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_avg2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_shr2(uint32_t src2, uint32_t src1);
uint32_t cdj_c674x_shru2(uint32_t src2, uint32_t src1);
uint32_t cdj_c674x_cmpeq2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_cmpgt2(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_spack2(uint32_t src1, uint32_t src2);
/* SSHVL and SSHVR differ only in the sign convention of src1 (printed pages
 * 496 and 498): pass right = true for SSHVR. */
CdjC674xPacked16Sat cdj_c674x_sshv(uint32_t src2, uint32_t src1, bool right);
#endif
