/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pack/unpack, shuffle, bit-manipulation, merge-byte and dual-result .L/.S/.M
 * instructions, TI SPRUFE8B (July 2010).
 *
 * BITC4 pp161-162, BITR 163-164, DEAL 231-232, DPACK2 254-255, DPACKX2 256-257,
 * LMBD 304-305, NORM 390-391, ROTL 414-415, SHFL 443-444, SHFL3 445-446,
 * SHLMB 449-450, SHRMB 455-456, SWAP4 555-556, UNPKHU4 559-560,
 * UNPKLU4 561-562, XPND2 568-569, XPND4 570-571.
 *
 * EVERY before/after register value asserted below is transcribed from the
 * Example block of that instruction's own entry, including the register names
 * and the "1 cycle / 2 cycles after instruction" heading that fixes the delay
 * slot.  No expected value comes from running this emulator.  The three places
 * where the manual prints no example are marked DERIVED and name the sentence
 * they follow.
 *
 * Instruction words come from ti-cgt-c6000 8.5.0 "asm6x -mv6740 -al", all but
 * the ROTL ucst5 form by way of tools/cdj_dsp/isa_probe, which does not probe
 * immediate operands; encode() rebuilds them from the Opcode figures and
 * assembler_words() asserts the two agree, so the field layout used for the
 * examples is the assembler's, not this file's guess. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static CdjC674x cpu;

static void reset(void)
{
    cdj_c674x_reset(&cpu, 0x1000);
}

static bool issue(uint32_t word)
{
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = cpu.pc}},
        .count = 1, .next_pc = cpu.pc + 4,
    };
    return cdj_c674x_execute(&cpu, &p, NULL, NULL, NULL);
}

static void run(uint32_t word)
{
    if (!issue(word)) {
        fprintf(stderr, "packbits %08x: unexpected fault \"%s\"\n", word,
                cpu.fault ? cpu.fault : "?");
        assert(false);
    }
}

static void rejects(uint32_t word, const char *reason)
{
    reset();
    if (issue(word) || strcmp(cpu.fault, reason)) {
        fprintf(stderr, "packbits %08x: expected \"%s\", got \"%s\"\n", word,
                reason, cpu.fault ? cpu.fault : "execution");
        assert(false);
    }
}

/* One cycle of nothing, so a two-cycle instruction's delay slot elapses.
 * SPRUFE8B printed page 388: an all-zero word is NOP 1. */
static void nop(void)
{
    run(0);
}

/* SPRUFE8B Figure D-1/E-1/F-1 field layout shared by every predicable row
 * here: creg 31-29, z 28, dst 27-23, src2 22-18, src1 17-13, x 12, the
 * instruction's own bits 11-2, s 1, p 0. */
static uint32_t encode(unsigned dst, unsigned src2, unsigned src1, unsigned x,
                       unsigned oplow, unsigned side)
{
    return dst << 23 | src2 << 18 | src1 << 13 | x << 12 | oplow | side << 1;
}

/* The nonconditional .L forms: bits 31-28 are the literal 0001 opcode field.
 * DPACK2/DPACKX2 (printed pages 254, 256) put a 4-bit dst in bits 27-24 and
 * reserve bit 23; SHFL3 (445) spends bits 27-23 on dst. */
static uint32_t encode_dual(unsigned dst_even, unsigned src2, unsigned src1,
                            unsigned x, unsigned oplow, unsigned side)
{
    return 1u << 28 | dst_even << 23 | src2 << 18 | src1 << 13 | x << 12 |
           oplow | side << 1;
}

#define UNPKHU4_L 0x358u
#define UNPKLU4_L 0x358u
#define SWAP4_L   0x358u
#define UNPKHU4_S 0xf20u
#define UNPKLU4_S 0xf20u
#define M_UNARY   0x0f0u   /* BITR/BITC4/DEAL/SHFL/XPND2/XPND4 share bits 11-2 */
#define ROTL_REG  0x770u
#define ROTL_CST  0x7b0u
#define LMBD_REG  0xd78u
#define NORM_SC   0xc78u
#define NORM_LONG 0xc18u
#define SHLMB_L   0xc38u
#define SHLMB_S   0xe70u
#define SHRMB_L   0xc58u
#define SHRMB_S   0xeb0u
#define DPACK2_OP 0x698u
#define DPACKX2_OP 0x678u
#define SHFL3_OP  0x6d8u
/* Bits 17-13 sub-select the src2-only operations (each entry's Opcode figure). */
#define OP_SWAP4   1u
#define OP_UNPKLU4 2u
#define OP_UNPKHU4 3u
#define OP_XPND4   0x18u
#define OP_XPND2   0x19u
#define OP_SHFL    0x1cu
#define OP_DEAL    0x1du
#define OP_BITC4   0x1eu
#define OP_BITR    0x1fu
#define OP_NORM    0u

