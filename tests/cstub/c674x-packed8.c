/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed 8-bit (4x8) instructions: semantics, dispatch and pipeline latency.
 *
 * EVIDENCE RULES FOLLOWED HERE
 *  - Every expected register value below is TRANSCRIBED from the worked
 *    "Example" block of the instruction's own SPRUFE8B (July 2010) entry,
 *    hex digit for hex digit, with the printed page and the manual's own
 *    register names quoted beside it.  Nothing is taken from running this
 *    emulator and nothing is re-derived where the manual prints a value.
 *  - The two exceptions, both declared at their use site, are (a) the
 *    instruction WORDS, which came from ti-cgt-c6000 8.5.0 "asm6x -mv6740"
 *    and are additionally checked bit-for-bit against each entry's Opcode
 *    figure by ti_encodings() below, and (b) the odd-dst refusal, which is a
 *    fail-closed guard rather than a value the manual prints.
 *  - Latency comes from each entry's Pipeline table and "Delay Slots" line:
 *    E1/0 for ADD4, SUB4, SUBABS4, SADDU4, MAXU4, MINU4, CMPEQ4, CMPGTU4 and
 *    SPACKU4; E2/1 for AVGU4; E4/3 for MPYU4 and MPYSU4.  The manual's own
 *    example headers say "1 cycle after instruction", "2 cycles after
 *    instruction" and "4 cycles after instruction" respectively, and the
 *    assertions below read the destination at exactly those cycles.
 *
 * Two mnemonics of the family are documented pseudo-operations and have no
 * encoding of their own: CMPLTU4 (printed page 213, "The assembler uses the
 * operation CMPGTU4 (.unit) src1, src2, dst to perform this task") and MPYUS4
 * (printed page 363, "The assembler uses the MPYSU4 (.unit)src1, src2, dst
 * instruction to perform this operation").  pseudo_operations() checks that
 * identity against the pseudo-ops' OWN printed examples.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c674x_packed8.h"

static uint32_t memory[64];
static bool read_word(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address < 0x1000 || address >= 0x1100 || (address & 3)) return false;
    *value = memory[(address - 0x1000) / 4];
    return true;
}

/* The field layout every Opcode figure in this family shares: creg 31-29,
 * z 28, dst 27-23, src2 22-18, src1 17-13, x 12, opfield 11-2, s 1, p 0.
 * `op` is the opfield already shifted into bits 11-2. */
static uint32_t encode(uint32_t op, unsigned side, unsigned cross,
                       unsigned dst, unsigned src2, unsigned src1)
{
    return (uint32_t)dst << 23 | (uint32_t)src2 << 18 |
           (uint32_t)src1 << 13 | (uint32_t)cross << 12 | op |
           (uint32_t)side << 1;
}

#define OP_ADD4    0xcb8u  /* printed page 140, bits 11-2 = 1100101110 */
#define OP_SUB4    0xcd8u  /* printed page 551, 1100110110 */
#define OP_SUBABS4 0xb58u  /* printed page 534, 1011010110 */
#define OP_SADDU4  0xcf0u  /* printed page 435, 1100111100 */
#define OP_AVGU4   0x4b0u  /* printed page 149, 0100101100 */
#define OP_MAXU4   0x878u  /* printed page 309, 1000011110 */
#define OP_MINU4   0x918u  /* printed page 314, 1001000110 */
#define OP_CMPEQ4  0x720u  /* printed page 181, 0111001000 */
#define OP_CMPGTU4 0x560u  /* printed page 199, 0101011000 */
#define OP_MPYU4   0x130u  /* printed page 360, 0001001100 */
#define OP_MPYSU4  0x170u  /* printed page 357, 0001011100 */
#define OP_SPACKU4 0xd30u  /* printed page 474, 1101001100 */

/* Words emitted by ti-cgt-c6000 8.5.0 "asm6x -mv6740".  They are evidence
 * that the bit pattern is LEGAL, not that the semantics are right; the
 * semantic assertions are the manual examples further down.  Each word is
 * also reproduced here from encode() plus the opfield read out of the
 * instruction's own Opcode figure, so a transcription slip in either the
 * opfield or the field layout fails this function. */
static void ti_encodings(void)
{
    struct Row { const char *source; uint32_t word, rebuilt; } rows[] = {
        {"ADD4 .L1 A4, A6, A5",    0x02988CB8u,
         0 /* filled below */},
        {"SUB4 .L1 A4, A6, A5",    0x02988CD8u, 0},
        {"SUBABS4 .L1 A4, A6, A5", 0x02988B58u, 0},
        {"SADDU4 .S1 A4, A6, A5",  0x02988CF0u, 0},
        {"AVGU4 .M1 A4, A6, A5",   0x029884B0u, 0},
        {"MAXU4 .L1 A4, A6, A5",   0x02988878u, 0},
        {"MINU4 .L1 A4, A6, A5",   0x02988918u, 0},
        {"CMPEQ4 .S1 A4, A6, A5",  0x02988720u, 0},
        {"CMPGTU4 .S1 A4, A6, A5", 0x02988560u, 0},
        {"SPACKU4 .S1 A4, A6, A5", 0x02988D30u, 0},
        {"MPYU4 .M1 A4, A6, A9:A8",  0x04188130u, 0},
        {"MPYSU4 .M1 A4, A6, A9:A8", 0x04188170u, 0},
    };
    static const uint32_t ops[] = {
        OP_ADD4, OP_SUB4, OP_SUBABS4, OP_SADDU4, OP_AVGU4, OP_MAXU4,
        OP_MINU4, OP_CMPEQ4, OP_CMPGTU4, OP_SPACKU4, OP_MPYU4, OP_MPYSU4,
    };
    unsigned count = sizeof(rows) / sizeof(rows[0]);
    assert(count == sizeof(ops) / sizeof(ops[0]));
    for (unsigned i = 0; i < count; ++i) {
        unsigned dst = i >= 10 ? 8 : 5;      /* A9:A8 for the two .M pairs */
        rows[i].rebuilt = encode(ops[i], 0, 0, dst, 6, 4);
        if (rows[i].rebuilt != rows[i].word) {
            fprintf(stderr, "%s: asm6x %08x, opcode figure %08x\n",
                    rows[i].source, rows[i].word, rows[i].rebuilt);
            assert(false);
        }
        /* The .S2/.L2/.M2 companions asm6x emitted differ only in s. */
        assert((rows[i].word | 2u) == encode(ops[i], 1, 0, dst, 6, 4));
    }
}

/* One single-cycle row, run on both sides and across both cross-path
 * settings.  `src1v`/`src2v`/`expect` are the manual's own hex. */
struct Single { uint32_t op, src1v, src2v, expect; const char *cite; };

static void run_single(const struct Single *tc)
{
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross) {
        CdjC674x c;
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = tc->src1v;
        c.r[side ^ cross][6] = tc->src2v;
        c.r[side][5] = 0xdeadbeefu;
        memory[0] = encode(tc->op, side, cross, 5, 6, 4);
        if (!cdj_c674x_step(&c, read_word, NULL, NULL)) {
            fprintf(stderr, "%s: fault \"%s\"\n", tc->cite,
                    c.fault ? c.fault : "?");
            assert(false);
        }
        if (c.r[side][5] != tc->expect) {
            fprintf(stderr, "%s: got %08x want %08x\n", tc->cite,
                    c.r[side][5], tc->expect);
            assert(false);
        }
        /* "1 cycle after instruction": the write is already architectural
         * and nothing is left in flight. */
        assert(c.cycles == 1 && !c.load_count && !c.store_count);
        /* Sources are read-only, and no other register moved. */
        assert(c.r[side][4] == tc->src1v &&
               c.r[side ^ cross][6] == tc->src2v);
    }
    /* A false predicate writes nothing (every entry's "else nop"). */
    CdjC674x c;
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = tc->src1v; c.r[0][6] = tc->src2v; c.r[0][5] = 0x99u;
    c.r[1][0] = 0;                       /* creg 2 = B0, z = 0: !B0 is false */
    memory[0] = 2u << 29 | encode(tc->op, 0, 0, 5, 6, 4);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][5] == 0x99u && !c.load_count);
}

