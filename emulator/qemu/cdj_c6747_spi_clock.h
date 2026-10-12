/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_SPI_CLOCK_H
#define CDJ_C6747_SPI_CLOCK_H
#include "cdj_c6747_spi.h"
#include "cdj_c6747_pll.h"

static inline unsigned cdj_spi_divider(uint32_t value)
{
    return (value & 0x8000u) ? (value & 31u) + 1 : 1;
}

/* SPRUH91D Table 6-2 requires SYSCLK2 to remain half the CPU frequency.
 * Only settled enabled dividers are supported for live serial transfers;
 * PLL GO/clock changes while active are rejected by the bus integration. */
static inline bool cdj_spi_clock_ready(const CdjC6747Pll *pll)
{
    return !pll->go_remaining &&
        (pll->active_dividers[0] & 0x8000u) &&
        (pll->active_dividers[1] & 0x8000u) &&
        cdj_spi_divider(pll->active_dividers[1]) ==
            2 * cdj_spi_divider(pll->active_dividers[0]);
}

/* cdj_spi_core_tick's per-cycle fast path.  An idle transfer with every
 * word field clear and no flags is exactly the idle state
 * cdj_c6747_spi_transfer_valid accepts (phase 0 is SPI_TRANSFER_IDLE, and a
 * zero format is never a valid word), and advance() with it only validates
 * and returns true.  Any other state takes the full call, faults included. */
static inline bool cdj_spi_core_idle(const CdjC6747SpiTransfer *transfer,
                                     const CdjWm8740 *dac)
{
    return !transfer->phase && !transfer->half_ticks_remaining &&
        !transfer->active_control && !transfer->active_format &&
        !transfer->active_delay && !transfer->queued_control &&
        !transfer->queued_format && !transfer->queued_delay &&
        !transfer->queued_valid && !transfer->tx_full &&
        transfer->previous_cshold <= 1 && !transfer->reserved[0] &&
        !transfer->reserved[1] && !transfer->reserved[2] &&
        dac->program[0] <= 0x1ff && dac->program[1] <= 0x1ff &&
        dac->program[2] <= 0x1ff && !(dac->program[3] & ~0x1dfu) &&
        !(dac->program[4] & ~0x70u) && dac->register4_unlocked <= 1;
}

static inline void cdj_spi_core_tick(CdjC6747Spi spis[2], CdjWm8740 *dac,
                                     CdjC6747SpiTransfer *transfer,
                                     const CdjC6747Pll *pll)
{
    if (transfer->fault) return;
    if (!cdj_spi_clock_ready(pll)) {
        if (transfer->phase || transfer->queued_valid) transfer->fault = 1;
        else transfer->clock_phase = 0;
        return;
    }
    unsigned denominator = cdj_spi_divider(pll->active_dividers[1]);
    unsigned numerator = 2 * cdj_spi_divider(pll->active_dividers[0]);
    /* cdj_spi_clock_ready() makes numerator == denominator, so this loop
     * runs once per cycle; it replaces a division and a modulo. */
    transfer->clock_phase += numerator;
    unsigned ticks = 0;
    while (transfer->clock_phase >= denominator) {
        transfer->clock_phase -= denominator;
        ++ticks;
    }
    if (cdj_spi_core_idle(transfer, dac)) return;
    if (!cdj_c6747_spi_wm8740_advance(spis, dac, transfer, ticks))
        transfer->fault = 1;
}
#endif
