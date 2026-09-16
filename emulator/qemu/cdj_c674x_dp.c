/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Double-precision (binary64) .L/.M/.S semantics and the DP/SP/integer
 * conversions, SPRUFE8B July 2010.  Written to the same contract as
 * cdj_c674x_sp.c: every function is a pure value-in/value-out computation,
 * none of them takes or reaches a CdjC674x, and the host floating-point
 * environment is never used - rounding, denormal flushing and NaN handling
 * are all done on the integer encoding, so results do not depend on the
 * build machine's FPU mode.
 *
 * Printed page citations are against SPRUFE8B; the rule each one fixes is
 * named beside the code that implements it.
 */
#include "cdj_c674x_dp.h"

/* Figure 3-2 and Table 3-6, printed page 72. */
#define DP_SIGN      UINT64_C(0x8000000000000000)
#define DP_EXPONENT  UINT64_C(0x7ff0000000000000)
#define DP_FRACTION  UINT64_C(0x000fffffffffffff)
#define DP_HIDDEN    UINT64_C(0x0010000000000000)
#define DP_QUIET     UINT64_C(0x0008000000000000)  /* QNaN: f msb = 1 */
/* Table 3-7, printed page 72. */
#define DP_NAN_OUT   UINT64_C(0x7fffffffffffffff)
#define DP_LFPN      UINT64_C(0x7fefffffffffffff)
#define DP_SFPN      UINT64_C(0x0010000000000000)

/* Tables 2-25/2-26/2-27, printed pages 59, 61 and 63, low (.1 unit) half. */
#define DP_NAN1   (1u << 0)
#define DP_NAN2   (1u << 1)
#define DP_DEN1   (1u << 2)
#define DP_DEN2   (1u << 3)
#define DP_INVAL  (1u << 4)
#define DP_INFO   (1u << 5)
#define DP_OVER   (1u << 6)
#define DP_INEX   (1u << 7)
#define DP_UNDER  (1u << 8)
#define DP_UNORD  (1u << 9)   /* FAUCR only */

/* ABSDP, printed page 105, notes 1-4 in order. */
CdjC674xDpResult cdj_c674x_abs_dp(uint64_t source2)
{
    unsigned exponent = (source2 >> 52) & 0x7ff;
    uint64_t fraction = source2 & DP_FRACTION;
    if (exponent == 0x7ff && fraction)
        return (CdjC674xDpResult){DP_NAN_OUT,
            DP_NAN2 | ((fraction & DP_QUIET) ? 0u : DP_INVAL)};
    if (!exponent && fraction)
        return (CdjC674xDpResult){0, DP_INEX | DP_DEN2};
    if (exponent == 0x7ff)              /* +/-infinity gives +infinity */
        return (CdjC674xDpResult){DP_EXPONENT, DP_INFO};
    return (CdjC674xDpResult){source2 & ~DP_SIGN, 0};
}

/* CMPEQDP (printed page 184), CMPGTDP (193) and CMPLTDP (207).  Their
 * special-case tables agree except that an equality compare leaves INVAL
 * clear where the ordered compares set it, which is the `relation` test
 * below.  Signed denormals compare as signed zero and NaNs are unordered. */
CdjC674xDpResult cdj_c674x_compare_dp(uint64_t left, uint64_t right,
                                      unsigned relation)
{
    unsigned le = (left >> 52) & 0x7ff, re = (right >> 52) & 0x7ff;
    uint64_t lf = left & DP_FRACTION, rf = right & DP_FRACTION;
    bool lnan = le == 0x7ff && lf, rnan = re == 0x7ff && rf;
    bool lden = !le && lf, rden = !re && rf;
    uint32_t status = (lnan ? DP_NAN1 : 0u) | (rnan ? DP_NAN2 : 0u) |
                      (lden ? DP_DEN1 : 0u) | (rden ? DP_DEN2 : 0u);
    if (lnan || rnan) {
        status |= DP_UNORD;
        if (relation) status |= DP_INVAL;
        return (CdjC674xDpResult){0, status};
    }
    if (lden) left &= DP_SIGN;
    if (rden) right &= DP_SIGN;
    bool both_zero = !(left << 1) && !(right << 1);
    bool equal = both_zero || left == right;
    bool less;
    if (equal) less = false;
    else if ((left ^ right) & DP_SIGN) less = (left >> 63) != 0;
    else less = (left >> 63) ? left > right : left < right;
    uint64_t value = relation == 0 ? equal :
                     relation == 1 ? (!equal && !less) : less;
    return (CdjC674xDpResult){value, status};
}