/* Every word ti-cgt-c6000 8.5.0 emitted for these rows, against the same word
 * rebuilt from the manual's Opcode figure.  A disagreement means the field
 * layout the examples below rely on is wrong. */
static void assembler_words(void)
{
    static const struct { uint32_t word; uint32_t built; const char *asm_line; } rows[] = {
        {0x02986358u, 0, "UNPKHU4 .L1 A6, A5"},
        {0x0298635Au, 0, "UNPKHU4 .L2 B6, B5"},
        {0x02986F20u, 0, "UNPKHU4 .S1 A6, A5"},
        {0x02986F22u, 0, "UNPKHU4 .S2 B6, B5"},
        {0x02984358u, 0, "UNPKLU4 .L1 A6, A5"},
        {0x0298435Au, 0, "UNPKLU4 .L2 B6, B5"},
        {0x02984F20u, 0, "UNPKLU4 .S1 A6, A5"},
        {0x02984F22u, 0, "UNPKLU4 .S2 B6, B5"},
        {0x02982358u, 0, "SWAP4 .L1 A6, A5"},
        {0x0298235Au, 0, "SWAP4 .L2 B6, B5"},
        {0x029BE0F0u, 0, "BITR .M1 A6, A5"},
        {0x029BE0F2u, 0, "BITR .M2 B6, B5"},
        {0x029BC0F0u, 0, "BITC4 .M1 A6, A5"},
        {0x029BA0F0u, 0, "DEAL .M1 A6, A5"},
        {0x029B80F0u, 0, "SHFL .M1 A6, A5"},
        {0x029B20F0u, 0, "XPND2 .M1 A6, A5"},
        {0x029B00F0u, 0, "XPND4 .M1 A6, A5"},
        {0x02988770u, 0, "ROTL .M1 A6, A4, A5"},
        {0x02988772u, 0, "ROTL .M2 B6, B4, B5"},
        {0x029207B0u, 0, "ROTL .M1 A4,10h,A5"},   /* the ucst5 opfield */
        {0x02988D78u, 0, "LMBD .L1 A4, A6, A5"},
        {0x02980C78u, 0, "NORM .L1 A6, A5"},
        {0x02980C18u, 0, "NORM .L1 A7:A6, A5"},
        {0x02988C38u, 0, "SHLMB .L1 A4, A6, A5"},
        {0x02988E70u, 0, "SHLMB .S1 A4, A6, A5"},
        {0x02988C58u, 0, "SHRMB .L1 A4, A6, A5"},
        {0x02988EB0u, 0, "SHRMB .S1 A4, A6, A5"},
        {0x14188698u, 0, "DPACK2 .L1 A4, A6, A9:A8"},
        {0x14188678u, 0, "DPACKX2 .L1 A4, A6, A9:A8"},
        {0x141886D8u, 0, "SHFL3 .L1 A4, A6, A9:A8"},
    };
    uint32_t built[] = {
        encode(5, 6, OP_UNPKHU4, 0, UNPKHU4_L, 0),
        encode(5, 6, OP_UNPKHU4, 0, UNPKHU4_L, 1),
        encode(5, 6, OP_UNPKHU4, 0, UNPKHU4_S, 0),
        encode(5, 6, OP_UNPKHU4, 0, UNPKHU4_S, 1),
        encode(5, 6, OP_UNPKLU4, 0, UNPKLU4_L, 0),
        encode(5, 6, OP_UNPKLU4, 0, UNPKLU4_L, 1),
        encode(5, 6, OP_UNPKLU4, 0, UNPKLU4_S, 0),
        encode(5, 6, OP_UNPKLU4, 0, UNPKLU4_S, 1),
        encode(5, 6, OP_SWAP4, 0, SWAP4_L, 0),
        encode(5, 6, OP_SWAP4, 0, SWAP4_L, 1),
        encode(5, 6, OP_BITR, 0, M_UNARY, 0),
        encode(5, 6, OP_BITR, 0, M_UNARY, 1),
        encode(5, 6, OP_BITC4, 0, M_UNARY, 0),
        encode(5, 6, OP_DEAL, 0, M_UNARY, 0),
        encode(5, 6, OP_SHFL, 0, M_UNARY, 0),
        encode(5, 6, OP_XPND2, 0, M_UNARY, 0),
        encode(5, 6, OP_XPND4, 0, M_UNARY, 0),
        encode(5, 6, 4, 0, ROTL_REG, 0),
        encode(5, 6, 4, 0, ROTL_REG, 1),
        encode(5, 4, 0x10, 0, ROTL_CST, 0),
        encode(5, 6, 4, 0, LMBD_REG, 0),
        encode(5, 6, OP_NORM, 0, NORM_SC, 0),
        encode(5, 6, OP_NORM, 0, NORM_LONG, 0),
        encode(5, 6, 4, 0, SHLMB_L, 0),
        encode(5, 6, 4, 0, SHLMB_S, 0),
        encode(5, 6, 4, 0, SHRMB_L, 0),
        encode(5, 6, 4, 0, SHRMB_S, 0),
        encode_dual(8, 6, 4, 0, DPACK2_OP, 0),
        encode_dual(8, 6, 4, 0, DPACKX2_OP, 0),
        encode_dual(8, 6, 4, 0, SHFL3_OP, 0),
    };
    assert(sizeof(built) / sizeof(built[0]) == sizeof(rows) / sizeof(rows[0]));
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (built[i] != rows[i].word) {
            fprintf(stderr, "%s: asm6x %08x, figure %08x\n", rows[i].asm_line,
                    rows[i].word, built[i]);
            assert(false);
        }
        /* And each word must actually reach a dispatch row.  Arm selection is
         * first-match-wins, so this is the check that no earlier row claims
         * these encodings; the expected values are asserted per instruction
         * below, from the manual's examples. */
        reset();
        run(rows[i].word);
    }
}

