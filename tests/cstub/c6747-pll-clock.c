/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stddef.h>
#include "cdj_c674x.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_timer.h"
static CdjC6747Pll pll;
static void tick(void *opaque) { cdj_c6747_pll_tick(opaque); }
static bool read_bus(void *unused, uint32_t a, uint32_t *v)
{ (void)unused; return cdj_c6747_pll_read(&pll, a, v); }
static bool write_bus(void *unused, uint32_t a, uint64_t v, unsigned size, bool commit)
{ (void)unused; return cdj_c6747_pll_write(&pll, a, v, size, commit); }
/* Every expected value below is hand-computed from the manuals, never read
 * back out of this emulator.  The one board fact is OSCIN = 16.9344 MHz
 * (RRV4356 X501, recorded in HANDOFF.md); every divider term cites its page
 * of SPRUH91D or SPRS377F. */
static void rates(void)
{
    uint64_t num;
    uint32_t den;

    /* AUXCLK = OSCIN: SPRUH91D Table 7-1 printed page 118 gives AUXCLK's
     * ratio as the PLL bypass clock, and 6.2 printed page 105 makes the
     * bypass clock the OSCIN reference itself.  16934400 Hz. */
    assert(cdj_c6747_pll_auxclk_hz() == 16934400u);

    /* POR configuration is bypass (PLLCTL reset value has PLLEN = 0,
     * SPRUH91D Figure 7-4 printed page 123), so every SYSCLKn is OSCIN over
     * its own PLLDIVn reset ratio (Figures 7-8 .. 7-15, printed pages
     * 126 .. 130): /1, /2, /3, /4, /3, /1, /6. */
    cdj_c6747_pll_reset(&pll);
    static const uint32_t bypass_ratio[7] = {1, 2, 3, 4, 3, 1, 6};
    for (unsigned n = 1; n <= 7; ++n) {
        assert(cdj_c6747_pll_sysclk_hz(&pll, n, &num, &den));
        assert(num == 16934400u && den == bypass_ratio[n - 1]);
    }
    assert(!cdj_c6747_pll_sysclk_hz(&pll, 0, &num, &den));
    assert(!cdj_c6747_pll_sysclk_hz(&pll, 8, &num, &den));
    /* 16934400 / 4 = 4233600 Hz on SYSCLK4, exactly. */
    assert(cdj_c6747_pll_sysclk_hz(&pll, 4, &num, &den) &&
           num / den == 4233600u && num % den == 0);

    /* PLL mode with PLLM = 22 (x23, multiplier is PLLM + 1, SPRUH91D 7.2
     * printed page 116), PREDIV /1 and POSTDIV /1 (written while PLLRST is
     * asserted):
     * Note on the operating point, because review asked whether this one is
     * legal: SPRS377F grades the C6747 at 300, 375 and 456 MHz max CPU
     * frequency (device nomenclature, and the Recommended Operating Conditions
     * tables).  389,491,200 Hz is above the 375 MHz grade and below the 456 MHz
     * one, so it is legal only on a 456 MHz part.  This model deliberately does
     * NOT enforce a maximum: the grade is a property of the fitted device, the
     * NXS DSP is a Pioneer-marked D810K013, and nothing in the register state
     * tells us which grade it is.  The rate below is what the board is measured
     * to run at (reports/dante-dsp-ram-benchmark records dsp_hz 389491200 from
     * TSCL on hardware), not an assertion that any C6747 may run there.
     *
     *   SYSCLK1 = 16934400 x 23 / (1 x 1 x 1) = 389491200 Hz
     *   SYSCLK2 = 389491200 / 2                = 194745600 Hz
     *   SYSCLK4 = 389491200 / 4                =  97372800 Hz
     * SPRS377F Table 6-4 printed page 73 allows this point: PLLREF 16.9344
     * MHz is inside 12 .. 50 MHz, PLLM x23 inside x4 .. x32, and PLLOUT
     * 389.4912 MHz inside 300 .. 600 MHz. */
    assert(cdj_c6747_pll_write(&pll, 0x01c11100, 0x1c0, 4, true));
    assert(cdj_c6747_pll_write(&pll, 0x01c11110, 22, 4, true));
    assert(cdj_c6747_pll_write(&pll, 0x01c11128, 0x8000, 4, true));
    for (unsigned i = 0; i < 17; ++i) cdj_c6747_pll_tick(&pll);
    assert(cdj_c6747_pll_write(&pll, 0x01c11100, 0x1c8, 4, true));
    for (unsigned i = 0; i < 418; ++i) cdj_c6747_pll_tick(&pll);
    assert(cdj_c6747_pll_write(&pll, 0x01c11100, 0x1c9, 4, true));
    assert(cdj_c6747_pll_sysclk_hz(&pll, 1, &num, &den) &&
           num / den == 389491200u && num % den == 0);
    assert(cdj_c6747_pll_sysclk_hz(&pll, 2, &num, &den) &&
           num / den == 194745600u && num % den == 0);
    assert(cdj_c6747_pll_sysclk_hz(&pll, 4, &num, &den) &&
           num / den == 97372800u && num % den == 0);
    /* AUXCLK is unchanged by PLL mode - it is the bypass clock. */
    assert(cdj_c6747_pll_auxclk_hz() == 16934400u);

    /* A cleared divider enable is "Disable", and SPRUH91D Table 7-24 printed
     * page 137 ties SYSTAT's on/off status to DnEN, so the clock is off and
     * has no frequency.  PLLDIV2 = 0 takes effect on the next GO. */
    assert(cdj_c6747_pll_write(&pll, 0x01c1111c, 0, 4, true));
    assert(cdj_c6747_pll_write(&pll, 0x01c11138, 1, 4, true));
    for (unsigned i = 0; i < 8; ++i) cdj_c6747_pll_tick(&pll);
    assert(!cdj_c6747_pll_sysclk_hz(&pll, 2, &num, &den));
    assert(cdj_c6747_pll_sysclk_hz(&pll, 1, &num, &den));

    /* Timer64P: the internal clock source is AUXCLK (SPRUH91D Table 6-2
     * printed page 104, Table 7-1 printed page 118, 28.1.5.2.1 printed page
     * 1229) and timer 1:2 has no prescaler (28.1.5.4.2.2.2 printed page
     * 1236), so its input clock is 16934400 Hz whether the PLL is bypassed
     * or multiplying.  No PSC gate is in the path: Tables 8-1 and 8-2
     * printed pages 140 and 141 assign no LPSC to Timer64P0 or Timer64P1. */
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    uint32_t aux = cdj_c6747_pll_auxclk_hz();
    cdj_c6747_timers_reset(timers);
    for (unsigned t = 0; t < CDJ_C6747_TIMER_COUNT; ++t) {
        assert(cdj_c6747_timer_input_hz(&timers[t], aux,
                                        CDJ_C6747_TIMER_HALF_12, &num, &den));
        assert(num / den == 16934400u && num % den == 0);
    }
    /* TIMMODE = 0 at reset is 64-bit mode (TGCR, printed page 1254): the 3:4
     * half has no clock of its own there. */
    assert(!cdj_c6747_timer_input_hz(&timers[0], aux,
                                     CDJ_C6747_TIMER_HALF_34, &num, &den));
    /* Dual 32-bit unchained (TIMMODE = 1h) with PSC34 = 3 prescales the 3:4
     * half by PSC34 + 1 = 4 (28.1.5.4.2.2.1 printed page 1236):
     *   16934400 / 4 = 4233600 Hz. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x304, 4, true));
    assert(cdj_c6747_timer_input_hz(&timers[0], aux, CDJ_C6747_TIMER_HALF_34,
                                    &num, &den));
    assert(num / den == 4233600u && num % den == 0);
    assert(cdj_c6747_timer_input_hz(&timers[0], aux, CDJ_C6747_TIMER_HALF_12,
                                    &num, &den) && num / den == 16934400u);
    /* CLKSRC12 = 1 selects the TM64P_IN12 pin (TCR bit 8, printed page
     * 1253); no manual page fixes that pin's frequency, so the 1:2 rate is
     * refused.  But printed page 1229 scopes that precisely: "If the timer is
     * configured in dual 32-bit unchained mode (TIMMODE = 01 in TGCR),
     * CLKSRC12 controls the timer 1:2 side of the timer only."  TIMMODE is
     * still 1h here, so the 3:4 side keeps running from AUXCLK through its own
     * PSC34 prescaler and its rate is STILL derivable - refusing it would hide
     * a rate the manual fixes. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x100, 4, true));
    assert(!cdj_c6747_timer_input_hz(&timers[0], aux, CDJ_C6747_TIMER_HALF_12,
                                     &num, &den));
    assert(cdj_c6747_timer_input_hz(&timers[0], aux, CDJ_C6747_TIMER_HALF_34,
                                    &num, &den));
    assert(num / den == 4233600u && num % den == 0);
    /* Outside dual 32-bit unchained mode the same bit does take the whole
     * timer from the pin, so both halves are refused.  TIMMODE = 0h is 64-bit
     * mode (TGCR, printed page 1254). */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x300, 4, true));
    assert(!cdj_c6747_timer_input_hz(&timers[0], aux, CDJ_C6747_TIMER_HALF_12,
                                     &num, &den));
    assert(!cdj_c6747_timer_input_hz(&timers[0], aux, CDJ_C6747_TIMER_HALF_34,
                                     &num, &den));
    /* Restore dual 32-bit unchained with PSC34 = 3 for what follows. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x304, 4, true));

    /* McASP0 transmit chain off the same AUXCLK, with HCLKXDIV = 2 (/3),
     * CLKXDIV = 1 (/2), XMOD = 2 (2-slot TDM) and XSSZ = Fh (32-bit slots):
     *   AHCLKX = 16934400 / 3            = 5644800 Hz
     *   ACLKX  = 5644800 / 2             = 2822400 Hz
     *   AFSX   = 2822400 / (32 x 2 bits) =   44100 Hz
     * SPRUH91D Figure 24-15 printed page 996 and Tables 24-37 and 24-38
     * printed pages 1077 and 1078 give the two dividers; printed page 1011
     * gives XMOD as the TDM slot count (2h..20h); Table 24-35 printed
     * page 1075 gives XSSZ = Fh as 32 bits. */
    CdjC6747McaspControl mcasp;
    cdj_c6747_mcasp_control_reset(&mcasp);
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000b4, 0x8002, 4, true));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000b0, 0x61, 4, true));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000ac, 0x102, 4, true));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000a8, 0xf0, 4, true));
    assert(cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_AHCLKX,
                                       &num, &den));
    assert(num / den == 5644800u && num % den == 0);
    assert(cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_ACLKX,
                                       &num, &den));
    assert(num / den == 2822400u && num % den == 0);
    assert(cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_AFSX,
                                       &num, &den));
    assert(num / den == 44100u && num % den == 0 && den == 384u);
    /* An externally sourced high-frequency clock (HCLKXM = 0) or bit clock
     * (CLKXM = 0) is a board fact, not a manual fact: refuse. */
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000b4, 0x0002, 4, true));
    assert(!cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux,
                                        CDJ_C6747_MCASP_AHCLKX, &num, &den));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000b4, 0x8002, 4, true));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000b0, 0x41, 4, true));
    assert(cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_AHCLKX,
                                       &num, &den));
    assert(!cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_ACLKX,
                                        &num, &den));
    assert(!cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_AFSX,
                                        &num, &den));
    /* Burst mode (XMOD = 0) and an external frame sync (FSXM = 0) leave the
     * frame rate undefined by these manuals. */
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000b0, 0x61, 4, true));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000ac, 0x100, 4, true));
    assert(!cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_AFSX,
                                        &num, &den));
    assert(cdj_c6747_mcasp_control_write(&mcasp, 0x01d000ac, 2, 4, true));
    assert(!cdj_c6747_mcasp_tx_clock_hz(&mcasp, 0, aux, CDJ_C6747_MCASP_AFSX,
                                        &num, &den));
    assert(!cdj_c6747_mcasp_tx_clock_hz(&mcasp, 3, aux, CDJ_C6747_MCASP_ACLKX,
                                        &num, &den));
}

