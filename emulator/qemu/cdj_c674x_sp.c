/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Single-precision (binary32) .L/.M/.S semantics, SPRUFE8B July 2010.
 * Moved verbatim out of cdj_c674x.c, which is where the decode, the FADCR /
 * FAUCR / FMCR plumbing and the delayed-result queueing stay.
 *
 * Every function here is a pure value-in/value-out computation: none of them
 * takes or reaches a CdjC674x, so the execute loop's transactional copy is
 * unreachable from this file and no helper can read the struct tail past
 * offsetof(CdjC674x, loop).  The host floating-point environment is not used
 * either - rounding, denormals and NaNs are all done on the integer encoding -
 * so results do not depend on the build machine's FPU mode.
 */
#include "cdj_c674x_sp.h"

/* Convert a 32-bit integer to IEEE-754 binary32 without depending on the
 * host floating-point environment.  FADCR modes are nearest-even, toward
 * zero, toward +infinity and toward -infinity (SPRUFE8B Table 2-25). */
uint32_t cdj_c674x_integer_to_sp(uint32_t source, bool signed_source,
                              unsigned rmode, bool *inexact)
{
    bool negative = signed_source && (source & 0x80000000u);
    uint32_t magnitude = negative ? 0u - source : source;
    *inexact = false;
    if (!magnitude) return 0;
    unsigned msb = 31;
    while (!(magnitude & (1u << msb))) --msb;
    uint32_t mantissa;
    unsigned exponent = msb + 127;
    if (msb <= 23) {
        mantissa = magnitude << (23 - msb);
    } else {
        unsigned shift = msb - 23;
        uint32_t remainder_mask = (1u << shift) - 1;
        uint32_t remainder = magnitude & remainder_mask;
        mantissa = magnitude >> shift;
        *inexact = remainder != 0;
        bool increment = false;
        if (rmode == 0 && remainder) {
            uint32_t halfway = 1u << (shift - 1);
            increment = remainder > halfway ||
                        (remainder == halfway && (mantissa & 1));
        } else if (rmode == 2) increment = !negative && remainder;
        else if (rmode == 3) increment = negative && remainder;
        if (increment && ++mantissa == (1u << 24)) {
            mantissa >>= 1;
            ++exponent;
        }
    }
    return (negative ? 0x80000000u : 0) | exponent << 23 |
           (mantissa & 0x7fffffu);
}

CdjC674xSpResult cdj_c674x_compare_sp(uint32_t left, uint32_t right, unsigned relation)
{
    unsigned le = (left >> 23) & 255, re = (right >> 23) & 255;
    uint32_t lf = left & 0x7fffff, rf = right & 0x7fffff;
    bool lnan = le == 255 && lf, rnan = re == 255 && rf;
    bool lden = !le && lf, rden = !re && rf;
    uint32_t status = (lnan ? 1u : 0) | (rnan ? 2u : 0) |
                      (lden ? 4u : 0) | (rden ? 8u : 0);
    if (lnan || rnan) {
        status |= 1u << 9;             /* UNORD */
        if (relation) status |= 1u << 4; /* ordered compare is invalid */
        return (CdjC674xSpResult){0, status};
    }
    if (lden) left &= 0x80000000u;
    if (rden) right &= 0x80000000u;
    bool both_zero = !(left << 1) && !(right << 1);
    bool equal = both_zero || left == right;
    bool less;
    if (equal) less = false;
    else if ((left ^ right) & 0x80000000u) less = (left >> 31) != 0;
    else less = (left >> 31) ? left > right : left < right;
    uint32_t value = relation == 0 ? equal : relation == 1 ? (!equal && !less) : less;
    return (CdjC674xSpResult){value, status};
}

static uint32_t right_shift_jam32(uint32_t value, unsigned count)
{
    if (!count) return value;
    if (count < 32)
        return (value >> count) | ((value << (32 - count)) != 0);
    return value != 0;
}

/* operation is add, src1-src2, or src2-src1.  Warning bits always describe
 * the encoded src1/src2 fields, including the reversed .S SUBSP form. */
