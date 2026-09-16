/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "cdj_c674x.h"
#include "cdj_c6747_intc.h"
#include "cdj_c6747_timer.h"

/* IC-DEV-EVENT-SOURCES' acceptance test: raise T64P0_TINT12 from
 * cdj_c6747_timer.c on a period match and follow it all the way to a CPU
 * interrupt, over the same three hops the boards wire in their cycle_tick -
 * cdj_c6747_timers_tick() -> cdj_c6747_intc_deliver_event() ->
 * cdj_c6747_intc_cpu_pending() -> cdj_c674x_interrupt().
 *
 * Nothing here asserts elapsed time.  The timer is clocked by calling
 * cdj_c6747_timers_tick() a counted number of times and the only claim is the
 * ORDER of the resulting register and interrupt state. */
static void timer_event_reaches_the_cpu(void)
{
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    CdjC6747Intc intc;
    CdjC6747IntcDelivery delivery;
    CdjC674x cpu;
    uint32_t outputs = 0, fired = 0;

    cdj_c6747_timers_reset(timers);
    cdj_c6747_intc_reset(&intc);
    cdj_c6747_intc_delivery_reset(&delivery);

    /* Timer64P0, dual 32-bit unchained (TGCR TIMMODE = 1h) with timer 1:2 out
     * of reset, PRD12 = 2, PRDINTEN12 = 1 (INTCTLSTAT bit 0, printed page
     * 1259) and ENAMODE12 = 2h continuous.  28.1.5.4.2.2.2 printed page 1236
     * puts the match at the second input clock. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x05, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x18,
                                  2, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  1, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  0x80, 4, true));
    assert(!cdj_c6747_timers_tick(timers));
    outputs = cdj_c6747_timers_tick(timers);
    assert(outputs == (1u << CDJ_C6747_TIMER_OUT_TINT12));
    assert(timers[0].tim12 == 2 && (timers[0].intctlstat & 0x2));

    /* Table 2-1 printed page 70 names that output T64P0_TINT12 = event 4, and
     * cdj_c6747_intc_reset() leaves INTMUX1 at 0x07060504, so CPUINT4 selects
     * event 4 - the reset mux.  Feed every reported output the way a board
     * does, so the mapping under test is the shipped one. */
    for (unsigned bit = 0; outputs >> bit; ++bit)
        if (outputs & (1u << bit))
            assert(cdj_c6747_intc_deliver_event(&intc, &delivery,
                                                cdj_c6747_timer_event(bit)));
    assert(intc.interrupt_mux[0] == 0x07060504);
    assert(intc.event_flag[0] == (1u << 4));
    fired = cdj_c6747_intc_cpu_pending(&delivery);
    assert(fired == (1u << 4));

    /* SPRUFE8B 5.4.1: IFR is sticky.  With IE4 clear in IER the request must
     * latch in IFR bit 4 and NOT vector - masked is not dropped.  ISTP is
     * 0x1000, and IER needs NMIE (bit 1) as well as IE4 before INT4 is
     * recognized, so this first call has all three architectural enables
     * false. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.control[5] = 0x1000;                        /* ISTP */
    assert(cdj_c674x_interrupt(&cpu, fired));
    assert(cpu.control[2] == (1u << 4) && cpu.pc == 0x1000);

    /* The pulse was consumed, so nothing re-presents it; only the IFR latch
     * carries the event now. */
    assert(!cdj_c6747_intc_cpu_pending(&delivery));
    cpu.control[1] |= 1u;                           /* CSR GIE */
    cpu.control[26] |= 1u;                          /* TSR GIE */
    assert(cdj_c674x_interrupt(&cpu, 0));
    assert(cpu.control[2] == (1u << 4) && cpu.pc == 0x1000);

    /* Unmasking alone vectors the latched event: SPRUFE8B's INT4 fetch packet
     * is at ISTP + 0x80 and acceptance clears its IFR bit. */
    cpu.control[4] = (1u << 4) | 3u;                /* IER: NMIE, IE4 */
    assert(cdj_c674x_interrupt(&cpu, 0));
    assert(cpu.pc == 0x1080 && cpu.control[6] == 0x1000 && !cpu.control[2]);

    /* EVTFLAG is a separate sticky status and survives CPU acceptance until
     * firmware writes EVTCLR (SPRUFK5A 7.2.2). */
    assert(intc.event_flag[0] == (1u << 4));
    assert(cdj_c6747_intc_write_delivery(&intc, &delivery,
                                         CDJ_C6747_INTC_EVTCLR0,
                                         1u << 4, 4, true));
    assert(!intc.event_flag[0]);

    /* The next period match delivers again rather than being swallowed: the
     * counter resets to 0 "on the cycle after matching", so clocks 3 and 4
     * after the first match bring TIM12 back to 2 = PRD12. */
    assert(!cdj_c6747_timers_tick(timers) && !timers[0].tim12);
    assert(!cdj_c6747_timers_tick(timers) && timers[0].tim12 == 1);
    assert(cdj_c6747_timers_tick(timers) ==
           (1u << CDJ_C6747_TIMER_OUT_TINT12));
    assert(cdj_c6747_intc_deliver_event(&intc, &delivery, 4));
    assert(cdj_c6747_intc_cpu_pending(&delivery) == (1u << 4));

    /* With PRDINTEN12 cleared the same match still sets PRDINTSTAT12 but
     * generates no event at all, so nothing reaches the INTC - Table 28-24
     * printed pages 1258-1259 separate the status from the enable.  Writing
     * 0x2 clears PRDINTSTAT12 (R/W1C) and leaves PRDINTEN12 at the written 0. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  0x2, 4, true));
    assert(!timers[0].intctlstat);
    for (unsigned i = 0; i < 3; ++i)
        assert(!cdj_c6747_timers_tick(timers));
    assert(timers[0].tim12 == 2 && timers[0].intctlstat == 0x2);
    assert(!cdj_c6747_intc_cpu_pending(&delivery));
}

int main(void)
{
    CdjC6747Intc s;
    CdjC6747IntcDelivery request;
    uint32_t value;
    cdj_c6747_intc_reset(&s);
    cdj_c6747_intc_delivery_reset(&request);
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(s.event_mask[0] == 0xf && !s.event_mask[1] &&
           s.exception_mask[0] == UINT32_MAX &&
           s.interrupt_mux[0] == 0x07060504 &&
           s.interrupt_mux[2] == 0x0f0e0d0c);

    assert(!cdj_c6747_intc_deliver_event(&s, &request, 3));
    assert(cdj_c6747_intc_deliver_event(&s, &request, 4));
    assert(cdj_c6747_intc_cpu_pending(&request) == (1u << 4));
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_deliver_event(&s, &request, 34));
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_INTMUX1 + 8,
                                0x220e0d0c, 4, true));
    /* Remapping does not replay a sticky status flag. A subsequent direct
     * event is delivered once even though EVTFLAG was already set. */
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_deliver_event(&s, &request, 34));
    assert(cdj_c6747_intc_cpu_pending(&request) == (1u << 15));
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_deliver_event(&s, &request, 127));
    assert(!cdj_c6747_intc_deliver_event(&s, &request, 128));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTFLAG0, &value) &&
           value == 0x10);
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTFLAG0 + 4, &value) &&
           value == 4);

    /* EVTMASK affects only event-combiner outputs. INT4 still receives direct
     * event 4 while masked; INT5 receives combined event 0 only on inactive to
     * active transitions, and each request pulse is consumed exactly once. */
    CdjC6747Intc combined_intc;
    CdjC6747IntcDelivery combined_request;
    cdj_c6747_intc_reset(&combined_intc);
    cdj_c6747_intc_delivery_reset(&combined_request);
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_INTMUX1,
        0x07060004, 4, true));
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_EVTMASK0,
        1u << 4, 4, true));
    assert(cdj_c6747_intc_deliver_event(&combined_intc, &combined_request, 4));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == (1u << 4));
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_EVTMASK0,
        0, 4, true));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == (1u << 5));
    assert(cdj_c6747_intc_deliver_event(&combined_intc, &combined_request, 5));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == 0);
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_EVTCLR0,
        (1u << 4) | (1u << 5), 4, true));
    assert(cdj_c6747_intc_deliver_event(&combined_intc, &combined_request, 5));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == (1u << 5));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == 0);

    /* Check-phase writes are atomic and command registers do not invent
     * readback. EVTSET cannot manufacture reserved events 0..3. */
    assert(cdj_c6747_intc_write_delivery(
        &s, &request, CDJ_C6747_INTC_EVTSET0, UINT32_MAX, 4, false));
    assert(s.event_flag[0] == 0x10);
    assert(request.cpu_request == 0);
    assert(cdj_c6747_intc_write_delivery(
        &s, &request, CDJ_C6747_INTC_EVTSET0, UINT32_MAX, 4, true));
    assert(s.event_flag[0] == 0xfffffff0);
    assert(cdj_c6747_intc_cpu_pending(&request) == 0x7ff0u);
    assert(!cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTSET0, &value));
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTCLR0,
                                0x80000010, 4, true));
    assert(s.event_flag[0] == 0x7fffffe0);

    /* Masked event and exception views are derived, not stale latches. */
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTMASK0,
                                0x7fffff00, 4, true));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTMASK0, &value) &&
           value == 0x7fffff0f);
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_MEVTFLAG0, &value) &&
           value == 0xe0);
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EXPMASK0,
                                0x7fffff00, 4, true));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_MEXPFLAG0, &value) &&
           value == 0xe0);

    /* Four 7-bit selectors occupy separate bytes; reserved high bits read 0. */
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_INTMUX1,
                                UINT32_MAX, 4, true));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_INTMUX1, &value) &&
           value == 0x7f7f7f7f);

    assert(!cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTFLAG0, 0, 4, true));
    assert(!cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTCLR0, 0, 2, true));
    assert(!cdj_c6747_intc_read(&s, CDJ_C6747_INTC_BASE + 0x10, &value));
    timer_event_reaches_the_cpu();
    puts("C6747 interrupt controller register semantics passed");
}
