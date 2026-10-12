"""Compile the NXS board's idle-loop skip proof against its real peripheral models."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_dsp_idle_proof_rules(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    directory = ROOT / 'emulator/qemu'
    source = (directory / 'cdj2000_nxs_hpi.c').read_text()
    prefix = source[source.index('#include "cdj_c674x.h"'):source.index('static NxsHpi *nxs_hpi;')]
    start = source.index('static uint8_t *dsp_memory_span(')
    span = source[start:source.index('\n}\n', start) + 3]
    start = source.index('/*\n * Idle-loop skip.')
    idle = source[start:source.index('static bool dsp_write(void *opaque', start)]
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef int MemoryRegion;
typedef int QEMUTimer;
typedef int AudioBackend;
typedef int SWVoiceOut;
typedef int Notifier;
typedef int QemuMutex;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static uint32_t ldl_le_p(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
'''
    stubs = r'''
/* The real probe and slot tick drive EDMA/McASP models; scripted here. */
static unsigned probe_calls, probe_visible_from = ~0u;
static int functional_slot_probe(NxsHpi *s, CdjC6747Edma *edma,
                                 CdjC6747McaspControl *mcasp)
{
    (void)s; (void)edma; (void)mcasp;
    return probe_calls++ >= probe_visible_from;
}
static uint64_t tick_packets[16];
static unsigned tick_count;
static bool functional_audio_tick(NxsHpi *s)
{
    assert(tick_count < 16);
    tick_packets[tick_count++] = s->cpu.packets;
    return true;
}
'''
    checks = r'''
int main(void)
{
    NxsHpi *s = calloc(1, sizeof(*s));
    s->shared_ram = calloc(1, SHARED_RAM_SIZE);
    s->sdram = calloc(1, SDRAM_SIZE);
    assert(s && s->shared_ram && s->sdram);
    cdj_c6747_cache_reset(&s->cache);
    cdj_c6747_emifb_reset(&s->emifb);
    cdj_c6747_pll_reset(&s->pll);
    s->pll.reset_age = 17;
    s->cpu.pc = L2_BASE + 0x100;
    s->cpu.cycles = 20;
    s->cpu.control_ready[5] = 10;
    s->cpu.control_ready[31] = 7;
    assert(dsp_idle_quiescent(s));
    dsp_idle_anchor(s, 0);
    assert(dsp_idle_repeat(s));

    /* Registers, PC and in-flight work must all match. */
    s->cpu.r[0][10] = 1; assert(!dsp_idle_repeat(s)); s->cpu.r[0][10] = 0;
    s->cpu.pc += 4; assert(!dsp_idle_repeat(s)); s->cpu.pc -= 4;
    s->cpu.load_count = 1; assert(!dsp_idle_repeat(s)); s->cpu.load_count = 0;
    assert(dsp_idle_repeat(s));

    /* A delayed-control cycle may only stay put or move by exactly one period
     * (10 cycles here) and be passed; the loop-context slot must not change. */
    s->cpu.cycles = 30; s->cpu.control_ready[5] = 20; assert(dsp_idle_repeat(s));
    s->cpu.control_ready[5] = 15; assert(!dsp_idle_repeat(s));
    s->cpu.control_ready[5] = 10; assert(dsp_idle_repeat(s));
    s->cpu.control_ready[31] = 8; assert(!dsp_idle_repeat(s));
    s->cpu.control_ready[31] = 7;

    /* Skipping k periods is k periods of counters, delayed cycles and PLL. */
    s->cpu.packets = 7;
    s->idle_anchor_packets = 3;           /* period: 4 packets, 10 cycles */
    s->cpu.control_ready[5] = 20;
    /* Whole periods that fit, minus one left to execute afterwards. */
    assert(dsp_idle_periods(s, 41) == 9);
    CdjC6747Pll pll = s->pll;
    for (unsigned i = 0; i < 30; ++i) cdj_c6747_pll_tick(&pll);
    dsp_idle_skip(s, 3);
    assert(s->cpu.packets == 19 && s->cpu.cycles == 60);
    assert(s->cpu.control_ready[5] == 50 && s->cpu.control_ready[31] == 7);
    assert(!memcmp(&pll, &s->pll, sizeof(pll)));
    s->cpu.packets = 7; s->cpu.cycles = 30; s->cpu.control_ready[5] = 10;

    /* A running functional McASP transmitter bounds the skip to its next
     * slot edge: packet-interval mode at packets % 1024, cycle clock at
     * mcasp_next_cycle. */
    s->functional_audio = true;
    assert(dsp_idle_quiescent(s));
    assert(dsp_idle_periods(s, 100000) == 24999);  /* transmitter idle */
    s->mcasp_control.gblctl[2] = 0x1f00;
    s->cpu.packets = 1000; s->idle_anchor_packets = 996;
    s->tx_capture = (FILE *)1;                     /* capture: every slot visible */
    assert(dsp_idle_periods(s, 100000) == 4);      /* 1000 + 5 * 4 < 1024 */
    s->tx_capture = NULL;
    probe_calls = 0; probe_visible_from = 2;       /* slots 1024, 2048 invisible */
    assert(dsp_idle_periods(s, 100000) == 516);    /* 1000 + 517 * 4 < 3072 */
    assert(probe_calls == 3);
    probe_calls = 0; probe_visible_from = 0;
    s->cpu.packets = 1020; s->idle_anchor_packets = 1016;
    assert(dsp_idle_periods(s, 100000) == 0);
    probe_visible_from = ~0u;
    /* A skip from 1000 to 3100 replays the slots at 1024, 2048 and 3072. */
    s->cpu.packets = 3100; tick_count = 0;
    assert(dsp_idle_skip_slots(s, 1000));
    assert(tick_count == 3 && tick_packets[0] == 1024 &&
           tick_packets[1] == 2048 && tick_packets[2] == 3072);
    assert(s->cpu.packets == 3100);
    s->cpu.packets = 3100; tick_count = 0;
    assert(dsp_idle_skip_slots(s, 3072) && tick_count == 0);
    s->mcasp_control.gblctl[2] = 0;
    s->cycle_audio_clock = true;
    s->mcasp_control.gblctl[1] = 0x1f00;
    s->mcasp_next_cycle = 0; assert(dsp_idle_periods(s, 100000) == 0);
    s->mcasp_next_cycle = 101; assert(dsp_idle_periods(s, 100000) == 6);
    s->mcasp_control.gblctl[1] = 0; s->cycle_audio_clock = false;
    s->functional_audio = false;
    s->cpu.packets = 7; s->idle_anchor_packets = 3;

    /* Memory must be back to its anchor value, whatever happened between. */
    s->l2[0x40] = 0x11;
    dsp_idle_note_write(s, L2_BASE + 0x41, 0x22, 1);
    assert(!s->idle_dirty && s->idle_log_count == 1);
    s->l2[0x41] = 0x22; assert(!dsp_idle_repeat(s));
    dsp_idle_note_write(s, L2_BASE + 0x40, 0x3344, 2);
    assert(s->idle_log_count == 1);
    s->l2[0x41] = 0; assert(dsp_idle_repeat(s));
    dsp_idle_note_write(s, L2_BASE + 0x7e, 0x1122334455667788u, 8);
    assert(s->idle_log_count == 4);

    /* Device writes and an overflowing write log void the proof. */
    dsp_idle_note_write(s, CDJ_C6747_TIMER0_BASE + 0x10, 1, 4);
    assert(s->idle_dirty);
    dsp_idle_anchor(s, 0);
    assert(!s->idle_dirty && !s->idle_log_count);
    for (uint32_t i = 0; i < 64; ++i)
        dsp_idle_note_write(s, L2_BASE + 0x1000 + 4 * i, 1, 4);
    assert(!s->idle_dirty);
    dsp_idle_note_write(s, L2_BASE + 0x2000, 1, 4);
    assert(s->idle_dirty);

    /* Anything clocked by DSP cycles keeps the system from being quiescent. */
    s->timers[1].tgcr = 3; assert(dsp_idle_quiescent(s));
    s->timers[1].tcr = 0x40; assert(!dsp_idle_quiescent(s));
    s->timers[1].tgcr = 0; assert(dsp_idle_quiescent(s));
    s->psc.remaining[1][0] = 2; assert(!dsp_idle_quiescent(s));
    s->psc.remaining[1][0] = 0;
    s->pll.go_remaining = 1; assert(!dsp_idle_quiescent(s));
    s->pll.go_remaining = 0;
    s->spi_transfer.phase = 1; assert(!dsp_idle_quiescent(s));
    s->spi_transfer.phase = 0;
    assert(dsp_idle_quiescent(s));
    free(s->shared_ram); free(s->sdram); free(s);
    return 0;
}
'''
    fixture = tmp_path / 'idle.c'
    fixture.write_text(harness + prefix + span + stubs + idle + checks)
    binary = tmp_path / 'idle-test'
    models = sorted(directory.glob('cdj_c6747_*.c'))
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(directory), str(fixture), *map(str, models),
                    str(directory / 'cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
