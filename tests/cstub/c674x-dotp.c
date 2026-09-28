/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Packed .M-unit dot products: DOTP2 (SPRUFE8B printed pages 235-237),
 * DOTPN2 (238-239), DOTPNRSU2 (240-241), DOTPNRUS2 (242-243), DOTPRSU2
 * (244-246), DOTPRUS2 (247-248), DOTPSU4 (249-250), DOTPUS4 (251), DOTPU4
 * (252-253).
 *
 * Every expected register value below is transcribed from the manual's own
 * "Before instruction / N cycles after instruction" example blocks, in the
 * manual's hex, except where a comment says otherwise.  There is no execution
 * oracle for this core, so nothing here was produced by running the emulator.
 *
 * Every opfield was read out of TI's assembler, not out of this decoder.
 * asm6x -mv6740 assembles, on side A with src1 = A4, src2 = A6, dst = A5:
 *
 *   DOTP2     .M1 A4,A6,A5     02988330h    DOTP2 .M1 A4,A6,A9:A8  041882F0h
 *   DOTPN2    .M1 A4,A6,A5     02988270h
 *   DOTPNRSU2 .M1 A4,A6,A5     029881F0h    DOTPNRUS2 .M1 A6,A4,A5 029881F0h
 *   DOTPRSU2  .M1 A4,A6,A5     02988370h    DOTPRUS2  .M1 A6,A4,A5 02988370h
 *   DOTPSU4   .M1 A4,A6,A5     029880B0h    DOTPUS4   .M1 A6,A4,A5 029880B0h
 *   DOTPU4    .M1 A4,A6,A5     029881B0h
 *
 * so (word >> 6) & 31 is 0c, 0b, 09, 07, 0d, 02 and 06 respectively, over the
 * Figure E-1 compound low bits 0x30.  The NRUS2/RUS2/US4 pseudo-operations
 * assemble to the identical word with the two source fields exchanged, which
 * is what printed pages 242, 247 and 251 say they are.
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "cdj_c674x.h"

static void issue(CdjC674x *c, uint32_t word)
{
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = c->pc}},
        .count = 1, .next_pc = c->pc + 4,
    };
    if (!cdj_c674x_execute(c, &p, NULL, NULL, NULL)) {
        fprintf(stderr, "dotp test fault word=%08x: %s\n", word, c->fault);
        assert(false);
    }
}

/* Advance n further single-cycle packets, i.e. run out n delay slots.  A word
 * of 0 is NOP 1 (SPRUFE8B printed page 388). */
static void cycles(CdjC674x *c, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) issue(c, 0);
}

/* Execute one word on a fresh CPU and report whether it was REJECTED, with the
 * fault text, so refusals can be asserted by name. */
static bool rejects(uint32_t word, const char **fault)
{
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = c.pc}},
        .count = 1, .next_pc = c.pc + 4,
    };
    bool ok = cdj_c674x_execute(&c, &p, NULL, NULL, NULL);
    if (fault) *fault = c.fault;
    return !ok;
}

/* Figure E-1 compound .M word, predicated on [B0] so the disabled path is
 * exercised too: creg = 001, z = 0. */
static uint32_t dotp_word(unsigned op, unsigned side, unsigned cross,
                          unsigned dst, unsigned src1, unsigned src2)
{
    return 1u << 29 | dst << 23 | src2 << 18 | src1 << 13 | cross << 12 |
           op << 6 | 0x30u | side << 1;
}

