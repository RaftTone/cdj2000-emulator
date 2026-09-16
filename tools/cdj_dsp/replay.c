/* SPDX-License-Identifier: GPL-2.0-or-later
 * Headless replay of an NXS pre-execution UHPI L2 dump. No boot ROM or
 * external processors run here; this is a deterministic core diagnostic. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c6747_syscfg.h"
#include "cdj_c6747_psc.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_gpio.h"
#include "cdj_c6747_i2c.h"
#include "cdj_c6747_intc.h"
#include "cdj_c6747_timer.h"
#include "cdj_c6747_spi.h"
#include "cdj_c6747_spi_clock.h"
#include "cdj_c6747_cache.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_hpi.h"
#include "cdj_c6747_emifb.h"
#include "cdj_dsp_checkpoint.h"
static uint8_t ram[0x40000];
static uint8_t shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
/* Physical L1D storage. Partition visibility is modeled; cache contents and
 * timing are not. */
static uint8_t l1d[CDJ_DSP_L1D_SIZE];
static uint8_t sdram[0x2000000];
static CdjC6747Syscfg syscfg;
static CdjC6747Psc psc;
static CdjC6747Mcasp mcasp;
static CdjC6747McaspControl mcasp_control;
static CdjC6747Gpio gpio;
static CdjC6747I2c i2c;
static CdjC6747Intc intc;
static CdjC6747IntcDelivery intc_delivery;
static CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
static CdjC6747Spi spis[CDJ_C6747_SPI_COUNT];
static CdjWm8740 wm8740;
static CdjC6747SpiTransfer spi_transfer;
static CdjC6747Cache cache;
static CdjC6747Edma edma;
static CdjC6747SyscfgPriority syscfg_priority;
static CdjC6747Pll pll;
static CdjC6747Hpi hpi;
static CdjC6747Emifb emifb;
static CdjC674x cpu;
static CdjDspCheckpointState checkpoint_state;
typedef struct {
    uint64_t packets, cycles;
    uint32_t address, value;
    unsigned size, boot_phase;
    bool hint, dspint;
} PendingHpicEvent;
static PendingHpicEvent hpic_events[16];
static unsigned hpic_count;
static bool hpic_overflow;
static bool functional_audio;
static FILE *tx_capture;
static uint64_t tx_capture_sequence;
static bool tx_capture_failed;
static bool read_bus(void *unused, uint32_t address, uint32_t *value);

/* Compact dynamic coverage is emitted once at the end of a run. Keeping it
 * here avoids millions of per-step JSON records during connected-event replay
 * while preserving exact packet-start and transition counts. */
#define COVERAGE_PC_SLOTS 131072u
#define COVERAGE_EDGE_SLOTS 262144u
typedef struct {
    uint32_t pc;
    uint64_t direct_fetches, loop_fetches, scheduler_cycles, idle_cycles;
    uint64_t predicate_states;
    CdjC674xPacket packet;
    bool used, has_packet, encoding_changed;
} CoveragePc;
typedef struct {
    uint32_t from, to;
    uint64_t count;
    bool used;
} CoverageEdge;
static CoveragePc coverage_pcs[COVERAGE_PC_SLOTS];
static CoverageEdge coverage_edges[COVERAGE_EDGE_SLOTS];
static unsigned coverage_pc_count, coverage_edge_count;
static unsigned coverage_source_pc_count;
static uint64_t coverage_source_fetches, coverage_scheduler_cycles, coverage_idle_cycles;
static uint64_t coverage_initial_packets, coverage_initial_cycles;
static uint32_t coverage_first_pc, coverage_last_pc;
static bool coverage_started, coverage_overflow;

static uint32_t coverage_mix(uint64_t value)
{
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return value;
}

static bool coverage_loop_fetch(const CdjC674x *state)
{
    if (!state->loop_active || state->loop_wait) return false;
    if (!state->loop.sealed) return true;
    return state->loop.cycle >= state->loop.post_cycle && !state->idle_cycles;
}

static bool coverage_packet_equal(const CdjC674xPacket *left,
                                  const CdjC674xPacket *right)
{
    if (left->count != right->count || left->next_pc != right->next_pc)
        return false;
    for (unsigned i = 0; i < left->count; ++i) {
        const CdjC674xInstruction *a = &left->instructions[i];
        const CdjC674xInstruction *b = &right->instructions[i];
        if (a->pc != b->pc || a->word != b->word ||
            a->compact != b->compact || a->header != b->header)
            return false;
    }
    return true;
}

static bool coverage_capture(const CdjC674x *before, CdjC674xPacket *packet)
{
    bool source_fetch = (!before->loop_active && !before->idle_cycles) ||
        coverage_loop_fetch(before);
    if (!source_fetch) return false;
    /* Fetch reads only pc/fault and writes fault diagnostics on rejection.
     * It does not inspect registers, pipelines, or retained loop state.
     * Keep its diagnostics private without copying the complete CPU. */
    CdjC674x scratch;
    scratch.pc = before->pc;
    scratch.fault = before->fault;
    return cdj_c674x_fetch(&scratch, read_bus, NULL, packet);
}

/* Coverage retains only pre-step classification and predicate values. */
typedef struct {
    uint32_t pc;
    unsigned predicates;
    bool loop_active, idle, loop_fetch, direct_fetch;
} CoverageBefore;

static CoverageBefore coverage_before(const CdjC674x *state)
{
    return (CoverageBefore){
        .pc = state->pc,
        .predicates = (state->r[1][0] != 0) |
            (state->r[1][1] != 0) << 1 | (state->r[1][2] != 0) << 2 |
            (state->r[0][1] != 0) << 3 | (state->r[0][2] != 0) << 4 |
            (state->r[0][0] != 0) << 5,
        .loop_active = state->loop_active,
        .idle = !state->loop_active && state->idle_cycles,
        .loop_fetch = coverage_loop_fetch(state),
        .direct_fetch = !state->loop_active && !state->idle_cycles,
    };
}

static void coverage_record(const CoverageBefore *before,
                            const CdjC674xPacket *packet)
{
    uint32_t pc = before->pc;
    bool loop_fetch = before->loop_fetch;
    bool direct_fetch = before->direct_fetch;
    bool source_fetch = packet != NULL;
    unsigned slot = coverage_mix(pc) & (COVERAGE_PC_SLOTS - 1);
    CoveragePc *matched = NULL;
    for (unsigned probes = 0; probes < COVERAGE_PC_SLOTS; ++probes) {
        CoveragePc *entry = &coverage_pcs[slot];
        if (!entry->used) {
            *entry = (CoveragePc){.pc = pc, .used = true};
            ++coverage_pc_count;
        }
        if (entry->pc == pc) {
            if (direct_fetch) ++entry->direct_fetches;
            if (loop_fetch) ++entry->loop_fetches;
            if (before->loop_active) ++entry->scheduler_cycles;
            if (before->idle) ++entry->idle_cycles;
            matched = entry;
            break;
        }
        slot = (slot + 1) & (COVERAGE_PC_SLOTS - 1);
        if (probes + 1 == COVERAGE_PC_SLOTS) coverage_overflow = true;
    }
    if (before->loop_active) ++coverage_scheduler_cycles;
    if (before->idle) ++coverage_idle_cycles;
    if (!source_fetch) return;
    if (matched) {
        matched->predicate_states |= UINT64_C(1) << before->predicates;
    }
    if (matched && !matched->has_packet) {
        matched->packet = *packet;
        matched->has_packet = true;
        ++coverage_source_pc_count;
    } else if (matched && !coverage_packet_equal(&matched->packet, packet)) {
        matched->encoding_changed = true;
    }
    ++coverage_source_fetches;
    if (coverage_started) {
        uint64_t key = (uint64_t)coverage_last_pc << 32 | pc;
        slot = coverage_mix(key) & (COVERAGE_EDGE_SLOTS - 1);
        for (unsigned probes = 0; probes < COVERAGE_EDGE_SLOTS; ++probes) {
            CoverageEdge *edge = &coverage_edges[slot];
            if (!edge->used) {
                *edge = (CoverageEdge){.from = coverage_last_pc, .to = pc,
                                       .used = true};
                ++coverage_edge_count;
            }
            if (edge->from == coverage_last_pc && edge->to == pc) {
                ++edge->count;
                break;
            }
            slot = (slot + 1) & (COVERAGE_EDGE_SLOTS - 1);
            if (probes + 1 == COVERAGE_EDGE_SLOTS) coverage_overflow = true;
        }
    } else {
        coverage_first_pc = pc;
        coverage_started = true;
    }
    coverage_last_pc = pc;
}

