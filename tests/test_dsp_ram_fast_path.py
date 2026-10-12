"""The NXS board's dsp_write RAM fast path against the full peripheral chain.

Two boards, one with CDJ_NXS_DSP_RAM_FAST=0 semantics (ram_slow), take the
same random stores - RAM window edges, MMIO registers of every model, odd
sizes, SDRAM with EMIFB on and off, every L1D SRAM partition, idle-proof
logging armed, and a real EDMA transfer staged into RAM - and must agree on
every result and on every byte of memory and model state after each one."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _between(source, start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]


def test_dsp_write_ram_fast_path_matches_full_chain(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    directory = ROOT / 'emulator/qemu'
    source = (directory / 'cdj2000_nxs_hpi.c').read_text()
    prefix = _between(source, '#include "cdj_c674x.h"', 'static NxsHpi *nxs_hpi;')
    span = _between(source, 'static uint8_t *dsp_memory_span(', 'typedef struct {\n    uint8_t *target;')
    edma = _between(source, 'typedef struct {\n    uint8_t *target;',
                    'static bool advance_functional_mcasp_slots(')
    idle = _between(source, '/*\n * Idle-loop skip.', 'static bool dsp_write(void *opaque')
    write = _between(source, 'static bool dsp_write(void *opaque', '\n/* cdj_c674x_fetch fast path')
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>
typedef int MemoryRegion;
typedef int QEMUTimer;
typedef int AudioBackend;
typedef int SWVoiceOut;
typedef int Notifier;
typedef int QemuMutex;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static uint32_t ldl_le_p(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void stw_le_p(uint8_t *p, uint64_t v) { p[0] = v; p[1] = v >> 8; }
static void stl_le_p(uint8_t *p, uint64_t v) { stw_le_p(p, v); stw_le_p(p + 2, v >> 16); }
static void stq_le_p(uint8_t *p, uint64_t v) { stl_le_p(p, v); stl_le_p(p + 4, v >> 32); }
#define g_try_renew(type, p, n) ((type *)realloc((p), sizeof(type) * (n)))
#define g_try_malloc malloc
#define g_free free
static unsigned reports;
static void info_report(const char *format, ...) { (void)format; ++reports; }
'''
    stubs = r'''
static unsigned events;
static void record_event(NxsHpi *s, const char *type, uint64_t offset,
                         uint64_t address, uint64_t value, uint64_t size)
{ (void)s; (void)type; (void)offset; (void)address; (void)value; (void)size; ++events; }
static int functional_slot_probe(NxsHpi *s, CdjC6747Edma *edma,
                                 CdjC6747McaspControl *mcasp)
{ (void)s; (void)edma; (void)mcasp; return 0; }
static bool functional_audio_tick(NxsHpi *s) { (void)s; return true; }
/* Batched ticks (tests/cstub/dsp-ticks.c); nothing ticks here. */
static void dsp_ticks_flush(NxsHpi *s) { (void)s; }
static void dsp_horizon_close(NxsHpi *s) { (void)s; }
/* The core's code-page set (cdj_c674x_may_hold_code): none here. */
bool cdj_c674x_may_hold_code(const void *host, size_t size)
{ (void)host; (void)size; return false; }
static bool log_sample(uint64_t *count) { ++*count; return true; }
'''
    checks = r'''
static NxsHpi *board(bool slow)
{
    NxsHpi *s = calloc(1, sizeof(*s));
    s->shared_ram = calloc(1, SHARED_RAM_SIZE);
    s->sdram = calloc(1, SDRAM_SIZE);
    s->ram_slow = slow;
    cdj_c6747_syscfg_reset(&s->syscfg);
    cdj_c6747_pll_reset(&s->pll);
    cdj_c6747_timers_reset(s->timers);
    cdj_c6747_spis_reset(s->spis);
    cdj_c6747_cache_reset(&s->cache);
    cdj_c6747_edma_reset(&s->edma);
    cdj_c6747_mcasp_reset(&s->mcasp);
    cdj_c6747_mcasp_control_reset(&s->mcasp_control);
    cdj_c6747_intc_reset(&s->intc);
    s->idle_skip = true;
    return s;
}

/* Everything a store can change, compared byte for byte (all of memory
 * every 4096 stores and at the end, the models after every one). */
static unsigned compares;
static void same(const NxsHpi *a, const NxsHpi *b, uint32_t address)
{
    bool memory = ++compares % 4096 == 0 || address == 0;
    if ((memory && (memcmp(a->l2, b->l2, sizeof(a->l2)) ||
                    memcmp(a->l1d, b->l1d, sizeof(a->l1d)) ||
                    memcmp(a->shared_ram, b->shared_ram, SHARED_RAM_SIZE) ||
                    memcmp(a->sdram, b->sdram, SDRAM_SIZE))) ||
        memcmp(&a->hpi, &b->hpi, sizeof(a->hpi)) ||
        memcmp(&a->syscfg, &b->syscfg,
               offsetof(NxsHpi, shared_ram) - offsetof(NxsHpi, syscfg)) ||
        a->idle_dirty != b->idle_dirty || a->idle_log_count != b->idle_log_count ||
        memcmp(a->idle_log_address, b->idle_log_address, sizeof(a->idle_log_address)) ||
        memcmp(a->idle_log_value, b->idle_log_value, sizeof(a->idle_log_value))) {
        fprintf(stderr, "state differs after a store to %#x\n", address);
        abort();
    }
}

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint32_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static unsigned fast_hits, ram_hits, mmio_hits, rejected, edma_transfers;

static bool both(NxsHpi *a, NxsHpi *b, uint32_t address, uint64_t value,
                 unsigned size)
{
    bool ok = dsp_write(a, address, value, size, false);
    assert(ok == dsp_write(b, address, value, size, false));
    same(a, b, address);
    if (ok) {
        bool span = (size == 1 || size == 2 || size == 4 || size == 8) &&
                    dsp_memory_span(a, address, size);
        fast_hits += span;
        mmio_hits += !span;
        assert(dsp_write(a, address, value, size, true));
        assert(dsp_write(b, address, value, size, true));
        same(a, b, address);
    } else {
        ++rejected;
    }
    return ok;
}

static const uint32_t edges[] = {
    0x11800000u, 0x11840000u, 0x00800000u, 0x00840000u, 0x11f00000u,
    0x11f04000u, 0x11f06000u, 0x11f07000u, 0x11f08000u, 0x00f00000u,
    0x00f08000u, 0x80000000u, 0x80020000u, 0xc0000000u, 0xc2000000u,
    0xdffffff8u, 0xe0000000u, 0xb0000000u, 0xb0000008u, 0xb0000020u,
    0x01c00000u, 0x01c01010u, 0x01c04000u, 0x01c04020u, 0x01c08000u,
    0x01d00000u, 0x01d00044u, 0x01d04000u, 0x01d0c000u, 0x01e10030u,
    0x01c11000u, 0x01c11138u, 0x01c20000u, 0x01c21010u, 0x01800000u,
    0x01800020u, 0x01840000u, 0x01844000u, 0x01c14000u, 0x01c14174u,
    0x01c10000u, 0x01e27000u, 0x01e26008u, 0x01e26010u, 0x01c41000u,
    0x01e12038u, 0x01c22000u, 0x01e28000u, 0x00000000u, 0xfffffff8u,
};

int main(void)
{
    NxsHpi *a = board(false), *b = board(true);
    static const unsigned sizes[] = {1, 2, 3, 4, 5, 8, 16};
    static const uint32_t l1dcfg[] = {0, 1, 2, 3, 7};
    for (unsigned round = 0; round < 200000; ++round) {
        if (round % 5000 == 0) {
            /* SDRAM gating and the L1D SRAM partition move under the stores. */
            uint32_t sdcfg = (round / 5000) & 1 ? 0x00004720u | (1u << 16) : 0;
            a->emifb.sdcfg = b->emifb.sdcfg = sdcfg;
            a->cache.l1dcfg = b->cache.l1dcfg = l1dcfg[(round / 10000) % 5];
            a->syscfg.cfgchip[1] = b->syscfg.cfgchip[1] = round & 0x8000 ? 0x8000 : 0;
            /* Arm the idle proof so RAM stores are logged (or void it). */
            a->idle_anchor_valid = b->idle_anchor_valid = (round / 5000) % 3 != 2;
            a->idle_dirty = b->idle_dirty = false;
            a->idle_log_count = b->idle_log_count = 0;
        }
        uint32_t address = edges[next() % (sizeof(edges) / sizeof(edges[0]))];
        static const uint32_t ram[][2] = {
            {0x11800000u, 0x40000u}, {0x00800000u, 0x40000u},
            {0x11f00000u, 0x8000u}, {0x80000000u, 0x20000u},
            {0xc0000000u, 0x20000000u},
        };
        switch (next() % 5) {
        case 4: {
            unsigned w = next() % 5;
            address = ram[w][0] + (next() << 16 | next()) % ram[w][1];
            break;
        }
        case 0: address += next() % 64 - 32; break;
        case 1: address += (next() % 0x40000) & ~3u; break;
        case 2: address = next() << 16 | next(); break;
        default: break;
        }
        unsigned size = sizes[next() % 7];
        uint64_t value = (uint64_t)next() << 32 | next();
        if (size < 8) value &= (UINT64_C(1) << (8 * size)) - 1;
        if (next() % 4 == 0) value &= 0xff;
        bool span = (size == 1 || size == 2 || size == 4 || size == 8) &&
                    dsp_memory_span(a, address, size);
        ram_hits += span;
        both(a, b, address, value, size);
    }

    /* A real EDMA transfer: PaRAM set 1 copies 16 bytes L2 -> SDRAM, staged
     * through the EDMA bus context and committed by edma_mcasp_transaction. */
    a->emifb.sdcfg = b->emifb.sdcfg = 0x00004720u | (1u << 16);
    for (unsigned i = 0; i < 16; ++i) a->l2[0x100 + i] = b->l2[0x100 + i] = 0xa0 + i;
    memset(a->sdram + 0x200, 0, 16); memset(b->sdram + 0x200, 0, 16);
    const uint32_t param[8] = {0, 0x11800100u, (1u << 16) | 16u, 0xc0000200u,
                               0, 0xffffu, 0, 1};
    for (unsigned i = 0; i < 8; ++i)
        assert(both(a, b, 0x01c04020u + 4 * i, param[i], 4));
    assert(both(a, b, 0x01c01010u, 2, 4));             /* ESR: channel 1 */
    same(a, b, 0);
    edma_transfers = !memcmp(a->sdram + 0x200, a->l2 + 0x100, 16);
    assert(edma_transfers);
    printf("fast=%u mmio=%u rejected=%u ram-candidates=%u reports=%u\n",
           fast_hits, mmio_hits, rejected, ram_hits, reports);
    /* Both kinds of store really occurred, on both sides of each window. */
    assert(fast_hits > 20000 && mmio_hits > 50 && rejected > 20000);
    return 0;
}
'''
    fixture = tmp_path / 'ram_fast.c'
    fixture.write_text(harness + prefix + span + edma + stubs + idle + write + checks)
    binary = tmp_path / 'ram-fast-test'
    models = sorted(directory.glob('cdj_c6747_*.c'))
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function',
                    '-I', str(directory), str(fixture), *map(str, models),
                    str(directory / 'cdj_c674x_loop.c'), '-o', str(binary), '-lm'],
                   check=True)
    result = subprocess.run([str(binary)], check=True, timeout=120,
                            capture_output=True, text=True)
    print(result.stdout)
