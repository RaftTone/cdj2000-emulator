/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_dsp_checkpoint.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECKPOINT_ENDIAN 0x01020304u
#define CHECKPOINT_MAGIC "CDJDSP12"
#define CHECKPOINT_SCHEMA1_MAGIC "CDJDSP1\0"
#define CHECKPOINT_SCHEMA2_MAGIC "CDJDSP2\0"
#define CHECKPOINT_SCHEMA3_MAGIC "CDJDSP3\0"
#define CHECKPOINT_SCHEMA4_MAGIC "CDJDSP4\0"
#define CHECKPOINT_SCHEMA5_MAGIC "CDJDSP5\0"
#define CHECKPOINT_SCHEMA6_MAGIC "CDJDSP6\0"
#define CHECKPOINT_SCHEMA7_MAGIC "CDJDSP7\0"
#define CHECKPOINT_SCHEMA8_MAGIC "CDJDSP8\0"
#define CHECKPOINT_SCHEMA9_MAGIC "CDJDSP9\0"
#define CHECKPOINT_SCHEMA10_MAGIC "CDJDSP10"
#define CHECKPOINT_SCHEMA11_MAGIC "CDJDSP11"
#define CHECKPOINT_COMPONENTS 9u

typedef struct {
    char magic[8];
    uint32_t schema, endian, header_size, state_size;
    uint32_t component_size[CHECKPOINT_COMPONENTS];
    uint32_t l2_size, sdram_size, page_size, page_count, present_pages;
    uint64_t payload_size, payload_checksum;
} CheckpointHeader;

static void fail(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
}

static uint64_t checksum(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    sizes[0] = sizeof(CdjC674x);
    sizes[1] = sizeof(CdjC6747Syscfg);
    sizes[2] = sizeof(CdjC6747Psc);
    sizes[3] = sizeof(CdjC6747Mcasp);
    sizes[4] = sizeof(CdjC6747Gpio);
    sizes[5] = sizeof(CdjC6747I2c);
    sizes[6] = sizeof(CdjC6747Pll);
    sizes[7] = sizeof(CdjC6747Hpi);
    /* Preserve the fixed nine-component header used by schemas 1-3. The
     * final component now describes the complete peripheral-state tail. */
    sizes[8] = sizeof(CdjC6747Emifb) + sizeof(CdjC6747Intc) +
               sizeof(CdjC6747Timer) * CDJ_C6747_TIMER_COUNT +
               sizeof(CdjC6747Spi) * CDJ_C6747_SPI_COUNT +
               sizeof(CdjC6747Cache) + sizeof(CdjC6747McaspControl) +
               sizeof(CdjC6747Edma) + sizeof(CdjC6747SyscfgPriority) +
               sizeof(CdjC6747IntcDelivery) + sizeof(CdjWm8740) +
               sizeof(CdjC6747SpiTransfer) + sizeof(CdjDspScheduler);
}

static void schema10_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    component_sizes(sizes);
    sizes[8] -= sizeof(CdjDspScheduler);
}

static void schema9_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema10_component_sizes(sizes);
    sizes[8] -= sizeof(CdjC6747SpiTransfer);
}

static void schema8_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema9_component_sizes(sizes);
    sizes[8] -= sizeof(CdjWm8740);
}

static void schema6_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema8_component_sizes(sizes);
    sizes[8] -= sizeof(CdjC6747McaspControl);
    sizes[8] -= sizeof(CdjC6747Edma);
    sizes[8] -= sizeof(CdjC6747SyscfgPriority);
    sizes[8] -= sizeof(CdjC6747IntcDelivery);
}

/* The schema-7 McASP control structure ended immediately before xbuf.  Every
 * later member was appended, so its exact prefix can be migrated without
 * interpreting or fabricating any captured register state. */
#define SCHEMA7_MCASP_CONTROL_SIZE offsetof(CdjC6747McaspControl, xbuf)

static void schema7_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema6_component_sizes(sizes);
    sizes[8] += SCHEMA7_MCASP_CONTROL_SIZE;
}