static void coverage_emit(void)
{
    printf("{\"event\":\"coverage_summary\",\"first_pc\":%" PRIu32
           ",\"last_pc\":%" PRIu32 ",\"unique_pcs\":%u,"
           "\"unique_edges\":%u,\"unique_source_pcs\":%u,"
           "\"source_fetches\":%" PRIu64
           ",\"scheduler_cycles\":%" PRIu64 ",\"idle_cycles\":%" PRIu64
           ",\"initial_packets\":%" PRIu64 ",\"final_packets\":%" PRIu64
           ",\"packet_delta\":%" PRIu64
           ",\"initial_cycles\":%" PRIu64 ",\"final_cycles\":%" PRIu64
           ",\"cycle_delta\":%" PRIu64
           ",\"overflow\":%s}\n",
           coverage_first_pc, coverage_last_pc, coverage_pc_count,
           coverage_edge_count, coverage_source_pc_count, coverage_source_fetches,
           coverage_scheduler_cycles, coverage_idle_cycles,
           coverage_initial_packets, cpu.packets,
           cpu.packets - coverage_initial_packets,
           coverage_initial_cycles, cpu.cycles,
           cpu.cycles - coverage_initial_cycles,
           coverage_overflow ? "true" : "false");
    for (unsigned i = 0; i < COVERAGE_PC_SLOTS; ++i) {
        CoveragePc *entry = &coverage_pcs[i];
        if (!entry->used) continue;
        printf("{\"event\":\"coverage_pc\",\"pc\":%" PRIu32
               ",\"direct_fetches\":%" PRIu64
               ",\"loop_fetches\":%" PRIu64
               ",\"scheduler_cycles\":%" PRIu64
               ",\"idle_cycles\":%" PRIu64
               ",\"source_predicate_states\":%" PRIu64
               ",\"encoding_changed\":%s}\n",
               entry->pc, entry->direct_fetches, entry->loop_fetches,
               entry->scheduler_cycles, entry->idle_cycles, entry->predicate_states,
               entry->encoding_changed ? "true" : "false");
        if (!entry->has_packet) continue;
        for (unsigned j = 0; j < entry->packet.count; ++j) {
            const CdjC674xInstruction *insn = &entry->packet.instructions[j];
            printf("{\"event\":\"coverage_instruction\",\"source_pc\":%" PRIu32
                   ",\"packet_next_pc\":%" PRIu32 ",\"index\":%u,"
                   "\"pc\":%" PRIu32 ",\"word\":%" PRIu32
                   ",\"header\":%" PRIu32 ",\"compact\":%s,"
                   "\"parallel\":%s}\n",
                   entry->pc, entry->packet.next_pc, j, insn->pc, insn->word,
                   insn->header, insn->compact ? "true" : "false",
                   j + 1 < entry->packet.count ? "true" : "false");
        }
    }
    for (unsigned i = 0; i < COVERAGE_EDGE_SLOTS; ++i) {
        CoverageEdge *edge = &coverage_edges[i];
        if (!edge->used) continue;
        printf("{\"event\":\"coverage_edge\",\"from\":%" PRIu32
               ",\"to\":%" PRIu32 ",\"count\":%" PRIu64 "}\n",
               edge->from, edge->to, edge->count);
    }
}

static void restore_devices(const CdjDspCheckpointState *state)
{
    cpu = state->cpu;
    syscfg = state->syscfg;
    psc = state->psc;
    mcasp = state->mcasp;
    mcasp_control = state->mcasp_control;
    gpio = state->gpio;
    i2c = state->i2c;
    pll = state->pll;
    hpi = state->hpi;
    emifb = state->emifb;
    intc = state->intc;
    intc_delivery = state->intc_delivery;
    memcpy(timers, state->timers, sizeof(timers));
    memcpy(spis, state->spis, sizeof(spis));
    wm8740 = state->wm8740;
    spi_transfer = state->spi_transfer;
    cache = state->cache;
    edma = state->edma;
    syscfg_priority = state->syscfg_priority;
}

static void capture_devices(CdjDspCheckpointState *state, const char *reason)
{
    state->cpu = cpu;
    state->syscfg = syscfg;
    state->psc = psc;
    state->mcasp = mcasp;
    state->mcasp_control = mcasp_control;
    state->gpio = gpio;
    state->i2c = i2c;
    state->pll = pll;
    state->hpi = hpi;
    state->emifb = emifb;
    state->intc = intc;
    state->intc_delivery = intc_delivery;
    memcpy(state->timers, timers, sizeof(state->timers));
    memcpy(state->spis, spis, sizeof(state->spis));
    state->wm8740 = wm8740;
    state->spi_transfer = spi_transfer;
    state->cache = cache;
    state->edma = edma;
    state->syscfg_priority = syscfg_priority;
    state->dsp_started = true;
    state->dsp_halted = cpu.fault != NULL;
    ++state->checkpoint_sequence;
    cdj_dsp_checkpoint_prepare(state, reason);
}
static void cycle_tick(void *unused)
{
    (void)unused;
    uint64_t transfers = wm8740.transfers;
    if (!cdj_c674x_loop_functional_timing())
        cdj_spi_core_tick(spis, &wm8740, &spi_transfer, &pll);
    if (wm8740.transfers != transfers)
        printf("{\"event\":\"wm8740_latch\",\"word\":%u,\"transfers\":%" PRIu64 "}\n",
               wm8740.last_word, wm8740.transfers);
    cdj_c6747_pll_tick(&pll);
    /* SPRUH91D chapter 28 counts on the timer input clock; this callback is
     * the only per-cycle hook the core offers, so one call is one input clock
     * period - an approximation of nothing measurable, declared in the replay
     * manifest beside the other approximations.  The events themselves are
     * Table 2-1's, mapped by cdj_c6747_timer_event(). */
    uint32_t timer_outputs = cdj_c6747_timers_tick(timers);
    for (unsigned bit = 0; timer_outputs >> bit; ++bit)
        if (timer_outputs & (1u << bit))
            cdj_c6747_intc_deliver_event(&intc, &intc_delivery,
                                         cdj_c6747_timer_event(bit));
}
static uint32_t global(uint32_t a)
{ return a >= 0x00800000 && a < 0x00840000 ? a + 0x11000000 : a; }
static uint8_t *host_memory(uint32_t a)
{
    uint32_t l1_offset;
    if (cdj_c6747_l1d_sram_span(&cache, a, 4, &l1_offset))
        return l1d + l1_offset;
    if (a >= 0x11800000 && a <= 0x1183fffc) return ram + a - 0x11800000;
    if (a >= 0x80000000 && a <= 0x8001fffc) return shared_ram + a - 0x80000000;
    uint32_t offset;
    if (cdj_c6747_emifb_sdram_offset(&emifb, a, 4, sizeof(sdram), &offset))
        return sdram + offset;
    return NULL;
}
static bool read_bus(void *unused, uint32_t a, uint32_t *v)
{
    (void)unused;
    if (cdj_c6747_syscfg_read(&syscfg, a, v)) return true;
    if (cdj_c6747_syscfg_priority_read(&syscfg_priority, a, v)) return true;
    if (cdj_c6747_psc_read(&psc, a, v)) return true;
    if (cdj_c6747_mcasp_read(&mcasp, a, v)) return true;
    if (cdj_c6747_mcasp_control_read(&mcasp_control, a, v)) return true;
    if (cdj_c6747_gpio_read(&gpio, a, v)) return true;
    if (cdj_c6747_i2c_read(&i2c, a, v)) return true;
    if (cdj_c6747_intc_read(&intc, a, v)) return true;
    if (cdj_c6747_timers_read(timers, a, v)) return true;
    if (!cdj_c674x_loop_functional_timing() &&
        cdj_c6747_spi_wm8740_timed_mapped(a))
        return cdj_c6747_spi_wm8740_read_timed(spis, &spi_transfer, a, v);
    if (cdj_c6747_spis_read(spis, a, v)) return true;
    if (cdj_c6747_cache_read(&cache, a, v)) return true;
    if (cdj_c6747_edma_read(&edma, a, v)) return true;
    if (cdj_c6747_pll_read(&pll, a, v)) return true;
    if (cdj_c6747_emifb_read(&emifb, a, v)) return true;
    if ((syscfg.cfgchip[1] & 0x8000) && cdj_c6747_hpi_cpu_read(&hpi, a, v)) return true;
    if (!(a & 3) && a >= 0x80000000 && a <= 0x8001fffc) {
        unsigned offset = a - 0x80000000;
        *v = shared_ram[offset] | (uint32_t)shared_ram[offset + 1] << 8 |
             (uint32_t)shared_ram[offset + 2] << 16 |
             (uint32_t)shared_ram[offset + 3] << 24;
        return true;
    }
    uint32_t offset;
    if (!(a & 3) &&
        cdj_c6747_emifb_sdram_offset(&emifb, a, 4, sizeof(sdram), &offset)) {
        *v = sdram[offset] | (uint32_t)sdram[offset + 1] << 8 |
             (uint32_t)sdram[offset + 2] << 16 | (uint32_t)sdram[offset + 3] << 24;
        return true;
    }
    a = global(a);
    uint32_t l1_offset;
    if (!(a & 3) && cdj_c6747_l1d_sram_span(
            &cache, a, 4, &l1_offset)) {
        *v = l1d[l1_offset] | (uint32_t)l1d[l1_offset+1] << 8 |
             (uint32_t)l1d[l1_offset+2] << 16 |
             (uint32_t)l1d[l1_offset+3] << 24;
        return true;
    }
    if ((a & 3) || a < 0x11800000 || a > 0x1183fffc) return false;
    a -= 0x11800000;
    *v = ram[a] | (uint32_t)ram[a+1] << 8 | (uint32_t)ram[a+2] << 16 | (uint32_t)ram[a+3] << 24;
    return true;
}

