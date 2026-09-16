/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed 16-bit semantics, SPRUFE8B July 2010.  Semantics only; the decoder,
 * the delay-slot queueing and the delayed CSR.SAT/SSR queueing stay in
 * cdj_c674x.c.  Each rule below quotes the printed page it came from. */
#include "cdj_c674x_packed16.h"

static int32_t msb16(uint32_t word) { return (int16_t)(word >> 16); }
static int32_t lsb16(uint32_t word) { return (int16_t)word; }
static uint32_t pack(uint32_t high, uint32_t low)
{
    return (high & 0xffffu) << 16 | (low & 0xffffu);
}

/* Arithmetic shift right without relying on the implementation-defined >>
 * of a negative signed integer.  count stays below 32 at every call site. */
static uint32_t asr(int32_t value, unsigned count)
{
    return value < 0 ? ~(~(uint32_t)value >> count) : (uint32_t)value >> count;
}

/* SADD2 printed page 425, SSUB2 502 and SPACK2 472 all state the same
 * signed 16-bit clamp: below -2^15 gives -2^15, above 2^15-1 gives 2^15-1. */
static uint32_t sat16(int32_t value)
{
    return (uint32_t)(value > 32767 ? 32767 :
                      value < -32768 ? -32768 : value) & 0xffffu;
}

/* SADDUS2 printed page 433: "If the sum is greater than 2^16 - 1, then the
 * result is set to 2^16 - 1.  If the sum is less than 0, then the result is
 * cleared to 0." */
static uint32_t satu16(int32_t value)
{
    return (uint32_t)(value > 65535 ? 65535 : value < 0 ? 0 : value) & 0xffffu;
}

/* ABS2, printed page 103: "1. If the value is between 0 and 2^15, then
 * value -> dst.  2. If the value is less than 0 and not equal to -2^15, then
 * -value -> dst.  3. If the value is equal to -2^15, then 2^15 - 1 -> dst."
 * Page 103 NOTE: this instruction does not affect the SAT bit in the CSR. */
static uint32_t abs16(int32_t value)
{
    return (uint32_t)(value == -32768 ? 32767 : value < 0 ? -value : value) &
           0xffffu;
}

uint32_t cdj_c674x_abs2(uint32_t src2)
{
    return pack(abs16(msb16(src2)), abs16(lsb16(src2)));
}

/* ADD2, printed page 138: "msb16(src1) + msb16(src2) -> msb16(dst);
 * lsb16(src1) + lsb16(src2) -> lsb16(dst)"; "The carry from the lower half
 * add does not affect the upper half add." */
uint32_t cdj_c674x_add2(uint32_t src1, uint32_t src2)
{
    return pack((uint32_t)(msb16(src1) + msb16(src2)),
                (uint32_t)(lsb16(src1) + lsb16(src2)));
}

/* SUB2, printed page 549: "(lsb16(src1) - lsb16(src2)) -> lsb16(dst);
 * (msb16(src1) - msb16(src2)) -> msb16(dst)"; "Any borrow from the lower-half
 * subtraction does not affect the upper-half subtraction." */
uint32_t cdj_c674x_sub2(uint32_t src1, uint32_t src2)
{
    return pack((uint32_t)(msb16(src1) - msb16(src2)),
                (uint32_t)(lsb16(src1) - lsb16(src2)));
}

/* SADD2, printed page 426: "sat(msb16(src1) + msb16(src2)) -> msb16(dst);
 * sat(lsb16(src1) + lsb16(src2)) -> lsb16(dst)". */
uint32_t cdj_c674x_sadd2(uint32_t src1, uint32_t src2)
{
    return pack(sat16(msb16(src1) + msb16(src2)),
                sat16(lsb16(src1) + lsb16(src2)));
}

/* SSUB2, printed page 503: "sat(msb16(src1) - msb16(src2)) -> msb16(dst);
 * sat(lsb16(src1) - lsb16(src2)) -> lsb16(dst)". */
uint32_t cdj_c674x_ssub2(uint32_t src1, uint32_t src2)
{
    return pack(sat16(msb16(src1) - msb16(src2)),
                sat16(lsb16(src1) - lsb16(src2)));
}

/* SADDUS2, printed page 434: "sat(umsb16(src1) + smsb16(src2)) ->
 * umsb16(dst); sat(ulsb16(src1) + slsb16(src2)) -> ulsb16(dst)".  src1 is
 * unsigned packed 16-bit, src2 is signed packed 16-bit, dst is unsigned.
 * SADDSU2 (printed page 431) is the same instruction with the two source
 * operands written the other way round; the assembler emits this one. */
uint32_t cdj_c674x_saddus2(uint32_t src1, uint32_t src2)
{
    return pack(satu16((int32_t)(uint16_t)(src1 >> 16) + msb16(src2)),
                satu16((int32_t)(uint16_t)src1 + lsb16(src2)));
}

/* MAX2, printed page 307: "if (lsb16(src1) >= lsb16(src2)), lsb16(src1) ->
 * lsb16(dst) else lsb16(src2) -> lsb16(dst)", and likewise for msb16. */
