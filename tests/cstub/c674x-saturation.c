/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TI SPRUFE8B: SADD pp422-424, SSHL pp493-494, SSUB pp499-500;
 * SMPY p461, SMPYH p463, SMPYHL p464, SMPYLH p466, SMPY2 p468;
 * CSR Table 2-9, SSR 2.9.13, compact Figures D-4/E-5/F-22/F-25/F-26.
 * Covers scalar32 and signed40 SADD/SSUB, RPACK2, and the saturating 16x16
 * multiply family, not the packed 2x16 / 4x8 saturating arithmetic. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "cdj_c674x.h"

static void issue(CdjC674x *c, uint32_t word, bool compact, uint32_t header)
{
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = c->pc,
                          .compact = compact, .header = header}},
        .count = 1, .next_pc = c->pc + (compact ? 2 : 4),
    };
    if (!cdj_c674x_execute(c, &p, NULL, NULL, NULL)) {
        fprintf(stderr, "saturation test fault word=%08x: %s\n", word, c->fault);
        assert(false);
    }
}

static uint32_t clamp(int64_t n, bool *saturated)
{
    *saturated = n < INT32_MIN || n > INT32_MAX;
    return (uint32_t)(n < INT32_MIN ? INT32_MIN : n > INT32_MAX ? INT32_MAX : n);
}

static void delayed_flags(CdjC674x *c, bool saturated, unsigned unit_bit)
{
    assert(!(c->control[1] & 0x200));
    assert(!c->control[21]);
    issue(c, 0, false, 0);
    assert((c->control[1] & 0x200) == (saturated ? 0x200u : 0));
    assert(c->control[21] == (saturated ? unit_bit : 0));
    issue(c, 0, false, 0);
    assert(c->control[21] == (saturated ? unit_bit : 0));
}

static const uint32_t values[] = {
    0, 1, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x40000000u,
    0xc0000000u, 15, 0xfffffff0u,
};

static void rpack2(void)
{
    /* These operand/result pairs also appear in ghidra-c6000's independent
     * RPACK2 fixtures. The first follows the manual's execution rule; its
     * printed example has an inconsistent upper halfword. */
    static const struct {
        uint32_t src1, src2, result;
        unsigned side, cross;
        bool saturated;
    } rows[] = {
        {0xfedcba98u, 0x12345678u, 0xfdb92468u, 0, 0, false},
        {0x87654321u, 0x12345678u, 0x80002468u, 1, 1, true},
        {0x40000000u, 0xc0000000u, 0x7fff8000u, 0, 0, true},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const unsigned side = rows[i].side, cross = rows[i].cross;
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1] = rows[i].src1;
        c.r[side ^ cross][2] = rows[i].src2;
        uint32_t word = 0x10000ef0u | 3u << 23 | 2u << 18 |
                        1u << 13 | cross << 12 | side << 1;
        issue(&c, word, false, 0);
        assert(c.r[side][3] == rows[i].result);
        delayed_flags(&c, rows[i].saturated, 1u << (2 + side));
    }
}

static void full_arithmetic(void)
{
    /* op, immediate src1, subtract, src1 cross (SSUB reverse route), S unit */
    static const struct { unsigned opcode; bool imm, sub, reverse, sunit; } forms[] = {
        {0x278, false, false, false, false}, /* SADD .L reg */
        {0x258, true, false, false, false},  /* SADD .L scst5 */
        {0x820, false, false, false, true},  /* SADD .S reg */
        {0x1f8, false, true, false, false},  /* SSUB .L local-cross */
        {0x3f8, false, true, true, false},   /* SSUB .L cross-local */
        {0x1d8, true, true, false, false},   /* SSUB .L scst5 */
    };
    for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        unsigned field = forms[f].imm ? (values[i] & 31) : 1;
        int64_t a = forms[f].imm ? (int)(field ^ 16) - 16 : (int32_t)values[i];
        int64_t b = (int32_t)values[j];
        c.r[side ^ (forms[f].reverse ? cross : 0)][1] = values[i];
        c.r[side ^ (forms[f].reverse ? 0 : cross)][2] = values[j];
        c.r[side][3] = 0x11223344;
        c.r[1][0] = enabled;
        uint32_t word = (1u << 29) | 3u << 23 | 2u << 18 | field << 13 |
                        cross << 12 | forms[f].opcode | side << 1;
        bool saturated;
        uint32_t expected = clamp(forms[f].sub ? a - b : a + b, &saturated);
        issue(&c, word, false, 0);
        assert(c.r[side][3] == (enabled ? expected : 0x11223344u));
        delayed_flags(&c, enabled && saturated,
                      1u << ((forms[f].sunit ? 2 : 0) + side));
    }
}