static uint8_t *memory_span(uint32_t address, size_t size)
{
    uint64_t end;
    address = global(address);
    end = (uint64_t)address + size;
    if (!size || end > UINT64_C(0x100000000)) return NULL;
    uint32_t l1_offset;
    if (cdj_c6747_l1d_sram_span(&cache, address, size, &l1_offset))
        return l1d + l1_offset;
    if (address >= 0x11800000u && end <= UINT64_C(0x11840000))
        return ram + address - 0x11800000u;
    if (address >= 0x80000000u && end <= UINT64_C(0x80020000))
        return shared_ram + address - 0x80000000u;
    uint32_t offset;
    if (cdj_c6747_emifb_sdram_offset(&emifb, address, size, sizeof(sdram),
                                    &offset))
        return sdram + offset;
    return NULL;
}

/* Optional bounded observer. RAM-only reads: no MMIO side effects, CPU
 * changes, scheduling changes or inferred buffer ownership. */
static bool observe_pcm;
static unsigned pcm_observations;
static unsigned connected_stop_limit;
static uint32_t pcm_return_pc, pcm_output_base;
static void pcm_span(const char *name, uint32_t address, size_t size)
{
    const uint8_t *p = memory_span(address, size);
    printf(",\"%s\":{\"address\":%" PRIu32 ",\"hex\":", name, address);
    if (!p) printf("null");
    else {
        putchar('"');
        for (size_t i = 0; i < size; ++i) printf("%02x", p[i]);
        putchar('"');
    }
    putchar('}');
}
static void pcm_observe(void)
{
    if (!observe_pcm || pcm_observations >= 64 || cpu.idle_cycles) return;
    bool returning = pcm_return_pc && cpu.pc == pcm_return_pc;
    switch (cpu.pc) {
    case 0xc003c698: case 0xc003c488: case 0xc003c40c:
    case 0xc003c398: case 0xc003c2b4: case 0xc003c210:
    case 0xc003c16c: case 0xc003c034: case 0xc003bf00: break;
    default: if (!returning) return;
    }
    if (cpu.pc == 0xc003c698) {
        pcm_return_pc = cpu.r[1][3];
        pcm_output_base = cpu.r[0][4];
    }
    printf("{\"event\":\"pcm_observation\",\"index\":%u,\"pc\":%" PRIu32
           ",\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64
           ",\"a4\":%" PRIu32 ",\"b4\":%" PRIu32 ",\"a6\":%" PRIu32
           ",\"b6\":%" PRIu32 ",\"b14\":%" PRIu32
           ",\"b3\":%" PRIu32 ",\"return_observation\":%s",
           ++pcm_observations, cpu.pc, cpu.packets, cpu.cycles,
           cpu.r[0][4], cpu.r[1][4], cpu.r[0][6], cpu.r[1][6], cpu.r[1][14],
           cpu.r[1][3], returning ? "true" : "false");
    if (returning) {
        pcm_span("output_plane0", pcm_output_base, 0x930);
        pcm_span("output_plane1", pcm_output_base + 0xdc8, 0x930);
        pcm_return_pc = 0;
    }
    pcm_span("state", 0x1182dda0, 0x8c);
    pcm_span("a4_span", cpu.r[0][4], 0x100);
    pcm_span("b4_span", cpu.r[1][4], 0x100);
    pcm_span("b14_span", cpu.r[1][14], 0x80);
    const uint8_t *p = memory_span(0x1182ddb8, 4);
    if (p) {
        uint32_t base = p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
        pcm_span("raw_banks", base, 0x7980);
    }
    printf("}\n");
}

typedef struct {
    uint8_t *target;
    uint8_t *bytes;
    size_t size;
} EdmaStagedWrite;

typedef struct {
    CdjC6747McaspControl *mcasp;
    EdmaStagedWrite *writes;
    size_t write_count, write_capacity;
} EdmaBusContext;

static bool edma_stage_write(EdmaBusContext *context, uint8_t *target,
                             const uint8_t *bytes, size_t size)
{
    if (context->write_count == context->write_capacity) {
        size_t capacity = context->write_capacity ?
                          context->write_capacity * 2u : 8u;
        if (capacity < context->write_capacity) return false;
        EdmaStagedWrite *writes = realloc(
            context->writes, capacity * sizeof(*writes));
        if (!writes) return false;
        context->writes = writes;
        context->write_capacity = capacity;
    }
    uint8_t *copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, bytes, size);
    context->writes[context->write_count++] =
        (EdmaStagedWrite){target, copy, size};
    return true;
}

static void edma_free_staged_writes(EdmaBusContext *context)
{
    for (size_t i = 0; i < context->write_count; ++i)
        free(context->writes[i].bytes);
    free(context->writes);
}

static bool edma_read_bytes(void *unused, uint32_t address, uint8_t *bytes,
                            size_t size)
{
    (void)unused;
    uint8_t *source = memory_span(address, size);
    if (!source) return false;
    memcpy(bytes, source, size);
    EdmaBusContext *context = unused;
    uintptr_t read_start = (uintptr_t)source;
    uintptr_t read_end = read_start + size;
    for (size_t i = 0; i < context->write_count; ++i) {
        EdmaStagedWrite *write = &context->writes[i];
        uintptr_t write_start = (uintptr_t)write->target;
        uintptr_t write_end = write_start + write->size;
        if (write_start < read_end && read_start < write_end) {
            uintptr_t start = write_start > read_start ? write_start : read_start;
            uintptr_t end = write_end < read_end ? write_end : read_end;
            memcpy(bytes + start - read_start,
                   write->bytes + start - write_start, end - start);
        }
    }
    return true;
}