static uint64_t right_shift_jam64(uint64_t value, unsigned count)
{
    if (!count) return value;
    if (count < 64)
        return (value >> count) | ((value << (64 - count)) != 0);
    return value != 0;
}

/* ADDDP (printed page 125) and SUBDP (printed page 541).  operation is add,
 * src1-src2, or src2-src1; the .S reverse opfield 111 0111 encodes the last
 * of those.  SUBDP note 2 requires the warning bits to describe the encoded
 * src1/src2 fields rather than the assembly operand order, so the DEN/NaN
 * bits are taken from source1/source2 before any operand swap. */
CdjC674xDpResult cdj_c674x_add_sub_dp(uint64_t source1, uint64_t source2,
                                      unsigned operation, unsigned rmode)
{
    unsigned e1 = (source1 >> 52) & 0x7ff, e2 = (source2 >> 52) & 0x7ff;
    uint64_t f1 = source1 & DP_FRACTION, f2 = source2 & DP_FRACTION;
    bool nan1 = e1 == 0x7ff && f1, nan2 = e2 == 0x7ff && f2;
    bool inf1 = e1 == 0x7ff && !f1, inf2 = e2 == 0x7ff && !f2;
    bool den1 = !e1 && f1, den2 = !e2 && f2;
    uint32_t status = (nan1 ? DP_NAN1 : 0u) | (nan2 ? DP_NAN2 : 0u) |
                      (den1 ? DP_DEN1 : 0u) | (den2 ? DP_DEN2 : 0u);
    /* ADDDP note 3 / SUBDP note 4: either NaN gives NaN_out, and an SNaN
     * source - fraction msb clear, Table 3-6 - also sets INVAL. */
    if (nan1 || nan2) {
        if ((nan1 && !(f1 & DP_QUIET)) || (nan2 && !(f2 & DP_QUIET)))
            status |= DP_INVAL;
        return (CdjC674xDpResult){DP_NAN_OUT, status};
    }
    /* ADDDP note 10 / SUBDP note 11: a signed denormalized source is treated
     * as a signed 0 and sets DENn; INEX too unless the other source is NaN
     * or signed infinity. */
    if (den1 && !inf2) status |= DP_INEX;
    if (den2 && !inf1) status |= DP_INEX;

    uint64_t left = source1, right = source2;
    if (operation == 2) {
        left = source2;
        right = source1;
    }
    if (operation) right ^= DP_SIGN;
    unsigned le = (left >> 52) & 0x7ff, re = (right >> 52) & 0x7ff;
    uint64_t lf = left & DP_FRACTION, rf = right & DP_FRACTION;
    if (!le && lf) left &= DP_SIGN;
    if (!re && rf) right &= DP_SIGN;
    bool linf = le == 0x7ff && !lf, rinf = re == 0x7ff && !rf;
    if (linf || rinf) {
        /* ADDDP notes 4-5: opposite-signed infinities give NaN_out and
         * INVAL, any other infinity gives signed infinity and INFO. */
        if (linf && rinf && ((left ^ right) & DP_SIGN))
            return (CdjC674xDpResult){DP_NAN_OUT, status | DP_INVAL};
        uint64_t infinity = linf ? left : right;
        return (CdjC674xDpResult){infinity & (DP_SIGN | DP_EXPONENT),
                                  status | DP_INFO};
    }

    le = (left >> 52) & 0x7ff;
    re = (right >> 52) & 0x7ff;
    bool lzero = le == 0, rzero = re == 0;
    unsigned lsign = (unsigned)(left >> 63), rsign = (unsigned)(right >> 63);
    if (lzero && rzero) {
        /* ADDDP note 9 keeps the common sign; note 8 makes an exact
         * cancellation +0 except under round-toward-negative-infinity. */
        unsigned sign = lsign == rsign ? lsign : (rmode == 3);
        return (CdjC674xDpResult){(uint64_t)sign << 63, status};
    }
    if (lzero) return (CdjC674xDpResult){right, status};
    if (rzero) return (CdjC674xDpResult){left, status};

    uint64_t lsig = (DP_HIDDEN | (left & DP_FRACTION)) << 3;
    uint64_t rsig = (DP_HIDDEN | (right & DP_FRACTION)) << 3;
    int exponent;
    if (le > re) {
        rsig = right_shift_jam64(rsig, le - re);
        exponent = (int)le;
    } else if (re > le) {
        lsig = right_shift_jam64(lsig, re - le);
        exponent = (int)re;
    } else exponent = (int)le;

    uint64_t significand;
    unsigned sign;
    if (lsign == rsign) {
        sign = lsign;
        significand = lsig + rsig;
        if (significand & (UINT64_C(1) << 56)) {
            significand = right_shift_jam64(significand, 1);
            ++exponent;
        }
    } else {
        if (lsig == rsig)
            return (CdjC674xDpResult){rmode == 3 ? DP_SIGN : 0, status};
        if (lsig > rsig) {
            sign = lsign; significand = lsig - rsig;
        } else {
            sign = rsign; significand = rsig - lsig;
        }
        while (!(significand & (UINT64_C(1) << 55))) {
            significand <<= 1;
            --exponent;
        }
    }

    /* ADDDP note 7: underflow sets INEX and UNDER and delivers signed 0
     * except under the rounding mode that rounds away from zero, which
     * delivers the smallest floating-point number of that sign. */
    if (exponent <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        return (CdjC674xDpResult){(uint64_t)sign << 63 |
                                  (smallest ? DP_SFPN : 0),
                                  status | DP_UNDER | DP_INEX};
    }
    uint64_t mantissa = significand >> 3;
    unsigned remainder = (unsigned)(significand & 7);
    bool increment = (rmode == 0 &&
                      (remainder > 4 || (remainder == 4 && (mantissa & 1)))) ||
                     (rmode == 2 && !sign && remainder) ||
                     (rmode == 3 && sign && remainder);
    if (increment && ++mantissa == (UINT64_C(1) << 53)) {
        mantissa >>= 1;
        ++exponent;
    }
    /* ADDDP note 6: overflow sets INEX and OVER and rounds to signed
     * infinity or signed LFPN according to the mode. */
    if (exponent >= 2047) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        uint64_t result = (uint64_t)sign << 63 |
                          (infinity ? DP_EXPONENT : DP_LFPN);
        status |= DP_INEX | DP_OVER | (infinity ? DP_INFO : 0u);
        return (CdjC674xDpResult){result, status};
    }
    if (remainder) status |= DP_INEX;     /* note 2: rounding sets INEX */
    return (CdjC674xDpResult){(uint64_t)sign << 63 |
                              (uint64_t)exponent << 52 |
                              (mantissa & DP_FRACTION), status};
}