/*
 * The 32-bit-dst forms.  Each row is one worked example, transcribed.
 *
 *  op 0c DOTP2      p236 ex1  A5 6A321193h A6 B1746CA4h -> A8 E6DFF6D4h
 *        DOTP2      p237 ex3  B2 12343497h B5 21FF50A7h -> B8 12FC544Dh
 *  op 09 DOTPN2     p239 ex1  A5 3629274Ah A6 325C8036h -> A8 1E442F20h
 *        DOTPN2     p239 ex2  B2 3FF65010h B5 B1C30244h -> B8 EBBE6A22h
 *  op 07 DOTPNRSU2  p241 ex1  A5 3629274Ah A6 325C8036h -> A8 FFFFF6FAh
 *        DOTPNRSU2  p241 ex2  B2 3FF65010h B5 B1C30244h -> B8 00002BB4h
 *  op 0d DOTPRSU2   p245 ex1  A5 3629274Ah A6 325C8036h -> A8 00001E55h
 *        DOTPRSU2   p246 ex2  B2 B1C30244h B5 3FF65010h -> B8 FFFFED29h
 *  op 02 DOTPSU4    p250 ex1  A5 6A321193h A6 B1746CA4h -> A8 0000214Ah
 *        DOTPSU4    p250 ex2  B2 3FF65010h B5 C3560244h -> B8 00003181h
 *  op 06 DOTPU4     p253      A5 6A321193h A6 B1746CA4h -> A8 0000C54Ah
 *
 * DOTP2 example 1 doubles as the DOTPNRUS2/DOTPRUS2/DOTPUS4 coverage: those
 * pseudo-operations are the same three words with src1 and src2 exchanged in
 * the syntax only, so the encodings above already are them.
 */
static const struct { unsigned op; uint32_t src1, src2, expected; } examples[] = {
    {0x0c, 0x6a321193u, 0xb1746ca4u, 0xe6dff6d4u},
    {0x0c, 0x12343497u, 0x21ff50a7u, 0x12fc544du},
    {0x09, 0x3629274au, 0x325c8036u, 0x1e442f20u},
    {0x09, 0x3ff65010u, 0xb1c30244u, 0xebbe6a22u},
    {0x07, 0x3629274au, 0x325c8036u, 0xfffff6fau},
    {0x07, 0x3ff65010u, 0xb1c30244u, 0x00002bb4u},
    {0x0d, 0x3629274au, 0x325c8036u, 0x00001e55u},
    {0x0d, 0xb1c30244u, 0x3ff65010u, 0xffffed29u},
    {0x02, 0x6a321193u, 0xb1746ca4u, 0x0000214au},
    {0x02, 0x3ff65010u, 0xc3560244u, 0x00003181u},
    {0x06, 0x6a321193u, 0xb1746ca4u, 0x0000c54au},
    /* Printed page 236's NOTE, transcribed: "In the overflow case, where all
     * four halfwords in src1 and src2 are 8000h, the value 8000 0000h is
     * written into the 32-bit dst". */
    {0x0c, 0x80008000u, 0x80008000u, 0x80000000u},
};

static void scalar_examples(void)
{
    for (unsigned f = 0; f < sizeof(examples) / sizeof(examples[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = examples[f].src1;
        c.r[side ^ cross][6] = examples[f].src2;
        c.r[side][5] = 0xdeadbeefu;
        c.r[1][0] = enabled;
        issue(&c, dotp_word(examples[f].op, side, cross, 5, 4, 6));
        /* Four-cycle, three delay slots: dst is stale for three more packets
         * and appears in the fourth ("4 cycles after instruction"). */
        for (unsigned n = 0; n < 3; ++n) {
            assert(c.r[side][5] == 0xdeadbeefu);
            assert(c.load_count == (enabled ? 1u : 0u));
            issue(&c, 0);
        }
        assert(c.r[side][5] == (enabled ? examples[f].expected : 0xdeadbeefu));
        assert(!c.load_count);
        /* None of these touches CSR.SAT or SSR. */
        assert(!(c.control[1] & 0x200) && !c.control[21]);
    }
}

/*
 * DOTP2 dst_o:dst_e, printed page 237.
 *
 *   ex2  A5 6A321193h A6 B1746CA4h -> A9:A8 FFFF FFFFh E6DF F6D4h
 *   ex4  B2 12343497h B5 21FF50A7h -> B9:B8 0000 0000h 12FC 544Dh
 *
 * and printed page 236's NOTE for the overflow case: "0000 0000 8000 0000h is
 * written into the 64-bit dst".
 */
static const struct { uint32_t src1, src2, odd, even; } pairs[] = {
    {0x6a321193u, 0xb1746ca4u, 0xffffffffu, 0xe6dff6d4u},
    {0x12343497u, 0x21ff50a7u, 0x00000000u, 0x12fc544du},
    {0x80008000u, 0x80008000u, 0x00000000u, 0x80000000u},
};

static void pair_examples(void)
{
    for (unsigned f = 0; f < sizeof(pairs) / sizeof(pairs[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = pairs[f].src1;
        c.r[side ^ cross][6] = pairs[f].src2;
        c.r[side][8] = 0x11223344u; c.r[side][9] = 0x55667788u;
        c.r[1][0] = enabled;
        issue(&c, dotp_word(0x0b, side, cross, 8, 4, 6));
        for (unsigned n = 0; n < 3; ++n) {
            assert(c.r[side][8] == 0x11223344u && c.r[side][9] == 0x55667788u);
            issue(&c, 0);
        }
        assert(c.r[side][8] == (enabled ? pairs[f].even : 0x11223344u));
        assert(c.r[side][9] == (enabled ? pairs[f].odd : 0x55667788u));
    }

    /* An odd dst_o:dst_e base is not a legal register pair.  Rejecting it must
     * leave no delayed result and no consumed cycle. */
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.word = dotp_word(0x0b, side, 0, 9, 4, 6),
                              .pc = 0x1000}}};
        assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
        assert(c.fault && !c.cycles && !c.packets && !c.load_count);
    }
}

