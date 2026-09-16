/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <string.h>
#include "cdj_c674x_approx.h"

/* Table 2-26 (printed page 61), low (.1 unit) half. */
#define AP_NAN2   (1u << 1)
#define AP_DEN2   (1u << 3)
#define AP_INVAL  (1u << 4)
#define AP_INFO   (1u << 5)
#define AP_OVER   (1u << 6)
#define AP_INEX   (1u << 7)
#define AP_UNDER  (1u << 8)
#define AP_DIV0   (1u << 10)

/* NaN_out, Table 3-5 (printed page 71) and Table 3-7 (printed page 72). */
#define NAN_OUT_SP UINT32_C(0x7fffffff)
#define NAN_OUT_DP UINT64_C(0x7fffffffffffffff)

static double bits_to_double(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint64_t double_to_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float bits_to_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t float_to_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* Keep the top `keep` explicit mantissa bits and round to nearest at the first
 * discarded one, by adding half an interval to the whole pattern and masking.
 * Carrying out of the mantissa into the exponent is correct IEEE behaviour and
 * is why this is done on the bit pattern rather than on the fields.
 *
 * 8 kept bits put the error at or below 2^-9 of the mantissa, inside the
 * "less than 2-8" the pages promise, and leave an exactly representable result
 * (mantissa all zero) untouched - which is what makes each entry's own worked
 * example come out exact rather than merely close. */
#define APPROX_MANTISSA_BITS 8

static uint32_t round_sp(uint32_t bits)
{
    unsigned drop = 23u - APPROX_MANTISSA_BITS;
    uint32_t half = UINT32_C(1) << (drop - 1);
    return (bits + half) & ~((UINT32_C(1) << drop) - 1u);
}

static uint64_t round_dp(uint64_t bits)
{
    unsigned drop = 52u - APPROX_MANTISSA_BITS;
    uint64_t half = UINT64_C(1) << (drop - 1);
    return (bits + half) & ~((UINT64_C(1) << drop) - 1u);
}

/* Classification of one source, shared by all four forms. */
typedef struct {
    bool sign, zero, denormal, infinity, snan, qnan;
} Classified;

static Classified classify_sp(uint32_t bits)
{
    unsigned exponent = (bits >> 23) & 0xffu;
    uint32_t fraction = bits & UINT32_C(0x7fffff);
    Classified c = { .sign = (bits >> 31) & 1u };
    if (exponent == 0xffu) {
        /* Table 3-4 (printed page 71): a NaN has a nonzero fraction, and
         * Table 3-6's convention makes the fraction msb the quiet bit. */
        if (!fraction) c.infinity = true;
        else if (fraction & UINT32_C(0x400000)) c.qnan = true;
        else c.snan = true;
    } else if (!exponent) {
        if (!fraction) c.zero = true; else c.denormal = true;
    }
    return c;
}

static Classified classify_dp(uint64_t bits)
{
    unsigned exponent = (unsigned)((bits >> 52) & 0x7ffu);
    uint64_t fraction = bits & UINT64_C(0xfffffffffffff);
    Classified c = { .sign = (bits >> 63) & 1u };
    if (exponent == 0x7ffu) {
        if (!fraction) c.infinity = true;
        else if (fraction & UINT64_C(0x8000000000000)) c.qnan = true;
        else c.snan = true;
    } else if (!exponent) {
        if (!fraction) c.zero = true; else c.denormal = true;
    }
    return c;
}

static uint32_t signed_inf_sp(bool sign) { return (sign ? 0x80000000u : 0u) | 0x7f800000u; }
static uint32_t signed_zero_sp(bool sign) { return sign ? 0x80000000u : 0u; }
static uint64_t signed_inf_dp(bool sign)
{
    return (sign ? UINT64_C(0x8000000000000000) : 0) | UINT64_C(0x7ff0000000000000);
}
static uint64_t signed_zero_dp(bool sign)
{
    return sign ? UINT64_C(0x8000000000000000) : 0;
}

CdjC674xApproxResult cdj_c674x_approx(CdjC674xApproxKind kind, uint64_t src2)
{
    bool dp = kind == CDJ_C674X_RCPDP || kind == CDJ_C674X_RSQRDP;
    bool reciprocal = kind == CDJ_C674X_RCPSP || kind == CDJ_C674X_RCPDP;
    CdjC674xApproxResult r = { .value = 0, .status = 0, .pair = dp,
                               .valid = kind <= CDJ_C674X_RSQRDP };
    if (!r.valid) return r;

    Classified c = dp ? classify_dp(src2) : classify_sp((uint32_t)src2);

    /* Note 1 and note 2, identical across all four entries: an SNaN source
     * gives NaN_out with INVAL and NAN2, a QNaN source NaN_out with NAN2. */
    if (c.snan || c.qnan) {
        r.value = dp ? NAN_OUT_DP : NAN_OUT_SP;
        r.status = AP_NAN2 | (c.snan ? AP_INVAL : 0u);
        return r;
    }
    /* RSQR* note 3: a negative, nonzero, nondenormalized source is invalid.
     * Checked before the denormal and zero cases, which it excludes by its own
     * wording. */
    if (!reciprocal && c.sign && !c.zero && !c.denormal) {
        r.value = dp ? NAN_OUT_DP : NAN_OUT_SP;
        r.status = AP_INVAL;
        return r;
    }
    /* Note 3 (RCP*) / note 4 (RSQR*): a denormalized source gives signed
     * infinity.  The status sets differ between the two families and are taken
     * from each entry rather than shared: RCPSP lists DIV0, INFO, OVER, INEX
     * and DEN2; RSQRSP lists DIV0, INEX and DEN2 - no INFO, no OVER. */
    if (c.denormal) {
        r.value = dp ? signed_inf_dp(c.sign) : signed_inf_sp(c.sign);
        r.status = AP_DIV0 | AP_INEX | AP_DEN2 |
                   (reciprocal ? (AP_INFO | AP_OVER) : 0u);
        return r;
    }
    /* Note 4 (RCP*) / note 5 (RSQR*): a signed zero gives signed infinity with
     * DIV0 and INFO. */
    if (c.zero) {
        r.value = dp ? signed_inf_dp(c.sign) : signed_inf_sp(c.sign);
        r.status = AP_DIV0 | AP_INFO;
        return r;
    }
    /* Note 5 (RCP*): signed infinity gives signed 0.  Note 6 (RSQR*) covers
     * only POSITIVE infinity, because a negative infinity was already taken by
     * note 3 above. */
    if (c.infinity) {
        r.value = dp ? signed_zero_dp(c.sign) : signed_zero_sp(c.sign);
        return r;
    }

    /* The normal-number case: correct exponent, mantissa within 2^-8. */
    if (dp) {
        double source = bits_to_double(src2);
        double exact = reciprocal ? 1.0 / source : 1.0 / sqrt(source);
        r.value = round_dp(double_to_bits(exact));
        /* RCPDP note 6 has no underflow clause of its own, but the double
         * reciprocal of a finite normal cannot underflow to zero, so there is
         * nothing to report here beyond the rounding. */
        if (bits_to_double(r.value) != exact) r.status |= AP_INEX;
    } else {
        float source = bits_to_float((uint32_t)src2);
        float exact = reciprocal ? 1.0f / source : (float)(1.0 / sqrt((double)source));
        uint32_t rounded = round_sp(float_to_bits(exact));
        /* RCPSP note 6: "If the result underflows, signed 0 is placed in dst
         * and the INEX and UNDER bits are set.  Underflow occurs when
         * 2^126 < src2 < infinity."  Detected on the computed result rather
         * than by re-deriving the threshold, so the two cannot disagree. */
        if (reciprocal && (rounded & 0x7f800000u) == 0) {
            r.value = signed_zero_sp(c.sign);
            r.status = AP_INEX | AP_UNDER;
            return r;
        }
        r.value = rounded;
        if (bits_to_float(rounded) != exact) r.status |= AP_INEX;
    }
    return r;
}
