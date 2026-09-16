/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed 8-bit (4x8) semantics, SPRUFE8B July 2010.  Semantics only; the
 * decoder, the unit restrictions and the delayed-result queueing stay in
 * cdj_c674x.c.  Every rule below cites the printed page it comes from.
 *
 * Every one of these entries numbers its bytes the same way: "The 8-bit
 * values in each input are numbered from 0 to 3, starting with the
 * least-significant byte, then working towards the most-significant byte"
 * (CMPEQ4, printed page 181), so byte N of a source is bits 8N+7..8N.
 */
#include <stdbool.h>
#include "cdj_c674x_packed8.h"

static uint8_t ubyte(uint32_t word, unsigned n)
{
    return (uint8_t)(word >> (n * 8));
}

static int8_t sbyte(uint32_t word, unsigned n)
{
    return (int8_t)ubyte(word, n);
}

/* Assemble four independently computed bytes; each caller has already
 * reduced its lane to eight bits, so no lane can disturb another. */
static uint32_t pack_bytes(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Printed page 141: "byteN(src1) + byteN(src2) -> byteN(dst)" for N = 0..3,
 * and printed page 140: "No saturation is performed. The carry from one 8-bit
 * add does not affect the add of any other 8-bit add." */
uint32_t cdj_c674x_add4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n)
        out[n] = (uint8_t)(ubyte(src1, n) + ubyte(src2, n));
    return pack_bytes(out);
}

/* Printed page 552: "(byteN(src1) - byteN(src2)) -> byteN(dst)"; printed page
 * 551 repeats that no saturation is performed. */
uint32_t cdj_c674x_sub4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n)
        out[n] = (uint8_t)(ubyte(src1, n) - ubyte(src2, n));
    return pack_bytes(out);
}

/* Printed page 535: "abs(ubyteN(src1) - ubyteN(src2)) -> ubyteN(dst)".  The
 * operands are unsigned (printed page 534, operand types u4/xu4/u4), so the
 * difference of two 0..255 values lies in -255..255 and its absolute value
 * always fits the unsigned 8-bit result; no saturation arises. */
uint32_t cdj_c674x_subabs4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n) {
        uint8_t a = ubyte(src1, n), b = ubyte(src2, n);
        out[n] = a >= b ? (uint8_t)(a - b) : (uint8_t)(b - a);
    }
    return pack_bytes(out);
}

/* Printed page 435: "If the sum is in the range 0 to 2^8 - 1, inclusive, then
 * no saturation is performed and the sum is left unchanged.  If the sum is
 * greater than 2^8 - 1, then the result is set to 2^8 - 1."  Both operands
 * are unsigned, so the sum is never below 0 and only the upper clamp can
 * fire. */
uint32_t cdj_c674x_saddu4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n) {
        unsigned sum = (unsigned)ubyte(src1, n) + ubyte(src2, n);
        out[n] = (uint8_t)(sum > 255 ? 255 : sum);
    }
    return pack_bytes(out);
}

/* Printed page 149: "((ubyteN(src1) + ubyteN(src2) + 1) >> 1) ->
 * ubyteN(dst)".  The sum plus one reaches at most 511, so the shifted result
 * is at most 255 - the entry's "No overflow conditions exist". */
uint32_t cdj_c674x_avgu4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n)
        out[n] = (uint8_t)(((unsigned)ubyte(src1, n) + ubyte(src2, n) + 1) >> 1);
    return pack_bytes(out);
}

/* Printed page 309: "if (ubyteN(src1) >= ubyteN(src2)), ubyteN(src1) ->
 * ubyteN(dst) else ubyteN(src2) -> ubyteN(dst)".  The >= makes the tie
 * explicit, though both arms then yield the same byte. */
uint32_t cdj_c674x_maxu4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n) {
        uint8_t a = ubyte(src1, n), b = ubyte(src2, n);
        out[n] = a >= b ? a : b;
    }
    return pack_bytes(out);
}