/*
 * The rounded forms halt instead of inventing a value where SPRUFE8B declares
 * the result undefined.  Both cases are the manual's own example 3:
 *
 *   DOTPNRSU2 .M2 B12,B23,B11  p241  7FFF8000h, FFFFFFFFh -> "Overflow
 *                                    occurs; result undefined"
 *   DOTPRSU2  .M2 B12,B23,B11  p246  7FFF7FFFh, FFFFFFFFh -> same
 *
 * Printed page 244 fixes the boundary: overflow is avoided while the sum of
 * the two products plus the rounding term stays inside [-2^31, 2^31 - 1].
 * The control rows below sit just inside it and must still execute; their
 * expected values are DERIVED from the page 245 pseudocode, not transcribed,
 * because the manual gives no worked example at the boundary:
 *
 *   src1 7FFF0000h (msb 32767, lsb 0), src2 FFFE0000h (umsb 65534, ulsb 0)
 *     32767 x 65534 = 2147352578; + 0 + 8000h = 2147385346, which is
 *     inside 2^31 - 1, and 2147385346 >> 16 = 32766 -> 0000 7FFEh
 *   the same operands with op 07 subtract the zero lsb product, so the value
 *   and the in-range verdict are identical.
 */
static void undefined_intermediate(void)
{
    static const struct { unsigned op; uint32_t src1, src2; bool halts;
                          uint32_t expected; } rows[] = {
        {0x07, 0x7fff8000u, 0xffffffffu, true,  0},
        {0x0d, 0x7fff7fffu, 0xffffffffu, true,  0},
        {0x07, 0x7fff0000u, 0xfffe0000u, false, 0x00007ffeu},
        {0x0d, 0x7fff0000u, 0xfffe0000u, false, 0x00007ffeu},
    };
    for (unsigned f = 0; f < sizeof(rows) / sizeof(rows[0]); ++f)
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = rows[f].src1;
        c.r[side][6] = rows[f].src2;
        c.r[side][5] = 0xdeadbeefu;
        c.r[1][0] = 1;
        uint32_t word = dotp_word(rows[f].op, side, 0, 5, 4, 6);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.word = word, .pc = 0x1000}}};
        if (rows[f].halts) {
            assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
            assert(c.fault && !c.cycles && !c.packets && !c.load_count);
            assert(c.r[side][5] == 0xdeadbeefu);
        } else {
            issue(&c, word);
            for (unsigned n = 0; n < 3; ++n) issue(&c, 0);
            assert(c.r[side][5] == rows[f].expected);
        }
    }

    /* A predicated-off instruction reads no operands, so the undefined case
     * must not halt the packet when the predicate is false. */
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = 0x7fff8000u; c.r[side][6] = 0xffffffffu;
        c.r[side][5] = 0xdeadbeefu; c.r[1][0] = 0;
        issue(&c, dotp_word(0x07, side, 0, 5, 4, 6));
        for (unsigned n = 0; n < 3; ++n) issue(&c, 0);
        assert(c.r[side][5] == 0xdeadbeefu && !c.load_count);
    }
}

