/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed .M-unit dot products, SPRUFE8B July 2010.  Semantics only; the
 * decoder, the register-pair rule and the E4 delayed-result queueing stay in
 * cdj_c674x.c.  Each rule below quotes the printed page it comes from. */
#include "cdj_c674x_dotp.h"

static int32_t s16(uint32_t word, unsigned half)
{
    return (int16_t)(word >> (16u * half));
}

static int32_t u16(uint32_t word, unsigned half)
{
    return (int32_t)(uint32_t)(uint16_t)(word >> (16u * half));
}

static int32_t s8(uint32_t word, unsigned byte)
{
    return (int8_t)(word >> (8u * byte));
}

static int32_t u8(uint32_t word, unsigned byte)
{
    return (int32_t)(uint32_t)(uint8_t)(word >> (8u * byte));
}

/* ">> 16" on a signed intermediate, rounding toward negative infinity.  C
 * leaves the right shift of a negative signed value implementation-defined, so
 * the sign fill is written out.  The caller has already established that the
 * intermediate fits in 32 bits. */
static int32_t shift_right_16(int32_t value)
{
    uint32_t shifted = (uint32_t)value >> 16;
    if (value < 0) shifted |= 0xffff0000u;
    return (int32_t)shifted;
}

/* The rounded forms.  Printed page 240 (DOTPNRSU2) and printed page 244
 * (DOTPRSU2) give the same three steps: multiply src1's signed halfwords by
 * src2's unsigned halfwords, combine the two products, add 8000h, then signed
 * shift right by 16 and sign extend into dst.
 *
 * Printed page 244 also states the limit: "Overflow can be avoided if the sum
 * of the two products plus the rounding term is less than or equal to 2^31 - 1
 * for a positive sum and greater than or equal to -2^31 for a negative sum."
 * Outside that range the manual declares the result undefined and prints
 * "xxxx xxxxh" in its own worked examples (DOTPNRSU2 example 3, printed page
 * 241; DOTPRSU2 example 3, printed page 246) - and it contradicts itself in
 * the same breath, because both entries also claim "The intermediate results
 * ... are maintained to a 33-bit precision, ensuring that no overflow may
 * occur".  Nothing in SPRUFE8B says what those words actually produce, so the
 * caller is told to halt rather than invent one. */
static CdjC674xDotpResult rounded(int64_t products)
{
    CdjC674xDotpResult result = {0, false, true};
    int64_t intermediate = products + 0x8000;

    if (intermediate > INT32_MAX || intermediate < INT32_MIN) {
        result.undefined = true;
        return result;
    }
    result.value = (uint64_t)(int64_t)shift_right_16((int32_t)intermediate);
    return result;
}