static void single_cycle_rows(void)
{
    static const struct Single rows[] = {
        /* ADD4, printed page 141, Example 1: A0 FF 68 4E 3Dh, A1 3F F6 F1
         * 05h, "1 cycle after instruction" A2 3E 5E 3F 42h. */
        {OP_ADD4, 0xFF684E3Du, 0x3FF6F105u, 0x3E5E3F42u, "ADD4 p141 ex1"},
        /* ADD4, printed page 141, Example 2: A0 4A E2 D3 1Fh, A1 32 1A C1
         * 28h, A2 7C FC 94 47h. */
        {OP_ADD4, 0x4AE2D31Fu, 0x321AC128u, 0x7CFC9447u, "ADD4 p141 ex2"},
        /* SUB4, printed page 552: A2 37 89 F2 3Ah, A8 04 B8 49 75h,
         * A9 33 D1 A9 C5h. */
        {OP_SUB4, 0x3789F23Au, 0x04B84975u, 0x33D1A9C5u, "SUB4 p552"},
        /* SUBABS4, printed page 535: A2 37 89 F2 3Ah, A8 04 B8 49 75h,
         * A9 33 2F A9 3Bh. */
        {OP_SUBABS4, 0x3789F23Au, 0x04B84975u, 0x332FA93Bu, "SUBABS4 p535"},
        /* SADDU4, printed page 436, Example 1: A2 57 89 F2 3Ah, A8 74 B8 49
         * 75h, A9 CB FF FF AFh - two lanes saturate to FFh. */
        {OP_SADDU4, 0x5789F23Au, 0x74B84975u, 0xCBFFFFAFu, "SADDU4 p436 ex1"},
        /* SADDU4, printed page 436, Example 2: B2 14 7C 01 24h, B8 A0 51 01
         * A6h, B12 B4 CD 02 CA. */
        {OP_SADDU4, 0x147C0124u, 0xA05101A6u, 0xB4CD02CAu, "SADDU4 p436 ex2"},
        /* MAXU4, printed page 310, Example 1: A2 37 89 F2 3Ah, A8 04 B8 49
         * 75h, A9 37 B8 F2 75h. */
        {OP_MAXU4, 0x3789F23Au, 0x04B84975u, 0x37B8F275u, "MAXU4 p310 ex1"},
        /* MAXU4, printed page 310, Example 2 (MAXU4 .L2X A2, B8, B12):
         * A2 01 24 24 B9h, B8 01 A6 A0 51h, B12 01 A6 A0 B9h. */
        {OP_MAXU4, 0x012424B9u, 0x01A6A051u, 0x01A6A0B9u, "MAXU4 p310 ex2"},
        /* MINU4, printed page 315, Example 1: A2 37 89 F2 3Ah, A8 04 B8 49
         * 75h, A9 04 89 49 3Ah. */
        {OP_MINU4, 0x3789F23Au, 0x04B84975u, 0x0489493Au, "MINU4 p315 ex1"},
        /* MINU4, printed page 315, Example 2: B2 01 24 24 B9h, B8 01 A6 A0
         * 51h, B12 01 24 24 51h. */
        {OP_MINU4, 0x012424B9u, 0x01A6A051u, 0x01242451u, "MINU4 p315 ex2"},
        /* CMPEQ4, printed page 182, Example 1: A3 02 3A 4E 1Ch, A4 02 B8 4E
         * 76h, A5 0000 000Ah ("true, false, false, false"). */
        {OP_CMPEQ4, 0x023A4E1Cu, 0x02B84E76u, 0x0000000Au, "CMPEQ4 p182 ex1"},
        /* CMPEQ4, printed page 182, Example 2: B2 F2 3A 37 89h, B8 04 B8 37
         * 89h, B13 0000 0003h. */
        {OP_CMPEQ4, 0xF23A3789u, 0x04B83789u, 0x00000003u, "CMPEQ4 p182 ex2"},
        /* CMPEQ4, printed page 183, Example 3: B2 01 B6 24 51h, B8 05 B6 24
         * 51h, B13 0000 0007h. */
        {OP_CMPEQ4, 0x01B62451u, 0x05B62451u, 0x00000007u, "CMPEQ4 p183 ex3"},
        /* CMPGTU4, printed page 200, Example 1: A3 25 3A 1C E4h, A4 02 B8 4E
         * 76h, A5 0000 0009h. */
        {OP_CMPGTU4, 0x253A1CE4u, 0x02B84E76u, 0x00000009u,
         "CMPGTU4 p200 ex1"},
        /* CMPGTU4, printed page 200, Example 2: B2 89 F2 3A 37h, B8 04 8F 17
         * 89h, B13 0000 000Eh. */
        {OP_CMPGTU4, 0x89F23A37u, 0x048F1789u, 0x0000000Eu,
         "CMPGTU4 p200 ex2"},
        /* CMPGTU4, printed page 201, Example 3: B2 12 33 9D 51h, B8 75 67 24
         * C5h, B13 0000 0002h. */
        {OP_CMPGTU4, 0x12339D51u, 0x756724C5u, 0x00000002u,
         "CMPGTU4 p201 ex3"},
        /* SPACKU4, printed page 475, Example 1: A2 3789 F23Ah, A8 04B8 4975h,
         * A9 FF 00 FF FFh - one lane clamps low, three clamp high. */
        {OP_SPACKU4, 0x3789F23Au, 0x04B84975u, 0xFF00FFFFu,
         "SPACKU4 p475 ex1"},
        /* SPACKU4, printed page 476, Example 2: B2 A124 2451h, B8 01A6 A051h,
         * B12 00 FF FF 00h. */
        {OP_SPACKU4, 0xA1242451u, 0x01A6A051u, 0x00FFFF00u,
         "SPACKU4 p476 ex2"},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        run_single(&rows[i]);
}

/* AVGU4, printed page 150: "Two-cycle", "Delay Slots 1", written in E2.  The
 * example header reads "2 cycles after instruction": A0 1A 2E 5F 4Eh,
 * A1 9E F2 6E 3Fh, A2 5C 90 67 47h. */
static void avgu4_two_cycle(void)
{
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross) {
        CdjC674x c;
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = 0x1A2E5F4Eu;
        c.r[side ^ cross][6] = 0x9EF26E3Fu;
        c.r[side][5] = 0xdeadbeefu;
        memory[0] = encode(OP_AVGU4, side, cross, 5, 6, 4);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        /* One delay slot: dst is NOT yet written one cycle after issue. */
        assert(c.cycles == 1 && c.load_count == 1 && c.loads[0].due == 2 &&
               c.loads[0].bank == side && c.loads[0].dst == 5 &&
               c.r[side][5] == 0xdeadbeefu);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 2 && !c.load_count && c.r[side][5] == 0x5C906747u);
    }
    /* A false predicate queues no delayed result at all. */
    CdjC674x c;
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x1A2E5F4Eu; c.r[0][6] = 0x9EF26E3Fu; c.r[0][5] = 0x99u;
    c.r[1][0] = 0;
    memory[0] = 2u << 29 | encode(OP_AVGU4, 0, 0, 5, 6, 4);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.load_count && c.r[0][5] == 0x99u);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][5] == 0x99u);
}

