/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The C64x+ nonconditional encodings (bits 31-28 = 0001).
 *
 * SPRUFE8B Table 3-9 (printed page 77) reserves creg = 000 with z = 1 for the
 * formats that carry creg; TI reuses the same bit pattern as a literal opcode
 * field in Figure C-3 (printed page 724), Figure D-3 (735), Figure E-3 (743),
 * Figure F-14 (749) and Figure H-1 (765).  These assertions fix three things:
 * the 19 documented-but-unimplemented extensions reject by name, the three
 * ADDAB/ADDAH/ADDAW long-immediate forms execute, and everything else in the
 * creg/z hole still rejects as a reserved predicate.
 *
 * Every instruction word below was emitted by ti-cgt-c6000 8.5.0
 * "asm6x -mv6740 -al"; the listing line is quoted beside it.  Every expected
 * ADDA result is either printed in the manual's own worked example or computed
 * by hand from its Execution line.  No expected value comes from this
 * emulator. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

#define BASE 0x1000u
static uint8_t memory[256];
static unsigned committed;

static bool read_word(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    if ((address & 3) || address < BASE || address > BASE + sizeof(memory) - 4)
        return false;
    *value = 0;
    for (unsigned i = 0; i < 4; ++i)
        *value |= (uint32_t)memory[address - BASE + i] << (8 * i);
    return true;
}
static bool write_value(void *opaque, uint32_t address, uint64_t value,
                        unsigned size, bool commit)
{
    (void)opaque;
    if (address < BASE || (uint64_t)address + size > BASE + sizeof(memory))
        return false;
    if (commit) {
        ++committed;
        for (unsigned i = 0; i < size; ++i)
            memory[address - BASE + i] = value >> (8 * i);
    }
    return true;
}
static void reset(CdjC674x *c)
{
    cdj_c674x_reset(c, BASE);
    committed = 0;
    memset(memory, 0, sizeof(memory));
}
static bool execute(CdjC674x *c, uint32_t word)
{
    CdjC674xPacket p = {.count = 1, .next_pc = c->pc + 4,
                       .instructions = {{.word = word, .pc = c->pc}}};
    return cdj_c674x_execute(c, &p, read_word, write_value, NULL);
}
/* Run one word and require the named rejection reason. */
static void rejects(uint32_t word, const char *reason)
{
    CdjC674x c; reset(&c);
    if (execute(&c, word) || strcmp(c.fault, reason)) {
        fprintf(stderr, "%08x: expected \"%s\", got \"%s\"\n", word, reason,
                c.fault ? c.fault : "execution");
        assert(false);
    }
    assert(!committed && c.fault_word == word);
}
/* Run one ADDA long-immediate word and require dst == expected, with no
 * memory traffic: these are address-arithmetic instructions. */
static void adda(uint32_t word, unsigned side, unsigned dst,
                 uint32_t b14, uint32_t b15, uint32_t expected)
{
    CdjC674x c; reset(&c);
    c.r[1][14] = b14; c.r[1][15] = b15;
    /* Nothing in the predicate registers may influence an unconditional
     * instruction (printed page 115: "it cannot be predicated"). */
    c.r[0][0] = c.r[0][1] = c.r[0][2] = 0;
    c.r[1][0] = c.r[1][1] = c.r[1][2] = 0;
    if (!execute(&c, word)) {
        fprintf(stderr, "%08x: unexpected fault \"%s\"\n", word, c.fault);
        assert(false);
    }
    assert(c.r[side][dst] == expected);
    assert(!committed && !c.store_count && !c.load_count);
    /* Single-cycle, zero delay slots: the result is already architectural. */
    assert(c.cycles == 1 && c.packets == 1);
    /* The base registers are read-only here, and no other file changed. */
    assert(c.r[1][14] == b14 && c.r[1][15] == b15);
    assert(c.r[side ^ 1][dst] == 0 || dst == 14 || dst == 15);
}

/* The documented nonconditional instructions this decoder does not implement.
 * They must be reachable and say so, never "reserved predicate".  Opfields read
 * from each instruction's own Opcode figure, words from asm6x.  DPACKX2
 * (op 0x33), DPACK2 (0x34) and SHFL3 (0x36) have left this list: they are
 * implemented and their semantics are covered by tests/cstub/c674x-packbits.c,
 * which also re-checks these three words for reachability.
 *
 * The eight-strong Figure E-3 group - CMPY (0x0a), CMPYR (0x0b), CMPYR1
 * (0x0c), DDOTPL2R (0x14), DDOTPH2R (0x15), DDOTPL2 (0x16), DDOTPH2 (0x17)
 * and DDOTP4 (0x18) - has left it for the same reason: semantics in
 * cdj_c674x_dotp.c, covered by tests/cstub/c674x-dotp.c, which re-checks
 * these words through the whole core.  SMPY32 (0x19), MPY2IR (0x0f) and the four
 * dual ADD/SUB forms (0x0c-0x0f on the .L unit) have since left it the same
 * way.  What remains here is what is still genuinely
 * refused. */
