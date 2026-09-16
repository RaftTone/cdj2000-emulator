/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c674x_packbits.h"
/* SPRUFE8B (July 2010) pack/unpack, shuffle and bit-manipulation semantics.
 * Each function names the printed page whose Description and Execution block it
 * implements; the per-instruction Example blocks are transcribed in
 * tests/cstub/c674x-packbits.c rather than re-derived here. */

/* UNPKHU4, printed page 559: "ubyte3(src2) -> ubyte2(dst); 0 -> ubyte3(dst);
 * ubyte2(src2) -> ubyte0(dst); 0 -> ubyte1(dst)".  The src2 bytes are
 * zero-extended, so nothing else reaches dst. */
uint32_t cdj_c674x_unpkhu4(uint32_t src2)
{
    return ((src2 >> 24) & 0xffu) << 16 | ((src2 >> 16) & 0xffu);
}

/* UNPKLU4, printed page 561: "ubyte0(src2) -> ubyte0(dst); 0 -> ubyte1(dst);
 * ubyte1(src2) -> ubyte2(dst); 0 -> ubyte3(dst)". */
uint32_t cdj_c674x_unpklu4(uint32_t src2)
{
    return ((src2 >> 8) & 0xffu) << 16 | (src2 & 0xffu);
}

/* SWAP4, printed page 555: "ubyte0(src2) -> ubyte1(dst); ubyte1(src2) ->
 * ubyte0(dst); ubyte2(src2) -> ubyte3(dst); ubyte3(src2) -> ubyte2(dst)". */
uint32_t cdj_c674x_swap4(uint32_t src2)
{
    return ((src2 >> 16) & 0xffu) << 24 | ((src2 >> 24) & 0xffu) << 16 |
           (src2 & 0xffu) << 8 | ((src2 >> 8) & 0xffu);
}

/* BITR, printed page 163: "bit 0 of the source becomes bit 31 of the result,
 * bit 1 of the source becomes bit 30 of the result, ... and so on". */
uint32_t cdj_c674x_bitr(uint32_t src2)
{
    uint32_t dst = 0;
    for (unsigned i = 0; i < 32; ++i)
        dst |= ((src2 >> i) & 1u) << (31 - i);
    return dst;
}

/* BITC4, printed page 161: "For each of the 8-bit quantities in src2, the count
 * of the number of 1 bits in that value is written to the corresponding
 * position in dst."  A count is at most 8, so no byte ever carries. */
uint32_t cdj_c674x_bitc4(uint32_t src2)
{
    uint32_t dst = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        uint32_t count = 0;
        for (unsigned i = 0; i < 8; ++i)
            count += (src2 >> (byte * 8 + i)) & 1u;
        dst |= count << (byte * 8);
    }
    return dst;
}

/* DEAL, printed page 231: "src2 31,29,27...1 -> dst 31,30,29...16" and
 * "src2 30,28,26...0 -> dst 15,14,13...0". */
uint32_t cdj_c674x_deal(uint32_t src2)
{
    uint32_t dst = 0;
    for (unsigned i = 0; i < 16; ++i) {
        dst |= ((src2 >> (2 * i)) & 1u) << i;
        dst |= ((src2 >> (2 * i + 1)) & 1u) << (16 + i);
    }
    return dst;
}

/* SHFL, printed page 443: "src2 15,14,13...0 -> dst 30,28,26...0" and
 * "src2 31,30,29...16 -> dst 31,29,27...1".  Printed page 231 states DEAL is
 * the exact inverse, which the two entries' Examples confirm as a pair. */
uint32_t cdj_c674x_shfl(uint32_t src2)
{
    uint32_t dst = 0;
    for (unsigned i = 0; i < 16; ++i) {
        dst |= ((src2 >> i) & 1u) << (2 * i);
        dst |= ((src2 >> (16 + i)) & 1u) << (2 * i + 1);
    }
    return dst;
}

/* SHFL3, printed page 445, Execution block verbatim:
 *   inp0 = src2 & FFFFh; inp1 = src1 & FFFFh; inp2 = src1 >> 16 & FFFFh;
 *   for (I = 0; I < 16; I++) {
 *       result |= (inp0 >> I & 1) << (I * 3);
 *       result |= (inp1 >> I & 1) << ((I * 3) + 1);
 *       result |= (inp2 >> I & 1) << ((I * 3) + 2); }
 * The upper halfword of src2 takes no part (the figure's c15-c0). */