/* The exact words TI's assembler produced, replayed through the decoder so a
 * change in the opfield extraction cannot go unnoticed.  A4 = src1, A6 = src2,
 * A5 = dst; the pseudo-operations share three of these words. */
static void assembler_words(void)
{
    static const struct { uint32_t word; uint32_t src1, src2, expected; } words[] = {
        {0x02988330u, 0x6a321193u, 0xb1746ca4u, 0xe6dff6d4u}, /* DOTP2     */
        {0x02988270u, 0x3629274au, 0x325c8036u, 0x1e442f20u}, /* DOTPN2    */
        {0x029881f0u, 0x3629274au, 0x325c8036u, 0xfffff6fau}, /* DOTPNRSU2 */
        {0x02988370u, 0x3629274au, 0x325c8036u, 0x00001e55u}, /* DOTPRSU2  */
        {0x029880b0u, 0x6a321193u, 0xb1746ca4u, 0x0000214au}, /* DOTPSU4   */
        {0x029881b0u, 0x6a321193u, 0xb1746ca4u, 0x0000c54au}, /* DOTPU4    */
    };
    for (unsigned f = 0; f < sizeof(words) / sizeof(words[0]); ++f) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = words[f].src1; c.r[0][6] = words[f].src2;
        c.r[0][5] = 0xdeadbeefu; c.r[1][0] = 1;
        issue(&c, words[f].word);
        for (unsigned n = 0; n < 3; ++n) issue(&c, 0);
        assert(c.r[0][5] == words[f].expected);
    }
    /* DOTP2 .M1 A4,A6,A9:A8 = 041882F0h, printed page 237 example 2's value. */
    {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = 0x6a321193u; c.r[0][6] = 0xb1746ca4u;
        c.r[0][8] = 0x11223344u; c.r[0][9] = 0x55667788u; c.r[1][0] = 1;
        issue(&c, 0x041882f0u);
        for (unsigned n = 0; n < 3; ++n) issue(&c, 0);
        assert(c.r[0][8] == 0xe6dff6d4u && c.r[0][9] == 0xffffffffu);
    }
}

/* ---- the nonconditional .M group, Figure E-3 (printed page 743) ----------
 *
 * CMPY, CMPYR1, DDOTP4, DDOTPH2 and DDOTPL2 through the whole core, so the
 * Figure E-3 classification, the arm-table route, the register-pair shapes and
 * the four-cycle latency are all under test - not just the arithmetic.
 *
 * Every expected value is TRANSCRIBED from the instruction's own Example
 * block; each case names its printed page and the arithmetic the manual prints
 * beside it. */