static void schema5_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema6_component_sizes(sizes);
    sizes[8] -= sizeof(CdjC6747Cache);
}

static void schema4_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema5_component_sizes(sizes);
    sizes[8] -= sizeof(CdjC6747Spi) * CDJ_C6747_SPI_COUNT;
}

static void schema3_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema4_component_sizes(sizes);
    sizes[8] = sizeof(CdjC6747Emifb) + sizeof(CdjC6747Intc);
}

static void legacy_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema3_component_sizes(sizes);
    sizes[8] = sizeof(CdjC6747Emifb);
}

/* Every size above is derived from the CURRENT structure layout, so growing or
 * reordering any serialised structure silently redefines what an older schema
 * was, and every checkpoint already written under that schema number stops
 * matching its recorded header.  That has happened: 146 checkpoints in runs/
 * carry schema-7/8 headers whose component tail is 1900, 7080, 7092 or 7096
 * bytes where the derivations below now produce 2188 and 7712, and the reader
 * refuses them.  The numbers below are the ones recorded in every loadable
 * run's manifest, so pin them: a layout change now fails the build instead of
 * orphaning checkpoints.  Update one only together with a new schema and its
 * own migration path, never to make a derivation match a changed struct. */
_Static_assert(sizeof(CdjC674x) == 6592, "CdjC674x ABI is checkpoint-bound");
_Static_assert(sizeof(CdjC6747Syscfg) == 112, "component 1 size changed");
_Static_assert(sizeof(CdjC6747Psc) == 784, "component 2 size changed");
_Static_assert(sizeof(CdjC6747Mcasp) == 36, "component 3 size changed");
_Static_assert(sizeof(CdjC6747Gpio) == 84, "component 4 size changed");
_Static_assert(sizeof(CdjC6747I2c) == 32, "component 5 size changed");
_Static_assert(sizeof(CdjC6747Pll) == 144, "component 6 size changed");
_Static_assert(sizeof(CdjC6747Hpi) == 6, "component 7 size changed");
/* Component 8 is one lumped total, so these pin its members individually. */
_Static_assert(sizeof(CdjC6747Emifb) == 28, "component 8 member size changed");
_Static_assert(sizeof(CdjC6747Intc) == 60, "component 8 member size changed");
_Static_assert(sizeof(CdjC6747Timer) * CDJ_C6747_TIMER_COUNT == 200,
               "component 8 member size changed");
_Static_assert(sizeof(CdjC6747Spi) * CDJ_C6747_SPI_COUNT == 168,
               "component 8 member size changed");
_Static_assert(sizeof(CdjC6747Cache) == 1072, "component 8 member size changed");
_Static_assert(sizeof(CdjC6747McaspControl) == 1912,
               "component 8 member size changed");
_Static_assert(sizeof(CdjC6747Edma) == 4256, "component 8 member size changed");
_Static_assert(sizeof(CdjC6747SyscfgPriority) == 12,
               "component 8 member size changed");
_Static_assert(sizeof(CdjC6747IntcDelivery) == 4,
               "component 8 member size changed");
_Static_assert(sizeof(CdjWm8740) == 32, "component 8 member size changed");
_Static_assert(sizeof(CdjC6747SpiTransfer) == 40,
               "component 8 member size changed");
_Static_assert(sizeof(CdjDspScheduler) == 32, "component 8 member size changed");
/* State sizes every recorded schema's header carries.  SCHEMA7_MCASP_CONTROL_SIZE
 * is the one migration constant taken from a member offset rather than a member
 * size; 660 is the value the four loadable schema-7 checkpoints were written
 * with.  (The "Schema-7 state ends above" comment in cdj_c6747_mcasp.h sits
 * before xrsr, which would give 1300 and match no recorded file.) */
_Static_assert(SCHEMA7_MCASP_CONTROL_SIZE == 660, "schema-7 prefix moved");
_Static_assert(sizeof(CdjDspCheckpointState) == 15808, "schema-11 state size");
_Static_assert(_Alignof(CdjDspCheckpointState) == 8, "state alignment changed");
/* The reader rounds each prefix up to the structure's alignment, exactly as
 * below, because appending INTC reused schema-2's trailing padding. */
