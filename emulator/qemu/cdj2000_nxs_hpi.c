/* SPDX-License-Identifier: GPL-2.0-or-later
 * NXS MAIN-facing TI UHPI transport with a partial C674x execution core.
 * Matches the independently verified NXS upload path: byte-addressed global
 * L2, HWOB=1, HPID auto-increment and fixed-address accesses.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "cdj2000_nxs_hpi.h"
#include "cdj_c674x.h"
#include "cdj_c6747_syscfg.h"
#include "cdj_c6747_psc.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_gpio.h"
#include "cdj_c6747_i2c.h"
#include "cdj_c6747_intc.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_timer.h"
#include "cdj_c6747_spi.h"
#include "cdj_c6747_spi_clock.h"
#include "cdj_c6747_cache.h"
#include "cdj_c6747_edma.h"
#include "cdj_c6747_hpi.h"
#include "cdj_c6747_emifb.h"
#include "cdj_dsp_checkpoint.h"
#include "cdj_dsp_scheduler.h"

#define HPI_BASE 0x0c000000u
#define L2_BASE 0x11800000u
#define L2_SIZE 0x40000u
#define SHARED_RAM_BASE 0x80000000u
#define SHARED_RAM_SIZE 0x20000u
#define SDRAM_BASE 0xc0000000u
#define SDRAM_SIZE 0x02000000u
/* Cooperative QEMU scheduling quantum, not a C6747 timing property. HINT
 * still yields immediately. One million packets lets initialization reach
 * its genuine wait loop after the final MAIN event instead of stranding the
 * DSP merely because no later host transition happens to resume it. */

typedef struct {
    MemoryRegion registers;
    uint8_t l2[L2_SIZE];
    uint8_t l1d[CDJ_DSP_L1D_SIZE];
    uint32_t address;
    CdjC6747Hpi hpi;
    bool reset_released, dsp_started, dsp_halted, dsp_running;
    bool functional_audio;
    CdjDspScheduler scheduler;
    QEMUTimer *dsp_timer;
    unsigned boot_phase;
    uint64_t words;
    uint64_t event_sequence, checkpoint_sequence;
    int64_t checkpoint_check_ns; /* Host-only diagnostic polling throttle. */
    CdjC674x cpu;
    CdjC6747Syscfg syscfg;
    CdjC6747Psc psc;
    CdjC6747Mcasp mcasp;
    CdjC6747McaspControl mcasp_control;
    CdjC6747Gpio gpio;
    CdjC6747I2c i2c;
    CdjC6747Intc intc;
    CdjC6747IntcDelivery intc_delivery;
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT];
    CdjWm8740 wm8740;
    CdjC6747SpiTransfer spi_transfer;
    CdjC6747Cache cache;
    CdjC6747Edma edma;
    CdjC6747SyscfgPriority syscfg_priority;
    CdjC6747Pll pll;
    CdjC6747Emifb emifb;
    uint8_t *shared_ram;
    uint8_t *sdram;
    void (*hint)(void *, bool);
    void *opaque;
    FILE *event_log;
    FILE *tx_capture;
    char *tx_capture_path;
    uint64_t tx_capture_sequence;
    bool tx_capture_failed;
} NxsHpi;
static NxsHpi *nxs_hpi;
static void run_dsp(NxsHpi *s);

static void record_event(NxsHpi *s, const char *type, uint64_t offset,
                         uint64_t address, uint64_t value, unsigned size)
{
    const char *path = getenv("CDJ_NXS_DSP_EVENTS");
    if (!path || !*path) return;
    if (!s->event_log) {
        s->event_log = fopen(path, "a");
        if (!s->event_log) {
            error_report("nxs-hpi: cannot open DSP event transcript %s", path);
            return;
        }
    }
    ++s->event_sequence;
    if (fprintf(s->event_log,
                "{\"sequence\":%" PRIu64 ",\"event\":\"%s\","
                "\"offset\":%" PRIu64 ",\"address\":%" PRIu64 ","
                "\"value\":%" PRIu64 ",\"size\":%u,\"boot_phase\":%u,"
                "\"hint\":%s,\"dspint\":%s,\"packets\":%" PRIu64 ","
                "\"cycles\":%" PRIu64 "}\n",
                s->event_sequence, type, offset, address, value, size,
                s->boot_phase, s->hpi.hint ? "true" : "false",
                s->hpi.dspint ? "true" : "false", s->cpu.packets,
                s->cpu.cycles) < 0 || fflush(s->event_log))
        error_report("nxs-hpi: DSP event transcript write failed");
}