static uint32_t shift_expected(uint32_t source, unsigned count, bool *sat)
{
    if (count >= 32) {
        *sat = source != 0;
        return !source ? 0 : source >> 31 ? 0x80000000u : 0x7fffffffu;
    }
    return clamp((int64_t)(int32_t)source * (INT64_C(1) << count), sat);
}

static void full_shifts(void)
{
    static const unsigned counts[] = {0, 1, 15, 30, 31, 32, 33, 63, 64, 95, 96, UINT_MAX};
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned imm = 0; imm < 2; ++imm)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(counts) / sizeof(counts[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1] = counts[j]; c.r[side ^ cross][2] = values[i];
        c.r[side][3] = 0x11223344; c.r[1][0] = enabled;
        unsigned count = counts[j] & (imm ? 31 : 63);
        uint32_t word = 1u << 29 | 3u << 23 | 2u << 18 |
                        (imm ? count : 1u) << 13 | cross << 12 |
                        (imm ? 0x8a0 : 0x8e0) | side << 1;
        bool saturated;
        uint32_t expected = shift_expected(values[i], count, &saturated);
        issue(&c, word, false, 0);
        assert(c.r[side][3] == (enabled ? expected : 0x11223344u));
        delayed_flags(&c, enabled && saturated, 1u << (2 + side));
    }
}

static void long_arithmetic(void)
{
    const int64_t limit = INT64_C(1) << 39;
    const int64_t longs[] = {0, 1, -1, limit - 1, limit - 16, -limit, -limit + 16,
                            INT64_C(0x7fffffff), -INT64_C(0x80000000)};
    const unsigned ops[] = {0x618, 0x638, 0x598};
    for (unsigned f = 0; f < 3; ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross <= (f == 1 ? 1u : 0u); ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(longs) / sizeof(longs[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        unsigned field = f == 1 ? 1u : values[i] & 31;
        int64_t left = f == 1 ? (int32_t)values[i] : (int)(field ^ 16) - 16;
        c.r[side ^ cross][1] = values[i];
        c.r[side][2] = (uint32_t)longs[j];
        c.r[side][3] = 0xa5a5a500u | (((uint64_t)longs[j] >> 32) & 255);
        c.r[side][4] = 0x11223344; c.r[side][5] = 0x55667788;
        c.r[1][0] = enabled;
        int64_t result = f == 2 ? left - longs[j] : left + longs[j];
        bool sat = result >= limit || result < -limit;
        if (result >= limit) result = limit - 1;
        if (result < -limit) result = -limit;
        uint32_t word = 1u << 29 | 4u << 23 | 2u << 18 | field << 13 |
                        cross << 12 | ops[f] | side << 1;
        issue(&c, word, false, 0);
        assert(c.r[side][4] == (enabled ? (uint32_t)result : 0x11223344u));
        assert(c.r[side][5] == (enabled ? ((uint64_t)result >> 32) & 255 : 0x55667788u));
        delayed_flags(&c, enabled && sat, 1u << side);
    }
    for (unsigned f = 0; f < 3; ++f)
    for (unsigned bad = 0; bad < 3; ++bad) {
        if (f == 1 && bad == 2) continue; /* cross sint src1 is valid */
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.word = (bad == 0 ? 5u : 4u) << 23 |
                (bad == 1 ? 3u : 2u) << 18 | 1u << 13 |
                (bad == 2 ? 1u << 12 : 0) | ops[f], .pc = 0x1000}}};
        assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
        assert(c.fault && !c.cycles && !c.packets && !c.load_count);
        assert(!c.control[21] && !(c.control[1] & 0x200));
    }
}

