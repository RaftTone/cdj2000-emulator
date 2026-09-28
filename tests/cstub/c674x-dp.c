/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Double-precision C674x instruction semantics and timing, against SPRUFE8B
 * (July 2010) and nothing else.
 *
 * Provenance of every expected value in this file:
 *
 *  - The encodings come from TI's assembler (asm6x -mv6740).  dp_word() below
 *    rebuilds them from fields and is checked against the assembler's own
 *    output first, so a mistake in the field layout fails immediately rather
 *    than quietly agreeing with our decoder.
 *  - Values marked TRANSCRIBED are copied literally out of the named printed
 *    page's "Example" block, including the "N cycles after instruction"
 *    header that fixes the latency.
 *  - Values marked TRANSCRIBED TABLE are copied literally out of the named
 *    page's special-case table (the DP compares have one each).
 *  - Values marked DERIVED are computed here from the manual's own prose
 *    rules because the page gives no example for that case; the rule and its
 *    printed page are quoted beside each one.  Nothing is derived from this
 *    emulator.
 *
 * There is no execution oracle for this part: the installed TI tools are
 * codegen only.  So a DERIVED case is only ever an arithmetic consequence of
 * a quoted sentence, never an observation.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
/* Deliberately only cdj_c674x.h: every expectation below is checked through
 * the whole core, never against the pure helpers in cdj_c674x_dp.h, so that
 * the decode, the delay slots and the status plumbing are all under test. */
#include "cdj_c674x.h"

static uint32_t memory[64];
static bool read_word(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address < 0x1000 || address >= 0x1100 || (address & 3)) return false;
    *value = memory[(address - 0x1000) / 4];
    return true;
}

/* Field layout of the conditional 32-bit DP opcodes (the Opcode figures on
 * printed pages 105, 125, 184, 258, 275, 318, 352, 477 and 541 all share it):
 * dst 27-23, src2 22-18, src1 17-13, x 12, opfield 11-5 with bits 4-2 fixed,
 * s 1, p 0.  `encoding` is bits 11-2 in place. */
static uint32_t dp_word(unsigned dst, unsigned src2, unsigned src1,
                        unsigned cross, unsigned encoding, unsigned side)
{
    return dst << 23 | src2 << 18 | src1 << 13 | cross << 12 | encoding |
           side << 1;
}

#define ABSDP    0xb20u
#define SPDP     0x0a0u
#define CMPEQDP  0xa20u
#define CMPGTDP  0xa60u
#define CMPLTDP  0xaa0u
#define ADDDP_L  0x318u
#define ADDDP_S  0xe58u
#define SUBDP_L  0x338u
#define SUBDP_LR 0x3b8u
#define SUBDP_S  0xe78u
#define SUBDP_SR 0xef8u
#define MPYDP    0x700u
#define MPYSPDP  0x5b0u
#define MPYSP2DP 0x5f0u
#define DPSP     0x138u
#define DPINT    0x118u
#define DPTRUNC  0x038u
#define INTDP    0x738u
#define INTDPU   0x778u

/* Words emitted by asm6x -mv6740 for the manual's own example operands.  Held
 * here so the field builder is measured against TI, not against us. */
static void test_encodings_match_ti_assembler(void)
{
    assert(dp_word(2, 1, 0, 0, ABSDP, 0)    == 0x01040B20); /* A1:A0,A3:A2 */
    assert(dp_word(2, 1, 0, 0, ABSDP, 1)    == 0x01040B22); /* .S2 B1:B0   */
    assert(dp_word(4, 0, 2, 1, ADDDP_L, 0)  == 0x02005318); /* .L1X        */
    assert(dp_word(4, 2, 0, 1, SUBDP_LR, 0) == 0x020813B8);
    assert(dp_word(4, 2, 0, 0, MPYDP, 0)    == 0x02080700);
    assert(dp_word(4, 2, 0, 0, CMPEQDP, 0)  == 0x02080A20);
    assert(dp_word(4, 2, 0, 0, CMPGTDP, 0)  == 0x02080A60);
    assert(dp_word(4, 2, 0, 1, CMPLTDP, 0)  == 0x02081AA0);
    assert(dp_word(0, 2, 0, 1, SPDP, 0)     == 0x000810A0);
    assert(dp_word(4, 1, 0, 0, DPSP, 0)     == 0x02040138);
    assert(dp_word(4, 1, 0, 0, DPINT, 0)    == 0x02040118);
    assert(dp_word(4, 1, 0, 0, DPTRUNC, 0)  == 0x02040038);
    assert(dp_word(0, 4, 0, 1, INTDP, 0)    == 0x00101738);
    assert(dp_word(0, 4, 0, 0, INTDPU, 0)   == 0x00100778);
    assert(dp_word(8, 4, 2, 0, MPYSPDP, 0)  == 0x041045B0);
    assert(dp_word(8, 3, 2, 0, MPYSP2DP, 0) == 0x040C45F0);
    assert(dp_word(4, 2, 0, 0, ADDDP_S, 0)  == 0x02080E58);
    assert(dp_word(4, 2, 0, 1, SUBDP_S, 0)  == 0x02081E78);
    assert(dp_word(4, 0, 2, 1, SUBDP_SR, 0) == 0x02005EF8);
    assert(dp_word(4, 2, 0, 0, SUBDP_L, 0)  == 0x02080338);
    assert(dp_word(4, 2, 0, 0, ADDDP_L, 1)  == 0x0208031A);
}

static void load(CdjC674x *c, uint32_t word)
{
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(c, 0x1000);
    memory[0] = word;
}

static void run(CdjC674x *c, unsigned steps)
{
    for (unsigned i = 0; i < steps; ++i)
        assert(cdj_c674x_step(c, read_word, NULL, NULL));
}

static void set_pair(CdjC674x *c, unsigned bank, unsigned reg, uint64_t value)
{
    c->r[bank][reg] = (uint32_t)value;
    c->r[bank][reg + 1] = (uint32_t)(value >> 32);
}

static uint64_t get_pair(const CdjC674x *c, unsigned bank, unsigned reg)
{
    return (uint64_t)c->r[bank][reg + 1] << 32 | c->r[bank][reg];
}

/* Every manual example in this family uses these two operands. */
#define DP_8_6  UINT64_C(0x4021333333333333)   /* 8.6  */
#define DP_M2_5 UINT64_C(0xC004000000000000)   /* -2.5 */

/* ---- the manual's own examples ------------------------------------------ */