CdjC674xDotpResult cdj_c674x_dotp(unsigned opfield, uint32_t src1,
                                  uint32_t src2)
{
    CdjC674xDotpResult result = {0, false, true};

    switch (opfield) {
    case CDJ_C674X_DOTP2:
    case CDJ_C674X_DOTP2L:
        /* Printed page 236: "(lsb16(src1) x lsb16(src2)) + (msb16(src1) x
         * msb16(src2)) -> dst", and printed page 235 for the pair form: "the
         * upper word of the register pair always contains either all 0s or all
         * 1s, depending on whether the result is positive or negative".
         *
         * The one documented overflow, printed page 236: "In the overflow
         * case, where all four halfwords in src1 and src2 are 8000h, the value
         * 8000 0000h is written into the 32-bit dst and 0000 0000 8000 0000h
         * is written into the 64-bit dst."  That is 2^30 + 2^30 = 2^31 held as
         * a positive 64-bit sum, which is what this expression produces; no
         * special case is needed and none is written. */
        result.value = (uint64_t)((int64_t)s16(src1, 0) * s16(src2, 0) +
                                  (int64_t)s16(src1, 1) * s16(src2, 1));
        break;
    case CDJ_C674X_DOTPN2:
        /* Printed page 238: "(msb16(src1) x msb16(src2)) - (lsb16(src1) x
         * lsb16(src2)) -> dst", and "unlike DOTP2, no overflow case exists for
         * this instruction". */
        result.value = (uint64_t)((int64_t)s16(src1, 1) * s16(src2, 1) -
                                  (int64_t)s16(src1, 0) * s16(src2, 0));
        break;
    case CDJ_C674X_DOTPNRSU2:
        /* Printed page 240: "int33 = (smsb16(src1) x umsb16(src2)) -
         * (slsb16(src1) x ulsb16(src2)) + 8000h; int33 >> 16 -> dst". */
        result = rounded((int64_t)s16(src1, 1) * u16(src2, 1) -
                         (int64_t)s16(src1, 0) * u16(src2, 0));
        break;
    case CDJ_C674X_DOTPRSU2:
        /* Printed page 245: "int33 = (smsb16(src1) x umsb16(src2)) +
         * (slsb16(src1) x ulsb16(src2)) + 8000h; int33 >> 16 -> dst". */
        result = rounded((int64_t)s16(src1, 1) * u16(src2, 1) +
                         (int64_t)s16(src1, 0) * u16(src2, 0));
        break;
    case CDJ_C674X_DOTPSU4:
        /* Printed page 249: the four signed-by-unsigned byte products summed
         * and "written as a signed 32-bit result to dst".  The largest
         * magnitude reachable is 4 x 128 x 255 = 130,560, so the sum cannot
         * leave 32 bits and the manual states no overflow case. */
        result.value = (uint64_t)(int64_t)(int32_t)
            (s8(src1, 0) * u8(src2, 0) + s8(src1, 1) * u8(src2, 1) +
             s8(src1, 2) * u8(src2, 2) + s8(src1, 3) * u8(src2, 3));
        break;
    case CDJ_C674X_DOTPU4:
        /* Printed page 252: the four unsigned byte products summed and written
         * as an unsigned 32-bit result.  4 x 255 x 255 = 260,100. */
        result.value = (uint32_t)
            (u8(src1, 0) * u8(src2, 0) + u8(src1, 1) * u8(src2, 1) +
             u8(src1, 2) * u8(src2, 2) + u8(src1, 3) * u8(src2, 3));
        break;
    default:
        result.valid = false;
        break;
    }
    return result;
}

/* ---- the nonconditional .M group, Figure E-3 (printed page 743) ---------- */

/* sat() in these Execution blocks is the 32-bit signed clamp: the C674x
 * saturating operations clamp to 7FFF FFFFh / 8000 0000h (SPRUFE8B 2.9.4,
 * and every sat() in the .M unit's own entries is applied to a value that is
 * being written as a signed 32-bit quantity). */
static int32_t sat32(int64_t v, bool *saturated)
{
    if (v > INT64_C(0x7fffffff))  { *saturated = true; return (int32_t)0x7fffffff; }
    if (v < -INT64_C(0x80000000)) { *saturated = true; return (int32_t)0x80000000; }
    return (int32_t)v;
}

/* msb16(sat(x + 0000 8000h)): the rounding step CMPYR, DDOTPH2R and DDOTPL2R
 * all share.  The addition is inside the saturate, so a sum that overflows on
 * the rounding constant clamps before the halfword is taken. */
static uint32_t round_msb16(int64_t value, bool *saturated)
{
    return ((uint32_t)sat32(value + 0x8000, saturated) >> 16) & 0xffffu;
}

/* msb16(sat((x + 0000 4000h) << 1)): CMPYR1's variant, which rounds at bit 14
 * and then doubles, so it keeps one more significant bit than CMPYR.  The
 * shift is inside the saturate as printed. */
static uint32_t round_shift_msb16(int64_t value, bool *saturated)
{
    return ((uint32_t)sat32((value + 0x4000) * 2, saturated) >> 16) & 0xffffu;
}