static bool edma_write_bytes(void *unused, uint32_t address,
                             const uint8_t *bytes, size_t size, bool commit)
{
    EdmaBusContext *context = unused;
    uint8_t *target = memory_span(address, size);
    if (target) {
        return !commit || edma_stage_write(context, target, bytes, size);
    }
    if (size == 4) {
        uint32_t value = bytes[0] | (uint32_t)bytes[1] << 8 |
                         (uint32_t)bytes[2] << 16 |
                         (uint32_t)bytes[3] << 24;
        return cdj_c6747_mcasp_control_write(context->mcasp, address,
                                             value, 4, commit);
    }
    return false;
}

static bool service_mcasp_axevt(CdjC6747Edma *edma_state,
                                CdjC6747McaspControl *mcasp_state,
                                EdmaBusContext *context)
{
    const CdjC6747EdmaBus bus = {edma_read_bytes, edma_write_bytes, context};
    for (unsigned instance = 1; instance <= 2; ++instance) {
        unsigned channel = instance == 1 ? 3 : 5;
        for (unsigned serializer = 0;
             serializer < 16 &&
             cdj_c6747_mcasp_axevt_ready(mcasp_state, instance);
             ++serializer) {
            uint64_t before = mcasp_state->xbuf_writes[instance];
            if (!cdj_c6747_edma_event(edma_state, channel, &bus)) return false;
            if (mcasp_state->xbuf_writes[instance] == before) break;
        }
    }
    return true;
}

static void deliver_edma_notifications(void)
{
    if (cdj_c6747_edma_take_irq_notification(&edma, 1))
        cdj_c6747_intc_deliver_event(&intc, &intc_delivery, 8);
}

static bool edma_mcasp_transaction(bool edma_access, uint32_t address,
                                   uint64_t value, unsigned size, bool commit)
{
    if (!(edma_access ? cdj_c6747_edma_write_mapped(address, size) :
                       cdj_c6747_mcasp_control_write_mapped(address, size)))
        return false;
    CdjC6747Edma trial_edma = edma;
    CdjC6747McaspControl trial_mcasp = mcasp_control;
    EdmaBusContext trial_context = {.mcasp = &trial_mcasp};
    const CdjC6747EdmaBus trial_bus = {
        edma_read_bytes, edma_write_bytes, &trial_context,
    };
    bool ok = edma_access ?
        cdj_c6747_edma_write(&trial_edma, address, value, size, true,
                             &trial_bus) :
        cdj_c6747_mcasp_control_write(&trial_mcasp, address, value, size, true);
    if (ok) ok = service_mcasp_axevt(&trial_edma, &trial_mcasp,
                                     &trial_context);
    if (ok && commit) {
        for (size_t i = 0; i < trial_context.write_count; ++i) {
            EdmaStagedWrite *write = &trial_context.writes[i];
            memcpy(write->target, write->bytes, write->size);
        }
        edma = trial_edma;
        mcasp_control = trial_mcasp;
        deliver_edma_notifications();
    }
    edma_free_staged_writes(&trial_context);
    return ok;
}

static bool advance_functional_mcasp_slots(void)
{
    CdjC6747McaspControl original_mcasp = mcasp_control;
    CdjC6747Edma trial_edma = edma;
    CdjC6747McaspControl trial_mcasp = mcasp_control;
    EdmaBusContext trial_context = {.mcasp = &trial_mcasp};
    bool advanced = false, ok = true;

    for (unsigned instance = 1; instance <= 2; ++instance) {
        if ((trial_mcasp.gblctl[instance] & 0x1f00u) != 0x1f00u)
            continue;
        bool axevt;
        if (!cdj_c6747_mcasp_tx_slot(&trial_mcasp, instance, &axevt)) {
            ok = false;
            break;
        }
        advanced = true;
    }
    if (ok && advanced)
        ok = service_mcasp_axevt(&trial_edma, &trial_mcasp, &trial_context);
    if (ok && advanced) {
        for (size_t i = 0; i < trial_context.write_count; ++i) {
            EdmaStagedWrite *write = &trial_context.writes[i];
            memcpy(write->target, write->bytes, write->size);
        }
        edma = trial_edma;
        mcasp_control = trial_mcasp;
        deliver_edma_notifications();
        if (tx_capture) {
            for (unsigned instance = 1; instance <= 2; ++instance) {
                for (unsigned serializer = 0; serializer < 16; ++serializer) {
                    uint64_t sequence = trial_mcasp.xrsr_source_sequence[instance][serializer];
                    if (!sequence || sequence ==
                        original_mcasp.xrsr_source_sequence[instance][serializer])
                        continue;
                    if (fprintf(tx_capture,
                            "{\"sequence\":%" PRIu64 ",\"instance\":%u,"
                            "\"slot\":%u,\"serializer\":%u,\"word\":%u,"
                            "\"xbuf_sequence\":%" PRIu64 ",\"packets\":%" PRIu64 ","
                            "\"cycles\":%" PRIu64 ",\"source\":\"genuine_xbuf\","
                            "\"clock\":\"functional-coarse-packet-slot\"}\n",
                            ++tx_capture_sequence, instance,
                            trial_mcasp.xslot[instance], serializer,
                            trial_mcasp.xrsr[instance][serializer], sequence,
                            cpu.packets, cpu.cycles) < 0 || fflush(tx_capture)) {
                        tx_capture_failed = true;
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;
            }
        }
    }
    edma_free_staged_writes(&trial_context);
    return ok;
}

static bool functional_audio_tick(void)
{
    if (!functional_audio ||
        cpu.packets % CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL)
        return true;
    if (tx_capture_failed) {
        cpu.fault = "DSP transmit capture write failed";
        cpu.fault_pc = cpu.pc;
        cpu.fault_word = 0;
        return false;
    }
    if (advance_functional_mcasp_slots()) return true;
    cpu.fault = "unsupported functional McASP transmit slot";
    cpu.fault_pc = cpu.pc;
    cpu.fault_word = 0;
    return false;
}

static bool write_bus(void *unused, uint32_t a, uint64_t v, unsigned size, bool commit)
{
    (void)unused;
    bool ok = false;
    if (!cdj_c674x_loop_functional_timing()) {
        if (cdj_c6747_spi_wm8740_timed_mapped(a)) {
            if ((a == CDJ_C6747_SPI1_BASE + 0x38 ||
                 a == CDJ_C6747_SPI1_BASE + 0x3c) &&
                (spis[1].gcr1 & (1u << 24)) && !cdj_spi_clock_ready(&pll))
                goto record;
            ok = cdj_c6747_spi_wm8740_write_timed(spis, &wm8740,
                       &spi_transfer, a, v, size, commit);
            goto record;
        }
        if ((spi_transfer.phase || spi_transfer.queued_valid) &&
            !cdj_c6747_syscfg_pll_locked(&syscfg) &&
            cdj_c6747_pll_write_mapped(a, size)) goto record;
    }
    ok = cdj_c6747_syscfg_write(&syscfg, a, v, size, commit);
    if (!ok) ok = cdj_c6747_syscfg_priority_write(
        &syscfg_priority, &syscfg, a, v, size, commit);
    if (!ok) ok = cdj_c6747_psc_write(&psc, a, v, size, commit);
    if (!ok) ok = cdj_c6747_mcasp_write(&mcasp, a, v, size, commit);
    if (!ok) ok = edma_mcasp_transaction(false, a, v, size, commit);
    if (!ok) ok = cdj_c6747_gpio_write(&gpio, a, v, size, commit);
    if (!ok) ok = cdj_c6747_i2c_write(&i2c, a, v, size, commit);
    if (!ok) ok = cdj_c6747_intc_write_delivery(
        &intc, &intc_delivery, a, v, size, commit);
    if (!ok) ok = cdj_c6747_timers_write(timers, a, v, size, commit);
    if (!ok) ok = cdj_c6747_spis_write_wm8740(
        spis, &wm8740, a, v, size,
        cdj_c674x_loop_functional_timing(), commit);
    if (!ok) ok = cdj_c6747_spis_write(spis, a, v, size, commit);
    if (!ok) ok = cdj_c6747_cache_write(&cache, a, v, size, commit);
    if (!ok) ok = edma_mcasp_transaction(true, a, v, size, commit);
    if (!ok && cdj_c6747_syscfg_pll_locked(&syscfg) &&
        cdj_c6747_pll_write_mapped(a, size)) ok = true;
    if (!ok) ok = cdj_c6747_pll_write(&pll, a, v, size, commit);
    if (!ok) ok = cdj_c6747_emifb_write(&emifb, a, v, size, commit);
    if (!ok && (syscfg.cfgchip[1] & 0x8000)) {
        ok = cdj_c6747_hpi_cpu_write(&hpi, a, v, size, commit);
        if (ok && commit) {
            if (hpic_count == sizeof(hpic_events) / sizeof(hpic_events[0])) {
                hpic_overflow = true;
            } else {
                hpic_events[hpic_count++] = (PendingHpicEvent){
                    .packets = cpu.packets, .cycles = cpu.cycles,
                    .address = a, .value = v, .size = size,
                    .boot_phase = checkpoint_state.boot_phase,
                    .hint = hpi.hint, .dspint = hpi.dspint
                };
            }
        }
    }
    if (!ok && (size == 1 || size == 2 || size == 4 || size == 8) &&
        a >= 0x80000000 && a <= 0x80020000 - size) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            shared_ram[a - 0x80000000 + i] = v >> (8 * i);
    }
    uint32_t offset;
    if (!ok && (size == 1 || size == 2 || size == 4 || size == 8) &&
        cdj_c6747_emifb_sdram_offset(&emifb, a, size, sizeof(sdram), &offset)) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            sdram[offset + i] = v >> (8 * i);
    }
    uint32_t physical = global(a);
    uint32_t l1_offset;
    if (!ok && (size == 1 || size == 2 || size == 4 || size == 8) &&
        cdj_c6747_l1d_sram_span(&cache, physical, size, &l1_offset)) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            l1d[l1_offset + i] = v >> (8*i);
    }
    if (!ok && (size == 1 || size == 2 || size == 4 || size == 8) &&
        physical >= 0x11800000 && physical <= 0x11840000 - size) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            ram[physical - 0x11800000 + i] = v >> (8*i);
    }