uint64_t cdj_c674x_shfl3(uint32_t src1, uint32_t src2)
{
    uint32_t inp0 = src2 & 0xffffu;
    uint32_t inp1 = src1 & 0xffffu;
    uint32_t inp2 = (src1 >> 16) & 0xffffu;
    uint64_t result = 0;
    for (unsigned i = 0; i < 16; ++i) {
        result |= (uint64_t)((inp0 >> i) & 1u) << (i * 3);
        result |= (uint64_t)((inp1 >> i) & 1u) << (i * 3 + 1);
        result |= (uint64_t)((inp2 >> i) & 1u) << (i * 3 + 2);
    }
    return result;
}

/* XPND2, printed page 568: "XPND2(src2 & 1) -> lsb16(dst); XPND2(src2 & 2) ->
 * msb16(dst)", each source bit replicated across its halfword. */
uint32_t cdj_c674x_xpnd2(uint32_t src2)
{
    return ((src2 & 2u) ? 0xffff0000u : 0u) | ((src2 & 1u) ? 0xffffu : 0u);
}

/* XPND4, printed page 570: bits 3-0 of src2 replicated across bytes 3-0. */
uint32_t cdj_c674x_xpnd4(uint32_t src2)
{
    uint32_t dst = 0;
    for (unsigned byte = 0; byte < 4; ++byte)
        if ((src2 >> byte) & 1u) dst |= 0xffu << (byte * 8);
    return dst;
}

/* ROTL, printed page 414: rotate src2 left by src1's five least-significant
 * bits; "Bits 5 through 31 of src1 are ignored and may be non-zero". */
uint32_t cdj_c674x_rotl(uint32_t src2, uint32_t src1)
{
    unsigned n = src1 & 31u;
    return n ? (src2 << n) | (src2 >> (32 - n)) : src2;
}

/* LMBD, printed page 304: "The LSB of the src1 operand determines whether to
 * search for a leftmost 1 or 0 in src2.  The number of bits to the left of the
 * first 1 or 0 ... is placed in dst."  The page's third diagram fixes the
 * not-found result at 32. */
uint32_t cdj_c674x_lmbd(uint32_t src1, uint32_t src2)
{
    uint32_t wanted = src1 & 1u;
    for (unsigned i = 32; i-- > 0;)
        if (((src2 >> i) & 1u) == wanted) return 31u - i;
    return 32u;
}

/* NORM, printed page 390: "The number of redundant sign bits of src2 is placed
 * in dst."  The page's four diagrams pin 0, 3, 30 and 31, so an all-sign word
 * gives 31 for the 32-bit form. */
uint32_t cdj_c674x_norm32(uint32_t src2)
{
    uint32_t sign = (src2 >> 31) & 1u, count = 0;
    for (unsigned i = 31; i-- > 0;) {
        if (((src2 >> i) & 1u) != sign) break;
        ++count;
    }
    return count;
}

/* NORM, printed page 390, slong form: the same count over the 40-bit value, so
 * an all-sign long gives 39.  Example 3 on printed page 391 pins 36 for the
 * 40-bit value 00 0000 0007h. */
uint32_t cdj_c674x_norm40(uint64_t src2)
{
    uint32_t sign = (uint32_t)((src2 >> 39) & 1u), count = 0;
    for (unsigned i = 39; i-- > 0;) {
        if ((uint32_t)((src2 >> i) & 1u) != sign) break;
        ++count;
    }
    return count;
}

/* SHLMB, printed page 449: "ubyte2(src2) -> ubyte3(dst); ubyte1(src2) ->
 * ubyte2(dst); ubyte0(src2) -> ubyte1(dst); ubyte3(src1) -> ubyte0(dst)". */
uint32_t cdj_c674x_shlmb(uint32_t src1, uint32_t src2)
{
    return (src2 << 8) | ((src1 >> 24) & 0xffu);
}

/* SHRMB, printed page 455: "ubyte0(src1) -> ubyte3(dst); ubyte3(src2) ->
 * ubyte2(dst); ubyte2(src2) -> ubyte1(dst); ubyte1(src2) -> ubyte0(dst)". */
