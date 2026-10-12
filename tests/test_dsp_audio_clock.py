"""CDJ_NXS_DSP_AUDIO_CLOCK=virtual: compile the board's slot clock
(thread_audio_tick, dsp_thread_slip) against stub peripherals and drive it the
way dsp_thread_run does, with a host that is faster or slower than the
configured DSP clock."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]

SHIM = r'''
#include <time.h>
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cdj_dsp_audio_clock.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NANOSECONDS_PER_SECOND 1000000000LL
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define CDJ_C6747_MCASP_AFSX 0
typedef int QemuMutex, QemuCond, QemuThread, QEMUBH;
typedef struct Notifier { int unused; } Notifier;
static void info_report(const char *format, ...) { (void)format; }
static void cpu_disable_ticks(void) {}
static void cpu_enable_ticks(void) {}
static bool runstate_is_running(void) { return true; }
typedef struct {
    struct { uint64_t packets; uint32_t pc, fault_pc; const char *fault; } cpu;
    struct { uint32_t gblctl[3], afsxctl[3]; } mcasp_control;
    bool thread_audio_clock;
    CdjDspAudioClock audio_clock;
    uint64_t audio_clock_next_packets;
} NxsHpi;
static uint64_t frame_hz = 44100;
static bool cdj_c6747_mcasp_tx_clock_hz(const void *m, unsigned i, uint64_t aux,
                                        int pin, uint64_t *num, uint32_t *den)
{
    (void)m; (void)i; (void)aux; (void)pin;
    *num = frame_hz;
    *den = 1;
    return true;
}
static uint64_t cdj_c6747_pll_auxclk_hz(void) { return 24576000; }
static uint64_t fired, last_fire, burst, max_burst, gap_min = UINT64_MAX, gap_max;
static bool advance_functional_mcasp_slots(NxsHpi *s)
{
    if (fired && s->cpu.packets == last_fire) {
        ++burst;
    } else {
        if (fired) {
            gap_min = MIN(gap_min, s->cpu.packets - last_fire);
            gap_max = MAX(gap_max, s->cpu.packets - last_fire);
        }
        burst = 1;
    }
    max_burst = MAX(max_burst, burst);
    last_fire = s->cpu.packets;
    ++fired;
    return true;
}
'''

DRIVER = r'''
#define DSP_THREAD_AUDIO_CHUNK_TEST DSP_THREAD_AUDIO_CHUNK
static NxsHpi hpi;

/* dsp_thread_run's pacing with a host executing @host_mpps million packets
 * per second of virtual time; returns virtual ns at the end. */
static int64_t run(int64_t from, int64_t until, uint64_t host_mpps)
{
    NxsDspThread *t = &dsp_thread;
    NxsHpi *s = &hpi;
    int64_t virt = from;
    while (virt < until) {
        dsp_thread_slip(s, virt);
        uint64_t limit = dsp_thread_packets_at(virt + t->quantum_ns);
        if (s->cpu.packets >= limit) {     /* a quantum ahead: wait */
            virt += t->quantum_ns;
            continue;
        }
        uint64_t quota = MIN(limit - s->cpu.packets, DSP_THREAD_AUDIO_CHUNK);
        for (uint64_t i = 0; i < quota; ++i) {
            ++s->cpu.packets;
            assert(thread_audio_tick(s));
        }
        virt += (int64_t)(quota * 1000 / host_mpps);
    }
    return virt;
}

static void reset(void)
{
    NxsDspThread *t = &dsp_thread;
    memset(&hpi, 0, sizeof(hpi));
    memset(t, 0, sizeof(*t));
    t->packets_per_us = 150;
    t->quantum_ns = 1000000;
    hpi.thread_audio_clock = true;
    hpi.mcasp_control.gblctl[1] = 0x1f00;
    hpi.mcasp_control.afsxctl[1] = 2u << 7;
    fired = last_fire = burst = max_burst = gap_max = 0;
    gap_min = UINT64_MAX;
}

int main(void)
{
    /* Deadlines are exact: slot n is due at floor(n * 1e9 / 88200) ns. */
    CdjDspAudioClock c = {0};
    assert(cdj_dsp_audio_clock_start(&c, 88200, 1, 0));
    for (uint64_t n = 1; n <= 88200 * 3; ++n) {
        assert(c.next_ns == (int64_t)(n * 1000000000ull / 88200));
        cdj_dsp_audio_clock_fire(&c);
    }
    assert(c.slots == 88200 * 3 && !c.late);
    assert(!cdj_dsp_audio_clock_start(&c, 0, 1, 0));

    /* 1. A host faster than the 150 M packets/s clock: the DSP stays within a
     * quantum ahead, every slot gets its 1700.68-packet budget, none is late,
     * and slots match virtual time. */
    reset();
    int64_t end = run(0, 200000000, 1000);
    uint64_t expect = (uint64_t)end * 88200 / 1000000000;
    assert(hpi.audio_clock.slots + 100 >= expect &&
           hpi.audio_clock.slots <= expect + 88200 / 1000 + 1);
    assert(hpi.audio_clock.late == 0);
    assert(gap_min == 1700 && gap_max == 1701 && max_burst == 1);
    assert(fired == hpi.audio_clock.slots);
    printf("fast: virtual=%" PRId64 " slots=%" PRIu64 " late=%" PRIu64 "\n",
           end, hpi.audio_clock.slots, hpi.audio_clock.late);

    /* 2. A host at a tenth of the clock: audio time still follows virtual
     * time (not the DSP's packets), ~90% of slots are counted as underruns,
     * and each slip's burst stays about one chunk of host time. */
    reset();
    end = run(0, 200000000, 15);
    expect = (uint64_t)end * 88200 / 1000000000;
    assert(hpi.audio_clock.slots + 100 >= expect && hpi.audio_clock.slots <= expect + 89);
    assert(hpi.audio_clock.late * 100 >= hpi.audio_clock.slots * 85);
    assert(hpi.audio_clock.late < hpi.audio_clock.slots);
    assert(max_burst <= DSP_THREAD_AUDIO_CHUNK * 88200 / 15000000 + 2);
    assert(hpi.cpu.packets <= (uint64_t)end * 15 / 1000 + DSP_THREAD_AUDIO_CHUNK);
    printf("slow: virtual=%" PRId64 " slots=%" PRIu64 " late=%" PRIu64 " burst=%" PRIu64 "\n",
           end, hpi.audio_clock.slots, hpi.audio_clock.late, max_burst);

    /* 3. Stop and restart: no slots while stopped, the clock restarts one
     * period after the DSP's time, counters keep accumulating. */
    uint64_t before = hpi.audio_clock.slots;
    hpi.mcasp_control.gblctl[1] = 0;
    end = run(end, end + 50000000, 1000);
    assert(hpi.audio_clock.slots == before && !hpi.audio_clock.rate_num);
    hpi.mcasp_control.gblctl[1] = 0x1f00;
    end = run(end, end + 50000000, 1000);
    assert(hpi.audio_clock.slots > before + 4000);

    /* 4. A rate change (48 kHz) restarts the clock at the new rate. */
    frame_hz = 48000;
    before = hpi.audio_clock.slots;
    int64_t from = end;
    end = run(end, end + 100000000, 1000);
    expect = (uint64_t)(end - from) * 96000 / 1000000000;
    assert(hpi.audio_clock.rate_num == 96000);
    assert(hpi.audio_clock.slots - before + 200 >= expect &&
           hpi.audio_clock.slots - before <= expect + 200);
    puts("ok");
    return 0;
}
'''


def section(source, start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]


def test_dsp_audio_clock(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    source = (ROOT / 'emulator/qemu/cdj2000_nxs_hpi.c').read_text()
    code = (SHIM +
            section(source, 'typedef struct {\n    bool on, quit;', 'static void run_dsp(') +
            section(source, "/* The DSP thread's packet count", 'static bool functional_audio_tick(') +
            section(source, '/* Packets the DSP should have executed', 'static void *dsp_thread_run(') +
            DRIVER)
    harness = tmp_path / 'audio_clock.c'
    harness.write_text(code)
    binary = tmp_path / 'audio_clock'
    subprocess.run([cc, '-std=gnu11', '-O2', '-g', '-Wall', '-Wno-unused-function',
                    '-I', str(ROOT / 'emulator/qemu'), str(harness), '-o', str(binary)],
                   check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip().endswith('ok')