static void test_manual_examples(void)
{
    CdjC674x c;

    /* ABSDP .S1 A1:A0,A3:A2, printed page 106.  TRANSCRIBED: before A1:A0 =
     * C004 0000h 0000 0000h (-2.5); "2 cycles after instruction" A3:A2 =
     * 4004 0000h 0000 0000h (2.5).  Table 4-12 (printed page 596) splits that
     * into dst_l on E1 and dst_h on E2, which the two steps check apart. */
    load(&c, dp_word(2, 1, 0, 0, ABSDP, 0));
    set_pair(&c, 0, 0, DP_M2_5);
    c.r[0][2] = c.r[0][3] = 0xdeadbeef;
    run(&c, 1);
    assert(c.cycles == 1 && c.r[0][2] == 0 && c.r[0][3] == 0xdeadbeef);
    run(&c, 1);
    assert(get_pair(&c, 0, 2) == UINT64_C(0x4004000000000000));
    assert(!c.load_count && !c.control[19]);

    /* ADDDP .L1X B1:B0,A3:A2,A5:A4, printed page 126.  TRANSCRIBED: B1:B0 =
     * 4021 3333h 3333 3333h (8.6), A3:A2 = C004 0000h 0000 0000h (-2.5),
     * "7 cycles after instruction" A5:A4 = 4018 6666h 6666 6666h (6.1).
     * Table 4-16 (printed page 599) puts dst_l on E6 and dst_h on E7. */
    load(&c, dp_word(4, 0, 2, 1, ADDDP_L, 0));
    set_pair(&c, 1, 0, DP_8_6);
    set_pair(&c, 0, 2, DP_M2_5);
    c.r[0][4] = c.r[0][5] = 0xdeadbeef;
    run(&c, 5);
    assert(c.r[0][4] == 0xdeadbeef && c.r[0][5] == 0xdeadbeef);
    run(&c, 1);
    assert(c.cycles == 6 && c.r[0][4] == 0x66666666 &&
           c.r[0][5] == 0xdeadbeef);
    run(&c, 1);
    assert(get_pair(&c, 0, 4) == UINT64_C(0x4018666666666666));
    assert(!c.load_count && !c.control[18] && !c.control[19]);

    /* SUBDP .L1X B1:B0,A3:A2,A5:A4, printed page 543.  TRANSCRIBED: same
     * sources, "7 cycles after instruction" A5:A4 = 4026 3333h 3333 3333h
     * (11.1).  This is the reverse opfield 001 1101, which cross-paths src1. */
    load(&c, dp_word(4, 2, 0, 1, SUBDP_LR, 0));
    set_pair(&c, 1, 0, DP_8_6);
    set_pair(&c, 0, 2, DP_M2_5);
    run(&c, 7);
    assert(get_pair(&c, 0, 4) == UINT64_C(0x4026333333333333));

    /* MPYDP .M1 A1:A0,A3:A2,A5:A4, printed page 319.  TRANSCRIBED: A1:A0 =
     * 4021 3333h 3333 3333h (8.6), A3:A2 = C004 0000h 0000 0000h (-2.5),
     * "10 cycles after instruction" A5:A4 = C035 8000h 0000 0000h (-21.5).
     * Table 4-19 (printed page 600) puts dst_l on E9 and dst_h on E10, and
     * the status in FMCR on E9. */
    load(&c, dp_word(4, 2, 0, 0, MPYDP, 0));
    set_pair(&c, 0, 0, DP_8_6);
    set_pair(&c, 0, 2, DP_M2_5);
    c.r[0][4] = c.r[0][5] = 0xdeadbeef;
    run(&c, 8);
    assert(c.r[0][4] == 0xdeadbeef && !c.control[20]);
    run(&c, 1);
    assert(c.cycles == 9 && c.r[0][4] == 0 && c.r[0][5] == 0xdeadbeef);
    /* Note 5, printed page 318: "If rounding is performed, the INEX bit is
     * set."  The exact product needs 106 significand bits, so it is. */
    assert(c.control[20] == 0x80);
    run(&c, 1);
    assert(get_pair(&c, 0, 4) == UINT64_C(0xC035800000000000));
    assert(!c.load_count);

    /* CMPEQDP .S1 A1:A0,A3:A2,A4, printed page 185.  TRANSCRIBED: 8.6 and
     * -2.5 give A4 = 0000 0000h "false".  CMPGTDP (printed page 194) gives
     * A4 = 0000 0001h "true".  CMPLTDP .S1X A1:A0,B3:B2,A4 (printed page 208)
     * gives A4 = 0000 0000h "false" "2 cycles after instruction". */
    static const struct { unsigned encoding, cross; uint32_t expected; }
    compares[] = {
        {CMPEQDP, 0, 0}, {CMPGTDP, 0, 1}, {CMPLTDP, 1, 0},
    };
    for (unsigned i = 0; i < 3; ++i) {
        load(&c, dp_word(4, 2, 0, compares[i].cross, compares[i].encoding, 0));
        set_pair(&c, 0, 0, DP_8_6);
        set_pair(&c, compares[i].cross, 2, DP_M2_5);
        c.r[0][4] = 0xdeadbeef;
        run(&c, 1);
        assert(c.r[0][4] == 0xdeadbeef);       /* Delay Slots 1 */
        run(&c, 1);
        assert(c.cycles == 2 && c.r[0][4] == compares[i].expected);
        assert(!c.load_count && !c.control[19]);
    }

    /* SPDP .S1X B2,A1:A0, printed page 478.  TRANSCRIBED: B2 = 4109 999Ah
     * (8.6), "2 cycles after instruction" A1:A0 = 4021 3333h 4000 0000h. */
    load(&c, dp_word(0, 2, 0, 1, SPDP, 0));
    c.r[1][2] = 0x4109999A;
    c.r[0][0] = c.r[0][1] = 0xdeadbeef;
    run(&c, 1);
    assert(c.r[0][0] == 0x40000000 && c.r[0][1] == 0xdeadbeef);
    run(&c, 1);
    assert(get_pair(&c, 0, 0) == UINT64_C(0x4021333340000000));
    assert(!c.control[19]);

    /* DPSP .L1 A1:A0,A4, printed page 261.  TRANSCRIBED: A1:A0 = 4021 3333h
     * 3333 3333h (8.6), "4 cycles after instruction" A4 = 4109 999Ah. */
    load(&c, dp_word(4, 1, 0, 0, DPSP, 0));
    set_pair(&c, 0, 0, DP_8_6);
    c.r[0][4] = 0xdeadbeef;
    run(&c, 3);
    assert(c.r[0][4] == 0xdeadbeef);           /* Delay Slots 3 */
    run(&c, 1);
    assert(c.cycles == 4 && c.r[0][4] == 0x4109999A);
    /* Note 1, printed page 260: rounding sets INEX, in FADCR for a .L unit. */
    assert(c.control[18] == 0x80);

    /* DPINT .L1 A1:A0,A4, printed page 259.  TRANSCRIBED: 8.6 gives A4 =
     * 0000 0009h "9" four cycles after.  DPTRUNC (printed page 263) gives
     * 0000 0008h "8". */
    load(&c, dp_word(4, 1, 0, 0, DPINT, 0));
    set_pair(&c, 0, 0, DP_8_6);
    run(&c, 4);
    assert(c.r[0][4] == 9 && c.control[18] == 0x80);
    load(&c, dp_word(4, 1, 0, 0, DPTRUNC, 0));
    set_pair(&c, 0, 0, DP_8_6);
    run(&c, 4);
    assert(c.r[0][4] == 8 && c.control[18] == 0x80);
    /* DPTRUNC ignores FADCR.RMODE outright (printed page 262): round toward
     * +infinity would give 9 for DPINT but must still give 8 here. */
    load(&c, dp_word(4, 1, 0, 0, DPTRUNC, 0));
    set_pair(&c, 0, 0, DP_8_6);
    c.control[18] = 2u << 9;
    run(&c, 4);
    assert(c.r[0][4] == 8);
    load(&c, dp_word(4, 1, 0, 0, DPINT, 0));
    set_pair(&c, 0, 0, DP_8_6);
    c.control[18] = 2u << 9;
    run(&c, 4);
    assert(c.r[0][4] == 9);

    /* INTDP .L1X B4,A1:A0, printed page 275.  TRANSCRIBED: B4 = 1965 1127h
     * (426,053,927) gives A1:A0 = 41B9 6511h 2700 0000h.  The page's example
     * header says "4 cycles after instruction", which is the E4 dst_l write;
     * Table 4-14 (printed page 598) puts dst_h on E5 and Delay Slots is 4. */
    load(&c, dp_word(0, 4, 0, 1, INTDP, 0));
    c.r[1][4] = 0x19651127;
    c.r[0][0] = c.r[0][1] = 0xdeadbeef;
    run(&c, 3);
    assert(c.r[0][0] == 0xdeadbeef);
    run(&c, 1);
    assert(c.cycles == 4 && c.r[0][0] == 0x27000000 &&
           c.r[0][1] == 0xdeadbeef);
    run(&c, 1);
    assert(get_pair(&c, 0, 0) == UINT64_C(0x41B9651127000000));
    /* "You cannot set configuration bits with this instruction." */
    assert(!c.control[18] && !c.control[19] && !c.control[20]);

    /* INTDPU .L1 A4,A1:A0, printed page 276.  TRANSCRIBED: A4 = FFFF FFDEh
     * (4,294,967,262) gives A1:A0 = 41EF FFFFh FBC0 0000h. */
    load(&c, dp_word(0, 4, 0, 0, INTDPU, 0));
    c.r[0][4] = 0xFFFFFFDE;
    run(&c, 5);
    assert(get_pair(&c, 0, 0) == UINT64_C(0x41EFFFFFFBC00000));
    assert(!c.control[18]);
    /* The signed form reads the same bits as -34. DERIVED from "the signed
     * integer value in src2 is converted" (printed page 275): -34 = -2^5 -
     * 2 = 1.0625 x 2^5, so e = 1028 and f = 0001b followed by zeros. */
    load(&c, dp_word(0, 4, 0, 0, INTDP, 0));
    c.r[0][4] = 0xFFFFFFDE;
    run(&c, 5);
    assert(get_pair(&c, 0, 0) == UINT64_C(0xC041000000000000));
}