#define CHECKPOINT_ROUND(bytes) \
    (((bytes) + _Alignof(CdjDspCheckpointState) - 1) / \
     _Alignof(CdjDspCheckpointState) * _Alignof(CdjDspCheckpointState))
#define CHECKPOINT_PREFIX(member) \
    CHECKPOINT_ROUND(offsetof(CdjDspCheckpointState, member))
_Static_assert(CHECKPOINT_PREFIX(intc) == 8024, "schema-1/2 state size");
_Static_assert(CHECKPOINT_PREFIX(timers) == 8080, "schema-3 state size");
_Static_assert(CHECKPOINT_PREFIX(spis) == 8280, "schema-4 state size");
_Static_assert(CHECKPOINT_PREFIX(cache) == 8448, "schema-5 state size");
_Static_assert(CHECKPOINT_PREFIX(mcasp_control) == 9520, "schema-6 state size");
_Static_assert(CHECKPOINT_ROUND(offsetof(CdjDspCheckpointState, mcasp_control) +
                                SCHEMA7_MCASP_CONTROL_SIZE) == 10184,
               "schema-7 state size");
_Static_assert(CHECKPOINT_PREFIX(wm8740) == 15704, "schema-8 state size");
_Static_assert(CHECKPOINT_PREFIX(spi_transfer) == 15736, "schema-9 state size");
_Static_assert(CHECKPOINT_PREFIX(scheduler) == 15776, "schema-10 state size");
#undef CHECKPOINT_PREFIX
#undef CHECKPOINT_ROUND

void cdj_dsp_checkpoint_prepare(CdjDspCheckpointState *state,
                                const char *reason)
{
    const char *fault = state->cpu.fault;
    state->had_fault = fault != NULL;
    snprintf(state->fault, sizeof(state->fault), "%s", fault ? fault : "");
    snprintf(state->stop_reason, sizeof(state->stop_reason), "%s",
             reason ? reason : "");
    state->cpu.cycle_tick = NULL;
    state->cpu.cycle_opaque = NULL;
    state->cpu.fault = NULL;
}

