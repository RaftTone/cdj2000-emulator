/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The fast core's side of tools/cdj_gui/bfin_diff.py:
 *
 *   bfin-diff-runner ELF EMIT CASES FIRST NCASES
 *
 * Runs the differential ELF on bf531 + the fast core. Each time a case
 * reaches EMIT (where GNU sim would write its block out) the block P5 points
 * at goes to stdout as: u32 case, u32 0, 416 bytes. A case the core rejects
 * (u32 case, u32 1) or that does not reach EMIT (u32 case, u32 2) is
 * recorded and the run restarts at the next case through FIRST, the index
 * _start dispatches on through the CASES table.
 */
#include "bf531.h"
#include <stdlib.h>
#include <string.h>

#define BLK 416

static uint8_t elf[64 << 20];

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s ELF EMIT CASES FIRST NCASES\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    size_t n = f ? fread(elf, 1, sizeof elf, f) : 0;
    uint32_t emit = strtoul(argv[2], 0, 0), cases = strtoul(argv[3], 0, 0);
    uint32_t first = strtoul(argv[4], 0, 0), entry;
    unsigned ncases = strtoul(argv[5], 0, 0), k = 0;
    bf531_host h = {0};
    bf531 *s = bf531_new(64u << 20, &h, NULL);
    uint32_t size;
    uint8_t *ram;

    if (!n || bf531_boot_elf(s, elf, n)) {
        return 2;
    }
    ram = (uint8_t *)bf531_sdram(s, &size);
    memcpy(&entry, elf + 24, 4);
    bfin_core *c = bf531_core(s);
    while (k < ncases) {
        uint32_t st[2];

        bfin_set_break(c, emit);
        bfin_stop stop = bf531_run(s, 100000);
        if (stop == BFIN_STOP_BREAK && bfin_get_pc(c) == emit) {
            st[0] = k++;
            st[1] = 0;
            fwrite(st, 4, 2, stdout);
            fwrite(ram + bfin_get_reg(c, 1, 5), 1, BLK, stdout);
            bfin_set_break(c, 0);
            bf531_run(s, 1);                    /* past the break */
            continue;
        }
        unsigned at = k;
        if (stop == BFIN_STOP_UNDEF) {
            for (unsigned j = 0; j < ncases; j++) {
                uint32_t a;

                memcpy(&a, ram + cases + 4 * j, 4);
                if (a <= bfin_trap_pc(c)) {
                    at = j;
                }
            }
        }
        if (at < k) {
            at = k;
        }
        st[0] = at;
        st[1] = stop == BFIN_STOP_UNDEF ? 1 : 2;
        fwrite(st, 4, 2, stdout);
        k = at + 1;
        memcpy(ram + first, &k, 4);
        bfin_reset(c, entry);
    }
    return 0;
}