/* One src2-only instruction whose dst is written in E1 (Delay Slots 0). */
static void single(unsigned side, unsigned dst, unsigned src2reg, unsigned op,
                   unsigned oplow, uint32_t before, uint32_t after)
{
    reset();
    cpu.r[side][src2reg] = before;
    cpu.r[side][dst] = 0xdeadbeefu;     /* so an expected 0 is still a write */
    run(encode(dst, src2reg, op, 0, oplow, side));
    assert(!cpu.load_count);            /* nothing deferred: single-cycle */
    assert(cpu.r[side][dst] == after);
    assert(cpu.r[side][src2reg] == before);   /* src2 unchanged */
}

/* One src2-only instruction whose dst is written in E2 (Delay Slots 1). */
static void two_cycle(unsigned side, unsigned dst, unsigned src2reg,
                      unsigned op, uint32_t before, uint32_t after)
{
    reset();
    cpu.r[side][src2reg] = before;
    cpu.r[side][dst] = 0xdeadbeefu;
    run(encode(dst, src2reg, op, 0, M_UNARY, side));
    /* "2 cycles after instruction": one cycle later dst still holds its old
     * value and the write is still in flight. */
    assert(cpu.load_count == 1 && cpu.r[side][dst] == 0xdeadbeefu);
    nop();
    assert(cpu.r[side][dst] == after);
    assert(cpu.r[side][src2reg] == before);
}

static void unpack_and_swap(void)
{
    /* UNPKHU4 printed page 560, Examples 1 and 2:
     *   UNPKHU4 .L1 A1,A2   A1 = 9E 52 6E 30h -> A2 = 00 9E 00 52h
     *   UNPKHU4 .L2 B17,B18 B17 = 11 05 69 34h -> B18 = 00 11 00 05h */
    single(0, 2, 1, OP_UNPKHU4, UNPKHU4_L, 0x9E526E30u, 0x009E0052u);
    single(1, 18, 17, OP_UNPKHU4, UNPKHU4_L, 0x11056934u, 0x00110005u);
    /* The .S unit opcode figure on printed page 559 is the same instruction. */
    single(0, 2, 1, OP_UNPKHU4, UNPKHU4_S, 0x9E526E30u, 0x009E0052u);
    single(1, 18, 17, OP_UNPKHU4, UNPKHU4_S, 0x11056934u, 0x00110005u);
    /* UNPKLU4 printed page 562, Examples 1 and 2:
     *   UNPKLU4 .L1 A1,A2   A1 = 9E 52 6E 30h -> A2 = 00 6E 00 30h
     *   UNPKLU4 .L2 B17,B18 B17 = 11 05 69 34h -> B18 = 00 69 00 34h */
    single(0, 2, 1, OP_UNPKLU4, UNPKLU4_L, 0x9E526E30u, 0x006E0030u);
    single(1, 18, 17, OP_UNPKLU4, UNPKLU4_L, 0x11056934u, 0x00690034u);
    single(0, 2, 1, OP_UNPKLU4, UNPKLU4_S, 0x9E526E30u, 0x006E0030u);
    single(1, 18, 17, OP_UNPKLU4, UNPKLU4_S, 0x11056934u, 0x00690034u);
    /* SWAP4 printed page 556, Example:
     *   SWAP4 .L1 A1,A2     A1 = 9E 52 6E 30h -> A2 = 52 9E 30 6Eh */
    single(0, 2, 1, OP_SWAP4, SWAP4_L, 0x9E526E30u, 0x529E306Eu);
    single(1, 2, 1, OP_SWAP4, SWAP4_L, 0x9E526E30u, 0x529E306Eu);
}