/* ---- the DP compare special-case tables -------------------------------- */

static void test_compare_tables(void)
{
    /* TRANSCRIBED TABLE, printed pages 184 (CMPEQDP), 193 (CMPGTDP) and 207
     * (CMPLTDP): "Special cases of inputs", columns Output / UNORD / INVAL.
     * The NaNn and DENn bits are set "when appropriate" (the note under each
     * table), which is the NAN1/NAN2/DEN1/DEN2 half of the status below. */
    static const uint64_t qnan = UINT64_C(0x7ff8000000000000);
    static const uint64_t pden = UINT64_C(0x0000000000000001);
    static const uint64_t nden = UINT64_C(0x8000000000000001);
    static const uint64_t pinf = UINT64_C(0x7ff0000000000000);
    static const uint64_t ninf = UINT64_C(0xfff0000000000000);
    static const uint64_t pzero = 0, nzero = UINT64_C(0x8000000000000000);
    static const uint64_t other = UINT64_C(0x3ff0000000000000); /* 1.0 */
    struct Case {
        uint64_t left, right;
        uint32_t eq, gt, lt;        /* Output column of each page's table */
        uint32_t eq_status, ordered_status;
    };
    /* UNORD is bit 9 and INVAL bit 4 of FAUCR (Table 2-26, printed page 61);
     * NAN1 bit 0, NAN2 bit 1, DEN1 bit 2, DEN2 bit 3. */
    static const struct Case cases[] = {
        {qnan,  other, 0, 0, 0, 0x201, 0x211},
        {other, qnan,  0, 0, 0, 0x202, 0x212},
        {qnan,  qnan,  0, 0, 0, 0x203, 0x213},
        {pden,  pzero, 1, 0, 0, 0x004, 0x004},
        {nden,  nzero, 1, 0, 0, 0x004, 0x004},
        {pzero, pden,  1, 0, 0, 0x008, 0x008},
        {pzero, nzero, 1, 0, 0, 0x000, 0x000},
        {nden,  pden,  1, 0, 0, 0x00c, 0x00c},
        {pinf,  pinf,  1, 0, 0, 0x000, 0x000},
        {pinf,  other, 0, 1, 0, 0x000, 0x000},
        {ninf,  ninf,  1, 0, 0, 0x000, 0x000},
        {ninf,  other, 0, 0, 1, 0x000, 0x000},
    };
    static const unsigned encodings[] = {CMPEQDP, CMPGTDP, CMPLTDP};
    CdjC674x c;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    for (unsigned k = 0; k < 3; ++k)
    for (unsigned side = 0; side < 2; ++side) {
        uint32_t expected = k == 0 ? cases[i].eq :
                            k == 1 ? cases[i].gt : cases[i].lt;
        uint32_t status = k == 0 ? cases[i].eq_status : cases[i].ordered_status;
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(6, 2, 0, 0, encodings[k], side));
        set_pair(&c, side, 0, cases[i].left);
        set_pair(&c, side, 2, cases[i].right);
        c.r[side][6] = 0xdeadbeef;
        run(&c, 1);
        assert(c.r[side][6] == 0xdeadbeef && !c.control[19]);
        run(&c, 1);
        assert(c.r[side][6] == expected);
        assert(c.control[19] == status << shift);
    }
}

/* ---- cases the manual states as prose but does not exemplify ------------ */