static void compact_arithmetic(void)
{
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned rs = 0; rs <= 16; rs += 16)
    for (unsigned sunit = 0; sunit < 2; ++sunit)
    for (unsigned sub = 0; sub < 2; ++sub)
    for (unsigned sat = 0; sat < 2; ++sat)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1 + rs] = values[i]; c.r[side ^ cross][2 + rs] = values[j];
        uint32_t word = 1u << 13 | cross << 12 | sub << 11 | 2u << 7 |
                        3u << 4 | (sunit ? 0xa : 0) | side;
        uint32_t header = 0xe0000000u | (rs ? 1u << 19 : 0) | sat << 14;
        bool saturated = false;
        uint32_t expected = sub ? values[i] - values[j] : values[i] + values[j];
        if (sat && !(sunit && sub))
            expected = clamp(sub ? (int64_t)(int32_t)values[i] - (int32_t)values[j] :
                                  (int64_t)(int32_t)values[i] + (int32_t)values[j], &saturated);
        issue(&c, word, true, header);
        assert(c.r[side][3 + rs] == expected);
        delayed_flags(&c, saturated, 1u << (2 * sunit + side));
    }
}

static void compact_shifts(void)
{
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned rs = 0; rs <= 16; rs += 16)
    for (unsigned imm = 0; imm < 2; ++imm)
    for (unsigned sat = 0; sat < 2; ++sat)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned count = 0; count < (imm ? 32u : 64u); ++count) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1 + rs] = count; c.r[side][2 + rs] = values[i];
        uint32_t word = imm ? (count & 7) << 13 | (count >> 3) << 11 | 0x442 :
                              1u << 13 | 3u << 11 | 0x462;
        word |= 2u << 7 | side;
        bool saturated = false;
        uint32_t expected = count < 32 ? values[i] >> count : 0;
        if (!imm || sat) expected = shift_expected(values[i], count, &saturated);
        issue(&c, word, true, 0xe0000000u | (rs ? 1u << 19 : 0) | sat << 14);
        assert(c.r[side][2 + rs] == expected);
        delayed_flags(&c, saturated, 1u << (2 + side));
    }
}

static uint32_t mvc_write(unsigned control, unsigned source)
{ return control << 23 | source << 18 | 0x3a2; }
static uint32_t mvc_read(unsigned control, unsigned destination)
{ return destination << 23 | control << 18 | 0x3e2; }

static void status_interactions(void)
{
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = c.r[1][1] = 0x7fffffff;
    c.r[0][2] = c.r[1][2] = 1;
    CdjC674xPacket p = {.count = 2, .next_pc = 0x1008,
        .instructions = {{.word = 3u << 23 | 2u << 18 | 1u << 13 | 0x278, .pc = 0x1000},
                         {.word = 3u << 23 | 2u << 18 | 1u << 13 | 0x822, .pc = 0x1004}}};
    assert(cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(!c.control[21] && !(c.control[1] & 0x200));
    issue(&c, mvc_read(21, 5), false, 0);
    assert(c.r[1][5] == 0 && c.control[21] == 9 && (c.control[1] & 0x200));
    issue(&c, mvc_read(21, 5), false, 0); assert(c.r[1][5] == 9);
    issue(&c, mvc_read(1, 5), false, 0); assert(c.r[1][5] & 0x200);
    c.r[0][1] = c.r[0][2] = 0;
    issue(&c, p.instructions[0].word, false, 0);
    issue(&c, 0, false, 0);
    assert(c.control[21] == 9 && (c.control[1] & 0x200));
    c.r[1][6] = 0; issue(&c, mvc_write(21, 6), false, 0);
    assert(!c.control[21] && (c.control[1] & 0x200));
    c.r[1][6] = UINT_MAX; issue(&c, mvc_write(21, 6), false, 0);
    assert(c.control[21] == 0x3f);
    c.r[1][6] = 0; issue(&c, mvc_write(1, 6), false, 0);
    assert(!(c.control[1] & 0x200) && c.control[21] == 0x3f);
    c.r[1][6] = 0x200; issue(&c, mvc_write(1, 6), false, 0);
    assert(!(c.control[1] & 0x200)); /* MVC cannot set SAT. */
    for (unsigned control = 1; control <= 21; control += 20) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][1] = 0x7fffffff; c.r[0][2] = 1;
        issue(&c, p.instructions[0].word, false, 0);
        c.r[1][6] = 0; issue(&c, mvc_write(control, 6), false, 0);
        assert(c.control[21] == 1 && (c.control[1] & 0x200));
    }
    /* A rejected packet must not leak its saturating register/flag effects. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x7fffffff; c.r[0][2] = 1;
    p.instructions[1] = p.instructions[0];
    assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(c.fault && !c.load_count && !c.cycles && !c.r[0][3]);
    assert(!c.control[21] && !(c.control[1] & 0x200));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x7fffffff; c.r[0][2] = 1;
    c.load_count = 40;
    for (unsigned i = 0; i < 40; ++i)
        c.loads[i] = (CdjC674xLoad){.due = 100, .size = CDJ_C674X_DELAYED_SAT, .address = 1};
    p.count = 1;
    assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(c.fault && c.load_count == 40 && !c.cycles && !c.r[0][3]);
    assert(!c.control[21] && !(c.control[1] & 0x200));
}

/*
 * Saturating 16x16 .M multiplies: SMPY (printed page 461), SMPYH (463),
 * SMPYHL (464), SMPYLH (466) and SMPY2 (468).
 *
 * Every opfield below came out of TI's assembler, not out of this decoder:
 * asm6x -mv6740 assembles "SMPY .M1 A1,A2,A3" to 01882d00h, SMPYH to
 * 01882100h, SMPYHL to 01882500h and SMPYLH to 01882900h, so (word >> 7) & 31
 * is 1a, 02, 0a and 12.  "SMPY2 .M1 A1,A2,A5:A4" is 02082070h, which is the
 * Figure E-1 compound format with (word >> 6) & 31 == 1.  The non-saturating
 * MPY, MPYH, MPYHL and MPYLH in the same listing are 01882c80h, 01882080h,
 * 01882480h and 01882880h: opfields 19, 01, 09 and 11.
 */
