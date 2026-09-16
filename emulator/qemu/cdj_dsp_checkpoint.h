/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_CHECKPOINT_H
#define CDJ_DSP_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdj_c674x.h"
#include "cdj_c6747_emifb.h"
#include "cdj_c6747_gpio.h"
#include "cdj_c6747_hpi.h"
#include "cdj_c6747_i2c.h"
#include "cdj_c6747_intc.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_psc.h"
#include "cdj_c6747_syscfg.h"
#include "cdj_c6747_timer.h"
#include "cdj_c6747_spi.h"
#include "cdj_c6747_cache.h"
#include "cdj_c6747_edma.h"
#include "cdj_dsp_scheduler.h"

#define CDJ_DSP_CHECKPOINT_SCHEMA 12u
#define CDJ_DSP_L1D_SIZE 0x8000u
#define CDJ_DSP_L2_SIZE 0x40000u
#define CDJ_DSP_SHARED_RAM_SIZE 0x20000u
#define CDJ_DSP_SDRAM_SIZE 0x02000000u
#define CDJ_DSP_COOPERATIVE_BUDGET 1000000u
#define CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL 1024u
#define CDJ_DSP_CHECKPOINT_PAGE_SIZE 4096u
#define CDJ_DSP_CHECKPOINT_REASON_SIZE 64u
#define CDJ_DSP_CHECKPOINT_FAULT_SIZE 96u

/* Checkpoints are intentionally ABI-bound: they store the portable interpreter
 * structs verbatim after clearing process-local pointers. The file header
 * records every structure size and native byte order, and readers reject any
 * mismatch. This preserves all pipeline/loop fields without pretending the
 * format is portable across incompatible builds. Source and firmware SHA-256
 * provenance is supplied by the Python run manifest. Schema 2 adds the fixed
 * 128 KiB C6747 shared-RAM image between L2 and sparse EMIFB SDRAM. Schema 3
 * appends INTC state. Schema-1/2 inputs remain readable; missing shared RAM or
 * interrupt-controller state is reset explicitly. Schema 4 appends both
 * Timer64P instances. Schema 5 appends both SPI instances. Schema 6 appends
 * cache-control state. Schema 7 appended the first McASP control/configuration
 * model. Schema 8 appends McASP transmit-buffer, EDMA3 channel-controller and
 * SYSCFG master-priority state. Schema 9 appends the board's write-only
 * WM8740 DAC control state. Schema 10 appends the timed SPI1 transfer-engine
 * state. Schema 11 appends the declared DSP activation scheduler state;
 * older inputs initialize any absent peripheral state. Schema 12 appends the
 * complete 32 KiB physical L1D image after shared RAM. The interpreter models
 * cache-partition SRAM visibility, but not cache contents or timing. */
typedef struct {
    uint32_t hpi_address, boot_phase;
    uint64_t words, event_sequence, checkpoint_sequence;
    uint8_t reset_released, dsp_started, dsp_halted, had_fault;
    char stop_reason[CDJ_DSP_CHECKPOINT_REASON_SIZE];
    char fault[CDJ_DSP_CHECKPOINT_FAULT_SIZE];
    CdjC674x cpu;
    CdjC6747Syscfg syscfg;
    CdjC6747Psc psc;
    CdjC6747Mcasp mcasp;
    CdjC6747Gpio gpio;
    CdjC6747I2c i2c;
    CdjC6747Pll pll;
    CdjC6747Hpi hpi;
    CdjC6747Emifb emifb;
    CdjC6747Intc intc;
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT];
    CdjC6747Cache cache;
    CdjC6747McaspControl mcasp_control;
    CdjC6747Edma edma;
    CdjC6747SyscfgPriority syscfg_priority;
    CdjC6747IntcDelivery intc_delivery;
    CdjWm8740 wm8740;
    CdjC6747SpiTransfer spi_transfer;
    CdjDspScheduler scheduler;
} CdjDspCheckpointState;

void cdj_dsp_checkpoint_prepare(CdjDspCheckpointState *state,
                                const char *reason);
bool cdj_dsp_checkpoint_write(const char *path,
                              const CdjDspCheckpointState *state,
                              const uint8_t *l2, size_t l2_size,
                              const uint8_t *shared_ram, size_t shared_ram_size,
                              const uint8_t *sdram, size_t sdram_size,
                              char *error, size_t error_size);
bool cdj_dsp_checkpoint_write_with_l1d(
    const char *path, const CdjDspCheckpointState *state,
    const uint8_t *l2, size_t l2_size,
    const uint8_t *shared_ram, size_t shared_ram_size,
    const uint8_t *l1d, size_t l1d_size,
    const uint8_t *sdram, size_t sdram_size,
    char *error, size_t error_size);
bool cdj_dsp_checkpoint_read(const char *path,
                             CdjDspCheckpointState *state,
                             uint8_t *l2, size_t l2_size,
                             uint8_t *shared_ram, size_t shared_ram_size,
                             uint8_t *sdram, size_t sdram_size,
                             char *error, size_t error_size);
bool cdj_dsp_checkpoint_read_with_l1d(
    const char *path, CdjDspCheckpointState *state,
    uint8_t *l2, size_t l2_size,
    uint8_t *shared_ram, size_t shared_ram_size,
    uint8_t *l1d, size_t l1d_size,
    uint8_t *sdram, size_t sdram_size,
    char *error, size_t error_size);

#endif
