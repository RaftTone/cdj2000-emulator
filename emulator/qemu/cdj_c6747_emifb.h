/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_EMIFB_H
#define CDJ_C6747_EMIFB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CDJ_C6747_EMIFB_BASE 0xb0000000u
#define CDJ_C6747_EMIFB_REVID (CDJ_C6747_EMIFB_BASE + 0x00u)
#define CDJ_C6747_EMIFB_SDCFG (CDJ_C6747_EMIFB_BASE + 0x08u)
#define CDJ_C6747_EMIFB_SDRFC (CDJ_C6747_EMIFB_BASE + 0x0cu)
#define CDJ_C6747_EMIFB_SDTIM1 (CDJ_C6747_EMIFB_BASE + 0x10u)
#define CDJ_C6747_EMIFB_SDTIM2 (CDJ_C6747_EMIFB_BASE + 0x14u)
#define CDJ_C6747_EMIFB_SDCFG2 (CDJ_C6747_EMIFB_BASE + 0x1cu)
#define CDJ_C6747_EMIFB_BPRIO (CDJ_C6747_EMIFB_BASE + 0x20u)

/* SPRUH91D 19.4.1-5 configuration registers. SDRAM command timing,
 * arbitration, performance counters, line-trap interrupts, PSC/reset-domain
 * coupling and retention are not modeled. */
typedef struct {
    uint32_t sdcfg, sdrfc, sdtim1, sdtim2, sdcfg2, bprio;
    uint32_t init_sequences;
} CdjC6747Emifb;

void cdj_c6747_emifb_reset(CdjC6747Emifb *s);
bool cdj_c6747_emifb_read(const CdjC6747Emifb *s, uint32_t address,
                          uint32_t *value);
/* Legal writes to locked fields complete without changing those fields.
 * Check phase is side-effect free; commit applies protection and readback. */
bool cdj_c6747_emifb_write(CdjC6747Emifb *s, uint32_t address,
                           uint64_t value, unsigned size, bool commit);
bool cdj_c6747_emifb_sdram_enabled(const CdjC6747Emifb *s);
/* Translate one contiguous transfer through the EMIFB aperture to populated
 * power-of-two SDRAM. Alignment is the requesting master's responsibility.
 * Transfers crossing the physical wrap boundary require splitting by caller. */
bool cdj_c6747_emifb_sdram_offset(const CdjC6747Emifb *s, uint32_t address,
                                 size_t size, size_t populated_size,
                                 uint32_t *offset);
#endif
