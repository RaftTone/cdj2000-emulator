/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_pll.h"
int main(void)
{
    CdjC6747Pll s, before;
    cdj_c6747_pll_reset(&s);
    unsigned offsets[] = {0x100,0x104,0x110,0x114,0x118,0x11c,0x120,
                          0x124,0x128,0x160,0x164,0x168,0x16c};
    uint32_t defaults[] = {0xf2,0x14,0x13,0x8000,0x8000,0x8001,0x8002,
                           0x8000,0x8001,0x8003,0x8002,0x8000,0x8005};
    uint32_t v;
    for (unsigned i = 0; i < 13; ++i) {
        uint32_t a = 0x01c11000 + offsets[i];
        assert(cdj_c6747_pll_read(&s, a, &v) && v == defaults[i]);
        uint32_t candidate = i == 0 ? 0x1d0 : i == 1 ? 0x1f : i == 2 ? 31 : 0x801f;
        before = s;
        assert(cdj_c6747_pll_write(&s, a, candidate, 4, false));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_pll_write(&s, a, candidate, 4, true));
        assert(cdj_c6747_pll_read(&s, a, &v) && v == candidate);
        before = s;
        assert(!cdj_c6747_pll_write(&s, a, UINT32_MAX, 4, true));
        assert(!cdj_c6747_pll_write(&s, a, 0, 1, true));
        assert(!cdj_c6747_pll_write(&s, a+1, 0, 4, true));
        assert(!memcmp(&s, &before, sizeof(s)));
    }
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x10, 4, true));
    assert(cdj_c6747_pll_read(&s, 0x01c11100, &v) && v == 0xd0);
    assert(!s.legacy_bit4_used);
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c0, 4, false));
    assert(!s.legacy_bit4_used);
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c0, 4, true));
    assert(s.legacy_bit4_used);
    assert(cdj_c6747_pll_read(&s, 0x01c11100, &v) && v == 0x1c0);
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1d0, 4, true));
    assert(s.legacy_bit4_used); /* Diagnostic is sticky until reset. */
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0xd8, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0xd1, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11104, 0x15, 4, true));
    assert(cdj_c6747_pll_read(&s, 0x01c1113c, &v) && v == 4);
    before = s;
    assert(cdj_c6747_pll_write(&s, 0x01c11138, 1, 4, false));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(cdj_c6747_pll_write(&s, 0x01c11138, 1, 4, true));
    assert(cdj_c6747_pll_read(&s, 0x01c1113c, &v) && v == 5);
    assert(!cdj_c6747_pll_write(&s, 0x01c11138, 1, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11118, 0x8001, 4, true));
    assert(cdj_c6747_pll_write(&s, 0x01c11138, 0, 4, true));
    assert(cdj_c6747_pll_read(&s, 0x01c11138, &v) && v == 0);
    for (unsigned i = 0; i < 7; ++i) cdj_c6747_pll_tick(&s);
    assert(s.active_dividers[0] == 0x8000 && s.go_remaining == 1);
    cdj_c6747_pll_tick(&s);
    assert(s.active_dividers[0] == 0x801f && !s.go_remaining);
    assert(cdj_c6747_pll_read(&s, 0x01c1113c, &v) && v == 4);
    cdj_c6747_pll_reset(&s);
    assert(!s.legacy_bit4_used);
    for (unsigned i = 0; i < 13; ++i) assert(s.config[i] == defaults[i]);
    /* Board-specific reset/lock timing, SPRS377F Table 6-4. No status bit
     * changes merely because the conservative lock-wait bound elapses. */
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c0, 4, true));
    assert(cdj_c6747_pll_write(&s, 0x01c11110, 22, 4, true));
    assert(cdj_c6747_pll_write(&s, 0x01c11128, 0x8000, 4, true));
    for (unsigned i = 0; i < 16; ++i) cdj_c6747_pll_tick(&s);
    before = s;
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, true));
    assert(!memcmp(&s, &before, sizeof(s)));
    cdj_c6747_pll_tick(&s);
    before = s;
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, false));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, true));
    assert(s.lock_wait_remaining == 418 && s.oscin_cycles == 17);
    assert(!cdj_c6747_pll_write(&s, 0x01c11110, 23, 4, true));
    for (unsigned i = 0; i < 417; ++i) cdj_c6747_pll_tick(&s);
    assert(s.lock_wait_remaining == 1);
    cdj_c6747_pll_tick(&s);
    assert(!s.lock_wait_remaining && s.oscin_cycles == 435);
    assert(cdj_c6747_pll_read(&s, 0x01c1113c, &v) && v == 4);
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c9, 4, true));
    assert(!s.early_enable);
    for (unsigned i = 0; i < 22; ++i) cdj_c6747_pll_tick(&s);
    assert(s.oscin_cycles == 435 && s.oscin_phase == 22);
    cdj_c6747_pll_tick(&s);
    assert(s.oscin_cycles == 436 && !s.oscin_phase);
    assert(cdj_c6747_pll_read(&s, 0x01c1113c, &v) && v == 4);
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c9, 4, true));
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c0, 4, true));
    assert(!s.reset_age && !s.lock_wait_remaining);
    /* PREDIV=2 violates this board's minimum PLL reference frequency. */
    assert(cdj_c6747_pll_write(&s, 0x01c11114, 0x8001, 4, true));
    for (unsigned i = 0; i < 17; ++i) cdj_c6747_pll_tick(&s);
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, true));
    /* Active /4 SYSCLK1 means four OSCIN periods per bypass CPU cycle. */
    cdj_c6747_pll_reset(&s);
    assert(cdj_c6747_pll_write(&s, 0x01c11118, 0x8003, 4, true));
    assert(cdj_c6747_pll_write(&s, 0x01c11138, 1, 4, true));
    for (unsigned i = 0; i < 8; ++i) cdj_c6747_pll_tick(&s);
    assert(s.oscin_cycles == 8);
    cdj_c6747_pll_tick(&s);
    assert(s.oscin_cycles == 12);
    for (unsigned m = 1; m <= 32; ++m) {
        cdj_c6747_pll_reset(&s);
        assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c0, 4, true));
        assert(cdj_c6747_pll_write(&s, 0x01c11110, m - 1, 4, true));
        assert(cdj_c6747_pll_write(&s, 0x01c11128, 0x8000, 4, true));
        for (unsigned i = 0; i < 17; ++i) cdj_c6747_pll_tick(&s);
        before = s;
        assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0x1e8, 4, true)); /* PLLENSRC */
        assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0x1ca, 4, true)); /* powered down */
        assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0xc8, 4, true)); /* wrong input */
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, true) == (m >= 18));
        if (m >= 18) {
            unsigned w = s.lock_wait_remaining;
            assert((uint64_t)w * w * m >= 4000000);
            assert((uint64_t)(w - 1) * (w - 1) * m < 4000000);
            if (m == 18) assert(w == 472);
            if (m == 23) assert(w == 418);
            if (m == 32) assert(w == 354);
            for (unsigned i = 0; i < 5; ++i) cdj_c6747_pll_tick(&s);
            assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, true));
            assert(s.lock_wait_remaining == w - 5); /* readback write doesn't restart */
            assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c9, 4, true));
            assert(s.early_enable && s.oscin_phase == 0);
            unsigned remaining = w - 5;
            for (unsigned i = 0; i < m * remaining - 1; ++i) cdj_c6747_pll_tick(&s);
            assert(s.lock_wait_remaining == 1);
            cdj_c6747_pll_tick(&s);
            assert(!s.lock_wait_remaining && !s.oscin_phase);
            assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c8, 4, true));
            assert(s.early_enable); /* Diagnostic remains sticky. */
            assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x1c2, 4, true));
            assert(!s.lock_wait_remaining && !s.reset_age);
        }
    }
    /* cdj_c6747_pll_ticks(n) is exactly n single ticks: PLL and bypass mode,
     * every multiplier and divider, and with GO, lock-wait and reset-age
     * countdowns still running when it starts. */
    uint32_t seed = 12345;
    for (unsigned trial = 0; trial < 4000; ++trial) {
        CdjC6747Pll a;
        cdj_c6747_pll_reset(&a);
        seed = seed * 1103515245u + 12345u;
        a.config[0] = (seed >> 8) & 0x1ff;
        a.config[2] = (seed >> 3) % 32;
        a.config[3] = ((seed >> 12) & 1 ? 0x8000u : 0) | ((seed >> 13) & 31);
        a.config[8] = ((seed >> 18) & 1 ? 0x8000u : 0) | ((seed >> 19) & 31);
        a.active_dividers[0] = ((seed >> 24) & 1 ? 0x8000u : 0) | ((seed >> 25) & 31);
        a.target_dividers[0] = ((seed >> 7) & 1 ? 0x8000u : 0) | ((seed >> 2) & 31);
        a.oscin_phase = (seed >> 5) % (a.config[2] + 1);
        a.oscin_cycles = seed;
        a.go_remaining = trial % 3 ? 0 : (seed >> 9) % 10;
        a.lock_wait_remaining = trial % 5 ? 0 : (seed >> 11) % 40;
        a.reset_age = trial % 7 ? 17 : (seed >> 14) % 17;
        CdjC6747Pll b = a;
        uint64_t n = trial < 100 ? trial : (seed >> 4) % 100000;
        for (uint64_t i = 0; i < n; ++i) cdj_c6747_pll_tick(&a);
        cdj_c6747_pll_ticks(&b, n);
        assert(!memcmp(&a, &b, sizeof(a)));
    }
    return 0;
}