static uint32_t mpy_word(unsigned op, unsigned side, unsigned cross,
                         unsigned dst, unsigned src1, unsigned src2,
                         bool compound)
{
    return 1u << 29 | dst << 23 | src2 << 18 | src1 << 13 | cross << 12 |
           op << (compound ? 6 : 7) | (compound ? 0x30u : 0u) | side << 1;
}

static void advance(CdjC674x *c, unsigned packets)
{
    while (packets--) issue(c, 0, false, 0);
}

static void mpy_saturating(void)
{
    /*
     * src1 = 8001FFFFh and src2 = 7FFF8000h give a different product for
     * every halfword selection, so a swapped high/low field or a lost sign
     * extension cannot pass unnoticed.  Hand arithmetic:
     *
     *   lsb16(src1) = FFFFh =      -1     msb16(src1) = 8001h = -32767
     *   lsb16(src2) = 8000h = -32768      msb16(src2) = 7FFFh =  32767
     *
     *   SMPY    (-1 x -32768) << 1     =       65536 -> 0001 0000h
     *   SMPYH   (-32767 x 32767) << 1  = -2147352578 -> 8001 FFFEh
     *   SMPYHL  (-32767 x -32768) << 1 =  2147418112 -> 7FFF 0000h
     *   SMPYLH  (-1 x 32767) << 1      =      -65534 -> FFFF 0002h
     *
     * None of the four is 8000 0000h, so none saturates (SPRUFE8B p461).
     * The non-saturating opfields in the same row are the same products
     * without the left shift:
     *
     *   MPY     -1 x -32768     =      32768 -> 0000 8000h
     *   MPYH    -32767 x 32767  = -1073676289 -> C000 FFFFh
     *   MPYHL   -32767 x -32768 =  1073709056 -> 3FFF 8000h
     *   MPYLH   -1 x 32767      =     -32767 -> FFFF 8001h
     */
    static const struct { unsigned op; uint32_t expected; } forms[] = {
        {0x1a, 0x00010000u}, {0x02, 0x8001fffeu},
        {0x0a, 0x7fff0000u}, {0x12, 0xffff0002u},
        {0x19, 0x00008000u}, {0x01, 0xc000ffffu},
        {0x09, 0x3fff8000u}, {0x11, 0xffff8001u},
    };
    for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1] = 0x8001ffffu;
        c.r[side ^ cross][2] = 0x7fff8000u;
        c.r[side][3] = 0xdeadbeefu;
        c.r[1][0] = enabled;
        /* E2 write, one delay slot: dst is still stale in the issue packet. */
        issue(&c, mpy_word(forms[f].op, side, cross, 3, 1, 2, false), false, 0);
        assert(c.r[side][3] == 0xdeadbeefu);
        assert(c.load_count == (enabled ? 1u : 0u));
        issue(&c, 0, false, 0);
        assert(c.r[side][3] == (enabled ? forms[f].expected : 0xdeadbeefu));
        assert(!c.load_count);
        delayed_flags(&c, false, 0);
    }

    /*
     * The one saturating case, 8000h x 8000h: (-32768 x -32768) << 1 is
     * 8000 0000h, which SPRUFE8B p461 replaces with 7FFF FFFFh and which sets
     * CSR.SAT and SSR.M1/M2 one cycle after dst is written.  The halfwords not
     * selected are deliberately junk so a wrong selection yields no saturation
     * and fails the assertion.
     */
    static const struct { unsigned op; uint32_t src1, src2; } saturating[] = {
        {0x1a, 0x1234ffffu, 0x5678ffffu},  /* SMPY   lsb x lsb: FFFFh is -1 */
        {0x1a, 0x12348000u, 0x56788000u},  /* SMPY   lsb x lsb: saturates */
        {0x02, 0x80001234u, 0x80005678u},  /* SMPYH  msb x msb: saturates */
        {0x0a, 0x80001234u, 0x12348000u},  /* SMPYHL msb x lsb: saturates */
        {0x12, 0x00008000u, 0x80000000u},  /* SMPYLH lsb x msb: TI p467 */
    };
    for (unsigned f = 0; f < sizeof(saturating) / sizeof(saturating[0]); ++f)
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        bool sat = f != 0;               /* row 0 is the -1 x -1 control */
        c.r[side][1] = saturating[f].src1;
        c.r[side][2] = saturating[f].src2;
        c.r[side][3] = 0xdeadbeefu;
        c.r[1][0] = 1;
        issue(&c, mpy_word(saturating[f].op, side, 0, 3, 1, 2, false), false, 0);
        issue(&c, 0, false, 0);
        assert(c.r[side][3] == (sat ? 0x7fffffffu : 2u));
        delayed_flags(&c, sat, 1u << (4 + side));
    }

    /*
     * SMPY2 writes dst_e = sat(lsb x lsb << 1) and dst_o = sat(msb x msb << 1)
     * in E4, three delay slots (SPRUFE8B p468-469).  The first two rows are
     * TI's own worked examples 1 and 2; the last two isolate each half
     * saturating on its own, which the p468 note says still sets CSR.SAT.
     *
     *   A5 = 6A321193h (27186, 4499), A6 = B1746CA4h (-20108, 27812):
     *     4499 x 27812   =  125126188, << 1 =  250252376 -> 0EEA 8C58h
     *     27186 x -20108 = -546656088, << 1 = -1093312176 -> BED5 6150h
     *   B2 = 12343497h (4660, 13463), B5 = 21FF50A7h (8703, 20647):
     *     13463 x 20647 = 277970561, << 1 = 555941122 -> 2122 FD02h
     *     4660 x 8703   =  40555980, << 1 =  81111960 -> 04D5 AB98h
     *   80000001h x 80000001h: 1 x 1 << 1 = 2, and msb -32768 x -32768
     *     saturates to 7FFF FFFFh.
     *   00018000h x 00018000h: msb 1 x 1 << 1 = 2, and lsb -32768 x -32768
     *     saturates to 7FFF FFFFh.
     */
    static const struct { uint32_t src1, src2, even, odd; bool sat; } packed[] = {
        {0x6a321193u, 0xb1746ca4u, 0x0eea8c58u, 0xbed56150u, false},
        {0x12343497u, 0x21ff50a7u, 0x2122fd02u, 0x04d5ab98u, false},
        {0x80000001u, 0x80000001u, 0x00000002u, 0x7fffffffu, true},
        {0x00018000u, 0x00018000u, 0x7fffffffu, 0x00000002u, true},
    };
    for (unsigned f = 0; f < sizeof(packed) / sizeof(packed[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1] = packed[f].src1;
        c.r[side ^ cross][2] = packed[f].src2;
        c.r[side][4] = 0x11223344u; c.r[side][5] = 0x55667788u;
        c.r[1][0] = enabled;
        issue(&c, mpy_word(1, side, cross, 4, 1, 2, true), false, 0);
        advance(&c, 2);
        assert(c.r[side][4] == 0x11223344u && c.r[side][5] == 0x55667788u);
        issue(&c, 0, false, 0);
        assert(c.r[side][4] == (enabled ? packed[f].even : 0x11223344u));
        assert(c.r[side][5] == (enabled ? packed[f].odd : 0x55667788u));
        delayed_flags(&c, enabled && packed[f].sat, 1u << (4 + side));
    }

    /* An odd SMPY2 destination is not a legal register pair, and rejecting it
     * must leave no delayed result, no flag and no consumed cycle. */
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.word = mpy_word(1, side, 0, 5, 1, 2, true),
                              .pc = 0x1000}}};
        assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
        assert(c.fault && !c.cycles && !c.packets && !c.load_count);
        assert(!c.control[21] && !(c.control[1] & 0x200));
    }
}

