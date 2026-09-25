/*
 * Pioneer CDJ-2000 audio DSP — the bus surface.
 *
 * MAIN reports `E-7010: DSP DEVICE ERROR` because it looks for an audio DSP and
 * finds nothing.  That is not a GUI fault and not a cosmetic one: MAIN's whole
 * transport layer sits behind the DSP, so without it the time fields, tempo,
 * cue and track loading stay dead.  This file gives the DSP a bus to live on;
 * cdj2000_dsp_model.c decides what it says.
 *
 * Everything below is read off the firmware, not a datasheet.
 *
 * The shared window
 * -----------------
 * 0x1c747a and 0x1c74c0 arm one DMAC channel:
 *
 *     [0xff608080] = SAR      [0xff608084] = DAR
 *     [0xff608088] = len >> 2         <- longwords, not the boot channel's
 *     [0xff60808c] = 0x5430 (to the DSP) or 0x1430 (from it)
 *     [0xff608060] |= 1  (DMAOR DME) ; CHCR |= 1 (DE)
 *     spin until CHCR bit 1 (TE)                        <- 0x1c74b6
 *
 * and 0x1c75e4 clamps every transfer with
 *
 *     room = 0x10000 - (destination - 0xAC0C0000)
 *
 * which fixes the window at 64 KiB starting at 0xAC0C0000, i.e. physical
 * 0x0C0C0000.  It is modelled as plain RAM: the DMA has to be able to copy into
 * it without a special case, and both sides must see the same bytes.
 *
 * The mailbox
 * -----------
 * The top of the window is control, not data.  Three four-line accessors sit
 * next to each other and give the whole doorbell protocol:
 *
 *     0x2b0d6c   return L[0xac0cfffc]     read the answer
 *     0x2b0d72   L[0xac0cffec] = 0        lower the request
 *     0x2b0d7a   L[0xac0cffec] = 1        raise the request
 *
 * Those words are overlaid with MMIO on top of the RAM, because a request the
 * guest writes straight through the CPU is invisible in plain memory and the
 * model would never be told to run.  Their values are mirrored back into the
 * RAM image so the model sees one coherent window.
 *
 * The firmware
 * ------------
 * The DSP has no image of its own; MAIN carries it.  The bring-up at 0x1c77dc
 * reads a 16-byte header at 0xa4001000 — destination window offset in W[+0x0c],
 * a constant 0xfff0 in W[+0x0e] — and downloads the payload from 0xa4001010.
 * There are two records: 0xd3b0 bytes to window+0x1da0 and a second to
 * window+0x7800, which is exactly where the addresses MAIN reads all over the
 * image (0xac0c7ba0, 0xac0c7ccc, 0xac0c8140 …) live.  See
 * tools/cdj_main/dsp_image.py.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "accel/tcg/cpu-loop.h"

#include "cdj2000_dsp.h"

/*
 * The window control register.  Bit 0 is the run bit: 0x1c75c8 writes
 * `[0xAC000000] = (value & 0xFFFFF000) | 1` at the end of the reset sequence.
 * Bit 2 is set by 0x1c745e — which is the *success* arm of the ready poll, not
 * its timeout, so it is a handshake acknowledgement and not an error flag.  A
 * trace of a real bring-up reads 0 -> 4 -> 1 -> 5 -> 1, which is that pair
 * alternating exactly as the two writers predict.
 *
 * The register is read-modify-written, so it has to behave like memory before
 * any of it can mean anything.
 */
#define DSP_CTL_BASE          0x0C000000
#define DSP_CTL_SIZE          0x10
#define DSP_CTL_RUN           0x0001
#define DSP_CTL_ACK           0x0004

/* The mailbox overlay: the last 32 bytes of the window. */
#define DSP_MAILBOX_OFFSET    0xffe0
#define DSP_MAILBOX_SIZE      0x20

/* How often the model is given a chance to advance its own state. */
#define DSP_TICK_NS           (10 * 1000 * 1000)

/*
 * CDJ_DSP_READ_TRACE: who reads what in the window.  The window is RAM, so
 * MAIN's reads of it leave no trace anywhere.  With the switch set, an I/O
 * overlay over the listed ranges -- "LO-HI[,LO-HI...]" as hex window offsets,
 * or +0x7b80..+0x7d00 and +0x8100..+0x81e0 for any other value -- passes
 * every access through to that same RAM and counts the reads by word and by
 * the guest PC that made them (recovered from the translated code, which
 * leaves the CPU state alone; "dma" for a transfer's own copy).  A pair seen
 * for the first time is printed at once, and every second of guest time the
 * counts of that second follow on one census line.  Writes are not counted:
 * CDJ_DSP_TRACE's census has them.  Off by default, because the overlay puts
 * those pages of the window on QEMU's slow path.
 */
