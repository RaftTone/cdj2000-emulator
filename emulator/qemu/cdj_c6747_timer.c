/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "cdj_c6747_timer.h"

#define TIMER_REVID 0x4472020cu
#define TCR_WRITE_MASK 0x04c03ffeu
#define TGCR_WRITE_MASK 0x0000ff1fu
#define GPINTGPEN_WRITE_MASK 0x00030033u
#define GPDATGPDIR_WRITE_MASK 0x00030003u
#define INTCTL_ENABLE_MASK 0x00050005u
#define INTCTL_STATUS_MASK 0x000a000au

static CdjC6747Timer *decode(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                            uint32_t address, uint32_t *offset)
{
    uint32_t base;
    unsigned index;
    if (address >= CDJ_C6747_TIMER0_BASE && address < CDJ_C6747_TIMER0_BASE + 0x1000) {
        base = CDJ_C6747_TIMER0_BASE;
        index = 0;
    } else if (address >= CDJ_C6747_TIMER1_BASE &&
               address < CDJ_C6747_TIMER1_BASE + 0x1000) {
        base = CDJ_C6747_TIMER1_BASE;
        index = 1;
    } else {
        return NULL;
    }
    *offset = address - base;
    return &timers[index];
}

bool cdj_c6747_timer_input_hz(const CdjC6747Timer *s, uint32_t auxclk_hz,
                              unsigned half, uint64_t *numerator,
                              uint32_t *denominator)
{
    if (!s || !auxclk_hz || !numerator || !denominator ||
        half > CDJ_C6747_TIMER_HALF_34)
        return false;
    /* CLKSRC12 = 1 takes the clock from the TM64P_IN12 pin instead
     * (SPRUH91D Table 28-1 and 28.1.5.2.2 printed page 1229; TCR bit 8,
     * printed page 1253).  That pin's frequency is a board fact no manual
     * page fixes, so refuse rather than invent one.
     *
     * How far that refusal reaches depends on the mode, and printed page 1229
     * is explicit: "If the timer is configured in 64-bit mode or 32-bit chained
     * mode, CLKSRC12 controls the clock source for the entire timer.  If the
     * timer is configured in dual 32-bit unchained mode (TIMMODE = 01 in TGCR),
     * CLKSRC12 controls the timer 1:2 side of the timer only."  So in dual
     * 32-bit unchained mode the 3:4 side still runs from AUXCLK through its own
     * prescaler whatever CLKSRC12 says, and refusing it there would hide a rate
     * the manual does fix. */
    bool dual_unchained = ((s->tgcr >> 2) & 3u) == 1u;

    if ((s->tcr & (1u << 8)) &&
        (half == CDJ_C6747_TIMER_HALF_12 || !dual_unchained))
        return false;
    if (half == CDJ_C6747_TIMER_HALF_12) {
        /* Timer 1:2 has no prescaler (SPRUH91D 28.1.5.4.2.2.2 printed page
         * 1236), and in 64-bit, watchdog and chained modes CLKSRC12 clocks
         * the whole timer (28.1.5.2.1 printed page 1229). */
        *numerator = auxclk_hz;
        *denominator = 1u;
        return true;
    }
    /* Only dual 32-bit unchained mode (TIMMODE = 1h in TGCR, printed page
     * 1254) gives timer 3:4 a clock of its own: the 4-bit prescaler emits one
     * TIM34 clock every PSC34 + 1 input clocks (SPRUH91D 28.1.5.4.2.2.1
     * printed page 1236).  In the other three modes TIM34 advances off the
     * 1:2 side at a rate PRD12 sets, which is not a clock the PLLC defines -
     * refuse instead of inventing a ratio. */
    if (!dual_unchained) return false;
    *numerator = auxclk_hz;
    *denominator = ((s->tgcr >> 8) & 15u) + 1u;
    return true;
}

