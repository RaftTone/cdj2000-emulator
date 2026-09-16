/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 32-bit multiply, Galois-field multiply and 40-bit long .L/.S semantics,
 * SPRUFE8B July 2010.  Every rule below cites the printed page it came from.
 * Semantics only: no CdjC674x access, so the execute loop's transactional copy
 * is unreachable from here (see cdj_c674x_mpy.c for the same pattern).
 */
#include "cdj_c674x_mpy32.h"

int64_t cdj_c674x_sx40(uint64_t raw)
{
    /* Bit 39 is the sign of a 40-bit long (printed page 447: "only the bottom
     * 40 bits of the register pair are shifted.  The upper 24 bits of the
     * register pair are unused"). */
    uint64_t value = raw & CDJ_C674X_LONG40_MASK;
    return (int64_t)(value ^ (UINT64_C(1) << 39)) - (INT64_C(1) << 39);
}

uint64_t cdj_c674x_mpyi(uint32_t src1, uint32_t src2)
{
    /* MPYI, printed page 334: "The src1 operand is multiplied by the src2
     * operand.  The lower 32 bits of the result are placed in dst", with both
     * operands sint/xsint.  MPYID, printed page 335, is the same product with
     * "lsb32(src1 x src2) -> dst_l; msb32(src1 x src2) -> dst_h". */
    return (uint64_t)((int64_t)(int32_t)src1 * (int64_t)(int32_t)src2);
}

uint64_t cdj_c674x_mpy2(uint32_t src1, uint32_t src2)
{
    /* MPY2, printed page 366: "lsb16(src1) x lsb16(src2) -> dst_e;
     * msb16(src1) x msb16(src2) -> dst_o", both halves signed (printed page
     * 365: "treated as signed, packed 16-bit quantities").  Each product fits
     * in 32 bits: |a| <= 2^15 and |b| <= 2^15 give |a*b| <= 2^30. */
    uint32_t low = (uint32_t)((int32_t)(int16_t)src1 * (int32_t)(int16_t)src2);
    uint32_t high = (uint32_t)((int32_t)(int16_t)(src1 >> 16) *
                               (int32_t)(int16_t)(src2 >> 16));
    return (uint64_t)high << 32 | low;
}

/* One Galois-field multiply over GF(2^m), m = size + 1, with the generator
 * polynomial x^m + poly (printed page 272: "the unsigned, 8-bit value from
 * src1 is Galois field multiplied (gmpy) with the unsigned, 8-bit value from
 * src2", and printed page 32: "all Galois multiplies for fields of the form
 * GF(2^m), where m can range between 1 and 8 using any generator
 * polynomial").  A Galois-field multiply is the carry-less product of the two
 * operands reduced modulo the generator polynomial; both steps are bitwise
 * over GF(2), so addition is exclusive-or. */
static unsigned gmpy_byte(unsigned a, unsigned b, unsigned poly, unsigned size)
{
    unsigned m = size + 1, mask = (1u << m) - 1u;
    unsigned generator = (1u << m) | (poly & mask);
    unsigned product = 0;

    for (unsigned i = 0; i < 8; ++i)
        if ((b >> i) & 1u) product ^= a << i;
    /* Carry-less 8x8 product occupies bits 14-0; reduce from the top down. */
    for (unsigned i = 15; i >= m; --i)
        if ((product >> i) & 1u) product ^= generator << (i - m);
    return product & mask;
}

uint32_t cdj_c674x_gmpy4(uint32_t src1, uint32_t src2, unsigned poly,
                         unsigned size)
{
    /* Printed page 274: byte n of src1 against byte n of src2 into byte n of
     * dst, for n = 0..3. */
    uint32_t result = 0;
    for (unsigned n = 0; n < 4; ++n) {
        unsigned shift = n * 8;
        result |= (uint32_t)(gmpy_byte((src1 >> shift) & 0xffu,
                                       (src2 >> shift) & 0xffu, poly, size) &
                             0xffu) << shift;
    }
    return result;
}

uint32_t cdj_c674x_sat40(uint64_t src2, bool *saturated)
{
    /* SAT, printed page 437: "if (src2 > (2^31 - 1)), (2^31 - 1) -> dst;
     * else if (src2 < -2^31), -2^31 -> dst; else src2 31..0 -> dst". */
    int64_t value = cdj_c674x_sx40(src2);
    *saturated = value > INT64_C(0x7fffffff) || value < -INT64_C(0x80000000);
    if (value > INT64_C(0x7fffffff)) return 0x7fffffffu;
    if (value < -INT64_C(0x80000000)) return 0x80000000u;
    return (uint32_t)value;
}

uint32_t cdj_c674x_subc(uint32_t src1, uint32_t src2)
{
    /* SUBC, printed page 539: "if (src1 - src2 >= 0), ((src1 - src2) << 1) + 1
     * -> dst; else (src1 << 1) -> dst".  Both operands are uint/xuint, so the
     * sign test is the unsigned comparison src1 >= src2. */
    if (src1 >= src2) return ((src1 - src2) << 1) + 1u;
    return src1 << 1;
}

uint32_t cdj_c674x_abs32(uint32_t src2)
{
    /* ABS, printed page 101, sint form: "1. If src2 > 0, then src2 -> dst.
     * 2. If src2 < 0 and src2 != -2^31, then -src2 -> dst.  3. If
     * src2 = -2^31, then 2^31 - 1 -> dst". */
    if (src2 == 0x80000000u) return 0x7fffffffu;
    return (src2 & 0x80000000u) ? 0u - src2 : src2;
}

uint64_t cdj_c674x_abs40(uint64_t src2)
{
    /* ABS, printed page 101, slong form: the same three cases at 40 bits,
     * with -2^39 saturating to 2^39 - 1. */
    uint64_t value = src2 & CDJ_C674X_LONG40_MASK;
    if (value == UINT64_C(1) << 39) return (UINT64_C(1) << 39) - 1u;
    if (!(value & (UINT64_C(1) << 39))) return value;
    return (UINT64_C(0) - value) & CDJ_C674X_LONG40_MASK;
}

uint64_t cdj_c674x_shift40(uint64_t src2, unsigned count, unsigned operation)
{
    /* SHL printed page 447, SHR 451, SHRU 457: "When a register is used, the
     * six LSBs specify the shift amount and valid values are 0-40. ... If
     * 39 < src1 < 64, src2 is shifted ... by 40.  Only the six LSBs of src1
     * are used by the shifter".  A 40-bit shift of a 40-bit value leaves zero
     * for SHL/SHRU and the replicated sign for SHR, so clamping the count to
     * 40 is the whole rule. */
    uint64_t value = src2 & CDJ_C674X_LONG40_MASK;
    unsigned n = (count & 63u) > 39u ? 40u : (count & 63u);

    if (operation == CDJ_C674X_SHIFT40_LEFT)
        return (value << n) & CDJ_C674X_LONG40_MASK;
    if (operation == CDJ_C674X_SHIFT40_LOGICAL)
        return value >> n;
    /* SHR is sign-extending within the 40-bit field (printed page 451: "The
     * sign-extended result is placed in dst").  Shift unsigned and fill the
     * vacated bits by hand: a right shift of a negative signed value is
     * implementation-defined in C. */
    uint64_t shifted = value >> n;
    if (n && (value & (UINT64_C(1) << 39)))
        shifted |= CDJ_C674X_LONG40_MASK << (40u - n);
    return shifted & CDJ_C674X_LONG40_MASK;
}