static void m_unary(void)
{
    /* BITR printed page 164:   BITR .M2 B4,B5   B4 = A6E2 C179h -> B5 = 9E83 4765h */
    two_cycle(1, 5, 4, OP_BITR, 0xA6E2C179u, 0x9E834765u);
    /* BITC4 printed page 162:  BITC4 .M1 A1,A2  A1 = 9E 52 6E 30h -> A2 = 05 03 05 02h */
    two_cycle(0, 2, 1, OP_BITC4, 0x9E526E30u, 0x05030502u);
    /* DEAL printed page 232:   DEAL .M1 A1,A2   A1 = 9E52 6E30h -> A2 = B174 6CA4h */
    two_cycle(0, 2, 1, OP_DEAL, 0x9E526E30u, 0xB1746CA4u);
    /* SHFL printed page 444:   SHFL .M1 A1,A2   A1 = B174 6CA4h -> A2 = 9E52 6E30h
     * (printed page 231: "DEAL is the exact inverse of SHFL"). */
    two_cycle(0, 2, 1, OP_SHFL, 0xB1746CA4u, 0x9E526E30u);
    /* XPND2 printed page 569, Examples 1 and 2:
     *   XPND2 .M1 A1,A2  A1 = B174 6CA1h (2 LSBs 01) -> A2 = 0000 FFFFh
     *   XPND2 .M2 B1,B2  B1 = 0000 0003h (2 LSBs 11) -> B2 = FFFF FFFFh */
    two_cycle(0, 2, 1, OP_XPND2, 0xB1746CA1u, 0x0000FFFFu);
    two_cycle(1, 2, 1, OP_XPND2, 0x00000003u, 0xFFFFFFFFu);
    /* XPND4 printed page 571, Examples 1 and 2:
     *   XPND4 .M1 A1,A2  A1 = B174 6CA4h (4 LSBs 0100) -> A2 = 00 FF 00 00h
     *   XPND4 .M2 B1,B2  B1 = 0000 000Ah (4 LSBs 1010) -> B2 = FF 00 FF 00h */
    two_cycle(0, 2, 1, OP_XPND4, 0xB1746CA4u, 0x00FF0000u);
    two_cycle(1, 2, 1, OP_XPND4, 0x0000000Au, 0xFF00FF00u);
}

static void rotl(void)
{
    /* ROTL printed page 415, Example 1:
     *   ROTL .M2 B2,B4,B5   B2 = A6E2 C179h, B4 = 1458 3B69h -> B5 = C582 F34Dh
     * ("2 cycles after instruction"; bits 31-5 of src1 are ignored, and 1458
     * 3B69h exercises that.) */
    reset();
    cpu.r[1][2] = 0xA6E2C179u;
    cpu.r[1][4] = 0x14583B69u;
    cpu.r[1][5] = 0xdeadbeefu;
    run(encode(5, 2, 4, 0, ROTL_REG, 1));
    assert(cpu.load_count == 1 && cpu.r[1][5] == 0xdeadbeefu);
    nop();
    assert(cpu.r[1][5] == 0xC582F34Du);
    assert(cpu.r[1][2] == 0xA6E2C179u && cpu.r[1][4] == 0x14583B69u);
    /* ROTL printed page 415, Example 2, the ucst5 opfield:
     *   ROTL .M1 A4,10h,A5  A4 = 187A 65FCh -> A5 = 65FC 187Ah */
    reset();
    cpu.r[0][4] = 0x187A65FCu;
    cpu.r[0][5] = 0xdeadbeefu;
    run(encode(5, 4, 0x10, 0, ROTL_CST, 0));
    assert(cpu.load_count == 1 && cpu.r[0][5] == 0xdeadbeefu);
    nop();
    assert(cpu.r[0][5] == 0x65FC187Au);
    /* DERIVED, no manual example: printed page 414 says ROTL "rotates the
     * 32-bit value of src2 to the left" by src1's five least-significant bits,
     * so a rotate of zero is src2 itself.  The entry's own
     * "(src2 << src1) | (src2 >> (32 - src1))" has no value at src1 = 0. */
    reset();
    cpu.r[0][4] = 0x187A65FCu;
    run(encode(5, 4, 0, 0, ROTL_CST, 0));
    nop();
    assert(cpu.r[0][5] == 0x187A65FCu);
}