static void test_derived_add_sub_dp(void)
{
    /* Every expected value here is DERIVED from the printed-page-125 notes
     * quoted beside it.  ADDDP gives no example for any of them. */
    static const uint64_t lfpn = UINT64_C(0x7fefffffffffffff);
    static const uint64_t sfpn = UINT64_C(0x0010000000000000);
    static const uint64_t nan_out = UINT64_C(0x7fffffffffffffff);
    static const uint64_t one = UINT64_C(0x3ff0000000000000);
    static const uint64_t two = UINT64_C(0x4000000000000000);
    struct Case {
        uint64_t left, right, expected;
        uint32_t status, rmode, encoding;
    };
    static const struct Case cases[] = {
        /* Exact: 1.0 + 1.0 = 2.0, nothing rounded, no bits set. */
        {one, one, two, 0x000, 0, ADDDP_L},
        /* Note 6 overflow table, all eight entries: LFPN + LFPN overflows,
         * INEX and OVER are set, and the result is signed infinity or signed
         * LFPN by sign and mode -
         *   +  nearest +infinity, zero +LFPN, +inf +infinity, -inf +LFPN
         *   -  nearest -infinity, zero -LFPN, +inf -LFPN, -inf -infinity.
         * INFO is FADCR bit 5 "Result is signed infinity" (Table 2-25,
         * printed page 59), so it accompanies the infinite answers. */
        {lfpn, lfpn, UINT64_C(0x7ff0000000000000), 0x0e0, 0, ADDDP_L},
        {lfpn, lfpn, lfpn, 0x0c0, 1, ADDDP_L},
        {lfpn, lfpn, UINT64_C(0x7ff0000000000000), 0x0e0, 2, ADDDP_L},
        {lfpn, lfpn, lfpn, 0x0c0, 3, ADDDP_L},
        {UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff),
         UINT64_C(0xfff0000000000000), 0x0e0, 0, ADDDP_L},
        {UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff),
         UINT64_C(0xffefffffffffffff), 0x0c0, 1, ADDDP_L},
        {UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff),
         UINT64_C(0xffefffffffffffff), 0x0c0, 2, ADDDP_L},
        {UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff),
         UINT64_C(0xfff0000000000000), 0x0e0, 3, ADDDP_L},
        /* Note 7 underflow table, all eight entries.  SFPN + (-SFPN - 1ulp)
         * and its mirror leave |result| = 2^-1074, below SFPN, so INEX and
         * UNDER are set and the answer is signed 0 except under the mode
         * that rounds away from zero, which gives signed SFPN:
         *   +  nearest +0, zero +0, +inf +SFPN, -inf +0
         *   -  nearest -0, zero -0, +inf -0,    -inf -SFPN. */
        {UINT64_C(0x0010000000000001), UINT64_C(0x8010000000000000),
         0, 0x180, 0, ADDDP_L},
        {UINT64_C(0x0010000000000001), UINT64_C(0x8010000000000000),
         0, 0x180, 1, ADDDP_L},
        {UINT64_C(0x0010000000000001), UINT64_C(0x8010000000000000),
         sfpn, 0x180, 2, ADDDP_L},
        {UINT64_C(0x0010000000000001), UINT64_C(0x8010000000000000),
         0, 0x180, 3, ADDDP_L},
        {sfpn, UINT64_C(0x8010000000000001),
         UINT64_C(0x8000000000000000), 0x180, 0, ADDDP_L},
        {sfpn, UINT64_C(0x8010000000000001),
         UINT64_C(0x8000000000000000), 0x180, 1, ADDDP_L},
        {sfpn, UINT64_C(0x8010000000000001),
         UINT64_C(0x8000000000000000), 0x180, 2, ADDDP_L},
        {sfpn, UINT64_C(0x8010000000000001),
         UINT64_C(0x8010000000000000), 0x180, 3, ADDDP_L},
        /* Note 8: equal numbers of opposite sign give +0, or -0 under
         * round-toward-negative-infinity. */
        {one, UINT64_C(0xbff0000000000000), 0, 0x000, 0, ADDDP_L},
        {one, UINT64_C(0xbff0000000000000), UINT64_C(0x8000000000000000),
         0x000, 3, ADDDP_L},
        /* Note 9: both sources 0 with the same sign keeps that sign. */
        {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000),
         UINT64_C(0x8000000000000000), 0x000, 0, ADDDP_L},
        /* Note 3: a QNaN source gives NaN_out and sets NAN2 only; an SNaN
         * source (fraction msb clear, Table 3-6 printed page 72) also sets
         * INVAL. */
        {one, UINT64_C(0x7ff8000000000000), nan_out, 0x002, 0, ADDDP_L},
        {one, UINT64_C(0x7ff4000000000000), nan_out, 0x012, 0, ADDDP_L},
        {UINT64_C(0x7ff4000000000000), one, nan_out, 0x011, 0, ADDDP_L},
        /* Note 4: +infinity with -infinity gives NaN_out and INVAL. */
        {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
         nan_out, 0x010, 0, ADDDP_L},
        /* Note 5: signed infinity with anything else gives that infinity and
         * sets INFO. */
        {UINT64_C(0xfff0000000000000), one, UINT64_C(0xfff0000000000000),
         0x020, 0, ADDDP_L},
        /* Note 10: a denormalized source acts as a signed 0, sets DENn, and
         * sets INEX because the other source is neither NaN nor infinity. */
        {UINT64_C(0x0000000000000001), one, one, 0x084, 0, ADDDP_L},
        {UINT64_C(0x8000000000000001), UINT64_C(0x7ff0000000000000),
         UINT64_C(0x7ff0000000000000), 0x024, 0, ADDDP_L},
        /* Note 2: rounding sets INEX.  1.0 + 2^-60 is halfway-free and rounds
         * down to 1.0 under nearest-even, up to the next double toward
         * +infinity. */
        {one, UINT64_C(0x3c30000000000000), one, 0x080, 0, ADDDP_L},
        {one, UINT64_C(0x3c30000000000000), UINT64_C(0x3ff0000000000001),
         0x080, 2, ADDDP_L},
        /* Round-to-nearest-even across the tie, which is the only rounding
         * rule Table 2-25 (printed page 59) names: "Round toward nearest
         * representable floating-point number".  The ulp of 1.0 is 2^-52.
         *   1.0            + 2^-53       : exact tie, even mantissa, stays.
         *   (1 + 2^-52)    + 2^-53       : exact tie, odd mantissa, rounds up.
         *   1.0            + 1.5 x 2^-53 : past the tie, rounds up. */
        {one, UINT64_C(0x3ca0000000000000), one, 0x080, 0, ADDDP_L},
        {UINT64_C(0x3ff0000000000001), UINT64_C(0x3ca0000000000000),
         UINT64_C(0x3ff0000000000002), 0x080, 0, ADDDP_L},
        {one, UINT64_C(0x3ca8000000000000), UINT64_C(0x3ff0000000000001),
         0x080, 0, ADDDP_L},
        /* Round toward negative infinity takes a negative result away from
         * zero; round toward zero leaves it alone. */
        {UINT64_C(0xbff0000000000000), UINT64_C(0xbca0000000000000),
         UINT64_C(0xbff0000000000001), 0x080, 3, ADDDP_L},
        {UINT64_C(0xbff0000000000000), UINT64_C(0xbca0000000000000),
         UINT64_C(0xbff0000000000000), 0x080, 1, ADDDP_L},
        {UINT64_C(0xbff0000000000000), UINT64_C(0xbca0000000000000),
         UINT64_C(0xbff0000000000000), 0x080, 2, ADDDP_L},
        /* SUBDP, printed page 541.  Notes 9 and 10 mirror ADDDP's 8 and 9
         * with the signs swapped: equal same-signed sources cancel to +0,
         * -0 under round-toward-negative-infinity. */
        {one, one, 0, 0x000, 0, SUBDP_L},
        {one, one, UINT64_C(0x8000000000000000), 0x000, 3, SUBDP_L},
        {two, one, one, 0x000, 0, SUBDP_L},
        /* SUBDP note 5: both sources +infinity gives NaN_out and INVAL. */
        {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000),
         nan_out, 0x010, 0, SUBDP_L},
    };
    CdjC674x c;
    /* Each case runs on .L1/.L2 and .S1/.S2: ADDDP note 1 (printed page 125)
     * says the .S forms "take the rounding mode from and set the warning bits
     * in the floating-point adder configuration register (FADCR)". */
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    for (unsigned unit = 0; unit < 2; ++unit)
    for (unsigned side = 0; side < 2; ++side) {
        struct Case tc = cases[i];
        unsigned encoding = tc.encoding == ADDDP_L ?
            (unit ? ADDDP_S : ADDDP_L) : (unit ? SUBDP_S : SUBDP_L);
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(4, 2, 0, 0, encoding, side));
        set_pair(&c, side, 0, tc.left);
        set_pair(&c, side, 2, tc.right);
        c.control[18] = tc.rmode << (shift + 9);
        run(&c, 7);
        assert(get_pair(&c, side, 4) == tc.expected);
        assert(c.control[18] == ((tc.rmode << (shift + 9)) |
                                 (tc.status << shift)));
    }

    /* The two reverse opfields compute the same difference from swapped
     * encoded fields, and SUBDP note 2 (printed page 541) requires the
     * source-specific warning bits to follow the ENCODED src1/src2 and "not
     * the order of the sources in the assembly form".  A denormal in the
     * encoded src2 field must therefore raise DEN2 even where it is the
     * assembly left operand.  DERIVED from that sentence. */
    load(&c, dp_word(4, 2, 0, 1, SUBDP_LR, 0));   /* src1 = xdp */
    set_pair(&c, 1, 0, UINT64_C(0x0000000000000001));
    set_pair(&c, 0, 2, one);
    run(&c, 7);
    assert(get_pair(&c, 0, 4) == UINT64_C(0xbff0000000000000));
    assert(c.control[18] == 0x084);                /* DEN1 | INEX */
    load(&c, dp_word(4, 0, 2, 1, SUBDP_SR, 0));   /* src2 - src1 */
    set_pair(&c, 1, 0, UINT64_C(0x0000000000000001));
    set_pair(&c, 0, 2, one);
    run(&c, 7);
    assert(get_pair(&c, 0, 4) == UINT64_C(0xbff0000000000000));
    assert(c.control[18] == 0x088);                /* DEN2 | INEX */
}

