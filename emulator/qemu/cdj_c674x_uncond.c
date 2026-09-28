/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c674x_uncond.h"
/* Every opfield below was read out of the instruction's own Opcode figure in
 * SPRUFE8B and then confirmed against ti-cgt-c6000 8.5.0 asm6x -mv6740, whose
 * listing words are quoted in tests/cstub/c674x-uncond.c.
 *
 * The manual's complete set of 32-bit encodings with the literal 0001 in bits
 * 31-28 is 28 instructions: the three ADDA long-immediate forms, CALLP,
 * DINT, RINT and the 22 classified here, of which DPACKX2, DPACK2, SHFL3, the
 * four dual ADD/SUB forms and the eight-strong CMPY/DDOTP group plus SMPY32 and
 * MPY2IR are now implemented and routed to the caller's dispatch table. */

/* Figure D-3 (printed page 735), .L unit nonconditional: op is bits 11-5 and
 * bits 4-2 are 110, the same low bits as the predicable Figure D-1. */
/*
 * Classify a .L nonconditional opfield.
 *
 * Returns 0 for "not one of these", L_DST4 when the figure allocates only four
 * bits to dst and reserves bit 23 as 0, and L_DST5 when dst is a full five bits.
 * Six of the seven reserve bit 23; SHFL3 (printed page 445) is the exception,
 * where bits 27-23 are all dst.  Accepting bit 23 set for the other six would
 * claim encodings the manual reserves.
 */
#define L_DST4 1
#define L_DST5 2

static int l_unit_op(unsigned op)
{
    switch (op) {
    case 0x0c: /* ADDSUB,   printed page 132 */
    case 0x0d: /* ADDSUB2,  printed page 133 */
    case 0x0e: /* SADDSUB,  printed page 427 */
    case 0x0f: /* SADDSUB2, printed page 429 */
    case 0x33: /* DPACKX2,  printed page 256 */
    case 0x34: /* DPACK2,   printed page 254 */
        return L_DST4;
    case 0x36: /* SHFL3,    printed page 445 */
        return L_DST5;
    default:
        return 0;
    }
}

/* Figure E-3 (printed page 743), .M unit nonconditional: bit 11 is 0, op is
 * bits 10-6 and bits 5-2 are 1100.  Returns 0 for "not one of these", or which
 * of the two dispositions the opfield has. */
#define M_UNIMPLEMENTED 1
#define M_ARM_TABLE     2

static int m_unit_op(unsigned op)
{
    switch (op) {
    case 0x1b: /* XORMPY,   printed page 566 */
    case 0x1f: /* GMPY,     printed page 270 */
        return M_ARM_TABLE;
    /* The remaining implemented .M operations also route to their arm rows. */
    case 0x0a: /* CMPY,     printed page 215 */
    case 0x0b: /* CMPYR,    printed page 217 */
    case 0x0c: /* CMPYR1,   printed page 219 */
    case 0x14: /* DDOTPL2R, printed page 229 */
    case 0x15: /* DDOTPH2R, printed page 225 */
    case 0x16: /* DDOTPL2,  printed page 227 */
    case 0x17: /* DDOTPH2,  printed page 223 */
    case 0x18: /* DDOTP4,   printed page 221 */
    case 0x0f: /* MPY2IR,   printed page 367 */
    case 0x19: /* SMPY32,   printed page 470 */
        return M_ARM_TABLE;
    default:
        return 0;
    }
}

CdjC674xUncondKind cdj_c674x_uncond_classify(uint32_t word)
{
    if ((word >> 28) != 1) return CDJ_C674X_UNCOND_NONE;
    /* Figure C-3 (printed page 724): y is bit 7, op is bits 6-4 and bits 3-2
     * are 11.  Only op 011/101/111 are assigned; the other five op values in
     * that slot belong to Figure C-5's predicable 15-bit-offset loads and
     * stores, and ADDAD has no long-immediate form (printed pages 117-118). */
    switch (word & 0x7cu) {
    case 0x3cu: return CDJ_C674X_UNCOND_ADDAB; /* printed page 115 */
    case 0x5cu: return CDJ_C674X_UNCOND_ADDAH; /* printed page 120 */
    case 0x7cu: return CDJ_C674X_UNCOND_ADDAW; /* printed page 123 */
    default: break;
    }
    if ((word & 0x1cu) == 0x18u) {
        unsigned op = (word >> 5) & 0x7fu;
        int l = l_unit_op(op);

        if (l == L_DST5 || (l == L_DST4 && !(word & (1u << 23))))
            /* DPACKX2, DPACK2 and SHFL3 have semantics in
             * cdj_c674x_packbits.c and a dispatch row of their own. */
            return op == 0x33 || op == 0x34 || op == 0x36 ||
                   (op >= 0x0c && op <= 0x0f) ?
                   CDJ_C674X_UNCOND_ARM_TABLE :
                   CDJ_C674X_UNCOND_UNIMPLEMENTED;
    }
    if ((word & 0x3cu) == 0x30u) {
        unsigned extent = (word >> 10) & 3u; /* bits 11-10 */
        if (!(extent & 2u)) {
            int m = m_unit_op((word >> 6) & 0x1fu);
            if (m == M_ARM_TABLE) return CDJ_C674X_UNCOND_ARM_TABLE;
            if (m == M_UNIMPLEMENTED) return CDJ_C674X_UNCOND_UNIMPLEMENTED;
        }
        /* Figure F-14 (printed page 749), .S unit nonconditional: bits 11-10
         * are 11, op is bits 9-6.  RPACK2 (printed page 416) is its only
         * member and has a dispatch-table arm. */
        if (extent == 3u && ((word >> 6) & 0xfu) == 0xbu)
            return CDJ_C674X_UNCOND_ARM_TABLE;
    }
    /* Figure H-1 (printed page 765): every bit outside op (16-13) and p is 0.
     * op 0000 is SWE (printed page 557) and 0001 SWENR (558). */
    if ((word & ~0x2001u) == 0x10000000u)
        return CDJ_C674X_UNCOND_UNIMPLEMENTED;
    return CDJ_C674X_UNCOND_NONE;
}

CdjC674xAddaLong cdj_c674x_adda_long(CdjC674xUncondKind kind, uint32_t word,
                                    uint32_t b14, uint32_t b15)
{
    /* ADDAB adds ucst15 unscaled, ADDAH scales it by a left shift of 1 and
     * ADDAW by 2 (printed pages 115, 120, 123: "The offset, ucst15, is
     * [scaled by a left-shift of n and] added to baseR"). */
    unsigned shift = kind == CDJ_C674X_UNCOND_ADDAH ? 1 :
                     kind == CDJ_C674X_UNCOND_ADDAW ? 2 : 0;
    uint32_t base = (word & 0x80u) ? b15 : b14; /* y = bit 7 */
    CdjC674xAddaLong result = {
        .dst = (word >> 23) & 31u,
        .side = (word >> 1) & 1u, /* s = bit 1: 0 = .D1/A file, 1 = .D2/B */
        .result = base + (((word >> 8) & 0x7fffu) << shift),
    };
    return result;
}