static void unimplemented_extensions(void)
{
    static const struct { uint32_t word; const char *asm_line; } rows[] = {
        /* Figure D-3, .L unit: op = bits 11-5, bits 4-2 = 110. */
        /* Figure E-3, .M unit: bit 11 = 0, op = bits 10-6, bits 5-2 = 1100. */
        {0x118826F0u, "XORMPY   .M1 A1,A2,A3     op 0x1b, printed page 566"},
        {0x118827F0u, "GMPY     .M1 A1,A2,A3     op 0x1f, printed page 270"},
        /* Figure F-14, .S unit: bits 11-10 = 11, op = bits 9-6. */
        {0x11882EF0u, "RPACK2   .S1 A1,A2,A3     op 0xb, printed page 416"},
        /* Figure H-1, no unit: op = bits 16-13, all other bits zero. */
        {0x10000000u, "SWE                       op 0x0, printed page 557"},
        {0x10002000u, "SWENR                     op 0x1, printed page 558"},
    };
    assert(sizeof(rows) / sizeof(rows[0]) == 5);
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        /* p = 1 (parallel) must classify identically to p = 0. */
        rejects(rows[i].word, "instruction not implemented");
        rejects(rows[i].word | 1u, "instruction not implemented");
    }
}

/* SPRUFE8B printed pages 115/120/123, second Opcode figure on each:
 * 31-28 = 0001, dst 27-23, ucst15 22-8, y 7, op 6-4, bits 3-2 = 11, s 1, p 0.
 * ADDAB adds ucst15 unscaled, ADDAH shifts it left 1, ADDAW left 2. */
static void adda_long_immediate(void)
{
    /* Printed pages 115 and 116, Examples 2 and 3, with the manual's own
     * before/after register values:
     *   ADDAB .D1X B14,42h,A4   B14 = 0020 1000h -> A4 = 0020 1042h
     *   ADDAB .D2  B14,7FFFh,B4 B14 = 0010 0000h -> B4 = 0010 7FFFh */
    adda(0x1200423Cu, 0, 4, 0x00201000u, 0, 0x00201042u);
    adda(0x127FFF3Eu, 1, 4, 0x00100000u, 0, 0x00107FFFu);
    /* Printed pages 120 and 121, Examples 2 and 3:
     *   ADDAH .D1X B14,42h,A4   B14 = 0020 1000h -> A4 = 0020 1084h
     *   ADDAH .D2  B14,7FFFh,B4 B14 = 0010 0000h -> B4 = 0010 FFFEh */
    adda(0x1200425Cu, 0, 4, 0x00201000u, 0, 0x00201084u);
    adda(0x127FFF5Eu, 1, 4, 0x00100000u, 0, 0x0010FFFEu);
    /* Printed pages 123 and 124, Examples 2 and 3:
     *   ADDAW .D1X B14,42h,A4   B14 = 0020 1000h -> A4 = 0020 1108h
     *   ADDAW .D2  B14,7FFFh,B4 B14 = 0010 0000h -> B4 = 0011 FFFCh */
    adda(0x1200427Cu, 0, 4, 0x00201000u, 0, 0x00201108u);
    adda(0x127FFF7Eu, 1, 4, 0x00100000u, 0, 0x0011FFFCu);

    /* y = 1 selects B15.  The manual prints no B15 example, so these are
     * hand-computed from the Execution line "B14/B15 + (ucst15 << n) -> dst"
     * with B15 = 0030 0000h and B14 deliberately set to a trap value that
     * would show up immediately if y were decoded the other way round. */
    adda(0x120042BCu, 0, 4, 0xdeadbeefu, 0x00300000u, 0x00300042u);
    adda(0x120042DCu, 0, 4, 0xdeadbeefu, 0x00300000u, 0x00300084u);
    adda(0x120042FCu, 0, 4, 0xdeadbeefu, 0x00300000u, 0x00300108u);
    adda(0x127FFFBEu, 1, 4, 0xdeadbeefu, 0x00300000u, 0x00307FFFu);
    adda(0x127FFFDEu, 1, 4, 0xdeadbeefu, 0x00300000u, 0x0030FFFEu);
    adda(0x127FFFFEu, 1, 4, 0xdeadbeefu, 0x00300000u, 0x0031FFFCu);

    /* The s bit selects both the unit and the destination file (printed page
     * 115).  asm6x: ADDAB .D1 B14,4,A5 = 1280043Ch and ADDAB .D2 B14,4,B5 =
     * 1280043Eh, the same word with s flipped. */
    adda(0x1280043Cu, 0, 5, 0x00201000u, 0, 0x00201004u);
    adda(0x1280043Eu, 1, 5, 0x00201000u, 0, 0x00201004u);

    /* The addition is modular 32-bit address arithmetic: 0xffffffff + (0x7fff
     * << 2) = 0x1_0001fffb truncated to 32 bits. */
    adda(0x127FFF7Eu, 1, 4, 0xffffffffu, 0, 0x0001FFFBu);

    /* Two nonconditional writes to one register in one execute packet are
     * still a parallel write conflict. */
    CdjC674x c; reset(&c);
    CdjC674xPacket p = {.count = 2, .next_pc = c.pc + 8,
                       .instructions = {{.word = 0x1280043Du, .pc = c.pc},
                                        {.word = 0x1280083Cu, .pc = c.pc + 4}}};
    assert(!cdj_c674x_execute(&c, &p, read_word, write_value, NULL));
    assert(!strcmp(c.fault, "parallel register write conflict"));
}