record:
    if (commit || !ok)
        printf("{\"event\":\"%s\",\"address\":%" PRIu32 ",\"value\":%" PRIu64 ",\"size\":%u}\n",
               ok ? "write" : "rejected_write", a, v, size);
    return ok;
}

typedef struct {
    uint64_t sequence, offset, address, value, packets, cycles;
    unsigned size, boot_phase;
    bool hint, dspint;
    char type[48];
} RecordedEvent;

static bool parse_event(const char *line, RecordedEvent *event)
{
    char hint[6], dspint[6];
    int consumed = -1;
    int fields = sscanf(line,
        "{\"sequence\":%" SCNu64 ",\"event\":\"%47[^\"]\","
        "\"offset\":%" SCNu64 ",\"address\":%" SCNu64 ","
        "\"value\":%" SCNu64 ",\"size\":%u,\"boot_phase\":%u,"
        "\"hint\":%5[^,],\"dspint\":%5[^,],\"packets\":%" SCNu64 ","
        "\"cycles\":%" SCNu64 "}%n",
        &event->sequence, event->type, &event->offset, &event->address,
        &event->value, &event->size, &event->boot_phase, hint, dspint,
        &event->packets, &event->cycles, &consumed);
    if (fields != 11 || consumed < 0 || line[consumed] != '\n' ||
        line[consumed + 1] != '\0' ||
        (strcmp(hint, "true") && strcmp(hint, "false")) ||
        (strcmp(dspint, "true") && strcmp(dspint, "false"))) return false;
    event->hint = !strcmp(hint, "true");
    event->dspint = !strcmp(dspint, "true");
    return true;
}

typedef struct {
    uint64_t steps_remaining;
    uint64_t initial_packets, initial_cycles;
    uint64_t packet_limit, cycle_limit;
} ReplayLimits;

static uint64_t counter_delta(uint64_t value, uint64_t initial)
{
    return value >= initial ? value - initial : 0;
}

static const char *limit_reached(const ReplayLimits *limits)
{
    if (!limits->steps_remaining) return "step_limit";
    if (limits->packet_limit &&
        counter_delta(cpu.packets, limits->initial_packets) >= limits->packet_limit)
        return "packet_limit";
    if (limits->cycle_limit &&
        counter_delta(cpu.cycles, limits->initial_cycles) >= limits->cycle_limit)
        return "cycle_limit";
    return NULL;
}

static const char *run_quota(ReplayLimits *limits, uint32_t breakpoint,
                             unsigned quota)
{
    const char *reason = "phase budget exhausted";
    for (unsigned step = 0; step < quota; ++step) {
        const char *limited = limit_reached(limits);
        if (limited) return limited;
        if (breakpoint && cpu.pc == breakpoint) return "breakpoint";
        deliver_edma_notifications();
        if (!cdj_c674x_interrupt(
                &cpu, cdj_c6747_intc_cpu_pending(&intc_delivery)))
            return cpu.fault ? cpu.fault : "CPU interrupt stopped";
        pcm_observe();
        CoverageBefore before = coverage_before(&cpu);
        CdjC674xPacket coverage_packet;
        bool has_coverage_packet = !before.direct_fetch &&
            coverage_capture(&cpu, &coverage_packet);
        if (!cdj_c674x_step_capture_direct(&cpu, read_bus, write_bus, NULL,
                before.direct_fetch ? &coverage_packet : NULL))
            return cpu.fault ? cpu.fault : "CPU stopped";
        coverage_record(&before, (before.direct_fetch || has_coverage_packet) ?
                        &coverage_packet : NULL);
        --limits->steps_remaining;
        if (spi_transfer.fault) {
            cpu.fault = "unsupported SPI transfer clock or state";
            cpu.fault_pc = cpu.pc;
            return cpu.fault;
        }
        cdj_c6747_psc_tick(&psc);
        if (!functional_audio_tick())
            return cpu.fault;
        if (hpi.hint) return "HINT host-event yield";
    }
    return reason;
}

static const char *run_budget(ReplayLimits *limits, uint32_t breakpoint)
{
    return run_quota(limits, breakpoint, CDJ_DSP_COOPERATIVE_BUDGET);
}

typedef enum {
    EVENT_REPLAY_ERROR,
    EVENT_REPLAY_COMPLETE,
    EVENT_REPLAY_BUDGET_EXHAUSTED,
} EventReplayResult;

static EventReplayResult event_budget_exhausted(
    const RecordedEvent *event, const ReplayLimits *limits)
{
    const char *limited = limit_reached(limits);
    printf("{\"event\":\"event_budget_exhausted\",\"reason\":\"%s\","
           "\"next_sequence\":%" PRIu64 ",\"next_type\":\"%s\","
           "\"pc\":%" PRIu32 ",\"packets\":%" PRIu64
           ",\"cycles\":%" PRIu64 "}\n",
           limited ? limited : "unknown_limit", event->sequence, event->type,
           cpu.pc, cpu.packets, cpu.cycles);
    return EVENT_REPLAY_BUDGET_EXHAUSTED;
}