static bool state_valid(const CdjDspCheckpointState *state)
{
    bool timers_valid = true;
    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i) {
        const CdjC6747Timer *timer = &state->timers[i];
        timers_valid = timers_valid && timer->tim34_shadow_valid <= 1 &&
                       !(timer->emumgt & ~3u) &&
                       !(timer->gpintgpen & ~0x00030033u) &&
                       !(timer->gpdatgpdir & ~0x00030003u) &&
                       !(timer->tcr & ~0x04c03ffeu) &&
                       !(timer->tgcr & ~0x0000ff1fu) &&
                       !(timer->wdtcr & ~0xffffc000u) &&
                       !(timer->intctlstat & ~0x000f000fu);
    }
    bool spis_valid = true;
    for (unsigned i = 0; i < CDJ_C6747_SPI_COUNT; ++i) {
        const CdjC6747Spi *spi = &state->spis[i];
        spis_valid = spis_valid && spi->receive_empty <= 1 &&
                     spi->receive_buffer_full <= 1 &&
                     !(spi->gcr0 & ~1u) && !(spi->gcr1 & ~0x01010103u) &&
                     (spi->gcr1 & 3u) != 1 && (spi->gcr1 & 3u) != 2 &&
                     !(spi->interrupt_enable & ~0x0101035fu) &&
                     !(spi->interrupt_level & ~0x0000035fu) &&
                     !(spi->flags & ~0x0000035fu) &&
                     !(spi->pin_function & ~CDJ_C6747_SPI_PIN_MASK) &&
                     !(spi->pin_direction & ~CDJ_C6747_SPI_PIN_MASK) &&
                     !(spi->pin_input & ~CDJ_C6747_SPI_PIN_MASK) &&
                     !(spi->pin_input_valid & ~CDJ_C6747_SPI_PIN_MASK) &&
                     !(spi->pin_output & ~CDJ_C6747_SPI_PIN_MASK) &&
                     !(spi->dat0 & ~0xffffu) && !(spi->dat1 & ~0x1701ffffu) &&
                     !(spi->receive_data & ~0xffffu) &&
                     !(spi->receive_status & ~0x5f000000u) &&
                     !(spi->chip_select_default & ~0xffu);
        for (unsigned format = 0; format < 4; ++format)
            spis_valid = spis_valid && !(spi->format[format] & ~0x3ff7ff1fu);
    }
    bool cache_valid = !(state->cache.l2cfg & ~0xfu) &&
                       !(state->cache.l1pcfg & ~7u) &&
                       !(state->cache.l1pcc & ~0x00010001u) &&
                       !(state->cache.l1dcfg & ~7u) &&
                       !(state->cache.l1dcc & ~0x00010001u);
    for (unsigned i = 0; i < 256; ++i)
        cache_valid = cache_valid && !(state->cache.mar[i] & ~1u);
    return timers_valid && spis_valid && cache_valid &&
           cdj_wm8740_valid(&state->wm8740) &&
           cdj_c6747_spi_transfer_valid(&state->spi_transfer) &&
           cdj_dsp_scheduler_valid(&state->scheduler) &&
           cdj_c6747_mcasp_control_valid(&state->mcasp_control) &&
           cdj_c6747_edma_valid(&state->edma) &&
           cdj_c6747_syscfg_priority_valid(&state->syscfg_priority) &&
           !(state->intc_delivery.cpu_request & ~0xfff0u) &&
           state->boot_phase <= 7 && state->reset_released <= 1 &&
           state->dsp_started <= 1 && state->dsp_halted <= 1 &&
           state->had_fault <= 1 && state->cpu.cycle_tick == NULL &&
           state->cpu.cycle_opaque == NULL && state->cpu.fault == NULL &&
           state->cpu.branch_count <= 5 && state->cpu.store_count <= 24 &&
           state->cpu.load_count <= 40 && state->cpu.loop.length <= 48 &&
           !(state->intc.event_flag[0] & 0xf) &&
           (state->intc.event_mask[0] & 0xf) == 0xf &&
           (state->intc.exception_mask[0] & 0xf) == 0xf &&
           !(state->intc.interrupt_mux[0] & 0x80808080u) &&
           !(state->intc.interrupt_mux[1] & 0x80808080u) &&
           !(state->intc.interrupt_mux[2] & 0x80808080u) &&
           state->stop_reason[sizeof(state->stop_reason) - 1] == '\0' &&
           state->fault[sizeof(state->fault) - 1] == '\0';
}