static void lmbd(void)
{
    /* LMBD printed page 305:
     *   LMBD .L1 A1,A2,A3   A1 = 0000 0001h, A2 = 009E 3A81h -> A3 = 0000 0008h
     * ("1 cycle after instruction"). */
    reset();
    cpu.r[0][1] = 0x00000001u;
    cpu.r[0][2] = 0x009E3A81u;
    run(encode(3, 2, 1, 0, LMBD_REG, 0));
    assert(!cpu.load_count && cpu.r[0][3] == 0x00000008u);
    /* Printed page 304's three diagrams, transcribed as they stand: searching
     * for 0 in 01xx...x returns 0, searching for 1 in 0000 1xx...x returns 4,
     * and searching for 0 in all-ones returns 32.  The x bits are free, so
     * each is asserted with them zero and with them one.  The last row is
     * DERIVED - the manual draws the not-found case only for a 0 search, and
     * printed page 305's Execution line makes lmb1 the same function of the
     * other bit value. */
    static const struct { uint32_t src1, src2, dst; } diagrams[] = {
        {0, 0x40000000u, 0}, {0, 0x7fffffffu, 0},
        {1, 0x08000000u, 4}, {1, 0x0fffffffu, 4},
        {0, 0xffffffffu, 32},
        {1, 0x00000000u, 32},   /* DERIVED, see above */
    };
    for (unsigned i = 0; i < sizeof(diagrams) / sizeof(diagrams[0]); ++i) {
        reset();
        cpu.r[0][1] = diagrams[i].src1;
        cpu.r[0][2] = diagrams[i].src2;
        cpu.r[0][3] = 0xdeadbeefu;
        run(encode(3, 2, 1, 0, LMBD_REG, 0));
        assert(cpu.r[0][3] == diagrams[i].dst);
    }
}

static void norm(void)
{
    /* NORM printed page 391, Examples 1 and 2 ("1 cycle after instruction"):
     *   NORM .L1 A1,A2   A1 = 02A3 469Fh -> A2 = 0000 0005h (5)
     *   NORM .L1 A1,A2   A1 = FFFF F25Ah -> A2 = 0000 0013h (19) */
    single(0, 2, 1, OP_NORM, NORM_SC, 0x02A3469Fu, 0x00000005u);
    single(0, 2, 1, OP_NORM, NORM_SC, 0xFFFFF25Au, 0x00000013u);
    /* Printed page 390's four diagrams: 01xx... is 0, 0000 1xx... is 3,
     * all-ones-but-bit-0 is 30, and all-ones is 31. */
    single(0, 2, 1, OP_NORM, NORM_SC, 0x40000000u, 0);
    single(0, 2, 1, OP_NORM, NORM_SC, 0x08000000u, 3);
    single(0, 2, 1, OP_NORM, NORM_SC, 0xfffffffeu, 30);
    single(0, 2, 1, OP_NORM, NORM_SC, 0xffffffffu, 31);
    /* NORM printed page 391, Example 3, the slong opfield:
     *   NORM .L1 A1:A0,A3   A0 = 0000 0007h, A1 = 0000 0000h -> A3 = 0000 0024h (36) */
    reset();
    cpu.r[0][0] = 0x00000007u;
    cpu.r[0][1] = 0x00000000u;
    run(encode(3, 0, OP_NORM, 0, NORM_LONG, 0));
    assert(!cpu.load_count && cpu.r[0][3] == 0x00000024u);
    /* Only bits 7-0 of the odd register are part of a 40-bit long (SPRUFE8B
     * 2.3.1), so the same example with the rest of A1 set must still be 36. */
    reset();
    cpu.r[0][0] = 0x00000007u;
    cpu.r[0][1] = 0xffffff00u;
    run(encode(3, 0, OP_NORM, 0, NORM_LONG, 0));
    assert(cpu.r[0][3] == 0x00000024u);
    /* An odd src2 names no pair, and the .L long-data input is local-only, as
     * for the other 40-bit .L forms.  Both stay fail-closed. */
    rejects(encode(3, 1, OP_NORM, 0, NORM_LONG, 0), "invalid long register pair");
    rejects(encode(3, 0, OP_NORM, 1, NORM_LONG, 0),
            "cross-path long operand not supported");
}

