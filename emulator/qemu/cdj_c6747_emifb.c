/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_emifb.h"

#define SDCFG_VALID ((1u << 26) | (1u << 25) | (1u << 23) | (1u << 16) | \
                     (1u << 15) | (1u << 14) | (7u << 9) | (7u << 4) | 7u)
#define SDCFG_BOOT_UNLOCK (1u << 23)
#define SDCFG_SDREN (1u << 16)
#define SDCFG_TIMUNLOCK (1u << 15)
#define SDTIM2_VALID ((15u << 27) | (127u << 16) | 31u)
#define SDRFC_VALID ((3u << 30) | (1u << 23) | 0xffffu)

void cdj_c6747_emifb_reset(CdjC6747Emifb *s)
{
    *s = (CdjC6747Emifb){
        .sdcfg = 0x00010620u,
        .sdrfc = 0x000004e2u,
        .sdtim1 = 0x14d93a90u,
        .sdtim2 = 0x700a0007u,
        .bprio = 0xffu,
    };
}

bool cdj_c6747_emifb_read(const CdjC6747Emifb *s, uint32_t address,
                          uint32_t *value)
{
    switch (address) {
    case CDJ_C6747_EMIFB_REVID: *value = 0x4033131fu; return true;
    case CDJ_C6747_EMIFB_SDCFG: *value = s->sdcfg; return true;
    case CDJ_C6747_EMIFB_SDRFC: *value = s->sdrfc; return true;
    case CDJ_C6747_EMIFB_SDTIM1: *value = s->sdtim1; return true;
    case CDJ_C6747_EMIFB_SDTIM2: *value = s->sdtim2; return true;
    case CDJ_C6747_EMIFB_SDCFG2: *value = s->sdcfg2; return true;
    case CDJ_C6747_EMIFB_BPRIO: *value = s->bprio; return true;
    default: return false;
    }
}

static bool valid_sdcfg(uint32_t v)
{
    unsigned cl = (v >> 9) & 7, ibank = (v >> 4) & 7, page = v & 7;
    return !(v & ~SDCFG_VALID) && cl >= 2 && cl <= 3 && ibank <= 2 &&
           page <= 3;
}

static bool valid_sdcfg2(uint32_t v)
{
    unsigned pasr = (v >> 16) & 7, rows = v & 7;
    return !(v & ~0x00070007u) && pasr != 3 && pasr != 4 && pasr != 7 &&
           rows <= 5;
}

bool cdj_c6747_emifb_write(CdjC6747Emifb *s, uint32_t address,
                           uint64_t value, unsigned size, bool commit)
{
    if (size != 4 || value > UINT32_MAX) return false;
    uint32_t v = value;

    switch (address) {
    case CDJ_C6747_EMIFB_SDCFG: {
        if (!valid_sdcfg(v)) return false;
        if (!commit) return true;
        uint32_t old = s->sdcfg;
        uint32_t protected = (1u << 26) | (1u << 25) | SDCFG_SDREN;
        uint32_t next = (v & ~protected) | (old & protected);

        /* BOOT_UNLOCK protects the following transaction. TIMUNLOCK, in
         * contrast, permits CL in the same transaction and timing registers
         * while it remains set. */
        if (old & SDCFG_BOOT_UNLOCK) next = (next & ~protected) | (v & protected);
        if (!(v & SDCFG_TIMUNLOCK))
            next = (next & ~(7u << 9)) | (old & (7u << 9));
        s->sdcfg = next;
        ++s->init_sequences; /* every 32-bit write includes lower two bytes */
        return true;
    }
    case CDJ_C6747_EMIFB_SDRFC:
        if (v & ~SDRFC_VALID) return false;
        if (commit) {
            uint32_t rate = v & 0xffffu;
            if (rate < 0x100) rate = 2 * ((s->sdtim1 >> 25) & 0x7f);
            s->sdrfc = (v & 0xffff0000u) | rate;
        }
        return true;
    case CDJ_C6747_EMIFB_SDTIM1:
        if (v & 7) return false;
        if (((v >> 11) & 0x1f) < ((v >> 19) & 7)) return false;
        if (commit && (s->sdcfg & SDCFG_TIMUNLOCK)) s->sdtim1 = v;
        return true;
    case CDJ_C6747_EMIFB_SDTIM2:
        if (v & ~SDTIM2_VALID) return false;
        if (commit && (s->sdcfg & SDCFG_TIMUNLOCK)) s->sdtim2 = v;
        return true;
    case CDJ_C6747_EMIFB_SDCFG2:
        if (!valid_sdcfg2(v)) return false;
        if (commit) { s->sdcfg2 = v; ++s->init_sequences; }
        return true;
    case CDJ_C6747_EMIFB_BPRIO:
        if (v & ~0xffu) return false;
        if (commit) s->bprio = v;
        return true;
    default:
        return false;
    }
}

bool cdj_c6747_emifb_sdram_enabled(const CdjC6747Emifb *s)
{
    return (s->sdcfg & SDCFG_SDREN) != 0;
}

bool cdj_c6747_emifb_sdram_offset(const CdjC6747Emifb *s, uint32_t address,
                                 size_t size, size_t populated_size,
                                 uint32_t *offset)
{
    /* SPRUH91D Table 5-1 routes C0000000-DFFFFFFF through MPU2 to
     * EMIFB; Table 5-2 resets MPU2 to assume-allowed. Section 5.2.2
     * explicitly warns that unpopulated SDRAM aliases populated memory.
     * Tables 19-14/15 map only row/column/bank bits to the SDRAM pins.
     * The NXS's 16-bit, four-bank, 512-column, 13-row configuration has
     * 25 physical address bits (32 MiB).
     *
     * The C6747 data sheet labels D0000000-DFFFFFFF reserved. Extending
     * the pin alias there is a board-model inference from the documented
     * MPU2 aperture, not a guarantee for arbitrary reserved chip addresses.
     * Keep this restricted to EMIFB; never synthesize unmapped read data.
     * MPU protection and geometry reconfiguration are not modeled here. */
    if (!cdj_c6747_emifb_sdram_enabled(s) || !size || !populated_size ||
        populated_size > 0x20000000u ||
        (populated_size & (populated_size - 1)) ||
        address < 0xc0000000u || address >= 0xe0000000u ||
        size > (uint64_t)0xe0000000u - address)
        return false;
    uint32_t physical = (address - 0xc0000000u) & (populated_size - 1);
    if (size > populated_size - physical) return false;
    *offset = physical;
    return true;
}