bool cdj_dsp_checkpoint_write_with_l1d(
    const char *path, const CdjDspCheckpointState *input,
    const uint8_t *l2, size_t l2_size,
    const uint8_t *shared_ram, size_t shared_ram_size,
    const uint8_t *l1d, size_t l1d_size,
    const uint8_t *sdram, size_t sdram_size,
    char *error, size_t error_size)
{
    CdjDspCheckpointState state = *input;
    if (!path || !l2 || !shared_ram || !l1d || !sdram ||
        l2_size != CDJ_DSP_L2_SIZE ||
        shared_ram_size != CDJ_DSP_SHARED_RAM_SIZE ||
        l1d_size != CDJ_DSP_L1D_SIZE ||
        sdram_size != CDJ_DSP_SDRAM_SIZE || !state_valid(&state)) {
        fail(error, error_size, "invalid checkpoint state or memory size");
        return false;
    }
    const uint32_t page_count = sdram_size / CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    const size_t bitmap_size = (page_count + 7u) / 8u;
    uint8_t *bitmap = calloc(1, bitmap_size);
    if (!bitmap) {
        fail(error, error_size, "cannot allocate checkpoint page bitmap");
        return false;
    }
    uint32_t present = 0;
    for (uint32_t page = 0; page < page_count; ++page) {
        const uint8_t *data = sdram + (size_t)page * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
        bool nonzero = false;
        for (size_t i = 0; i < CDJ_DSP_CHECKPOINT_PAGE_SIZE; ++i) nonzero |= data[i] != 0;
        if (nonzero) {
            bitmap[page / 8] |= 1u << (page % 8);
            ++present;
        }
    }
    CheckpointHeader header = {0};
    memcpy(header.magic, CHECKPOINT_MAGIC, sizeof(header.magic));
    header.schema = CDJ_DSP_CHECKPOINT_SCHEMA;
    header.endian = CHECKPOINT_ENDIAN;
    header.header_size = sizeof(header);
    header.state_size = sizeof(state);
    component_sizes(header.component_size);
    header.l2_size = l2_size;
    header.sdram_size = sdram_size;
    header.page_size = CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    header.page_count = page_count;
    header.present_pages = present;
    header.payload_size = sizeof(state) + l2_size + shared_ram_size + l1d_size + bitmap_size +
                          (uint64_t)present * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = checksum(hash, &state, sizeof(state));
    hash = checksum(hash, l2, l2_size);
    hash = checksum(hash, shared_ram, shared_ram_size);
    hash = checksum(hash, l1d, l1d_size);
    hash = checksum(hash, bitmap, bitmap_size);
    for (uint32_t page = 0; page < page_count; ++page) {
        if (bitmap[page / 8] & (1u << (page % 8)))
            hash = checksum(hash, sdram + (size_t)page * header.page_size,
                            header.page_size);
    }
    header.payload_checksum = hash;
    FILE *file = fopen(path, "wb");
    if (!file) {
        if (error && error_size)
            snprintf(error, error_size, "cannot create checkpoint: %s", strerror(errno));
        free(bitmap);
        return false;
    }
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(&state, 1, sizeof(state), file) == sizeof(state) &&
              fwrite(l2, 1, l2_size, file) == l2_size &&
              fwrite(shared_ram, 1, shared_ram_size, file) == shared_ram_size &&
              fwrite(l1d, 1, l1d_size, file) == l1d_size &&
              fwrite(bitmap, 1, bitmap_size, file) == bitmap_size;
    for (uint32_t page = 0; ok && page < page_count; ++page) {
        if (bitmap[page / 8] & (1u << (page % 8))) {
            const uint8_t *data = sdram + (size_t)page * header.page_size;
            ok = fwrite(data, 1, header.page_size, file) == header.page_size;
        }
    }
    ok = ok && fflush(file) == 0 && fclose(file) == 0;
    free(bitmap);
    if (!ok) {
        remove(path);
        fail(error, error_size, "incomplete checkpoint write");
    }
    return ok;
}

