/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Audit characterization test (Track 1, instruction encodings and numerical
 * semantics).  The existing reference-backed SP tests in tests/cstub/c674x.c
 * cover 18 hand-picked MPYSP cases and 21 SPINT cases, all special values.
 * They do not establish that the hand-rolled integer MPYSP/ADDSP/SUBSP paths
 * are *correctly rounded* over the normal range in all four FADCR/FMCR modes
 * (SPRUFE8B Table 2-25 RMODE, printed page 59; Table 2-27, printed page 63).
 *
 * Oracle: the host FPU under fesetround().  For SP normal operands the exact
 * product fits in a double (24+24 = 48 <= 53 significand bits), and so does
 * the exact sum when the operand exponents are within 25 of each other
 * (24+25 = 49 bits), so the single double->float conversion performed here is
 * one correctly-rounded SP rounding.  The expected value therefore does not
 * come from the emulator.  Samples whose SP result is not normal (overflow,
 * underflow, subnormal) are skipped: C674x flushes those, and the existing
 * hand-written cases already cover that behaviour.
 *
 * ADDSP/SUBSP read FADCR (control 18) and MPYSP reads FMCR (control 20);
 * .L1/.M1 RMODE is bits 10-9 and the warning bits are 8-0.  For normal
 * operands and a normal result the only documented warning is INEX (bit 7).
 */
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static uint32_t state = 0x12345678u;
static uint32_t rnd(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static uint32_t bits_of(float f)
{
    uint32_t w;
    memcpy(&w, &f, sizeof w);
    return w;
}

static float float_of(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

/* Build an SP normal with an exponent in [lo, hi]. */
static uint32_t normal_sp(unsigned lo, unsigned hi)
{
    uint32_t r = rnd();
    unsigned exponent = lo + (r % (hi - lo + 1));
    return (r & 0x80000000u) | exponent << 23 | (rnd() & 0x7fffffu);
}

static const int modes[4] = {
    FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD,
};

/* operation: 0 = MPYSP (FMCR), 1 = ADDSP (FADCR), 2 = SUBSP (FADCR). */
static unsigned run(unsigned operation, unsigned rmode, uint32_t wa, uint32_t wb,
                    uint32_t *value, uint32_t *status)
{
    static const uint32_t opfield[3] = {0xe00u, 0x218u, 0x238u};
    CdjC674x c;
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = wa;
    c.r[0][4] = wb;
    c.r[0][5] = 0xdeadbeefu;
    c.control[operation ? 18 : 20] = rmode << 9;
    uint32_t word = 5u << 23 | 4u << 18 | 3u << 13 | opfield[operation];
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = 0x1000, .compact = false}},
        .count = 1, .next_pc = 0x1004,
    };
    if (!cdj_c674x_execute(&c, &p, NULL, NULL, NULL)) {
        fprintf(stderr, "sp-rounding: execute rejected %08x: %s\n",
                word, c.fault ? c.fault : "(no fault text)");
        return 0;
    }
    if (c.load_count != 1) {
        fprintf(stderr, "sp-rounding: expected one delayed result, got %u\n",
                c.load_count);
        return 0;
    }
    *value = (uint32_t)c.loads[0].value;
    *status = c.loads[0].address;
    return 1;
}

int main(void)
{
    unsigned checked[3] = {0, 0, 0}, failures = 0;
    for (unsigned operation = 0; operation < 3; ++operation)
    for (unsigned rmode = 0; rmode < 4; ++rmode)
    for (unsigned trial = 0; trial < 20000; ++trial) {
        uint32_t wa, wb;
        if (operation == 0) {
            /* Keep the product's exponent well inside the normal range. */
            wa = normal_sp(64, 190);
            wb = normal_sp(64, 190);
        } else {
            wa = normal_sp(40, 214);
            unsigned ea = (wa >> 23) & 255;
            unsigned lo = ea > 25 ? ea - 25 : 1, hi = ea + 25 < 254 ? ea + 25 : 254;
            wb = normal_sp(lo, hi);
        }
        double exact;
        double da = (double)float_of(wa), db = (double)float_of(wb);
        if (operation == 0) exact = da * db;
        else if (operation == 1) exact = da + db;
        else exact = da - db;

        int previous = fegetround();
        if (fesetround(modes[rmode])) return fprintf(stderr, "fesetround\n"), 1;
        volatile float rounded = (float)exact;
        float expected = rounded;
        fesetround(previous);

        /* Only normal, finite expected results are in scope here. */
        if (!isfinite(expected) || expected == 0.0f ||
            fabsf(expected) < 1.17549435e-38f) continue;
        bool inexact = (double)expected != exact;

        uint32_t value = 0, status = 0;
        if (!run(operation, rmode, wa, wb, &value, &status)) return 1;
        uint32_t want_status = inexact ? 1u << 7 : 0u;
        if (value != bits_of(expected) || status != want_status) {
            if (failures < 12)
                fprintf(stderr,
                        "sp-rounding MISMATCH op=%u rmode=%u a=%08x b=%08x: "
                        "got %08x/status %03x want %08x/status %03x\n",
                        operation, rmode, wa, wb, value, status,
                        bits_of(expected), want_status);
            ++failures;
        }
        ++checked[operation];
    }
    printf("sp-rounding: checked MPYSP=%u ADDSP=%u SUBSP=%u, failures=%u\n",
           checked[0], checked[1], checked[2], failures);
    assert(!failures);
    return 0;
}