/* True where the manuals fix this half's input clock, so that counting it is
 * reporting a documented sequence rather than inventing a board signal.
 * cdj_c6747_timer_input_hz() is the single source of truth for the CLKSRC12
 * rule, including the fact that in dual 32-bit unchained mode CLKSRC12 reaches
 * only the 1:2 side (printed page 1229); the AUXCLK value passed in is a
 * placeholder because only the refusal is wanted here, never the rate.
 * TIEN12 is checked here instead: it gates the internal clock with the
 * TM64P_IN12 pin level (Table 28-17, printed page 1253), which is a run/stop
 * condition and so deliberately absent from the rate function.  It lives in
 * the lower half of TCR, which "has no control" over timer 3:4 (printed page
 * 1236), so in unchained mode it gates only the 1:2 side - the same division
 * CLKSRC12 has. */
static bool input_clock_known(const CdjC6747Timer *s, unsigned half)
{
    uint64_t numerator;
    uint32_t denominator;
    bool dual_unchained = ((s->tgcr >> 2) & 3u) == 1u;
    if ((s->tcr & (1u << 9)) &&
        (half == CDJ_C6747_TIMER_HALF_12 || !dual_unchained))
        return false;
    return cdj_c6747_timer_input_hz(s, 1u, half, &numerator, &denominator);
}

/* One clock into one GP up-counter; true on a period match this clock.
 *
 * ENAMODEn, Table 28-17 printed pages 1252-1253 and the identical prose in
 * 28.1.5.4.1.1 (1231), 28.1.5.4.2.1.1 (1234) and 28.1.5.4.2.2.3 (1237):
 *   0  disabled - "does not run and maintains its current count value"
 *   1h one time - "counts up until the counter value equals the period value
 *      and then stops"
 *   2h continuous - "resets itself to zero and begins counting again"
 *   3h continuous with period reload - as 2h, and "reloads the period
 *      registers ... with the value in the period reload registers (RELn)"
 *
 * The zero reset happens "on the cycle after matching", so it is done at the
 * top of the following call rather than at the match.  That is what makes the
 * period value observable in TIMn for exactly one input clock, which is what
 * Figure 28-7 (printed page 1236) draws: TIM34 15 -> 16 = PRD34 -> 0.
 *
 * A zero period does not count: "when both the timer counter and timer period
 * are cleared to 0, the timer can be enabled but the timer counter does not
 * increment because the timer period is 0" (printed pages 1231, 1234, 1237).
 * This is also what keeps a reset timer silent instead of matching 0 == 0 on
 * every clock. */
static bool advance(uint64_t *counter, uint64_t *period, uint64_t reload,
                    unsigned enamode)
{
    if (!enamode || !*period) return false;
    if (*counter == *period) {
        if (enamode == 1u) return false;
        *counter = 0;
        if (enamode == 3u) *period = reload;
        return false;
    }
    return ++*counter == *period;
}

/* CMP0-7 against TIM12, Table 28-25 printed page 1259: "When PLUSEN = 1 in the
 * timer global control register (TGCR) and the timer is configured in 32-bit
 * unchained mode, TIM12 is compared to all 8 compare registers (CMP0-CMP7).
 * When CMPn matches TIM12, a timer CMPn interrupt and DMA event are generated.
 * A CMPn match will not affect the TIM12 count or behavior."  28.1.5.4.2.2
 * (printed page 1235) adds "upon a successful non-zero match", which is why a
 * CMPn of 0 never fires.  Only evaluated on a clock that moved TIM12: there is
 * no INTCTLSTAT status bit for a compare match, so nothing would latch or
 * de-assert it and a stalled counter sitting on a match would otherwise raise
 * the event on every single clock. */
static unsigned compare_matches(const CdjC6747Timer *s)
{
    unsigned fired = 0;
    if (!(s->tgcr & (1u << 4)) || ((s->tgcr >> 2) & 3u) != 1u) return 0;
    for (unsigned n = 0; n < 8; ++n)
        if (s->compare[n] && s->compare[n] == s->tim12)
            fired |= 1u << (CDJ_C6747_TIMER_OUT_CMP0 + n);
    return fired;
}

