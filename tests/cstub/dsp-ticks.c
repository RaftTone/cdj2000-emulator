/* SPDX-License-Identifier: GPL-2.0-or-later
 * Batched board ticks (cdj_dsp_ticks.h) against per-cycle ticking.
 *
 * 1. Random PLL/timer/SPI states: whenever cdj_dsp_ticks_quiet holds, n
 *    per-cycle ticks raise no timer event and leave exactly the state
 *    cdj_dsp_ticks_apply(n) does.
 * 2. The boards' protocol in lockstep: system A ticks every cycle; system B
 *    only counts ticks while steady, re-tests quiet after each full tick and
 *    flushes before every register access.  Random tick runs and register
 *    writes (PLL GO and lock sequences, timer starts and stops, SPI idle
 *    and active) must leave both equal at every flush and deliver the same
 *    timer events at the same cycles.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_dsp_ticks.h"

typedef struct {
    CdjC6747Pll pll;
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    CdjC6747Spi spis[2];
    CdjC6747SpiTransfer transfer;
    CdjWm8740 dac;
} Board;

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static unsigned below(unsigned n) { return rnd() % n; }

static bool ft;

/* The boards' per-cycle tick (cdj2000_nxs_hpi.c dsp_cycle_tick, replay.c
 * cycle_tick) minus logging; returns the timer outputs. */
static uint32_t full_tick(Board *b)
{
    if (!ft) cdj_spi_core_tick(b->spis, &b->dac, &b->transfer, &b->pll);
    cdj_c6747_pll_tick(&b->pll);
    return cdj_c6747_timers_tick(b->timers);
}

static bool same(const Board *a, const Board *b)
{
    return !memcmp(&a->pll, &b->pll, sizeof(a->pll)) &&
           !memcmp(a->timers, b->timers, sizeof(a->timers)) &&
           !memcmp(a->spis, b->spis, sizeof(a->spis)) &&
           !memcmp(&a->transfer, &b->transfer, sizeof(a->transfer)) &&
           !memcmp(&a->dac, &b->dac, sizeof(a->dac));
}

static const unsigned pll_offsets[] = {0x100, 0x104, 0x110, 0x114, 0x118,
    0x11c, 0x120, 0x124, 0x128, 0x138, 0x160, 0x164, 0x168, 0x16c};

/* SPI1 set up for the WM8740 as the firmware does (c6747-spi-timed.c), so
 * a word write starts a genuine timed transfer. */
static void configure_spi(Board *b)
{
    static const uint32_t setup[][2] = {
        {0, 1}, {4, 3}, {0x14, 0xe01}, {0x3c, 0}, {0x3c, 0},
        {0x50, 0x21810}, {0x48, 0x02020408}, {8, 0}, {0xc, 0},
        {4, 0x01000003},
    };
    for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i)
        assert(cdj_c6747_spi_wm8740_write_timed(
            b->spis, &b->dac, &b->transfer, CDJ_C6747_SPI1_BASE + setup[i][0],
            setup[i][1], 4, true));
}

static bool spi_word(Board *b, uint32_t word)
{
    return cdj_c6747_spi_wm8740_write_timed(b->spis, &b->dac, &b->transfer,
                                            CDJ_C6747_SPI1_BASE + 0x3c, word,
                                            4, true);
}

static void random_state(Board *b)
{
    memset(b, 0, sizeof(*b));
    cdj_c6747_pll_reset(&b->pll);
    cdj_c6747_timers_reset(b->timers);
    cdj_c6747_spis_reset(b->spis);
    cdj_c6747_spi_transfer_reset(&b->transfer);
    cdj_wm8740_reset(&b->dac);
    for (unsigned i = below(6); i; --i) {
        uint32_t value = below(2) ? rnd() : (0x8000u | below(32));
        cdj_c6747_pll_write(&b->pll, 0x01c11000u +
                            pll_offsets[below(sizeof pll_offsets /
                                              sizeof pll_offsets[0])],
                            below(3) ? value : below(0x200), 4, true);
        for (unsigned t = below(20); t; --t) cdj_c6747_pll_tick(&b->pll);
    }
    if (!below(3)) b->pll.go_remaining = below(9);
    if (!below(3)) b->pll.lock_wait_remaining = below(500);
    if (!below(3)) b->pll.reset_age = below(18);
    b->pll.oscin_phase = below(40);
    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i) {
        CdjC6747Timer *t = &b->timers[i];
        if (below(2)) continue;
        t->tgcr = rnd() & 0xff1fu;
        t->tcr = below(2) ? rnd() & 0x00c000c0u : 0;
        t->tim12 = below(64); t->tim34 = below(64);
        t->prd12 = below(64); t->prd34 = below(64);
        t->rel12 = below(64); t->rel34 = below(64);
        if (below(4) == 0) t->compare[below(8)] = below(64);
    }
    if (below(3) == 0) {
        /* A genuine transfer, some way through. */
        configure_spi(b);
        spi_word(b, rnd() & 0xfff);
        if (below(2)) spi_word(b, rnd() & 0xfff);
        cdj_c6747_spi_wm8740_advance(b->spis, &b->dac, &b->transfer,
                                     below(1500));
    } else if (below(4) == 0) {
        b->transfer.phase = below(4);
        b->transfer.half_ticks_remaining = below(10);
        b->transfer.queued_valid = below(2);
    }
    if (below(8) == 0) b->transfer.fault = 1;
    b->transfer.clock_phase = below(4);
    if (below(8) == 0) b->dac.program[below(5)] = rnd() & 0x3ff;
}

