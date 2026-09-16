/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Saturating 16x16 .M multiplies, SPRUFE8B July 2010.  Semantics only; the
 * decoder and the delayed CSR.SAT/SSR queueing stay in cdj_c674x.c.
 *
 * SMPY   printed page 461: "The 16 least-significant bits of src1 operand is
 *        multiplied by the 16 least-significant bits of the src2 operand. The
 *        result is left shifted by 1 and placed in dst. If the left-shifted
 *        result is 8000 0000h, then the result is saturated to 7FFF FFFFh."
 * SMPYH  463 (msb16 x msb16), SMPYHL 464 (msb16 x lsb16),
 * SMPYLH 466 (lsb16 x msb16) repeat that rule verbatim for their halfwords.
 * SMPY2  468 applies sat((x << 1)) independently to the lsb16 and msb16
 *        products, so it is two calls of the scalar rule below.
 *
 * The only operand pair that can saturate is 8000h x 8000h: with signed
 * 16-bit halfwords the product magnitude never exceeds 2^30, it reaches
 * +2^30 only for (-32768) x (-32768), and -2^30 is unreachable because it
 * would need |src1| = |src2| = 32768 with opposite signs, which no pair of
 * int16 values has.  The test below is still the manual's test - the value of
 * the left-shifted result - rather than that derivation.
 */
#include "cdj_c674x_mpy.h"

CdjC674xMpyResult cdj_c674x_smpy16(uint32_t src1, uint32_t src2,
                                   bool src1_high, bool src2_high)
{
    int32_t left = (int16_t)(src1_high ? src1 >> 16 : src1);
    int32_t right = (int16_t)(src2_high ? src2 >> 16 : src2);
    /* |left * right| <= 2^30, so the product itself cannot overflow int32_t;
     * shift as unsigned so the documented 8000 0000h case is well defined. */
    uint32_t shifted = (uint32_t)(left * right) << 1;
    bool saturated = shifted == 0x80000000u;

    return (CdjC674xMpyResult){saturated ? 0x7fffffffu : shifted, saturated};
}