static void merge_byte(void)
{
    /* SHLMB printed page 450, Examples 1 and 2 ("1 cycle after instruction"):
     *   SHLMB .L1 A2,A8,A9   A2 = 3789 F23Ah, A8 = 04B8 4975h -> A9 = B849 7537h
     *   SHLMB .S2 B2,B8,B12  B2 = 0124 2451h, B8 = 01A6 A051h -> B12 = A6A0 5101h
     * SHRMB printed page 456, Examples 1 and 2:
     *   SHRMB .L1 A2,A8,A9   A2 = 3789 F23Ah, A8 = 04B8 4975h -> A9 = 3A04 B849h
     *   SHRMB .S2 B2,B8,B12  B2 = 0124 2451h, B8 = 01A6 A051h -> B12 = 5101 A6A0h */
    static const struct {
        unsigned side, dst, src1reg, src2reg, oplow;
        uint32_t src1, src2, after;
    } rows[] = {
        {0, 9, 2, 8, SHLMB_L, 0x3789F23Au, 0x04B84975u, 0xB8497537u},
        {1, 12, 2, 8, SHLMB_S, 0x01242451u, 0x01A6A051u, 0xA6A05101u},
        {0, 9, 2, 8, SHRMB_L, 0x3789F23Au, 0x04B84975u, 0x3A04B849u},
        {1, 12, 2, 8, SHRMB_S, 0x01242451u, 0x01A6A051u, 0x5101A6A0u},
        /* The same two examples on the other unit of each entry's pair: both
         * Opcode figures on printed pages 449 and 455 describe one operation. */
        {0, 9, 2, 8, SHLMB_S, 0x3789F23Au, 0x04B84975u, 0xB8497537u},
        {1, 12, 2, 8, SHLMB_L, 0x01242451u, 0x01A6A051u, 0xA6A05101u},
        {0, 9, 2, 8, SHRMB_S, 0x3789F23Au, 0x04B84975u, 0x3A04B849u},
        {1, 12, 2, 8, SHRMB_L, 0x01242451u, 0x01A6A051u, 0x5101A6A0u},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        reset();
        cpu.r[rows[i].side][rows[i].src1reg] = rows[i].src1;
        cpu.r[rows[i].side][rows[i].src2reg] = rows[i].src2;
        run(encode(rows[i].dst, rows[i].src2reg, rows[i].src1reg, 0,
                   rows[i].oplow, rows[i].side));
        assert(!cpu.load_count);
        assert(cpu.r[rows[i].side][rows[i].dst] == rows[i].after);
        /* src2 is the cross-capable operand: the same word with x = 1 must
         * read the other register file instead (SPRUFE8B 2.3.2). */
        reset();
        cpu.r[rows[i].side][rows[i].src1reg] = rows[i].src1;
        cpu.r[rows[i].side ^ 1][rows[i].src2reg] = rows[i].src2;
        run(encode(rows[i].dst, rows[i].src2reg, rows[i].src1reg, 1,
                   rows[i].oplow, rows[i].side));
        assert(cpu.r[rows[i].side][rows[i].dst] == rows[i].after);
    }
}

static void dual_result(void)
{
    /* SHFL3 printed page 446 ("1 cycle after instruction"):
     *   SHFL3 .L1 A0,A1,A3:A2   A0 = 8765 4321h, A1 = 1234 5678h
     *                        -> A2 = 7E17 9306h, A3 = 0000 8C11h */
    reset();
    cpu.r[0][0] = 0x87654321u;
    cpu.r[0][1] = 0x12345678u;
    run(encode_dual(2, 1, 0, 0, SHFL3_OP, 0));
    assert(!cpu.load_count);
    assert(cpu.r[0][2] == 0x7E179306u && cpu.r[0][3] == 0x00008C11u);
    /* DPACK2 printed page 255:
     *   DPACK2 .L1 A0,A1,A3:A2  A0 = 8765 4321h, A1 = 1234 5678h
     *                        -> A2 = 4321 5678h, A3 = 8765 1234h */
    reset();
    cpu.r[0][0] = 0x87654321u;
    cpu.r[0][1] = 0x12345678u;
    run(encode_dual(2, 1, 0, 0, DPACK2_OP, 0));
    assert(!cpu.load_count);
    assert(cpu.r[0][2] == 0x43215678u && cpu.r[0][3] == 0x87651234u);
    /* DPACKX2 printed page 257, Example 1:
     *   DPACKX2 .L1 A0,A1,A3:A2 A0 = 8765 4321h, A1 = 1234 5678h
     *                        -> A2 = 4321 1234h, A3 = 5678 8765h */
    reset();
    cpu.r[0][0] = 0x87654321u;
    cpu.r[0][1] = 0x12345678u;
    run(encode_dual(2, 1, 0, 0, DPACKX2_OP, 0));
    assert(cpu.r[0][2] == 0x43211234u && cpu.r[0][3] == 0x56788765u);
    /* DPACKX2 printed page 257, Example 2, the cross path:
     *   DPACKX2 .L1X A0,B0,A3:A2 A0 = 3FFF 8000h, B0 = 4000 7777h
     *                        -> A2 = 8000 4000h, A3 = 7777 3FFFh */
    reset();
    cpu.r[0][0] = 0x3FFF8000u;
    cpu.r[1][0] = 0x40007777u;
    run(encode_dual(2, 0, 0, 1, DPACKX2_OP, 0));
    assert(cpu.r[0][2] == 0x80004000u && cpu.r[0][3] == 0x77773FFFu);
    /* The same four on the B file: s = 1 changes nothing but the register
     * file (SPRUFE8B 3.6, the s bit). */
    reset();
    cpu.r[1][0] = 0x87654321u;
    cpu.r[1][1] = 0x12345678u;
    run(encode_dual(2, 1, 0, 0, DPACK2_OP, 1));
    assert(cpu.r[1][2] == 0x43215678u && cpu.r[1][3] == 0x87651234u);
    /* Nonconditional: printed pages 254 and 256 say "This instruction executes
     * unconditionally", and 445 that it "cannot be predicated".  Bits 31-29 are
     * the opcode's own, so no predicate register may suppress the write. */
    reset();
    for (unsigned p = 0; p < 3; ++p) { cpu.r[0][p] = 0; cpu.r[1][p] = 0; }
    cpu.r[0][0] = 0x87654321u;
    cpu.r[0][1] = 0x12345678u;
    run(encode_dual(2, 1, 0, 0, DPACK2_OP, 0));
    assert(cpu.r[0][2] == 0x43215678u && cpu.r[0][3] == 0x87651234u);
    /* SHFL3 is the one of the three whose dst field can name an odd register;
     * that is not a pair, so it stays fail-closed. */
    rejects(encode_dual(3, 1, 0, 0, SHFL3_OP, 0),
            "invalid long register pair");
    /* The words the assembler emitted, executed as words: these three left the
     * unimplemented list in tests/cstub/c674x-uncond.c and must now run. */
    reset();
    cpu.r[0][4] = 0x87654321u;
    cpu.r[0][6] = 0x12345678u;
    run(0x14188698u);                   /* DPACK2 .L1 A4, A6, A9:A8 */
    assert(cpu.r[0][8] == 0x43215678u && cpu.r[0][9] == 0x87651234u);
    reset();
    cpu.r[0][4] = 0x87654321u;
    cpu.r[0][6] = 0x12345678u;
    run(0x14188678u);                   /* DPACKX2 .L1 A4, A6, A9:A8 */
    assert(cpu.r[0][8] == 0x43211234u && cpu.r[0][9] == 0x56788765u);
    reset();
    cpu.r[0][4] = 0x87654321u;
    cpu.r[0][6] = 0x12345678u;
    run(0x141886D8u);                   /* SHFL3 .L1 A4, A6, A9:A8 */
    assert(cpu.r[0][8] == 0x7E179306u && cpu.r[0][9] == 0x00008C11u);
}