/* Exact 128-bit product of two 53-bit significands, done in 32-bit halves so
 * that no compiler extension (__int128) is needed. */
static void multiply64(uint64_t a, uint64_t b, uint64_t *high, uint64_t *low)
{
    uint64_t al = a & 0xffffffffu, ah = a >> 32;
    uint64_t bl = b & 0xffffffffu, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t middle = (ll >> 32) + (lh & 0xffffffffu) + (hl & 0xffffffffu);
    *low = (ll & 0xffffffffu) | (middle << 32);
    *high = hh + (lh >> 32) + (hl >> 32) + (middle >> 32);
}

/* MPYDP, printed page 318, notes 1-5.  Denormal sources are treated as
 * signed zero; the finite product is formed exactly as a 106-bit integer and
 * rounded once, avoiding host FP and excess-precision differences. */
CdjC674xDpResult cdj_c674x_multiply_dp(uint64_t left, uint64_t right,
                                       unsigned rmode)
{
    uint64_t sign = (left ^ right) & DP_SIGN;
    unsigned le = (left >> 52) & 0x7ff, re = (right >> 52) & 0x7ff;
    uint64_t lf = left & DP_FRACTION, rf = right & DP_FRACTION;
    bool lnan = le == 0x7ff && lf, rnan = re == 0x7ff && rf;
    bool linf = le == 0x7ff && !lf, rinf = re == 0x7ff && !rf;
    bool lden = !le && lf, rden = !re && rf;
    bool lzero = !le && !lf, rzero = !re && !rf;
    uint32_t status = (lnan ? DP_NAN1 : 0u) | (rnan ? DP_NAN2 : 0u) |
                      (lden ? DP_DEN1 : 0u) | (rden ? DP_DEN2 : 0u);
    /* Note 1: the sign of NaN_out is the exclusive-OR of the input signs. */
    if (lnan || rnan) {
        if ((lnan && !(lf & DP_QUIET)) || (rnan && !(rf & DP_QUIET)))
            status |= DP_INVAL;
        return (CdjC674xDpResult){sign | DP_NAN_OUT, status};
    }
    /* Notes 2 and 4: infinity times signed 0 - and a denormal is a signed 0
     * here - is a signed NaN_out with INVAL. */
    if ((linf && (rzero || rden)) || (rinf && (lzero || lden)))
        return (CdjC674xDpResult){sign | DP_NAN_OUT, status | DP_INVAL};
    if (linf || rinf)
        return (CdjC674xDpResult){sign | DP_EXPONENT, status | DP_INFO};
    if (lzero || rzero || lden || rden) {
        /* Note 4: a denormal source sets INEX except when the other source
         * is signed infinity, signed NaN or signed 0. */
        if ((lden && !rzero && !rden) || (rden && !lzero && !lden))
            status |= DP_INEX;
        return (CdjC674xDpResult){sign, status};
    }
    uint64_t high, low;
    multiply64(DP_HIDDEN | lf, DP_HIDDEN | rf, &high, &low);
    unsigned msb = (high & (UINT64_C(1) << 41)) ? 105 : 104;
    int exponent = (int)le + (int)re - 1023 + (int)msb - 104;
    unsigned shift = msb - 52;                    /* 52 or 53 */
    uint64_t mantissa = (high << (64 - shift)) | (low >> shift);
    uint64_t remainder = low & ((UINT64_C(1) << shift) - 1);
    bool inexact = remainder != 0, increment = false;
    if (rmode == 0 && remainder) {
        uint64_t halfway = UINT64_C(1) << (shift - 1);
        increment = remainder > halfway ||
                    (remainder == halfway && (mantissa & 1));
    } else if (rmode == 2) increment = !sign && remainder;
    else if (rmode == 3) increment = sign && remainder;
    if (increment && ++mantissa == (UINT64_C(1) << 53)) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 2047) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        uint64_t result = sign | (infinity ? DP_EXPONENT : DP_LFPN);
        status |= DP_INEX | DP_OVER | (infinity ? DP_INFO : 0u);
        return (CdjC674xDpResult){result, status};
    }
    if (exponent <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        return (CdjC674xDpResult){sign | (smallest ? DP_SFPN : 0),
                                  status | DP_UNDER | DP_INEX};
    }
    if (inexact) status |= DP_INEX;      /* note 5 */
    return (CdjC674xDpResult){sign | (uint64_t)exponent << 52 |
                              (mantissa & DP_FRACTION), status};
}