static unsigned tick_one(CdjC6747Timer *s)
{
    unsigned mode = (s->tgcr >> 2) & 3u;
    unsigned enamode12 = (s->tcr >> 6) & 3u;
    unsigned enamode34 = (s->tcr >> 22) & 3u;
    unsigned fired = 0;
    uint64_t counter, period;

    /* 64-bit watchdog mode, 28.1.6 printed pages 1240-1242.  A timeout
     * "resets the entire processor" and there is no device-level reset wired
     * here; setting WDFLAG without the reset would be a half-truth firmware
     * could act on, and the WDKEY service state machine of Figure 28-10 is
     * likewise unmodelled.  So the watchdog counter does not advance. */
    if (mode == 2u) return 0;

    if (mode == 1u) {
        /* Dual 32-bit unchained, 28.1.5.4.2.2 printed pages 1235-1237: two
         * independent timers, each released by its own TGCR reset bit and
         * enabled by its own ENAMODEn (Table 28-4, printed page 1237). */
        if ((s->tgcr & 1u) && input_clock_known(s, CDJ_C6747_TIMER_HALF_12)) {
            uint32_t before = s->tim12;
            counter = s->tim12;
            period = s->prd12;
            if (advance(&counter, &period, s->rel12, enamode12))
                fired |= 1u << CDJ_C6747_TIMER_OUT_TINT12;
            s->tim12 = (uint32_t)counter;
            s->prd12 = (uint32_t)period;
            if (s->tim12 != before) fired |= compare_matches(s);
        }
        if ((s->tgcr & 2u) && enamode34 &&
            input_clock_known(s, CDJ_C6747_TIMER_HALF_34)) {
            /* 4-bit prescaler, Table 28-18 printed page 1254: "TDDR34
             * increments every timer clock.  The TIM34 counter increments on
             * the cycle after TDDR34 matches PSC34.  TDDR34 resets to 0 and
             * continues."  Figure 28-7 (printed page 1236) draws PSC34 = 2 as
             * TDDR34 1, 2, 0, 1, 2, 0 with TIM34 incremented on each 2 -> 0,
             * so one TIM34 clock every PSC34 + 1 input clocks - the divisor
             * cdj_c6747_timer_input_hz() already derives.
             *
             * ENAMODE34 gates the prescale counter too, not just TIM34: Table
             * 28-18's sentence opens "When the timer is enabled, TDDR34
             * increments every timer clock", 28.1.5.4.2.2.1 (printed page
             * 1236) repeats "When the timer is enabled, the prescale counter
             * starts incrementing by 1 at every timer input clock cycle", and
             * ENAMODE34 = 0 (Table 28-17, printed page 1252) is "The timer is
             * disabled (not counting) and maintains current value".  TDDR34 is
             * firmware-writable through TGCR_WRITE_MASK, so a programmed
             * prescale phase has to survive a disabled interval instead of
             * being scrambled by it. */
            uint32_t tddr = (s->tgcr >> 12) & 15u;
            if (tddr == ((s->tgcr >> 8) & 15u)) {
                s->tgcr &= ~0xf000u;
                counter = s->tim34;
                period = s->prd34;
                if (advance(&counter, &period, s->rel34, enamode34))
                    fired |= 1u << CDJ_C6747_TIMER_OUT_TINT34;
                s->tim34 = (uint32_t)counter;
                s->prd34 = (uint32_t)period;
            } else {
                s->tgcr = (s->tgcr & ~0xf000u) | ((tddr + 1u) << 12);
            }
        }
        return fired;
    }

    /* 64-bit and chained modes both need both halves out of reset (Tables 28-2
     * and 28-3, printed pages 1231 and 1234), are controlled by ENAMODE12
     * alone - "the ENAMODE34 bit has no effect" - and take their clock through
     * the CLKSRC12 mux for "the entire timer" (printed page 1229). */
    if ((s->tgcr & 3u) != 3u ||
        !input_clock_known(s, CDJ_C6747_TIMER_HALF_12) || !enamode12)
        return 0;

    if (mode == 3u) {
        /* Chained, 28.1.5.4.2.1 printed page 1232: TIM34 is a 32-bit prescale
         * counter against PRD34 and "one cycle after the prescale counter
         * matches the prescale period, a clock signal is generated and the
         * prescale counter register is reset to 0"; TIM12 "increments by 1 at
         * every prescaler output clock cycle".  Figure 28-4 (printed page
         * 1233) routes only the 1:2 comparator to the pulse generator, so
         * there is no TINT34 in this mode. */
        if (s->tim34 == s->prd34) {
            /* ENAMODE12 = 3h reloads BOTH period registers, not just the one
             * the counter matched: 28.1.5.4.2.1.1 (printed page 1234) says it
             * "resets itself to zero, reloads the period registers (PRD12 and
             * PRD34) with the value in the period reload registers (REL12 and
             * REL34), and begins counting again".  Table 28-3 on the same page
             * annotates the reload "(Timer 3 only)", which could be read as
             * restricting it; the explicit prose naming both registers is
             * followed here, and the 64-bit branch below reloads both halves
             * too, so the two modes stay consistent with each other.  The
             * reset condition is advance()'s own, evaluated before the call
             * because advance() only sees the period it is given. */
            bool reload_prd34 =
                enamode12 == 3u && s->prd12 && s->tim12 == s->prd12;
            s->tim34 = 0;
            counter = s->tim12;
            period = s->prd12;
            if (advance(&counter, &period, s->rel12, enamode12))
                fired |= 1u << CDJ_C6747_TIMER_OUT_TINT12;
            s->tim12 = (uint32_t)counter;
            s->prd12 = (uint32_t)period;
            if (reload_prd34) s->prd34 = s->rel34;
        } else {
            ++s->tim34;
        }
        return fired;
    }

    /* 64-bit GP, 28.1.5.4.1 printed page 1230: "The counter registers (TIM12
     * and TIM34) form a 64-bit timer counter register and the period registers
     * (PRD12 and PRD34) form a 64-bit timer period register", TIM34 holding
     * the upper word per Figure 28-3's TIM34:TIM12 ordering. */
    counter = ((uint64_t)s->tim34 << 32) | s->tim12;
    period = ((uint64_t)s->prd34 << 32) | s->prd12;
    if (advance(&counter, &period,
                ((uint64_t)s->rel34 << 32) | s->rel12, enamode12))
        fired |= 1u << CDJ_C6747_TIMER_OUT_TINT12;
    s->tim12 = (uint32_t)counter;
    s->tim34 = (uint32_t)(counter >> 32);
    s->prd12 = (uint32_t)period;
    s->prd34 = (uint32_t)(period >> 32);
    return fired;
}