/* Predication (SPRUFE8B Table 3-1): creg 001 is B0.  A false predicate must
 * leave dst alone, on the single-cycle and the two-cycle rows alike, and must
 * not leave a delayed write in flight. */
static void predication(void)
{
    for (unsigned z = 0; z < 2; ++z) {
        reset();
        cpu.r[1][0] = z;                /* predicate false: B0 == 0 ^ z */
        cpu.r[0][1] = 0x9E526E30u;
        cpu.r[0][2] = 0x11223344u;
        run(1u << 29 | z << 28 |
            encode(2, 1, OP_SWAP4, 0, SWAP4_L, 0));
        assert(cpu.r[0][2] == 0x11223344u && !cpu.load_count);
        reset();
        cpu.r[1][0] = z;
        cpu.r[0][1] = 0x9E526E30u;
        cpu.r[0][2] = 0x11223344u;
        run(1u << 29 | z << 28 | encode(2, 1, OP_BITC4, 0, M_UNARY, 0));
        assert(!cpu.load_count);
        nop();
        assert(cpu.r[0][2] == 0x11223344u);
        /* The true half of the same pair still executes. */
        reset();
        cpu.r[1][0] = z ^ 1u;
        cpu.r[0][1] = 0x9E526E30u;
        run(1u << 29 | z << 28 | encode(2, 1, OP_BITC4, 0, M_UNARY, 0));
        nop();
        assert(cpu.r[0][2] == 0x05030502u);
    }
}

/* src2 is "xu4"/"xuint" on every row here: x = 1 reads the opposite file. */
static void cross_path(void)
{
    reset();
    cpu.r[1][1] = 0x9E526E30u;
    run(encode(2, 1, OP_SWAP4, 1, SWAP4_L, 0));
    assert(cpu.r[0][2] == 0x529E306Eu);
    reset();
    cpu.r[0][1] = 0x9E526E30u;
    run(encode(2, 1, OP_BITC4, 1, M_UNARY, 1));
    nop();
    assert(cpu.r[1][2] == 0x05030502u);
    /* ROTL's src1 is plain "uint" and stays local while src2 crosses. */
    reset();
    cpu.r[1][2] = 0xA6E2C179u;
    cpu.r[0][4] = 0x14583B69u;
    run(encode(5, 2, 4, 1, ROTL_REG, 0));
    nop();
    assert(cpu.r[0][5] == 0xC582F34Du);
    /* NORM's 32-bit src2 is "xsint" and LMBD's src2 "xuint", with LMBD's src1
     * a local "uint" (printed pages 390 and 304 opcode maps). */
    reset();
    cpu.r[1][1] = 0x02A3469Fu;
    cpu.r[0][2] = 0xdeadbeefu;
    run(encode(2, 1, OP_NORM, 1, NORM_SC, 0));
    assert(cpu.r[0][2] == 0x00000005u);
    reset();
    cpu.r[0][1] = 0x00000001u;
    cpu.r[1][2] = 0x009E3A81u;
    run(encode(3, 2, 1, 1, LMBD_REG, 0));
    assert(cpu.r[0][3] == 0x00000008u);
}