static void nonconditional_m_group(void)
{
    /* Figure E-3 word: 0001 in bits 31-28, dst 27-23, src2 22-18, src1 17-13,
     * x 12, 0 in bit 11, opfield 10-6, 1100 in bits 5-2, s 1, p 0. */
    #define E3(dst, src2, src1, cross, op, side) \
        (0x10000000u | (uint32_t)(dst) << 23 | (uint32_t)(src2) << 18 | \
         (uint32_t)(src1) << 13 | (uint32_t)(cross) << 12 | \
         (uint32_t)(op) << 6 | 0x30u | (uint32_t)(side) << 1)

    /* CMPY .M1 A0,A1,A3:A2, printed page 216 Example 1.  A0 = 0008 0004h and
     * A1 = 0009 0002h give A2 = 0000 0034h ((4 x 9) + (8 x 2) = 52) and
     * A3 = 0000 0040h ((8 x 9) - (4 x 2) = 64) four cycles after. */
    {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][0] = 0x00080004u; c.r[0][1] = 0x00090002u;
        c.r[0][2] = c.r[0][3] = 0xdeadbeefu;
        issue(&c, E3(2, 1, 0, 0, 0x0a, 0));
        cycles(&c, 2);
        assert(c.r[0][2] == 0xdeadbeefu);       /* still in flight */
        cycles(&c, 1);
        assert(c.r[0][2] == 0x00000034u && c.r[0][3] == 0x00000040u);
    }

    /* CMPYR1 .M1 A0,A1,A2, printed page 219 Example 1.  A0 = 0800 0400h and
     * A1 = 0900 0200h give A2 = 0080 0068h.  This case is also what settles
     * the manual's tmp_e/tmp_o typo - see cdj_c674x_dotp.c. */
    {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][0] = 0x08000400u; c.r[0][1] = 0x09000200u;
        issue(&c, E3(2, 1, 0, 0, 0x0c, 0));
        cycles(&c, 3);
        assert(c.r[0][2] == 0x00800068u);
    }

    /* DDOTP4 .M1 A4,A5,A9:A8, printed page 222 Example 1.  A4 = 0005 0003h
     * and A5 = 0102 0304h give A8 = 0000 001Bh ((5 x 3) + (3 x 4) = 27) and
     * A9 = 0000 000Bh ((5 x 1) + (3 x 2) = 11). */
    {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = 0x00050003u; c.r[0][5] = 0x01020304u;
        issue(&c, E3(8, 5, 4, 0, 0x18, 0));
        cycles(&c, 3);
        assert(c.r[0][8] == 0x0000001Bu && c.r[0][9] == 0x0000000Bu);
    }

    /* DDOTPH2 .M1 A5:A4,A6,A9:A8 and DDOTPL2 .M1 A5:A4,A6,A9:A8, printed
     * pages 224 and 228 Example 1.  Both read src1_e = A4 = 0005 0003h,
     * src1_o = A5 = 0002 0004h and src2 = A6 = 0007 0001h.  DDOTPH2 gives
     * A8 = 0000 0021h, A9 = 0000 0012h; DDOTPL2 gives A8 = 0000 0026h,
     * A9 = 0000 0021h.  (DDOTPL2's printed arithmetic annotations are
     * DDOTPH2's, copied; its hex values are the authority and are what is
     * transcribed here.) */
    {
        static const struct { unsigned op; uint32_t e, o; } cases[] = {
            { 0x17, 0x00000021u, 0x00000012u },   /* DDOTPH2 */
            { 0x16, 0x00000026u, 0x00000021u },   /* DDOTPL2 */
        };
        for (unsigned i = 0; i < 2; ++i) {
            CdjC674x c; cdj_c674x_reset(&c, 0x1000);
            c.r[0][4] = 0x00050003u; c.r[0][5] = 0x00020004u;
            c.r[0][6] = 0x00070001u;
            issue(&c, E3(8, 6, 4, 0, cases[i].op, 0));
            cycles(&c, 3);
            assert(c.r[0][8] == cases[i].e && c.r[0][9] == cases[i].o);
        }
    }

    /* A src1 pair must be even, and a dst pair must be even. */
    {
        const char *fault;
        assert(rejects(E3(8, 6, 5, 0, 0x17, 0), &fault) && fault);  /* odd src1 */
        assert(rejects(E3(9, 6, 4, 0, 0x17, 0), &fault) && fault);  /* odd dst  */
    }

    /* SMPY32 .M1 A0,A1,A2, printed page 470 Example 1: A0 = 8765 4321h and
     * A1 = 1234 5678h give A2 = EED8 ED1Ah four cycles after. */
    {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][0] = 0x87654321u; c.r[0][1] = 0x12345678u;
        issue(&c, E3(2, 1, 0, 0, 0x19, 0));
        cycles(&c, 3);
        assert(c.r[0][2] == 0xeed8ed1au);
    }

    /* MPY2IR .M2 B2,B5,B9:B8, printed page 367 Example 1: B2 = 8000 8001h and
     * B5 = 8000 0000h give B8 = 7FFF 0000h and B9 = 7FFF FFFFh.  B9 is the
     * manual's own explicit saturating branch, msb16(src1) = 8000h with
     * src2 = 8000 0000h, and B8 is the ordinary rounded path - so one example
     * covers both sides of that branch. */
    {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[1][2] = 0x80008001u; c.r[1][5] = 0x80000000u;
        issue(&c, E3(8, 5, 2, 0, 0x0f, 1));
        cycles(&c, 3);
        assert(c.r[1][8] == 0x7fff0000u && c.r[1][9] == 0x7fffffffu);
    }

    /* GMPY and XORMPY now use the same E-3 shape; their arithmetic and
     * polynomial selection are covered in c674x-mpy32.c. */
    #undef E3
}

int main(void)
{
    scalar_examples();
    pair_examples();
    undefined_intermediate();
    assembler_words();
    nonconditional_m_group();
    return 0;
}
