/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_DOTP_H
#define CDJ_C674X_DOTP_H
#include <stdbool.h>
#include <stdint.h>
/* Packed dot products on the .M unit, SPRUFE8B (July 2010).  Every one of
 * these is the Figure E-1 compound .M format - bit 11 is 0, the opfield is
 * bits 10-6 and bits 5-2 are 1100 - and every one is a four-cycle instruction
 * with three delay slots whose dst is written in E4.
 *
 *   opfield  instruction   printed page
 *   00010    DOTPSU4       249    (DOTPUS4, 251, is the same encoding: its
 *                                 syntax lists src2 first and "the assembler
 *                                 uses the DOTPSU4 (.unit) src1, src2, dst
 *                                 instruction to perform this task")
 *   00110    DOTPU4        252
 *   00111    DOTPNRSU2     240    (DOTPNRUS2, 242, same encoding)
 *   01001    DOTPN2        238
 *   01011    DOTP2         235    dst_o:dst_e form, operand type sllong
 *   01100    DOTP2         235    32-bit dst form, operand type int
 *   01101    DOTPRSU2      244    (DOTPRUS2, 247, same encoding)
 *
 * Pure value-in/value-out: nothing here includes or touches CdjC674x, so the
 * execute loop's transactional copy is unreachable from this file. */
typedef enum {
    CDJ_C674X_DOTPSU4   = 0x02,
    CDJ_C674X_DOTPU4    = 0x06,
    CDJ_C674X_DOTPNRSU2 = 0x07,
    CDJ_C674X_DOTPN2    = 0x09,
    CDJ_C674X_DOTP2L    = 0x0b,
    CDJ_C674X_DOTP2     = 0x0c,
    CDJ_C674X_DOTPRSU2  = 0x0d,
} CdjC674xDotpOp;

typedef struct {
    /* The full 64-bit result.  DOTP2's dst_o:dst_e form writes all of it; the
     * 32-bit dst forms write the low word, which printed page 236 says is the
     * same value ("The 32-bit result version returns the same results that the
     * 64-bit result version does in the lower 32 bits"). */
    uint64_t value;
    /* DOTPRSU2 / DOTPNRSU2 only: the intermediate sum left the range in which
     * the manual defines a result.  The caller must fail closed. */
    bool undefined;
    /* opfield is one of CdjC674xDotpOp. */
    bool valid;
} CdjC674xDotpResult;

CdjC674xDotpResult cdj_c674x_dotp(unsigned opfield, uint32_t src1,
                                  uint32_t src2);

/* ---- the NONCONDITIONAL .M group, Figure E-3 (printed page 743) ----------
 *
 * Same unit, different format: bits 31-28 are the literal 0001 opcode field,
 * bit 11 is 0, the opfield is bits 10-6 and bits 5-2 are 1100.  Because bits
 * 31-28 are an opcode and not creg/z, these are unconditional and are reached
 * through CDJ_C674X_UNCOND_ARM_TABLE rather than the predicate path.  All are
 * four-cycle with three delay slots.
 *
 *   opfield  instruction  printed page   src1 shape
 *   01010    CMPY         215            32-bit
 *   01011    CMPYR        217            32-bit
 *   01100    CMPYR1       219            32-bit
 *   10100    DDOTPL2R     229            register pair
 *   10101    DDOTPH2R     225            register pair
 *   10110    DDOTPL2      227            register pair
 *   10111    DDOTPH2      223            register pair
 *   11000    DDOTP4       221            32-bit
 *
 * CMPY and the two DDOTP non-rounding forms write a 64-bit register pair; the
 * rounding forms (CMPYR, CMPYR1, DDOTPH2R, DDOTPL2R) pack two rounded 16-bit
 * halves into a single 32-bit dst. */
typedef enum {
    CDJ_C674X_CMPY     = 0x0a,
    CDJ_C674X_CMPYR    = 0x0b,
    CDJ_C674X_CMPYR1   = 0x0c,
    CDJ_C674X_DDOTPL2R = 0x14,
    CDJ_C674X_DDOTPH2R = 0x15,
    CDJ_C674X_DDOTPL2  = 0x16,
    CDJ_C674X_DDOTPH2  = 0x17,
    CDJ_C674X_DDOTP4   = 0x18,
} CdjC674xCmpyOp;

typedef struct {
    uint64_t value;   /* pair forms use all 64 bits, rounding forms the low 32 */
    bool pair_dst;    /* true when dst is dst_o:dst_e rather than a 32-bit dst */
    bool pair_src1;   /* true when src1 is src1_o:src1_e (the DDOTP*2 forms)  */
    bool saturated;   /* a sat() in the Execution pseudocode actually clamped */
    bool valid;
} CdjC674xCmpyResult;

/* src1_hi is meaningful only when the opfield takes a src1 pair; pass 0
 * otherwise.  Callers must consult pair_src1/pair_dst from a probe (src values
 * are irrelevant to those two fields) to know which registers to read and
 * write before committing. */
CdjC674xCmpyResult cdj_c674x_cmpy(unsigned opfield, uint32_t src1,
                                  uint32_t src1_hi, uint32_t src2);

/* SMPY32 (opfield 11001, printed page 470) and MPY2IR (01111, printed page
 * 367): two more members of the same Figure E-3 nonconditional .M group, both
 * four-cycle with three delay slots.  SMPY32 writes a 32-bit dst, MPY2IR a
 * dst_o:dst_e pair, so they use the same CdjC674xCmpyResult shape fields. */
#define CDJ_C674X_SMPY32 0x19u
#define CDJ_C674X_MPY2IR 0x0fu
CdjC674xCmpyResult cdj_c674x_mpy32_nonconditional(unsigned opfield,
                                                  uint32_t src1, uint32_t src2);
#endif
