#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdj_dsp_checkpoint.h"

static uint8_t l2[CDJ_DSP_L2_SIZE], restored_l2[CDJ_DSP_L2_SIZE];
static uint8_t shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t restored_shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t l1d[CDJ_DSP_L1D_SIZE], restored_l1d[CDJ_DSP_L1D_SIZE];
static uint8_t sdram[CDJ_DSP_SDRAM_SIZE], restored_sdram[CDJ_DSP_SDRAM_SIZE];

static void unused_tick(void *opaque) { (void)opaque; }

#define CHECKPOINT_COMPONENTS 9u
typedef struct {
    char magic[8];
    uint32_t schema, endian, header_size, state_size;
    uint32_t component_size[CHECKPOINT_COMPONENTS];
    uint32_t l2_size, sdram_size, page_size, page_count, present_pages;
    uint64_t payload_size, payload_checksum;
} TestCheckpointHeader;

static uint64_t test_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void downgrade_to_schema11(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size && fclose(file) == 0);
    TestCheckpointHeader *header = (TestCheckpointHeader *)bytes;
    assert(header->schema == 12 && header->state_size == sizeof(CdjDspCheckpointState));
    size_t l1d_offset = sizeof(*header) + header->state_size +
                        CDJ_DSP_L2_SIZE + CDJ_DSP_SHARED_RAM_SIZE;
    memmove(bytes + l1d_offset, bytes + l1d_offset + CDJ_DSP_L1D_SIZE,
            size - l1d_offset - CDJ_DSP_L1D_SIZE);
    memcpy(header->magic, "CDJDSP11", sizeof(header->magic));
    header->schema = 11;
    header->payload_size -= CDJ_DSP_L1D_SIZE;
    size -= CDJ_DSP_L1D_SIZE;
    header->payload_checksum = test_checksum(bytes + sizeof(*header),
                                             header->payload_size);
    file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, size, file) == size &&
           fflush(file) == 0 && fclose(file) == 0);
    free(bytes);
}

static void downgrade_to_schema10(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size && fclose(file) == 0);

    TestCheckpointHeader *header = (TestCheckpointHeader *)bytes;
    assert(header->header_size == sizeof(*header) &&
           header->schema == 11 &&
           header->state_size == sizeof(CdjDspCheckpointState) &&
           header->payload_size == size - sizeof(*header));
    size_t alignment = _Alignof(CdjDspCheckpointState);
    size_t schema10_state_size = (offsetof(CdjDspCheckpointState, scheduler) +
                                  alignment - 1) / alignment * alignment;
    assert(schema10_state_size <= header->state_size);
    size_t removed = header->state_size - schema10_state_size;
    memmove(bytes + sizeof(*header) + schema10_state_size,
            bytes + sizeof(*header) + header->state_size,
            header->payload_size - header->state_size);
    memcpy(header->magic, "CDJDSP10", sizeof(header->magic));
    header->schema = 10;
    header->state_size = schema10_state_size;
    header->component_size[8] -= sizeof(CdjDspScheduler);
    header->payload_size -= removed;
    header->payload_checksum = test_checksum(bytes + sizeof(*header),
                                             header->payload_size);
    size -= removed;

    file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, size, file) == size &&
           fflush(file) == 0 && fclose(file) == 0);
    free(bytes);
}

static void downgrade_schema10_to_schema9(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size && fclose(file) == 0);

    TestCheckpointHeader *header = (TestCheckpointHeader *)bytes;
    size_t alignment = _Alignof(CdjDspCheckpointState);
    size_t schema10_state_size = (offsetof(CdjDspCheckpointState, scheduler) +
                                  alignment - 1) / alignment * alignment;
    assert(header->header_size == sizeof(*header) &&
           header->schema == 10 && header->state_size == schema10_state_size &&
           header->payload_size == size - sizeof(*header));
    size_t schema9_state_size = (offsetof(CdjDspCheckpointState, spi_transfer) +
                                 alignment - 1) / alignment * alignment;
    assert(schema9_state_size <= header->state_size);
    size_t removed = header->state_size - schema9_state_size;
    memmove(bytes + sizeof(*header) + schema9_state_size,
            bytes + sizeof(*header) + header->state_size,
            header->payload_size - header->state_size);
    memcpy(header->magic, "CDJDSP9\0", sizeof(header->magic));
    header->schema = 9;
    header->state_size = schema9_state_size;
    header->component_size[8] -= sizeof(CdjC6747SpiTransfer);
    header->payload_size -= removed;
    header->payload_checksum = test_checksum(bytes + sizeof(*header),
                                             header->payload_size);
    size -= removed;

    file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, size, file) == size &&
           fflush(file) == 0 && fclose(file) == 0);
    free(bytes);
}

