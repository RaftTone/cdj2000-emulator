/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_psc.h"
/* Tables 8-1/8-2: populated modules and modules restricted to Enable. */
static const uint32_t populated[2] = {0xba3f, 0x87333ffe};
static const uint32_t always_enabled[2] = {0x1800, 0x87000000};
static unsigned domain(unsigned bank, unsigned module)
{ return module == (bank ? 31u : 15u); }
static bool mapped(uint32_t address, unsigned *bank, unsigned *offset)
{
    if (address & 3) return false;
    if (address >= 0x01c10000 && address < 0x01c11000) *bank = 0;
    else if (address >= 0x01e27000 && address < 0x01e28000) *bank = 1;
    else return false;
    *offset = address & 0xfff;
    return true;
}
static uint32_t status(unsigned state)
{
    /* LRSTDONE/LRST and reserved bit 11 read one; MRST/MCKOUT track
     * module state (Table 8-22). These are logical signal levels only. */
    return 0xb00 | state | (state >= 2 ? 0x400 : 0) |
           (state == 1 || state == 3 ? 0x1000 : 0);
}
void cdj_c6747_psc_reset(CdjC6747Psc *s)
{
    *s = (CdjC6747Psc){0};
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned m = 0; m < 32; ++m) {
            if (!(populated[b] & (1u << m))) continue;
            unsigned state = ((always_enabled[b] & (1u << m)) || (!b && m == 15)) ? 3 : 0;
            s->status[b][m] = status(state);
            s->control[b][m] = state;
            /* Entry is after the missing boot ROM: DSP is already executing.
             * Preserve that explicit handoff assumption for its local reset. */
            if (!b && m == 15) s->control[b][m] |= 0x100;
        }
}
void cdj_c6747_psc_tick(CdjC6747Psc *s)
{
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned d = 0; d < 2; ++d) {
            if (!s->remaining[b][d] || --s->remaining[b][d]) continue;
            for (unsigned m = 0; m < 32; ++m)
                if ((populated[b] & (1u << m)) && domain(b, m) == d)
                    s->status[b][m] = status(s->target[b][m]);
        }
}
bool cdj_c6747_psc_read(const CdjC6747Psc *s, uint32_t address, uint32_t *value)
{
    unsigned b, off;
    if (!mapped(address, &b, &off)) return false;
    /* Firmware performs read/OR/write on PTCMD. Its GO bits are W-0 in
     * Table 8-14; zero readback is an explicit, unmeasured bus assumption. */
    if (off == 0x120) { *value = 0; return true; }
    if (off == 0x128) {
        *value = (s->remaining[b][0] ? 1 : 0) | (s->remaining[b][1] ? 2 : 0);
        return true;
    }
    if ((off >= 0x800 && off < 0x880) || (off >= 0xa00 && off < 0xa80)) {
        unsigned m = (off & 0x7f) / 4;
        if (!(populated[b] & (1u << m))) return false;
        *value = off < 0xa00 ? s->status[b][m] : s->control[b][m];
        return true;
    }
    return false;
}
bool cdj_c6747_psc_write(CdjC6747Psc *s, uint32_t address, uint64_t value,
                        unsigned size, bool commit)
{
    unsigned b, off;
    if (size != 4 || !mapped(address, &b, &off)) return false;
    if (off >= 0xa00 && off < 0xa80) {
        unsigned m = (off - 0xa00) / 4;
        if (!(populated[b] & (1u << m))) return false;
        /* Auto-sleep/wake, FORCE, emulation interrupts and DSP self-reset
         * require hardware connections that do not exist yet. */
        unsigned allowed = !b && m == 15 ? 0x103 : 3;
        if (value & ~(uint64_t)allowed) return false;
        if (!b && m == 15 && value != 0x103) return false;
        if (commit) s->control[b][m] = value;
        return true;
    }
    if (off != 0x120 || value & ~UINT64_C(3)) return false;
    if (!commit) return true; /* Validation must not depend on pending stores. */
    for (unsigned d = 0; d < 2; ++d) {
        if (!(value & (1u << d))) continue;
        /* Software must wait for idle before GO (8.3.2). Repeated GO while
         * busy is ignored in this model; it never restarts the countdown. */
        if (s->remaining[b][d]) continue;
        for (unsigned m = 0; m < 32; ++m) {
            if (!(populated[b] & (1u << m)) || domain(b, m) != d) continue;
            s->target[b][m] = always_enabled[b] & (1u << m) ? 3 : s->control[b][m] & 3;
        }
        s->remaining[b][d] = 8;
    }
    return true;
}
