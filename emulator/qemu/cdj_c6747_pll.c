/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_pll.h"
static const unsigned offsets[] = {0x100,0x104,0x110,0x114,0x118,0x11c,
                                  0x120,0x124,0x128,0x160,0x164,0x168,0x16c};
static const uint32_t defaults[] = {0xf2,0x14,0x13,0x8000,0x8000,0x8001,
                                   0x8002,0x8000,0x8001,0x8003,0x8002,0x8000,0x8005};
static const unsigned divider_index[] = {4,5,6,9,10,11,12};
static unsigned ratio(uint32_t divider)
{ return divider & 0x8000 ? (divider & 31) + 1 : 1; }
static unsigned lock_wait(const CdjC6747Pll *s)
{
    /* SPRS377F Table 6-4: ceil(2000*N/sqrt(M)) OSCIN periods. Integer
     * arithmetic preserves the conservative bound without libm rounding. */
    unsigned n = ratio(s->config[3]), m = s->config[2] + 1, wait = 0;
    uint64_t square = 4000000ull * n * n;
    while ((uint64_t)wait * wait * m < square) ++wait;
    return wait;
}
static bool valid_operating_point(const CdjC6747Pll *s)
{
    unsigned n = ratio(s->config[3]), m = s->config[2] + 1;
    /* Board square-wave input; datasheet PLLREF 12..50 MHz, M=4..32,
     * PLLOUT 300..600 MHz. The custom chip's core speed grade is separate. */
    return CDJ_C6747_OSCIN_HZ >= 12000000u * n &&
           CDJ_C6747_OSCIN_HZ <= 50000000u * n &&
           m >= 4 && m <= 32 &&
           (uint64_t)CDJ_C6747_OSCIN_HZ * m >= 300000000ull * n &&
           (uint64_t)CDJ_C6747_OSCIN_HZ * m <= 600000000ull * n;
}
/* AUXCLK is the PLL bypass clock, i.e. OSCIN itself, with no PREDIV, PLLM,
 * POSTDIV or PLLDIVn in its path. SPRUH91D Table 7-1 printed page 118 (PDF
 * page 118) gives AUXCLK's ratio as "PLL Bypass Clock" and names its
 * consumers - McASP serial clock, Timers, I2C0, RTC, USB2.0; Table 6-2
 * printed page 104 (PDF page 104) repeats it; section 6.2 printed page 105
 * (PDF page 105) defines the bypass clock as "the reference clock supplied
 * on OSCIN"; Figure 7-1 printed page 117 (PDF page 117) shows AUXCLK
 * branching off ahead of the PLLEN bypass mux and the PLLDIV blocks.
 * NOT modelled: gating by CKEN.AUXEN (SPRUH91D 7.4.20 printed page 135,
 * status in CKSTAT 7.4.21 printed page 136). CKEN is outside this model's
 * register window, so a write to 0x01c11148 already fails closed and the
 * only state this can report is CKEN's reset value, AUXEN = 1. */
