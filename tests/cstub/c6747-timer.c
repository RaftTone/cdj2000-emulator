/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>

#include "cdj_c6747_timer.h"

int main(void)
{
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    uint32_t value;
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE, &value) &&
           value == 0x4472020c);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER1_BASE, &value) &&
           value == 0x4472020c);

    /* Check phase is atomic; reserved bits and TCR.TSTAT read zero. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  UINT32_MAX, 4, false));
    assert(timers[0].tgcr == 0);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  UINT32_MAX, 4, true));
    assert(timers[0].tgcr == 0xff1f);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  UINT32_MAX, 4, true));
    assert(timers[0].tcr == 0x04c03ffe);

    /* The firmware's Timer64P0 setup sequence selects unchained/Plus mode,
     * releases both halves and enables period interrupt 12. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x17, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  1, 4, true));
    assert(timers[0].tgcr == 0x17 && timers[0].intctlstat == 1);

    /* Read-reset is Plus-only and valid only in unchained mode. */
    timers[0].tim12 = 123;
    timers[0].tcr = 1u << 10;
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10,
                                 &value) && value == 123 && timers[0].tim12 == 0);

    /* A 64-bit TIM12/TIM34 read is coherent through the upper shadow. */
    timers[0].tgcr = 3;
    timers[0].tim12 = 0xffffffff;
    timers[0].tim34 = 7;
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10, &value) &&
           value == UINT32_MAX);
    timers[0].tim34 = 8;
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x14, &value) &&
           value == 7);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x14, &value) &&
           value == 8);

    /* Clearing reset bits clears the corresponding counters. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0, 4, true));
    assert(!timers[0].tim12 && !timers[0].tim34);

    /* INTCTLSTAT status is W1C while enable bits take the written value. */
    timers[1].intctlstat = 0x000a000a;
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x44,
                                  0x00020005, 4, true));
    assert(timers[1].intctlstat == 0x0008000f);

    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x7c,
                                  0x12345678, 4, true));
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER1_BASE + 0x7c, &value) &&
           value == 0x12345678);
    /* ---- Counting, SPRUH91D chapter 28 ------------------------------------
     * Every expected value below is the manual's own arithmetic.  No value
     * here comes from running this emulator, and none of it pins a rate: the
     * unit of cdj_c6747_timers_tick() is "one timer input clock period" and
     * nothing asserts how long that is. */

    /* A timer held in reset does not count, and the reset state in particular
     * does not match its own zero period: "when both the timer counter and
     * timer period are cleared to 0, the timer can be enabled but the timer
     * counter does not increment because the timer period is 0" - printed
     * pages 1231, 1234 and 1237. */
    cdj_c6747_timers_reset(timers);
    for (unsigned i = 0; i < 64; ++i) assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim12 && !timers[0].intctlstat);

    /* PER-TIMER64P's acceptance test.  Timer64P0, dual 32-bit unchained
     * (TIMMODE = 1h, TGCR bits 3-2) with timer 1:2 out of reset (TIM12RS = 1),
     * PRD12 = 3 and ENAMODE12 = 2h continuous (TCR bits 7-6, so 2h << 6 =
     * 0x80).  28.1.5.4.2.2.2 printed page 1236: "the timer counter increments
     * by 1 at every timer input clock cycle", so after k clocks TIM12 = k, and
     * the match is at k = PRD12 = 3.  Table 28-24 printed page 1258:
     * PRDINTSTAT12 is INTCTLSTAT bit 1 = 0x2, set by the match itself, while
     * PRDINTEN12 (bit 0) only enables interrupt GENERATION - so the status bit
     * must appear here with PRDINTEN12 still 0, and no event is reported. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x05, 4, true));           /* TIMMODE=1h, TIM12RS */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  3, 4, true));              /* PRD12 = 3 */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));           /* ENAMODE12 = 2h */
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 1);
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 2);
    assert(!timers[0].intctlstat);
    assert(!cdj_c6747_timers_tick(timers));
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10,
                                 &value) && value == 3);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                 &value) && value == 0x2);

    /* "resets the timer counter to 0 on the cycle after matching and
     * continues" - Table 28-17 printed page 1253 - so the period value stands
     * for exactly one input clock, as Figure 28-7 printed page 1236 draws. */
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 0);
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 1);

    /* Firmware clears the status the documented way: PRDINTSTAT12 is R/W1C
     * (Figure 28-28, printed page 1258), and writing 0 leaves it alone. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  0, 4, true));
    assert(timers[0].intctlstat == 0x2);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  0x2, 4, true));
    assert(!timers[0].intctlstat);

    /* With PRDINTEN12 set, the same match also reports TINT12, which is bit
     * CDJ_C6747_TIMER_OUT_TINT12 of timer 0 and SPRUH91D Table 2-1 event 4
     * (printed page 70).  TIM12 is at 1, so two more clocks reach 3 = PRD12
     * after passing through 2. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  0x1, 4, true));
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 2);
    assert(cdj_c6747_timers_tick(timers) == (1u << CDJ_C6747_TIMER_OUT_TINT12));
    assert(timers[0].tim12 == 3 && timers[0].intctlstat == 0x3);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUT_TINT12) == 4);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUT_TINT34) == 64);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUTPUTS +
                                 CDJ_C6747_TIMER_OUT_TINT12) == 40);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUTPUTS +
                                 CDJ_C6747_TIMER_OUT_TINT34) == 48);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUT_CMP0) == 78);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUTPUTS +
                                 CDJ_C6747_TIMER_OUT_CMP0 + 7) == 93);
    assert(!cdj_c6747_timer_event(CDJ_C6747_TIMER_COUNT *
                                  CDJ_C6747_TIMER_OUTPUTS));

    /* ENAMODE12 = 1h: "it counts up until the counter value equals the period
     * value and then stops" - printed page 1253.  PRD12 = 2, so clocks 1 and 2
     * count and every clock after that leaves TIM12 at 2. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x05, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  2, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  1u << 6, 4, true));        /* ENAMODE12 = 1h */
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 1);
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 2);
    assert(timers[0].intctlstat == 0x2);
    for (unsigned i = 0; i < 16; ++i) assert(!cdj_c6747_timers_tick(timers));
    assert(timers[0].tim12 == 2);

    /* ENAMODE12 = 3h reloads PRD12 from REL12 at the zero reset - "reloads the
     * period registers (PRD12 and/or PRD34) with the value in the period
     * reload registers (REL12 and/or REL34)", printed page 1237.  PRD12 = 1,
     * REL12 = 3: clock 1 matches at TIM12 = 1, clock 2 resets to 0 and swaps
     * the period in, then clocks 3, 4 and 5 reach 3 = the new period. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x05, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  1, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x34,
                                  3, 4, true));              /* REL12 = 3 */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  3u << 6, 4, true));        /* ENAMODE12 = 3h */
    assert(!cdj_c6747_timers_tick(timers));
    assert(timers[0].tim12 == 1 && timers[0].prd12 == 1);
    assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim12 && timers[0].prd12 == 3);
    assert(!cdj_c6747_timers_tick(timers) && !cdj_c6747_timers_tick(timers));
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 3);

    /* The 3:4 side of unchained mode runs off the 4-bit prescaler: "TDDR34
     * increments every timer clock.  The TIM34 counter increments on the cycle
     * after TDDR34 matches PSC34" (Table 28-18, printed page 1254), i.e. one
     * TIM34 clock every PSC34 + 1 input clocks.  Figure 28-7 printed page 1236
     * is reproduced exactly here: PSC34 = 2 (TGCR bits 11-8, so 0x200),
     * TDDR34 = 1 (bits 15-12, 0x1000), TIM34 = 15, PRD34 = 16, ENAMODE34 = 2h
     * (TCR bits 23-22, 2h << 22 = 0x00800000).  The figure's TDDR34 row is
     * 1, 2, 0, 1, 2, 0 with TIM34 incremented on each 2 -> 0 and reset on the
     * clock after 16 matches PRD34, so TIM34 runs 15, 15, 16, 16, 16, 0.
     * PRDINTSTAT34 is INTCTLSTAT bit 17 = 0x00020000 (printed page 1258). */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x1206, 4, true));
    assert(timers[0].tgcr == 0x1206);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x14,
                                  15, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x1c,
                                  16, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x00800000, 4, true));
    assert(!cdj_c6747_timers_tick(timers));              /* TDDR34 1 -> 2 */
    assert(timers[0].tim34 == 15 && ((timers[0].tgcr >> 12) & 15) == 2);
    assert(!cdj_c6747_timers_tick(timers));              /* TDDR34 2 -> 0 */
    assert(timers[0].tim34 == 16 && !((timers[0].tgcr >> 12) & 15));
    assert(timers[0].intctlstat == 0x00020000);
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim34 == 16);
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim34 == 16);
    assert(!cdj_c6747_timers_tick(timers) && !timers[0].tim34);
    /* PRDINTEN34 is bit 16; with it set the next period match reports TINT34,
     * Table 2-1 event 64 for Timer64P0 (printed page 72). */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  0x00030000, 4, true));
    assert(timers[0].intctlstat == 0x00010000);
    {
        /* The arrival clock is fixed by the cited pages, so assert it exactly
         * rather than searching: PSC34 = 2 gives three input clocks per TIM34
         * clock (Table 28-18, printed page 1254) and 16 TIM34 clocks climb
         * 0 -> 16 = PRD34, so the match lands on input clock 48 and not one
         * clock earlier.  A search loop would also pass for a prescaler off by
         * one or a TIM34 that skipped a count. */
        for (unsigned i = 0; i < 16 * 3 - 1; ++i)
            assert(!cdj_c6747_timers_tick(timers));
        assert(timers[0].tim34 == 15 && ((timers[0].tgcr >> 12) & 15) == 2);
        assert(cdj_c6747_timers_tick(timers) ==
               (1u << CDJ_C6747_TIMER_OUT_TINT34));
        assert(timers[0].tim34 == 16 &&
               timers[0].intctlstat == 0x00030000);
    }

    /* 64-bit GP mode (TIMMODE = 0) needs both TIM12RS and TIM34RS - Table 28-2
     * printed page 1231 - and carries TIM12 into TIM34: PRD34:PRD12 =
     * 1:0000 0000h, so the match is 0x100000000 clocks away.  Start one clock
     * short by seeding TIM12 = 0xffffffff. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x03, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x1c,
                                  1, 4, true));              /* PRD34 = 1 */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x10,
                                  UINT32_MAX, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));           /* ENAMODE12 = 2h */
    assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim12 && timers[0].tim34 == 1);
    assert(timers[0].intctlstat == 0x2);
    /* ENAMODE34 has no effect in 64-bit mode (printed page 1231), and the 3:4
     * prescaler is not in this path, so TDDR34 never moves. */
    assert(!((timers[0].tgcr >> 12) & 15));

    /* One half still in reset stops 64-bit mode dead. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x01, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  2, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));
    for (unsigned i = 0; i < 8; ++i) assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim12 && !timers[0].intctlstat);

    /* Chained mode (TIMMODE = 3h): TIM34/PRD34 is a 32-bit prescaler clocking
     * TIM12 - 28.1.5.4.2.1 printed page 1232 - and Figure 28-5 printed page
     * 1233 is reproduced here: TIM34 = 200, PRD34 = 202, TIM12 = 3, PRD12 = 4.
     * The figure's prescale row is 200, 201, 202, 0, 1, 2 with the timer
     * counter incremented on 202 -> 0, so TIM12 goes 3, 3, 3, 4 and the match
     * at PRD12 = 4 sets PRDINTSTAT12. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x0f, 4, true));           /* TIMMODE=3h, both RS */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x14,
                                  200, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x1c,
                                  202, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x10,
                                  3, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  4, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));
    assert(!cdj_c6747_timers_tick(timers));
    assert(timers[0].tim34 == 201 && timers[0].tim12 == 3);
    assert(!cdj_c6747_timers_tick(timers));
    assert(timers[0].tim34 == 202 && timers[0].tim12 == 3);
    assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim34 && timers[0].tim12 == 4);
    assert(timers[0].intctlstat == 0x2);
    /* Only the 1:2 comparator drives the pulse generator in chained mode
     * (Figure 28-4, printed page 1233): nothing sets PRDINTSTAT34 even though
     * the prescaler matched PRD34. */
    assert(!(timers[0].intctlstat & 0x00020000));

    /* CMP0-7 against TIM12, Table 28-25 printed page 1259, require PLUSEN = 1
     * (TGCR bit 4 = 0x10) and 32-bit unchained mode, and printed page 1235
     * requires a "non-zero match".  CMP3 (offset 0x6c) = 2 therefore fires at
     * TIM12 = 2 as Table 2-1 event 81 for Timer64P0 (printed page 72), while
     * CMP0 = 0 never fires and the compare does not disturb TIM12. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x15, 4, true));       /* PLUSEN, TIMMODE=1h */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  4, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x6c,
                                  2, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 1);
    assert(cdj_c6747_timers_tick(timers) ==
           (1u << (CDJ_C6747_TIMER_OUT_CMP0 + 3)));
    assert(timers[0].tim12 == 2);
    assert(cdj_c6747_timer_event(CDJ_C6747_TIMER_OUT_CMP0 + 3) == 81);
    /* No INTCTLSTAT bit exists for a compare match, so nothing latched. */
    assert(!timers[0].intctlstat);
    /* Without PLUSEN the same match is silent. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x05, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x10,
                                  1, 4, true));
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 2);

    /* Fail-closed refusals.  CLKSRC12 = 1 (TCR bit 8) takes the clock from
     * TM64P_IN12 and TIEN12 = 1 (bit 9) gates the internal clock with the same
     * pin (printed page 1253); neither pin's behaviour is fixed by any manual
     * page, so the counter must not move.  Printed page 1229 confines both to
     * the 1:2 side in dual 32-bit unchained mode, so the 3:4 side keeps
     * counting - refusing there would hide a sequence the manual does fix. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        /* pass 0 sets CLKSRC12 = 0x100 alone, pass 1 TIEN12 = 0x200 alone;
         * ENAMODE12 and ENAMODE34 are both 2h (0x80 and 0x00800000). */
        uint32_t tcr = 0x00800080u | (pass ? 0x200u : 0x100u);
        cdj_c6747_timers_reset(timers);
        assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                      0x07, 4, true));   /* TIMMODE=1h, both RS */
        assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                      2, 4, true));
        assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x1c,
                                      2, 4, true));
        assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                      tcr, 4, true));
        /* PSC34 = 0, so the 3:4 side takes one input clock per TIM34 clock and
         * two clocks reach PRD34 = 2 after passing through 1. */
        for (unsigned i = 0; i < 2; ++i)
            assert(!cdj_c6747_timers_tick(timers));
        assert(!timers[0].tim12 && !(timers[0].intctlstat & 0x2));
        assert(timers[0].tim34 == 2 && (timers[0].intctlstat & 0x00020000));
    }
    /* In 64-bit mode CLKSRC12 "controls the clock source for the entire
     * timer", so the whole thing stops. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x03, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  2, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x180, 4, true));      /* CLKSRC12, ENAMODE12=2h */
    for (unsigned i = 0; i < 8; ++i) assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim12 && !timers[0].tim34 && !timers[0].intctlstat);

    /* 64-bit watchdog mode (TIMMODE = 2h) does not count: a timeout "resets
     * the entire processor" (28.1.6.1, printed page 1240) and no device-level
     * reset exists here to drive, so the counter stays still rather than
     * setting WDFLAG without the reset that gives it meaning. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x0b, 4, true));       /* TIMMODE=2h, both RS */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  2, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x28,
                                  0xa5c64000, 4, true)); /* WDEN, WDKEY A5C6h */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));
    for (unsigned i = 0; i < 64; ++i) assert(!cdj_c6747_timers_tick(timers));
    assert(!timers[0].tim12 && !timers[0].intctlstat &&
           !(timers[0].wdtcr & (1u << 15)));

    /* Timer64P1 is an independent instance and reports in its own bit field. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x24,
                                  0x05, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x18,
                                  1, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x44,
                                  1, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x20,
                                  0x80, 4, true));
    assert(cdj_c6747_timers_tick(timers) ==
           (1u << (CDJ_C6747_TIMER_OUTPUTS + CDJ_C6747_TIMER_OUT_TINT12)));
    assert(!timers[0].tim12 && timers[1].tim12 == 1);

    /* ENAMODE34 = 0 gates the prescale counter as well as TIM34.  Table 28-18
     * (printed page 1254) conditions its whole sentence - "When the timer is
     * enabled, TDDR34 increments every timer clock" - 28.1.5.4.2.2.1 (printed
     * page 1236) repeats it, and Table 28-17's ENAMODE34 = 0 (printed page
     * 1252) is "The timer is disabled (not counting) and maintains current
     * value".  TDDR34 is firmware-writable, so a programmed prescale phase has
     * to survive a disabled interval unscrambled. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x0306, 4, true)); /* PSC34 3, TIMMODE 1h */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x1c,
                                  10, 4, true));
    for (unsigned i = 0; i < 6; ++i) {
        assert(!cdj_c6747_timers_tick(timers));
        assert(!((timers[0].tgcr >> 12) & 15) && !timers[0].tim34);
    }
    /* Enabling it starts the prescaler from the phase that was preserved.
     * PSC34 = 3, so TDDR34 runs 1, 2, 3, 0 and TIM34 increments on the 3 -> 0
     * clock: one TIM34 clock every PSC34 + 1 = 4 input clocks. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x00800000, 4, true)); /* ENAMODE34 = 2h */
    for (unsigned i = 1; i <= 4; ++i) {
        assert(!cdj_c6747_timers_tick(timers));
        assert(((timers[0].tgcr >> 12) & 15) == (i == 4 ? 0 : i));
        assert(timers[0].tim34 == (i == 4 ? 1u : 0u));
    }

    /* Read Reset Mode, 28.1.5.4.2.2.6 printed page 1238: a read of TIM12 with
     * PLUSEN, 32-bit unchained mode and READRSTMODE12 set copies the count to
     * CAP12, reloads PRD12 from REL12 when ENAMODE12 = 3h, and zeroes the
     * counter.  The captured value is the pre-reset count the read returned. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x15, 4, true));   /* PLUSEN, 1h, TIM12RS */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  100, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x34,
                                  55, 4, true));     /* REL12 */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  (3u << 6) | (1u << 10), 4, true));
    assert(!cdj_c6747_timers_tick(timers));
    assert(!cdj_c6747_timers_tick(timers));
    assert(!cdj_c6747_timers_tick(timers));
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10, &value));
    assert(value == 3 && timers[0].cap12 == 3 && !timers[0].tim12 &&
           timers[0].prd12 == 55);
    /* Read Reset Mode is confined to 32-bit unchained mode: Table 28-25
     * (printed page 1259) and 28.1.5.4.2.2.6 (printed page 1238) both scope it
     * to PLUSEN with "the timer ... configured in 32-bit unchained mode", and
     * chained mode (TIMMODE = 3h) is not included.  Testing TIMMODE by a bit
     * mask that only looks at bit 2 would also match 3h. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x1f, 4, true)); /* PLUSEN, TIMMODE 3h, RS */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  100, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  (2u << 6) | (1u << 10), 4, true));
    assert(!cdj_c6747_timers_tick(timers));
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10, &value));
    assert(value == 1 && timers[0].tim12 == 1 && !timers[0].cap12);

    /* Without READRSTMODE12 the read is non-destructive and captures nothing. */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x15, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  100, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  2u << 6, 4, true));
    assert(!cdj_c6747_timers_tick(timers));
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10, &value));
    assert(value == 1 && timers[0].tim12 == 1 && !timers[0].cap12);

    /* Chained mode with ENAMODE12 = 3h reloads BOTH period registers:
     * 28.1.5.4.2.1.1 printed page 1234 says it "reloads the period registers
     * (PRD12 and PRD34) with the value in the period reload registers (REL12
     * and REL34)". */
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x0f, 4, true));   /* TIMMODE 3h, both RS */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  1, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x1c,
                                  1, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x34,
                                  9, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x38,
                                  7, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  3u << 6, 4, true));
    for (unsigned i = 0; i < 12; ++i) cdj_c6747_timers_tick(timers);
    assert(timers[0].prd12 == 9 && timers[0].prd34 == 7);

    cdj_c6747_timers_reset(timers);
    assert(!cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE, 0, 4, true));
    assert(!cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24, 0, 2, true));
    assert(!cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x2c, &value));
    puts("C6747 Timer64P register semantics passed");
}
