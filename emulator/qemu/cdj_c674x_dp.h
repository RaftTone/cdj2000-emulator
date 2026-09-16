/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_DP_H
#define CDJ_C674X_DP_H
#include <stdbool.h>
#include <stdint.h>
/* Pure binary64 semantics for the C674x double-precision .L/.M/.S
 * instructions and the DP/SP/integer conversions, SPRUFE8B July 2010.
 * The encodings, the delay slots and the FADCR/FAUCR/FMCR plumbing stay in
 * cdj_c674x.c; nothing here takes or reaches a CdjC674x, so the execute
 * loop's transactional copy is unreachable from this file.
 *
 * Field layout is Figure 3-2 and Table 3-6 (printed page 72): sign bit 63,
 * 11-bit exponent bits 62-52, 52-bit fraction.  Table 3-7 (printed page 72)
 * fixes the constants this file returns verbatim: NaN_out 7FFF FFFF FFFF
 * FFFFh, LFPN 7FEF FFFF FFFF FFFFh, SFPN 0010 0000 0000 0000h.
 *
 * status is the warning mask the caller ORs into FADCR/FAUCR/FMCR, in the
 * low-half (.1 unit) bit positions of Tables 2-25/2-26/2-27 (printed pages
 * 59, 61 and 63); the caller shifts it left by 16 for a .2 unit.  rmode is
 * the two-bit RMODE field: 0 nearest-even, 1 toward zero, 2 toward
 * +infinity, 3 toward -infinity.  value carries a 64-bit DP result, or a
 * 32-bit one in its low half where the instruction's dst is a single
 * register. */
typedef struct { uint64_t value; uint32_t status; } CdjC674xDpResult;

/* ABSDP, printed page 105. */
CdjC674xDpResult cdj_c674x_abs_dp(uint64_t source2);

/* CMPEQDP/CMPGTDP/CMPLTDP, printed pages 184, 193 and 207.  relation 0
 * equal, 1 greater-than, 2 less-than; value is 0 or 1. */
CdjC674xDpResult cdj_c674x_compare_dp(uint64_t source1, uint64_t source2,
                                      unsigned relation);

/* ADDDP/SUBDP, printed pages 125 and 541.  operation 0 add, 1 src1-src2,
 * 2 src2-src1.  The warning bits always describe the encoded src1/src2
 * fields, which is what SUBDP note 2 (printed page 541) requires. */
CdjC674xDpResult cdj_c674x_add_sub_dp(uint64_t source1, uint64_t source2,
                                      unsigned operation, unsigned rmode);

/* MPYDP, printed page 318.  Also the engine for MPYSPDP (printed page 352)
 * and MPYSP2DP (printed page 354) once their single-precision operands have
 * been widened by cdj_c674x_sp_operand_to_dp. */
CdjC674xDpResult cdj_c674x_multiply_dp(uint64_t source1, uint64_t source2,
                                       unsigned rmode);

/* Widen a binary32 multiplier operand to binary64 for MPYSPDP/MPYSP2DP.  A
 * denormal becomes a DP denormal rather than the exactly equal DP normal, so
 * that cdj_c674x_multiply_dp's own "treated as signed 0 and the DENn bit is
 * set" path applies (printed pages 352 and 354, note 4). */
uint64_t cdj_c674x_sp_operand_to_dp(uint32_t source);

/* SPDP, printed page 477. */
CdjC674xDpResult cdj_c674x_sp_to_dp(uint32_t source2);

/* DPSP, printed page 260.  value is the binary32 result in its low 32 bits. */
CdjC674xDpResult cdj_c674x_dp_to_sp(uint64_t source2, unsigned rmode);

/* DPINT (printed page 258) and DPTRUNC (printed page 262, which supplies
 * rmode 1 regardless of FADCR).  value is the 32-bit integer. */
CdjC674xDpResult cdj_c674x_dp_to_integer(uint64_t source2, unsigned rmode);

/* INTDP/INTDPU, printed pages 275 and 276.  Both are exact for every 32-bit
 * input, and both say "You cannot set configuration bits with this
 * instruction", so there is no status to return. */
uint64_t cdj_c674x_integer_to_dp(uint32_t source2, bool signed_source);
#endif