uint32_t cdj_c6747_pll_auxclk_hz(void)
{
    return CDJ_C6747_OSCIN_HZ;
}
static bool enabled_ratio(uint32_t reg, uint32_t *value)
{
    /* Divider Value = RATIO + 1 (SPRUH91D Tables 7-8 and 7-9, printed pages
     * 125 and 126; POSTDIV Table 7-17, printed page 131). A clear enable bit
     * is "Disable", and SPRUH91D Table 7-24 printed page 137 ties SYSTAT's
     * SYSnON status to the DnEN default - so disabled means the clock is off.
     * No page gives a frequency for a disabled divider, so refuse rather
     * than assume divide-by-one. */
    if (!(reg & 0x8000u)) return false;
    *value = (reg & 31u) + 1u;
    return true;
}
bool cdj_c6747_pll_sysclk_hz(const CdjC6747Pll *s, unsigned n,
                            uint64_t *numerator, uint32_t *denominator)
{
    uint32_t sysdiv, prediv, postdiv;
    if (!s || !numerator || !denominator || n < 1 || n > 7) return false;
    if (!enabled_ratio(s->active_dividers[n - 1], &sysdiv)) return false;
    if (!(s->config[0] & 1)) {
        /* Bypass: "the reference clock supplied on OSCIN passes directly to
         * the system of PLLDIV blocks" (SPRUH91D 6.2, printed page 105), so
         * neither PREDIV nor POSTDIV is in the path. */
        *numerator = CDJ_C6747_OSCIN_HZ;
        *denominator = sysdiv;
        return true;
    }
    /* PLL mode, SPRUH91D Figure 7-1 printed page 117:
     *   SYSCLKn = OSCIN / PREDIV x (PLLM + 1) / POSTDIV / PLLDIVn.
     * The multiplier is PLLM + 1: section 7.2 printed page 116 reads the 13h
     * reset value as a 20x multiplier. PLLEN alone distinguishes PLL from
     * bypass here because the write path accepts PLLEN only with
     * PLLENSRC = 0, PLLPWRDN = 0 and PLLRST released (Table 7-5, printed
     * page 123). */
    if (!enabled_ratio(s->config[3], &prediv) ||
        !enabled_ratio(s->config[8], &postdiv)) return false;
    *numerator = (uint64_t)CDJ_C6747_OSCIN_HZ * (s->config[2] + 1u);
    *denominator = prediv * postdiv * sysdiv;
    return true;
}
static int index_of(uint32_t address)
{
    for (unsigned i = 0; i < 13; ++i)
        if (address == 0x01c11000u + offsets[i]) return i;
    return -1;
}
bool cdj_c6747_pll_write_mapped(uint32_t address, unsigned size)
{
    return size == 4 && (address == 0x01c11138u || index_of(address) >= 0);
}
void cdj_c6747_pll_reset(CdjC6747Pll *s)
{
    *s = (CdjC6747Pll){0};
    for (unsigned i = 0; i < 13; ++i) s->config[i] = defaults[i];
    for (unsigned i = 0; i < 7; ++i)
        s->active_dividers[i] = s->target_dividers[i] = defaults[divider_index[i]];
}
void cdj_c6747_pll_tick(CdjC6747Pll *s)
{
    /* Measure elapsed input periods using the clock state before this edge.
     * In PLL mode one DSP period is N*POSTDIV*SYSCLK1/M input periods.
     * Preserve the remainder so x23 advances OSCIN once per 23 DSP cycles. */
    unsigned elapsed;
    if (s->config[0] & 1) {
        unsigned numerator = ratio(s->config[3]) * ratio(s->config[8]) *
                             ratio(s->active_dividers[0]);
        unsigned denominator = s->config[2] + 1;
        s->oscin_phase += numerator;
        elapsed = s->oscin_phase / denominator;
        s->oscin_phase %= denominator;
    } else {
        elapsed = ratio(s->active_dividers[0]);
        s->oscin_phase = 0;
    }
    s->oscin_cycles += elapsed;
    if ((s->config[0] & 0x12b) == 0x100) {
        /* Powered, square-wave input, software-selected bypass, reset held.
         * 1000 ns minimum at 16.9344 MHz rounds upward to 17 periods. */
        if (s->reset_age < 17) {
            s->reset_age += elapsed;
            if (s->reset_age > 17) s->reset_age = 17;
        }
    }
    if (s->lock_wait_remaining)
        s->lock_wait_remaining = s->lock_wait_remaining > elapsed ?
                                 s->lock_wait_remaining - elapsed : 0;
    if (s->go_remaining && --s->go_remaining == 0)
        for (unsigned i = 0; i < 7; ++i) s->active_dividers[i] = s->target_dividers[i];
}
bool cdj_c6747_pll_read(const CdjC6747Pll *s, uint32_t address, uint32_t *value)
{
    if (address == 0x01c11138u) { *value = s->command; return true; }
    if (address == 0x01c1113cu) {
        /* STABLE is oscillator-counter completion, not PLL lock. Assume
         * it completed before ROM handoff (7.4.17); no pin timing modeled. */
        *value = 4 | (s->go_remaining != 0); return true;
    }
    int i = index_of(address);
    if (i < 0) return false;
    *value = s->config[i];
    return true;
}
bool cdj_c6747_pll_write(CdjC6747Pll *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit)
{
    if (address == 0x01c11138u) {
        if (size != 4 || value > 1 || (value && s->go_remaining)) return false;
        if (commit) {
            s->command = value; /* W0C command latch, SPRUH91D 7.4.16. */
            if (value) {
                for (unsigned d = 0; d < 7; ++d)
                    s->target_dividers[d] = s->config[divider_index[d]];
                s->go_remaining = 8;
            }
        }
        return true;
    }
    int i = index_of(address);
    if (i < 0 || size != 4 || value > UINT32_MAX) return false;
    /* Reprogramming during phase alignment is not modeled. */
    if (s->go_remaining) return false;
    uint32_t mask = i == 0 ? 0x1fb : i <= 2 ? 31 : 0x801f;
    if (value & ~mask) return false;
    if (i != 0 && (s->config[0] & 8) &&
        i != 4 && i != 5 && i != 6 && i != 9 && i != 10 && i != 11 && i != 12)
        return false;
    if (i == 0) {
        /* TI marks bit 4 reserved-one, but legacy DaVinci PLL code clears
         * it as PLLDIS (Linux v6.1 drivers/clk/davinci/pll.c). Preserve the
         * writable latch as a flagged compatibility assumption, NOT proof
         * of C6747 physical clock semantics. */
        value |= 0xc0;
        if ((value & 1) && ((value & 0x129) != 0x109 ||
                            s->reset_age < 17 || !valid_operating_point(s)))
            return false;
        if (value & 8) {
            if ((value & 0x122) != 0x100 || s->reset_age < 17 ||
                !valid_operating_point(s)) return false;
            /* While out of reset, source/power changes are not supported. */
            if ((s->config[0] & 8) && ((value ^ s->config[0]) & ~1u)) return false;
        }
    }
    if (i == 1 && value != 0x14 && value != 0x1f && (value < 0x17 || value > 0x1d))
        return false; /* OCSEL reserved source selectors. */
    /* PLL_MASTER_LOCK defaults clear; setting it through SYSCFG remains
     * unsupported there. Physical clock outputs remain unconnected. */
    if (commit) {
        if (i == 0) {
            bool enable = (value & 1) && !(s->config[0] & 1);
            if ((value & 8) && !(s->config[0] & 8))
                s->lock_wait_remaining = lock_wait(s);
            if (!(value & 8)) s->lock_wait_remaining = 0;
            if ((value & 0x122) != 0x100 ||
                (!(value & 8) && (s->config[0] & 8))) s->reset_age = 0;
            if ((value ^ s->config[0]) & 1) s->oscin_phase = 0;
            if (enable && s->lock_wait_remaining) s->early_enable = true;
        }
        s->config[i] = value;
        if (i == 0 && !(value & 16)) s->legacy_bit4_used = true;
    }
    return true;
}