CdjC674xCmpyResult cdj_c674x_cmpy(unsigned opfield, uint32_t src1,
                                  uint32_t src1_hi, uint32_t src2)
{
    CdjC674xCmpyResult r = { .value = 0, .pair_dst = false, .pair_src1 = false,
                             .saturated = false, .valid = true };
    /* src1_e is the even (low) register of a src1 pair and src1_hi the odd
     * one; the DDOTP*2 forms name them src1_e and src1_o. */
    int64_t s1h = s16(src1, 1), s1l = s16(src1, 0);
    int64_t s2h = s16(src2, 1), s2l = s16(src2, 0);
    int64_t o1h = s16(src1_hi, 1), o1l = s16(src1_hi, 0);
    int64_t e, o;

    switch (opfield) {
    case CDJ_C674X_CMPY:
        /* Printed page 215:
         *   sat((lsb16(src1) x msb16(src2)) + (msb16(src1) x lsb16(src2))) -> dst_e
         *   (msb16(src1) x msb16(src2)) - (lsb16(src1) x lsb16(src2))      -> dst_o
         * Only dst_e is saturated; dst_o is printed without sat().  Two
         * 16x16 products cannot leave 32 bits anyway, so the asymmetry is
         * observable only as a status effect, and it is reproduced rather than
         * tidied up. */
        r.pair_dst = true;
        e = s1l * s2h + s1h * s2l;
        o = s1h * s2h - s1l * s2l;
        r.value = ((uint64_t)(uint32_t)(int32_t)o << 32) |
                  (uint32_t)sat32(e, &r.saturated);
        break;
    case CDJ_C674X_CMPYR:
        /* Printed page 217: both halves saturate, then each is rounded by
         * msb16(sat(tmp + 0000 8000h)) into one 32-bit dst. */
        e = sat32(s1l * s2h + s1h * s2l, &r.saturated);
        o = sat32(s1h * s2h - s1l * s2l, &r.saturated);
        r.value = (round_msb16(o, &r.saturated) << 16) |
                  round_msb16(e, &r.saturated);
        break;
    case CDJ_C674X_CMPYR1:
        /* Printed page 219.  THE MANUAL'S PSEUDOCODE HAS A TYPO HERE: it
         * computes tmp_o and then prints
         *     msb16(sat((tmp_e + 0000 4000h) << 1)) -> msb16(dst)
         * using tmp_e for BOTH halves and leaving tmp_o unused.  Its own
         * Example 1 settles it: CMPYR1 .M1 A0,A1,A2 with A0 = 0800 0400h and
         * A1 = 0900 0200h gives A2 = 0080 0068h.  tmp_e = 1024x2304 +
         * 2048x512 = 34 0000h rounds to 0068h, and tmp_o = 2048x2304 -
         * 1024x512 = 40 0000h rounds to 0080h, which is what the upper half
         * holds.  Taking the pseudocode literally would put 0068h in both
         * halves and contradict the example, so tmp_o is used. */
        e = sat32(s1l * s2h + s1h * s2l, &r.saturated);
        o = sat32(s1h * s2h - s1l * s2l, &r.saturated);
        r.value = (round_shift_msb16(o, &r.saturated) << 16) |
                  round_shift_msb16(e, &r.saturated);
        break;
    case CDJ_C674X_DDOTP4:
        /* Printed page 221: src2's halfwords each supply a packed BYTE pair.
         *   (msb16(src1) x msb8(lsb16(src2))) + (lsb16(src1) x lsb8(lsb16(src2))) -> dst_e
         *   (msb16(src1) x msb8(msb16(src2))) + (lsb16(src1) x lsb8(msb16(src2))) -> dst_o
         * No sat() is printed on either line. */
        r.pair_dst = true;
        e = s1h * s8(src2, 1) + s1l * s8(src2, 0);
        o = s1h * s8(src2, 3) + s1l * s8(src2, 2);
        r.value = ((uint64_t)(uint32_t)(int32_t)o << 32) |
                  (uint32_t)(int32_t)e;
        break;
    case CDJ_C674X_DDOTPH2:
        /* Printed page 223:
         *   sat((msb16(src1_o) x msb16(src2)) + (lsb16(src1_o) x lsb16(src2))) -> dst_o
         *   sat((lsb16(src1_o) x msb16(src2)) + (msb16(src1_e) x lsb16(src2))) -> dst_e
         * dst_e deliberately mixes the two src1 registers: that straddling
         * pair is what makes this a "double" dot product. */
        r.pair_dst = r.pair_src1 = true;
        o = o1h * s2h + o1l * s2l;
        e = o1l * s2h + s1h * s2l;
        r.value = ((uint64_t)(uint32_t)sat32(o, &r.saturated) << 32) |
                  (uint32_t)sat32(e, &r.saturated);
        break;
    case CDJ_C674X_DDOTPL2:
        /* Printed page 227:
         *   sat((msb16(src1_e) x msb16(src2)) + (lsb16(src1_e) x lsb16(src2))) -> dst_e
         *   sat((lsb16(src1_o) x msb16(src2)) + (msb16(src1_e) x lsb16(src2))) -> dst_o */
        r.pair_dst = r.pair_src1 = true;
        e = s1h * s2h + s1l * s2l;
        o = o1l * s2h + s1h * s2l;
        r.value = ((uint64_t)(uint32_t)sat32(o, &r.saturated) << 32) |
                  (uint32_t)sat32(e, &r.saturated);
        break;
    case CDJ_C674X_DDOTPH2R:
        /* Printed page 225: the DDOTPH2 sums, each rounded by
         * msb16(sat(sum + 0000 8000h)) into one 32-bit dst. */
        r.pair_src1 = true;
        o = o1h * s2h + o1l * s2l;
        e = o1l * s2h + s1h * s2l;
        r.value = (round_msb16(o, &r.saturated) << 16) |
                  round_msb16(e, &r.saturated);
        break;
    case CDJ_C674X_DDOTPL2R:
        /* Printed page 229.  Note the halves are crossed relative to
         * DDOTPH2R: the src1_e sum goes to lsb16(dst) and the straddling sum
         * to msb16(dst). */
        r.pair_src1 = true;
        e = s1h * s2h + s1l * s2l;
        o = o1l * s2h + s1h * s2l;
        r.value = (round_msb16(o, &r.saturated) << 16) |
                  round_msb16(e, &r.saturated);
        break;
    default:
        r.valid = false;
        break;
    }
    return r;
}

