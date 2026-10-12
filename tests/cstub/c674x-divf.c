/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runs TI RTS __c6xabi_divf (a flat image linked at 0x1000 by cl6x, entry
 * `fdiv` at 0x1220 -> argv[1]) against IEEE division. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c674x.h"

static uint8_t mem[0x40000] __attribute__((aligned(16)));
static bool rd(void *u, uint32_t a, uint32_t *v)
{ (void)u; if (a < 0x40000 && !(a & 3)) { memcpy(v, mem + a, 4); return true; } return false; }
static bool wr(void *u, uint32_t a, uint64_t v, unsigned sz, bool c)
{
    (void)u;
    if (a >= 0x40000 || a + sz > 0x40000) return false;
    if (c) for (unsigned i = 0; i < sz; i++) mem[a + i] = v >> (8 * i);
    return true;
}
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t call(uint32_t entry, float a, float b)
{
    CdjC674x c;
    cdj_c674x_reset(&c, entry);
    c.r[0][4] = bits(a); c.r[1][4] = bits(b);
    c.r[1][3] = 0x2000; c.r[1][15] = 0x3f000;
    for (long s = 0; s < 5000 && c.pc != 0x2000; s++)
        if (!cdj_c674x_step(&c, rd, wr, NULL)) { fprintf(stderr, "%s\n", c.fault); exit(1); }
    return c.r[0][4];
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(mem + 0x1000, 1, 0x2000, f) > 0);
    fclose(f);
    uint32_t entry = strtoul(argv[2], 0, 0);
    static const float t[][2] = {{1, 1024}, {1, 3}, {10, 4}, {-7, 2}, {3, .5f}, {1e10f, 3},
                                 {1, 1e-3f}, {355, 113}, {0, 5}, {5, 1}, {1, 7}, {123456.f, -7.f}};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint32_t got = call(entry, t[i][0], t[i][1]), want = bits(t[i][0] / t[i][1]);
        if (got != want) { fprintf(stderr, "%g/%g: %08x want %08x\n", t[i][0], t[i][1], got, want); return 1; }
    }
    puts("ok");
    return 0;
}