bool cdj_dsp_checkpoint_read_with_l1d(
    const char *path, CdjDspCheckpointState *state,
    uint8_t *l2, size_t l2_size,
    uint8_t *shared_ram, size_t shared_ram_size,
    uint8_t *l1d, size_t l1d_size,
    uint8_t *sdram, size_t sdram_size,
    char *error, size_t error_size)
{
    if (!path || !state || !l2 || !shared_ram || !l1d || !sdram ||
        l2_size != CDJ_DSP_L2_SIZE ||
        shared_ram_size != CDJ_DSP_SHARED_RAM_SIZE ||
        l1d_size != CDJ_DSP_L1D_SIZE ||
        sdram_size != CDJ_DSP_SDRAM_SIZE) {
        fail(error, error_size, "missing checkpoint destination");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error && error_size)
            snprintf(error, error_size, "cannot open checkpoint: %s", strerror(errno));
        return false;
    }
    CheckpointHeader header = {0};
    uint32_t expected[CHECKPOINT_COMPONENTS], schema10_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema9_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema8_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema6_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema7_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema5_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema4_expected[CHECKPOINT_COMPONENTS];
    uint32_t schema3_expected[CHECKPOINT_COMPONENTS];
    uint32_t legacy_expected[CHECKPOINT_COMPONENTS];
    component_sizes(expected);
    schema10_component_sizes(schema10_expected);
    schema9_component_sizes(schema9_expected);
    schema8_component_sizes(schema8_expected);
    schema7_component_sizes(schema7_expected);
    schema6_component_sizes(schema6_expected);
    schema5_component_sizes(schema5_expected);
    schema4_component_sizes(schema4_expected);
    schema3_component_sizes(schema3_expected);
    legacy_component_sizes(legacy_expected);
    bool header_read = fread(&header, 1, sizeof(header), file) == sizeof(header);
    bool schema1 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA1_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 1;
    bool schema2 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA2_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 2;
    bool schema3 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA3_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 3;
    bool schema4 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA4_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 4;
    bool schema5 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA5_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 5;
    bool schema6 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA6_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 6;
    bool schema7 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA7_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 7;
    bool schema8 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA8_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 8;
    bool schema9 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA9_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 9;
    bool schema10 = header_read &&
                    memcmp(header.magic, CHECKPOINT_SCHEMA10_MAGIC,
                           sizeof(header.magic)) == 0 && header.schema == 10;
    bool schema11 = header_read &&
                    memcmp(header.magic, CHECKPOINT_SCHEMA11_MAGIC,
                           sizeof(header.magic)) == 0 && header.schema == 11;
    bool current = header_read &&
                   memcmp(header.magic, CHECKPOINT_MAGIC,
                          sizeof(header.magic)) == 0 &&
                   header.schema == CDJ_DSP_CHECKPOINT_SCHEMA;
    /* Appending INTC reused four bytes of the schema-2 structure's trailing
     * alignment padding, so round the prefix back to the old ABI size. */
    size_t alignment = _Alignof(CdjDspCheckpointState);
    size_t legacy_state_size = (offsetof(CdjDspCheckpointState, intc) +
                                alignment - 1) / alignment * alignment;
    size_t schema3_state_size = (offsetof(CdjDspCheckpointState, timers) +
                                 alignment - 1) / alignment * alignment;
    size_t schema4_state_size = (offsetof(CdjDspCheckpointState, spis) +
                                 alignment - 1) / alignment * alignment;
    size_t schema5_state_size = (offsetof(CdjDspCheckpointState, cache) +
                                 alignment - 1) / alignment * alignment;
    size_t schema6_state_size = (offsetof(CdjDspCheckpointState, mcasp_control) +
                                 alignment - 1) / alignment * alignment;
    size_t schema7_state_size = (offsetof(CdjDspCheckpointState, mcasp_control) +
                                 SCHEMA7_MCASP_CONTROL_SIZE + alignment - 1) /
                                alignment * alignment;
    size_t schema8_state_size = (offsetof(CdjDspCheckpointState, wm8740) +
                                 alignment - 1) / alignment * alignment;
    size_t schema9_state_size = (offsetof(CdjDspCheckpointState, spi_transfer) +
                                 alignment - 1) / alignment * alignment;
    size_t schema10_state_size = (offsetof(CdjDspCheckpointState, scheduler) +
                                  alignment - 1) / alignment * alignment;
    bool legacy = (schema1 || schema2) &&
                  header.state_size == legacy_state_size &&
                  memcmp(header.component_size, legacy_expected,
                         sizeof(legacy_expected)) == 0;
    bool old_schema3 = schema3 && header.state_size == schema3_state_size &&
                       memcmp(header.component_size, schema3_expected,
                              sizeof(schema3_expected)) == 0;
    bool old_schema4 = schema4 && header.state_size == schema4_state_size &&
                       memcmp(header.component_size, schema4_expected,
                              sizeof(schema4_expected)) == 0;
    bool old_schema5 = schema5 && header.state_size == schema5_state_size &&
                       memcmp(header.component_size, schema5_expected,
                              sizeof(schema5_expected)) == 0;
    bool old_schema6 = schema6 && header.state_size == schema6_state_size &&
                       memcmp(header.component_size, schema6_expected,
                              sizeof(schema6_expected)) == 0;
    bool old_schema7 = schema7 && header.state_size == schema7_state_size &&
                       memcmp(header.component_size, schema7_expected,
                              sizeof(schema7_expected)) == 0;
    bool old_schema8 = schema8 && header.state_size == schema8_state_size &&
                       memcmp(header.component_size, schema8_expected,
                              sizeof(schema8_expected)) == 0;
    bool old_schema9 = schema9 && header.state_size == schema9_state_size &&
                       memcmp(header.component_size, schema9_expected,
                              sizeof(schema9_expected)) == 0;
    bool old_schema10 = schema10 && header.state_size == schema10_state_size &&
                        memcmp(header.component_size, schema10_expected,
                               sizeof(schema10_expected)) == 0;
    bool old_schema11 = schema11 && header.state_size == sizeof(*state) &&
                        memcmp(header.component_size, expected,
                               sizeof(expected)) == 0;
    bool old = legacy || old_schema3 || old_schema4 || old_schema5 ||
               old_schema6 || old_schema7 || old_schema8 || old_schema9 ||
               old_schema10 || old_schema11;
    bool legacy_schema = old && !old_schema11;
    bool ok = (old || current) &&
              header.endian == CHECKPOINT_ENDIAN &&
              header.header_size == sizeof(header) &&
              (old || (header.state_size == sizeof(*state) &&
                       memcmp(header.component_size, expected,
                              sizeof(expected)) == 0)) &&
              header.l2_size == l2_size && header.sdram_size == sdram_size &&
              header.page_size == CDJ_DSP_CHECKPOINT_PAGE_SIZE &&
              header.page_count == sdram_size / header.page_size;
    const size_t bitmap_size = ok ? (header.page_count + 7u) / 8u : 0;
    uint64_t expected_payload = header.state_size + l2_size +
        (schema1 ? 0 : shared_ram_size) + bitmap_size +
        (current ? l1d_size : 0u) +
        (uint64_t)header.present_pages * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    ok = ok && header.present_pages <= header.page_count &&
         header.payload_size == expected_payload;
    uint8_t *bitmap = ok ? malloc(bitmap_size) : NULL;
    memset(state, 0, sizeof(*state));
    ok = ok && bitmap && fread(state, 1, header.state_size, file) == header.state_size &&
         fread(l2, 1, l2_size, file) == l2_size;
    if (ok && schema1) memset(shared_ram, 0, shared_ram_size);
    else if (ok) ok = fread(shared_ram, 1, shared_ram_size, file) == shared_ram_size;
    if (ok && current) ok = fread(l1d, 1, l1d_size, file) == l1d_size;
    else if (ok) memset(l1d, 0, l1d_size);
    ok = ok && fread(bitmap, 1, bitmap_size, file) == bitmap_size;
    if (ok) memset(sdram, 0, sdram_size);
    uint32_t present = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    if (ok) {
        hash = checksum(hash, state, header.state_size);
        hash = checksum(hash, l2, l2_size);
        if (!schema1) hash = checksum(hash, shared_ram, shared_ram_size);
        if (current) hash = checksum(hash, l1d, l1d_size);
        hash = checksum(hash, bitmap, bitmap_size);
    }
    for (uint32_t page = 0; ok && page < header.page_count; ++page) {
        if (bitmap[page / 8] & (1u << (page % 8))) {
            uint8_t *data = sdram + (size_t)page * header.page_size;
            ok = fread(data, 1, header.page_size, file) == header.page_size;
            if (ok) hash = checksum(hash, data, header.page_size);
            ++present;
        }
    }
    ok = ok && present == header.present_pages && hash == header.payload_checksum &&
         fgetc(file) == EOF && !ferror(file);
    /* Validate the old payload exactly as captured before initializing
     * appended state. This also avoids relying on old tail padding
     * having happened to contain zero bytes. */
    if (ok && legacy) cdj_c6747_intc_reset(&state->intc);
    if (ok && (legacy || old_schema3)) cdj_c6747_timers_reset(state->timers);
    if (ok && (legacy || old_schema3 || old_schema4))
        cdj_c6747_spis_reset(state->spis);
    if (ok && (legacy || old_schema3 || old_schema4 || old_schema5))
        cdj_c6747_cache_reset(&state->cache);
    if (ok && legacy_schema && !old_schema7 && !old_schema8 && !old_schema9 &&
        !old_schema10) {
        cdj_c6747_mcasp_control_reset(&state->mcasp_control);
        cdj_c6747_edma_reset(&state->edma);
        cdj_c6747_syscfg_priority_reset(&state->syscfg_priority);
        cdj_c6747_intc_delivery_reset(&state->intc_delivery);
    }
    if (ok && old_schema7) {
        CdjC6747McaspControl upgraded;
        cdj_c6747_mcasp_control_reset(&upgraded);
        memcpy(&upgraded, &state->mcasp_control,
               SCHEMA7_MCASP_CONTROL_SIZE);
        state->mcasp_control = upgraded;
        cdj_c6747_edma_reset(&state->edma);
        cdj_c6747_syscfg_priority_reset(&state->syscfg_priority);
        cdj_c6747_intc_delivery_reset(&state->intc_delivery);
    }
    if (ok && old_schema8)
        for (unsigned i = 0; i < CDJ_C6747_SPI_COUNT; ++i) {
            state->spis[i].receive_buffer_full = false;
            state->spis[i].receive_buffer_data = 0;
        }
    if (ok && legacy_schema && !old_schema9 && !old_schema10)
        cdj_wm8740_reset(&state->wm8740);
    if (ok && legacy_schema && !old_schema10)
        cdj_c6747_spi_transfer_reset(&state->spi_transfer);
    /* Scheduler reset opts into DEFERRED_V1. Historical checkpoints instead
     * retain their actual legacy synchronous policy as the all-zero mode. */
    if (ok && legacy_schema)
        memset(&state->scheduler, 0, sizeof(state->scheduler));
    ok = ok && state_valid(state);
    fclose(file);
    free(bitmap);
    if (!ok) fail(error, error_size, "incompatible, corrupt, or incomplete checkpoint");
    return ok;
}