static void test_derived_multiply_dp(void)
{
    /* DERIVED from the printed-page-318 notes; MPYDP's only example is the
     * one already checked above. */
    static const uint64_t one = UINT64_C(0x3ff0000000000000);
    static const uint64_t nan_out = UINT64_C(0x7fffffffffffffff);
    struct Case { uint64_t left, right, expected; uint32_t status, rmode; };
    static const struct Case cases[] = {
        /* Exact: 1.0 x -2.0 = -2.0. */
        {one, UINT64_C(0xc000000000000000), UINT64_C(0xc000000000000000),
         0x000, 0},
        /* Note 1: the sign of NaN_out is the exclusive-OR of the input signs,
         * and an SNaN source also sets INVAL. */
        {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff8000000000000),
         UINT64_C(0xffffffffffffffff), 0x002, 0},
        {one, UINT64_C(0x7ff4000000000000), nan_out, 0x012, 0},
        /* Note 2: signed infinity times signed 0 gives signed NaN_out and
         * sets INVAL; times a normal number it gives signed infinity. */
        {UINT64_C(0x7ff0000000000000), UINT64_C(0x8000000000000000),
         UINT64_C(0xffffffffffffffff), 0x010, 0},
        {UINT64_C(0xfff0000000000000), UINT64_C(0xc000000000000000),
         UINT64_C(0x7ff0000000000000), 0x020, 0},
        /* Note 3: a signed 0 source gives signed 0. */
        {UINT64_C(0x8000000000000000), UINT64_C(0xc000000000000000),
         0, 0x000, 0},
        /* Note 4: a denormal acts as signed 0 and sets DENn, plus INEX
         * unless the other source is infinity, NaN or signed 0; infinity
         * times a denormal is therefore a signed NaN_out with INVAL. */
        {UINT64_C(0x0000000000000001), UINT64_C(0xc000000000000000),
         UINT64_C(0x8000000000000000), 0x084, 0},
        {UINT64_C(0x0000000000000001), UINT64_C(0x7ff0000000000000),
         nan_out, 0x014, 0},
        {UINT64_C(0x0000000000000001), 0, 0, 0x004, 0},
        /* OVERFLOW AND UNDERFLOW ROUNDING IS DERIVED BY ANALOGY, NOT STATED.
         * An earlier comment here said ADDDP's rounding tables are "the only
         * statement the manual makes about them for .M".  That is wrong in a
         * way worth spelling out: the manual makes NO such statement for
         * MPYDP.  The word LFPN does not occur anywhere in the MPYDP entry,
         * and its notes 1-5 cover only NaN, signed infinity, signed zero,
         * denormalized sources and rounding-sets-INEX - never what an
         * overflowing product rounds to.  The rounding table that names +LFPN
         * and +infinity per rounding mode belongs to ADDDP.
         *
         * These four rows therefore assert this core's chosen behaviour -
         * that a .M overflow rounds the way the .L/.S adder does - which is a
         * reasonable reading of one FPU but is an ANALOGY.  They are kept so
         * the choice is pinned and visible rather than drifting, and recorded
         * in DSP_ARCHITECTURE_COVERAGE.md as unresolved.  LFPN x LFPN
         * overflows and SFPN x SFPN underflows. */
        {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff),
         UINT64_C(0x7ff0000000000000), 0x0e0, 0},
        {UINT64_C(0x7fefffffffffffff), UINT64_C(0x7fefffffffffffff),
         UINT64_C(0x7fefffffffffffff), 0x0c0, 1},
        {UINT64_C(0xffefffffffffffff), UINT64_C(0x7fefffffffffffff),
         UINT64_C(0xffefffffffffffff), 0x0c0, 2},
        {UINT64_C(0x0010000000000000), UINT64_C(0x0010000000000000),
         0, 0x180, 0},
        /* Note 5: rounding sets INEX.  (1 + 2^-52) squared needs 105
         * significand bits, so nearest-even drops the low 2^-104 term. */
        {UINT64_C(0x3ff0000000000001), UINT64_C(0x3ff0000000000001),
         UINT64_C(0x3ff0000000000002), 0x080, 0},
    };
    CdjC674x c;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    for (unsigned side = 0; side < 2; ++side) {
        struct Case tc = cases[i];
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(4, 2, 0, 0, MPYDP, side));
        set_pair(&c, side, 0, tc.left);
        set_pair(&c, side, 2, tc.right);
        c.control[20] = tc.rmode << (shift + 9);
        run(&c, 10);
        assert(get_pair(&c, side, 4) == tc.expected);
        /* Section 2.10.3 (printed page 63): .M-unit status lives in FMCR. */
        assert(c.control[20] == ((tc.rmode << (shift + 9)) |
                                 (tc.status << shift)));
        assert(!c.control[18] && !c.control[19]);
    }

    /* MPYSPDP and MPYSP2DP have no examples at all (printed pages 352-355).
     * DERIVED: both pages say "the single-precision src1 operand is
     * multiplied by the ... src2 operand to produce a double-precision
     * result", and every binary32 value is exact in binary64, so
     *   2.0f x 8.6(dp) = 17.2(dp), which is 8.6(dp) with the exponent
     *   incremented: 4021 3333 3333 3333h -> 4031 3333 3333 3333h, exact.
     *   -1.5f x 3.0f  = -4.5, exact in both formats: C012 0000 0000 0000h. */
    load(&c, dp_word(8, 4, 2, 0, MPYSPDP, 0));
    c.r[0][2] = 0x40000000;                        /* 2.0f */
    set_pair(&c, 0, 4, DP_8_6);
    c.r[0][8] = c.r[0][9] = 0xdeadbeef;
    run(&c, 5);
    assert(c.r[0][8] == 0xdeadbeef);               /* Delay Slots 6 */
    run(&c, 1);
    assert(c.cycles == 6 && c.r[0][8] == 0x33333333 &&
           c.r[0][9] == 0xdeadbeef);
    run(&c, 1);
    assert(get_pair(&c, 0, 8) == UINT64_C(0x4031333333333333));
    assert(!c.control[20]);
    load(&c, dp_word(8, 3, 2, 0, MPYSP2DP, 0));
    c.r[0][2] = 0xbfc00000;                        /* -1.5f */
    c.r[0][3] = 0x40400000;                        /* 3.0f  */
    c.r[0][8] = c.r[0][9] = 0xdeadbeef;
    run(&c, 3);
    assert(c.r[0][8] == 0xdeadbeef);               /* Delay Slots 4 */
    run(&c, 1);
    assert(c.cycles == 4 && c.r[0][8] == 0 && c.r[0][9] == 0xdeadbeef);
    run(&c, 1);
    assert(get_pair(&c, 0, 8) == UINT64_C(0xc012000000000000));
    assert(!c.control[20]);
    /* Note 4 on both pages: a denormalized source is treated as signed 0 and
     * sets DENn.  A binary32 denormal is exactly representable as a binary64
     * normal, so this is the one place where widening must NOT be exact. */
    load(&c, dp_word(8, 3, 2, 0, MPYSP2DP, 0));
    c.r[0][2] = 0x00000001;                        /* smallest SP denormal */
    c.r[0][3] = 0x40400000;                        /* 3.0f */
    run(&c, 5);
    assert(get_pair(&c, 0, 8) == 0 && c.control[20] == 0x084);
    /* And the same source as the DP operand of MPYSPDP raises DEN2. */
    load(&c, dp_word(8, 4, 2, 0, MPYSPDP, 0));
    c.r[0][2] = 0x40400000;
    set_pair(&c, 0, 4, UINT64_C(0x0000000000000001));
    run(&c, 7);
    assert(get_pair(&c, 0, 8) == 0 && c.control[20] == 0x088);
}