static EventReplayResult replay_external_events(
    const char *path, ReplayLimits *limits,
    uint32_t breakpoint, const char **reason)
{
    FILE *file = fopen(path, "r");
    if (!file) { perror("event transcript"); return EVENT_REPLAY_ERROR; }
    char line[768];
    uint64_t expected = 0;
    bool stop_pending = false, fault_matched = false;
    bool schedule_pending = false, slice_end_pending = false;
    uint32_t slice_executed = 0;
    CdjDspScheduler *scheduler = &checkpoint_state.scheduler;
    bool deferred = scheduler->mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1;
    bool initial_schedule = deferred && checkpoint_state.dsp_started &&
        !checkpoint_state.dsp_halted &&
        (!strcmp(checkpoint_state.stop_reason, "DSP start boundary") ||
         !strcmp(checkpoint_state.stop_reason, "boot-phase boundary"));
    unsigned verified_stops = 0;
    *reason = "event_eof";
    /* QEMU captures these boundaries after the triggering event but BEFORE
     * run_dsp requests an activation (or coalesces a pending activation). */
    if (initial_schedule) {
        if (!cdj_dsp_scheduler_request(scheduler)) {
            fclose(file);
            return EVENT_REPLAY_ERROR;
        }
        schedule_pending = true;
    }
    if (!deferred && (!strcmp(checkpoint_state.stop_reason, "DSP start boundary") ||
        !strcmp(checkpoint_state.stop_reason, "boot-phase boundary"))) {
        *reason = run_budget(limits, breakpoint);
        stop_pending = true;
    }
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            fputs("event transcript line too long\n", stderr);
            fclose(file);
            return EVENT_REPLAY_ERROR;
        }
        RecordedEvent event;
        if (!parse_event(line, &event) || event.sequence != ++expected) {
            fputs("invalid or non-contiguous event transcript\n", stderr);
            fclose(file);
            return EVENT_REPLAY_ERROR;
        }
        if (event.sequence <= checkpoint_state.event_sequence) continue;
        /* Limits are host-requested diagnostic boundaries, not connected
         * firmware events.  Do not apply the next host event after reaching
         * one.  An immediately following DSP stop may still prove that the
         * limit happened to coincide with a genuine recorded boundary. */
        if (limit_reached(limits) && !cpu.fault &&
            ((strcmp(event.type, "dsp_stop") &&
              !(deferred && (!strcmp(event.type, "dsp_slice_end") ||
                             !strcmp(event.type, "dsp_hpic_write")))) || !stop_pending)) {
            EventReplayResult result = event_budget_exhausted(&event, limits);
            fclose(file);
            return result;
        }
        bool trigger = false;
        uint32_t begin_quota = 0;
        if (schedule_pending && strcmp(event.type, "dsp_schedule")) goto mismatch;
        if (deferred && stop_pending && strcmp(event.type, "dsp_hpic_write") &&
            strcmp(event.type, slice_end_pending ? "dsp_slice_end" : "dsp_stop"))
            goto mismatch;
        if (!strcmp(event.type, "dsp_schedule")) {
            if (!deferred || !schedule_pending ||
                event.offset != scheduler->activation_id || event.address != scheduler->slice_id ||
                event.value != scheduler->remaining ||
                event.size != (unsigned)(scheduler->pending | scheduler->rearm << 1) ||
                event.packets != cpu.packets || event.cycles != cpu.cycles) goto mismatch;
            schedule_pending = false;
        } else if (!strcmp(event.type, "dsp_slice_begin")) {
            if (!deferred || !checkpoint_state.dsp_started || checkpoint_state.dsp_halted ||
                stop_pending || cpu.fault || hpic_count || hpic_overflow)
                goto mismatch;
            begin_quota = cdj_dsp_scheduler_begin(scheduler);
            if (!begin_quota || event.offset != scheduler->activation_id ||
                event.address != scheduler->slice_id || event.value != scheduler->remaining ||
                event.size != begin_quota || event.packets != cpu.packets ||
                event.cycles != cpu.cycles) goto mismatch;
        } else if (!strcmp(event.type, "dsp_slice_end")) {
            if (!deferred || !slice_end_pending || hpic_count || hpic_overflow ||
                !cdj_dsp_scheduler_end(scheduler, slice_executed, hpi.hint, !!cpu.fault) ||
                event.offset != scheduler->activation_id || event.address != scheduler->slice_id ||
                event.value != scheduler->remaining || event.size != slice_executed ||
                event.packets != cpu.packets || event.cycles != cpu.cycles) goto mismatch;
            slice_end_pending = false;
        } else if (!strcmp(event.type, "boot_phase")) {
            if (event.offset || event.address || event.size || event.value > 7)
                goto mismatch;
            trigger = event.value != checkpoint_state.boot_phase;
            checkpoint_state.boot_phase = event.value;
            cdj_c6747_gpio_set_input(&gpio, 4, 5, event.value & 1);
            cdj_c6747_gpio_set_input(&gpio, 4, 2, event.value & 2);
            cdj_c6747_gpio_set_input(&gpio, 4, 3, event.value & 4);
        } else if (!strcmp(event.type, "hpi_host_control_write")) {
            if (event.offset || event.size != 4 || event.address != checkpoint_state.hpi_address ||
                event.value > UINT32_MAX) goto mismatch;
            bool old_hint = hpi.hint;
            bool old_dspint = hpi.dspint;
            cdj_c6747_hpi_host_write(&hpi, event.value);
            bool dspint_rising = !old_dspint && hpi.dspint;
            if (dspint_rising)
                cdj_c6747_intc_deliver_event(&intc, &intc_delivery, 34);
            trigger = (old_hint && !hpi.hint) || dspint_rising;
        } else if (!strcmp(event.type, "hpi_host_address_write")) {
            if (event.offset != 0x40000 || event.size != 4 ||
                event.address != checkpoint_state.hpi_address || event.value > UINT32_MAX)
                goto mismatch;
            checkpoint_state.hpi_address = event.value;
        } else if (!strcmp(event.type, "hpi_host_data_autoincrement_write") ||
                   !strcmp(event.type, "hpi_host_data_fixed_write")) {
            uint32_t address = checkpoint_state.hpi_address;
            bool autoincrement = !strcmp(event.type, "hpi_host_data_autoincrement_write");
            uint64_t expected_offset = autoincrement ? 0x80000 : 0xc0000;
            uint8_t *target = host_memory(address);
            if (event.offset != expected_offset || event.address != address ||
                event.size != 4 || event.value > UINT32_MAX || address & 3 ||
                !target) goto mismatch;
            for (unsigned i = 0; i < 4; ++i)
                target[i] = event.value >> (8 * i);
            ++checkpoint_state.words;
            if (autoincrement)
                checkpoint_state.hpi_address += 4;
        } else if (!strcmp(event.type, "hpi_host_data_read")) {
            uint32_t address = checkpoint_state.hpi_address;
            uint8_t *source = host_memory(address);
            if ((event.offset != 0x80000 && event.offset != 0xc0000) ||
                event.address != address || event.size != 4 ||
                event.value > UINT32_MAX || address & 3 ||
                !source) goto mismatch;
            uint32_t value = source[0] | (uint32_t)source[1] << 8 |
                (uint32_t)source[2] << 16 | (uint32_t)source[3] << 24;
            if (event.value != value) goto mismatch;
            if (event.offset == 0x80000) checkpoint_state.hpi_address += 4;
        } else if (!strcmp(event.type, "dsp_hpic_write")) {
            if (!hpic_count || hpic_overflow) goto mismatch;
            PendingHpicEvent *actual = &hpic_events[0];
            if (event.offset || event.address != actual->address ||
                event.value != actual->value || event.size != actual->size ||
                event.boot_phase != actual->boot_phase || event.hint != actual->hint ||
                event.dspint != actual->dspint || event.packets != actual->packets ||
                event.cycles != actual->cycles) goto mismatch;
            memmove(hpic_events, hpic_events + 1,
                    --hpic_count * sizeof(hpic_events[0]));
        } else if (!strcmp(event.type, "dsp_stop")) {
            uint32_t pc = cpu.fault ? cpu.fault_pc : cpu.pc;
            if (event.offset || event.size || !stop_pending || hpic_count || hpic_overflow ||
                event.address != pc || event.value != cpu.fault_word ||
                event.packets != cpu.packets || event.cycles != cpu.cycles ||
                event.hint != hpi.hint || event.dspint != hpi.dspint) goto mismatch;
            stop_pending = false;
            ++verified_stops;
            checkpoint_state.event_sequence = event.sequence;
            printf("{\"event\":\"verified_connected_stop\",\"sequence\":%" PRIu64
                   ",\"pc\":%" PRIu32 ",\"word\":%" PRIu32
                   ",\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64 "}\n",
                   event.sequence, pc, cpu.fault_word, cpu.packets, cpu.cycles);
            if (cpu.fault) { fault_matched = true; break; }
        } else if (!strcmp(event.type, "dsp_start") ||
                   !strcmp(event.type, "rom_hpi_ready") ||
                   !strcmp(event.type, "reset_assert")) {
            fputs("event requires a checkpoint from after DSP start/reset handoff\n", stderr);
            fclose(file);
            return EVENT_REPLAY_ERROR;
        } else goto mismatch;
        if (event.boot_phase != checkpoint_state.boot_phase ||
            event.hint != hpi.hint || event.dspint != hpi.dspint) goto mismatch;
        if (connected_stop_limit && verified_stops >= connected_stop_limit &&
            !strcmp(event.type, "dsp_stop")) break;
        if (begin_quota) {
            uint64_t before_steps = limits->steps_remaining;
            *reason = run_quota(limits, breakpoint, begin_quota);
            if (!strcmp(*reason, "phase budget exhausted"))
                *reason = "deferred slice boundary";
            slice_executed = before_steps - limits->steps_remaining;
            stop_pending = slice_end_pending = true;
            if (slice_executed != begin_quota && !hpi.hint && !cpu.fault) {
                EventReplayResult result = event_budget_exhausted(&event, limits);
                fclose(file);
                return result;
            }
        }
        if (trigger && deferred && (!checkpoint_state.dsp_started ||
                                   checkpoint_state.dsp_halted)) {
            /* run_dsp ignores triggers outside its runnable lifecycle. */
            trigger = false;
        }
        if (trigger && deferred) {
            if (stop_pending || cpu.fault || !cdj_dsp_scheduler_request(scheduler))
                goto mismatch;
            schedule_pending = true;
        } else if (trigger) {
            if (stop_pending || cpu.fault) goto mismatch;
            if (limit_reached(limits)) {
                EventReplayResult result = event_budget_exhausted(&event, limits);
                fclose(file);
                return result;
            }
            *reason = run_budget(limits, breakpoint);
            stop_pending = true;
        }
        continue;