/* Part 1: the header's claim, state by state. */
static void quiet_states(void)
{
    unsigned quiet = 0, loud = 0;
    for (unsigned trial = 0; trial < 200000; ++trial) {
        Board a;
        ft = below(4) == 0;
        random_state(&a);
        if (!cdj_dsp_ticks_quiet(a.timers, &a.transfer, &a.dac, ft)) {
            ++loud;
            continue;
        }
        ++quiet;
        Board b = a;
        uint64_t n = 1 + below(below(2) ? 4 : 5000);
        for (uint64_t i = 0; i < n; ++i) assert(!full_tick(&a));
        assert(cdj_dsp_ticks_quiet(a.timers, &a.transfer, &a.dac, ft));
        cdj_dsp_ticks_apply(b.spis, &b.dac, &b.transfer, &b.pll, n, ft);
        assert(same(&a, &b));
    }
    printf("quiet states %u, not quiet %u\n", quiet, loud);
    assert(quiet > 20000 && loud > 20000);
}

/* Part 2: the boards' protocol, lockstep. */
typedef struct {
    Board b;
    CdjDspTicks ticks;
    uint64_t events[64];
    unsigned event_count;
} Lazy;

static uint64_t now;

static void lazy_flush(Lazy *l)
{
    if (l->ticks.debt)
        cdj_dsp_ticks_apply(l->b.spis, &l->b.dac, &l->b.transfer, &l->b.pll,
                            l->ticks.debt, ft);
    l->ticks.debt = 0;
    l->ticks.steady = false;
}

static void lazy_tick(Lazy *l)
{
    if (l->ticks.steady) {
        ++l->ticks.debt;
        return;
    }
    uint32_t out = full_tick(&l->b);
    if (out && l->event_count < 64) l->events[l->event_count++] = now ^ out;
    l->ticks.steady = cdj_dsp_ticks_quiet(l->b.timers,
                                          &l->b.transfer, &l->b.dac, ft);
}

static void write_both(Board *a, Lazy *l)
{
    lazy_flush(l);
    uint32_t value;
    switch (below(5)) {
    case 0: case 1: {
        uint32_t address = 0x01c11000u +
            pll_offsets[below(sizeof pll_offsets / sizeof pll_offsets[0])];
        value = below(2) ? (0x8000u | below(32)) : below(0x200);
        if (address == 0x01c11138u) value = below(2);
        bool ok = cdj_c6747_pll_write(&a->pll, address, value, 4, true);
        assert(ok == cdj_c6747_pll_write(&l->b.pll, address, value, 4, true));
        break;
    }
    case 2: case 3: {
        /* TGCR, TCR, PRD12, TIM12, INTCTLSTAT of either timer. */
        static const unsigned regs[] = {0x24, 0x20, 0x18, 0x10, 0x44};
        uint32_t address = (below(2) ? CDJ_C6747_TIMER1_BASE :
                            CDJ_C6747_TIMER0_BASE) + regs[below(5)];
        value = below(2) ? rnd() & 0x00c000c0u : below(2) ? 0x7u : below(40);
        bool ok = cdj_c6747_timers_write(a->timers, address, value, 4, true);
        assert(ok == cdj_c6747_timers_write(l->b.timers, address, value, 4,
                                            true));
        break;
    }
    case 4:
        if (below(2)) {
            /* A WM8740 word, starting (or queueing) a genuine transfer. */
            uint32_t word = rnd() & 0xfff;
            bool ok = spi_word(a, word);
            assert(ok == spi_word(&l->b, word));
            break;
        }
        /* An SPI transfer state forced idle or active, as a register write
         * could leave it. */
        if (below(2)) {
            a->transfer.phase = l->b.transfer.phase = 0;
            a->transfer.queued_valid = l->b.transfer.queued_valid = 0;
            a->transfer.half_ticks_remaining = 0;
            l->b.transfer.half_ticks_remaining = 0;
        } else {
            unsigned h = 1 + below(8);
            a->transfer.phase = l->b.transfer.phase = 1 + below(3);
            a->transfer.half_ticks_remaining = h;
            l->b.transfer.half_ticks_remaining = h;
        }
        break;
    }
}

static void lockstep(void)
{
    uint64_t steady_ticks = 0, total = 0;
    for (unsigned trial = 0; trial < 3000; ++trial) {
        Board a;
        Lazy l = {0};
        ft = below(3) == 0;
        random_state(&a);
        /* Faults end a trial's interest early: keep them out of the start. */
        a.transfer.fault = 0;
        l.b = a;
        uint64_t events[64];
        unsigned event_count = 0;
        now = 0;
        for (unsigned op = 0; op < 200; ++op) {
            if (below(4)) {
                for (unsigned n = 1 + below(below(2) ? 3 : 3000); n; --n) {
                    ++now;
                    uint32_t out = full_tick(&a);
                    if (out && event_count < 64) events[event_count++] = now ^ out;
                    lazy_tick(&l);
                    steady_ticks += l.ticks.steady;
                    ++total;
                }
            } else {
                write_both(&a, &l);
                assert(same(&a, &l.b));
            }
        }
        lazy_flush(&l);
        assert(same(&a, &l.b));
        assert(event_count == l.event_count);
        assert(!memcmp(events, l.events, event_count * sizeof(events[0])));
    }
    printf("lockstep ticks %llu, counted while steady %llu\n",
           (unsigned long long)total, (unsigned long long)steady_ticks);
    assert(steady_ticks > total / 4 && steady_ticks < total);
}

int main(void)
{
    quiet_states();
    lockstep();
    puts("dsp ticks: ok");
    return 0;
}