#define DSP_READ_WATCH_MAX    8
#define DSP_READ_BY_DMA       0xffffffffu

typedef struct {
    MemoryRegion region;
    void *dsp;                          /* the CdjDspState it belongs to */
    uint32_t offset;                    /* where in the window it starts */
} DspReadWatch;

typedef struct {
    uint32_t offset;                    /* the word read, 4-aligned */
    uint32_t pc;                        /* the reader, or DSP_READ_BY_DMA */
    uint64_t second;                    /* reads in the census second */
    uint64_t total;
} DspReader;

typedef struct {
    MemoryRegion window;
    MemoryRegion mailbox;
    MemoryRegion ctl;

    uint8_t *ram;                       /* the window's host memory */
    uint32_t ctl_value;

    QEMUTimer *tick;
    CdjDspModel *model;

    /* The interrupt line to MAIN (cdj_dsp_event). */
    qemu_irq irq;
    CdjDspPendingFn pending;
    void *pending_opaque;
    bool event_raised;
    uint64_t events;

    /* Reporting, so a run says what the DSP was asked for without a debugger. */
    bool trace;
    uint64_t transfers;
    uint64_t doorbells;

    /* CDJ_DSP_READ_TRACE */
    DspReadWatch read_watch[DSP_READ_WATCH_MAX];
    unsigned read_watches;
    GHashTable *readers;                /* offset << 32 | pc -> DspReader */
    int64_t read_census_ns;
    bool in_transfer;                   /* a DMA is copying through the window */
} CdjDspState;

/* One DSP per machine, and the DMAC has to reach it from the board file. */
static CdjDspState *cdj_dsp;

/*
 * The mailbox words are ordinary window memory that this device merely watches:
 * reads and writes go to the same RAM the model sees, so there is one copy of
 * the truth and the model can answer by writing into the window like anything
 * else.  The overlay exists only so that a store by the guest is *observed* —
 * in plain RAM a request would be invisible and the model would never run.
 */
static uint64_t cdj_dsp_mailbox_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjDspState *dsp = opaque;
    const uint8_t *at = dsp->ram + DSP_MAILBOX_OFFSET + offset;
    uint64_t value;

    switch (size) {
    case 1:  value = *at;            break;
    case 2:  value = lduw_le_p(at);  break;
    default: value = ldl_le_p(at);   break;
    }
    if (dsp->trace) {
        fprintf(stderr, "cdj2000-dsp: mailbox read  +0x%04x = 0x%08x\n",
                (unsigned)(DSP_MAILBOX_OFFSET + offset), (uint32_t)value);
    }
    if (DSP_MAILBOX_OFFSET + (offset & ~3ull) == CDJ_DSP_MAIL_UP && value) {
        cdj_dsp_model_up_seen(dsp->model, dsp->ram, CDJ_DSP_WINDOW_SIZE);
    }
    return value;
}

static void cdj_dsp_mailbox_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    CdjDspState *dsp = opaque;
    uint8_t *at = dsp->ram + DSP_MAILBOX_OFFSET + offset;
    uint32_t previous = ldl_le_p(dsp->ram + DSP_MAILBOX_OFFSET + (offset & ~3ull));

    switch (size) {
    case 1:  *at = value;            break;
    case 2:  stw_le_p(at, value);    break;
    default: stl_le_p(at, value);    break;
    }
    if (dsp->trace) {
        fprintf(stderr, "cdj2000-dsp: mailbox write +0x%04x = 0x%08x\n",
                (unsigned)(DSP_MAILBOX_OFFSET + offset), (uint32_t)value);
    }

    /* A rising edge of the request word is what runs the model. */
    if (DSP_MAILBOX_OFFSET + (offset & ~3ull) == CDJ_DSP_MAIL_REQ
        && !previous
        && ldl_le_p(dsp->ram + CDJ_DSP_MAIL_REQ)) {
        dsp->doorbells++;
        cdj_dsp_model_doorbell(dsp->model, dsp->ram, CDJ_DSP_WINDOW_SIZE);
    }
}

static const MemoryRegionOps cdj_dsp_mailbox_ops = {
    .read = cdj_dsp_mailbox_read,
    .write = cdj_dsp_mailbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t cdj_dsp_ctl_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjDspState *dsp = opaque;

    return offset ? 0 : dsp->ctl_value;
}

