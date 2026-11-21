/* SPDX-License-Identifier: GPL-2.0-or-later */
/* emulator/qemu/cdj_c6727_dmax.c: the McASP halves keep the DSP clock's
 * time when the core is late for one. tests/test_c6727_pace.py. */
#include <assert.h>
#include "cdj_c6727_dmax.h"

void info_report(const char *format, ...) { (void)format; }
void warn_report(const char *format, ...) { (void)format; }
static void no_irq(void *chip, int line, int level)
{
    (void)chip; (void)line; (void)level;
}
static uint8_t *no_ram(void *chip, uint32_t addr)
{
    (void)chip; (void)addr;
    return NULL;
}

/* Halves sent over @ms of DSP time, run in the core's 100 us chunks, with
 * the core late (the transmit TCC left set) for @late_ms from 500 ms on. */
static uint64_t halves(int ms, int late_ms, uint64_t *late)
{
    static CdjDmax d;
    int64_t t;

    memset(&d, 0, sizeof(d));
    d.set_irq = no_irq;
    d.ram = no_ram;
    d.enabled = 1u << 6 | 1u << 8;      /* event entries 0: TCC 0, DTCR0 bit 0 */
    cdj_dmax_audio_tx(&d, true);
    for (t = 100000; t <= (int64_t)ms * 1000000; t += 100000) {
        d.ctl[0x80 / 4] = t >= 500000000 && t < 500000000 + late_ms * 1000000LL;
        cdj_dmax_audio_run(&d, t);
    }
    *late = d.audio_overruns;
    return d.audio_halves;
}

int main(void)
{
    const int64_t half = 16 * 1000000000LL / 44100;
    uint64_t on_time, late, n;

    on_time = halves(1000, 0, &late);
    assert(late == 0);
    /* One half every 16 frames of 44.1 kHz from the first, due one half in. */
    assert(on_time == (uint64_t)((1000000000 - 100000) / half));
    /* A core 2 ms late delays halves; the McASP's clock does not move. */
    n = halves(1000, 2, &late);
    assert(late > 0);
    assert(n == on_time);
    return 0;
}