static void assert_schema8_peripheral_tail_preserved(
    const CdjDspCheckpointState *state)
{
    assert(state->mcasp_control.gblctl[1] == 0x1f00 &&
           state->mcasp_control.xfmt[1] == 0xf2 &&
           state->mcasp_control.srctl[1][3] == 1 &&
           state->mcasp_control.xrdy[1] == (1u << 3));
    assert(state->edma.drae[1] == 0x28 &&
           state->edma.param[3][0] == 0x00103200 &&
           state->edma.param[3][5] == 0x00024400 &&
           state->edma.irq_notifications == (1u << 2));
    assert(state->syscfg_priority.mstpri[0] == 0x44442122 &&
           state->syscfg_priority.mstpri[1] == 0x44442000 &&
           state->syscfg_priority.mstpri[2] == 0x54604404);
    assert(state->intc_delivery.cpu_request == (1u << 8));
}

/* Deterministic pattern with no zero byte, so a field the writer drops or the
 * reader defaults cannot restore equal by accident. */
static void fill_pattern(void *data, size_t size, uint32_t seed)
{
    uint8_t *bytes = data;
    uint32_t state = seed | 1u;
    for (size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = (uint8_t)((state >> 24) | 1u);
    }
}

/* Every byte of the checkpoint state carries the pattern above, except where
 * cdj_dsp_checkpoint_write's own validity rules constrain the value.  Those are
 * narrowed to a legal value that is still non-zero and still distinct from a
 * reset, never relaxed.  The six peripheral structures with their own
 * *_valid() predicates are reset and then given distinct values instead of a
 * pattern, because a patterned value is not a legal state for them; the
 * schema-11 round trip below asserts their fields individually.
 *
 * The state is written with one fwrite and read with one fread, so the
 * bit-identical comparison is what makes a dropped field impossible to miss:
 * there is no per-field code to audit, only whole-struct coverage. */
