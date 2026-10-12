/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DP source pairs are read half by half (SPRUFE8B Tables 4-15, 4-16, 4-19,
 * 4-20): ADDDP, SUBDP and the compares read src_l on E1 and src_h on E2,
 * MPYSPDP src2_l on E1 and src2_h on E2, MPYDP src1_h on E3.  TI's
 * __c6xabi_divf issues SUBDP on the cycle the low word of a MPYSP2DP result
 * lands, so the high word lands exactly on E2; reading both halves at issue
 * returned the stale high word and 1.0f/1024.0f came out as 2^-17.
 *
 * Encodings are TI asm6x -mv6740 output.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

#define NOP(n) ((uint32_t)((n) - 1) << 13)
#define MPYSP2DP_A5_A3_A7A6 0x030ca5f0u
#define SUBDP_A9A8_A7A6_A11A10 0x05190338u
#define ADDDP_A9A8_A7A6_A11A10 0x05190318u
#define MPYSPDP_A3_A7A6_A13A12 0x061865b0u
#define CMPEQDP_A7A6_A15A14_A2 0x0138ca20u
#define MPYDP_A7A6_A9A8_A13A12 0x0620c700u
#define LDW_A4_A7 0x03900264u
#define LDW_A4_A9 0x04900264u    /* dst 9 */

static uint32_t code[16];
static bool read_word(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address >= 0x1000 && address < 0x1040 && !(address & 3)) {
        *value = code[(address - 0x1000) / 4];
        return true;
    }
    if (address == 0x2000) { *value = 0x3ff00000; return true; }   /* 1.0 high */
    return false;
}
static bool write_word(void *unused, uint32_t a, uint64_t v, unsigned s, bool c)
{
    (void)unused; (void)a; (void)v; (void)s; (void)c;
    return false;
}

static CdjC674x run(unsigned count, const uint32_t *words)
{
    CdjC674x c;
    memcpy(code, words, count * sizeof *words);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = c.r[0][5] = 0x3f800000;            /* 1.0f */
    c.r[0][9] = 0x40000000;                        /* A9:A8 = 2.0 */
    c.r[0][15] = 0x3ff00000;                       /* A15:A14 = 1.0 */
    c.r[0][4] = 0x2000;
    for (unsigned i = 0; i < count; ++i) {
        bool ok = cdj_c674x_step(&c, read_word, write_word, NULL);
        if (!ok) { fprintf(stderr, "fault: %s\n", c.fault); assert(ok); }
    }
    return c;
}
static uint64_t pair(const CdjC674x *c, unsigned reg)
{ return (uint64_t)c->r[0][reg + 1] << 32 | c->r[0][reg]; }

int main(void)
{
    const uint64_t one = UINT64_C(0x3ff0000000000000), two = UINT64_C(0x4000000000000000);

    /* MPYSP2DP: low on E4, high on E5.  SUBDP on E4 sees the high word on E2. */
    CdjC674x c = run(4, (uint32_t[]){MPYSP2DP_A5_A3_A7A6, NOP(3),
                                      SUBDP_A9A8_A7A6_A11A10, NOP(7)});
    assert(pair(&c, 6) == one && pair(&c, 10) == one);   /* 2.0 - 1.0 */
    c = run(4, (uint32_t[]){MPYSP2DP_A5_A3_A7A6, NOP(3),
                            ADDDP_A9A8_A7A6_A11A10, NOP(7)});
    assert(pair(&c, 10) == UINT64_C(0x4008000000000000));  /* 2.0 + 1.0 */
    c = run(4, (uint32_t[]){MPYSP2DP_A5_A3_A7A6, NOP(3),
                            MPYSPDP_A3_A7A6_A13A12, NOP(7)});
    assert(pair(&c, 12) == one);                          /* 1.0f * 1.0 */
    c = run(4, (uint32_t[]){MPYSP2DP_A5_A3_A7A6, NOP(3),
                            CMPEQDP_A7A6_A15A14_A2, NOP(2)});
    assert(c.r[0][2] == 1);                               /* 1.0 == 1.0 */

    /* MPYDP: src1_h is first read on E3.  An LDW lands 5 cycles after issue. */
    c = run(5, (uint32_t[]){LDW_A4_A7, NOP(2), MPYDP_A7A6_A9A8_A13A12, NOP(9), NOP(2)});
    assert(pair(&c, 12) == two);                      /* lands on E3: 1.0 * 2.0 */
    c = run(5, (uint32_t[]){LDW_A4_A7, NOP(1), MPYDP_A7A6_A9A8_A13A12, NOP(9), NOP(2)});
    assert(pair(&c, 12) == 0);                            /* one cycle late: stale 0 */
    /* src2_h is first read on E2: a high half landing on E2 counts. */
    {
        uint32_t words[] = {LDW_A4_A9, NOP(3), MPYDP_A7A6_A9A8_A13A12, NOP(9), NOP(2)};
        memcpy(code, words, sizeof words);
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = 0x2000; c.r[0][7] = 0x3ff00000;       /* A7:A6 = 1.0 */
        for (unsigned i = 0; i < 5; ++i) assert(cdj_c674x_step(&c, read_word, write_word, NULL));
        assert(pair(&c, 12) == one);                      /* 1.0 * 1.0 */
    }
    puts("ok");
    return 0;
}
