/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_MPY_H
#define CDJ_C674X_MPY_H
#include <stdbool.h>
#include <stdint.h>
/* Saturating 16x16 .M-unit multiply semantics, SPRUFE8B (July 2010):
 * SMPY printed page 461, SMPYH 463, SMPYHL 464, SMPYLH 466, SMPY2 468.
 * Pure value-in/value-out functions: no CdjC674x access, so the execute
 * loop's transactional copy is never reachable from here. */
typedef struct {
    uint32_t value;
    bool saturated;
} CdjC674xMpyResult;
/* One signed 16-bit halfword of each source is multiplied, the product is
 * shifted left by one bit, and a left-shifted result of 8000 0000h is
 * replaced by 7FFF FFFFh with saturated set.  src1_high/src2_high select the
 * upper halfword (msb16) instead of the lower one (lsb16). */
CdjC674xMpyResult cdj_c674x_smpy16(uint32_t src1, uint32_t src2,
                                   bool src1_high, bool src2_high);
#endif