/* MPYU4 (printed page 361) and MPYSU4 (printed page 358): "Four-cycle",
 * "Delay Slots 3", dst_o:dst_e written in E4, example headers "4 cycles after
 * instruction".  dst_e is the even register of the pair. */
struct Pair { uint32_t op, src1v, src2v, dst_e, dst_o; const char *cite; };

static void four_cycle_pairs(void)
{
    static const struct Pair rows[] = {
        /* MPYU4, printed page 361, Example 1 (MPYU4 .M1 A5,A6,A9:A8):
         * A5 68 32 C1 93h, A6 B1 74 2C ABh, A9:A8 47E8 16A8h 212C 6231h. */
        {OP_MPYU4, 0x6832C193u, 0xB1742CABu, 0x212C6231u, 0x47E816A8u,
         "MPYU4 p361 ex1"},
        /* MPYU4, printed page 361, Example 2 (MPYU4 .M2 B2,B5,B9:B8):
         * B2 3D E6 50 7Fh, B5 C3 56 02 44h, B9:B8 2E77 4D44h 00A0 21BCh. */
        {OP_MPYU4, 0x3DE6507Fu, 0xC3560244u, 0x00A021BCu, 0x2E774D44u,
         "MPYU4 p361 ex2"},
        /* MPYSU4, printed page 358, Example 1 (MPYSU4 .M1 A5,A6,A9:A8):
         * A5 6A 32 11 93h signed, A6 B1 74 6C A4h unsigned,
         * A9:A8 494A 16A8h 072C BA2Ch.  (The manual's decimal annotation for
         * the 072C lane reads "1386"; 072Ch is 1836 and 17 x 108 = 1836, so
         * the hex - which is the register content this test compares - and
         * the Execution line agree and only the annotation is a typo.  The
         * same applies to the FCA4h lane of Example 2, annotated "-680"
         * where FCA4h is -860 = -10 x 86.) */
        {OP_MPYSU4, 0x6A321193u, 0xB1746CA4u, 0x072CBA2Cu, 0x494A16A8u,
         "MPYSU4 p358 ex1"},
        /* MPYSU4, printed page 358, Example 2 (MPYSU4 .M2 B5,B6,B9:B8):
         * B5 3F F6 50 10h signed, B6 C3 56 02 44h unsigned,
         * B9:B8 2FFD FCA4h 00A0 0440h. */
        {OP_MPYSU4, 0x3FF65010u, 0xC3560244u, 0x00A00440u, 0x2FFDFCA4u,
         "MPYSU4 p358 ex2"},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross) {
        const struct Pair *tc = &rows[i];
        CdjC674x c;
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = tc->src1v;
        c.r[side ^ cross][6] = tc->src2v;
        c.r[side][8] = c.r[side][9] = 0xdeadbeefu;
        memory[0] = encode(tc->op, side, cross, 8, 6, 4);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.load_count == 1 && c.loads[0].due == 4 &&
               c.loads[0].size == 16 && c.loads[0].bank == side &&
               c.loads[0].dst == 8);
        /* Three delay slots: neither half is visible before E4. */
        for (unsigned step = 2; step <= 3; ++step) {
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(c.cycles == step && c.load_count == 1 &&
                   c.r[side][8] == 0xdeadbeefu &&
                   c.r[side][9] == 0xdeadbeefu);
        }
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        if (c.r[side][8] != tc->dst_e || c.r[side][9] != tc->dst_o) {
            fprintf(stderr, "%s: got %08x:%08x want %08x:%08x\n", tc->cite,
                    c.r[side][9], c.r[side][8], tc->dst_o, tc->dst_e);
            assert(false);
        }
        assert(c.cycles == 4 && !c.load_count);
    }
    /* A false predicate queues no product. */
    CdjC674x c;
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x6832C193u; c.r[0][6] = 0xB1742CABu;
    c.r[0][8] = c.r[0][9] = 0x99u;
    c.r[1][0] = 0;
    memory[0] = 2u << 29 | encode(OP_MPYU4, 0, 0, 8, 6, 4);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.load_count && c.r[0][8] == 0x99u && c.r[0][9] == 0x99u);
}

