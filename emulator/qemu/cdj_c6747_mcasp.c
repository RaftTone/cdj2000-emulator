/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_mcasp.h"
/* C6747 datasheet SPRS377F Table 6-43: 16, 12, 4 serializers.
 * The summary's "16/9" wording is not the per-instance configuration. */
static const uint32_t pin_mask[3] = {0xfe00ffffu, 0xfe000fffu, 0xfe00000fu};
static const unsigned serializer_count[3] = {16, 12, 4};
static bool locate(uint32_t address, unsigned *bank, unsigned *offset)
{
    if (address < 0x01d00000u || address >= 0x01d0c000u || (address & 3))
        return false;
    *bank = (address - 0x01d00000u) / 0x4000;
    *offset = (address - 0x01d00000u) % 0x4000;
    return true;
}
bool cdj_c6747_mcasp_control_write_mapped(uint32_t address, unsigned size)
{
    unsigned bank, offset;
    return size == 4 && locate(address, &bank, &offset);
}

void cdj_c6747_mcasp_reset(CdjC6747Mcasp *s)
{
    *s = (CdjC6747Mcasp){0};
}
bool cdj_c6747_mcasp_read(const CdjC6747Mcasp *s, uint32_t address,
                         uint32_t *value)
{
    unsigned b, o;
    if (!locate(address, &b, &o)) return false;
    switch (o) {
    case 0x10: *value = s->pfunc[b]; return true;
    case 0x14: *value = s->pdir[b]; return true;
    case 0x18: *value = s->pdout[b]; return true;
    /* 0x1c reads PDIN, not PDOUT. External inputs are not modeled.
     * PDCLR is a write-only alias; do not invent its readback either. */
    default: return false;
    }
}
bool cdj_c6747_mcasp_write(CdjC6747Mcasp *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit)
{
    unsigned b, o;
    if (size != 4 || value > UINT32_MAX || !locate(address, &b, &o)) return false;
    if (o < 0x10 || o > 0x20 || (value & ~pin_mask[b])) return false;
    /* Nonzero reserved bits explicitly stop; TI cautions against writing
     * them. Validation never changes state. E3 commit updates the latch. */
    if (!commit) return true;
    switch (o) {
    case 0x10: s->pfunc[b] = value; break;
    case 0x14: s->pdir[b] = value; break;
    case 0x18: s->pdout[b] = value; break;
    case 0x1c: s->pdout[b] |= value; break;
    case 0x20: s->pdout[b] &= ~(uint32_t)value; break;
    }
    return true;
}

void cdj_c6747_mcasp_control_reset(CdjC6747McaspControl *s)
{
    *s = (CdjC6747McaspControl){0};
    /* SPRUH91D 24.1.29-30: ASYNC and CLKXM reset set; HCLKXM resets set. */
    for (unsigned b = 0; b < 3; ++b) {
        s->aclkxctl[b] = 0x60;
        s->ahclkxctl[b] = 0x8000;
        s->xdma_next[b] = serializer_count[b];
        s->xslot[b] = 0x17f;
    }
}

static uint32_t active_tx_mask(const CdjC6747McaspControl *s, unsigned b)
{
    uint32_t mask = 0;
    for (unsigned n = 0; n < serializer_count[b]; ++n)
        if ((s->srctl[b][n] & 3u) == 1) mask |= 1u << n;
    return mask;
}

static unsigned next_tx_serializer(const CdjC6747McaspControl *s,
                                   unsigned b, unsigned after)
{
    for (unsigned n = after; n < serializer_count[b]; ++n)
        if ((s->srctl[b][n] & 3u) == 1) return n;
    return serializer_count[b];
}

static void note_xbuf_write(CdjC6747McaspControl *s, unsigned b,
                            unsigned serializer, uint32_t value)
{
    s->xbuf[b][serializer] = value;
    s->xbuf_sequence[b][serializer] = ++s->xbuf_writes[b];
    s->xrdy[b] &= ~(1u << serializer);
    if (!(s->xrdy[b] & active_tx_mask(s, b))) s->xstat[b] &= ~0x20u;
}

bool cdj_c6747_mcasp_axevt_ready(const CdjC6747McaspControl *s,
                                unsigned instance)
{
    return instance < 3 && (s->xstat[instance] & 0x20u);
}

