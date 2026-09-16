/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 32-bit multiply, Galois-field multiply and 40-bit long .L/.S semantics.
 *
 * TI SPRUFE8B July 2010, printed pages: MPYI 334, MPYID 335-336, MPY2 365-366,
 * GMPY4 272-274, DMV 234, SAT 437-439, SUBC 539-540, ABS 101-102,
 * CMPEQ 177-178, CMPGT 188-190, CMPGTU 197-198, CMPLT 202-204,
 * CMPLTU 211-212, SHL 447-448, SHR 451-452, SHRU 457-458, B NRP 157-158,
 * BPOS 170-171.
 *
 * EVIDENCE.  Every "manual example" case below transcribes one of that page's
 * own Example blocks literally: the before-instruction register hex, the
 * "N cycles after instruction" latency and the after hex, with no
 * re-derivation.  Cases marked DERIVED are computed here from the page's
 * Execution pseudocode because the manual prints no example for that form; each
 * says which rule it applies.  Nothing here was produced by running this
 * emulator.
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
        fprintf(stderr, "mpy32 test fault word=%08x: %s\n", word, c->fault);
        assert(false);
    }
}

/* Advance n further single-cycle execute packets, i.e. run out n delay slots.
 * A word of 0 is NOP 1 (SPRUFE8B printed page 388). */
static void cycles(CdjC674x *c, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) issue(c, 0);
}

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

/* Assemble one predicable 32-bit word: dst/src2/src1 fields, cross bit and the
 * bits 11-0 opcode tail, with creg = 0 / z = 0 (unconditional "always"). */
static uint32_t word_of(unsigned dst, unsigned src2, unsigned src1,
                        unsigned cross, uint32_t tail, unsigned side)
{
    return (uint32_t)dst << 23 | (uint32_t)src2 << 18 |
           (uint32_t)src1 << 13 | (uint32_t)cross << 12 | tail |
           (uint32_t)side << 1;
}

/* ---- MPYI, printed page 334 -------------------------------------------- */
static void mpyi(void)
{
    /* Manual example, printed page 334: MPYI .M1X A1,B2,A3 with A1 =
     * 0034 5678h and B2 = 0011 2765h leaves A3 = CBCA 6558h 9 cycles after
     * the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x00345678u;
    c.r[1][2] = 0x00112765u;
    c.r[0][3] = 0x11223344u;
    issue(&c, word_of(3, 2, 1, 1, 0x200, 0));
    for (unsigned i = 1; i < 9; ++i) {
        assert(c.r[0][3] == 0x11223344u);   /* still in flight */
        cycles(&c, 1);
    }
    assert(c.r[0][3] == 0xcbca6558u);
    assert(c.r[0][1] == 0x00345678u && c.r[1][2] == 0x00112765u);

    /* Predication off leaves dst alone (Execution: "else nop"). */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][1] = 0x00345678u;
    d.r[0][2] = 0x00112765u;
    d.r[0][3] = 0x11223344u;
    d.r[0][0] = 0;                          /* creg 001 = A0, z = 0 */
    issue(&d, (1u << 29) | word_of(3, 2, 1, 0, 0x200, 0));
    cycles(&d, 12);
    assert(d.r[0][3] == 0x11223344u);
}

/* ---- MPYID, printed pages 335-336 --------------------------------------- */
static void mpyid(void)
{
    /* Manual example, printed page 336: MPYID .M1 A1,A2,A5:A4.  The before
     * block names A1 = 0034 5678h and "B2" = 0011 2765h while the instruction
     * reads A2 - the register name in the example's second row is a misprint,
     * the hex is the src2 value - and 10 cycles after the instruction
     * A5:A4 = 0000 0381h CBCA 6558h.  The pipeline table on printed page 335
     * writes dst_l in E9 and dst_h in E10, which this checks separately. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x00345678u;
    c.r[0][2] = 0x00112765u;
    c.r[0][4] = 0x11223344u;
    c.r[0][5] = 0x55667788u;
    issue(&c, word_of(4, 2, 1, 0, 0x400, 0));
    cycles(&c, 7);                           /* 8 cycles after instruction */
    assert(c.r[0][4] == 0x11223344u && c.r[0][5] == 0x55667788u);
    cycles(&c, 1);                           /* 9: dst_l only */
    assert(c.r[0][4] == 0xcbca6558u);
    assert(c.r[0][5] == 0x55667788u);
    cycles(&c, 1);                           /* 10: dst_h */
    assert(c.r[0][5] == 0x00000381u);
    assert(c.r[0][4] == 0xcbca6558u);

    /* An odd destination is not a register pair. */
    const char *fault;
    assert(rejects(word_of(5, 2, 1, 0, 0x400, 0), &fault));
    assert(fault);

    /* The cst5 src1 forms stay fail-closed: printed pages 334 and 335 type
     * that field "cst5", which Table 3-2 (printed page 68) does not define as
     * signed or unsigned. */
    assert(rejects(word_of(3, 2, 1, 0, 0x300, 0), &fault));
    assert(rejects(word_of(4, 2, 1, 0, 0x600, 0), &fault));
}

