/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_CACHE_H
#define CDJ_C6747_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CDJ_C6747_CACHE_BASE 0x01840000u

/* C674x megamodule cache-control state (SPRUFK5A chapters 2-4, with the
 * C6747 register map and reset sizes from SPRUH91D/SPRS377F).  Backing memory
 * is intentionally coherent and untimed, so coherence commands complete
 * immediately; this is a functional abstraction, not a cache/timing model. */
typedef struct {
    uint32_t l2cfg, l1pcfg, l1pcc, l1dcfg, l1dcc;
    uint32_t block_base[7];
    uint32_t mar[256];
} CdjC6747Cache;

void cdj_c6747_cache_reset(CdjC6747Cache *cache);
bool cdj_c6747_cache_read(CdjC6747Cache *cache, uint32_t address,
                          uint32_t *value);
/* Only documented word accesses are accepted. Write-only block base
 * registers remain unreadable; coherence commands complete immediately. */
bool cdj_c6747_cache_write(CdjC6747Cache *cache, uint32_t address,
                           uint64_t value, unsigned size, bool commit);
/* Translate a currently exposed local/global L1D SRAM span to its physical
 * 32 KiB backing-store offset. Reserved cache modes expose no SRAM. */
bool cdj_c6747_l1d_sram_span(const CdjC6747Cache *cache, uint32_t address,
                             size_t size, uint32_t *offset);

#endif