static uint32_t xstat_read(const CdjC6747McaspControl *s, unsigned b)
{
    uint32_t value = s->xstat[b] & 0xf7u;
    /* Table 24-41 defines XTDMSLOT as one for an even current slot and zero
     * for odd.  This also preserves its reset value while XSLOT is 383. */
    value |= (!(s->xslot[b] & 1u)) << 3;
    /* XERR is read-only and is the OR of the four transmit error flags. */
    if (value & 0x87u) value |= 0x100u;
    return value;
}

bool cdj_c6747_mcasp_control_read(const CdjC6747McaspControl *s,
                                 uint32_t address, uint32_t *value)
{
    unsigned b, o;
    if (!locate(address, &b, &o)) return false;
    switch (o) {
    case 0x44: case 0x60: case 0xa0:
        /* RGBLCTL/XGBLCTL reads return the complete GBLCTL value. */
        *value = s->gblctl[b]; return true;
    case 0x48: *value = s->amute[b]; return true;
    case 0x4c: *value = s->dlbctl[b]; return true;
    case 0x50: *value = s->ditctl[b]; return true;
    case 0xa4: *value = s->xmask[b]; return true;
    case 0xa8: *value = s->xfmt[b]; return true;
    case 0xac: *value = s->afsxctl[b]; return true;
    case 0xb0: *value = s->aclkxctl[b]; return true;
    case 0xb4: *value = s->ahclkxctl[b]; return true;
    case 0xb8: *value = s->xtdm[b]; return true;
    case 0xbc: *value = s->xintctl[b]; return true;
    case 0xc0: *value = xstat_read(s, b); return true;
    case 0xc4: *value = s->xslot[b]; return true;
    case 0xc8: *value = s->xclkchk[b]; return true;
    case 0xcc: *value = 0; return true;
    default:
        if (o >= 0x100 && o < 0x160 && !(o & 3)) {
            *value = s->dit[b][(o - 0x100) / 4];
            return true;
        }
        if (o >= 0x180 && o < 0x1c0 && !(o & 3)) {
            unsigned serializer = (o - 0x180) / 4;
            if (serializer >= serializer_count[b]) return false;
            *value = s->srctl[b][serializer] |
                     ((s->xrdy[b] >> serializer) & 1u) << 4;
            return true;
        }
        /* Offset 04h is an observed write-only compatibility latch, not a
         * documented McASP register. It has no claimed hardware effect. */
        return false;
    }
}

static bool legal_xfmt(uint32_t value)
{
    unsigned data_delay = value >> 16;
    unsigned pad = (value >> 13) & 3;
    unsigned slot_size = (value >> 4) & 15;
    return !(value & ~0x3ffffu) && data_delay != 3 && pad != 3 &&
           slot_size >= 3 && (slot_size & 1);
}

static bool legal_afsxctl(uint32_t value)
{
    unsigned mode = value >> 7;
    return !(value & ~0xff93u) &&
           (mode == 0 || (mode >= 2 && mode <= 0x20) || mode == 0x180);
}

static bool legal_dlbctl(uint32_t value)
{
    if (value & ~0x0fu) return false;
    unsigned mode = (value >> 2) & 3;
    return (value & 1) ? mode == 1 : mode == 0;
}

static bool legal_srctl(uint32_t value)
{
    unsigned mode = value & 3, drive = (value >> 2) & 3;
    return !(value & ~0x0fu) && mode != 3 && drive != 1;
}