uint64_t cdj_c674x_sp_operand_to_dp(uint32_t source)
{
    uint64_t sign = (uint64_t)(source & 0x80000000u) << 32;
    unsigned exponent = (source >> 23) & 255;
    uint64_t fraction = source & 0x7fffffu;
    /* A binary32 denormal is exactly representable as a binary64 normal, but
     * MPYSPDP/MPYSP2DP note 4 requires it to act as a signed 0 with DENn set.
     * Handing the multiplier a DP denormal instead reaches exactly that path
     * and preserves the sign; the magnitude is discarded either way. */
    if (!exponent) return sign | (fraction ? 1u : 0u);
    /* Shifting the 23-bit fraction up by 29 keeps the quiet/signaling msb in
     * the msb of the 52-bit fraction, so SNaN stays SNaN (Tables 3-4, 3-6). */
    if (exponent == 255) return sign | DP_EXPONENT | (fraction << 29);
    return sign | ((uint64_t)(exponent - 127 + 1023) << 52) | (fraction << 29);
}

/* SPDP, printed page 477, notes 1-5.  "No overflow or underflow can occur". */
CdjC674xDpResult cdj_c674x_sp_to_dp(uint32_t source2)
{
    unsigned exponent = (source2 >> 23) & 255;
    uint32_t fraction = source2 & 0x7fffffu;
    uint64_t sign = (uint64_t)(source2 & 0x80000000u) << 32;
    if (exponent == 255 && fraction)
        return (CdjC674xDpResult){DP_NAN_OUT,
            DP_NAN2 | ((fraction & 0x400000u) ? 0u : DP_INVAL)};
    if (exponent == 255)
        return (CdjC674xDpResult){sign | DP_EXPONENT, DP_INFO};
    if (!exponent && fraction)
        return (CdjC674xDpResult){sign, DP_INEX | DP_DEN2};
    if (!exponent) return (CdjC674xDpResult){sign, 0};
    return (CdjC674xDpResult){sign |
        ((uint64_t)(exponent - 127 + 1023) << 52) |
        ((uint64_t)fraction << 29), 0};
}