CdjC674xSpResult cdj_c674x_add_sub_sp(uint32_t source1, uint32_t source2,
                           unsigned operation, unsigned rmode)
{
    unsigned e1 = (source1 >> 23) & 255, e2 = (source2 >> 23) & 255;
    uint32_t f1 = source1 & 0x7fffff, f2 = source2 & 0x7fffff;
    bool nan1 = e1 == 255 && f1, nan2 = e2 == 255 && f2;
    bool inf1 = e1 == 255 && !f1, inf2 = e2 == 255 && !f2;
    bool den1 = !e1 && f1, den2 = !e2 && f2;
    uint32_t status = (nan1 ? 1u : 0) | (nan2 ? 2u : 0) |
                      (den1 ? 4u : 0) | (den2 ? 8u : 0);
    if (nan1 || nan2) {
        if ((nan1 && !(f1 & 0x400000)) || (nan2 && !(f2 & 0x400000)))
            status |= 1u << 4;
        return (CdjC674xSpResult){0x7fffffffu, status};
    }
    if (den1 && !inf2) status |= 1u << 7;
    if (den2 && !inf1) status |= 1u << 7;

    uint32_t left = source1, right = source2;
    if (operation == 2) {
        left = source2;
        right = source1;
    }
    if (operation) right ^= 0x80000000u;
    unsigned le = (left >> 23) & 255, re = (right >> 23) & 255;
    uint32_t lf = left & 0x7fffff, rf = right & 0x7fffff;
    if (!le && lf) left &= 0x80000000u;
    if (!re && rf) right &= 0x80000000u;
    bool linf = le == 255 && !lf, rinf = re == 255 && !rf;
    if (linf || rinf) {
        if (linf && rinf && ((left ^ right) & 0x80000000u))
            return (CdjC674xSpResult){0x7fffffffu, status | (1u << 4)};
        uint32_t infinity = linf ? left : right;
        return (CdjC674xSpResult){infinity & 0xff800000u, status | (1u << 5)};
    }

    le = (left >> 23) & 255; re = (right >> 23) & 255;
    bool lzero = le == 0, rzero = re == 0;
    unsigned lsign = left >> 31, rsign = right >> 31;
    if (lzero && rzero) {
        unsigned sign = lsign == rsign ? lsign : (rmode == 3);
        return (CdjC674xSpResult){sign << 31, status};
    }
    if (lzero) return (CdjC674xSpResult){right, status};
    if (rzero) return (CdjC674xSpResult){left, status};

    uint32_t lsig = (0x800000u | (left & 0x7fffff)) << 3;
    uint32_t rsig = (0x800000u | (right & 0x7fffff)) << 3;
    int exponent;
    if (le > re) {
        rsig = right_shift_jam32(rsig, le - re);
        exponent = le;
    } else if (re > le) {
        lsig = right_shift_jam32(lsig, re - le);
        exponent = re;
    } else exponent = le;

    uint32_t significand;
    unsigned sign;
    if (lsign == rsign) {
        sign = lsign;
        significand = lsig + rsig;
        if (significand & (1u << 27)) {
            significand = right_shift_jam32(significand, 1);
            ++exponent;
        }
    } else {
        if (lsig == rsig)
            return (CdjC674xSpResult){rmode == 3 ? 0x80000000u : 0, status};
        if (lsig > rsig) {
            sign = lsign; significand = lsig - rsig;
        } else {
            sign = rsign; significand = rsig - lsig;
        }
        while (!(significand & (1u << 26))) {
            significand <<= 1;
            --exponent;
        }
    }

    if (exponent <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        return (CdjC674xSpResult){sign << 31 | (smallest ? 0x00800000u : 0),
                          status | (1u << 8) | (1u << 7)};
    }
    uint32_t mantissa = significand >> 3;
    unsigned remainder = significand & 7;
    bool increment = (rmode == 0 &&
                      (remainder > 4 || (remainder == 4 && (mantissa & 1)))) ||
                     (rmode == 2 && !sign && remainder) ||
                     (rmode == 3 && sign && remainder);
    if (increment && ++mantissa == (1u << 24)) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        uint32_t result = sign << 31 |
                          (infinity ? 0x7f800000u : 0x7f7fffffu);
        status |= (1u << 7) | (1u << 6) | (infinity ? 1u << 5 : 0);
        return (CdjC674xSpResult){result, status};
    }
    if (remainder) status |= 1u << 7;
    return (CdjC674xSpResult){sign << 31 | (uint32_t)exponent << 23 |
                      (mantissa & 0x7fffff), status};
}

/* C674x MPYSP treats denormal sources as signed zero and flushes an
 * underflow result to signed zero or the smallest normal according to FMCR.
 * Normal finite multiplication is evaluated exactly as a 48-bit integer and
 * rounded once, avoiding host FP and excess-precision differences. */