bool cdj_c6747_mcasp_control_valid(const CdjC6747McaspControl *s)
{
    for (unsigned b = 0; b < 3; ++b) {
        if (s->undocumented04[b] > 1 || (s->gblctl[b] & ~0x1f1fu) ||
            (s->amute[b] & ~0x1fefu) || (s->amute[b] & 3u) == 3 ||
            !legal_dlbctl(s->dlbctl[b]) || (s->ditctl[b] & ~0x0du) ||
            (s->xfmt[b] && !legal_xfmt(s->xfmt[b])) ||
            !legal_afsxctl(s->afsxctl[b]) ||
            (s->aclkxctl[b] & ~0xffu) ||
            (s->ahclkxctl[b] & ~0xcfffu) ||
            (s->xintctl[b] & ~0xbfu) || (s->xstat[b] & ~0xf7u) ||
            (s->xclkchk[b] & ~0x00ffff0fu) ||
            (s->xclkchk[b] & 0xfu) > 8)
            return false;
        uint32_t serializer_mask = (1u << serializer_count[b]) - 1u;
        if ((s->xrdy[b] & ~serializer_mask) ||
            s->xdma_next[b] > serializer_count[b]) return false;
        if ((s->xstat[b] & 0x20u) &&
            !(s->xrdy[b] & active_tx_mask(s, b))) return false;
        for (unsigned n = 0; n < serializer_count[b]; ++n)
            if (s->xbuf_sequence[b][n] > s->xbuf_writes[b] ||
                s->xrsr_source_sequence[b][n] >
                    s->xbuf_sequence[b][n] ||
                (!s->xrsr_source_sequence[b][n] && s->xrsr[b][n]))
                return false;
        if (s->xslot[b] > 0x17fu) return false;
        for (unsigned n = 0; n < 16; ++n) {
            if (n >= serializer_count[b]) {
                if (s->srctl[b][n] || s->xbuf[b][n] ||
                    s->xbuf_sequence[b][n] || s->xrsr[b][n] ||
                    s->xrsr_source_sequence[b][n]) return false;
            } else if (!legal_srctl(s->srctl[b][n]) ||
                       (((s->xrdy[b] >> n) & 1u) &&
                        (s->srctl[b][n] & 3u) != 1)) {
                return false;
            }
        }
    }
    return true;
}

static void write_gblctl(CdjC6747McaspControl *s, unsigned b,
                         unsigned offset, uint32_t value)
{
    uint32_t mask = offset == 0x60 ? 0x001fu :
                    offset == 0xa0 ? 0x1f00u : 0x1f1fu;
    uint32_t old = s->gblctl[b];
    uint32_t next = (old & ~mask) | (value & mask);
    uint32_t rising = next & ~old;
    s->gblctl[b] = next;

    /* XSLOT initializes so that the next state-machine transfer is slot 0. */
    if (!(next & 0x1000u)) s->xslot[b] = 0x17f;

    /* The functional model latches reset-release writes immediately. Real
     * hardware synchronizes them to ACLKX/ACLKR; this abstraction makes the
     * required firmware write/read polling deterministic without inventing
     * a serial clock rate. */
    if (!(next & 0x400u)) {
        s->xrdy[b] = 0;
        s->xstat[b] &= ~0x20u;
        s->xdma_next[b] = serializer_count[b];
    } else if (rising & 0x400u) {
        s->xrdy[b] = active_tx_mask(s, b);
        s->xdma_next[b] = next_tx_serializer(s, b, 0);
        if (s->xrdy[b]) {
            s->xstat[b] |= 0x20u;
            if (s->axevt_generation[b] != UINT64_MAX)
                ++s->axevt_generation[b];
        }
    }
    /* Releasing the state machine before every ready XBUF has been preloaded
     * has the documented underrun. */
    if ((rising & 0x800u) && s->xrdy[b]) s->xstat[b] |= 1u;
}

bool cdj_c6747_mcasp_tx_clock_hz(const CdjC6747McaspControl *s,
                                unsigned instance, uint32_t auxclk_hz,
                                unsigned tap, uint64_t *numerator,
                                uint32_t *denominator)
{
    if (!s || instance >= 3 || !auxclk_hz || !numerator || !denominator ||
        tap > CDJ_C6747_MCASP_AFSX)
        return false;
    unsigned b = instance;
    /* HCLKXM = 0 sources AHCLKX from the AHCLKX pin (AHCLKXCTL bit 15,
     * SPRUH91D Table 24-38 printed page 1078); a pin frequency is a board
     * fact outside these manuals. */
    if (!(s->ahclkxctl[b] & 0x8000u)) return false;
    uint32_t den = (s->ahclkxctl[b] & 0xfffu) + 1u; /* HCLKXDIV + 1. */
    if (tap == CDJ_C6747_MCASP_AHCLKX) {
        *numerator = auxclk_hz;
        *denominator = den;
        return true;
    }
    /* CLKXM = 0 sources ACLKX from the ACLKX pin (ACLKXCTL bit 5, Table
     * 24-37 printed page 1077). */
    if (!(s->aclkxctl[b] & 0x20u)) return false;
    den *= (s->aclkxctl[b] & 0x1fu) + 1u; /* CLKXDIV + 1. */
    if (tap == CDJ_C6747_MCASP_ACLKX) {
        *numerator = auxclk_hz;
        *denominator = den;
        return true;
    }
    /* AFSX is pinned only for an internally generated TDM frame sync. DITEN
     * selects the 384-subframe DIT frame instead (DITCTL bit 0), FSXM = 0
     * takes AFSX from the pin and XMOD = 0 is burst mode, whose frame comes
     * from a separately timed source (AFSXCTL, Table 24-36 printed page
     * 1076). */
    if (s->ditctl[b] & 1u) return false;
    if (!(s->afsxctl[b] & 2u)) return false;
    uint32_t slots = s->afsxctl[b] >> 7;
    if (slots < 2 || slots > 0x20u) return false;
    /* XSSZ's legal encodings 3h, 5h, ... Fh are slot sizes 8, 12, ... 32
     * bits, that is 2 x (XSSZ + 1) (XFMT, Table 24-35 printed page 1075). */
    unsigned slot_size = (s->xfmt[b] >> 4) & 15u;
    if (slot_size < 3 || !(slot_size & 1u)) return false;
    den *= slots * 2u * (slot_size + 1u);
    *numerator = auxclk_hz;
    *denominator = den;
    return true;
}

