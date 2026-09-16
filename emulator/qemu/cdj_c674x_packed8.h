/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_PACKED8_H
#define CDJ_C674X_PACKED8_H
#include <stdint.h>
/* Packed 8-bit (4x8) operand semantics, SPRUFE8B (July 2010):
 * ADD4 printed pages 140-141, SUB4 551-552, SUBABS4 534-535, SADDU4 435-436,
 * AVGU4 149-150, MAXU4 309-310, MINU4 314-315, CMPEQ4 181-183,
 * CMPGTU4 199-201, MPYU4 360-361, MPYSU4 357-358, SPACKU4 474-476.
 *
 * Pure value-in/value-out functions: no CdjC674x access, so the execute
 * loop's transactional copy is never reachable from here.  Pipeline latency,
 * unit restrictions and the register-pair rules stay in cdj_c674x.c.
 *
 * Two mnemonics of this family are documented pseudo-operations with no
 * encoding of their own and therefore no function here:
 *   CMPLTU4 (printed page 213) - "The assembler uses the operation CMPGTU4
 *   (.unit) src1, src2, dst to perform this task", so it is cdj_c674x_cmpgtu4
 *   with the operands exchanged.
 *   MPYUS4 (printed page 363) - "The assembler uses the MPYSU4 (.unit) src1,
 *   src2, dst instruction to perform this operation", likewise
 *   cdj_c674x_mpysu4 with the operands exchanged.
 */

/* ADD4/SUB4: 2s-complement per byte, "No saturation is performed. The carry
 * from one 8-bit add does not affect the add of any other 8-bit add"
 * (printed pages 140, 551). */
uint32_t cdj_c674x_add4(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_sub4(uint32_t src1, uint32_t src2);
/* SUBABS4: bytes are unsigned; dst byte is abs(src1 byte - src2 byte)
 * (printed page 534). */
uint32_t cdj_c674x_subabs4(uint32_t src1, uint32_t src2);
/* SADDU4: unsigned bytes, each sum above 2^8 - 1 clamped to 2^8 - 1.
 * "This instruction does not affect the SAT bit in CSR" (printed page 435). */
uint32_t cdj_c674x_saddu4(uint32_t src1, uint32_t src2);
/* AVGU4: (ua + ub + 1) >> 1 per unsigned byte; "No overflow conditions
 * exist" (printed page 149). */
uint32_t cdj_c674x_avgu4(uint32_t src1, uint32_t src2);
/* MAXU4/MINU4: per-byte unsigned maximum/minimum (printed pages 309, 314). */
uint32_t cdj_c674x_maxu4(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_minu4(uint32_t src1, uint32_t src2);
/* CMPEQ4/CMPGTU4: byte N's verdict lands in bit N of dst and "The remaining
 * bits of dst are cleared to 0" (printed pages 181, 199). */
uint32_t cdj_c674x_cmpeq4(uint32_t src1, uint32_t src2);
uint32_t cdj_c674x_cmpgtu4(uint32_t src1, uint32_t src2);
/* MPYU4/MPYSU4: four 8x8 products as four 16-bit fields of dst_o:dst_e,
 * returned with dst_e in the low 32 bits (printed pages 360, 357). */
uint64_t cdj_c674x_mpyu4(uint32_t src1, uint32_t src2);
uint64_t cdj_c674x_mpysu4(uint32_t src1, uint32_t src2);
/* SPACKU4: four signed 16-bit halfwords clamped to 0..255 and packed, src1
 * supplying the two most-significant bytes (printed page 474). */
uint32_t cdj_c674x_spacku4(uint32_t src1, uint32_t src2);
#endif
