/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reproduce pending IRQ recognition in the interpreter's B IRP -> SPLOOP gap.
 * Only synthetic instruction packets are used; no firmware/media is required. */
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static uint32_t memory[64];

static bool read_word(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    if (address < 0x1000 || address >= 0x1100 || (address & 3))
        return false;
    *value = memory[(address - 0x1000) / 4];
    return true;
}

static bool check(bool functional)
{
    CdjC674x cpu;
    cdj_c674x_loop_set_functional_timing(functional);
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.control[27] = 0x4001; /* ITSR: saved GIE and SPLX restart. */
    cpu.control[1] |= 2;     /* CSR PGIE enables interrupt return. */
    cpu.control[6] = 0x1040; /* IRP points at the returned loop setup. */
    cpu.control[13] = 2;    /* ILC. */
    cpu.control[5] = 0x1000;
    cpu.control[4] = (1u << 4) | 3;
    cpu.r[0][4] = 11;
    memory[0] = 0x001800e2; /* B IRP, followed by five delay packets. */
    memory[16] = 0x0003a001; /* SPLOOP 2, parallel with MVK 77,A4. */
    memory[17] = (4u << 23) | (77u << 7) | 0x28;
    memory[18] = 0;
    memory[19] = 0x00034000; /* SPKERNEL. */
    if (!cdj_c674x_step(&cpu, read_word, NULL, NULL))
        return false;
    for (unsigned i = 0; i < 5; ++i) {
        if (!cdj_c674x_interrupt(&cpu, 0) ||
            !cdj_c674x_step(&cpu, read_word, NULL, NULL))
            return false;
    }
    if (cpu.pc != 0x1040 || cpu.loop_active || !(cpu.control[26] & 0x4000))
        return false;
    /* Recognition here must defer until the setup packet restores the loop.
     * Taking IRQ4 here loses SPLX; its parallel MVK then wrongly executes. */
    if (!cdj_c674x_interrupt(&cpu, 1u << 4) ||
        cpu.pc != 0x1040 || cpu.control[2] != (1u << 4))
        return false;
    if (!cdj_c674x_step(&cpu, read_word, NULL, NULL) ||
        !cpu.loop_active || cpu.r[0][4] != 11)
        return false;
    /* Pending IRQ remains latched and must subsequently be serviced. */
    if (cpu.control[2] != (1u << 4))
        return false;
    for (unsigned i = 0; i < 50; ++i) {
        if (!cdj_c674x_interrupt(&cpu, 0))
            return false;
        if (cpu.pc == 0x1080)
            return !(cpu.control[2] & (1u << 4));
        if (!cdj_c674x_step(&cpu, read_word, NULL, NULL))
            return false;
    }
    return false;
}

int main(void)
{
    unsigned failures = 0;
    for (unsigned functional = 0; functional < 2; ++functional) {
        bool ok = check(functional);
        printf("%s return-window pending IRQ preserved and parallel operation "
               "annulled functional=%u\n", ok ? "PASS" : "FAIL", functional);
        failures += !ok;
    }
    return failures ? 1 : 0;
}