bool cdj_c6747_mcasp_tx_slot(CdjC6747McaspControl *s, unsigned instance,
                            bool *axevt)
{
    if (!s || !axevt || instance >= 3) return false;

    unsigned b = instance;
    uint32_t active_serializers = active_tx_mask(s, b);
    if ((s->gblctl[b] & 0x1f00u) != 0x1f00u || !active_serializers)
        return false;

    uint32_t mode = s->afsxctl[b] >> 7;
    bool dit = s->ditctl[b] & 1u;
    uint32_t slots;
    if (dit) {
        /* DIT has a 384-subframe counter and no programmable TDM selection. */
        if (mode != 0x180u || s->xtdm[b] != UINT32_MAX) return false;
        slots = 0x180u;
    } else {
        /* Burst mode needs a separately modeled frame source. */
        if (mode < 2 || mode > 32) return false;
        slots = mode;
    }

    uint32_t current = s->xslot[b];
    if (current != 0x17fu && current >= slots) return false;
    uint32_t next = current == 0x17fu || current + 1u == slots ?
                    0 : current + 1u;
    bool active_slot = dit || (s->xtdm[b] & (UINT32_C(1) << next));
    if (s->tx_slot_boundaries[b] == UINT64_MAX ||
        (active_slot && s->axevt_generation[b] == UINT64_MAX))
        return false;

    s->xslot[b] = next;
    ++s->tx_slot_boundaries[b];
    if (!active_slot) {
        *axevt = false;
        return true;
    }

    bool underrun = false;
    for (unsigned n = 0; n < serializer_count[b]; ++n) {
        if (!(active_serializers & (1u << n))) continue;
        if (s->xbuf_sequence[b][n] > s->xrsr_source_sequence[b][n]) {
            s->xrsr[b][n] = s->xbuf[b][n];
            s->xrsr_source_sequence[b][n] = s->xbuf_sequence[b][n];
        } else {
            underrun = true;
        }
    }
    if (underrun) s->xstat[b] |= 1u;

    /* A transfer empties every active serializer buffer in lockstep.  McASP
     * issues one AXEVT for the set, not one event per serializer. */
    s->xrdy[b] = active_serializers;
    s->xdma_next[b] = next_tx_serializer(s, b, 0);
    s->xstat[b] |= 0x20u;
    ++s->axevt_generation[b];
    *axevt = true;
    return true;
}