/* CMPLTU4 (printed page 213) and MPYUS4 (printed page 363) are documented
 * pseudo-operations; the assembler emits CMPGTU4 / MPYSU4 with the operands
 * exchanged, which asm6x confirms by producing the identical word.  These are
 * the pseudo-ops' OWN printed examples, run through the shared encoding with
 * the operands in the order the pseudo-op's syntax line gives them
 * ("CMPLTU4 (.unit) src2, src1, dst", "MPYUS4 (.unit) src2, src1, dst"). */
static void pseudo_operations(void)
{
    /* Printed page 214, Examples 1-3, each headed "assembler treats as
     * CMPGTU4 <src1>,<src2>,<dst>": A3/A4 -> A5 0000 0009h, B2/B8 -> B13
     * 0000 000Eh and B13 0000 0002h. */
    struct Lt { uint32_t src1v, src2v, expect; } lt[] = {
        {0x253A1CE4u, 0x02B84E76u, 0x00000009u},
        {0x89F23A37u, 0x048F1789u, 0x0000000Eu},
        {0x12339D51u, 0x756724C5u, 0x00000002u},
    };
    for (unsigned i = 0; i < sizeof(lt) / sizeof(lt[0]); ++i) {
        CdjC674x c;
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        /* CMPLTU4 .S1 A4, A3, A5 assembles to CMPGTU4 .S1 A3, A4, A5: the
         * encoding's src1 field holds the manual's src1 either way. */
        c.r[0][4] = lt[i].src1v;
        c.r[0][6] = lt[i].src2v;
        memory[0] = encode(OP_CMPGTU4, 0, 0, 5, 6, 4);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][5] == lt[i].expect);
    }
    /* MPYUS4 has no worked example of its own (printed pages 363-364 carry
     * only the Execution and Pipeline tables), so the identity is checked
     * against MPYSU4's printed page 358 Example 1 with src1 and src2 in the
     * encoding's own fields. */
    CdjC674x c;
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x6A321193u;       /* MPYSU4's src1, signed */
    c.r[0][6] = 0xB1746CA4u;       /* MPYSU4's src2, unsigned */
    memory[0] = encode(OP_MPYSU4, 0, 0, 8, 6, 4);
    for (unsigned step = 0; step < 4; ++step)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][8] == 0x072CBA2Cu && c.r[0][9] == 0x494A16A8u);
    /* The pure functions are asymmetric, which is the whole content of the
     * pseudo-ops: exchanging the operands is not a no-op. */
    assert(cdj_c674x_mpysu4(0xB1746CA4u, 0x6A321193u) !=
           cdj_c674x_mpysu4(0x6A321193u, 0xB1746CA4u));
    assert(cdj_c674x_cmpgtu4(0x02B84E76u, 0x253A1CE4u) !=
           cdj_c674x_cmpgtu4(0x253A1CE4u, 0x02B84E76u));
}