/*
 * Figure E-5 "M3" (printed page 744) is the only compact .M format.  Its
 * fields are src1 at bits 15-13, x at 12, dst at 11-10, src2 at 9-7 and op at
 * 6-5, over the constant 1111b at bits 4-1 and s at bit 0; the header SAT bit
 * picks MPY/MPYH/MPYLH/MPYHL or SMPY/SMPYH/SMPYLH/SMPYHL.  dst is an even
 * register - [A0, A2, A4, A6] with RS = 0 and [A16, A18, A20, A22] with RS = 1
 * - while src1 and src2 are ordinary three-bit compact fields.
 *
 * The word/register mapping below was read back from TI's disassembler: with a
 * .fphead whose SAT bit is clear, dis6x -i reports 291eh as "MPY.M1 A1,A2,A4"
 * and 299eh as "MPY.M1 A1,A3,A4"; with SAT set the same words are SMPY.  All
 * 4,096 words of the format were checked that way under both SAT and both RS.
 */
static void compact_multiplies(void)
{
    /* Same operands and hand arithmetic as mpy_saturating() above. */
    static const struct { unsigned op; uint32_t plain, saturating; } ops[] = {
        {0, 0x00008000u, 0x00010000u},   /* MPY   / SMPY   */
        {1, 0xc000ffffu, 0x8001fffeu},   /* MPYH  / SMPYH  */
        {2, 0xffff8001u, 0xffff0002u},   /* MPYLH / SMPYLH */
        {3, 0x3fff8000u, 0x7fff0000u},   /* MPYHL / SMPYHL */
    };
    for (unsigned f = 0; f < sizeof(ops) / sizeof(ops[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned rs = 0; rs <= 16; rs += 16)
    for (unsigned sat = 0; sat < 2; ++sat)
    for (unsigned d = 0; d < 4; ++d) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1 + rs] = 0x8001ffffu;
        c.r[side ^ cross][3 + rs] = 0x7fff8000u;
        uint32_t stale = 0xdeadbeefu;
        c.r[side][2 * d + rs] = stale;
        uint32_t word = 1u << 13 | cross << 12 | d << 10 | 3u << 7 |
                        ops[f].op << 5 | 0x1eu | side;
        uint32_t header = 0xe0000000u | (rs ? 1u << 19 : 0) | sat << 14;
        /* dst is always even and the sources are the odd registers 1 and 3,
         * so no destination can alias a source and mask a decode error. */
        issue(&c, word, true, header);
        assert(c.r[side][2 * d + rs] == stale);
        issue(&c, 0, false, 0);
        assert(c.r[side][2 * d + rs] ==
               (sat ? ops[f].saturating : ops[f].plain));
        delayed_flags(&c, false, 0);
    }

    /* The compact form saturates exactly where the full-width one does, and
     * only when the header SAT bit selects the saturating half of Figure E-5.
     * 8000h x 8000h: see SPRUFE8B p461 and the MPY entry on p316 - without
     * saturation the unshifted product is 4000 0000h. */
    for (unsigned sat = 0; sat < 2; ++sat)
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1] = 0x00008000u; c.r[side][3] = 0x00008000u;
        uint32_t word = 1u << 13 | 2u << 10 | 3u << 7 | 0x1eu | side;
        issue(&c, word, true, 0xe0000000u | (uint32_t)sat << 14);
        issue(&c, 0, false, 0);
        assert(c.r[side][4] == (sat ? 0x7fffffffu : 0x40000000u));
        delayed_flags(&c, sat != 0, 1u << (4 + side));
    }
}

int main(void)
{
    full_arithmetic(); full_shifts(); long_arithmetic(); rpack2();
    compact_arithmetic(); compact_shifts();
    mpy_saturating(); compact_multiplies();
    status_interactions();
    puts("C674x scalar saturation and delayed CSR/SSR tests passed");
    return 0;
}
