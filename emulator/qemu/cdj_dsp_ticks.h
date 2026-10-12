/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_TICKS_H
#define CDJ_DSP_TICKS_H
/*
 * The boards' per-cycle tick, batched (PERFORMANCE.md, "Batched board
 * ticks").  The C674x core calls the board's cycle_tick once per CPU cycle;
 * the tick clocks the SPI1/WM8740 shift logic (strict timing only), the PLL's
 * OSCIN-period counter and the two Timer64Ps, whose outputs are INTC events.
 *
 * In the steady state that cdj_dsp_ticks_quiet() tests - both timers stopped
 * (the condition under which cdj_c6747_timers_tick changes nothing) and the
 * SPI transfer idle in cdj_spi_core_tick's fast path (or functional timing,
 * which does not tick it) - n consecutive ticks are exactly
 * cdj_dsp_ticks_apply(n).  The timers do nothing.  cdj_c6747_pll_ticks(n) is
 * exactly n PLL ticks, countdowns included (tests/cstub/c6747-pll.c).  An
 * idle SPI tick only sets clock_phase: to 0 while the SPI clock is not ready
 * (a PLL GO in progress among the reasons), else to its residue below the
 * divider, which later ready ticks keep (numerator == denominator); either
 * way n ticks leave what the first one does, whenever a GO ends.
 * No tick in it can raise an event.  So a board only counts those ticks
 * (`debt`) and applies them before anything can read the state they move:
 * every bus access that is not plain RAM (a register read or write may read
 * or change PLL, timer or SPI state, and a write may leave the steady state),
 * and the end of each DSP activation (checkpoints, reports, MAIN).  Plain
 * RAM never depends on that state.  The board re-tests quiet after each
 * per-cycle tick and drops back to per-cycle ticking at every flush.
 * tests/cstub/dsp-ticks.c checks batched against per-cycle ticking.
 */
#include <stdbool.h>
#include <stdint.h>
#include "cdj_c6747_pll.h"
#include "cdj_c6747_spi_clock.h"
#include "cdj_c6747_timer.h"

typedef struct {
    uint64_t debt;      /* ticks counted but not applied; 0 unless steady */
    bool steady;        /* the next tick may only be counted */
} CdjDspTicks;

static inline bool cdj_dsp_ticks_quiet(const CdjC6747Timer *timers,
                                       const CdjC6747SpiTransfer *transfer,
                                       const CdjWm8740 *dac,
                                       bool functional_timing)
{
    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i)
        if ((timers[i].tgcr & 3u) && (timers[i].tcr & 0x00c000c0u))
            return false;
    return functional_timing || transfer->fault ||
           cdj_spi_core_idle(transfer, dac);
}

/* n >= 1 steady ticks.  Callers own any logging the per-cycle tick does; in
 * the steady state there is none (no WM8740 latch, no timer event). */
static inline void cdj_dsp_ticks_apply(CdjC6747Spi spis[2], CdjWm8740 *dac,
                                       CdjC6747SpiTransfer *transfer,
                                       CdjC6747Pll *pll, uint64_t n,
                                       bool functional_timing)
{
    if (!functional_timing) cdj_spi_core_tick(spis, dac, transfer, pll);
    cdj_c6747_pll_ticks(pll, n);
}
#endif
