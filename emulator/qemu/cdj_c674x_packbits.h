/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_PACKBITS_H
#define CDJ_C674X_PACKBITS_H
#include <stdbool.h>
#include <stdint.h>
/* Pack/unpack, shuffle, bit-manipulation and merge-byte semantics, SPRUFE8B
 * (July 2010).  Every rule below is cited to the printed page of the
 * instruction's own entry.  These are pure value-in/value-out functions: they
 * never see CdjC674x, so the execute loop's transactional copy is unreachable
 * from here.  Delay slots are NOT modelled here - they belong to the arm in
 * cdj_c674x.c, which is where the pipeline lives.
 *
 * The three functions that produce a register pair return dst_o in bits 63-32
 * and dst_e in bits 31-0, which is the order their syntax writes (dst_o:dst_e).
 */

/* UNPKHU4 (printed page 559): ubyte3(src2) -> ubyte2(dst), ubyte2(src2) ->
 * ubyte0(dst), zero into ubyte3 and ubyte1. */
uint32_t cdj_c674x_unpkhu4(uint32_t src2);
/* UNPKLU4 (printed page 561): ubyte1(src2) -> ubyte2(dst), ubyte0(src2) ->
 * ubyte0(dst), zero into ubyte3 and ubyte1. */
uint32_t cdj_c674x_unpklu4(uint32_t src2);
/* SWAP4 (printed page 555): exchange the byte pair inside each halfword. */
uint32_t cdj_c674x_swap4(uint32_t src2);
/* BITR (printed page 163): bit 0 of src2 becomes bit 31 of dst, and so on. */
uint32_t cdj_c674x_bitr(uint32_t src2);
/* BITC4 (printed page 161): per packed byte, the count of 1 bits. */
uint32_t cdj_c674x_bitc4(uint32_t src2);
/* DEAL (printed page 231): odd bits of src2 to dst 31-16, even bits to 15-0. */
uint32_t cdj_c674x_deal(uint32_t src2);
/* SHFL (printed page 443): the exact inverse of DEAL - lower halfword of src2
 * into the even bit positions, upper halfword into the odd ones. */
uint32_t cdj_c674x_shfl(uint32_t src2);
/* SHFL3 (printed page 445): 3-way bit interleave of three 16-bit values into a
 * 48-bit result.  Transcribed from the entry's own pseudocode: inp0 is
 * src2 & FFFFh, inp1 src1 & FFFFh and inp2 src1 >> 16, contributing to result
 * bits I*3, I*3+1 and I*3+2.  Bits 47-32 land in dst_o, whose bits 31-16 are
 * therefore always zero. */
uint64_t cdj_c674x_shfl3(uint32_t src1, uint32_t src2);
/* XPND2 (printed page 568): bit 0 of src2 replicated over lsb16(dst), bit 1
 * over msb16(dst).  Bits 31-2 of src2 are ignored. */
uint32_t cdj_c674x_xpnd2(uint32_t src2);
/* XPND4 (printed page 570): bits 3-0 of src2 replicated over the four bytes of
 * dst, bit 0 to the least-significant byte.  Bits 31-4 are ignored. */
uint32_t cdj_c674x_xpnd4(uint32_t src2);
/* ROTL (printed page 414): rotate src2 left by the five least-significant bits
 * of src1; bits 31-5 of src1 "are ignored and may be non-zero".  A rotate of
 * zero returns src2 unchanged - that is the Description's plain reading, and
 * the entry's own "(src2 << src1) | (src2 >> (32 - src1))" has no defined value
 * there. */
uint32_t cdj_c674x_rotl(uint32_t src2, uint32_t src1);
/* LMBD (printed page 304): bit 0 of src1 selects the bit value searched for;
 * the result is the number of bits to the left of the leftmost occurrence, or
 * 32 when src2 contains no such bit. */
uint32_t cdj_c674x_lmbd(uint32_t src1, uint32_t src2);
/* NORM (printed page 390), 32-bit src2: the number of redundant sign bits. */
uint32_t cdj_c674x_norm32(uint32_t src2);
/* NORM (printed page 390), 40-bit slong src2: same, over bits 39-0.  Bits
 * 63-40 of the argument are ignored. */
uint32_t cdj_c674x_norm40(uint64_t src2);
/* SHLMB (printed page 449): src2 shifted left one byte, with ubyte3(src1)
 * merged into ubyte0. */
uint32_t cdj_c674x_shlmb(uint32_t src1, uint32_t src2);
/* SHRMB (printed page 455): src2 shifted right one byte, with ubyte0(src1)
 * merged into ubyte3. */
uint32_t cdj_c674x_shrmb(uint32_t src1, uint32_t src2);
/* DPACK2 (printed page 254): PACK2 into dst_e and PACKH2 into dst_o. */
uint64_t cdj_c674x_dpack2(uint32_t src1, uint32_t src2);
/* DPACKX2 (printed page 256): two PACKLH2 operations. */
uint64_t cdj_c674x_dpackx2(uint32_t src1, uint32_t src2);

/* ADDSUB (printed page 132), ADDSUB2 (133), SADDSUB (427) and SADDSUB2 (429):
 * the nonconditional .L dual-result forms, all "Single-cycle / Delay Slots 0",
 * all writing dst_o:dst_e in E1.  In every one the ADD goes to dst_o and the
 * SUB to dst_e.
 *
 *   opfield  instruction  saturates  packed
 *   0001100  ADDSUB       no         no
 *   0001101  ADDSUB2      no         2x16
 *   0001110  SADDSUB      yes        no
 *   0001111  SADDSUB2     yes        2x16
 *
 * SADDSUB and SADDSUB2 differ in more than width.  SADDSUB states positively
 * that "If either result saturates, the L1 or L2 bit in SSR and the SAT bit in
 * CSR are written one cycle after the results are written to dst_o:dst_e",
 * while SADDSUB2 carries the packed exemption - "This operation is performed on
 * each halfword separately.  This instruction does not affect the SAT bit in
 * CSR or the L1 or L2 bits in SSR" - so the narrow form reports saturation and
 * the packed one deliberately does not.  `saturated` is therefore true only for
 * SADDSUB. */
#define CDJ_C674X_ADDSUB    0x0cu
#define CDJ_C674X_ADDSUB2   0x0du
#define CDJ_C674X_SADDSUB   0x0eu
#define CDJ_C674X_SADDSUB2  0x0fu
typedef struct {
    uint64_t value;    /* dst_o in the high word, dst_e in the low */
    bool saturated;    /* SADDSUB only; always false for SADDSUB2 */
    bool valid;
} CdjC674xAddsubResult;
CdjC674xAddsubResult cdj_c674x_addsub(unsigned opfield, uint32_t src1,
                                      uint32_t src2);
#endif
