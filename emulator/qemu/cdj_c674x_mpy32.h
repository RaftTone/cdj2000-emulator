/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_MPY32_H
#define CDJ_C674X_MPY32_H
#include <stdbool.h>
#include <stdint.h>
/* 32-bit multiply, Galois-field multiply and the 40-bit long .L/.S forms,
 * SPRUFE8B (July 2010).  Pure value-in/value-out semantics: nothing here
 * includes or touches CdjC674x, so the execute loop's transactional copy is
 * unreachable from this file.  The decoder, the delay-slot queueing and the
 * delayed CSR.SAT/SSR effects stay in cdj_c674x.c.
 *
 * A 40-bit "long" is carried as a uint64_t whose bits 39-0 are the
 * architectural value; bits 63-40 are ignored on input and zero on output,
 * matching the manual's "the upper 24 bits of the register pair are unused".
 */

/* 40-bit mask and the signed interpretation of one. */
#define CDJ_C674X_LONG40_MASK UINT64_C(0xffffffffff)
int64_t cdj_c674x_sx40(uint64_t raw);

/* MPYI (printed page 334) and MPYID (335): "The src1 operand is multiplied by
 * the src2 operand."  Both operands are sint/xsint.  MPYI keeps lsb32 of this
 * product, MPYID the whole 64 bits as dst_h:dst_l. */
uint64_t cdj_c674x_mpyi(uint32_t src1, uint32_t src2);

/* MPY2 (printed pages 365-366): lsb16(src1) x lsb16(src2) -> dst_e and
 * msb16(src1) x msb16(src2) -> dst_o, both signed.  Returned as
 * (dst_o << 32) | dst_e. */
uint64_t cdj_c674x_mpy2(uint32_t src1, uint32_t src2);

/* GMPY4 (printed pages 272-274): four independent Galois-field multiplies of
 * the unsigned bytes of src1 and src2 over GF(2^(size+1)) with generator
 * polynomial x^(size+1) + poly.  size and poly are the GFPGFR SIZE and POLY
 * fields (printed page 40); their reset values are 7 and 1Dh. */
uint32_t cdj_c674x_gmpy4(uint32_t src1, uint32_t src2, unsigned poly,
                         unsigned size);

/* GMPY/XORMPY (printed pages 270 and 566): nine low bits of src2 multiply
 * src1 over GF(2), reducing each 32-bit left shift with the side's GPLY
 * polynomial.  XORMPY passes zero for poly. */
uint32_t cdj_c674x_gmpy_word(uint32_t src1, uint32_t src2, uint32_t poly);

/* SAT (printed page 437): a signed 40-bit src2 clamped to 32 bits.
 * *saturated reports whether the clamp fired, which the caller turns into the
 * delayed CSR.SAT / SSR write one cycle after dst. */
uint32_t cdj_c674x_sat40(uint64_t src2, bool *saturated);

/* SUBC (printed page 539): unsigned conditional subtract and shift. */
uint32_t cdj_c674x_subc(uint32_t src1, uint32_t src2);

/* ABS (printed page 101), both operand widths.  -2^31 saturates to 2^31 - 1
 * and -2^39 to 2^39 - 1; neither form touches CSR.SAT. */
uint32_t cdj_c674x_abs32(uint32_t src2);
uint64_t cdj_c674x_abs40(uint64_t src2);

/* SHL / SHR / SHRU 40-bit forms (printed pages 447, 451, 457).  operation 0 is
 * SHL, 1 is SHR (sign-extending) and 2 is SHRU (zero-extending).  Counts above
 * 39 shift by 40, as those pages require. */
#define CDJ_C674X_SHIFT40_LEFT 0u
#define CDJ_C674X_SHIFT40_ARITHMETIC 1u
#define CDJ_C674X_SHIFT40_LOGICAL 2u
uint64_t cdj_c674x_shift40(uint64_t src2, unsigned count, unsigned operation);
#endif