uint32_t cdj_c6747_timers_tick(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT])
{
    uint32_t fired = 0;
    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i) {
        CdjC6747Timer *s = &timers[i];
        /* After reset TGCR is 0, so both halves are held in reset (Table 28-18,
         * printed page 1254) and the common case costs one load and one test. */
        if (!(s->tgcr & 3u)) continue;
        unsigned outputs = tick_one(s);
        if (!outputs) continue;
        /* INTCTLSTAT, Table 28-24 printed pages 1258-1259.  PRDINTSTATn
         * "reflects the condition that timer counter matched the period
         * register when timer is enabled" - the match itself sets it, with no
         * mention of the enable - while PRDINTENn only "enable[s] interrupt
         * generation".  So the status bit appears even with the enable clear,
         * and firmware clears it the documented way, by writing 1 to the
         * R/W1C bit, which the 0x44 write path already implements. */
        if (outputs & (1u << CDJ_C6747_TIMER_OUT_TINT12)) {
            s->intctlstat |= 1u << 1;                   /* PRDINTSTAT12 */
            if (!(s->intctlstat & 1u))                  /* PRDINTEN12 */
                outputs &= ~(1u << CDJ_C6747_TIMER_OUT_TINT12);
        }
        if (outputs & (1u << CDJ_C6747_TIMER_OUT_TINT34)) {
            s->intctlstat |= 1u << 17;                  /* PRDINTSTAT34 */
            if (!(s->intctlstat & (1u << 16)))          /* PRDINTEN34 */
                outputs &= ~(1u << CDJ_C6747_TIMER_OUT_TINT34);
        }
        fired |= (uint32_t)outputs << (i * CDJ_C6747_TIMER_OUTPUTS);
    }
    return fired;
}