bool cdj_c6747_mcasp_control_write(CdjC6747McaspControl *s,
                                  uint32_t address, uint64_t value,
                                  unsigned size, bool commit)
{
    unsigned b, o;
    if (size != 4 || value > UINT32_MAX || !locate(address, &b, &o))
        return false;
    uint32_t v = value;

    /* Offset 04h is absent from SPRUH91D Table 24-7. Firmware writes exactly
     * one to it once per instance. Preserve that write-only observation as a
     * compatibility latch and reject every other value and all reads. */
    if (o == 0x04) {
        if (v != 1) return false;
        if (commit) s->undocumented04[b] = 1;
        return true;
    }

    /* The DMA port aliases all transmit serializers at one address and
     * advances in increasing active-serializer order for the current slot.
     * XBUSEL selects this port.  Writes through the unselected port are
     * architecturally ignored, not rejected (SPRUH91D 24.0.21.3.2-3). */
    if (o == 0x2000) {
        if (s->xfmt[b] & 8u) return true;
        unsigned serializer = s->xdma_next[b];
        if (serializer >= serializer_count[b]) {
            if (commit) s->xstat[b] |= 0x80u;
            return true;
        }
        if (s->xbuf_writes[b] == UINT64_MAX) return false;
        if (!commit) return true;
        note_xbuf_write(s, b, serializer, v);
        s->xdma_next[b] = next_tx_serializer(s, b, serializer + 1);
        return true;
    }

    if (o >= 0x200 && o < 0x240) {
        unsigned serializer = (o - 0x200) / 4;
        if (serializer >= serializer_count[b]) return false;
        if (!(s->xfmt[b] & 8u)) return true;
        if (s->xbuf_writes[b] == UINT64_MAX) return false;
        if (!commit) return true;
        note_xbuf_write(s, b, serializer, v);
        return true;
    }

    bool valid = true;
    switch (o) {
    case 0x44: case 0x60: case 0xa0: valid = !(v & ~0x1f1fu); break;
    case 0x48: valid = !(v & ~0x1fefu) && (v & 3) != 3; break;
    case 0x4c: valid = legal_dlbctl(v); break;
    case 0x50:
        valid = !(v & ~0x0du) &&
                (!((v ^ s->ditctl[b]) & 1u) ||
                 !(s->gblctl[b] & 0x800u));
        break;
    case 0xa4: break;
    case 0xa8: valid = legal_xfmt(v); break;
    case 0xac: valid = legal_afsxctl(v); break;
    case 0xb0: valid = !(v & ~0xffu); break;
    case 0xb4: valid = !(v & ~0xcfffu); break;
    case 0xb8: break;
    case 0xbc: valid = !(v & ~0xbfu); break;
    case 0xc0:
        /* Observed firmware clears all low status bits with 0xffff. Bits
         * outside the W1C fields are ignored exactly as the register does. */
        valid = !(v & 0xffff0000u);
        break;
    case 0xc4: return false; /* XSLOT is read-only. */
    case 0xc8:
        valid = !(v & ~0x00ffff0fu) && (v & 0xfu) <= 8;
        break;
    case 0xcc: valid = v == 0; break;
    default:
        if (o >= 0x100 && o < 0x160 && !(o & 3)) {
            if (commit) s->dit[b][(o - 0x100) / 4] = v;
            return true;
        }
        if (o < 0x180 || o >= 0x1c0 || (o & 3)) return false;
        {
            unsigned serializer = (o - 0x180) / 4;
            if (serializer >= serializer_count[b] || !legal_srctl(v))
                return false;
            if (!commit) return true;
            s->srctl[b][serializer] = v;
            if ((v & 3) != 1) {
                s->xrdy[b] &= ~(1u << serializer);
                if (s->xdma_next[b] == serializer)
                    s->xdma_next[b] = next_tx_serializer(
                        s, b, serializer + 1);
                if (!(s->xrdy[b] & active_tx_mask(s, b)))
                    s->xstat[b] &= ~0x20u;
            }
            return true;
        }
    }
    if (!valid) return false;
    if (!commit) return true;

    switch (o) {
    case 0x44: case 0x60: case 0xa0: write_gblctl(s, b, o, v); break;
    case 0x48: s->amute[b] = v; break;
    case 0x4c: s->dlbctl[b] = v; break;
    case 0x50: s->ditctl[b] = v; break;
    case 0xa4: s->xmask[b] = v; break;
    case 0xa8: s->xfmt[b] = v; break;
    case 0xac: s->afsxctl[b] = v; break;
    case 0xb0: s->aclkxctl[b] = v; break;
    case 0xb4: s->ahclkxctl[b] = v; break;
    case 0xb8: s->xtdm[b] = v; break;
    case 0xbc: s->xintctl[b] = v; break;
    case 0xc0: s->xstat[b] &= ~(v & 0xf7u); break;
    case 0xc8: s->xclkchk[b] = v; break;
    case 0xcc: break;
    }
    return true;
}
