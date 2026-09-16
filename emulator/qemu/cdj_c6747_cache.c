/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "cdj_c6747_cache.h"

enum {
    L2WBAR, L2WIBAR, L2IBAR, L1PIBAR, L1DWIBAR, L1DWBAR, L1DIBAR
};

static bool mar_supported(unsigned index)
{
    /* C6747 device map: EMIFA CS0/2-5, shared RAM, and EMIFB CS0. */
    return (index >= 64 && index <= 103) || index == 128 ||
           (index >= 192 && index <= 223);
}

bool cdj_c6747_l1d_sram_span(const CdjC6747Cache *cache, uint32_t address,
                             size_t size, uint32_t *offset)
{
    static const uint32_t bytes[8] = {
        0x8000, 0x7000, 0x6000, 0x4000, 0, 0, 0, 0
    };
    uint32_t base, at, available;
    if (!cache || !offset || !size) return false;
    if (address >= 0x00f00000u && address < 0x00f08000u)
        base = 0x00f00000u;
    else if (address >= 0x11f00000u && address < 0x11f08000u)
        base = 0x11f00000u;
    else
        return false;
    at = address - base;
    available = bytes[cache->l1dcfg & 7u];
    if (at > available || size > available - at) return false;
    *offset = at;
    return true;
}

void cdj_c6747_cache_reset(CdjC6747Cache *cache)
{
    memset(cache, 0, sizeof(*cache));
    /* C6747 defaults: L1P/L1D are all cache; L2 is all RAM. */
    cache->l1pcfg = 7;
    cache->l1dcfg = 7;
}

static bool block_base_index(uint32_t offset, unsigned *index)
{
    switch (offset) {
    case 0x4000: *index = L2WBAR; break;
    case 0x4010: *index = L2WIBAR; break;
    case 0x4018: *index = L2IBAR; break;
    case 0x4020: *index = L1PIBAR; break;
    case 0x4030: *index = L1DWIBAR; break;
    case 0x4040: *index = L1DWBAR; break;
    case 0x4048: *index = L1DIBAR; break;
    default: return false;
    }
    return true;
}

static bool word_count_offset(uint32_t offset)
{
    switch (offset) {
    case 0x4004: case 0x4014: case 0x401c: case 0x4024:
    case 0x4034: case 0x4044: case 0x404c:
        return true;
    default:
        return false;
    }
}

static bool global_operation_offset(uint32_t offset)
{
    switch (offset) {
    case 0x5000: case 0x5004: case 0x5008: case 0x5028:
    case 0x5040: case 0x5044: case 0x5048:
        return true;
    default:
        return false;
    }
}

bool cdj_c6747_cache_read(CdjC6747Cache *cache, uint32_t address,
                          uint32_t *value)
{
    if (!cache || !value || address < CDJ_C6747_CACHE_BASE ||
        address >= CDJ_C6747_CACHE_BASE + 0x10000 || (address & 3))
        return false;
    uint32_t offset = address - CDJ_C6747_CACHE_BASE;
    switch (offset) {
    case 0x0000: *value = cache->l2cfg; return true;
    case 0x0020: *value = cache->l1pcfg; return true;
    case 0x0024: *value = cache->l1pcc; return true;
    case 0x0040: *value = cache->l1dcfg; return true;
    case 0x0044: *value = cache->l1dcc; return true;
    default: break;
    }
    if (word_count_offset(offset) || global_operation_offset(offset)) {
        *value = 0; /* The coherent-backing-store operation is complete. */
        return true;
    }
    if (offset >= 0x8000 && offset <= 0x83fc) {
        unsigned index = (offset - 0x8000) / 4;
        if (!mar_supported(index)) return false;
        *value = cache->mar[index];
        return true;
    }
    /* Block base registers are architecturally write-only. */
    return false;
}

bool cdj_c6747_cache_write(CdjC6747Cache *cache, uint32_t address,
                           uint64_t value, unsigned size, bool commit)
{
    if (!cache || size != 4 || value > UINT32_MAX ||
        address < CDJ_C6747_CACHE_BASE ||
        address >= CDJ_C6747_CACHE_BASE + 0x10000 || (address & 3))
        return false;
    uint32_t offset = address - CDJ_C6747_CACHE_BASE;
    uint32_t word = value;
    unsigned index;
    switch (offset) {
    case 0x0000:
        /* IP/ID commands (bits 9/8) complete immediately and read as zero. */
        if (commit) cache->l2cfg = word & 0xf;
        return true;
    case 0x0020:
        if (commit) cache->l1pcfg = word & 7;
        return true;
    case 0x0024:
        if (commit) cache->l1pcc = (cache->l1pcc & 1) << 16 | (word & 1);
        return true;
    case 0x0040:
        if (commit) cache->l1dcfg = word & 7;
        return true;
    case 0x0044:
        if (commit) cache->l1dcc = (cache->l1dcc & 1) << 16 | (word & 1);
        return true;
    default:
        break;
    }
    if (block_base_index(offset, &index)) {
        if (commit) cache->block_base[index] = word;
        return true;
    }
    if (word_count_offset(offset) || global_operation_offset(offset))
        return true; /* Unified backing memory is already coherent. */
    if (offset >= 0x8000 && offset <= 0x83fc) {
        index = (offset - 0x8000) / 4;
        if (!mar_supported(index)) return false;
        if (commit) cache->mar[index] = word & 1;
        return true;
    }
    return false;
}