static void cdj_dsp_ctl_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    CdjDspState *dsp = opaque;

    if (offset) {
        return;
    }
    if (dsp->trace && (dsp->ctl_value ^ value) & (DSP_CTL_RUN | DSP_CTL_ACK)) {
        fprintf(stderr, "cdj2000-dsp: control 0x%08x -> 0x%08x%s%s\n",
                dsp->ctl_value, (uint32_t)value,
                (value & DSP_CTL_RUN) ? " run" : "",
                (value & DSP_CTL_ACK) ? " ack" : "");
    }
    if ((value & DSP_CTL_RUN) && !(dsp->ctl_value & DSP_CTL_RUN)) {
        cdj_dsp_model_reset(dsp->model, dsp->ram, CDJ_DSP_WINDOW_SIZE);
    }
    /*
     * The ACK bit is MAIN taking an event: the handler 0x1c09a0 and the poll
     * 0x1c7ce4 both set it after reading the event word.  Any write with the
     * bit set lowers the line -- not only a rising edge, because the bring-up
     * leaves the bit set (0x1c745e) and MAIN's handler ORs it in again.
     */
    if ((value & DSP_CTL_ACK) && dsp->event_raised) {
        dsp->event_raised = false;
        if (dsp->pending) {
            dsp->pending(dsp->pending_opaque, false);
        }
        qemu_set_irq(dsp->irq, 0);
        if (dsp->trace) {
            fprintf(stderr, "cdj2000-dsp: event acknowledged t=%.3f\n",
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
        }
    }
    dsp->ctl_value = value;
}

