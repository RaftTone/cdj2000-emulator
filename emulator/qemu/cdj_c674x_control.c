/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Control-register read view and MVC reachability, SPRUFE8B July 2010.
 * Moved verbatim out of cdj_c674x.c.
 *
 * These helpers touch only the transactional prefix through control_ready[]
 * and cycles, so they never reach the struct tail past offsetof(CdjC674x,
 * loop).  The MVC *write* masks are not here: they also
 * update control_ready[] and post delayed IFR effects on the writeback queue,
 * so they stay with the execute packet's commit step in cdj_c674x.c.
 */
#include "cdj_c674x_control.h"

uint64_t cdj_c674x_timestamp(const CdjC674x *cpu)
{
    uint64_t origin = cpu->control_ready[16];
    if (origin == 0 || cpu->cycles <= origin) return 0;
    return cpu->cycles - origin;
}

uint32_t cdj_c674x_control_read(const CdjC674x *cpu, unsigned id)
{
    switch (id) {
    case 0:                         /* AMR */
        return cpu->control[id] & 0x03ffffffu;
    case 1:                         /* CSR */
        return cpu->control[id] & 0xffff03ffu;
    case 2:                         /* IFR */
        return cpu->control[id] & 0xfff2u;
    case 21:                        /* SSR, SPRUFE8B 2.9.13 */
        return cpu->control[id] & 0x3fu;
    case 4:                         /* IER */
        return (cpu->control[id] & 0xfff2u) | 1u;
    case 5: {                       /* ISTP */
        uint32_t pending = cpu->control[2] & cpu->control[4] & 0xfff2u;
        unsigned highest = 0;
        while (pending && !(pending & 1)) {
            ++highest;
            pending >>= 1;
        }
        return (cpu->control[id] & 0xfffffc00u) | highest << 5;
    }
    case 6: case 7:                 /* IRP, NRP */
    case 13: case 14:               /* ILC, RILC */
    case 22: case 23:               /* GPLYA, GPLYB */
        return cpu->control[id];
    case 24:                        /* GFPGFR, SPRUFE8B 2.8.5 */
        return cpu->control[id] & 0x070000ffu;
    case 11:                        /* TSCH; TI dis6x 0x002c03e2 */
        return cpu->control[16];
    case 10:                        /* TSCL; TI dis6x 0x022803e2 */
        return (uint32_t)cdj_c674x_timestamp(cpu);
    case 27:                        /* ITSR */
        return (cpu->control[id] & 0x0000c6deu) |
               ((cpu->control[1] >> 1) & 1u);
    case 18: case 19: case 20:      /* FADCR, FAUCR, FMCR */
        /* All three reserve bits 31-27 and 15-11, "always read as 0":
         * SPRUFE8B Tables 2-25 (printed page 59), 2-26 (printed page 61) and
         * 2-27 (printed page 63), with Figures 2-29/2-30/2-31 marking those
         * fields R-0.  Every other bit is R/W on each register. */
        return cpu->control[id] & 0x07ff07ffu;
    case 26:                        /* TSR */
        return (cpu->control[id] & 0x0000c6deu) |
               (cpu->control[1] & 1u);
    default:
        return 0;
    }
}

bool cdj_c674x_control_read_supported(unsigned id)
{
    return id == 0 || id == 1 || id == 2 || id == 4 || id == 5 || id == 6 || id == 7 ||
           id == 10 || id == 11 || id == 13 || id == 14 ||
           id == 22 || id == 23 || id == 24 || id == 26 || id == 27 ||
           (id >= 18 && id <= 21);
}

bool cdj_c674x_control_write_supported(unsigned id)
{
    return id <= 7 || id == 10 || id == 13 || id == 14 ||
           id == 22 || id == 23 || id == 24 || id == 26 || id == 27 ||
           (id >= 18 && id <= 21);
}