static void test_derived_conversions(void)
{
    CdjC674x c;
    /* SPDP, printed page 477, notes 1-4.  DERIVED from the notes; the page's
     * single example is already covered above. */
    struct SpDp { uint32_t source; uint64_t expected; uint32_t status; };
    static const struct SpDp spdp[] = {
        {0x00000000, 0, 0x000},
        {0x80000000, UINT64_C(0x8000000000000000), 0x000},
        {0x00000001, 0, 0x088},                  /* note 3: INEX | DEN2 */
        {0x80000001, UINT64_C(0x8000000000000000), 0x088},
        {0x7f800000, UINT64_C(0x7ff0000000000000), 0x020},  /* note 4: INFO */
        {0xff800000, UINT64_C(0xfff0000000000000), 0x020},
        {0x7fc00000, UINT64_C(0x7fffffffffffffff), 0x002},  /* note 2: QNaN  */
        {0x7fa00000, UINT64_C(0x7fffffffffffffff), 0x012},  /* note 1: SNaN  */
        /* Exact widening of 1.0f and of the smallest SP normal. */
        {0x3f800000, UINT64_C(0x3ff0000000000000), 0x000},
        {0x00800000, UINT64_C(0x3810000000000000), 0x000},
    };
    for (unsigned i = 0; i < sizeof(spdp) / sizeof(spdp[0]); ++i)
    for (unsigned side = 0; side < 2; ++side) {
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(4, 2, 0, 0, SPDP, side));
        c.r[side][2] = spdp[i].source;
        run(&c, 2);
        assert(get_pair(&c, side, 4) == spdp[i].expected);
        /* Two-cycle DP status goes to FAUCR (section 4.2.7, printed page
         * 596), not FADCR. */
        assert(c.control[19] == spdp[i].status << shift);
        assert(!c.control[18] && !c.control[20]);
    }

    /* ABSDP, printed page 105, notes 1-4.  DERIVED. */
    struct AbsDp { uint64_t source, expected; uint32_t status; };
    static const struct AbsDp absdp[] = {
        {UINT64_C(0x8000000000000000), 0, 0x000},
        {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000), 0x000},
        {UINT64_C(0x8000000000000001), 0, 0x088},     /* note 3: +0 */
        {UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000000000), 0x020},
        {UINT64_C(0x7ff8000000000000), UINT64_C(0x7fffffffffffffff), 0x002},
        {UINT64_C(0xfff4000000000000), UINT64_C(0x7fffffffffffffff), 0x012},
    };
    for (unsigned i = 0; i < sizeof(absdp) / sizeof(absdp[0]); ++i)
    for (unsigned side = 0; side < 2; ++side) {
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(4, 3, 0, 0, ABSDP, side));
        set_pair(&c, side, 2, absdp[i].source);
        run(&c, 2);
        assert(get_pair(&c, side, 4) == absdp[i].expected);
        assert(c.control[19] == absdp[i].status << shift);
    }

    /* DPSP, printed page 260, notes 1-7.  DERIVED. */
    struct DpSp { uint64_t source; uint32_t expected, status, rmode; };
    static const struct DpSp dpsp[] = {
        {UINT64_C(0x3ff0000000000000), 0x3f800000, 0x000, 0},
        {UINT64_C(0x8000000000000000), 0x80000000, 0x000, 0},
        {UINT64_C(0x0000000000000001), 0x00000000, 0x088, 0},   /* note 4 */
        {UINT64_C(0xfff0000000000000), 0xff800000, 0x020, 0},   /* note 5 */
        {UINT64_C(0x7ff8000000000000), 0x7fffffff, 0x002, 0},   /* note 3 */
        {UINT64_C(0x7ff4000000000000), 0x7fffffff, 0x012, 0},   /* note 2 */
        /* Note 6 overflow table: a DP value past the SP range. */
        {UINT64_C(0x7fefffffffffffff), 0x7f800000, 0x0e0, 0},
        {UINT64_C(0x7fefffffffffffff), 0x7f7fffff, 0x0c0, 1},
        {UINT64_C(0xffefffffffffffff), 0xff7fffff, 0x0c0, 2},
        {UINT64_C(0xffefffffffffffff), 0xff800000, 0x0e0, 3},
        /* Note 7 underflow table: a DP value below the smallest SP normal.
         * SP denormals are not produced - SFPN here is 0080 0000h. */
        {UINT64_C(0x0010000000000000), 0x00000000, 0x180, 0},
        {UINT64_C(0x0010000000000000), 0x00800000, 0x180, 2},
        {UINT64_C(0x8010000000000000), 0x80000000, 0x180, 2},
        {UINT64_C(0x8010000000000000), 0x80800000, 0x180, 3},
        /* Note 1: rounding sets INEX.  1 + 2^-52 has no SP representation;
         * nearest-even truncates to 1.0f, toward +infinity rounds up. */
        {UINT64_C(0x3ff0000000000001), 0x3f800000, 0x080, 0},
        {UINT64_C(0x3ff0000000000001), 0x3f800001, 0x080, 2},
    };
    for (unsigned i = 0; i < sizeof(dpsp) / sizeof(dpsp[0]); ++i)
    for (unsigned side = 0; side < 2; ++side) {
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(4, 3, 0, 0, DPSP, side));
        set_pair(&c, side, 2, dpsp[i].source);
        c.control[18] = dpsp[i].rmode << (shift + 9);
        run(&c, 4);
        assert(c.r[side][4] == dpsp[i].expected);
        assert(c.control[18] == ((dpsp[i].rmode << (shift + 9)) |
                                 (dpsp[i].status << shift)));
        assert(!c.control[19] && !c.control[20]);
    }

    /* DPINT and DPTRUNC, printed pages 258 and 262, notes 1-4.  DERIVED. */
    struct DpInt {
        uint64_t source;
        uint32_t expected, status, rmode;
        /* The answer DPTRUNC must give for this source whatever FADCR holds,
         * because printed page 262 says its rounding is always toward zero. */
        uint32_t truncated, truncated_status;
    };
    static const struct DpInt dpint[] = {
        {UINT64_C(0x0000000000000000), 0, 0x000, 0, 0, 0x000},
        /* note 3 */
        {UINT64_C(0x0000000000000001), 0, 0x088, 0, 0, 0x088},
        /* Note 1, and an UNRESOLVED CHOICE inside it.  The note reads in full
         * "If src2 is NaN, the maximum signed integer (7FFF FFFFh or
         * 8000 0000h) is placed in dst and the INVAL bit is set."  It offers
         * both values and does NOT say which is chosen.  These two rows pin a
         * sign-selected answer - a NaN with the sign bit clear gives
         * 7FFF FFFFh, one with it set gives 8000 0000h - which mirrors how
         * note 2 must work for signed infinity, where the sign IS meaningful.
         * For a NaN the sign bit carries no arithmetic meaning, so this is a
         * reading of the note rather than the note itself.  Recorded in
         * DSP_ARCHITECTURE_COVERAGE.md; it is pinned here so the choice is
         * visible and stable rather than accidental, not because the manual
         * settles it. */
        {UINT64_C(0x7ff8000000000000), 0x7fffffff, 0x012, 0, 0x7fffffff, 0x012},
        {UINT64_C(0xfff8000000000000), 0x80000000, 0x012, 0, 0x80000000, 0x012},
        /* note 2 */
        {UINT64_C(0x7ff0000000000000), 0x7fffffff, 0x0c0, 0, 0x7fffffff, 0x0c0},
        {UINT64_C(0xfff0000000000000), 0x80000000, 0x0c0, 0, 0x80000000, 0x0c0},
        /* 2^31 overflows; -2^31 does not (the range is 2^31-1 .. -2^31). */
        {UINT64_C(0x41e0000000000000), 0x7fffffff, 0x0c0, 0, 0x7fffffff, 0x0c0},
        {UINT64_C(0xc1e0000000000000), 0x80000000, 0x000, 0, 0x80000000, 0x000},
        /* 2^31 - 1 is exact; 2^31 - 0.5 rounds up to 2^31 and then overflows
         * under DPINT, while DPTRUNC truncates it back to 2^31 - 1. */
        {UINT64_C(0x41dfffffffc00000), 0x7fffffff, 0x000, 0, 0x7fffffff, 0x000},
        {UINT64_C(0x41dfffffffe00000), 0x7fffffff, 0x0c0, 0, 0x7fffffff, 0x080},
        /* Note 4: rounding sets INEX.  Nearest-even breaks 2.5 to 2 and 3.5
         * to 4; 0.5 to 0 and -0.5 to -0 = 0. */
        {UINT64_C(0x4004000000000000), 2, 0x080, 0, 2, 0x080},
        {UINT64_C(0x400c000000000000), 4, 0x080, 0, 3, 0x080},
        {UINT64_C(0x3fe0000000000000), 0, 0x080, 0, 0, 0x080},
        {UINT64_C(0xbfe0000000000000), 0, 0x080, 0, 0, 0x080},
        /* Toward +infinity lifts any positive remainder, toward -infinity
         * any negative one (Table 2-25, printed page 59). */
        {UINT64_C(0x3fe0000000000000), 1, 0x080, 2, 0, 0x080},
        {UINT64_C(0xbfe0000000000000), 0, 0x080, 2, 0, 0x080},
        {UINT64_C(0xbfe0000000000000), 0xffffffff, 0x080, 3, 0, 0x080},
        {UINT64_C(0x3fe0000000000000), 0, 0x080, 3, 0, 0x080},
        {UINT64_C(0x3fe0000000000000), 0, 0x080, 1, 0, 0x080},
        {UINT64_C(0xc004000000000000), 0xfffffffe, 0x080, 0, 0xfffffffe, 0x080},
        {UINT64_C(0xc00c000000000000), 0xfffffffc, 0x080, 0, 0xfffffffd, 0x080},
    };
    for (unsigned i = 0; i < sizeof(dpint) / sizeof(dpint[0]); ++i)
    for (unsigned side = 0; side < 2; ++side) {
        unsigned shift = side ? 16 : 0;
        load(&c, dp_word(4, 3, 0, 0, DPINT, side));
        set_pair(&c, side, 2, dpint[i].source);
        c.control[18] = dpint[i].rmode << (shift + 9);
        run(&c, 4);
        assert(c.r[side][4] == dpint[i].expected);
        assert(c.control[18] == ((dpint[i].rmode << (shift + 9)) |
                                 (dpint[i].status << shift)));
        load(&c, dp_word(4, 3, 0, 0, DPTRUNC, side));
        set_pair(&c, side, 2, dpint[i].source);
        c.control[18] = dpint[i].rmode << (shift + 9);
        run(&c, 4);
        assert(c.r[side][4] == dpint[i].truncated);
        assert(c.control[18] == ((dpint[i].rmode << (shift + 9)) |
                                 (dpint[i].truncated_status << shift)));
    }

    /* INTDP/INTDPU, printed pages 275-276.  DERIVED: exact in every case,
     * and no configuration bits. */
    struct IntDp { uint32_t source; uint64_t signed_dp, unsigned_dp; };
    static const struct IntDp intdp[] = {
        {0, 0, 0},
        {1, UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000)},
        {0xffffffff, UINT64_C(0xbff0000000000000),
         UINT64_C(0x41efffffffe00000)},
        {0x80000000, UINT64_C(0xc1e0000000000000),
         UINT64_C(0x41e0000000000000)},
        {0x7fffffff, UINT64_C(0x41dfffffffc00000),
         UINT64_C(0x41dfffffffc00000)},
    };
    for (unsigned i = 0; i < sizeof(intdp) / sizeof(intdp[0]); ++i)
    for (unsigned side = 0; side < 2; ++side) {
        load(&c, dp_word(4, 2, 0, 0, INTDP, side));
        c.r[side][2] = intdp[i].source;
        run(&c, 5);
        assert(get_pair(&c, side, 4) == intdp[i].signed_dp);
        assert(!c.control[18] && !c.control[19] && !c.control[20]);
        load(&c, dp_word(4, 2, 0, 0, INTDPU, side));
        c.r[side][2] = intdp[i].source;
        run(&c, 5);
        assert(get_pair(&c, side, 4) == intdp[i].unsigned_dp);
        assert(!c.control[18] && !c.control[19] && !c.control[20]);
    }
}