/* Printed page 314: "if (ubyteN(src1) <= ubyteN(src2)), ubyteN(src1) ->
 * ubyteN(dst) else ubyteN(src2) -> ubyteN(dst)". */
uint32_t cdj_c674x_minu4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4];
    for (unsigned n = 0; n < 4; ++n) {
        uint8_t a = ubyte(src1, n), b = ubyte(src2, n);
        out[n] = a <= b ? a : b;
    }
    return pack_bytes(out);
}

/* Printed page 182: "if (sbyteN(src1) == sbyteN(src2)), 1 -> dst N else
 * 0 -> dst N", and printed page 181: "The remaining bits of dst are cleared
 * to 0."  The operand types are s4/xs4 (printed page 181), so the comparison
 * is written here on signed bytes exactly as the manual states it; equality
 * of two 8-bit values does not depend on the signedness of the reading. */
uint32_t cdj_c674x_cmpeq4(uint32_t src1, uint32_t src2)
{
    uint32_t result = 0;
    for (unsigned n = 0; n < 4; ++n)
        if (sbyte(src1, n) == sbyte(src2, n)) result |= 1u << n;
    return result;
}

/* Printed page 200: "if (ubyteN(src1) > ubyteN(src2)), 1 -> dst N else
 * 0 -> dst N", with the remaining bits of dst cleared (printed page 199). */
uint32_t cdj_c674x_cmpgtu4(uint32_t src1, uint32_t src2)
{
    uint32_t result = 0;
    for (unsigned n = 0; n < 4; ++n)
        if (ubyte(src1, n) > ubyte(src2, n)) result |= 1u << n;
    return result;
}

/* Printed page 361: "(ubyte0(src1) x ubyte0(src2)) -> lsb16(dst_e);
 * (ubyte1 x ubyte1) -> msb16(dst_e); (ubyte2 x ubyte2) -> lsb16(dst_o);
 * (ubyte3 x ubyte3) -> msb16(dst_o)".  Returned as one 64-bit value with
 * dst_e in bits 31-0, matching the entry's dst_o:dst_e diagram. */
uint64_t cdj_c674x_mpyu4(uint32_t src1, uint32_t src2)
{
    uint64_t result = 0;
    for (unsigned n = 0; n < 4; ++n) {
        uint32_t product = (uint32_t)ubyte(src1, n) * ubyte(src2, n);
        result |= (uint64_t)(product & 0xffffu) << (n * 16);
    }
    return result;
}

/* Printed page 358: "(sbyteN(src1) x ubyteN(src2))" into the same four 16-bit
 * fields.  src1 is s4 and src2 is xu4 (printed page 357), so each product
 * lies in -32640..32385 and is stored as a 16-bit two's-complement field. */
uint64_t cdj_c674x_mpysu4(uint32_t src1, uint32_t src2)
{
    uint64_t result = 0;
    for (unsigned n = 0; n < 4; ++n) {
        int32_t product = (int32_t)sbyte(src1, n) * ubyte(src2, n);
        result |= (uint64_t)((uint32_t)product & 0xffffu) << (n * 16);
    }
    return result;
}

/* Printed page 475: msb16(src1) produces ubyte3(dst), lsb16(src1) ubyte2(dst),
 * msb16(src2) ubyte1(dst) and lsb16(src2) ubyte0(dst); for each, a value above
 * 0000 00FFh gives FFh, a value below 0 gives 0, and otherwise the halfword is
 * truncated to eight bits.  The halfwords are read as signed (operand types
 * s2/xs2, printed page 474).  Printed page 475 also states that this
 * instruction does not affect the SAT bit in CSR. */
static uint8_t spack_byte(uint32_t word, bool high)
{
    int32_t value = (int16_t)(high ? word >> 16 : word);
    return (uint8_t)(value > 255 ? 255 : value < 0 ? 0 : value);
}

uint32_t cdj_c674x_spacku4(uint32_t src1, uint32_t src2)
{
    uint8_t out[4] = {
        spack_byte(src2, false), spack_byte(src2, true),
        spack_byte(src1, false), spack_byte(src1, true),
    };
    return pack_bytes(out);
}