static void exhaustive_round_trip(const char *path)
{
    static CdjDspCheckpointState before, after;
    fill_pattern(&before, sizeof(before), 0x9e3779b9u);

    before.boot_phase = 5;
    before.reset_released = before.dsp_started = before.dsp_halted = 1;
    before.cpu.branch_count = 5;
    before.cpu.store_count = 24;
    before.cpu.load_count = 40;
    before.cpu.loop.length = 48;
    /* A patterned byte is not a valid _Bool object representation, and these
     * are the only _Bool members of the patterned components. */
    before.cpu.loop_active = true;
    before.cpu.loop_pred_invert = true;
    before.cpu.loop.sealed = before.cpu.loop.predicate_loop =
        before.cpu.loop.delayed_count = true;
    for (unsigned i = 0; i < 40; ++i) before.cpu.loads[i].sign_extend = true;
    for (unsigned i = 0; i < 112; ++i) before.cpu.loop_instructions[i].compact = true;
    before.syscfg.unlocked = true;
    before.hpi.hpirst = before.hpi.hwob = before.hpi.dual_hpia =
        before.hpi.hpiasel = before.hpi.dspint = before.hpi.hint = true;
    before.pll.legacy_bit4_used = before.pll.early_enable = true;
    /* prepare() clears these three and derives had_fault from cpu.fault. */
    before.cpu.cycle_tick = unused_tick;
    before.cpu.cycle_opaque = &before;
    before.cpu.fault = "parallel register write conflict";

    before.intc.event_flag[0] = (before.intc.event_flag[0] & ~0xfu) | 0x10u;
    before.intc.event_mask[0] |= 0xfu;
    before.intc.exception_mask[0] |= 0xfu;
    for (unsigned i = 0; i < 3; ++i)
        before.intc.interrupt_mux[i] &= ~0x80808080u;
    before.intc_delivery.cpu_request =
        (before.intc_delivery.cpu_request & 0xfff0u) | 0x10u;

    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i) {
        CdjC6747Timer *timer = &before.timers[i];
        timer->tim34_shadow_valid = 1;
        timer->emumgt = (timer->emumgt & 3u) | 1u;
        timer->gpintgpen = (timer->gpintgpen & 0x00030033u) | 1u;
        timer->gpdatgpdir = (timer->gpdatgpdir & 0x00030003u) | 1u;
        timer->tcr = (timer->tcr & 0x04c03ffeu) | 2u;
        timer->tgcr = (timer->tgcr & 0x0000ff1fu) | 1u;
        timer->wdtcr = (timer->wdtcr & 0xffffc000u) | 0x4000u;
        timer->intctlstat = (timer->intctlstat & 0x000f000fu) | 1u;
    }
    for (unsigned i = 0; i < CDJ_C6747_SPI_COUNT; ++i) {
        CdjC6747Spi *spi = &before.spis[i];
        spi->receive_empty = spi->receive_buffer_full = 1;
        spi->gcr0 = 1;
        spi->gcr1 = (spi->gcr1 & 0x01010100u) | 3u;
        spi->interrupt_enable = (spi->interrupt_enable & 0x0101035fu) | 1u;
        spi->interrupt_level = (spi->interrupt_level & 0x0000035fu) | 1u;
        spi->flags = (spi->flags & 0x0000035fu) | 1u;
        spi->pin_function &= CDJ_C6747_SPI_PIN_MASK;
        spi->pin_direction &= CDJ_C6747_SPI_PIN_MASK;
        spi->pin_input &= CDJ_C6747_SPI_PIN_MASK;
        spi->pin_input_valid &= CDJ_C6747_SPI_PIN_MASK;
        spi->pin_output &= CDJ_C6747_SPI_PIN_MASK;
        spi->dat0 = (spi->dat0 & 0xffffu) | 1u;
        spi->dat1 = (spi->dat1 & 0x1701ffffu) | 1u;
        spi->receive_data = (spi->receive_data & 0xffffu) | 1u;
        spi->receive_status = (spi->receive_status & 0x5f000000u) | 0x01000000u;
        spi->chip_select_default = (spi->chip_select_default & 0xffu) | 1u;
        for (unsigned format = 0; format < 4; ++format)
            spi->format[format] = (spi->format[format] & 0x3ff7ff1fu) | 1u;
    }
    before.cache.l2cfg = (before.cache.l2cfg & 0xfu) | 1u;
    before.cache.l1pcfg = (before.cache.l1pcfg & 7u) | 1u;
    before.cache.l1pcc = (before.cache.l1pcc & 0x00010001u) | 1u;
    before.cache.l1dcfg = (before.cache.l1dcfg & 7u) | 1u;
    before.cache.l1dcc = (before.cache.l1dcc & 0x00010001u) | 1u;
    for (unsigned i = 0; i < 256; ++i) before.cache.mar[i] &= 1u;
    before.cache.mar[0] = before.cache.mar[255] = 1;

    cdj_c6747_mcasp_control_reset(&before.mcasp_control);
    before.mcasp_control.gblctl[2] = 0x1f00;
    before.mcasp_control.xslot[2] = 7;
    cdj_c6747_edma_reset(&before.edma);
    before.edma.drae[3] = 0x28;
    before.edma.transfer_requests = 9;
    cdj_c6747_syscfg_priority_reset(&before.syscfg_priority);
    before.syscfg_priority.mstpri[2] = 0x54604404;
    cdj_wm8740_reset(&before.wm8740);
    before.wm8740.program[2] = 0x1ff;
    before.wm8740.transfers = 3;
    cdj_c6747_spi_transfer_reset(&before.spi_transfer);
    before.spi_transfer.clock_phase = 0x1234;
    cdj_dsp_scheduler_reset(&before.scheduler);
    assert(cdj_dsp_scheduler_request(&before.scheduler));

    cdj_dsp_checkpoint_prepare(&before, "fault");
    /* prepare() only NUL-terminates at the end of each string, so the pattern
     * beyond it stays in the comparison.  The final byte is the one the writer
     * requires to be NUL. */
    before.stop_reason[sizeof(before.stop_reason) - 1] = '\0';
    before.fault[sizeof(before.fault) - 1] = '\0';
    assert(before.had_fault == 1);

    fill_pattern(l2, sizeof(l2), 0x12345678u);
    fill_pattern(shared_ram, sizeof(shared_ram), 0x87654321u);
    fill_pattern(l1d, sizeof(l1d), 0x31415926u);
    /* A fully patterned SDRAM makes every sparse page present; pattern two
     * pages and leave the rest zero so the bitmap is exercised both ways. */
    memset(sdram, 0, sizeof(sdram));
    fill_pattern(sdram, CDJ_DSP_CHECKPOINT_PAGE_SIZE, 0xa5a5a5a5u);
    fill_pattern(sdram + sizeof(sdram) - CDJ_DSP_CHECKPOINT_PAGE_SIZE,
                 CDJ_DSP_CHECKPOINT_PAGE_SIZE, 0x5a5a5a5au);

    char error[160] = {0};
    assert(cdj_dsp_checkpoint_write_with_l1d(
        path, &before, l2, sizeof(l2), shared_ram, sizeof(shared_ram),
        l1d, sizeof(l1d), sdram, sizeof(sdram), error, sizeof(error)));
    memset(&after, 0x3c, sizeof(after));
    assert(cdj_dsp_checkpoint_read_with_l1d(
        path, &after, restored_l2, sizeof(restored_l2), restored_shared_ram,
        sizeof(restored_shared_ram), restored_l1d, sizeof(restored_l1d),
        restored_sdram, sizeof(restored_sdram), error, sizeof(error)));
    /* One comparison over every byte, padding included. */
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    /* TSCL enable origin and the TSCH snapshot intentionally reuse existing
     * ABI fields, so schema 12 round-trips timestamp state without a layout
     * or migration change. */
    assert(after.cpu.control_ready[16] == before.cpu.control_ready[16] &&
           after.cpu.control[16] == before.cpu.control[16] &&
           after.cpu.control_ready[16] != 0u);
    assert(memcmp(l2, restored_l2, sizeof(l2)) == 0);
    assert(memcmp(shared_ram, restored_shared_ram, sizeof(shared_ram)) == 0);
    assert(memcmp(l1d, restored_l1d, sizeof(l1d)) == 0);
    assert(memcmp(sdram, restored_sdram, sizeof(sdram)) == 0);

    /* The comparison only means something if the pattern really reached the
     * regions a field could hide in, so spot-check the extremes of the CPU's
     * large arrays and of the components with no validity rules at all. */
    assert(after.cpu.r[0][0] && after.cpu.r[1][31] && after.cpu.control[31] &&
           after.cpu.control_ready[31] && after.cpu.loads[39].due &&
           after.cpu.stores[23].value && after.cpu.loop.tags[47][7] &&
           after.cpu.loop_instructions[111].word && after.cpu.loop.end_cycle);
    assert(after.syscfg.pinmux[19] && after.psc.target[1][31] &&
           after.mcasp.pdir[2] && after.gpio.falling[3] && after.i2c.output[1] &&
           after.pll.oscin_cycles && after.emifb.bprio && after.hpi.hint &&
           after.hpi_address && after.words && after.event_sequence &&
           after.checkpoint_sequence && after.timers[1].compare[7] &&
           after.spis[1].format[3]);
    /*
     * How much of the state this round trip ACTUALLY exercises.
     *
     * "One comparison over every byte" is true and is not the same as "every
     * byte carries a distinguishing value".  The validators force large parts of
     * the peripheral tail to zero - mcasp_control, edma, wm8740, spi_transfer,
     * syscfg_priority and scheduler between them are mostly zero at write time -
     * and a byte that is zero before and zero after would still compare equal if
     * the serialiser dropped it.  So in that region this test proves the
     * round trip is consistent, not that it is complete.
     *
     * That matters because there ARE genuine per-field copy sites a field can be
     * dropped from: capture_checkpoint() in emulator/qemu/cdj2000_nxs_hpi.c and
     * capture_devices() in tools/cdj_dsp/replay.c copy member by member, unlike
     * the whole-struct fwrite/fread here.
     *
     * Rather than let the test imply coverage it does not have, measure the
     * shortfall and pin it.  The count falls when someone gives those members
     * distinct legal values; it must never rise, because that would mean a
     * component stopped carrying a pattern it used to carry.
     */
    unsigned zero_bytes = 0;
    const unsigned char *raw = (const unsigned char *)&before;
    for (size_t i = 0; i < sizeof(before); ++i) {
        zero_bytes += raw[i] == 0;
    }
    printf("# round trip: %u of %zu state bytes are zero at write time (%.0f%%)\n",
           zero_bytes, sizeof(before), 100.0 * zero_bytes / sizeof(before));
    /* Measured at the time of writing: 7,147 of 15,808.  Allow the count to fall
     * freely and fail if it grows, so coverage can only improve. */
    assert(zero_bytes <= 7147);

    puts("DSP checkpoint whole-state byte-pattern round trip passed");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    exhaustive_round_trip(argv[1]);
    assert(offsetof(CdjDspCheckpointState, scheduler) +
           sizeof(CdjDspScheduler) == sizeof(CdjDspCheckpointState));
    CdjDspCheckpointState before = {0}, after = {0};
    before.hpi_address = 0x11801234;
    before.boot_phase = 3;
    before.words = 12345;
    before.event_sequence = 456;
    before.checkpoint_sequence = 7;
    before.reset_released = before.dsp_started = before.dsp_halted = 1;
    before.cpu.pc = 0x11804468;
    before.cpu.cycles = 6132079;
    before.cpu.packets = 2599580;
    before.cpu.branch_due = 6132081;
    before.cpu.branch_target = 0x11805500;
    before.cpu.branch_count = 2;
    before.cpu.branch_queue[1].due = 6132082;
    before.cpu.branch_queue[1].target = 0x11806600;
    before.cpu.store_count = 1;
    before.cpu.stores[0].due = 6132080;
    before.cpu.stores[0].address = 0xc0001000;
    before.cpu.stores[0].value = UINT64_C(0x123456789abcdef0);
    before.cpu.stores[0].size = 8;
    before.cpu.load_count = 5;
    before.cpu.loads[4].due = 6132085;
    before.cpu.loads[4].address = 0xc000101f;
    before.cpu.loads[4].size = 8 | (5 << 8);
    before.cpu.loads[4].dst = 10;
    before.cpu.store_count = 2;
    before.cpu.stores[1].due = 6132083;
    before.cpu.stores[1].address = 0xc000103e;
    before.cpu.stores[1].value = 0x12345678;
    before.cpu.stores[1].size = 4 | (5 << 8);
    before.cpu.loads[3].due = 6132080;
    before.cpu.loads[3].address = 0x9;
    before.cpu.loads[3].size = CDJ_C674X_DELAYED_SAT;
    before.cpu.loads[0].due = 6132081;
    before.cpu.loads[0].address = 0x11802000;
    before.cpu.loads[0].bank = 1;
    before.cpu.loads[0].dst = 7;
    before.cpu.loads[0].size = 4;
    before.cpu.loads[0].sign_extend = true;
    before.cpu.loads[1].due = 6132083;
    before.cpu.loads[1].value = 0x4f800000;
    before.cpu.loads[1].address = 1u << 23;
    before.cpu.loads[1].bank = 1;
    before.cpu.loads[1].dst = 9;
    before.cpu.loads[1].size = 0;
    before.cpu.loads[1].sign_extend = true;
    before.cpu.loads[2].due = 6132084;
    before.cpu.loads[2].value = UINT64_C(0xfffffa9ba111462c);
    before.cpu.loads[2].bank = 0;
    before.cpu.loads[2].dst = 8;
    before.cpu.loads[2].size = 16;
    before.cpu.loop_active = true;
    before.cpu.loop_wait = 2;
    before.cpu.loop_tags = 9;
    before.cpu.loop_packets = 5;
    before.cpu.control_ready[31] = UINT64_C(0x8100000711803000);
    before.cpu.loop.ii = 4;
    before.cpu.loop.length = 3;
    before.cpu.loop.iterations = 14;
    before.cpu.loop.cycle = 17;
    before.cpu.loop.tags[2][7] = 91;
    before.cpu.loop.count[2] = 8;
    before.cpu.loop_instructions[91].pc = 0x11803000;
    before.cpu.loop_instructions[91].word = 0xfeedbeef;
    before.cpu.cycle_tick = unused_tick;
    before.cpu.cycle_opaque = &before;
    before.cpu.fault = "instruction not implemented";
    before.cpu.fault_pc = 0x11804468;
    before.cpu.fault_word = 0xc09868c0;
    before.syscfg.cfgchip[1] = 0x18000;
    before.psc.remaining[1][1] = 3;
    before.mcasp.pdir[2] = 0x55aa;
    before.gpio.input[2] = 0x24;
    before.i2c.mode[1] = 0x20;
    before.pll.oscin_cycles = 98765;
    before.hpi.hint = true;
    before.emifb.sdcfg = 0x12345678;
    cdj_c6747_intc_reset(&before.intc);
    assert(cdj_c6747_intc_event(&before.intc, 34));
    cdj_c6747_timers_reset(before.timers);
    before.timers[0].tgcr = 0x17;
    before.timers[0].intctlstat = 1;
    before.timers[1].prd12 = UINT32_MAX;
    cdj_c6747_spis_reset(before.spis);
    before.spis[1].gcr0 = 1;
    before.spis[1].gcr1 = 3;
    before.spis[1].pin_function = 0xe01;
    before.spis[1].format[0] = 0x00021810;
    before.spis[1].delay = 0x02020408;
    cdj_c6747_cache_reset(&before.cache);
    before.cache.l2cfg = 3;
    before.cache.l1pcc = 0x10000;
    before.cache.mar[192] = 1;
    cdj_c6747_mcasp_control_reset(&before.mcasp_control);
    before.mcasp_control.gblctl[1] = 0x1f00;
    before.mcasp_control.xfmt[1] = 0xf2;
    before.mcasp_control.srctl[1][3] = 1;
    before.mcasp_control.xrdy[1] = 1u << 3;
    cdj_c6747_edma_reset(&before.edma);
    before.edma.drae[1] = 0x28;
    before.edma.param[3][0] = 0x00103200;
    before.edma.param[3][5] = 0x00024400;
    before.edma.irq_notifications = 1u << 2;
    cdj_c6747_syscfg_priority_reset(&before.syscfg_priority);
    before.syscfg_priority.mstpri[0] = 0x44442122;
    before.syscfg_priority.mstpri[1] = 0x44442000;
    cdj_c6747_intc_delivery_reset(&before.intc_delivery);
    before.intc_delivery.cpu_request = 1u << 8;
    cdj_wm8740_reset(&before.wm8740);
    before.wm8740.program[0] = 0x1ff;
    before.wm8740.program[1] = 0x1fe;
    before.wm8740.last_word = 0x3fe;
    before.wm8740.transfers = 2;
    before.wm8740.active_attenuation[0] = 0xff;
    before.wm8740.active_attenuation[1] = 0xfe;
    cdj_c6747_spi_transfer_reset(&before.spi_transfer);
    before.spi_transfer.clock_phase = 0;
    before.spi_transfer.half_ticks_remaining = 775;
    before.spi_transfer.active_control = 0x1ff;
    before.spi_transfer.active_format = 0x00021810;
    before.spi_transfer.active_delay = 0x02020408;
    before.spi_transfer.queued_control = 0x3ff;
    before.spi_transfer.queued_format = 0x00021810;
    before.spi_transfer.queued_delay = 0x02020408;
    before.spi_transfer.phase = 2;
    before.spi_transfer.queued_valid = 1;
    before.spi_transfer.tx_full = 1;
    cdj_dsp_scheduler_reset(&before.scheduler);
    assert(cdj_dsp_scheduler_request(&before.scheduler));
    assert(cdj_dsp_scheduler_begin(&before.scheduler) ==
           CDJ_DSP_SCHEDULER_SLICE_STEPS);
    assert(cdj_dsp_scheduler_request(&before.scheduler));
    assert(cdj_dsp_scheduler_end(&before.scheduler,
                                 CDJ_DSP_SCHEDULER_SLICE_STEPS,
                                 false, false));
    l2[0] = 0x68;
    l2[sizeof(l2) - 1] = 0xa5;
    shared_ram[0] = 0x56;
    shared_ram[sizeof(shared_ram) - 1] = 0x78;
    sdram[0x1000] = 0x12;
    sdram[sizeof(sdram) - 1] = 0x34;
    cdj_dsp_checkpoint_prepare(&before, "instruction not implemented");
    assert(before.had_fault && before.cpu.cycle_tick == NULL &&
           before.cpu.cycle_opaque == NULL && before.cpu.fault == NULL);

    char error[160] = {0};
    assert(cdj_dsp_checkpoint_write(argv[1], &before, l2, sizeof(l2),
                                    shared_ram, sizeof(shared_ram),
                                    sdram, sizeof(sdram), error, sizeof(error)));
    assert(cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                   sizeof(restored_l2), restored_shared_ram,
                                   sizeof(restored_shared_ram), restored_sdram,
                                   sizeof(restored_sdram), error, sizeof(error)));
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(memcmp(l2, restored_l2, sizeof(l2)) == 0);
    assert(memcmp(shared_ram, restored_shared_ram, sizeof(shared_ram)) == 0);
    assert(memcmp(sdram, restored_sdram, sizeof(sdram)) == 0);
    assert(after.cpu.store_count == 2 && after.cpu.load_count == 5);
    assert(after.cpu.loads[4].size == (8 | (5 << 8)) &&
           after.cpu.loads[4].address == 0xc000101f && after.cpu.loads[4].dst == 10);
    assert(after.cpu.stores[1].size == (4 | (5 << 8)) &&
           after.cpu.stores[1].address == 0xc000103e && after.cpu.stores[1].value == 0x12345678);
    assert(after.cpu.loads[3].size == CDJ_C674X_DELAYED_SAT &&
           after.cpu.loads[3].address == 0x9 && after.cpu.loads[3].due == 6132080);
    assert(after.cpu.loads[1].size == 0 &&
           after.cpu.loads[1].value == 0x4f800000 &&
           after.cpu.loads[1].address == (1u << 23) &&
           after.cpu.loads[1].sign_extend);
    assert(after.cpu.loads[2].size == 16 && after.cpu.loads[2].dst == 8 &&
           after.cpu.loads[2].value == UINT64_C(0xfffffa9ba111462c));
    assert(after.cpu.loop_active && after.cpu.loop.tags[2][7] == 91);
    assert(after.cpu.loop_instructions[91].word == 0xfeedbeef);
    assert(after.intc.event_flag[1] == 4 &&
           after.intc.exception_mask[0] == UINT32_MAX);
    assert(after.timers[0].tgcr == 0x17 &&
           after.timers[0].intctlstat == 1 &&
           after.timers[1].prd12 == UINT32_MAX);
    assert(after.spis[1].gcr0 == 1 && after.spis[1].gcr1 == 3 &&
           after.spis[1].pin_function == 0xe01 &&
           after.spis[1].format[0] == 0x00021810 &&
           after.spis[1].delay == 0x02020408);
    assert(after.cache.l2cfg == 3 && after.cache.l1pcc == 0x10000 &&
           after.cache.mar[192] == 1);
    assert(after.mcasp_control.gblctl[1] == 0x1f00 &&
           after.mcasp_control.xfmt[1] == 0xf2 &&
           after.mcasp_control.srctl[1][3] == 1 &&
           after.mcasp_control.xrdy[1] == (1u << 3));
    assert(after.edma.drae[1] == 0x28 &&
           after.edma.param[3][0] == 0x00103200 &&
           after.edma.param[3][5] == 0x00024400 &&
           after.edma.irq_notifications == (1u << 2));
    assert(after.syscfg_priority.mstpri[0] == 0x44442122 &&
           after.syscfg_priority.mstpri[1] == 0x44442000 &&
           after.syscfg_priority.mstpri[2] == 0x54604404);
    assert(after.intc_delivery.cpu_request == (1u << 8));
    assert(after.wm8740.program[0] == 0x1ff &&
           after.wm8740.program[1] == 0x1fe &&
           after.wm8740.last_word == 0x3fe &&
           after.wm8740.transfers == 2 &&
           after.wm8740.active_attenuation[0] == 0xff &&
           after.wm8740.active_attenuation[1] == 0xfe);
    assert(after.spi_transfer.half_ticks_remaining == 775 &&
           after.spi_transfer.active_control == 0x1ff &&
           after.spi_transfer.queued_control == 0x3ff &&
           after.spi_transfer.phase == 2 && after.spi_transfer.queued_valid &&
           after.spi_transfer.tx_full &&
           cdj_c6747_spi_transfer_valid(&after.spi_transfer));
    assert(after.scheduler.activation_id == 1 &&
           after.scheduler.slice_id == 1 &&
           after.scheduler.remaining ==
               CDJ_DSP_SCHEDULER_ACTIVATION_STEPS -
               CDJ_DSP_SCHEDULER_SLICE_STEPS &&
           after.scheduler.slice_steps == CDJ_DSP_SCHEDULER_SLICE_STEPS &&
           after.scheduler.pending && after.scheduler.rearm &&
           after.scheduler.mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 &&
           cdj_dsp_scheduler_valid(&after.scheduler));
    CdjDspCheckpointState invalid = before;
    invalid.spi_transfer.reserved[0] = 1;
    assert(!cdj_dsp_checkpoint_write(argv[1], &invalid, l2, sizeof(l2),
                                     shared_ram, sizeof(shared_ram), sdram,
                                     sizeof(sdram), error, sizeof(error)));
    invalid = before;
    invalid.scheduler.reserved = 1;
    assert(!cdj_dsp_checkpoint_write(argv[1], &invalid, l2, sizeof(l2),
                                     shared_ram, sizeof(shared_ram), sdram,
                                     sizeof(sdram), error, sizeof(error)));

    downgrade_to_schema11(argv[1]);
    memset(restored_l1d, 0xa5, sizeof(restored_l1d));
    assert(cdj_dsp_checkpoint_read_with_l1d(
        argv[1], &after, restored_l2, sizeof(restored_l2), restored_shared_ram,
        sizeof(restored_shared_ram), restored_l1d, sizeof(restored_l1d),
        restored_sdram, sizeof(restored_sdram), error, sizeof(error)));
    for (size_t i = 0; i < sizeof(restored_l1d); ++i)
        assert(restored_l1d[i] == 0);
    assert(after.scheduler.mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1);

    downgrade_to_schema10(argv[1]);
    memset(&after, 0xa5, sizeof(after));
    assert(cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                   sizeof(restored_l2), restored_shared_ram,
                                   sizeof(restored_shared_ram), restored_sdram,
                                   sizeof(restored_sdram), error, sizeof(error)));
    CdjDspScheduler legacy_scheduler = {0};
    assert(memcmp(&after.scheduler, &legacy_scheduler,
                  sizeof(legacy_scheduler)) == 0 &&
           after.scheduler.mode == CDJ_DSP_SCHEDULER_MODE_LEGACY &&
           cdj_dsp_scheduler_valid(&after.scheduler));
    assert(after.spi_transfer.half_ticks_remaining == 775 &&
           after.spi_transfer.active_control == 0x1ff &&
           after.spi_transfer.queued_control == 0x3ff &&
           after.spi_transfer.phase == 2 && after.spi_transfer.queued_valid &&
           after.spi_transfer.tx_full &&
           cdj_c6747_spi_transfer_valid(&after.spi_transfer));
    assert(after.wm8740.program[0] == 0x1ff &&
           after.wm8740.program[1] == 0x1fe &&
           after.wm8740.last_word == 0x3fe &&
           after.wm8740.transfers == 2);
    assert_schema8_peripheral_tail_preserved(&after);

    downgrade_schema10_to_schema9(argv[1]);
    memset(&after, 0xa5, sizeof(after));
    assert(cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                   sizeof(restored_l2), restored_shared_ram,
                                   sizeof(restored_shared_ram), restored_sdram,
                                   sizeof(restored_sdram), error, sizeof(error)));
    CdjC6747SpiTransfer reset_transfer;
    cdj_c6747_spi_transfer_reset(&reset_transfer);
    assert(memcmp(&after.spi_transfer, &reset_transfer,
                  sizeof(reset_transfer)) == 0);
    assert(after.wm8740.program[0] == 0x1ff &&
           after.wm8740.program[1] == 0x1fe &&
           after.wm8740.last_word == 0x3fe &&
           after.wm8740.transfers == 2 &&
           after.wm8740.active_attenuation[0] == 0xff &&
           after.wm8740.active_attenuation[1] == 0xfe);
    assert(memcmp(&after.scheduler, &legacy_scheduler,
                  sizeof(legacy_scheduler)) == 0 &&
           cdj_dsp_scheduler_valid(&after.scheduler));
    assert_schema8_peripheral_tail_preserved(&after);
    assert(memcmp(l2, restored_l2, sizeof(l2)) == 0);
    assert(memcmp(shared_ram, restored_shared_ram, sizeof(shared_ram)) == 0);
    assert(memcmp(sdram, restored_sdram, sizeof(sdram)) == 0);

    FILE *file = fopen(argv[1], "r+b");
    assert(file && fputc('X', file) != EOF && fclose(file) == 0);
    assert(!cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                    sizeof(restored_l2), restored_shared_ram,
                                    sizeof(restored_shared_ram), restored_sdram,
                                    sizeof(restored_sdram), error, sizeof(error)));
    puts("DSP checkpoint schema-11 scheduler round trip and schema-10/9 migrations passed");
    return 0;
}
