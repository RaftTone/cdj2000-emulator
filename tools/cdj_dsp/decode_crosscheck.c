/* SPDX-License-Identifier: GPL-2.0-or-later
 * Decoder cross-check, our half.  Loads a DSP checkpoint, writes the raw
 * bytes of [START, END) to OUT.bin for an independent disassembler, and
 * sweeps the same range through cdj_c674x_fetch, printing one line per
 * instruction:
 *
 *   ADDRESS SIZE WORD compact|full parallel|serial FAMILY LOWERED
 *
 * LOWERED is the 32-bit word the issue loop executes for a compact
 * instruction it rewrites to a full encoding ("-" otherwise), so its
 * operands can be checked by disassembling that word too.
 * FAMILY is cdj_c674x_describe(): what the interpreter would execute.  A
 * fetch rejection prints "ADDRESS fetch-fault REASON" and skips to the next
 * fetch block.  tools/cdj_dsp/decode_crosscheck.py compares the two sides.
 * The idea is Stijn Jacobs' "make m1" decoder-vs-objdump diff from
 * github.com/Stijn-Jacobs/cdj-nxs2-qemu (commit 08d5cb1).
 *
 * Usage: decode-crosscheck CHECKPOINT START END OUT.bin
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_dsp_checkpoint.h"

#define L2_BASE 0x11800000u
#define L2_SIZE 0x40000u
#define SDRAM_BASE 0xc0000000u
#define SDRAM_SIZE 0x2000000u
static uint8_t l2[L2_SIZE], shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t l1d[CDJ_DSP_L1D_SIZE], sdram[SDRAM_SIZE];
static CdjDspCheckpointState state;

static const uint8_t *span(uint32_t address)
{
    if (address - L2_BASE < L2_SIZE) return l2 + (address - L2_BASE);
    if (address - SDRAM_BASE < SDRAM_SIZE) return sdram + (address - SDRAM_BASE);
    return NULL;
}

static bool read_word(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    const uint8_t *p = span(address);
    if (!p || (address & 3) || !span(address + 3)) return false;
    memcpy(value, p, 4);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s CHECKPOINT START END OUT.bin\n", argv[0]);
        return 2;
    }
    char error[256];
    if (!cdj_dsp_checkpoint_read_with_l1d(argv[1], &state, l2, sizeof l2,
                                          shared_ram, sizeof shared_ram,
                                          l1d, sizeof l1d, sdram, sizeof sdram,
                                          error, sizeof error)) {
        fprintf(stderr, "%s: %s\n", argv[1], error);
        return 2;
    }
    uint32_t start = strtoul(argv[2], NULL, 0), end = strtoul(argv[3], NULL, 0);
    if ((start & 31) || end <= start || !span(start) || !span(end - 1)) return 2;
    FILE *out = fopen(argv[4], "wb");
    if (!out || fwrite(span(start), 1, end - start, out) != end - start ||
        fclose(out)) return 2;
    for (uint32_t pc = start; pc < end;) {
        CdjC674x cpu;
        CdjC674xPacket packet;
        cdj_c674x_reset(&cpu, pc);
        if (!cdj_c674x_fetch(&cpu, read_word, NULL, &packet)) {
            printf("%08" PRIx32 " fetch-fault %s\n", pc, cpu.fault);
            pc = (pc & ~31u) + 32;
            continue;
        }
        for (unsigned i = 0; i < packet.count; ++i) {
            const CdjC674xInstruction *insn = &packet.instructions[i];
            if (insn->pc >= end) break;
            uint32_t lowered;
            const char *family = cdj_c674x_describe(insn, &lowered);
            char rewritten[16] = "-";
            if (insn->compact && strncmp(family, "compact-", 8))
                snprintf(rewritten, sizeof rewritten, "%08" PRIx32, lowered);
            printf("%08" PRIx32 " %u %0*" PRIx32 " %s %s %s %s\n", insn->pc,
                   insn->compact ? 2 : 4, insn->compact ? 4 : 8, insn->word,
                   insn->compact ? "compact" : "full",
                   i + 1 < packet.count ? "parallel" : "serial", family,
                   rewritten);
        }
        pc = packet.next_pc;
    }
    return 0;
}