CdjC674xSpResult cdj_c674x_multiply_sp(uint32_t left, uint32_t right, unsigned rmode)
{
    unsigned sign = (left ^ right) & 0x80000000u;
    unsigned le = (left >> 23) & 255, re = (right >> 23) & 255;
    uint32_t lf = left & 0x7fffff, rf = right & 0x7fffff;
    bool lnan = le == 255 && lf, rnan = re == 255 && rf;
    bool linf = le == 255 && !lf, rinf = re == 255 && !rf;
    bool lden = !le && lf, rden = !re && rf;
    bool lzero = !le && !lf, rzero = !re && !rf;
    uint32_t status = (lnan ? 1u : 0) | (rnan ? 2u : 0) |
                      (lden ? 4u : 0) | (rden ? 8u : 0);
    if (lnan || rnan) {
        if ((lnan && !(lf & 0x400000)) || (rnan && !(rf & 0x400000)))
            status |= 1u << 4;
        return (CdjC674xSpResult){sign | 0x7fffffffu, status};
    }
    if ((linf && (rzero || rden)) || (rinf && (lzero || lden)))
        return (CdjC674xSpResult){sign | 0x7fffffffu, status | (1u << 4)};
    if (linf || rinf)
        return (CdjC674xSpResult){sign | 0x7f800000u, status | (1u << 5)};
    if (lzero || rzero || lden || rden) {
        if ((lden && !rzero && !rden) || (rden && !lzero && !lden))
            status |= 1u << 7;
        return (CdjC674xSpResult){sign, status};
    }
    uint64_t product = (uint64_t)(0x800000u | lf) * (0x800000u | rf);
    unsigned msb = (product & (UINT64_C(1) << 47)) ? 47 : 46;
    int exponent = (int)le + (int)re - 127 + (int)msb - 46;
    unsigned shift = msb - 23;
    uint32_t mantissa = product >> shift;
    uint64_t remainder = product & ((UINT64_C(1) << shift) - 1);
    bool inexact = remainder != 0, increment = false;
    if (rmode == 0 && remainder) {
        uint64_t halfway = UINT64_C(1) << (shift - 1);
        increment = remainder > halfway ||
                    (remainder == halfway && (mantissa & 1));
    } else if (rmode == 2) increment = !sign && remainder;
    else if (rmode == 3) increment = sign && remainder;
    if (increment && ++mantissa == (1u << 24)) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        uint32_t result = sign | (infinity ? 0x7f800000u : 0x7f7fffffu);
        status |= (1u << 7) | (1u << 6) | (infinity ? 1u << 5 : 0);
        return (CdjC674xSpResult){result, status};
    }
    if (exponent <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        status |= (1u << 8) | (1u << 7);
        return (CdjC674xSpResult){sign | (smallest ? 0x00800000u : 0), status};
    }
    if (inexact) status |= 1u << 7;
    return (CdjC674xSpResult){sign | (uint32_t)exponent << 23 |
                      (mantissa & 0x7fffff), status};
}

/* SPINT/SPTRUNC convert the binary32 bit pattern directly.  Invalid and
 * overflow inputs saturate by sign; denormals become zero.  SPINT uses FADCR
 * rounding while SPTRUNC supplies mode 1 regardless of FADCR. */
CdjC674xSpResult cdj_c674x_sp_to_integer(uint32_t source, unsigned rmode)
{
    bool negative = (source & 0x80000000u) != 0;
    unsigned exponent = (source >> 23) & 255;
    uint32_t fraction = source & 0x7fffff;
    uint32_t saturated = negative ? 0x80000000u : 0x7fffffffu;
    if (exponent == 255) {
        if (fraction)
            return (CdjC674xSpResult){saturated, (1u << 4) | (1u << 1)};
        return (CdjC674xSpResult){saturated, (1u << 7) | (1u << 6)};
    }
    if (!exponent) {
        if (fraction) return (CdjC674xSpResult){0, (1u << 7) | (1u << 3)};
        return (CdjC674xSpResult){0, 0};
    }
    int power = (int)exponent - 127;
    uint64_t significand = 0x800000u | fraction;
    uint64_t magnitude, remainder = 0, halfway = 0;
    if (power >= 23) {
        if (power > 31) return (CdjC674xSpResult){saturated, (1u << 7) | (1u << 6)};
        magnitude = significand << (power - 23);
    } else if (power >= 0) {
        unsigned shift = 23 - power;
        magnitude = significand >> shift;
        remainder = significand & ((UINT64_C(1) << shift) - 1);
        halfway = UINT64_C(1) << (shift - 1);
    } else {
        magnitude = 0;
        remainder = significand;
        if (power == -1) halfway = UINT64_C(1) << 23;
        else halfway = UINT64_MAX; /* magnitude is strictly below one half */
    }
    if (remainder) {
        bool increment = (rmode == 0 &&
                          (remainder > halfway ||
                           (remainder == halfway && (magnitude & 1)))) ||
                         (rmode == 2 && !negative) ||
                         (rmode == 3 && negative);
        if (increment) ++magnitude;
    }
    if ((!negative && magnitude > 0x7fffffffu) ||
        (negative && magnitude > 0x80000000u))
        return (CdjC674xSpResult){saturated, (1u << 7) | (1u << 6)};
    uint32_t result = negative ? 0u - (uint32_t)magnitude : magnitude;
    return (CdjC674xSpResult){result, remainder ? 1u << 7 : 0};
}