bool cdj_dsp_checkpoint_write(const char *path,
                              const CdjDspCheckpointState *state,
                              const uint8_t *l2, size_t l2_size,
                              const uint8_t *shared_ram, size_t shared_ram_size,
                              const uint8_t *sdram, size_t sdram_size,
                              char *error, size_t error_size)
{
    uint8_t *l1d = calloc(1, CDJ_DSP_L1D_SIZE);
    bool ok = l1d && cdj_dsp_checkpoint_write_with_l1d(
        path, state, l2, l2_size, shared_ram, shared_ram_size,
        l1d, CDJ_DSP_L1D_SIZE, sdram, sdram_size, error, error_size);
    if (!l1d) fail(error, error_size, "cannot allocate L1D checkpoint image");
    free(l1d);
    return ok;
}

bool cdj_dsp_checkpoint_read(const char *path,
                             CdjDspCheckpointState *state,
                             uint8_t *l2, size_t l2_size,
                             uint8_t *shared_ram, size_t shared_ram_size,
                             uint8_t *sdram, size_t sdram_size,
                             char *error, size_t error_size)
{
    uint8_t *l1d = malloc(CDJ_DSP_L1D_SIZE);
    bool ok = l1d && cdj_dsp_checkpoint_read_with_l1d(
        path, state, l2, l2_size, shared_ram, shared_ram_size,
        l1d, CDJ_DSP_L1D_SIZE, sdram, sdram_size, error, error_size);
    if (ok) {
        for (size_t i = 0; i < CDJ_DSP_L1D_SIZE; ++i)
            if (l1d[i]) {
                fail(error, error_size,
                     "checkpoint contains L1D state but caller cannot restore it");
                ok = false;
                break;
            }
    }
    if (!l1d) fail(error, error_size, "cannot allocate L1D checkpoint image");
    free(l1d);
    return ok;
}
