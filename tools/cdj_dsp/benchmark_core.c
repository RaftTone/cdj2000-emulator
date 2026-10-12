/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic CPU-time benchmark; not a firmware or end-to-end speed test.
 * Build from the repository root:
 * cc -O3 -I emulator/qemu tools/cdj_dsp/benchmark_core.c \
 *   emulator/qemu/cdj_c674x.c \
 *   emulator/qemu/cdj_c674x_mpy.c emulator/qemu/cdj_c674x_dotp.c emulator/qemu/cdj_c674x_packed8.c emulator/qemu/cdj_c674x_packed16.c emulator/qemu/cdj_c674x_packbits.c emulator/qemu/cdj_c674x_mpy32.c emulator/qemu/cdj_c674x_dp.c emulator/qemu/cdj_c674x_approx.c emulator/qemu/cdj_c674x_uncond.c \
 *   emulator/qemu/cdj_c674x_sp.c emulator/qemu/cdj_c674x_control.c \
 *   emulator/qemu/cdj_c674x_loop.c -lm -o /tmp/cdj-core-bench
 * Run alternating baseline/candidate binaries on an otherwise idle host.
 * The step workloads run a small loop from RAM through cdj_c674x_step with a
 * fetch-block hook, as the NXS board does; set CDJ_C674X_PACKET_CACHE=0 or
 * =decode to compare the packet cache's modes in one binary. */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "cdj_c674x.h"

static uint8_t ram[0x1000]; /* 0x1000..0x1fff: code, then data at 0x1100 */

static bool bench_read(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    if (address < 0x1000 || address > 0x1ffc || (address & 3)) return false;
    memcpy(value, ram + address - 0x1000, 4);
    return true;
}

static bool bench_write(void *opaque, uint32_t address, uint64_t value,
                        unsigned size, bool commit)
{
    (void)opaque;
    if (address < 0x1100 || address + size > 0x2000 || (address & (size - 1)))
        return false;
    if (commit) memcpy(ram + address - 0x1000, &value, size);
    return true;
}

static const uint8_t *bench_block(void *opaque, uint32_t block)
{
    (void)opaque;
    return block >= 0x1000 && block < 0x2000 ? ram + block - 0x1000 : NULL;
}

static void tick(void *opaque) { ++*(unsigned *)opaque; }

static bool between(void *opaque) { (void)opaque; return true; }

/* With CDJ_C674X_JIT=1, loop-buffer cycles and direct traces run compiled
 * through cdj_c674x_run (its between() is empty, the board's own work
 * aside). */