static bool capture_checkpoint(NxsHpi *s, const char *reason)
{
    const char *policy = getenv("CDJ_NXS_DSP_CHECKPOINT_POLICY");
    if (policy && !strcmp(policy, "fault") && !s->dsp_halted &&
        strcmp(reason, "debug request")) return false;
    const char *directory = getenv("CDJ_NXS_DSP_CHECKPOINT_DIR");
    if (!directory || !*directory || !s->shared_ram || !s->sdram) return false;
    if (g_mkdir_with_parents(directory, 0700)) {
        error_report("nxs-hpi: cannot create DSP checkpoint directory %s", directory);
        return false;
    }
    CdjDspCheckpointState state = {0};
    state.hpi_address = s->address;
    state.boot_phase = s->boot_phase;
    state.words = s->words;
    state.event_sequence = s->event_sequence;
    state.checkpoint_sequence = ++s->checkpoint_sequence;
    state.reset_released = s->reset_released;
    state.dsp_started = s->dsp_started;
    state.dsp_halted = s->dsp_halted;
    state.cpu = s->cpu;
    state.syscfg = s->syscfg;
    state.psc = s->psc;
    state.mcasp = s->mcasp;
    state.mcasp_control = s->mcasp_control;
    state.gpio = s->gpio;
    state.i2c = s->i2c;
    state.pll = s->pll;
    state.hpi = s->hpi;
    state.emifb = s->emifb;
    state.intc = s->intc;
    state.intc_delivery = s->intc_delivery;
    memcpy(state.timers, s->timers, sizeof(state.timers));
    memcpy(state.spis, s->spis, sizeof(state.spis));
    state.spi_transfer = s->spi_transfer;
    state.scheduler = s->scheduler;
    state.wm8740 = s->wm8740;
    state.cache = s->cache;
    state.edma = s->edma;
    state.syscfg_priority = s->syscfg_priority;
    cdj_dsp_checkpoint_prepare(&state, reason);
    g_autofree char *name = g_strdup_printf("%020" PRIu64 ".cdjdsp",
                                             state.checkpoint_sequence);
    g_autofree char *path = g_build_filename(directory, name, NULL);
    char error[160] = {0};
    if (!cdj_dsp_checkpoint_write_with_l1d(
            path, &state, s->l2, sizeof(s->l2),
            s->shared_ram, SHARED_RAM_SIZE, s->l1d, sizeof(s->l1d),
            s->sdram, SDRAM_SIZE, error, sizeof(error))) {
        error_report("nxs-hpi: checkpoint failed: %s", error);
        return false;
    }
    info_report("nxs-hpi: checkpoint=%s reason=%s event-sequence=%" PRIu64,
                path, reason, state.event_sequence);
    return true;
}

static void capture_requested_checkpoint(NxsHpi *s)
{
    const char *request = getenv("CDJ_NXS_DSP_CHECKPOINT_REQUEST");
    if (!request || !*request || s->dsp_running) return;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now < s->checkpoint_check_ns) return;
    s->checkpoint_check_ns = now + 100000000;
    if (!g_file_test(request, G_FILE_TEST_EXISTS)) return;
    bool ok = capture_checkpoint(s, "debug request");
    g_autofree char *response = ok ?
        g_strdup_printf("{\"ok\":true,\"file\":\"%020" PRIu64 ".cdjdsp\"}\n",
                        s->checkpoint_sequence) :
        g_strdup("{\"ok\":false,\"error\":\"DSP checkpoint capture failed; inspect main-stderr.log\"}\n");
    g_autofree char *done = g_strconcat(request, ".done", NULL);
    if (!g_file_set_contents(done, response, -1, NULL))
        error_report("nxs-hpi: cannot acknowledge DSP checkpoint request");
    if (remove(request))
        error_report("nxs-hpi: cannot remove DSP checkpoint request");
}

bool cdj_nxs_hpi_port(hwaddr address)
{
    return nxs_hpi && (address == HPI_BASE + 0x80000 || address == HPI_BASE + 0xc0000);
}

void cdj_nxs_hpi_reset_line(bool released)
{
    NxsHpi *s = nxs_hpi;

    if (!s || released == s->reset_released) return;
    s->reset_released = released;
    if (!released) {
        if (s->dsp_timer) timer_del(s->dsp_timer);
        uint8_t scheduler_mode = s->scheduler.mode;
        cdj_dsp_scheduler_reset(&s->scheduler);
        s->scheduler.mode = scheduler_mode;
        if (!scheduler_mode) memset(&s->scheduler, 0, sizeof(s->scheduler));
        /* External DSP reset covers the HPI boot contract, C674x megamodule
         * INTC, Timer64P/SPI blocks, and interpreter lifecycle. Other device
         * peripheral reset domains remain explicit models. */
        cdj_c6747_hpi_reset(&s->hpi);
        cdj_c6747_intc_reset(&s->intc);
        cdj_c6747_intc_delivery_reset(&s->intc_delivery);
        cdj_c6747_timers_reset(s->timers);
        cdj_c6747_spis_reset(s->spis);
        cdj_c6747_spi_transfer_reset(&s->spi_transfer);
        cdj_wm8740_reset(&s->wm8740);
        cdj_c6747_cache_reset(&s->cache);
        /* SRAM contents are undefined across external reset.  Clear the
         * functional backing store so a later SRAM partition cannot expose
         * bytes retained from the preceding DSP lifetime. */
        memset(s->l1d, 0, sizeof(s->l1d));
        cdj_c6747_edma_reset(&s->edma);
        cdj_c6747_mcasp_reset(&s->mcasp);
        cdj_c6747_mcasp_control_reset(&s->mcasp_control);
        s->dsp_started = s->dsp_halted = s->dsp_running = false;
        if (s->hint) s->hint(s->opaque, true);
        record_event(s, "reset_assert", 0, 0, 0, 0);
        info_report("nxs-hpi: DSP reset asserted; HPI boot state reset");
        return;
    }

    /* The on-chip ROM itself is not executed. TI SPRABB1C section 4.1 says
     * its HPI boot path sets HINT when ready for the host download. This is
     * the sole ROM handoff abstraction; uploaded firmware remains genuine. */
    cdj_c6747_hpi_rom_boot_ready(&s->hpi);
    if (s->hint) s->hint(s->opaque, false);
    record_event(s, "rom_hpi_ready", 0, 0, 1, 0);
    info_report("nxs-hpi: DSP reset released; ROM HPI-ready HINT asserted");
}