unsigned cdj_c6747_timer_event(unsigned bit)
{
    /* SPRUH91D Table 2-1 DSP Interrupt Map: T64P0_TINT12 = 4 (printed page
     * 70); T64P1_TINT12 = 40 and T64P1_TINT34 = 48 (printed page 71);
     * T64P0_TINT34 = 64, T64P0_CMPINT0-7 = 78-85 and T64P1_CMPINT0-7 = 86-93
     * (printed page 72).  Event 4 is the reset selection of CPUINT4. */
    static const unsigned char events[CDJ_C6747_TIMER_COUNT]
                                    [CDJ_C6747_TIMER_OUTPUTS] = {
        {  4, 64, 78, 79, 80, 81, 82, 83, 84, 85 },
        { 40, 48, 86, 87, 88, 89, 90, 91, 92, 93 },
    };
    if (bit >= CDJ_C6747_TIMER_COUNT * CDJ_C6747_TIMER_OUTPUTS) return 0;
    return events[bit / CDJ_C6747_TIMER_OUTPUTS]
                 [bit % CDJ_C6747_TIMER_OUTPUTS];
}

void cdj_c6747_timers_reset(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT])
{
    memset(timers, 0, sizeof(*timers) * CDJ_C6747_TIMER_COUNT);
}

/* Read Reset Mode applies only where Table 28-25 (printed page 1259) and
 * 28.1.5.4.2.2.6 (printed page 1238) put it: PLUSEN set and the timer "in
 * 32-bit unchained mode", with the half's own READRSTMODEn bit selected.
 * TIMMODE is compared in full rather than by a bit mask: 0x14 tests only
 * PLUSEN and TIMMODE bit 2, so it also matched chained mode (TIMMODE = 3h),
 * which the manual does not include. */
static bool read_reset_selected(const CdjC6747Timer *s, uint32_t readrstmode)
{
    return (s->tgcr & (1u << 4)) && ((s->tgcr >> 2) & 3u) == 1u &&
           (s->tcr & readrstmode);
}