CdjC674xCmpyResult cdj_c674x_mpy32_nonconditional(unsigned opfield,
                                                  uint32_t src1, uint32_t src2)
{
    CdjC674xCmpyResult r = { .value = 0, .pair_dst = false, .pair_src1 = false,
                             .saturated = false, .valid = true };
    switch (opfield) {
    case CDJ_C674X_SMPY32: {
        /* Printed page 470: msb32(sat((src2 x src1) << 1)) -> dst.  Both
         * sources are signed 32-bit, so the product needs 63 bits and the
         * left shift needs 64.  Only -2^31 x -2^31 = 2^62 overflows a signed
         * 64-bit value once doubled, which is the single case sat() exists
         * for; it clamps to 7FFF FFFF FFFF FFFFh and dst becomes 7FFF FFFFh. */
        int64_t product = (int64_t)(int32_t)src1 * (int64_t)(int32_t)src2;
        int64_t shifted;
        if (product >= (INT64_C(1) << 62)) {
            shifted = INT64_MAX;
            r.saturated = true;
        } else {
            shifted = product << 1;
        }
        r.value = (uint32_t)((uint64_t)shifted >> 32);
        break;
    }
    case CDJ_C674X_MPY2IR:
        /* Printed page 367.  Each signed halfword of src1 multiplies the whole
         * signed src2, the product is rounded by adding 4000h and shifted right
         * by 15, and the low 32 bits are written - msb16(src1) to dst_o and
         * lsb16(src1) to dst_e.  The manual gives the saturating case its own
         * explicit branch rather than a sat(): "if (msb16(src1) = 8000h &&
         * src2 = 8000 0000h), 7FFF FFFFh -> dst_o", which is the only input
         * pair whose rounded product leaves 32 bits. */
        r.pair_dst = true;
        {
            int32_t hi = (int16_t)(src1 >> 16), lo = (int16_t)(src1 & 0xffffu);
            bool sat_o = hi == INT16_MIN && src2 == 0x80000000u;
            bool sat_e = lo == INT16_MIN && src2 == 0x80000000u;
            uint32_t o = sat_o ? 0x7fffffffu : (uint32_t)
                (((int64_t)hi * (int64_t)(int32_t)src2 + 0x4000) >> 15);
            uint32_t e = sat_e ? 0x7fffffffu : (uint32_t)
                (((int64_t)lo * (int64_t)(int32_t)src2 + 0x4000) >> 15);
            r.saturated = sat_o || sat_e;
            r.value = ((uint64_t)o << 32) | e;
        }
        break;
    default:
        r.valid = false;
        break;
    }
    return r;
}