void cdj_nxs_hpi_boot_phase(unsigned phase)
{
    NxsHpi *s = nxs_hpi;

    if (!s || phase > 7) return;
    bool changed = phase != s->boot_phase;
    s->boot_phase = phase;
    /* Schematic-confirmed CPU_PH0/1/2 reach GP4[5]/GP4[2]/GP4[3]. */
    cdj_c6747_gpio_set_input(&s->gpio, 4, 5, phase & 1);
    cdj_c6747_gpio_set_input(&s->gpio, 4, 2, phase & 2);
    cdj_c6747_gpio_set_input(&s->gpio, 4, 3, phase & 4);
    info_report("nxs-hpi: MAIN boot phase=%u -> DSP GP4 inputs=%#x",
                phase, s->gpio.input[2] & 0x2c);
    record_event(s, "boot_phase", 0, 0, phase, 0);
    /* A phase-budget yield is a cooperative scheduling boundary, not a DSP
     * halt.  Resume when the genuine MAIN firmware changes the sideband that
     * the DSP is polling. */
    if (changed) {
        if (s->dsp_started && !s->dsp_halted)
            capture_checkpoint(s, "boot-phase boundary");
        run_dsp(s);
    }
}

static uint8_t *host_memory(NxsHpi *s, uint32_t address)
{
    uint32_t l1d_offset;
    if (address >= 0x11f00000u &&
        cdj_c6747_l1d_sram_span(&s->cache, address, 4, &l1d_offset))
        return s->l1d + l1d_offset;
    if (address >= L2_BASE && address <= L2_BASE + L2_SIZE - 4)
        return s->l2 + address - L2_BASE;
    if (address >= SHARED_RAM_BASE &&
        address <= SHARED_RAM_BASE + SHARED_RAM_SIZE - 4)
        return s->shared_ram + address - SHARED_RAM_BASE;
    uint32_t sdram_offset;
    if (cdj_c6747_emifb_sdram_offset(&s->emifb, address, 4, SDRAM_SIZE,
                                    &sdram_offset))
        return s->sdram + sdram_offset;
    return NULL;
}

static bool valid_data(NxsHpi *s)
{
    return s->hpi.hwob && !s->hpi.hpirst && !(s->address & 3) &&
           host_memory(s, s->address) != NULL;
}

static bool dsp_read(void *opaque, uint32_t address, uint32_t *value)
{
    NxsHpi *s = opaque;
    /* RAM and its local L2 alias do not overlap any peripheral window.
     * Instruction fetches dominate reads: avoid probing every MMIO device.
     * Keep SDRAM's dynamic enable gate and all alignment checks. */
    uint32_t l1d_offset;
    if (!(address & 3) && cdj_c6747_l1d_sram_span(
            &s->cache, address, 4, &l1d_offset)) {
        *value = ldl_le_p(s->l1d + l1d_offset);
        return true;
    }
    if (!(address & 3) && address >= SHARED_RAM_BASE &&
        address <= SHARED_RAM_BASE + SHARED_RAM_SIZE - 4) {
        *value = ldl_le_p(s->shared_ram + address - SHARED_RAM_BASE);
        return true;
    }
    uint32_t sdram_offset;
    if (!(address & 3) && address >= SDRAM_BASE && address < 0xe0000000u &&
        cdj_c6747_emifb_sdram_offset(&s->emifb, address, 4, SDRAM_SIZE,
                                    &sdram_offset)) {
        *value = ldl_le_p(s->sdram + sdram_offset);
        return true;
    }
    uint32_t local_address = address;
    if (address >= 0x00800000 && address < 0x00840000) local_address += 0x11000000;
    if (!(local_address & 3) && local_address >= L2_BASE &&
        local_address <= L2_BASE + L2_SIZE - 4) {
        *value = ldl_le_p(s->l2 + local_address - L2_BASE);
        return true;
    }
    if (cdj_c6747_syscfg_read(&s->syscfg, address, value)) return true;
    if (cdj_c6747_syscfg_priority_read(&s->syscfg_priority, address, value))
        return true;
    if (cdj_c6747_psc_read(&s->psc, address, value)) return true;
    if (cdj_c6747_mcasp_read(&s->mcasp, address, value)) return true;
    if (cdj_c6747_mcasp_control_read(&s->mcasp_control, address, value)) return true;
    if (cdj_c6747_gpio_read(&s->gpio, address, value)) return true;
    if (cdj_c6747_i2c_read(&s->i2c, address, value)) return true;
    if (cdj_c6747_intc_read(&s->intc, address, value)) return true;
    if (cdj_c6747_timers_read(s->timers, address, value)) return true;
    if (!cdj_c674x_loop_functional_timing() &&
        cdj_c6747_spi_wm8740_timed_mapped(address))
        return cdj_c6747_spi_wm8740_read_timed(s->spis, &s->spi_transfer,
                                               address, value);
    if (cdj_c6747_spis_read(s->spis, address, value)) return true;
    if (cdj_c6747_cache_read(&s->cache, address, value)) return true;
    if (cdj_c6747_edma_read(&s->edma, address, value)) return true;
    if (cdj_c6747_pll_read(&s->pll, address, value)) return true;
    if (cdj_c6747_emifb_read(&s->emifb, address, value)) return true;
    if ((s->syscfg.cfgchip[1] & 0x8000) &&
        cdj_c6747_hpi_cpu_read(&s->hpi, address, value)) return true;
    return false;
}