bool cdj_c6747_timers_read(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                           uint32_t address, uint32_t *value)
{
    uint32_t offset;
    CdjC6747Timer *s = decode(timers, address, &offset);
    if (!s || !value || (offset & 3)) return false;
    switch (offset) {
    case 0x00: *value = TIMER_REVID; break;
    case 0x04: *value = s->emumgt; break;
    case 0x08: *value = s->gpintgpen; break;
    case 0x0c: *value = s->gpdatgpdir; break;
    case 0x10:
        *value = s->tim12;
        if (((s->tgcr >> 2) & 3) == 0) {
            s->tim34_shadow = s->tim34;
            s->tim34_shadow_valid = true;
        } else if (read_reset_selected(s, 1u << 10)) {
            /* Read Reset Mode, 28.1.5.4.2.2.6 printed page 1238: the read
             * "copies values from the timer counter registers (TIM12 and/or
             * TIM34) to the timer capture registers (CAP12 and/or CAP34),
             * reloads the timer period registers ... if in continuous mode
             * with period reload (ENAMODE = 3h), and then restarts counting".
             * The captured value is the pre-reset count, which is what the
             * read itself returned.  No output event is generated here -
             * "Timer output events (TINTn, TEVTn, and TM64P_OUTn) are not
             * generated during this process" - and this path raises none. */
            s->cap12 = s->tim12;
            if (((s->tcr >> 6) & 3u) == 3u) s->prd12 = s->rel12;
            s->tim12 = 0;
        }
        break;
    case 0x14:
        *value = s->tim34_shadow_valid ? s->tim34_shadow : s->tim34;
        s->tim34_shadow_valid = false;
        if (read_reset_selected(s, 1u << 26)) {
            /* Same rule on the 3:4 side, ENAMODE34 selecting the reload. */
            s->cap34 = s->tim34;
            if (((s->tcr >> 22) & 3u) == 3u) s->prd34 = s->rel34;
            s->tim34 = 0;
        }
        break;
    case 0x18: *value = s->prd12; break;
    case 0x1c: *value = s->prd34; break;
    case 0x20: *value = s->tcr; break;
    case 0x24: *value = s->tgcr; break;
    case 0x28: *value = s->wdtcr; break;
    case 0x34: *value = s->rel12; break;
    case 0x38: *value = s->rel34; break;
    case 0x3c: *value = s->cap12; break;
    case 0x40: *value = s->cap34; break;
    case 0x44: *value = s->intctlstat; break;
    default:
        if (offset < 0x60 || offset > 0x7c) return false;
        *value = s->compare[(offset - 0x60) / 4];
        break;
    }
    return true;
}

bool cdj_c6747_timers_write(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                            uint32_t address, uint64_t value, unsigned size,
                            bool commit)
{
    uint32_t offset;
    CdjC6747Timer *s = decode(timers, address, &offset);
    if (!s || size != 4 || value > UINT32_MAX || (offset & 3) || offset == 0)
        return false;
    uint32_t word = value;
    switch (offset) {
    case 0x04: if (commit) s->emumgt = word & 3; break;
    case 0x08: if (commit) s->gpintgpen = word & GPINTGPEN_WRITE_MASK; break;
    case 0x0c: if (commit) s->gpdatgpdir = word & GPDATGPDIR_WRITE_MASK; break;
    case 0x10: if (commit) s->tim12 = word; break;
    case 0x14:
        if (commit) { s->tim34 = word; s->tim34_shadow_valid = false; }
        break;
    case 0x18: if (commit) s->prd12 = word; break;
    case 0x1c: if (commit) s->prd34 = word; break;
    case 0x20: if (commit) s->tcr = word & TCR_WRITE_MASK; break;
    case 0x24:
        if (commit) {
            s->tgcr = word & TGCR_WRITE_MASK;
            if (((s->tgcr >> 2) & 3) != 0) s->tim34_shadow_valid = false;
            if (!(s->tgcr & 1)) s->tim12 = 0;
            if (!(s->tgcr & 2)) {
                s->tim34 = 0;
                s->tim34_shadow_valid = false;
            }
        }
        break;
    case 0x28:
        if (commit) {
            uint32_t status = s->wdtcr & (1u << 15);
            if (word & (1u << 15)) status = 0;
            s->wdtcr = (word & 0xffff4000u) | status;
        }
        break;
    case 0x34: if (commit) s->rel12 = word; break;
    case 0x38: if (commit) s->rel34 = word; break;
    case 0x3c: if (commit) s->cap12 = word; break;
    case 0x40: if (commit) s->cap34 = word; break;
    case 0x44:
        if (commit) {
            uint32_t status = s->intctlstat & INTCTL_STATUS_MASK;
            status &= ~(word & INTCTL_STATUS_MASK);
            s->intctlstat = status | (word & INTCTL_ENABLE_MASK);
        }
        break;
    default:
        if (offset < 0x60 || offset > 0x7c) return false;
        if (commit) s->compare[(offset - 0x60) / 4] = word;
        break;
    }
    return true;
}
