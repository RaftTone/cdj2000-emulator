/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bfin-gnu-parity CASES: one instruction (or bundle) per record on the fast
 * core, from a set state, against the state GNU sim (bin/cdj-run) left.
 * tests/test_bfin_gnu_parity.py writes the records:
 *   u16 code[4]; u32 in[40]; u8 mem[256]; u32 out[40]; u8 memout[256];
 *   u32 region; u32 memcheck
 * in the register order of tools/cdj_gui/bfin_diff.py (R0-7, P0-5, SP, FP,
 * I, M, B, L, A0.W, A0.X, A1.W, A1.X, ASTAT, RETS, LC0, LC1). Prints each
 * mismatch; exit 1 if any.
 */
#include "bfin_priv.h"
#include <stdlib.h>

#define LO 0x1000000u
#define CODE 0x2000000u

static const char *const names[40] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "P0", "P1", "P2", "P3", "P4", "P5", "SP", "FP",
    "I0", "I1", "I2", "I3", "M0", "M1", "M2", "M3",
    "B0", "B1", "B2", "B3", "L0", "L1", "L2", "L3",
    "A0.W", "A0.X", "A1.W", "A1.X", "ASTAT", "RETS", "LC0", "LC1",
};

static uint32_t rd(void *o, uint32_t a, unsigned s) { return 0; }
static void wr(void *o, uint32_t a, uint32_t v, unsigned s) {}

struct rec {
    uint16_t code[4];
    uint32_t in[40];
    uint8_t mem[256];
    uint32_t out[40];
    uint8_t memout[256];
    uint32_t region;
    uint32_t memcheck;              /* 0: GNU sim's region not kept */
};

int main(int argc, char **argv)
{
    static struct rec r;
    uint8_t *ram = calloc(1, CODE + 0x100000 - LO);
    bfin_bus bus = { 0, rd, wr };
    bfin_core *c = bfin_new(&bus);
    FILE *f = fopen(argv[1], "rb");
    unsigned n = 0, bad = 0;

    bfin_map_ram(c, LO, CODE + 0x100000 - LO, ram);
    while (fread(&r, sizeof r, 1, f) == 1) {
        uint32_t *s = r.in, got[40];

        memcpy(ram + (CODE - LO), r.code, sizeof r.code);
        memcpy(ram + (r.region - LO), r.mem, 256);
        bfin_reset(c, CODE);
        memcpy(c->r, s, 32);
        memcpy(c->p, s + 8, 32);
        for (int k = 0; k < 4; k++) {
            c->i[k] = s[16 + k];
            c->m[k] = s[20 + k];
            c->b[k] = s[24 + k];
            c->l[k] = s[28 + k];
        }
        /* As the harness sets them: A.W = R0; A.X = R0.L (sign-extended). */
        c->aw[0] = s[32];
        c->ax[0] = (uint32_t)(int8_t)s[33];
        c->aw[1] = s[34];
        c->ax[1] = (uint32_t)(int8_t)s[35];
        bfin_set_astat(c, s[36]);
        c->rets = s[37];
        c->lc[0] = s[38];
        c->lc[1] = s[39];
        c->v_internal = 0;
        if (bfin_step(c, 1, NULL) != BFIN_STOP_BUDGET) {
            printf("case %u (%04x %04x): stopped\n", n, r.code[0], r.code[1]);
            bad++;
        }
        memcpy(got, c->r, 32);
        memcpy(got + 8, c->p, 32);
        for (int k = 0; k < 4; k++) {
            got[16 + k] = c->i[k];
            got[20 + k] = c->m[k];
            got[24 + k] = c->b[k];
            got[28 + k] = c->l[k];
        }
        got[32] = bfin_get_reg(c, 4, 1);
        got[33] = bfin_get_reg(c, 4, 0);
        got[34] = bfin_get_reg(c, 4, 3);
        got[35] = bfin_get_reg(c, 4, 2);
        got[36] = bfin_astat(c);
        got[37] = c->rets;
        got[38] = c->lc[0];
        got[39] = c->lc[1];
        for (int k = 0; k < 40; k++) {
            if (got[k] != r.out[k]) {
                printf("case %u (%04x %04x): %s %08x, GNU sim %08x\n", n,
                       r.code[0], r.code[1], names[k], got[k], r.out[k]);
                bad++;
            }
        }
        if (r.memcheck && memcmp(ram + (r.region - LO), r.memout, 256)) {
            printf("case %u (%04x %04x): memory\n", n, r.code[0], r.code[1]);
            bad++;
        }
        n++;
    }
    printf("%u cases, %u mismatches\n", n, bad);
    return bad != 0 || n == 0;
}