static uint8_t *dsp_memory_span(NxsHpi *s, uint32_t address, size_t size)
{
    uint64_t end = (uint64_t)address + size;
    if (!size || end > UINT64_C(0x100000000)) return NULL;
    uint32_t l1d_offset;
    if (cdj_c6747_l1d_sram_span(&s->cache, address, size, &l1d_offset))
        return s->l1d + l1d_offset;
    if (address >= L2_BASE && end <= (uint64_t)L2_BASE + L2_SIZE)
        return s->l2 + address - L2_BASE;
    if (address >= 0x00800000u &&
        end <= UINT64_C(0x00800000) + L2_SIZE)
        return s->l2 + address - 0x00800000u;
    if (address >= SHARED_RAM_BASE &&
        end <= (uint64_t)SHARED_RAM_BASE + SHARED_RAM_SIZE)
        return s->shared_ram + address - SHARED_RAM_BASE;
    uint32_t sdram_offset;
    if (cdj_c6747_emifb_sdram_offset(&s->emifb, address, size, SDRAM_SIZE,
                                    &sdram_offset))
        return s->sdram + sdram_offset;
    return NULL;
}

static bool dsp_l1d_write(NxsHpi *s, uint32_t address, uint64_t value,
                          unsigned size, bool commit)
{
    uint32_t offset;
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        !cdj_c6747_l1d_sram_span(&s->cache, address, size, &offset))
        return false;
    if (commit) {
        uint8_t *target = s->l1d + offset;
        if (size == 8) stq_le_p(target, value);
        else if (size == 1) *target = value;
        else if (size == 2) stw_le_p(target, value);
        else stl_le_p(target, value);
    }
    return true;
}

typedef struct {
    uint8_t *target;
    uint8_t *bytes;
    size_t size;
} EdmaStagedWrite;

typedef struct {
    NxsHpi *owner;
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
        EdmaStagedWrite *writes = g_try_renew(
            EdmaStagedWrite, context->writes, capacity);
        if (!writes) return false;
        context->writes = writes;
        context->write_capacity = capacity;
    }
    uint8_t *copy = g_try_malloc(size);
    if (!copy) return false;
    memcpy(copy, bytes, size);
    context->writes[context->write_count++] =
        (EdmaStagedWrite){target, copy, size};
    return true;
}

static void edma_free_staged_writes(EdmaBusContext *context)
{
    for (size_t i = 0; i < context->write_count; ++i)
        g_free(context->writes[i].bytes);
    g_free(context->writes);
}

static bool edma_read_bytes(void *opaque, uint32_t address, uint8_t *bytes,
                            size_t size)
{
    EdmaBusContext *context = opaque;
    uint8_t *source = dsp_memory_span(context->owner, address, size);
    if (!source) return false;
    memcpy(bytes, source, size);
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

static bool edma_write_bytes(void *opaque, uint32_t address,
                             const uint8_t *bytes, size_t size, bool commit)
{
    EdmaBusContext *context = opaque;
    uint8_t *target = dsp_memory_span(context->owner, address, size);
    if (target) {
        return !commit || edma_stage_write(context, target, bytes, size);
    }
    if (size == 4) {
        uint32_t value = ldl_le_p(bytes);
        return cdj_c6747_mcasp_control_write(context->mcasp, address,
                                             value, 4, commit);
    }
    return false;
}

static bool service_mcasp_axevt(CdjC6747Edma *edma,
                                CdjC6747McaspControl *mcasp,
                                EdmaBusContext *context)
{
    const CdjC6747EdmaBus bus = {edma_read_bytes, edma_write_bytes, context};
    for (unsigned instance = 1; instance <= 2; ++instance) {
        unsigned channel = instance == 1 ? 3 : 5;
        for (unsigned serializer = 0;
             serializer < 16 &&
             cdj_c6747_mcasp_axevt_ready(mcasp, instance);
             ++serializer) {
            uint64_t before = mcasp->xbuf_writes[instance];
            if (!cdj_c6747_edma_event(edma, channel, &bus)) return false;
            /* A disabled EDMA channel latches ER without servicing XBUF.
             * Stop until EESR is programmed rather than spinning. */
            if (mcasp->xbuf_writes[instance] == before) break;
        }
    }
    return true;
}

static void deliver_edma_notifications(NxsHpi *s)
{
    /* C6747 system event 8 is the EDMA3CC region-1 completion pulse. */
    if (cdj_c6747_edma_take_irq_notification(&s->edma, 1))
        cdj_c6747_intc_deliver_event(&s->intc, &s->intc_delivery, 8);
}

static bool edma_mcasp_transaction(NxsHpi *s, bool edma_access,
                                   uint32_t address, uint64_t value,
                                   unsigned size, bool commit)
{
    if (!(edma_access ? cdj_c6747_edma_write_mapped(address, size) :
                       cdj_c6747_mcasp_control_write_mapped(address, size)))
        return false;
    CdjC6747Edma trial_edma = s->edma;
    CdjC6747McaspControl trial_mcasp = s->mcasp_control;
    EdmaBusContext trial_context = {.owner = s, .mcasp = &trial_mcasp};
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
        s->edma = trial_edma;
        s->mcasp_control = trial_mcasp;
        deliver_edma_notifications(s);
    }
    edma_free_staged_writes(&trial_context);
    return ok;
}