uint32_t cdj_c674x_max2(uint32_t src1, uint32_t src2)
{
    return pack((uint32_t)(msb16(src1) >= msb16(src2) ? msb16(src1)
                                                      : msb16(src2)),
                (uint32_t)(lsb16(src1) >= lsb16(src2) ? lsb16(src1)
                                                      : lsb16(src2)));
}

/* MIN2, printed page 312: the same with <=. */
uint32_t cdj_c674x_min2(uint32_t src1, uint32_t src2)
{
    return pack((uint32_t)(msb16(src1) <= msb16(src2) ? msb16(src1)
                                                      : msb16(src2)),
                (uint32_t)(lsb16(src1) <= lsb16(src2) ? lsb16(src1)
                                                      : lsb16(src2)));
}

/* AVG2, printed page 147: "((lsb16(src1) + lsb16(src2) + 1) >> 1) ->
 * lsb16(dst)", and likewise for msb16.  "No overflow conditions exist": the
 * 17-bit sum is shifted arithmetically back down to 16 bits. */
uint32_t cdj_c674x_avg2(uint32_t src1, uint32_t src2)
{
    return pack(asr(msb16(src1) + msb16(src2) + 1, 1),
                asr(lsb16(src1) + lsb16(src2) + 1, 1));
}

/* SHR2, printed page 453: "The lower 5 bits of src1 are treated as the shift
 * amount.  Bits 5 through 31 of src1 are ignored"; page 454 NOTE: "If the
 * shift amount specified in src1 is in the range 16 to 31, the behavior is
 * identical to a shift value of 15."  The halves are signed and the shifted
 * quantity is sign-extended. */
uint32_t cdj_c674x_shr2(uint32_t src2, uint32_t src1)
{
    /* The NOTE needs no clamp of its own: each half is sign-extended to 32
     * bits first, and an arithmetic shift of a sign-extended 16-bit value by
     * 16 through 31 gives the same 0 or -1 that a shift by 15 does. */
    unsigned count = src1 & 31u;
    return pack(asr(msb16(src2), count), asr(lsb16(src2), count));
}

/* SHRU2, printed page 459: the same shift amount rule with unsigned halves
 * and zero extension.  NOTE: "If the shift amount specified in src1 is in
 * the range of 16 to 31, the dst will be cleared to all zeros" - which is
 * what shifting a 16-bit quantity right by 16 or more already gives. */
uint32_t cdj_c674x_shru2(uint32_t src2, uint32_t src1)
{
    unsigned count = src1 & 31u;
    return pack((uint32_t)(uint16_t)(src2 >> 16) >> count,
                (uint32_t)(uint16_t)src2 >> count);
}

/* CMPEQ2, printed page 179: the lower pair's result goes to bit 0 and the
 * upper pair's to bit 1; "The remaining bits of dst are cleared to 0." */
uint32_t cdj_c674x_cmpeq2(uint32_t src1, uint32_t src2)
{
    return (uint32_t)(msb16(src1) == msb16(src2)) << 1 |
           (uint32_t)(lsb16(src1) == lsb16(src2));
}

/* CMPGT2, printed page 191: signed halves, same bit placement.  CMPLT2
 * (printed page 205) is a pseudo-operation the assembler turns into
 * CMPGT2 src1, src2, dst with the operands exchanged. */
uint32_t cdj_c674x_cmpgt2(uint32_t src1, uint32_t src2)
{
    return (uint32_t)(msb16(src1) > msb16(src2)) << 1 |
           (uint32_t)(lsb16(src1) > lsb16(src2));
}

/* SPACK2, printed page 473: src1 and src2 are full signed 32-bit values,
 * saturated independently to signed 16 bits; src1 lands in msb16(dst) and
 * src2 in lsb16(dst). */
uint32_t cdj_c674x_spack2(uint32_t src1, uint32_t src2)
{
    return pack(sat16((int32_t)src1), sat16((int32_t)src2));
}

/* SSHVL printed page 496 and SSHVR printed page 498.  src1 is a
 * 2s-complement shift value "automatically limited to the range -31 to 31";
 * a left shift saturates to the signed 32-bit range and a right shift
 * sign-extends and discards the bits shifted past bit 0.  SSHVR is SSHVL
 * with the sign of src1 reversed, which is exactly what the two Execution
 * blocks spell out. */
CdjC674xPacked16Sat cdj_c674x_sshv(uint32_t src2, uint32_t src1, bool right)
{
    int64_t count = right ? -(int64_t)(int32_t)src1 : (int32_t)src1;
    if (count > 31) count = 31;
    if (count < -31) count = -31;
    if (count < 0)
        return (CdjC674xPacked16Sat){asr((int32_t)src2, (unsigned)-count),
                                     false};
    /* Multiply rather than shift: a left shift of a negative value is
     * undefined, and 2^31 x 2^31 still fits an int64_t exactly. */
    int64_t shifted = (int64_t)(int32_t)src2 * (INT64_C(1) << count);
    bool saturated = shifted > INT32_MAX || shifted < INT32_MIN;
    if (!saturated) return (CdjC674xPacked16Sat){(uint32_t)shifted, false};
    return (CdjC674xPacked16Sat){
        shifted > 0 ? 0x7fffffffu : 0x80000000u, true};
}