uint32_t cdj_c674x_shrmb(uint32_t src1, uint32_t src2)
{
    return (src1 & 0xffu) << 24 | (src2 >> 8);
}

/* DPACK2, printed page 254: "lsb16(src1) -> msb16(dst_e); lsb16(src2) ->
 * lsb16(dst_e); msb16(src1) -> msb16(dst_o); msb16(src2) -> lsb16(dst_o)". */
uint64_t cdj_c674x_dpack2(uint32_t src1, uint32_t src2)
{
    uint32_t even = (src1 & 0xffffu) << 16 | (src2 & 0xffffu);
    uint32_t odd = (src1 & 0xffff0000u) | (src2 >> 16);
    return (uint64_t)odd << 32 | even;
}

/* DPACKX2, printed page 256: "lsb16(src1) -> msb16(dst_e); msb16(src2) ->
 * lsb16(dst_e); msb16(src1) -> lsb16(dst_o); lsb16(src2) -> msb16(dst_o)". */
uint64_t cdj_c674x_dpackx2(uint32_t src1, uint32_t src2)
{
    uint32_t even = (src1 & 0xffffu) << 16 | (src2 >> 16);
    uint32_t odd = (src2 & 0xffffu) << 16 | (src1 >> 16);
    return (uint64_t)odd << 32 | even;
}

/* ---- the nonconditional .L dual-result forms ----------------------------- */

static int32_t sat32_signed(int64_t v, bool *saturated)
{
    if (v > INT64_C(0x7fffffff))  { *saturated = true; return (int32_t)0x7fffffff; }
    if (v < -INT64_C(0x80000000)) { *saturated = true; return (int32_t)0x80000000; }
    return (int32_t)v;
}

static uint32_t sat16_halves(int32_t hi, int32_t lo)
{
    if (hi > 32767) hi = 32767; else if (hi < -32768) hi = -32768;
    if (lo > 32767) lo = 32767; else if (lo < -32768) lo = -32768;
    return (uint32_t)((hi & 0xffff) << 16 | (lo & 0xffff));
}

CdjC674xAddsubResult cdj_c674x_addsub(unsigned opfield, uint32_t src1,
                                      uint32_t src2)
{
    CdjC674xAddsubResult r = { .value = 0, .saturated = false, .valid = true };
    int32_t s1h = (int16_t)(src1 >> 16), s1l = (int16_t)(src1 & 0xffffu);
    int32_t s2h = (int16_t)(src2 >> 16), s2l = (int16_t)(src2 & 0xffffu);
    uint32_t add, sub;

    switch (opfield) {
    case CDJ_C674X_ADDSUB:
        /* Printed page 132: src1 + src2 -> dst_o, src1 - src2 -> dst_e, with no
         * sat() printed on either line. */
        add = src1 + src2;
        sub = src1 - src2;
        break;
    case CDJ_C674X_ADDSUB2:
        /* Printed page 133: the same pair done on each halfword separately, so
         * a halfword carry does not cross into the other half. */
        add = (uint32_t)(((s1h + s2h) & 0xffff) << 16 | ((s1l + s2l) & 0xffff));
        sub = (uint32_t)(((s1h - s2h) & 0xffff) << 16 | ((s1l - s2l) & 0xffff));
        break;
    case CDJ_C674X_SADDSUB:
        /* Printed page 427: sat(src1 + src2) -> dst_o, sat(src1 - src2) ->
         * dst_e, over the full 32 bits.  This is the form that reports
         * saturation in CSR.SAT and SSR. */
        add = (uint32_t)sat32_signed((int64_t)(int32_t)src1 + (int32_t)src2,
                                     &r.saturated);
        sub = (uint32_t)sat32_signed((int64_t)(int32_t)src1 - (int32_t)src2,
                                     &r.saturated);
        break;
    case CDJ_C674X_SADDSUB2:
        /* Printed page 429: saturating on each halfword.  Its own note exempts
         * it from CSR.SAT and the SSR L1/L2 bits, so `saturated` stays false
         * here however the halves clamp - that is the documented behaviour, not
         * an omission. */
        add = sat16_halves(s1h + s2h, s1l + s2l);
        sub = sat16_halves(s1h - s2h, s1l - s2l);
        break;
    default:
        r.valid = false;
        return r;
    }
    r.value = (uint64_t)add << 32 | sub;   /* dst_o = add, dst_e = sub */
    return r;
}