static double step_loop(const uint32_t *code, unsigned words, unsigned steps,
                        uint64_t *packets)
{
    memset(ram, 0, sizeof ram);
    memcpy(ram, code, words * 4);
    CdjC674x cpu;
    unsigned ticks = 0;
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][10] = 0x1100;
    cpu.cycle_tick = tick;
    cpu.cycle_opaque = &ticks;
    cdj_c674x_set_fetch_block(bench_read, bench_block);
    /* CDJ_BENCH_HORIZON=1: between() is a no-op the run may skip, as the
     * board's horizon lets it whenever nothing is due. */
    static CdjC674xHorizon horizon = {.until = UINT64_MAX};
    const char *h = getenv("CDJ_BENCH_HORIZON");
    cdj_c674x_set_horizon(h && !strcmp(h, "1") ? &horizon : NULL);
    clock_t start = clock();
    for (unsigned i = 0; i < steps; ++i) {
        {
            unsigned status;
            unsigned n = cdj_c674x_run(&cpu, bench_read, bench_write, NULL,
                                       steps - i, between, NULL, &status);
            if (status == CDJ_C674X_RUN_FAULT) {
                fprintf(stderr, "fault %s at %08x\n", cpu.fault, cpu.fault_pc);
                return -1;
            }
            if (n) {
                i += n - 1;
                continue;
            }
        }
        if (!cdj_c674x_step(&cpu, bench_read, bench_write, NULL)) {
            fprintf(stderr, "fault %s at %08x\n", cpu.fault, cpu.fault_pc);
            return -1;
        }
    }
    *packets = cpu.packets;
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

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

    /* Register-only loop: MVK, ADD, MVKH, CMPEQ, B, NOP 5 (.S1/.L1). */
    static const uint32_t alu[] = {
        1u << 23 | 0x1234u << 7 | 0x28,               /* MVK.S1 0x1234,A1 */
        3u << 23 | 3u << 18 | 1u << 13 | 0x78,        /* ADD.L1 A1,A3,A3 */
        5u << 23 | 0x5678u << 7 | 0x68,               /* MVKH.S1 ...,A5 */
        6u << 23 | 3u << 18 | 1u << 13 | 0xa78,       /* CMPEQ.L1 A1,A3,A6 */
        0x10,                                         /* B.S1 0x1000 */
        4u << 13,                                     /* NOP 5 */
    };
    /* The same with a load and a store in place of MVKH and CMPEQ. */
    static const uint32_t mem[] = {
        1u << 23 | 0x1234u << 7 | 0x28,
        3u << 23 | 3u << 18 | 1u << 13 | 0x78,
        4u << 23 | 10u << 18 | 1u << 9 | 6u << 4 | 4, /* LDW *+A10[0],A4 */
        3u << 23 | 10u << 18 | 1u << 13 | 1u << 9 | 7u << 4 | 4, /* STW */
        0x10,
        4u << 13,
    };
    /* An SPLOOP (ILC 64, II 4) of load, add, constant and store - the
     * shape of the NXS audio kernels, which run from the loop buffer -
     * then a branch back to reload ILC. */
    static const uint32_t sploop[] = {
        3u << 23 | 64u << 7 | 0x28 | 2,               /* MVK.S2 64,B3 */
        13u << 23 | 3u << 18 | 0x3a2,                 /* MVC.S2 B3,ILC */
        3u << 13,                                     /* NOP 4 */
        3u << 23 | 0x38000,                           /* SPLOOP 4 */
        4u << 23 | 10u << 18 | 1u << 9 | 6u << 4 | 4, /* LDW *+A10[0],A4 */
        3u << 23 | 3u << 18 | 1u << 13 | 0x78,        /* ADD.L1 A1,A3,A3 */
        1u << 23 | 0x1234u << 7 | 0x28,               /* MVK.S1 0x1234,A1 */
        3u << 23 | 10u << 18 | 1u << 13 | 1u << 9 | 7u << 4 | 4, /* STW */
        0x34000,                                      /* SPKERNEL */
        (0x1fffffu & (uint32_t)-8) << 7 | 0x10,       /* B.S1 0x1000 */
        4u << 13,                                     /* NOP 5 */
    };
    /* The stage-1 memcpy kernel (dsp_stage1_memcpy, 0x11804838), the
     * hottest loop in NXS playback: SPLOOP 2 of LDNDW and STNDW. */
    static const uint32_t copy[] = {
        4u << 23 | 0x1400u << 7 | 0x28 | 2,           /* MVK.S2 0x1400,B4 */
        5u << 23 | 0x1800u << 7 | 0x28,               /* MVK.S1 0x1800,A5 */
        3u << 23 | 64u << 7 | 0x28 | 2,               /* MVK.S2 64,B3 */
        13u << 23 | 3u << 18 | 0x3a2,                 /* MVC.S2 B3,ILC */
        3u << 13,                                     /* NOP 4 */
        1u << 23 | 0x38000,                           /* SPLOOP 2 */
        7u << 23 | 4u << 18 | 1u << 13 | 11u << 9 | 0x100 | 1u << 7 |
            2u << 4 | 4,                              /* LDNDW *B4++,A7:A6 */
        3u << 13,                                     /* NOP 4 */
        0x34001,                                      /* SPKERNEL || */
        7u << 23 | 5u << 18 | 1u << 13 | 11u << 9 | 0x100 |
            7u << 4 | 4,                              /* STNDW A7:A6,*A5++ */
        (0x1fffffu & (uint32_t)-8) << 7 | 0x10,       /* B.S1 0x1000 */
        4u << 13,                                     /* NOP 5 */
    };
    /* The same kernel without pointer updates and with ILC 30000: nearly
     * every step is a steady-state loop-buffer cycle. */
    uint32_t steady[12];
    memcpy(steady, copy, sizeof steady);
    steady[2] = 3u << 23 | 30000u << 7 | 0x28 | 2;  /* MVK.S2 30000,B3 */
    steady[6] = (steady[6] & ~(15u << 9)) | 1u << 9; /* LDNDW *+B4[1] */
    steady[9] = (steady[9] & ~(15u << 9)) | 1u << 9; /* STNDW *+A5[1] */
    uint64_t packets;
    double t = step_loop(alu, 6, 30000000, &packets);
    printf("step-alu %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    t = step_loop(mem, 6, 30000000, &packets);
    printf("step-mem %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    t = step_loop(sploop, 11, 30000000, &packets);
    printf("step-sploop %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    t = step_loop(copy, 12, 30000000, &packets);
    printf("step-memcpy %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    t = step_loop(steady, 12, 30000000, &packets);
    printf("step-kernel %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    return 0;
}