/* ---- fail-closed behaviour ---------------------------------------------- */

static void test_rejections(void)
{
    CdjC674x c;
    /* RCPDP, RCPSP, RSQRDP and RSQRSP are now IMPLEMENTED as declared
     * approximations.  They were refused until the reason was re-read: each
     * entry fixes every special case exactly and leaves only the normal-number
     * mantissa to a tolerance - "the mantissa is accurate to the eighth binary
     * position (therefore, mantissa error is less than 2-8)" - and then frames
     * the result as a Newton-Raphson seed whose accuracy the firmware doubles
     * per iteration.  Meeting a specified tolerance is not inventing semantics.
     *
     * TRANSCRIBED.  Each entry prints one worked example, all four with
     * src2 = 4.0, and all four are exact here because 1/4 and 1/2 are exactly
     * representable: RCPSP printed page 412 gives 3E80 0000h; RSQRSP 421 gives
     * 3F00 0000h; RCPDP 410 gives 3FD0 0000h 0000 0000h; RSQRDP 419 gives
     * 3FE0 0000h 0000 0000h.  Latency is each entry's own: the SP forms are
     * "Delay Slots 0" and the DP forms "Delay Slots 1". */
    {
        CdjC674x a;
        load(&a, dp_word(5, 6, 0, 0, 0xf60, 0));       /* RCPSP .S1 A6,A5 */
        a.r[0][6] = 0x40800000u;
        run(&a, 1);
        assert(a.r[0][5] == 0x3e800000u && !a.control[19]);

        CdjC674x b;
        load(&b, dp_word(5, 6, 0, 0, 0xfa0, 0));       /* RSQRSP .S1 A6,A5 */
        b.r[0][6] = 0x40800000u;
        run(&b, 1);
        assert(b.r[0][5] == 0x3f000000u && !b.control[19]);

        CdjC674x c2;
        load(&c2, dp_word(8, 7, 0, 0, 0xb60, 0));      /* RCPDP .S1 A7:A6,A9:A8 */
        set_pair(&c2, 0, 6, UINT64_C(0x4010000000000000));
        run(&c2, 1);
        assert(c2.r[0][8] == 0);                       /* dst_l on E1 */
        run(&c2, 1);
        assert(get_pair(&c2, 0, 8) == UINT64_C(0x3FD0000000000000));

        CdjC674x d;
        load(&d, dp_word(8, 7, 0, 0, 0xba0, 0));       /* RSQRDP .S1 A7:A6,A9:A8 */
        set_pair(&d, 0, 6, UINT64_C(0x4010000000000000));
        run(&d, 2);
        assert(get_pair(&d, 0, 8) == UINT64_C(0x3FE0000000000000));
    }

    /* The special cases the notes DO fix, checked by name.  RCPSP note 4: a
     * signed zero gives signed infinity with DIV0 (FAUCR bit 10) and INFO
     * (bit 5).  RSQRSP note 3: a negative nonzero nondenormal gives NaN_out
     * with INVAL (bit 4). */
    {
        CdjC674x z;
        load(&z, dp_word(5, 6, 0, 0, 0xf60, 0));
        z.r[0][6] = 0;
        run(&z, 1);
        assert(z.r[0][5] == 0x7f800000u);
        assert(z.control[19] == ((1u << 10) | (1u << 5)));

        CdjC674x n;
        load(&n, dp_word(5, 6, 0, 0, 0xfa0, 0));
        n.r[0][6] = 0xc0000000u;                        /* -2.0 */
        run(&n, 1);
        assert(n.r[0][5] == 0x7fffffffu);
        assert(n.control[19] == (1u << 4));
    }


    /* Malformed register pairs halt rather than guess.  The manual's operand
     * tables give "dp" and "xdp" without defining a misaligned field, and
     * TI's assembler cannot emit one. */
    static const unsigned pair_dst[] = {
        ABSDP, SPDP, ADDDP_L, ADDDP_S, SUBDP_L, SUBDP_LR, SUBDP_S, SUBDP_SR,
        MPYDP, MPYSPDP, MPYSP2DP, INTDP, INTDPU,
    };
    for (unsigned i = 0; i < sizeof(pair_dst) / sizeof(pair_dst[0]); ++i) {
        /* src2 = 3 is odd, which ABSDP wants and the even-pair forms do not;
         * pick a src2 each form accepts so only dst is at fault. */
        unsigned src2 = pair_dst[i] == ABSDP ? 3 : 2;
        load(&c, dp_word(5, src2, 0, 0, pair_dst[i], 0));
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.fault && !strcmp(c.fault,
            "invalid double-precision result register pair"));
    }
    /* Even src2 where the odd (high) register must be named, and odd src1 or
     * src2 where the even (low) register must be. */
    static const unsigned odd_src2[] = {ABSDP, DPSP, DPINT, DPTRUNC};
    for (unsigned i = 0; i < 4; ++i) {
        load(&c, dp_word(4, 2, 0, 0, odd_src2[i], 0));
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.fault && !strcmp(c.fault,
            "invalid double-precision source register pair"));
    }
    static const unsigned even_sources[] = {
        CMPEQDP, CMPGTDP, CMPLTDP, ADDDP_L, ADDDP_S, SUBDP_L, SUBDP_LR,
        SUBDP_S, SUBDP_SR, MPYDP,
    };
    for (unsigned i = 0; i < sizeof(even_sources) / sizeof(even_sources[0]);
         ++i) {
        load(&c, dp_word(4, 3, 0, 0, even_sources[i], 0));
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.fault && !strcmp(c.fault,
            "invalid double-precision source register pair"));
        load(&c, dp_word(4, 2, 1, 0, even_sources[i], 0));
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.fault && !strcmp(c.fault,
            "invalid double-precision source register pair"));
    }
    /* MPYSPDP's src2 is a pair but its src1 is a plain SP register, so an odd
     * src1 is legal there; MPYSP2DP takes two SP registers. */
    load(&c, dp_word(4, 2, 1, 0, MPYSPDP, 0));
    run(&c, 7);
    load(&c, dp_word(4, 3, 1, 0, MPYSP2DP, 0));
    run(&c, 5);
    load(&c, dp_word(4, 3, 0, 0, MPYSPDP, 0));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.fault && !strcmp(c.fault,
        "invalid double-precision source register pair"));

    /* A rejected encoding rolls the whole packet back: a parallel MVK in the
     * same packet must not have written. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = (3u << 23) | (99u << 7) | 0x28 | 1;   /* MVK .S1 99,A3 || */
    memory[1] = dp_word(5, 2, 0, 0, ADDDP_L, 0);
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 0 && !c.cycles && c.pc == 0x1000);
}