/* ---- the nonconditional .L dual ADD/SUB forms ---------------------------- */
static void dual_addsub(void)
{
    /* Figure D-3 word: 0001 in bits 31-28, dst 27-23, src2 22-18, src1 17-13,
     * x 12, opfield 11-5, 110 in bits 4-2, s 1, p 0.  These forms spend only
     * bits 27-24 on dst and reserve bit 23 as 0. */
    #define D3(dst, src2, src1, op, side) \
        (0x10000000u | (uint32_t)(dst) << 23 | (uint32_t)(src2) << 18 | \
         (uint32_t)(src1) << 13 | (uint32_t)(op) << 5 | 0x18u | \
         (uint32_t)(side) << 1)

    /* TRANSCRIBED, each from its entry's Example 1, all four with
     * A0 = 0700 C005h.  The ADD goes to dst_o and the SUB to dst_e in every
     * form, which is what these cases pin.
     *   ADDSUB   p132  A1 = FFFF FFFFh -> A2 0700 C006h  A3 0700 C004h
     *   ADDSUB2  p133  A1 = FFFF 0001h -> A2 0701 C004h  A3 06FF C006h
     *   SADDSUB  p427  A1 = FFFF FFFFh -> A2 0700 C006h  A3 0700 C004h
     *   SADDSUB2 p429  A1 = FFFF 0001h -> A2 0701 C004h  A3 06FF C006h */
    static const struct {
        unsigned op; uint32_t src2, dst_e, dst_o;
    } cases[] = {
        { 0x0c, 0xffffffffu, 0x0700c006u, 0x0700c004u },
        { 0x0d, 0xffff0001u, 0x0701c004u, 0x06ffc006u },
        { 0x0e, 0xffffffffu, 0x0700c006u, 0x0700c004u },
        { 0x0f, 0xffff0001u, 0x0701c004u, 0x06ffc006u },
    };
    for (unsigned i = 0; i < 4; ++i)
    for (unsigned side = 0; side < 2; ++side) {
        reset();
        cpu.r[side][0] = 0x0700c005u;
        cpu.r[side][1] = cases[i].src2;
        run(D3(2, 1, 0, cases[i].op, side));
        assert(cpu.r[side][2] == cases[i].dst_e);   /* dst_e, E1 */
        assert(cpu.r[side][3] == cases[i].dst_o);   /* dst_o, E1 */
    }

    /* SADDSUB REPORTS SATURATION AND SADDSUB2 DOES NOT, which is stated rather
     * than an asymmetry of convenience.  Printed page 427: "If either result
     * saturates, the L1 or L2 bit in SSR and the SAT bit in CSR are written one
     * cycle after the results are written to dst_o:dst_e."  Printed page 429
     * exempts the packed form: "This operation is performed on each halfword
     * separately.  This instruction does not affect the SAT bit in CSR or the
     * L1 or L2 bits in SSR."  Both halves are checked, because an
     * implementation setting the flags unconditionally would satisfy the first
     * and fail the second. */
    for (unsigned side = 0; side < 2; ++side) {
        reset();
        cpu.r[side][0] = 0x7fffffffu; cpu.r[side][1] = 0x7fffffffu;
        run(D3(2, 1, 0, 0x0e, side));                /* SADDSUB, add clamps */
        assert(cpu.r[side][3] == 0x7fffffffu);
        run(0); run(0);
        assert((cpu.control[1] >> 9) & 1u);           /* CSR.SAT */
        assert(cpu.control[21] == (1u << side));      /* SSR L1/L2 */

        reset();
        cpu.r[side][0] = 0x7fff7fffu; cpu.r[side][1] = 0x7fff7fffu;
        run(D3(2, 1, 0, 0x0f, side));                 /* SADDSUB2, halves clamp */
        assert(cpu.r[side][3] == 0x7fff7fffu);
        run(0); run(0);
        assert(!((cpu.control[1] >> 9) & 1u) && !cpu.control[21]);
    }
    #undef D3
}

int main(void)
{
    assembler_words();
    unpack_and_swap();
    m_unary();
    rotl();
    lmbd();
    norm();
    merge_byte();
    dual_result();
    predication();
    cross_path();
    printf("c674x packbits ok\n");
    dual_addsub();
    return 0;
}
