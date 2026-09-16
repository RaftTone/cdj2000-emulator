/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic CPU-time benchmark; not a firmware or end-to-end speed test.
 * Build from the repository root:
 * cc -O3 -I emulator/qemu tools/cdj_dsp/benchmark_core.c \
 *   emulator/qemu/cdj_c674x.c \
 *   emulator/qemu/emulator/qemu/cdj_c674x_mpy.c emulator/qemu/cdj_c674x_dotp.c emulator/qemu/cdj_c674x_packed8.c emulator/qemu/cdj_c674x_packed16.c emulator/qemu/cdj_c674x_packbits.c emulator/qemu/cdj_c674x_mpy32.c emulator/qemu/cdj_c674x_dp.c emulator/qemu/cdj_c674x_approx.c emulator/qemu/cdj_c674x_uncond.c \
 *   emulator/qemu/cdj_c674x_sp.c emulator/qemu/cdj_c674x_control.c \
 *   emulator/qemu/cdj_c674x_loop.c -o /tmp/cdj-core-bench
 * Run alternating baseline/candidate binaries on an otherwise idle host. */
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include "cdj_c674x.h"

int main(void)
{
    CdjC674x cpu;
    cdj_c674x_reset(&cpu, 0x1000);
    CdjC674xPacket packet = {
        .count = 1, .next_pc = 0x1004,
        .instructions = {{.word = (3u << 23) | (123u << 7) | 0x28,
                          .pc = 0x1000}},
    };
    clock_t start = clock();
    for (unsigned i = 0; i < 3000000; ++i)
        if (!cdj_c674x_execute(&cpu, &packet, NULL, NULL, NULL)) return 1;
    printf("execute %.6f s; packets=%llu reg=%u state=%zu prefix=%zu\n",
           (double)(clock() - start) / CLOCKS_PER_SEC,
           (unsigned long long)cpu.packets, cpu.r[0][3], sizeof(cpu),
           offsetof(CdjC674x, loop));

    CdjC674xLoop loop;
    cdj_c674x_loop_init(&loop, 8, 10000000);
    loop.sealed = true;
    loop.length = 48;
    for (unsigned i = 0; i < 48; ++i) {
        loop.count[i] = 1;
        loop.tags[i][0] = i;
    }
    uint32_t tags[8];
    unsigned count;
    bool post, drained;
    start = clock();
    for (unsigned i = 0; i < 3000000; ++i)
        if (!cdj_c674x_loop_issue(&loop, tags, &count, &post, &drained)) return 2;
    printf("loop %.6f s; cycles=%llu count=%u\n",
           (double)(clock() - start) / CLOCKS_PER_SEC,
           (unsigned long long)loop.cycle, count);
    return 0;
}