/* DPSP, printed page 260, notes 1-7. */
CdjC674xDpResult cdj_c674x_dp_to_sp(uint64_t source2, unsigned rmode)
{
    unsigned exponent = (source2 >> 52) & 0x7ff;
    uint64_t fraction = source2 & DP_FRACTION;
    uint32_t sign = (uint32_t)(source2 >> 32) & 0x80000000u;
    if (exponent == 0x7ff && fraction)
        return (CdjC674xDpResult){0x7fffffffu,
            DP_NAN2 | ((fraction & DP_QUIET) ? 0u : DP_INVAL)};
    if (exponent == 0x7ff)
        return (CdjC674xDpResult){sign | 0x7f800000u, DP_INFO};
    if (!exponent && fraction)
        return (CdjC674xDpResult){sign, DP_INEX | DP_DEN2};
    if (!exponent) return (CdjC674xDpResult){sign, 0};
    int power = (int)exponent - 1023;
    uint64_t significand = DP_HIDDEN | fraction;      /* 53 bits, msb 52 */
    uint32_t mantissa = (uint32_t)(significand >> 29);/* 24 bits, msb 23 */
    uint64_t remainder = significand & ((UINT64_C(1) << 29) - 1);
    uint64_t halfway = UINT64_C(1) << 28;
    bool increment = (rmode == 0 && (remainder > halfway ||
                        (remainder == halfway && (mantissa & 1)))) ||
                     (rmode == 2 && !sign && remainder) ||
                     (rmode == 3 && sign && remainder);
    if (increment && ++mantissa == (1u << 24)) {
        mantissa >>= 1;
        ++power;
    }
    int biased = power + 127;
    if (biased >= 255) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        return (CdjC674xDpResult){sign | (infinity ? 0x7f800000u : 0x7f7fffffu),
            DP_INEX | DP_OVER | (infinity ? DP_INFO : 0u)};
    }
    if (biased <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        return (CdjC674xDpResult){sign | (smallest ? 0x00800000u : 0u),
            DP_INEX | DP_UNDER};
    }
    return (CdjC674xDpResult){sign | (uint32_t)biased << 23 |
        (mantissa & 0x7fffffu), remainder ? DP_INEX : 0u};
}

/* DPINT (printed page 258) and DPTRUNC (printed page 262), notes 1-4. */
CdjC674xDpResult cdj_c674x_dp_to_integer(uint64_t source2, unsigned rmode)
{
    bool negative = (source2 & DP_SIGN) != 0;
    unsigned exponent = (source2 >> 52) & 0x7ff;
    uint64_t fraction = source2 & DP_FRACTION;
    uint32_t saturated = negative ? 0x80000000u : 0x7fffffffu;
    if (exponent == 0x7ff) {
        if (fraction)
            return (CdjC674xDpResult){saturated, DP_INVAL | DP_NAN2};
        return (CdjC674xDpResult){saturated, DP_INEX | DP_OVER};
    }
    if (!exponent) {
        if (fraction) return (CdjC674xDpResult){0, DP_INEX | DP_DEN2};
        return (CdjC674xDpResult){0, 0};
    }
    int power = (int)exponent - 1023;
    /* Note 2: overflow above 2^31 - 1 or below -2^31.  Anything with an
     * exponent past 31 is already outside that range. */
    if (power > 31) return (CdjC674xDpResult){saturated, DP_INEX | DP_OVER};
    uint64_t significand = DP_HIDDEN | fraction;
    uint64_t magnitude, remainder, halfway;
    if (power >= 0) {
        unsigned shift = 52 - (unsigned)power;        /* 21..52 */
        magnitude = significand >> shift;
        remainder = significand & ((UINT64_C(1) << shift) - 1);
        halfway = UINT64_C(1) << (shift - 1);
    } else {
        magnitude = 0;
        remainder = significand;
        /* Below one half nothing can round up to one, so no reachable
         * halfway point exists. */
        halfway = power == -1 ? DP_HIDDEN : UINT64_MAX;
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
        return (CdjC674xDpResult){saturated, DP_INEX | DP_OVER};
    uint32_t result = negative ? 0u - (uint32_t)magnitude : (uint32_t)magnitude;
    return (CdjC674xDpResult){result, remainder ? DP_INEX : 0u};
}

/* INTDP (printed page 275) and INTDPU (printed page 276).  Every 32-bit
 * value fits the 53-bit significand exactly, so no rounding and no status. */
uint64_t cdj_c674x_integer_to_dp(uint32_t source2, bool signed_source)
{
    bool negative = signed_source && (source2 & 0x80000000u);
    uint64_t magnitude = negative ? (uint64_t)(0u - source2) : source2;
    if (!magnitude) return 0;
    unsigned msb = 31;
    while (!(magnitude & (UINT64_C(1) << msb))) --msb;
    return (negative ? DP_SIGN : 0) |
           ((uint64_t)(msb + 1023) << 52) |
           ((magnitude << (52 - msb)) & DP_FRACTION);
}