void cdj_dsp_event(unsigned code)
{
    CdjDspState *dsp = cdj_dsp;
    uint8_t *at;

    if (!dsp) {
        return;
    }
    at = dsp->ram + CDJ_DSP_MAIL_EVENT;
    at[0] = 0;
    at[1] = 0;
    at[2] = (code >> 8) & 0xff;
    at[3] = code & 0xff;
    dsp->events++;
    if (dsp->event_raised && dsp->trace) {
        fprintf(stderr, "cdj2000-dsp: event 0x%04x posted while the previous "
                "one is still pending\n", code);
    }
    dsp->event_raised = true;
    if (dsp->pending) {
        dsp->pending(dsp->pending_opaque, true);
    }
    qemu_set_irq(dsp->irq, 1);
    if (dsp->trace) {
        fprintf(stderr, "cdj2000-dsp: event 0x%04x posted (+0x%04x = %02x %02x "
                "%02x %02x), #%" PRIu64 " t=%.3f\n", code, CDJ_DSP_MAIL_EVENT,
                at[0], at[1], at[2], at[3], dsp->events,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    }
}

static const MemoryRegionOps cdj_dsp_ctl_ops = {
    .read = cdj_dsp_ctl_read,
    .write = cdj_dsp_ctl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* The guest instruction behind the access being served, 0 if it is unknown. */
static uint32_t cdj_dsp_reader_pc(CdjDspState *dsp)
{
    uint64_t data[8] = { 0 };

    if (dsp->in_transfer) {
        return DSP_READ_BY_DMA;
    }
    if (current_cpu
        && cpu_unwind_state_data(current_cpu, current_cpu->mem_io_pc, data)) {
        return (uint32_t)data[0];
    }
    return 0;
}

/* A debugger's access (gdbstub, monitor) is served and not counted. */
static MemTxResult cdj_dsp_watch_read(void *opaque, hwaddr offset,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    DspReadWatch *watch = opaque;
    CdjDspState *dsp = watch->dsp;
    uint32_t at = watch->offset + offset, pc;
    uint64_t value, key;
    DspReader *reader;

    switch (size) {
    case 1:  value = dsp->ram[at];                break;
    case 2:  value = lduw_le_p(dsp->ram + at);    break;
    default: value = ldl_le_p(dsp->ram + at);     break;
    }
    *data = value;
    if (attrs.debug) {
        return MEMTX_OK;
    }
    pc = cdj_dsp_reader_pc(dsp);
    key = (uint64_t)(at & ~3u) << 32 | pc;
    reader = g_hash_table_lookup(dsp->readers, GSIZE_TO_POINTER(key));
    if (!reader) {
        reader = g_new0(DspReader, 1);
        reader->offset = at & ~3u;
        reader->pc = pc;
        g_hash_table_insert(dsp->readers, GSIZE_TO_POINTER(key), reader);
        if (pc == DSP_READ_BY_DMA) {
            fprintf(stderr, "cdj2000-dsp: new reader +0x%04x by dma "
                    "(%u bytes) = 0x%0*" PRIx64 " t=%.3f\n", at, size,
                    size * 2, value, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
        } else {
            fprintf(stderr, "cdj2000-dsp: new reader +0x%04x by pc 0x%08x "
                    "(%u bytes) = 0x%0*" PRIx64 " t=%.3f\n", at, pc, size,
                    size * 2, value, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
        }
    }
    reader->second++;
    reader->total++;
    return MEMTX_OK;
}

static MemTxResult cdj_dsp_watch_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size,
                                       MemTxAttrs attrs)
{
    DspReadWatch *watch = opaque;
    CdjDspState *dsp = watch->dsp;
    uint8_t *at = dsp->ram + watch->offset + offset;

    switch (size) {
    case 1:  *at = value;            break;
    case 2:  stw_le_p(at, value);    break;
    default: stl_le_p(at, value);    break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps cdj_dsp_watch_ops = {
    .read_with_attrs = cdj_dsp_watch_read,
    .write_with_attrs = cdj_dsp_watch_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    /*
     * An eight-byte access (fmov.d with FPSCR.SZ = 1) must not be refused
     * where the plain RAM takes it: QEMU splits it into two words.
     */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static gint cdj_dsp_reader_order(gconstpointer a, gconstpointer b)
{
    const DspReader *x = *(DspReader *const *)a, *y = *(DspReader *const *)b;

    if (x->offset != y->offset) {
        return x->offset < y->offset ? -1 : 1;
    }
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}

/* One line per second of guest time: +OFFSET@PC=READS for that second. */
static void cdj_dsp_read_census(CdjDspState *dsp, int64_t now)
{
    g_autoptr(GPtrArray) rows = g_ptr_array_new();
    GHashTableIter iter;
    gpointer value;
    unsigned i;

    g_hash_table_iter_init(&iter, dsp->readers);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (((DspReader *)value)->second) {
            g_ptr_array_add(rows, value);
        }
    }
    if (!rows->len) {
        return;
    }
    g_ptr_array_sort(rows, cdj_dsp_reader_order);
    fprintf(stderr, "cdj2000-dsp: reads t=%.3f:", now / 1e9);
    for (i = 0; i < rows->len; i++) {
        DspReader *reader = g_ptr_array_index(rows, i);

        if (reader->pc == DSP_READ_BY_DMA) {
            fprintf(stderr, " +0x%04x@dma=%" PRIu64, reader->offset,
                    reader->second);
        } else {
            fprintf(stderr, " +0x%04x@%08x=%" PRIu64, reader->offset,
                    reader->pc, reader->second);
        }
        reader->second = 0;
    }
    fprintf(stderr, "\n");
}

/* CDJ_DSP_READ_TRACE's overlays, over the ranges SPEC lists. */
static void cdj_dsp_read_trace_init(CdjDspState *dsp, MemoryRegion *system,
                                    const char *spec)
{
    static const char defaults[] = "7b80-7d00,8100-81e0";
    g_auto(GStrv) ranges = NULL;
    unsigned i;

    if (!*spec || !strcmp(spec, "1")) {
        spec = defaults;
    }
    ranges = g_strsplit(spec, ",", -1);
    dsp->readers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, g_free);
    for (i = 0; ranges[i] && dsp->read_watches < DSP_READ_WATCH_MAX; i++) {
        DspReadWatch *watch = &dsp->read_watch[dsp->read_watches];
        char *end, *name;
        unsigned long lo = strtoul(ranges[i], &end, 16), hi;

        hi = *end == '-' ? strtoul(end + 1, &end, 16) : 0;
        lo &= ~3ul;
        hi = (hi + 3) & ~3ul;
        if (*end || hi <= lo || hi > DSP_MAILBOX_OFFSET) {
            fprintf(stderr, "cdj2000-dsp: CDJ_DSP_READ_TRACE: range \"%s\" is "
                    "not LO-HI inside +0x0000..+0x%04x, skipped\n",
                    ranges[i], DSP_MAILBOX_OFFSET);
            continue;
        }
        watch->dsp = dsp;
        watch->offset = lo;
        name = g_strdup_printf("cdj2000.dsp-read-watch-%04lx", lo);
        memory_region_init_io(&watch->region, NULL, &cdj_dsp_watch_ops, watch,
                              name, hi - lo);
        g_free(name);
        memory_region_add_subregion_overlap(system, CDJ_DSP_WINDOW_BASE + lo,
                                            &watch->region, 1);
        dsp->read_watches++;
        fprintf(stderr, "cdj2000-dsp: read trace over +0x%04lx..+0x%04lx\n",
                lo, hi);
    }
}

static void cdj_dsp_tick(void *opaque)
{
    CdjDspState *dsp = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    cdj_dsp_model_tick(dsp->model, dsp->ram, CDJ_DSP_WINDOW_SIZE);
    if (dsp->read_watches && now - dsp->read_census_ns >= NANOSECONDS_PER_SECOND) {
        cdj_dsp_read_census(dsp, now);
        dsp->read_census_ns = now;
    }
    timer_mod(dsp->tick, now + DSP_TICK_NS);
}

bool cdj_dsp_is_window(hwaddr address)
{
    return address >= CDJ_DSP_WINDOW_BASE
        && address < CDJ_DSP_WINDOW_BASE + CDJ_DSP_WINDOW_SIZE;
}

void cdj_dsp_transfer_start(void)
{
    /*
     * Nothing to do yet beyond counting: the busy bit the firmware polls lives
     * in the 0xfff10000 register file, which the board file owns, and it is
     * driven from there.  The hook exists so a model that needs to see a
     * transfer begin — one that streams, rather than answering in place — has
     * somewhere to hang.
     */
    if (cdj_dsp) {
        cdj_dsp->transfers++;
        cdj_dsp->in_transfer = true;
    }
}

void cdj_dsp_transfer_done(hwaddr source, hwaddr destination, unsigned bytes)
{
    CdjDspState *dsp = cdj_dsp;

    if (!dsp) {
        return;
    }
    dsp->in_transfer = false;
    if (dsp->trace) {
        fprintf(stderr, "cdj2000-dsp: dma %#" HWADDR_PRIx " -> %#" HWADDR_PRIx
                " (%u bytes)%s\n", source, destination, bytes,
                cdj_dsp_is_window(destination) ? " into the window" : " out of it");
    }
    if (cdj_dsp_is_window(destination)) {
        cdj_dsp_model_firmware(dsp->model, dsp->ram, CDJ_DSP_WINDOW_SIZE,
                               destination - CDJ_DSP_WINDOW_BASE, bytes);
    }
}

void cdj_dsp_init(MemoryRegion *system, Chardev *external, qemu_irq irq,
                  CdjDspPendingFn pending, void *pending_opaque)
{
    CdjDspState *dsp = g_new0(CdjDspState, 1);

    dsp->trace = getenv("CDJ_DSP_TRACE") != NULL;
    dsp->irq = irq;
    dsp->pending = pending;
    dsp->pending_opaque = pending_opaque;

    memory_region_init_ram(&dsp->window, NULL, "cdj2000.dsp-window",
                           CDJ_DSP_WINDOW_SIZE, &error_fatal);
    memory_region_add_subregion(system, CDJ_DSP_WINDOW_BASE, &dsp->window);
    dsp->ram = memory_region_get_ram_ptr(&dsp->window);

    /*
     * Higher priority than the RAM underneath, so the guest's stores to the
     * request word reach this device instead of vanishing into memory.
     */
    memory_region_init_io(&dsp->mailbox, NULL, &cdj_dsp_mailbox_ops, dsp,
                          "cdj2000.dsp-mailbox", DSP_MAILBOX_SIZE);
    memory_region_add_subregion_overlap(system,
                                        CDJ_DSP_WINDOW_BASE + DSP_MAILBOX_OFFSET,
                                        &dsp->mailbox, 1);

    memory_region_init_io(&dsp->ctl, NULL, &cdj_dsp_ctl_ops, dsp,
                          "cdj2000.dsp-ctl", DSP_CTL_SIZE);
    memory_region_add_subregion(system, DSP_CTL_BASE, &dsp->ctl);

    if (getenv("CDJ_DSP_READ_TRACE")) {
        cdj_dsp_read_trace_init(dsp, system, getenv("CDJ_DSP_READ_TRACE"));
    }

    /* The model is created after the window, because reset writes into it. */
    dsp->model = cdj_dsp_model_new(external);
    cdj_dsp_model_reset(dsp->model, dsp->ram, CDJ_DSP_WINDOW_SIZE);

    dsp->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_dsp_tick, dsp);
    timer_mod(dsp->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DSP_TICK_NS);

    cdj_dsp = dsp;
}
