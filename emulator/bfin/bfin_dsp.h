/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * From hw/cdj/bfin/bfin_dsp.h of Stijn Jacobs' cdj-nxs2-qemu,
 * https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, commit 08d5cb1.
 * Changed 2026-10-05 (bfin-link): the DSP32 groups, ALU2op and CCflag are
 * GNU sim's (bfin_dsp.c); bfin_dsp32mult is its own entry.
 */
/* The DSP32 groups and the arithmetic flag helpers they share with
 * bfin_exec.c. */
#ifndef BFIN_DSP_H
#define BFIN_DSP_H

#include "bfin_priv.h"

static inline void bfin_flags_nz(bfin_core *c, uint32_t v)
{
    c->astat &= ~(AS_AZ | AS_AN);
    c->astat |= (v ? 0 : AS_AZ) | (v >> 31 ? AS_AN : 0);
}

static inline void bfin_flags_v(bfin_core *c, int v)
{
    c->astat &= ~(AS_V | AS_V_COPY);
    if (v) {
        c->astat |= AS_V | AS_V_COPY | AS_VS;
    }
}

static inline void bfin_flags_ac0(bfin_core *c, int ac0)
{
    c->astat &= ~(AS_AC0 | AS_AC0_COPY);
    if (ac0) {
        c->astat |= AS_AC0 | AS_AC0_COPY;
    }
}

/* AZ and AN from v, AC0 and V cleared: what every logical result sets. */
static inline void bfin_flags_logic(bfin_core *c, uint32_t v)
{
    c->astat &= ~(AS_AZ | AS_AN | AS_AC0 | AS_AC0_COPY | AS_V | AS_V_COPY);
    c->astat |= (v ? 0 : AS_AZ) | (v >> 31 ? AS_AN : 0);
}

static inline uint32_t bfin_add32(bfin_core *c, uint32_t a, uint32_t b,
                                  int sub, int sat)
{
    uint32_t r = sub ? a - b : a + b;
    int v = sub ? ((a ^ b) & (a ^ r)) >> 31 : (~(a ^ b) & (a ^ r)) >> 31;

    if (v && sat) {
        r = (int32_t)a < 0 ? 0x80000000u : 0x7FFFFFFFu;
    }
    bfin_flags_logic(c, r);
    if (sub ? b <= a : r < a) {
        c->astat |= AS_AC0 | AS_AC0_COPY;
    }
    if (v) {
        c->astat |= AS_V | AS_V_COPY | AS_VS;
    }
    return r;
}

void bfin_dsp32mac(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_dsp32mult(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_dsp32alu(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_dsp32shift(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_dsp32shiftimm(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_alu2op(bfin_core *c, uint16_t iw0, uint16_t pad);
void bfin_ccflag(bfin_core *c, uint16_t iw0, uint16_t pad);

#endif