int main(void)
{
    rates();
    for (unsigned split = 0; split < 2; ++split) {
        CdjC674x cpu;
        cdj_c6747_pll_reset(&pll); cdj_c674x_reset(&cpu, 0x1000);
        cpu.cycle_tick = tick; cpu.cycle_opaque = &pll;
        cpu.r[0][5] = 0x01c11138; cpu.r[0][3] = 1;
        CdjC674xPacket packet = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.pc = 0x1000, .word = 0x01940274}}}; /* STW A3,*A5 */
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(!pll.go_remaining && cpu.store_count == 1);
        packet.instructions[0].word = 0;
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(!pll.go_remaining);
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(cpu.cycles == 3 && pll.go_remaining == 8); /* E3 starts GO */
        if (split) {
            for (unsigned i = 0; i < 8; ++i) {
                assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
                assert(pll.go_remaining == 7 - i);
            }
        } else {
            packet.instructions[0].word = 7u << 13; /* NOP 8, one API call */
            assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        }
        assert(cpu.cycles == 11 && !pll.go_remaining);
        /* E3 read observes completion inside a protected load's NOPs. */
        assert(cdj_c6747_pll_write(&pll, 0x01c11138, 1, 4, true));
        for (unsigned i = 0; i < 5; ++i) cdj_c6747_pll_tick(&pll);
        cpu.r[0][5] = 0x01c1113c;
        packet.instructions[0].word = 0x01940264;
        packet.instructions[0].header = 1u << 20;
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(cpu.cycles == 16 && cpu.r[0][3] == 4 && !pll.go_remaining);
    }
    return 0;
}