/* MPYU4/MPYSU4 write dst_o:dst_e (printed pages 360, 357), so an odd dst
 * names no architectural pair.  The manual prints no result for that case;
 * this core refuses rather than inventing one, exactly as the 32x32 .M pair
 * forms do.  DECLARED: this expectation is a fail-closed policy, not a
 * transcribed manual value. */
static void odd_destination_refused(void)
{
    static const uint32_t ops[] = {OP_MPYU4, OP_MPYSU4};
    for (unsigned i = 0; i < 2; ++i)
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x c;
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        uint32_t word = encode(ops[i], side, 0, 9, 6, 4);
        memory[0] = word;
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.fault && !strcmp(c.fault,
                                  "invalid multiply result register pair"));
        assert(c.fault_word == word && !c.cycles && !c.load_count);
        /* Rejection does not depend on the predicate being true: the packet
         * is refused even when the instruction would not execute. */
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        c.r[1][0] = 0;
        memory[0] = 2u << 29 | word;
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.fault && !strcmp(c.fault,
                                  "invalid multiply result register pair"));
    }
}

/* The pure semantics layer, called with no CdjC674x anywhere in sight.  Same
 * transcribed values as the execution tests above; this catches a broken lane
 * without the pipeline in the way. */
static void pure_functions(void)
{
    assert(cdj_c674x_add4(0xFF684E3Du, 0x3FF6F105u) == 0x3E5E3F42u);
    assert(cdj_c674x_add4(0x4AE2D31Fu, 0x321AC128u) == 0x7CFC9447u);
    assert(cdj_c674x_sub4(0x3789F23Au, 0x04B84975u) == 0x33D1A9C5u);
    assert(cdj_c674x_subabs4(0x3789F23Au, 0x04B84975u) == 0x332FA93Bu);
    assert(cdj_c674x_saddu4(0x5789F23Au, 0x74B84975u) == 0xCBFFFFAFu);
    assert(cdj_c674x_saddu4(0x147C0124u, 0xA05101A6u) == 0xB4CD02CAu);
    assert(cdj_c674x_avgu4(0x1A2E5F4Eu, 0x9EF26E3Fu) == 0x5C906747u);
    assert(cdj_c674x_maxu4(0x3789F23Au, 0x04B84975u) == 0x37B8F275u);
    assert(cdj_c674x_maxu4(0x012424B9u, 0x01A6A051u) == 0x01A6A0B9u);
    assert(cdj_c674x_minu4(0x3789F23Au, 0x04B84975u) == 0x0489493Au);
    assert(cdj_c674x_minu4(0x012424B9u, 0x01A6A051u) == 0x01242451u);
    assert(cdj_c674x_cmpeq4(0x023A4E1Cu, 0x02B84E76u) == 0x0000000Au);
    assert(cdj_c674x_cmpeq4(0xF23A3789u, 0x04B83789u) == 0x00000003u);
    assert(cdj_c674x_cmpeq4(0x01B62451u, 0x05B62451u) == 0x00000007u);
    assert(cdj_c674x_cmpgtu4(0x253A1CE4u, 0x02B84E76u) == 0x00000009u);
    assert(cdj_c674x_cmpgtu4(0x89F23A37u, 0x048F1789u) == 0x0000000Eu);
    assert(cdj_c674x_cmpgtu4(0x12339D51u, 0x756724C5u) == 0x00000002u);
    assert(cdj_c674x_spacku4(0x3789F23Au, 0x04B84975u) == 0xFF00FFFFu);
    assert(cdj_c674x_spacku4(0xA1242451u, 0x01A6A051u) == 0x00FFFF00u);
    assert(cdj_c674x_mpyu4(0x6832C193u, 0xB1742CABu) ==
           UINT64_C(0x47E816A8212C6231));
    assert(cdj_c674x_mpyu4(0x3DE6507Fu, 0xC3560244u) ==
           UINT64_C(0x2E774D4400A021BC));
    assert(cdj_c674x_mpysu4(0x6A321193u, 0xB1746CA4u) ==
           UINT64_C(0x494A16A8072CBA2C));
    assert(cdj_c674x_mpysu4(0x3FF65010u, 0xC3560244u) ==
           UINT64_C(0x2FFDFCA400A00440));
}

int main(void)
{
    ti_encodings();
    pure_functions();
    single_cycle_rows();
    avgu4_two_cycle();
    four_cycle_pairs();
    pseudo_operations();
    odd_destination_refused();
    printf("c674x packed 8-bit: ok\n");
    return 0;
}