mismatch:
        /* A requested limit explains a recorded stop whose counters are still
         * in the future.  If the counters already match, a bad PC, word, HPI
         * state, or queued DSP HPIC write remains a genuine divergence. */
        if (limit_reached(limits) && stop_pending && !cpu.fault &&
            (!strcmp(event.type, "dsp_stop") &&
             event.packets >= cpu.packets && event.cycles >= cpu.cycles &&
             (event.packets > cpu.packets || event.cycles > cpu.cycles))) {
            EventReplayResult result = event_budget_exhausted(&event, limits);
            fclose(file);
            return result;
        }
        fprintf(stderr, "event replay mismatch at sequence %" PRIu64 " (%s)\n",
                event.sequence, event.type);
        fprintf(stderr, "observed pc=%#x packets=%" PRIu64 " cycles=%" PRIu64
                " fault=%s; expected pc=%#" PRIx64 " packets=%" PRIu64 " cycles=%" PRIu64 "\n",
                cpu.pc, cpu.packets, cpu.cycles, cpu.fault ? cpu.fault : "none",
                event.address, event.packets, event.cycles);
        fclose(file);
        return EVENT_REPLAY_ERROR;
    }
    bool ok = !ferror(file) && verified_stops && !stop_pending && !hpic_count &&
              !schedule_pending && !slice_end_pending &&
              !hpic_overflow && (fault_matched || !cpu.fault);
    fclose(file);
    if (!ok) fputs("event transcript ended before a deterministic DSP boundary\n", stderr);
    return ok ? EVENT_REPLAY_COMPLETE : EVENT_REPLAY_ERROR;
}
int main(int argc, char **argv)
{
    const char *compact_trace = getenv("CDJ_DSP_COMPACT_TRACE");
    bool trace_steps = !compact_trace || strcmp(compact_trace, "1");
    const char *timing = getenv("CDJ_NXS_DSP_FUNCTIONAL_TIMING");
    observe_pcm = getenv("CDJ_DSP_OBSERVE_PCM") != NULL;
    const char *stop_limit = getenv("CDJ_DSP_CONNECTED_STOPS");
    if (stop_limit) {
        char *end;
        errno = 0;
        unsigned long n = strtoul(stop_limit, &end, 10);
        if (errno || !*stop_limit || *end || !n || n > UINT32_MAX) return 2;
        connected_stop_limit = n;
    }
    const char *audio = getenv("CDJ_NXS_DSP_FUNCTIONAL_AUDIO");
    cdj_c674x_loop_set_functional_timing(timing && !strcmp(timing, "1"));
    functional_audio = audio && !strcmp(audio, "1");
    const char *tx_path = getenv("CDJ_NXS_DSP_TX_CAPTURE");
    if (tx_path && *tx_path) {
        tx_capture = fopen(tx_path, "wb");
        if (!tx_capture) {
            perror("DSP transmit capture");
            return 2;
        }
    }
    if (argc != 8 && argc != 9) return 2;
    char *end;
    errno = 0;
    unsigned long long limit = strtoull(argv[2], &end, 10);
    if (errno || *end || !limit || argv[2][0] == '-') return 2;
    errno = 0;
    unsigned long long packet_limit = strtoull(argv[3], &end, 10);
    if (errno || *end || argv[3][0] == '-') return 2;
    errno = 0;
    unsigned long long cycle_limit = strtoull(argv[4], &end, 10);
    if (errno || *end || argv[4][0] == '-') return 2;
    errno = 0;
    unsigned long long breakpoint = strtoull(argv[5], &end, 0);
    if (errno || *end || breakpoint > UINT32_MAX) return 2;
    errno = 0;
    unsigned long boot_phase = strtoul(argv[6], &end, 0);
    if (errno || *end || boot_phase > 7) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("dump"); return 2; }
    char magic[8];
    bool checkpoint = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                      (!memcmp(magic, "CDJDSP1\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP2\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP3\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP4\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP5\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP6\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP7\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP8\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP9\0", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP10", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP11", sizeof(magic)) ||
                       !memcmp(magic, "CDJDSP12", sizeof(magic)));
    rewind(f);
    bool valid = false;
    if (!checkpoint)
        valid = fread(ram, 1, sizeof(ram), f) == sizeof(ram) &&
                fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (checkpoint) {
        char error[160] = {0};
        if (!cdj_dsp_checkpoint_read_with_l1d(
                                     argv[1], &checkpoint_state, ram, sizeof(ram),
                                     shared_ram, sizeof(shared_ram), l1d,
                                     sizeof(l1d), sdram,
                                     sizeof(sdram), error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 2;
        }
        restore_devices(&checkpoint_state);
        if (cdj_c674x_loop_functional_timing() &&
            cdj_c6747_spi_transfer_active(&spi_transfer)) {
            fputs("cannot switch an in-flight timed SPI transfer to instantaneous mode\n", stderr);
            return 2;
        }
        /* A fault is recorded after an atomic rejected packet. Clearing only
         * its diagnostic latch retries the same PC against the current core. */
        cpu.fault = NULL;
        cpu.fault_pc = 0;
        cpu.fault_word = 0;
        printf("{\"event\":\"checkpoint_restore\",\"captured_reason\":\"%s\","
               "\"captured_fault\":\"%s\",\"event_sequence\":%" PRIu64 ","
               "\"checkpoint_sequence\":%" PRIu64 "}\n",
               checkpoint_state.stop_reason, checkpoint_state.fault,
               checkpoint_state.event_sequence, checkpoint_state.checkpoint_sequence);
    } else {
        if (!valid) { fputs("expected exactly 256 KiB of L2 or a compatible checkpoint\n", stderr); return 2; }
        cdj_c6747_syscfg_reset(&syscfg);
        cdj_c6747_syscfg_priority_reset(&syscfg_priority);
        cdj_c6747_psc_reset(&psc);
        cdj_c6747_mcasp_reset(&mcasp);
        cdj_c6747_mcasp_control_reset(&mcasp_control);
        cdj_c6747_gpio_reset(&gpio);
        cdj_c6747_i2c_reset(&i2c);
        cdj_c6747_intc_reset(&intc);
        cdj_c6747_intc_delivery_reset(&intc_delivery);
        cdj_c6747_timers_reset(timers);
        cdj_c6747_spis_reset(spis);
        cdj_c6747_spi_transfer_reset(&spi_transfer);
        cdj_wm8740_reset(&wm8740);
        cdj_c6747_pll_reset(&pll);
        cdj_c6747_cache_reset(&cache);
        cdj_c6747_edma_reset(&edma);
        cdj_c6747_hpi_reset(&hpi);
        cdj_c6747_emifb_reset(&emifb);
        cdj_c6747_gpio_set_input(&gpio, 4, 5, boot_phase & 1);
        cdj_c6747_gpio_set_input(&gpio, 4, 2, boot_phase & 2);
        cdj_c6747_gpio_set_input(&gpio, 4, 3, boot_phase & 4);
        cdj_c6747_hpi_rom_boot_ready(&hpi);
        cdj_c6747_hpi_host_write(&hpi, 0x01050105); /* MAIN acks ROM HINT and selects HWOB. */
        cdj_c6747_hpi_host_write(&hpi, 0x01030103); /* Captured dump precedes DSPINT. */
        cdj_c6747_intc_deliver_event(&intc, &intc_delivery, 34);
        uint32_t entry;
        read_bus(NULL, 0x11800000, &entry);
        cdj_c674x_reset(&cpu, entry);
        checkpoint_state.boot_phase = boot_phase;
        checkpoint_state.reset_released = checkpoint_state.dsp_started = true;
    }
    cpu.cycle_tick = cycle_tick;
    coverage_initial_packets = cpu.packets;
    coverage_initial_cycles = cpu.cycles;
    ReplayLimits limits = {
        .steps_remaining = limit,
        .initial_packets = cpu.packets,
        .initial_cycles = cpu.cycles,
        .packet_limit = packet_limit,
        .cycle_limit = cycle_limit,
    };
    const char *reason = "step_limit";
    if (argc == 9) {
        if (!checkpoint) return 2;
        EventReplayResult result = replay_external_events(
            argv[8], &limits, breakpoint, &reason);
        if (result == EVENT_REPLAY_ERROR)
            return 2;
        if (result == EVENT_REPLAY_BUDGET_EXHAUSTED) {
            coverage_emit();
            int tx_status = tx_capture ? fclose(tx_capture) : 0;
            return ferror(stdout) || tx_capture_failed || tx_status ? 2 : 3;
        }
    } else {
        if (checkpoint && checkpoint_state.scheduler.mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 &&
            checkpoint_state.scheduler.pending) {
            fputs("pending deferred DSP checkpoint requires event transcript\n", stderr);
            return 2;
        }
        for (;;) {
            const char *limited = limit_reached(&limits);
            if (limited) { reason = limited; break; }
            if (breakpoint && cpu.pc == breakpoint) { reason = "breakpoint"; break; }
            deliver_edma_notifications();
            if (!cdj_c674x_interrupt(
                    &cpu, cdj_c6747_intc_cpu_pending(&intc_delivery))) {
                reason = "fault";
                break;
            }
            if (trace_steps) printf("{\"event\":\"step\",\"pc\":%" PRIu32 ",\"cycles\":%" PRIu64
                   ",\"loop_active\":%s,\"branch_due\":%" PRIu64 "}\n",
                   cpu.pc, cpu.cycles, cpu.loop_active ? "true" : "false", cpu.branch_due);
            pcm_observe();
            CoverageBefore before = coverage_before(&cpu);
            CdjC674xPacket coverage_packet;
            bool has_coverage_packet = !before.direct_fetch &&
                coverage_capture(&cpu, &coverage_packet);
            if (!cdj_c674x_step_capture_direct(&cpu, read_bus, write_bus, NULL,
                before.direct_fetch ? &coverage_packet : NULL)) { reason = "fault"; break; }
            coverage_record(&before, (before.direct_fetch || has_coverage_packet) ?
                        &coverage_packet : NULL);
            --limits.steps_remaining;
            if (spi_transfer.fault) {
                cpu.fault = "unsupported SPI transfer clock or state";
                cpu.fault_pc = cpu.pc;
                reason = "fault";
                break;
            }
            cdj_c6747_psc_tick(&psc);
            if (!functional_audio_tick()) { reason = "fault"; break; }
            if (hpi.hint) { reason = "host_event_required"; break; }
        }
    }
    coverage_emit();
    /* Fault strings originate in the interpreter and contain no JSON escapes. */
    printf("{\"event\":\"stop\",\"reason\":\"%s\",\"fault\":\"%s\",\"pc\":%" PRIu32
           ",\"fault_pc\":%" PRIu32 ",\"fault_word\":%" PRIu32
           ",\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64 ",\"registers\":[",
           reason, cpu.fault ? cpu.fault : "", cpu.pc, cpu.fault_pc, cpu.fault_word,
           cpu.packets, cpu.cycles);
    for (unsigned bank = 0; bank < 2; ++bank) {
        printf("%s[", bank ? "," : "");
        for (unsigned i = 0; i < 32; ++i) printf("%s%" PRIu32, i ? "," : "", cpu.r[bank][i]);
        printf("]");
    }
    printf("],\"control\":{\"csr\":%u,\"ifr\":%u,\"ier\":%u,\"irp\":%u,"
           "\"ilc\":%u,\"rilc\":%u,\"tsr\":%u,\"itsr\":%u,"
           "\"loop_context\":%" PRIu64 "},\"pending_stores\":%u,"
           "\"pending_loads\":%u,\"syscfg_unlocked\":%s,\"pll_legacy_bit4_used\":%s,"
           "\"pll_oscin_cycles\":%" PRIu64 ",\"pll_reset_age\":%u,\"pll_lock_wait_remaining\":%u,"
           "\"pll_early_enable\":%s,\"cfgchip\":[%u,%u,%u,%u],"
           "\"amute_clear_pulses\":%u,\"hpi\":{\"reset\":%s,\"hwob\":%s,"
           "\"dspint\":%s,\"hint\":%s},\"emifb\":{\"sdcfg\":%u,"
           "\"sdrfc\":%u,\"sdtim1\":%u,\"sdtim2\":%u,"
           "\"init_sequences\":%u},\"wm8740\":{\"transfers\":%" PRIu64 ","
           "\"last_word\":%u,\"program\":[%u,%u,%u,%u,%u],"
           "\"active_attenuation\":[%u,%u],\"register4_unlocked\":%s}}\n",
           cpu.control[1], cpu.control[2], cpu.control[4], cpu.control[6],
           cpu.control[13], cpu.control[14], cpu.control[26], cpu.control[27],
           cpu.control_ready[31],
           cpu.store_count, cpu.load_count, syscfg.unlocked ? "true" : "false",
           pll.legacy_bit4_used ? "true" : "false", pll.oscin_cycles,
           pll.reset_age, pll.lock_wait_remaining, pll.early_enable ? "true" : "false",
           syscfg.cfgchip[0], syscfg.cfgchip[1], syscfg.cfgchip[2],
           syscfg.cfgchip[3], syscfg.amute_clear_pulses,
           hpi.hpirst ? "true" : "false", hpi.hwob ? "true" : "false",
           hpi.dspint ? "true" : "false", hpi.hint ? "true" : "false",
           emifb.sdcfg, emifb.sdrfc, emifb.sdtim1, emifb.sdtim2,
           emifb.init_sequences, wm8740.transfers, wm8740.last_word,
           wm8740.program[0], wm8740.program[1], wm8740.program[2],
           wm8740.program[3], wm8740.program[4], wm8740.active_attenuation[0],
           wm8740.active_attenuation[1],
           wm8740.register4_unlocked ? "true" : "false");
    if (argc >= 8) {
        char error[160] = {0};
        capture_devices(&checkpoint_state, reason);
        if (!cdj_dsp_checkpoint_write_with_l1d(
                                      argv[7], &checkpoint_state, ram, sizeof(ram),
                                      shared_ram, sizeof(shared_ram), l1d,
                                      sizeof(l1d), sdram,
                                      sizeof(sdram), error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 2;
        }
    }
    int tx_status = tx_capture ? fclose(tx_capture) : 0;
    return ferror(stdout) || tx_capture_failed || tx_status ? 2 : 0;
}