/* ---- MPY2, printed pages 365-366 ---------------------------------------- */
static void mpy2(void)
{
    /* Manual example 1, printed page 366: MPY2 .M1 A5,A6, A9:A8 with
     * A5 = 6A32 1193h and A6 = B174 6CA4h gives A9:A8 = DF6A B0A8h
     * 0775 462Ch 4 cycles after the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][5] = 0x6a321193u;
    c.r[0][6] = 0xb1746ca4u;
    issue(&c, word_of(8, 6, 5, 0, 0x030, 0));
    cycles(&c, 2);
    assert(c.r[0][8] == 0 && c.r[0][9] == 0);
    cycles(&c, 1);
    assert(c.r[0][8] == 0x0775462cu);
    assert(c.r[0][9] == 0xdf6ab0a8u);

    /* Manual example 2, printed page 366: MPY2 .M2 B2, B5, B9:B8 with
     * B2 = 1234 3497h and B5 = 21FF 50A7h gives B9:B8 = 026A D5CCh
     * 1091 7E81h 4 cycles after the instruction. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[1][2] = 0x12343497u;
    d.r[1][5] = 0x21ff50a7u;
    issue(&d, word_of(8, 5, 2, 0, 0x030, 1));
    cycles(&d, 3);
    assert(d.r[1][8] == 0x10917e81u);
    assert(d.r[1][9] == 0x026ad5ccu);
}

/* ---- GMPY4, printed pages 272-274 --------------------------------------- */
static void gmpy4(void)
{
    /* Manual example 1, printed page 274: GMPY4 .M1 A5,A6,A7 with
     * polynomial = 0x1d, A5 = 45 23 00 01h and A6 = 57 34 00 01h gives
     * A7 = 72 92 00 01h 4 cycles after the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][5] = 0x45230001u;
    c.r[0][6] = 0x57340001u;
    c.r[0][7] = 0x11223344u;
    issue(&c, word_of(7, 6, 5, 0, 0x470, 0));
    cycles(&c, 2);
    assert(c.r[0][7] == 0x11223344u);
    cycles(&c, 1);
    assert(c.r[0][7] == 0x72920001u);

    /* Manual example 2, printed page 274: field size is 0x7,
     * A5 = FF FE 02 1Fh and A6 = FF FE 02 01h give A7 = E2 E3 04 1Fh. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][5] = 0xfffe021fu;
    d.r[0][6] = 0xfffe0201u;
    issue(&d, word_of(7, 6, 5, 0, 0x470, 0));
    cycles(&d, 3);
    assert(d.r[0][7] == 0xe2e3041fu);

    /* Commutative, as printed page 272 states. */
    CdjC674x e; cdj_c674x_reset(&e, 0x1000);
    e.r[0][5] = 0xfffe0201u;
    e.r[0][6] = 0xfffe021fu;
    issue(&e, word_of(7, 6, 5, 0, 0x470, 0));
    cycles(&e, 3);
    assert(e.r[0][7] == 0xe2e3041fu);
}

/* ---- DMV, printed page 234 ---------------------------------------------- */
static void dmv(void)
{
    /* Manual example 1, printed page 234: DMV .S1 A0,A1,A3:A2 with
     * A0 = 8765 4321h and A1 = 1234 5678h gives A2 = 1234 5678h and
     * A3 = 8765 4321h 1 cycle after the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][0] = 0x87654321u;
    c.r[0][1] = 0x12345678u;
    issue(&c, word_of(2, 1, 0, 0, 0xef0, 0));
    assert(c.r[0][2] == 0x12345678u);
    assert(c.r[0][3] == 0x87654321u);

    /* Manual example 2, printed page 234: DMV .S2X B0,A1,B3:B2 with
     * B0 = 0007 0009h and A1 = 1234 5678h gives B2 = 1234 5678h and
     * B3 = 0007 0009h.  src2 crosses, src1 stays local. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[1][0] = 0x00070009u;
    d.r[0][1] = 0x12345678u;
    issue(&d, word_of(2, 1, 0, 1, 0xef0, 1));
    assert(d.r[1][2] == 0x12345678u);
    assert(d.r[1][3] == 0x00070009u);

    const char *fault;
    assert(rejects(word_of(3, 1, 0, 0, 0xef0, 0), &fault));
}

/* ---- SAT, printed pages 437-439 ----------------------------------------- */
static void sat(void)
{
    /* Manual examples 1-3, printed pages 438-439, all SAT .L2 B1:B0,B5.
     * Each lists B5 one cycle after the instruction and CSR/SSR two cycles
     * after: CSR 0001 0300h "Saturated" with SSR 0000 0002h when the clamp
     * fires, CSR 0001 0100h "Not saturated" with SSR 0000 0000h when it does
     * not.  CSR bit 9 is SAT and SSR bit 1 is L2. */
    static const struct { uint32_t high, low, dst; bool saturated; } cases[] = {
        {0x0000001fu, 0x3413539au, 0x7fffffffu, true},   /* Example 1 */
        {0x00000000u, 0xa1907321u, 0x7fffffffu, true},   /* Example 2 */
        {0x000000ffu, 0xa1907321u, 0xa1907321u, false},  /* Example 3 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[1][0] = cases[i].low;
        c.r[1][1] = cases[i].high;
        c.control[1] &= ~0x200u;
        c.control[21] = 0;
        issue(&c, word_of(5, 0, 0, 0, 0x818, 1));
        assert(c.r[1][5] == cases[i].dst);
        assert(!(c.control[1] & 0x200u));   /* 1 cycle after: not yet */
        assert(!c.control[21]);
        cycles(&c, 1);                      /* 2 cycles after */
        assert(!!(c.control[1] & 0x200u) == cases[i].saturated);
        assert(c.control[21] == (cases[i].saturated ? 2u : 0u));
    }

    /* DERIVED from the Execution pseudocode on printed page 437, "else if
     * (src2 < -2^31), -2^31 -> dst": the most negative 40-bit value clamps to
     * 8000 0000h.  The manual prints no negative-overflow example. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][0] = 0x00000000u;
    d.r[0][1] = 0x00000080u;                /* 40-bit 80 0000 0000h = -2^39 */
    issue(&d, word_of(5, 0, 0, 0, 0x818, 0));
    assert(d.r[0][5] == 0x80000000u);
    cycles(&d, 1);
    assert(d.control[1] & 0x200u);
    assert(d.control[21] == 1u);            /* SSR.L1 */

    const char *fault;
    assert(rejects(word_of(5, 1, 0, 0, 0x818, 0), &fault));   /* odd pair */
    assert(rejects(word_of(5, 0, 0, 1, 0x818, 0), &fault));   /* cross long */
}

/* ---- SUBC, printed pages 539-540 ---------------------------------------- */
static void subc(void)
{
    /* Manual examples 1 and 2, printed page 540, both SUBC .L1 A0,A1,A0.
     * Example 1: A0 = 0000 125Ah and A1 = 0000 1F12h give A0 = 9396 decimal
     * one cycle after the instruction (the example's hex prints a stray extra
     * digit, "0000 024B4h"; 9396 is 0000 24B4h).  Example 2: A0 = 0002 1A31h
     * and A1 = 0001 F63Fh give A0 = 0000 47E5h. */
    static const struct { uint32_t src1, src2, dst; } cases[] = {
        {0x0000125au, 0x00001f12u, 0x000024b4u},
        {0x00021a31u, 0x0001f63fu, 0x000047e5u},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[0][0] = cases[i].src1;
        c.r[0][1] = cases[i].src2;
        issue(&c, word_of(0, 1, 0, 0, 0x978, 0));
        assert(c.r[0][0] == cases[i].dst);
    }

    /* DERIVED from the Execution pseudocode on printed page 539 with uint
     * operands: src1 = src2 takes the >= 0 branch, so dst = 1. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][0] = 0xdeadbeefu;
    d.r[0][1] = 0xdeadbeefu;
    issue(&d, word_of(2, 1, 0, 0, 0x978, 0));
    assert(d.r[0][2] == 1u);
}

/* ---- ABS, printed pages 101-102 ----------------------------------------- */
static void abs_forms(void)
{
    /* Manual example 1, printed page 102: ABS .L1 A1,A5 with A1 =
     * 8000 4E3Dh gives A5 = 7FFF B1C3h one cycle after the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x80004e3du;
    issue(&c, word_of(5, 1, 0, 0, 0x358, 0));
    assert(c.r[0][5] == 0x7fffb1c3u);

    /* ABS SETS CSR.SAT AND SSR WHEN IT SATURATES.  The ABS entry itself is
     * silent - it contains no occurrence of "SAT", "CSR" or "SSR" - so this
     * comes from a general rule chained with two stated facts about ABS:
     *
     *   Table 4-1, printed page 581, phase E2: "Single-cycle instructions that
     *   saturate results set the SAT bit in the control status register (CSR)
     *   if saturation occurs."
     *   ABS is "Instruction Type Single-cycle" (printed page 102) and its
     *   rule 3 saturates at -2^31 (printed page 101).
     *
     * SSR 2.9.13 (printed page 54) is unqualified in the same direction, and
     * Table 2-22 (printed page 55) puts L1 at bit 0 and L2 at bit 1.  CSR
     * Table 2-9 (printed page 39) gives the timing: "The SAT bit is set one
     * full cycle (one delay slot) after a saturate occurs."
     *
     * DERIVED, not transcribed: the manual prints no ABS example whose input
     * is -2^31, so there is no worked example of this case to copy.  The
     * non-saturating half of each pair is what makes the assertion meaningful -
     * a flag set unconditionally would pass the first check and fail here. */
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x s1; cdj_c674x_reset(&s1, 0x1000);
        s1.r[side][1] = 0x80000000u;
        issue(&s1, word_of(5, 1, 0, 0, 0x358, side));
        cycles(&s1, 2);
        assert(s1.r[side][5] == 0x7fffffffu);
        assert((s1.control[1] >> 9) & 1u);          /* CSR.SAT   */
        assert(s1.control[21] == (1u << side));     /* SSR L1/L2 */

        CdjC674x s2; cdj_c674x_reset(&s2, 0x1000);
        s2.r[side][1] = 0xfffffffeu;                /* -2, no saturation */
        issue(&s2, word_of(5, 1, 0, 0, 0x358, side));
        cycles(&s2, 2);
        assert(s2.r[side][5] == 2u);
        assert(!((s2.control[1] >> 9) & 1u) && !s2.control[21]);
    }

    /* Manual example 2, printed page 102: A1 = 3FF6 0010h gives
     * A5 = 3FF6 0010h. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][1] = 0x3ff60010u;
    issue(&d, word_of(5, 1, 0, 0, 0x358, 0));
    assert(d.r[0][5] == 0x3ff60010u);

    /* Manual example 3, printed page 102: ABS .L1 A1:A0,A5:A4 with
     * A0 = FFFF FFFFh and A1 = 0000 00FFh gives A4 = 0000 0001h and
     * A5 = 0000 0000h. */
    CdjC674x e; cdj_c674x_reset(&e, 0x1000);
    e.r[0][0] = 0xffffffffu;
    e.r[0][1] = 0x000000ffu;
    issue(&e, word_of(4, 0, 0, 0, 0x718, 0));
    assert(e.r[0][4] == 0x00000001u);
    assert(e.r[0][5] == 0x00000000u);

    /* DERIVED from rule 3 on printed page 101, "If src2 = -2^31, then
     * 2^31 - 1 -> dst" and its slong counterpart "If src2 = -2^39, then
     * 2^39 - 1 -> dst".  The manual prints no example for either. */
    CdjC674x f; cdj_c674x_reset(&f, 0x1000);
    f.r[0][1] = 0x80000000u;
    issue(&f, word_of(5, 1, 0, 0, 0x358, 0));
    assert(f.r[0][5] == 0x7fffffffu);

    CdjC674x g; cdj_c674x_reset(&g, 0x1000);
    g.r[0][0] = 0x00000000u;
    g.r[0][1] = 0x00000080u;                /* 80 0000 0000h */
    issue(&g, word_of(4, 0, 0, 0, 0x718, 0));
    assert(g.r[0][4] == 0xffffffffu);       /* 7F FFFF FFFFh = 2^39 - 1 */
    assert(g.r[0][5] == 0x0000007fu);

    /* A cross-path src2 is legal for the sint form (xsint) and rejected for
     * the slong form, which no cross path can carry. */
    CdjC674x h; cdj_c674x_reset(&h, 0x1000);
    h.r[1][1] = 0x80004e3du;
    issue(&h, word_of(5, 1, 0, 1, 0x358, 0));
    assert(h.r[0][5] == 0x7fffb1c3u);
    const char *fault;
    assert(rejects(word_of(4, 0, 0, 1, 0x718, 0), &fault));
    assert(rejects(word_of(4, 1, 0, 0, 0x718, 0), &fault));   /* odd src2 */
    assert(rejects(word_of(5, 0, 0, 0, 0x718, 0), &fault));   /* odd dst */

    /* MVK .L shares ABS's opfield with src1 = 5 (printed page 380); the ABS
     * rows must not claim it.  MVK .L1 -6, A5 leaves A5 = FFFF FFFAh. */
    CdjC674x i; cdj_c674x_reset(&i, 0x1000);
    issue(&i, word_of(5, 26, 5, 0, 0x358, 0));
    assert(i.r[0][5] == 0xfffffffau);
}

/* ---- CMPEQ / CMPGT / CMPGTU / CMPLT / CMPLTU 40-bit src2 forms ----------- */
static void cmp_long(void)
{
    /* Manual example 3, printed page 178: CMPEQ .L2X A1,B3:B2,B1 with
     * A1 = F23A 3789h and B3:B2 = 0000 00FFh F23A 3789h gives B1 =
     * 0000 0001h (true) one cycle after the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0xf23a3789u;
    c.r[1][2] = 0xf23a3789u;
    c.r[1][3] = 0x000000ffu;
    issue(&c, word_of(1, 2, 1, 1, 0xa38, 1));
    assert(c.r[1][1] == 1u);

    /* Manual example 3, printed page 198: CMPGTU .L1 0Eh,A3:A2,A4 with
     * A3:A2 = 0000 0000h 0000 000Ah gives A4 = 0000 0001h (true). */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][2] = 0x0000000au;
    d.r[0][3] = 0x00000000u;
    issue(&d, word_of(4, 2, 0x0e, 0, 0x998, 0));
    assert(d.r[0][4] == 1u);

    /* Manual example 3, printed page 212: CMPLTU .L1 A1,A5:A4,A2 with
     * A1 = 003B 8260h and A5:A4 = 0000 0000h 003A 0002h gives A2 =
     * 0000 0000h (false). */
    CdjC674x e; cdj_c674x_reset(&e, 0x1000);
    e.r[0][1] = 0x003b8260u;
    e.r[0][4] = 0x003a0002u;
    e.r[0][5] = 0x00000000u;
    e.r[0][2] = 0x11223344u;
    issue(&e, word_of(2, 4, 1, 0, 0xbb8, 0));
    assert(e.r[0][2] == 0u);

    /* DERIVED from the Execution pseudocode on printed pages 177, 188, 197,
     * 202 and 211, with src2 read as the 40-bit pair (printed page 447's
     * statement that only the bottom 40 bits participate) and the signed
     * forms' src1 sign extended as printed page 178's Example 3 requires.
     * The manual prints no 40-bit example for CMPEQ with a constant src1, for
     * CMPGT or for CMPLT, and none with a negative 40-bit src2.
     *
     * Case values: src2 = FF FFFF FFFFh is -1 as slong and 1,099,511,627,775
     * as ulong; src1 = -1 is 1Fh as scst5 and 31 as ucst5.
     *   CMPEQ  50h: scst5 -1 == slong -1                      -> 1
     *   CMPGT  44h: scst5 -1 >  slong -1                      -> 0
     *   CMPGT  45h: xsint 0 >  slong -1                       -> 1
     *   CMPGTU 4ch: ucst5 31 > ulong 1,099,511,627,775        -> 0
     *   CMPLT  54h: scst5 -1 <  slong -1                      -> 0
     *   CMPLT  55h: xsint -2 < slong -1                       -> 1
     *   CMPLTU 5ch: ucst5 31 < ulong 1,099,511,627,775        -> 1
     *   CMPGTU 4dh: xuint FFFF FFFFh > ulong FF FFFF FFFFh    -> 0
     *   CMPLTU 5dh: xuint FFFF FFFFh < ulong FF FFFF FFFFh    -> 1 */
    static const struct { uint32_t tail, src1; uint32_t src1_reg, expect; }
    derived[] = {
        {0xa18u, 0x1fu, 0,           1},  /* CMPEQ  scst5 */
        {0x898u, 0x1fu, 0,           0},  /* CMPGT  scst5 */
        {0x8b8u, 0u,    0x00000000u, 1},  /* CMPGT  xsint */
        {0x998u, 0x1fu, 0,           0},  /* CMPGTU ucst5 */
        {0xa98u, 0x1fu, 0,           0},  /* CMPLT  scst5 */
        {0xab8u, 0u,    0xfffffffeu, 1},  /* CMPLT  xsint */
        {0xb98u, 0x1fu, 0,           1},  /* CMPLTU ucst5 */
        {0x9b8u, 0u,    0xffffffffu, 0},  /* CMPGTU xuint */
        {0xbb8u, 0u,    0xffffffffu, 1},  /* CMPLTU xuint */
    };
    for (unsigned i = 0; i < sizeof(derived) / sizeof(derived[0]); ++i) {
        CdjC674x f; cdj_c674x_reset(&f, 0x1000);
        f.r[0][6] = 0xffffffffu;
        f.r[0][7] = 0x000000ffu;            /* FF FFFF FFFFh */
        f.r[0][4] = derived[i].src1_reg;
        f.r[0][5] = 0x11223344u;
        unsigned src1 = derived[i].tail & 0x20u ? 4u : derived[i].src1;
        issue(&f, word_of(5, 6, src1, 0, derived[i].tail, 0));
        assert(f.r[0][5] == derived[i].expect);
    }

    /* The scalar forms this family must not disturb: CMPEQ .L1 A4,A6,A5 and
     * CMPLTU .L1 A4,A6,A5 keep their existing opfields 53h and 5Fh. */
    CdjC674x g; cdj_c674x_reset(&g, 0x1000);
    g.r[0][4] = 0xffffffffu;
    g.r[0][6] = 0xffffffffu;
    issue(&g, word_of(5, 6, 4, 0, 0xa78, 0));
    assert(g.r[0][5] == 1u);
    issue(&g, word_of(5, 6, 4, 0, 0xbf8, 0));
    assert(g.r[0][5] == 0u);

    const char *fault;
    assert(rejects(word_of(5, 7, 4, 0, 0xa38, 0), &fault));   /* odd src2 */
    /* x routes src1 for the register forms (asm6x emits CMPEQ .L2X A1,B3:B2,B1
     * as 00883A3Ah); the constant forms have nothing to cross. */
    assert(rejects(word_of(5, 6, 4, 1, 0xa18, 0), &fault));
}

/* ---- SHL / SHR / SHRU 40-bit forms ------------------------------------- */
static void shift_long(void)
{
    /* Manual example 3, printed page 448: SHL .S2 B1:B0,B2,B3:B2 with
     * B1:B0 = 0000 0009h 4197 51A5h and B2 = 0000 0022h gives B3:B2 =
     * 0000 0094h 0000 0000h one cycle after the instruction. */
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 0x419751a5u;
    c.r[1][1] = 0x00000009u;
    c.r[1][2] = 0x00000022u;
    issue(&c, word_of(2, 0, 2, 0, 0xc60, 1));
    assert(c.r[1][2] == 0x00000000u);
    assert(c.r[1][3] == 0x00000094u);

    /* Manual example 4, printed page 448: SHL .S1 A5:A4,0,A1:A0 with
     * A5:A4 = FFFF FFFFh FFFF FFFFh gives A1:A0 = 0000 00FFh FFFF FFFFh. */
    CdjC674x d; cdj_c674x_reset(&d, 0x1000);
    d.r[0][4] = 0xffffffffu;
    d.r[0][5] = 0xffffffffu;
    issue(&d, word_of(0, 4, 0, 0, 0xc20, 0));
    assert(d.r[0][0] == 0xffffffffu);
    assert(d.r[0][1] == 0x000000ffu);

    /* Manual example 3, printed page 452: SHR .S2 B1:B0,B2,B3:B2 with
     * B1:B0 = 0000 0012h 1492 5A41h and B2 = 0000 0019h gives B3:B2 =
     * 0000 0000h 0000 090Ah. */
    CdjC674x e; cdj_c674x_reset(&e, 0x1000);
    e.r[1][0] = 0x14925a41u;
    e.r[1][1] = 0x00000012u;
    e.r[1][2] = 0x00000019u;
    issue(&e, word_of(2, 0, 2, 0, 0xd60, 1));
    assert(e.r[1][2] == 0x0000090au);
    assert(e.r[1][3] == 0x00000000u);

    /* Manual example 4, printed page 452: SHR .S1 A5:A4,0,A1:A0 with
     * A5:A4 = FFFF FFFFh FFFF FFFFh gives A1:A0 = 0000 00FFh FFFF FFFFh. */
    CdjC674x f; cdj_c674x_reset(&f, 0x1000);
    f.r[0][4] = 0xffffffffu;
    f.r[0][5] = 0xffffffffu;
    issue(&f, word_of(0, 4, 0, 0, 0xd20, 0));
    assert(f.r[0][0] == 0xffffffffu);
    assert(f.r[0][1] == 0x000000ffu);

    /* Manual example 2, printed page 458: SHRU .S1 A5:A4,0,A1:A0 with
     * A5:A4 = FFFF FFFFh FFFF FFFFh gives A1:A0 = 0000 00FFh FFFF FFFFh. */
    CdjC674x g; cdj_c674x_reset(&g, 0x1000);
    g.r[0][4] = 0xffffffffu;
    g.r[0][5] = 0xffffffffu;
    issue(&g, word_of(0, 4, 0, 0, 0x920, 0));
    assert(g.r[0][0] == 0xffffffffu);
    assert(g.r[0][1] == 0x000000ffu);

    /* DERIVED from the Execution lines and the "If 39 < src1 < 64, src2 is
     * shifted ... by 40" rule on printed pages 447, 451 and 457.  The manual
     * prints no 40-bit example with a shift that separates SHR from SHRU, and
     * none at all for SHL's xuint-into-ulong form.
     *   SHR  of FF FFFF FFFFh (= -1) by 8  -> FF FFFF FFFFh (sign fill)
     *   SHRU of FF FFFF FFFFh by 8         -> 00 FFFF FFFFh (zero fill)
     *   SHR  of FF FFFF FFFFh by 63        -> shift by 40   -> FF FFFF FFFFh
     *   SHRU of FF FFFF FFFFh by 63        -> shift by 40   -> 00 0000 0000h
     *   SHL  of FF FFFF FFFFh by 63        -> shift by 40   -> 00 0000 0000h
     *   SHL  xuint FFFF FFFFh by 8         -> FF FFFF FF00h (zero extended) */
    static const struct {
        uint32_t tail, count; uint32_t low, high;
    } derived[] = {
        {0xd20u, 8u,  0xffffffffu, 0x000000ffu},  /* SHR  ucst5 8 */
        {0x920u, 8u,  0xffffffffu, 0x00000000u},  /* SHRU ucst5 8 */
        {0xd60u, 63u, 0xffffffffu, 0x000000ffu},  /* SHR  reg 63 */
        {0x960u, 63u, 0x00000000u, 0x00000000u},  /* SHRU reg 63 */
        {0xc60u, 63u, 0x00000000u, 0x00000000u},  /* SHL  reg 63 */
    };
    for (unsigned i = 0; i < sizeof(derived) / sizeof(derived[0]); ++i) {
        CdjC674x h; cdj_c674x_reset(&h, 0x1000);
        h.r[0][4] = 0xffffffffu;
        h.r[0][5] = 0x000000ffu;
        h.r[0][8] = derived[i].count;
        unsigned src1 = derived[i].tail & 0x40u ? 8u : derived[i].count;
        issue(&h, word_of(0, 4, src1, 0, derived[i].tail, 0));
        assert(h.r[0][0] == derived[i].low);
        assert(h.r[0][1] == derived[i].high);
    }

    /* SHL's xuint source is zero extended, not sign extended: a count of zero
     * leaves 00 FFFF FFFFh, where a sign extension would leave FF FFFF FFFFh.
     * DERIVED from the operand types on printed page 447 (xuint src2, ulong
     * dst); the manual prints no example of this form. */
    CdjC674x i; cdj_c674x_reset(&i, 0x1000);
    i.r[1][6] = 0xffffffffu;                /* xuint source, crossed */
    issue(&i, word_of(8, 6, 0, 1, 0x4a0, 0));
    assert(i.r[0][8] == 0xffffffffu);
    assert(i.r[0][9] == 0x00000000u);
    issue(&i, word_of(8, 6, 8, 1, 0x4a0, 0));
    assert(i.r[0][8] == 0xffffff00u);
    assert(i.r[0][9] == 0x000000ffu);

    /* The xuint source form may cross; the pair-source forms may not. */
    const char *fault;
    assert(rejects(word_of(0, 4, 0, 1, 0xc20, 0), &fault));
    assert(rejects(word_of(0, 4, 0, 1, 0xd20, 0), &fault));
    assert(rejects(word_of(0, 4, 0, 1, 0x920, 0), &fault));
    assert(rejects(word_of(1, 4, 0, 0, 0xc20, 0), &fault));   /* odd dst */
    assert(rejects(word_of(0, 5, 0, 0, 0xc20, 0), &fault));   /* odd src2 */

    /* The scalar .S shifts this family must not disturb: SHR .S1 A6,8,A5 and
     * SHRU .S1 A6,8,A5, whose rows are unchanged. */
    CdjC674x j; cdj_c674x_reset(&j, 0x1000);
    j.r[0][6] = 0xf12363d1u;
    issue(&j, word_of(5, 6, 8, 0, 0xda0, 0));
    assert(j.r[0][5] == 0xfff12363u);       /* printed page 452, Example 1 */
    issue(&j, word_of(5, 6, 8, 0, 0x9a0, 0));
    assert(j.r[0][5] == 0x00f12363u);       /* printed page 458, Example 1 */
}

/* Execute one B NRP on an already-configured CPU, returning whether it was
 * accepted.  Unlike rejects() this keeps the caller's control state, which is
 * the whole point when the refusal depends on TSR. */
static bool b_nrp_executes(CdjC674x *c)
{
    CdjC674xPacket p = {
        .instructions = {{.word = 0x001c00e2u, .pc = c->pc}},
        .count = 1, .next_pc = c->pc + 4,
    };
    return cdj_c674x_execute(c, &p, NULL, NULL, NULL);
}

/* ---- B NRP, printed pages 157-158 --------------------------------------- */
static void b_nrp(void)
{
    /* Printed page 157: "NRP is placed in the program fetch counter (PFC).
     * This instruction also sets the NMIE bit."  Printed page 158's Table 3-22
     * gives five delay slots: the branch target executes in cycle 6.
     * NRP is control register 7 and NMIE is IER (control register 4) bit 1. */
    CdjC674x c; cdj_c674x_reset(&c, 0x20);
    c.control[7] = 0x00001000u;
    c.control[4] &= ~2u;
    issue(&c, 0x001c00e2u);
    assert(c.control[4] & 2u);
    for (unsigned i = 1; i < 6; ++i) {
        assert(c.pc != 0x00001000u);
        cycles(&c, 1);
    }
    assert(c.pc == 0x00001000u);

    /* Predicated off, no branch and no NMIE change (Execution: "else nop"). */
    CdjC674x d; cdj_c674x_reset(&d, 0x20);
    d.control[7] = 0x00001000u;
    d.control[4] &= ~2u;
    d.r[0][0] = 0;
    issue(&d, (1u << 29) | 0x001c00e2u);
    cycles(&d, 8);
    assert(!(d.control[4] & 2u));
    assert(d.pc != 0x00001000u);

    /* B IRP must keep reading IRP, not NRP. */
    CdjC674x e; cdj_c674x_reset(&e, 0x20);
    e.control[6] = 0x00002000u;
    e.control[7] = 0x00001000u;
    issue(&e, 0x001800e2u);
    cycles(&e, 5);
    assert(e.pc == 0x00002000u);

    /* The instruction page is not the whole rule.  5.3.4.2 (printed page 639)
     * adds "The NTSR register will be copied back into the TSR register during
     * the transfer of control out of the interrupt", the counterpart of the
     * ITSR -> TSR restore B IRP performs.  NTSR is control register 28, which
     * this core neither reads, writes nor models, so there is nothing to
     * restore from and performing the restore would zero TSR - a fabricated
     * effect.  B NRP therefore refuses exactly the states where the restore
     * would be observable, and stays available where it would be a no-op.
     *
     * Reset TSR is 0, so the case above already proves the no-op path works.
     * Here TSR carries the bits this core's own interrupt entry sets (9 and
     * 15), which is what a B NRP inside a maskable ISR would see. */
    CdjC674x f; cdj_c674x_reset(&f, 0x20);
    f.control[7] = 0x00001000u;
    f.control[26] = (1u << 15) | (1u << 9);
    assert(!b_nrp_executes(&f));
    assert(f.fault && strstr(f.fault, "NTSR"));
    assert(f.pc != 0x00001000u);

    /* Every restorable TSR bit must trigger the refusal, not just those two:
     * the mask is B IRP's restorable set. */
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (!((0x0000c6deu >> bit) & 1u)) continue;
        CdjC674x g; cdj_c674x_reset(&g, 0x20);
        g.control[7] = 0x00001000u;
        g.control[26] = 1u << bit;
        assert(!b_nrp_executes(&g));
        assert(g.fault && strstr(g.fault, "NTSR"));
    }

    /* A TSR bit OUTSIDE that mask is not restorable, so it must NOT refuse. */
    CdjC674x h; cdj_c674x_reset(&h, 0x20);
    h.control[7] = 0x00001000u;
    h.control[26] = ~0x0000c6deu & 0xffffffffu;
    issue(&h, 0x001c00e2u);
    assert(!h.fault);
    cycles(&h, 5);
    assert(h.pc == 0x00001000u);
}