static void test_predication_and_stickiness(void)
{
    CdjC674x c;
    /* A false predicate writes nothing and raises no warning bit, exactly as
     * the Execution box's "else nop" says. */
    load(&c, dp_word(4, 2, 0, 0, ADDDP_L, 0) | (1u << 29));  /* [B0] */
    set_pair(&c, 0, 0, UINT64_C(0x0000000000000001));
    set_pair(&c, 0, 2, UINT64_C(0x3ff0000000000000));
    c.r[1][0] = 0;
    c.r[0][4] = c.r[0][5] = 0xdeadbeef;
    run(&c, 7);
    assert(c.r[0][4] == 0xdeadbeef && c.r[0][5] == 0xdeadbeef);
    assert(!c.control[18] && !c.load_count);
    /* The same encoding with the predicate true does write both halves. */
    load(&c, dp_word(4, 2, 0, 0, ADDDP_L, 0) | (1u << 29));
    set_pair(&c, 0, 0, UINT64_C(0x0000000000000001));
    set_pair(&c, 0, 2, UINT64_C(0x3ff0000000000000));
    c.r[1][0] = 1;
    run(&c, 7);
    assert(get_pair(&c, 0, 4) == UINT64_C(0x3ff0000000000000));
    assert(c.control[18] == 0x084);

    /* Warning bits accumulate; they are never cleared by a later clean
     * operation (Table 2-25, printed page 59: each is a sticky status bit). */
    load(&c, dp_word(4, 2, 0, 0, CMPGTDP, 0));
    set_pair(&c, 0, 0, UINT64_C(0x7ff8000000000000));
    set_pair(&c, 0, 2, UINT64_C(0x3ff0000000000000));
    c.control[19] = 1u << 26;          /* pre-existing .S2 DIV0 */
    run(&c, 2);
    assert(c.r[0][4] == 0 && c.control[19] == ((1u << 26) | 0x211));
}

/* ---- ADDDP/SUBDP use FADCR even on .S ------------------------------------
 *
 * Note 1 on the ADDDP and SUBDP pages: "This instruction takes the rounding
 * mode from and sets the warning bits in the floating-point adder
 * configuration register (FADCR), not in the floating-point auxiliary
 * configuration register (FAUCR) as for other .S unit instructions."
 *
 * The .L forms would use FADCR anyway, so only the .S forms actually test the
 * exception the note carves out.  This was correct in the implementation but
 * pinned by nothing, so an .S form rerouted to FAUCR would have gone unnoticed;
 * that gap is what this closes.
 *
 * 1.0 + 2^-60 is used because the exact sum needs more than 53 significand
 * bits, so it must round and must therefore raise INEX somewhere - which is
 * what makes "in FADCR and not in FAUCR" an observable distinction rather than
 * two registers that both happen to stay zero.  DERIVED, not transcribed: the
 * manual prints no example with an inexact double sum. */
static void test_adddp_subdp_warn_in_fadcr_not_faucr(void)
{
    static const struct { const char *name; unsigned encoding; } forms[] = {
        { "ADDDP .L", ADDDP_L }, { "ADDDP .S", ADDDP_S },
        { "SUBDP .L", SUBDP_L }, { "SUBDP .S", SUBDP_S },
    };
    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
        CdjC674x c;
        load(&c, dp_word(4, 2, 0, 0, forms[i].encoding, 0));
        set_pair(&c, 0, 0, UINT64_C(0x3FF0000000000000));   /* 1.0   */
        set_pair(&c, 0, 2, UINT64_C(0x3C30000000000000));   /* 2^-60 */
        run(&c, 10);
        /* INEX is FADCR bit 7 for the adder (Table 2-20, printed page 51). */
        assert(c.control[18] == 0x80);
        assert(c.control[19] == 0);
        assert(c.control[20] == 0);
    }
}

/* TI asm6x and older GNU tic6x encode the unused src1 field differently for
 * DPSP/DPINT/DPTRUNC when the source pair is not A1:A0 or B1:B0.  Both encodings
 * name the same pair through src2.  A distinct pair at registers 0/1 makes a
 * mistaken src1 read visible; the result and FADCR must still be identical. */
static void test_conversion_source_pair_encoding_variants(void)
{
    static const struct { unsigned encoding, expected; } forms[] = {
        {DPSP, 0x4109999au}, {DPINT, 9u}, {DPTRUNC, 8u},
    };
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned variant = 0; variant < 2; ++variant) {
            for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
                CdjC674x c;
                load(&c, dp_word(8, 5, variant ? 4 : 0, 0,
                                 forms[i].encoding, side));
                set_pair(&c, side, 0, DP_8_6 | UINT64_C(0x8000000000000000));
                set_pair(&c, side, 4, DP_8_6);
                c.r[side][8] = 0xdeadbeefu;
                run(&c, 3);
                assert(c.r[side][8] == 0xdeadbeefu);
                run(&c, 1);
                assert(c.r[side][8] == forms[i].expected);
                assert(c.control[18] == (0x80u << (side ? 16 : 0)));
            }
        }
    }
}

int main(void)
{
    test_encodings_match_ti_assembler();
    test_manual_examples();
    test_compare_tables();
    test_derived_add_sub_dp();
    test_derived_multiply_dp();
    test_derived_conversions();
    test_rejections();
    test_predication_and_stickiness();
    test_adddp_subdp_warn_in_fadcr_not_faucr();
    test_conversion_source_pair_encoding_variants();
    printf("c674x double-precision tests passed\n");
    return 0;
}