/* The trap the audit named: 0x1280043C has (w & 0x0c) == 12 and op =
 * (w >> 4) & 7 == 3, so without the opcode classification it lands in the
 * 15-bit-offset memory arm and stores a byte.  Figure C-5's op 3/5/7 are
 * STB/STH/STW, which is exactly where Figure C-3 puts ADDAB/ADDAH/ADDAW. */
static void never_executed_as_a_store(void)
{
    /* asm6x: STB .D2T1 A5,*+B14[20h] = 0280203Ch, STH = 0280205Ch,
     * STW = 0280207Ch.  Each word with creg/z replaced by the literal 0001 is
     * the matching ADDAB/ADDAH/ADDAW B14,20h,A5. */
    CdjC674x c; reset(&c);
    c.r[1][14] = BASE; c.r[0][5] = 0x5a;
    assert(execute(&c, 0x0280203Cu | (1u << 29))); /* [B0] STB, B0 = 0 */
    assert(!c.store_count); /* predicate false: no store queued */
    reset(&c);
    c.r[1][14] = BASE; c.r[0][5] = 0x5a; c.r[1][0] = 1;
    assert(execute(&c, 0x0280203Cu | (1u << 29))); /* [B0] STB, B0 = 1 */
    assert(c.store_count == 1); /* the predicable store still works */

    const uint32_t stores[] = {0x0280203Cu, 0x0280205Cu, 0x0280207Cu};
    const uint32_t results[] = {BASE + 0x20u, BASE + 0x40u, BASE + 0x80u};
    for (unsigned i = 0; i < 3; ++i) {
        reset(&c);
        c.r[1][14] = BASE; c.r[0][5] = 0x5a;
        /* A store would leave A5 alone and queue a write to exactly these
         * mapped addresses, so a silent store cannot hide behind a fault. */
        assert(execute(&c, stores[i] | 0x10000000u));
        assert(!c.store_count && !committed);
        assert(c.r[0][5] == results[i]);
        for (unsigned j = 0; j < sizeof(memory); ++j) assert(!memory[j]);
    }
}

/* The reserved-predicate guard must keep every encoding that really does
 * carry creg, including the five 15-bit-offset loads, whose Figure C-5 op
 * values have no Figure C-3 counterpart. */
static void reserved_predicate_still_rejects(void)
{
    static const struct { uint32_t word; const char *asm_line; } rows[] = {
        {0x01882078u, "ADD  .L1   A1,A2,A3"},
        {0x01882C80u, "MPY  .M1   A1,A2,A3"},
        {0x01844840u, "ADD  .D1   A1,A2,A3"},
        {0x000C0362u, "B    .S2   B3"},
        {0x02905D40u, "ADDAW .D1  A4,2,A5 (the predicable ucst5 form)"},
        {0x02908274u, "STW  .D1T1 A5,*+A4[4]"},
        {0x0280206Cu, "LDW  .D2T1 *+B14[20h],A5 (Figure C-5 op 6)"},
        {0x0280202Cu, "LDB  .D2T1 *+B14[20h],A5 (Figure C-5 op 2)"},
        {0x0280200Cu, "LDHU .D2T1 *+B14[20h],A5 (Figure C-5 op 0)"},
        {0x0280204Cu, "LDH  .D2T1 *+B14[20h],A5 (Figure C-5 op 4)"},
        {0x0280201Cu, "LDBU .D2T1 *+B14[20h],A5 (Figure C-5 op 1)"},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        uint32_t bare = rows[i].word & 0x0fffffffu;
        /* creg = 000, z = 1: Reserved, SPRUFE8B Table 3-9, printed page 77. */
        rejects(bare | 0x10000000u, "reserved predicate");
        /* creg = 111: Reserved in the same table. */
        rejects(bare | 0xe0000000u, "reserved predicate");
        rejects(bare | 0xf0000000u, "reserved predicate");
        /* creg = 001 (B0) still predicates normally. */
        CdjC674x c; reset(&c);
        assert(execute(&c, bare | 0x20000000u) ||
               strcmp(c.fault, "reserved predicate"));
    }
    /* Unassigned opfields inside the nonconditional formats stay reserved:
     * .L bits 4-2 = 110 with op 0x00, .M bits 5-2 = 1100 with op 0x00, and
     * the Figure H-1 slot with a nonzero bit outside op. */
    rejects(0x12080018u, "reserved predicate");
    rejects(0x12080030u, "reserved predicate");
    rejects(0x10000004u, "reserved predicate");
}

int main(void)
{
    unimplemented_extensions();
    adda_long_immediate();
    never_executed_as_a_store();
    reserved_predicate_still_rejects();
    puts("C674x nonconditional encoding tests passed");
    return 0;
}