/* ---- BPOS, printed pages 170-171 ---------------------------------------- */
static void bpos(void)
{
    /* Printed page 170: "if (dst >= 0), PFC = (PCE1 + (se(scst10) << 2))",
     * five delay slots, dst read and not written.
     *
     * DERIVED, not transcribed.  Printed page 171's example, BPOS .S1 200h,A10
     * with PCE1 = 0010 0000h, is unusable twice over: it prints PC =
     * 0100 0800h where its own rule gives 0010 0000h + (200h << 2) =
     * 0010 0800h (the low three digits agree, the first does not), and 200h
     * does not fit the field at all - the opcode map types src "scst10" and
     * Table 3-2 (printed page 69) makes scstn "n-bit signed constant field",
     * so the range is -512 to 511.  Every expected value below therefore comes
     * from the Execution line, with 0FFh as a positive displacement and 3FFh as
     * se() = -1.  The encoding itself is confirmed against ti-cgt-c6000 8.5.0
     * asm6x -mv6740, which assembles "BPOS .S1 $, A5" at offset 8 of a fetch
     * packet as 02804020h: scst10 = 2, dst = A5, bits 11-2 = 020h. */
    CdjC674x c; cdj_c674x_reset(&c, 0x00100000u);
    c.r[0][10] = 0x0000000au;
    issue(&c, word_of(10, 0, 0, 0, 0x020, 0) | 0x0ffu << 13);
    for (unsigned i = 1; i < 6; ++i) {
        assert(c.pc != 0x001003fcu);
        cycles(&c, 1);
    }
    assert(c.pc == 0x001003fcu);
    assert(c.r[0][10] == 0x0000000au);

    /* A negative predication register takes "no other action": execution stays
     * sequential, so after the instruction and five more packets pc is
     * 0010 0018h rather than the branch target. */
    CdjC674x d; cdj_c674x_reset(&d, 0x00100000u);
    d.r[0][10] = 0x80000000u;
    issue(&d, word_of(10, 0, 0, 0, 0x020, 0) | 0x0ffu << 13);
    cycles(&d, 5);
    assert(d.pc == 0x00100018u);

    /* se(scst10): 3FFh is -1, so the target is PCE1 - 4.  PCE1 is the fetch
     * packet base, pc & ~31. */
    CdjC674x e; cdj_c674x_reset(&e, 0x00100020u);
    e.r[1][5] = 0;
    issue(&e, word_of(5, 0, 0, 0, 0x020, 1) | 0x3ffu << 13);
    cycles(&e, 5);
    assert(e.pc == 0x0010001cu);

    /* Printed page 170: BPOS may not share an execute packet with ADDKPC, and
     * only one BPOS may issue per cycle. */
    CdjC674x f; cdj_c674x_reset(&f, 0x00100000u);
    uint32_t bpos_word = word_of(10, 0, 0, 0, 0x020, 0) | 0x0ffu << 13;
    CdjC674xPacket p = {
        .instructions = {{.word = bpos_word | 1u, .pc = f.pc},
                         {.word = bpos_word, .pc = f.pc + 4}},
        .count = 2, .next_pc = f.pc + 8,
    };
    assert(!cdj_c674x_execute(&f, &p, NULL, NULL, NULL));
    assert(f.fault);

    CdjC674x g; cdj_c674x_reset(&g, 0x00100000u);
    CdjC674xPacket q = {
        .instructions = {{.word = bpos_word | 1u, .pc = g.pc},
                         {.word = 0x00000162u | 3u << 23, .pc = g.pc + 4}},
        .count = 2, .next_pc = g.pc + 8,
    };
    assert(!cdj_c674x_execute(&g, &q, NULL, NULL, NULL));
    assert(g.fault);
}

int main(void)
{
    mpyi();
    mpyid();
    mpy2();
    gmpy4();
    dmv();
    sat();
    subc();
    abs_forms();
    cmp_long();
    shift_long();
    b_nrp();
    bpos();
    printf("c674x-mpy32 ok\n");
    return 0;
}
