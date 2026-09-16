/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_PLL_H
#define CDJ_C6747_PLL_H
#include <stdbool.h>
#include <stdint.h>
/* SPRUH91D 7.4.3-17, SPRS377F Table 6-4. NXS-specific input-clock/reset
 * timing plus synthetic divider GO. POR defaults substitute for ROM handoff;
 * Physical output-clock consumers remain unsupported. Lock wait is a catalog
 * bound applied to the custom chip, not measured lock status. */
typedef struct {
    uint32_t config[13];
    bool legacy_bit4_used; /* Explicit unverified C6747 readback assumption. */
    uint32_t active_dividers[7], target_dividers[7], command;
    unsigned go_remaining;
    /* NXS OSCIN is 16.9344 MHz (RRV4356 X501). While bypassed, one
     * SYSCLK1/core cycle spans the active PLLDIV1 ratio in OSCIN periods.
     * Initial bypass is a missing-ROM handoff assumption. */
    uint64_t oscin_cycles;
    unsigned reset_age, lock_wait_remaining, oscin_phase;
    bool early_enable; /* Sticky: PLLEN set before conservative wait elapsed. */
} CdjC6747Pll;
/* NXS board fact, not a device property: X501 feeds OSCIN = 16.9344 MHz
 * (parent docs/dsp/dsp-hardware.md, from RRV4356 pp 12, 13, 96; recorded in
 * HANDOFF.md). It sits inside the datasheet's external-clock range of
 * 12..50 MHz, SPRS377F Table 6-3 printed page 70 (PDF page 70). */
#define CDJ_C6747_OSCIN_HZ 16934400u

/* AUXCLK in Hz: the clock the Timer64Ps, I2C0 and the McASP serial clock
 * generators run from. Exact, and independent of every PLL divider. */
uint32_t cdj_c6747_pll_auxclk_hz(void);

/* SYSCLKn (n = 1..7) as an exact unreduced fraction, *numerator Hz over
 * *denominator; both are left untouched and false returned wherever the
 * manuals fix no frequency (a disabled divider, an unsupported PREDIV or
 * POSTDIV state). Settled dividers only: during a GO the new PLLDIV ratios
 * are not in effect yet. */
bool cdj_c6747_pll_sysclk_hz(const CdjC6747Pll *s, unsigned n,
                            uint64_t *numerator, uint32_t *denominator);

/* One DSP-cycle edge before bus commits. GO lasts eight subsequent cycles;
 * still synthetic latency, not physical OSCIN/PLL alignment timing. */
void cdj_c6747_pll_tick(CdjC6747Pll *s);
void cdj_c6747_pll_reset(CdjC6747Pll *s);
bool cdj_c6747_pll_read(const CdjC6747Pll *s, uint32_t address, uint32_t *value);
bool cdj_c6747_pll_write(CdjC6747Pll *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit);
bool cdj_c6747_pll_write_mapped(uint32_t address, unsigned size);
#endif
