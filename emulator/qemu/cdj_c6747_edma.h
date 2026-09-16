/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_EDMA_H
#define CDJ_C6747_EDMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CDJ_C6747_EDMA_BASE 0x01c00000u
#define CDJ_C6747_EDMA_PARAM_BASE 0x01c04000u
#define CDJ_C6747_EDMA_CHANNELS 32u
#define CDJ_C6747_EDMA_QCHANNELS 8u
#define CDJ_C6747_EDMA_PARAMS 128u
#define CDJ_C6747_EDMA_REGIONS 4u

/* EDMA does not own DSP memory.  Its client supplies a byte-exact bus.  Reads
 * must be side-effect-free.  A write with commit=false validates the complete
 * destination without changing it; commit=true writes the complete byte span.
 * This is the same check/commit contract used by the C674x interpreter. */
typedef bool (*CdjC6747EdmaReadBytes)(void *opaque, uint32_t address,
                                     uint8_t *bytes, size_t size);
typedef bool (*CdjC6747EdmaWriteBytes)(void *opaque, uint32_t address,
                                      const uint8_t *bytes, size_t size,
                                      bool commit);
typedef struct {
    CdjC6747EdmaReadBytes read;
    CdjC6747EdmaWriteBytes write;
    void *opaque;
} CdjC6747EdmaBus;

/* SPRUH91D chapter 16, C6747's 32-channel/8-QDMA/128-PaRAM instance.
 * Transfers are deliberately completed synchronously.  That is a functional
 * scheduling abstraction: it preserves byte movement, PaRAM updates, linking,
 * chaining and completion state, but does not claim EDMA3CC/TC queue latency,
 * arbitration, bus contention or cycle timing. */
typedef struct {
    uint32_t param[CDJ_C6747_EDMA_PARAMS][8];
    uint32_t qchmap[CDJ_C6747_EDMA_QCHANNELS];
    uint32_t dmaqnum[4], qdmaqnum, qwmthra;
    uint32_t drae[CDJ_C6747_EDMA_REGIONS];
    uint32_t qrae[CDJ_C6747_EDMA_REGIONS];
    uint32_t er, esr, cer, eer, ser;
    uint32_t qer, qeer, qser;
    uint32_t ier, ipr;
    /* Completion/IEVAL pulses waiting for the SoC interrupt fabric.  Keeping
     * this edge state in the device makes checkpoints deterministic without
     * turning IPR into a repeatedly sampled level interrupt. */
    uint32_t irq_notifications;
    uint32_t emr, qemr, ccerr;
    uint64_t transfer_requests, bytes_transferred;
} CdjC6747Edma;

void cdj_c6747_edma_reset(CdjC6747Edma *s);
/* Cheap bus-window rejection before allocating a transactional device copy.
 * True is only a candidate: cdj_c6747_edma_write still validates the register. */
bool cdj_c6747_edma_write_mapped(uint32_t address, unsigned size);
/* Check every modeled register invariant before accepting restored state. */
bool cdj_c6747_edma_valid(const CdjC6747Edma *s);
bool cdj_c6747_edma_read(const CdjC6747Edma *s, uint32_t address,
                         uint32_t *value);
/* Register validation is side-effect-free.  The architecturally write-only
 * set/clear aliases have narrow compatibility readback because the recovered
 * firmware's generic wrappers use read/OR/write sequences on those addresses.
 * ICR reads zero so acknowledging one completion preserves unrelated IPR bits.
 * A commit can synchronously execute
 * transfers caused by ESR, newly-enabled pending events, or QDMA trigger words.
 * Unsupported transfer modes and inaccessible bus spans fail closed. */
bool cdj_c6747_edma_write(CdjC6747Edma *s, uint32_t address, uint64_t value,
                          unsigned size, bool commit,
                          const CdjC6747EdmaBus *bus);
/* Assert a C6747 peripheral event.  Disabled events latch in ER.  Enabled
 * events complete synchronously using the functional scheduling abstraction. */
bool cdj_c6747_edma_event(CdjC6747Edma *s, unsigned channel,
                          const CdjC6747EdmaBus *bus);
bool cdj_c6747_edma_irq_pending(const CdjC6747Edma *s, unsigned region);
/* Consume one region interrupt pulse.  IPR remains set until ICR is written. */
bool cdj_c6747_edma_take_irq_notification(CdjC6747Edma *s, unsigned region);

#endif