static bool advance_functional_mcasp_slots(NxsHpi *s)
{
    CdjC6747McaspControl original_mcasp = s->mcasp_control;
    CdjC6747Edma trial_edma = s->edma;
    CdjC6747McaspControl trial_mcasp = s->mcasp_control;
    EdmaBusContext trial_context = {.owner = s, .mcasp = &trial_mcasp};
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
        s->edma = trial_edma;
        s->mcasp_control = trial_mcasp;
        deliver_edma_notifications(s);
        if (s->tx_capture) {
            for (unsigned instance = 1; instance <= 2; ++instance) {
                for (unsigned serializer = 0; serializer < 16; ++serializer) {
                    uint64_t sequence = trial_mcasp.xrsr_source_sequence[instance][serializer];
                    if (!sequence || sequence ==
                        original_mcasp.xrsr_source_sequence[instance][serializer])
                        continue;
                    if (fprintf(s->tx_capture,
                            "{\"sequence\":%" PRIu64 ",\"instance\":%u,"
                            "\"slot\":%u,\"serializer\":%u,\"word\":%u,"
                            "\"xbuf_sequence\":%" PRIu64 ",\"packets\":%" PRIu64 ","
                            "\"cycles\":%" PRIu64 ",\"source\":\"genuine_xbuf\","
                            "\"clock\":\"functional-coarse-packet-slot\"}\n",
                            ++s->tx_capture_sequence, instance,
                            trial_mcasp.xslot[instance], serializer,
                            trial_mcasp.xrsr[instance][serializer], sequence,
                            s->cpu.packets, s->cpu.cycles) < 0 ||
                        fflush(s->tx_capture)) {
                        s->tx_capture_failed = true;
                        fclose(s->tx_capture);
                        s->tx_capture = NULL;
                        if (s->tx_capture_path)
                            remove(s->tx_capture_path);
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

static bool functional_audio_tick(NxsHpi *s)
{
    if (!s->functional_audio ||
        s->cpu.packets % CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL)
        return true;
    if (s->tx_capture_failed) {
        s->cpu.fault = "DSP transmit capture write failed";
        s->cpu.fault_pc = s->cpu.pc;
        s->cpu.fault_word = 0;
        return false;
    }
    if (advance_functional_mcasp_slots(s)) return true;
    s->cpu.fault = s->tx_capture_failed ? "DSP transmit capture write failed" :
                   "unsupported functional McASP transmit slot";
    s->cpu.fault_pc = s->cpu.pc;
    s->cpu.fault_word = 0;
    return false;
}

static bool dsp_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    NxsHpi *s = opaque;
    if (dsp_l1d_write(s, address, value, size, commit)) return true;
    if (cdj_c6747_syscfg_pll_locked(&s->syscfg) &&
        cdj_c6747_pll_write_mapped(address, size)) {
        if (commit) info_report("nxs-pll: locked write ignored address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (!cdj_c674x_loop_functional_timing() &&
        (s->spi_transfer.phase || s->spi_transfer.queued_valid) &&
        cdj_c6747_pll_write_mapped(address, size)) return false;
    if (cdj_c6747_pll_write(&s->pll, address, value, size, commit)) {
        if (commit) info_report("nxs-pll: write address=%#x value=%#x legacy-bit4-assumption=%d",
                                address, (uint32_t)value, s->pll.legacy_bit4_used);
        return true;
    }
    if (cdj_c6747_i2c_write(&s->i2c, address, value, size, commit)) {
        if (commit) info_report("nxs-i2c: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_intc_write_delivery(&s->intc, &s->intc_delivery,
                                      address, value, size, commit)) {
        if (commit) info_report("nxs-intc: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_timers_write(s->timers, address, value, size, commit)) {
        if (commit) info_report("nxs-timer: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (!cdj_c674x_loop_functional_timing() &&
        cdj_c6747_spi_wm8740_timed_mapped(address)) {
        if ((address == CDJ_C6747_SPI1_BASE + 0x38 ||
             address == CDJ_C6747_SPI1_BASE + 0x3c) &&
            (s->spis[1].gcr1 & (1u << 24)) &&
            !cdj_spi_clock_ready(&s->pll)) return false;
        return cdj_c6747_spi_wm8740_write_timed(s->spis, &s->wm8740,
                  &s->spi_transfer, address, value, size, commit);
    }
    if (cdj_c6747_spis_write_wm8740(
            s->spis, &s->wm8740, address, value, size,
            cdj_c674x_loop_functional_timing(), commit)) {
        if (commit)
            info_report("nxs-spi: WM8740 write word=%#x transfers=%" PRIu64,
                        (uint32_t)value & 0xffff, s->wm8740.transfers);
        return true;
    }
    if (cdj_c6747_spis_write(s->spis, address, value, size, commit)) {
        if (commit) info_report("nxs-spi: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_cache_write(&s->cache, address, value, size, commit)) {
        if (commit) info_report("nxs-cache: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (edma_mcasp_transaction(s, true, address, value, size, commit)) {
        if (commit) info_report("nxs-edma: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_gpio_write(&s->gpio, address, value, size, commit)) {
        if (commit) info_report("nxs-gpio: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_mcasp_write(&s->mcasp, address, value, size, commit)) {
        if (commit) info_report("nxs-mcasp: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (edma_mcasp_transaction(s, false, address, value, size, commit)) {
        if (commit)
            info_report("nxs-mcasp-control: write address=%#x value=%#x",
                        address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_psc_write(&s->psc, address, value, size, commit)) {
        if (commit) info_report("nxs-psc: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_syscfg_write(&s->syscfg, address, value, size, commit)) {
        if (commit) info_report("nxs-syscfg: write address=%#x value=%#x unlocked=%d",
                                address, (uint32_t)value, s->syscfg.unlocked);
        return true;
    }
    if (cdj_c6747_syscfg_priority_write(&s->syscfg_priority, &s->syscfg,
                                        address, value, size, commit)) {
        if (commit)
            info_report("nxs-syscfg: master priority address=%#x value=%#x",
                        address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_emifb_write(&s->emifb, address, value, size, commit)) {
        if (commit) info_report("nxs-emifb: write address=%#x value=%#x init-sequences=%u",
                                address, (uint32_t)value, s->emifb.init_sequences);
        return true;
    }
    if ((s->syscfg.cfgchip[1] & 0x8000)) {
        bool old_hint = s->hpi.hint;
        if (cdj_c6747_hpi_cpu_write(&s->hpi, address, value, size, commit)) {
            if (commit) {
                info_report("nxs-hpi: DSP HPIC write value=%#x DSPINT=%d HINT=%d",
                            (uint32_t)value, s->hpi.dspint, s->hpi.hint);
                record_event(s, "dsp_hpic_write", 0, address, value, size);
                if (!old_hint && s->hpi.hint && s->hint) s->hint(s->opaque, false);
            }
            return true;
        }
    }
    if ((size == 1 || size == 2 || size == 4 || size == 8) &&
        address >= SHARED_RAM_BASE &&
        address <= SHARED_RAM_BASE + SHARED_RAM_SIZE - size) {
        if (commit) {
            uint8_t *target = s->shared_ram + address - SHARED_RAM_BASE;
            if (size == 8) stq_le_p(target, value);
            else if (size == 1) *target = value;
            else if (size == 2) stw_le_p(target, value);
            else stl_le_p(target, value);
        }
        return true;
    }
    uint32_t sdram_offset;
    if ((size == 1 || size == 2 || size == 4 || size == 8) &&
        cdj_c6747_emifb_sdram_offset(&s->emifb, address, size, SDRAM_SIZE,
                                    &sdram_offset)) {
        if (commit) {
            if (size == 8) stq_le_p(s->sdram + sdram_offset, value);
            else if (size == 1) s->sdram[sdram_offset] = value;
            else if (size == 2) stw_le_p(s->sdram + sdram_offset, value);
            else stl_le_p(s->sdram + sdram_offset, value);
        }
        return true;
    }
    if (address >= 0x00800000 && address < 0x00840000) address += 0x11000000;
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        address < L2_BASE || address > L2_BASE + L2_SIZE - size) return false;
    if (commit) {
        if (size == 8) stq_le_p(s->l2 + address - L2_BASE, value);
        else if (size == 1) s->l2[address - L2_BASE] = value;
        else if (size == 2) stw_le_p(s->l2 + address - L2_BASE, value);
        else stl_le_p(s->l2 + address - L2_BASE, value);
    }
    return true;
}

static void dsp_cycle_tick(void *opaque)
{
    NxsHpi *s = opaque;
    uint64_t transfers = s->wm8740.transfers;
    if (!cdj_c674x_loop_functional_timing())
        cdj_spi_core_tick(s->spis, &s->wm8740, &s->spi_transfer, &s->pll);
    if (s->wm8740.transfers != transfers)
        info_report("nxs-spi: timed WM8740 latch word=%#x transfers=%" PRIu64,
                    s->wm8740.last_word, s->wm8740.transfers);
    cdj_c6747_pll_tick(&s->pll);
    /* SPRUH91D chapter 28 counts on the timer input clock; this callback is
     * the only per-cycle hook the core offers, so one call is one input clock
     * period - an approximation of nothing measurable.  It is declared in BOTH
     * provenance artifacts, because both reach this line: tools/cdj_dsp/replay.py
     * for replay runs and tools/cdj_main/nxs_vm.py for the QEMU firmware boots
     * that write runs/<run>/dsp-checkpoints/manifest.json.  This is the board a
     * "firmware delay loop terminated" observation would be made on, so the
     * declaration there is load-bearing, not decorative.  The events themselves
     * are Table 2-1's, mapped by cdj_c6747_timer_event(). */
    uint32_t timer_outputs = cdj_c6747_timers_tick(s->timers);
    for (unsigned bit = 0; timer_outputs >> bit; ++bit)
        if (timer_outputs & (1u << bit))
            cdj_c6747_intc_deliver_event(&s->intc, &s->intc_delivery,
                                         cdj_c6747_timer_event(bit));
}

static void report_dsp(NxsHpi *s, const char *reason)
{
    capture_requested_checkpoint(s);
    info_report("nxs-c674x: packets=%" PRIu64 " cycles=%" PRIu64
                " pc=%#x word=%#x stop=%s B15=%#x B14=%#x B3=%#x",
                s->cpu.packets, s->cpu.cycles, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                s->cpu.fault_word, reason, s->cpu.r[1][15], s->cpu.r[1][14], s->cpu.r[1][3]);
    record_event(s, "dsp_stop", 0, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                 s->cpu.fault_word, 0);
    if (!s->scheduler.mode || !s->scheduler.pending || s->dsp_halted ||
        !(s->scheduler.slice_id % 256)) capture_checkpoint(s, reason);
}

static void execute_dsp(NxsHpi *s, unsigned quota)
{
    if (!s->dsp_started || s->dsp_halted || s->dsp_running) return;
    int64_t entered_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    s->dsp_running = true;
    const char *reason = s->scheduler.mode ? "deferred slice boundary" :
                                           "phase budget exhausted";
    unsigned steps = 0;
    while (steps < quota) {
        deliver_edma_notifications(s);
        if (!cdj_c674x_interrupt(
                &s->cpu, cdj_c6747_intc_cpu_pending(&s->intc_delivery))) {
            reason = s->cpu.fault ? s->cpu.fault : "CPU interrupt stopped";
            s->dsp_halted = true;
            break;
        }
        if (!cdj_c674x_step(&s->cpu, dsp_read, dsp_write, s)) {
            reason = s->cpu.fault ? s->cpu.fault : "CPU stopped";
            s->dsp_halted = true;
            break;
        }
        ++steps;
        if (s->spi_transfer.fault) {
            s->cpu.fault = "unsupported SPI transfer clock or state";
            s->cpu.fault_pc = s->cpu.pc;
            reason = s->cpu.fault;
            s->dsp_halted = true;
            break;
        }
        cdj_c6747_psc_tick(&s->psc);
        if (!functional_audio_tick(s)) {
            reason = s->cpu.fault;
            s->dsp_halted = true;
            break;
        }
        if (s->hpi.hint) { reason = "HINT host-event yield"; break; }
    }
    s->dsp_running = false;
    if (s->scheduler.mode) {
        if (!cdj_dsp_scheduler_end(&s->scheduler, steps, s->hpi.hint,
                                   s->dsp_halted)) {
            s->cpu.fault = "invalid deferred DSP slice completion";
            s->cpu.fault_pc = s->cpu.pc;
            s->dsp_halted = true;
            reason = s->cpu.fault;
        }
        record_event(s, "dsp_slice_end", s->scheduler.activation_id,
                     s->scheduler.slice_id, s->scheduler.remaining, steps);
    }
    int64_t executed_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    report_dsp(s, reason);
    info_report("nxs-dsp-host-time: execution-ns=%" PRId64
                " reporting-ns=%" PRId64,
                executed_ns - entered_ns,
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - executed_ns);
}

static void deferred_dsp_tick(void *opaque)
{
    NxsHpi *s = opaque;
    if (!s->dsp_started || s->dsp_halted || !s->scheduler.pending) return;
    unsigned quota = cdj_dsp_scheduler_begin(&s->scheduler);
    if (!quota) {
        s->cpu.fault = "invalid deferred DSP scheduler state";
        s->cpu.fault_pc = s->cpu.pc;
        s->dsp_halted = true;
        report_dsp(s, s->cpu.fault);
        return;
    }
    record_event(s, "dsp_slice_begin", s->scheduler.activation_id,
                 s->scheduler.slice_id, s->scheduler.remaining, quota);
    execute_dsp(s, quota);
    /* Future relative to callback completion: never catch up in this timer
     * dispatch pass. This is a host fairness policy, not a DSP clock ratio. */
    if (s->scheduler.pending && !s->dsp_halted)
        timer_mod(s->dsp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
}

static void run_dsp(NxsHpi *s)
{
    if (!s->dsp_started || s->dsp_halted || s->dsp_running) return;
    if (!s->scheduler.mode) {
        execute_dsp(s, CDJ_DSP_COOPERATIVE_BUDGET);
        return;
    }
    if (!cdj_dsp_scheduler_request(&s->scheduler)) {
        s->cpu.fault = "invalid deferred DSP scheduling request";
        s->cpu.fault_pc = s->cpu.pc;
        s->dsp_halted = true;
        report_dsp(s, s->cpu.fault);
        return;
    }
    record_event(s, "dsp_schedule", s->scheduler.activation_id,
                 s->scheduler.slice_id, s->scheduler.remaining,
                 s->scheduler.pending | (s->scheduler.rearm << 1));
    if (!timer_pending(s->dsp_timer))
        timer_mod(s->dsp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
}

static void start_dsp(NxsHpi *s)
{
    /* Boot-ROM handoff abstraction: the host supplies the entry in L2[0].
     * No claim to execute the unavailable ROM. The uploaded code is decoded. */
    cdj_c674x_reset(&s->cpu, ldl_le_p(s->l2));
    s->cpu.cycle_tick = dsp_cycle_tick;
    s->cpu.cycle_opaque = s;
    s->dsp_started = true;
    record_event(s, "dsp_start", 0, s->cpu.pc, 0, 0);
    capture_checkpoint(s, "DSP start boundary");
    run_dsp(s);
    info_report("nxs-pll: oscin-cycles=%" PRIu64 " reset-age=%u lock-wait-remaining=%u early-enable=%d",
                s->pll.oscin_cycles, s->pll.reset_age, s->pll.lock_wait_remaining,
                s->pll.early_enable);
    info_report("nxs-syscfg: cfgchip=%#x,%#x,%#x,%#x amute-clear-pulses=%#x",
                s->syscfg.cfgchip[0], s->syscfg.cfgchip[1], s->syscfg.cfgchip[2],
                s->syscfg.cfgchip[3], s->syscfg.amute_clear_pulses);
}

static uint64_t hpi_read(void *opaque, hwaddr offset, unsigned size)
{
    NxsHpi *s = opaque;
    capture_requested_checkpoint(s);
    uint32_t result = 0xffffffff;
    uint32_t address = s->address;
    bool data_access = offset == 0x80000 || offset == 0xc0000;
    bool data_valid = data_access && valid_data(s);
    if (offset == 0) result = cdj_c6747_hpi_host_read(&s->hpi);
    else if (offset == 0x40000) result = s->address;
    if (data_valid) {
        result = ldl_le_p(host_memory(s, s->address));
        if (offset == 0x80000) s->address += 4;
    } else if (offset != 0 && offset != 0x40000) {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported read offset=%" HWADDR_PRIx " address=%#x HWOB=%d reset=%d\n",
                      offset, s->address, s->hpi.hwob, s->hpi.hpirst);
    }
    /* HPIC polling is DSP-to-MAIN observation, not an external stimulus, and
     * produced hundreds of thousands of redundant records. Data-port reads do
     * advance HPIA and therefore remain ordered state-changing events. */
    if (data_valid)
        record_event(s, "hpi_host_data_read", offset, address, result, size);
    return result;
}

static void hpi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NxsHpi *s = opaque;
    uint32_t address = s->address;
    if (offset == 0) {
        bool old_hint = s->hpi.hint, old_dspint = s->hpi.dspint;
        cdj_c6747_hpi_host_write(&s->hpi, value);
        bool dspint_rising = !old_dspint && s->hpi.dspint;
        if (dspint_rising)
            cdj_c6747_intc_deliver_event(&s->intc, &s->intc_delivery, 34);
        record_event(s, "hpi_host_control_write", offset, address, value, size);
        if (old_hint != s->hpi.hint && s->hint) s->hint(s->opaque, !s->hpi.hint);
        if (dspint_rising && !s->dsp_started) {
            info_report("nxs-hpi: DSPINT after %" PRIu64 " written words; starting partial C674x interpreter", s->words);
            const char *path = getenv("CDJ_NXS_HPI_DUMP");
            if (path && *path) {
                FILE *file = fopen(path, "wb");
                if (!file) error_report("nxs-hpi: cannot open L2 dump %s", path);
                else {
                    size_t count = fwrite(s->l2, 1, sizeof(s->l2), file);
                    int status = fclose(file);
                    if (count != sizeof(s->l2) || status) error_report("nxs-hpi: incomplete L2 dump");
                }
            }
            start_dsp(s);
        } else if (old_hint && !s->hpi.hint) {
            run_dsp(s);
        } else if (dspint_rising && s->dsp_started) {
            run_dsp(s);
        }
        return;
    }
    if (offset == 0x40000) {
        s->address = value;
        record_event(s, "hpi_host_address_write", offset, address, value, size);
        return;
    }
    if ((offset == 0x80000 || offset == 0xc0000) && valid_data(s)) {
        stl_le_p(host_memory(s, s->address), value);
        ++s->words;
        if (offset == 0x80000) s->address += 4;
        record_event(s, offset == 0x80000 ? "hpi_host_data_autoincrement_write" :
                     "hpi_host_data_fixed_write", offset, address, value, size);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported write offset=%" HWADDR_PRIx " address=%#x HWOB=%d reset=%d\n",
                      offset, s->address, s->hpi.hwob, s->hpi.hpirst);
    }
}

static const MemoryRegionOps hpi_ops = {
    .read = hpi_read, .write = hpi_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void cdj_nxs_hpi_init(MemoryRegion *system, void (*hint)(void *, bool), void *opaque)
{
    NxsHpi *s = g_new0(NxsHpi, 1);
    const char *timing = getenv("CDJ_NXS_DSP_FUNCTIONAL_TIMING");
    const char *audio = getenv("CDJ_NXS_DSP_FUNCTIONAL_AUDIO");
    const char *scheduler = getenv("CDJ_NXS_DSP_SCHEDULER");
    if (scheduler && !strcmp(scheduler, "deferred-v1")) {
        cdj_dsp_scheduler_reset(&s->scheduler);
        s->dsp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, deferred_dsp_tick, s);
        warn_report("nxs-c674x: deferred-v1 host scheduling diagnostic; not a hardware clock model");
    } else if (scheduler && strcmp(scheduler, "legacy")) {
        error_report("nxs-c674x: unsupported DSP scheduler %s", scheduler);
        exit(EXIT_FAILURE);
    }
    nxs_hpi = s;
    cdj_c674x_loop_set_functional_timing(timing && !strcmp(timing, "1"));
    s->functional_audio = audio && !strcmp(audio, "1");
    const char *tx_path = getenv("CDJ_NXS_DSP_TX_CAPTURE");
    if (tx_path && *tx_path) {
        s->tx_capture_path = g_strdup(tx_path);
        s->tx_capture = fopen(s->tx_capture_path, "wb");
        if (!s->tx_capture) {
            s->tx_capture_failed = true;
            error_report("nxs-hpi: cannot open DSP transmit capture %s", tx_path);
        }
    }
    if (cdj_c674x_loop_functional_timing())
        warn_report("nxs-c674x: functional SPLOOPD timing enabled; run is not cycle-validation evidence");
    if (s->functional_audio)
        warn_report("nxs-c674x: functional McASP slots every %u packets enabled; run is not audio-timing evidence",
                    CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL);
    cdj_c6747_syscfg_reset(&s->syscfg);
    cdj_c6747_syscfg_priority_reset(&s->syscfg_priority);
    cdj_c6747_psc_reset(&s->psc);
    cdj_c6747_mcasp_reset(&s->mcasp);
    cdj_c6747_mcasp_control_reset(&s->mcasp_control);
    cdj_c6747_gpio_reset(&s->gpio);
    cdj_c6747_i2c_reset(&s->i2c);
    cdj_c6747_intc_reset(&s->intc);
    cdj_c6747_intc_delivery_reset(&s->intc_delivery);
    cdj_c6747_timers_reset(s->timers);
    cdj_c6747_spis_reset(s->spis);
    cdj_c6747_spi_transfer_reset(&s->spi_transfer);
    cdj_wm8740_reset(&s->wm8740);
    cdj_c6747_cache_reset(&s->cache);
    cdj_c6747_edma_reset(&s->edma);
    cdj_c6747_pll_reset(&s->pll);
    cdj_c6747_hpi_reset(&s->hpi);
    cdj_c6747_emifb_reset(&s->emifb);
    s->shared_ram = g_malloc0(SHARED_RAM_SIZE);
    s->sdram = g_malloc0(SDRAM_SIZE);
    s->hint = hint;
    s->opaque = opaque;
    if (s->hint) s->hint(s->opaque, true); /* UHPI_HINT is active low and idle high. */
    memory_region_init_io(&s->registers, NULL, &hpi_ops, s, "nxs.uhpi", 0x100000);
    memory_region_add_subregion(system, HPI_BASE, &s->registers);
}
