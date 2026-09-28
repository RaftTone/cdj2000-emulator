/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c674x_control.h"
#include "cdj_c674x_loop.h"
static uint32_t memory[64];
static bool read_word(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address < 0x1000 || address >= 0x1100 || (address & 3)) return false;
    *value = memory[(address - 0x1000) / 4]; return true;
}
static bool write_memory(void *unused, uint32_t address, uint64_t value,
                         unsigned size, bool commit)
{
    (void)unused;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || address < 0x1000 ||
        address > 0x1100 - size) return false;
    if (commit) {
        for (unsigned i = 0; i < size; ++i) {
            unsigned offset = address - 0x1000 + i, shift = (offset & 3) * 8;
            memory[offset / 4] = (memory[offset / 4] & ~(255u << shift)) |
                                 ((uint32_t)((value >> (8 * i)) & 255) << shift);
        }
    }
    return true;
}
static uint32_t mvk(unsigned side, unsigned dst, int value)
{ return dst << 23 | ((uint32_t)value & 0xffff) << 7 | 0x28 | side << 1; }
static void test_cycle_tick(void *opaque)
{
    unsigned *ticks = opaque;
    memory[48] = ++*ticks;
}
static void finish_interrupt_entry(CdjC674x *c)
{
    assert(c->idle_cycles == 9);
    uint32_t pc = c->pc;
    uint64_t cycles = c->cycles;
    for (unsigned i = 0; i < 9; ++i) {
        assert(cdj_c674x_step(c, read_word, write_memory, NULL));
        assert(c->pc == pc && c->cycles == cycles + i + 1);
        assert(c->idle_cycles == 8 - i);
    }
}
static unsigned fetch_reads;
static bool count_fetch_read(void *unused, uint32_t address, uint32_t *value)
{
    ++fetch_reads;
    return read_word(unused, address, value);
}

static void test_fetch_headers(void)
{
    CdjC674x cpu;
    CdjC674xPacket packet;
    memset(memory, 0, sizeof(memory));
    /* Eight parallel full instructions share one header read. */
    for (unsigned i = 0; i < 7; ++i) memory[i] = 1;
    cdj_c674x_reset(&cpu, 0x1000);
    fetch_reads = 0;
    assert(cdj_c674x_fetch(&cpu, count_fetch_read, NULL, &packet));
    assert(packet.count == 8 && packet.next_pc == 0x1020);
    assert(fetch_reads == 9 && cpu.pc == 0x1000 && cpu.cycles == 0);
    /* Crossing a fetch block must inspect the new block's compact header. */
    memory[7] = 1;
    memory[8] = 0x12345678;
    memory[15] = 0xe0200001; /* slot 0 compact, first half parallel */
    cdj_c674x_reset(&cpu, 0x101c);
    fetch_reads = 0;
    assert(cdj_c674x_fetch(&cpu, count_fetch_read, NULL, &packet));
    assert(packet.count == 3 && packet.next_pc == 0x1024 && fetch_reads == 5);
    assert(!packet.instructions[0].compact);
    assert(packet.instructions[1].compact && packet.instructions[1].word == 0x5678);
    assert(packet.instructions[2].compact && packet.instructions[2].word == 0x1234);
    /* No cache survives a fetch: firmware/DMA updates are immediately seen. */
    memory[15] = 0;
    cdj_c674x_reset(&cpu, 0x1020);
    assert(cdj_c674x_fetch(&cpu, count_fetch_read, NULL, &packet));
    assert(packet.count == 1 && !packet.instructions[0].compact);
    assert(packet.instructions[0].word == 0x12345678);
    /* A missing next header fails without publishing a partial packet. */
    memory[63] = 1;
    cdj_c674x_reset(&cpu, 0x10fc);
    memset(&packet, 0, sizeof(packet));
    packet.next_pc = 0xdeadbeef;
    assert(!cdj_c674x_fetch(&cpu, count_fetch_read, NULL, &packet));
    assert(cpu.fault && cpu.fault_pc == 0x1100);
    assert(cpu.pc == 0x10fc && cpu.cycles == 0 && packet.next_pc == 0xdeadbeef);
}

static void test_packet_preserves_loop_storage(void)
{
    CdjC674x c;
    cdj_c674x_reset(&c, 0x1000);
    assert(cdj_c674x_loop_init(&c.loop, 4, 12));
    c.loop.tags[47][7] = 91;
    c.loop_instructions[111] = (CdjC674xInstruction){.pc=0x1234, .word=0x5678};
    c.loop_active = true;
    c.control[26] |= 1u << 14;
    c.branch_due = 1; c.branch_target = 0x2000;
    CdjC674x before = c;
    CdjC674xPacket p = {.count=1, .next_pc=0x1004,
        .instructions={{.pc=0x1000, .word=0x01803da8}}}; /* MVK 123,A3 */
    assert(cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(c.pc == 0x2000 && !c.loop_active && !(c.control[26] & (1u << 14)));
    assert(c.r[0][3] == 123);
    assert(!memcmp(&c.loop, &before.loop, sizeof(c) - offsetof(CdjC674x, loop)));
    /* A conflicting parallel register write must roll back the entire
     * architectural state, including the uncopied retained loop storage. */
    before = c;
    p.count = 2; p.instructions[1] = p.instructions[0];
    assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(c.fault);
    c.fault = before.fault; c.fault_pc = before.fault_pc; c.fault_word = before.fault_word;
    assert(!memcmp(&c, &before, sizeof(c)));
}

/* SPRUFE8B printed page 93, verbatim: "When PROT is 1, four cycles of NOP are
 * added after each LD instruction within the fetch packet whether the LD is in
 * 16-bit compact format or 32-bit format."  Both loads of a parallel pair issue
 * in the execute packet's one cycle, so the four added cycles follow that one
 * cycle once: the packet is 1 + 4 = 5 cycles, the same as a single protected
 * load.  Loads in *different* execute packets of the same fetch packet issue in
 * different cycles and so are expanded separately, once per execute packet.
 *
 * Encodings from asm6x -mv6740 (TMS320C6x Assembler v8.5.0):
 *   LDW .D1 *+A4[0], A10   -> 05100264, with the p bit set 05100265
 *   LDW .D2 *+B4[0], B10   -> 051002E6
 *   STW .D2 B10, *+B4[0]   -> 051002F6
 * The mixed fetch-packet header is built by hand from Figure 3-7 (printed page
 * 93): top nibble 0xE marks the compact header, bit 20 is PROT.  asm6x emits
 * compact headers only under compiler control, so the header word is the one
 * value here not taken from the assembler. */
static void test_protected_fetch_packet_expands_once(void)
{
    CdjC674x c;
    for (unsigned pair = 0; pair < 2; ++pair) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[0] = pair ? 0x05100265u : 0x05100264u;
        if (pair) memory[1] = 0x051002E6u;
        memory[7] = 0xe0000000u | (1u << 20);
        memory[16] = 0x11223344; /* 0x1040 */
        memory[18] = 0x55667788; /* 0x1048 */
        c.r[0][4] = 0x1040; c.r[1][4] = 0x1048;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        /* Hand-derived from printed page 93: one issue cycle plus four. */
        assert(c.cycles == 5 && c.pc == (pair ? 0x1008u : 0x1004u));
        /* Table 4-10, printed page 593: a load writes dst in E5, four delay
         * slots after E1, so both results are architectural by cycle 5. */
        assert(c.r[0][10] == 0x11223344 && !c.load_count);
        assert(c.r[1][10] == (pair ? 0x55667788u : 0u));
    }
    /* The narrowed guard still rejects a PROT packet that also holds a
     * genuinely conflicting multicycle instruction: printed page 82 forbids two
     * multicycle-NOP generators in one execute packet, and printed page 481
     * counts protected loads among the instructions that initiate them. */
    const uint32_t conflicts[] = {
        0x00002000u,        /* NOP 2, asm6x */
        0x00006000u,        /* NOP 4, asm6x */
        0x0001E000u,        /* IDLE, asm6x */
        (3u << 13) | 0x120u /* BNOP label, 3 */
    };
    for (unsigned i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); ++i)
    for (unsigned order = 0; order < 2; ++order) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[order] = 0x05100264u | (order ? 0u : 1u);
        memory[order ^ 1] = conflicts[i] | (order ? 1u : 0u);
        memory[7] = 0xe0000000u | (1u << 20);
        c.r[0][4] = 0x1040;
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0 && !c.load_count);
    }
}

/* METAMORPHIC TEST, not an absolute reference-backed one.  What comes from
     * SPRUFE8B printed page 93 is the EQUIVALENCE: a protected LD expands to the
     * same thing as that LD followed by an explicit NOP 4.  The absolute values
     * compared below (register files, loop.cycle/length/sealed, loop_tags,
     * load_count) are this emulator's own trace of the unprotected-plus-NOP-4
     * program, captured into reference[] in this same test.  So if our handling
     * of NOP 4 inside an SPLOOP body were itself wrong, both sides would be
     * wrong together and this test would still pass.  It is load-bearing -
     * reverting the per-packet protected-load fix fails this test and only this
     * test - but it pins a relation to the manual, not a number.  The coverage
     * inventory records its expected values as "mixed" for that reason.
     */
    /* The same rule inside a software-pipelined loop body, which is the case
 * BUILD.md:996 records the NXS firmware hitting at 0x11802ea8.  SPRUFE8B
 * printed page 93 again: the four cycles follow the execute packet's one issue
 * cycle, so a protected body packet holding two parallel loads is cycle-for-
 * cycle the same program as the unprotected pair followed by an explicit NOP 4.
 *
 * Encodings from asm6x -mv6740:
 *   SPLOOP 5             -> 02038000
 *   LDW .D1 *A4, A5      -> 02900264, p bit set 02900265
 *   LDW .D2 *B4, B5      -> 029002E6
 *   NOP 4                -> 00006000
 *   SPKERNEL 0, 0        -> 00034001
 *   ADD .L1 A5, A5, A6   -> 0314A078
 * asm6x accepts that whole body, two parallel loads included. */
static void test_protected_loop_body_expands_once(void)
{
    CdjC674x c, reference[20];
    for (unsigned prot = 0; prot < 2; ++prot) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[0] = 0x02038000u;
        memory[1] = 0x02900265u;
        memory[2] = 0x029002E6u;
        unsigned slot = 3;
        if (!prot) memory[slot++] = 0x00006000u; /* explicit NOP 4 */
        memory[slot++] = 0;                      /* NOP */
        memory[slot++] = 0x00034001u;            /* SPKERNEL 0,0 */
        memory[slot] = 0x0314A078u;              /* || ADD .L1 A5,A5,A6 */
        memory[7] = 0xe0000000u | (prot ? (1u << 20) : 0u);
        memory[16] = 0x00000101; /* 0x1040 */
        memory[18] = 0x00000202; /* 0x1048 */
        c.r[0][4] = 0x1040; c.r[1][4] = 0x1048;
        c.control[13] = 3; /* ILC */
        for (unsigned cycle = 0; cycle < 20; ++cycle) {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.cycles == cycle + 1);
            if (!prot) reference[cycle] = c;
            else {
                const CdjC674x *r = &reference[cycle];
                assert(!memcmp(c.r, r->r, sizeof(c.r)));
                assert(c.loop.cycle == r->loop.cycle &&
                       c.loop.length == r->loop.length);
                assert(c.loop.sealed == r->loop.sealed &&
                       c.loop_tags == r->loop_tags);
                assert(c.load_count == r->load_count);
                /* loop_wait itself is deliberately not compared: the protected
                 * form consumes the load's own issue cycle as the first of the
                 * five, so its countdown leads the explicit NOP 4 by one
                 * cycle while the program state matches. */
            }
        }
        /* The expansion is four cycles, counted once for the pair. */
        if (prot) assert(c.r[0][5] == 0x101 && c.r[1][5] == 0x202);
    }
    /* The narrowed loop-path guard still rejects the cases printed page 481
     * forbids: SPKERNEL in the protected packet, and a second multicycle-NOP
     * generator in it. */
    const uint32_t conflicts[] = {0x00034001u /* SPKERNEL */, 0x00002000u /* NOP 2 */};
    for (unsigned i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); ++i) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[0] = 0x02038000u;
        memory[1] = 0x02900265u;
        memory[2] = 0x029002E7u; /* second protected load, p bit set */
        memory[3] = conflicts[i];
        memory[7] = 0xe0000000u | (1u << 20);
        c.r[0][4] = 0x1040; c.r[1][4] = 0x1048;
        c.control[13] = 3;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL)); /* setup */
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.loop_tags == 0 && c.loop.length == 0);
    }
}

/* SPRUFE8B 3.8.11.5, printed page 83, verbatim: "A NOP n (with n > 1)
 * instruction cannot be placed in parallel with other multicycle NOP counts
 * (ADDKPC, BNOP, CALLP) with the exception of another NOP n where the NOP count
 * is the same."  asm6x agrees: NOP 2 || NOP 2 assembles, while NOP 4 || NOP 2
 * is rejected with "[E0801] Multiple multi-cycle NOP ... instructions not
 * allowed in the same execute packet".
 *
 * NOP encodings are from printed page 388 (src = count - 1 at bits 16-13) and
 * confirmed by asm6x: NOP 2 -> 00002000, NOP 4 -> 00006000, NOP 9 -> 00010000. */
static void test_equal_count_parallel_nops(void)
{
    CdjC674x c;
    for (unsigned count = 2; count <= 9; ++count) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[0] = ((count - 1) << 13) | 1u;
        memory[1] = (count - 1) << 13;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        /* Printed page 388: "For src + 1 cycles, no operation is performed." */
        assert(c.cycles == count && c.pc == 0x1008);
    }
    /* Unequal counts remain a fault in both orders. */
    for (unsigned order = 0; order < 2; ++order) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[order] = 0x00006000u | (order ? 0u : 1u);     /* NOP 4 */
        memory[order ^ 1] = 0x00002000u | (order ? 1u : 0u); /* NOP 2 */
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0);
    }
    /* A BNOP whose count happens to match is still forbidden: printed page 83
     * grants the exception to another NOP n only. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0x00002001u;          /* NOP 2 */
    memory[1] = (1u << 13) | 0x120u;  /* BNOP label, 1 -> two cycles */
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 0);
}

/* SPRUFE8B 3.8.11.5/9 prohibit NOP n (n > 1) alongside SPMASK.
 * Both orders must fail before an earlier packet member changes the CPU. */
static void test_multicycle_nop_spmask_conflict(void)
{
    CdjC674x c;
    for (unsigned order = 0; order < 2; ++order) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[order] = 0x00030000u | (order ? 0u : 1u); /* SPMASK 0 */
        memory[order ^ 1] = 0x00006000u | (order ? 1u : 0u); /* NOP 4 */
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.fault && !strcmp(c.fault,
                                  "NOP n cannot share SPMASK(R) packet"));
        assert(c.cycles == 0 && c.pc == 0x1000);
    }
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0x00030001u; /* SPMASK 0 || */
    memory[1] = 0;           /* NOP 1 */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 1 && c.pc == 0x1008);
}

/* SPRUFE8B SPMASKR, printed page 489: an SPMASKR outside the software-loop
 * mechanism is a NOP; an active-loop reload must not be silently discarded. */
static void test_spmaskr_outside_loop(void)
{
    CdjC674x c;
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0x00032000u; /* SPMASKR 0 */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 1 && c.pc == 0x1004 && !c.loop_active);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0x00032001u; /* SPMASKR 0 || NOP 1 */
    memory[1] = 0;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 1 && c.pc == 0x1008);

    for (unsigned order = 0; order < 2; ++order) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[order] = 0x00032000u | (order ? 0u : 1u);
        memory[order ^ 1] = 0x00006000u | (order ? 1u : 0u); /* NOP 4 */
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.fault && !strcmp(c.fault,
                                  "NOP n cannot share SPMASK(R) packet"));
        assert(c.cycles == 0 && c.pc == 0x1000);
    }

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 3;
    memory[0] = 0x00038000u; /* SPLOOP 1 */
    memory[1] = 0x00032000u; /* active-loop SPMASKR reload */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.fault && !strcmp(c.fault,
                              "SPMASKR reload not implemented"));
    assert(c.cycles == 1);
}

/* SPRUFE8B IDLE, printed page 274: opcode bits 16-13 = 1111 with every other
 * bit zero except p, i.e. 0001E000 - confirmed by asm6x, which assembles IDLE
 * to 0001E000 and IDLE || NOP to 0001E001 / 00000000.  Description, verbatim:
 * "Performs an infinite multicycle NOP that terminates upon servicing an
 * interrupt, or a branch occurs due to an IDLE instruction being in the delay
 * slots of a branch."  Delay Slots: 0.
 *
 * NOP's own entry, printed page 388: "The maximum value for count is 9", so
 * src 9..14 stay reserved and must still be rejected. */
static void test_idle_waits_for_an_interrupt_or_a_branch(void)
{
    CdjC674x c;
    /* src 9..14 -> count 10..15: still reserved, still rejected. */
    for (unsigned src = 9; src <= 14; ++src) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[0] = src << 13;
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0);
    }
    /* Zero delay slots: the packet issues one cycle, then waits. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0x0001E000u;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 1 && c.pc == 0x1004);
    assert(c.idle_cycles == CDJ_C674X_IDLE_FOREVER);
    /* The wait does not count down, however long the core is stepped. */
    for (unsigned i = 0; i < 64; ++i) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.pc == 0x1004 && c.cycles == 2 + i);
        assert(c.idle_cycles == CDJ_C674X_IDLE_FOREVER);
    }
    /* Servicing an interrupt terminates it.  Table 5-3 / section 5.4.4: IRP is
     * the first annulled execute packet, which is the one after the IDLE. */
    c.control[1] |= 1u;                /* CSR.GIE */
    c.control[4] |= (1u << 4) | 2u;    /* IER.IE4, NMIE */
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.control[6] == 0x1004 && c.pc == 0x00700080);
    finish_interrupt_entry(&c);
    /* The entry interval is the documented nine cycles, not the sentinel, so
     * the core resumes at the vector instead of idling forever. */
    assert(!c.idle_cycles && c.pc == 0x00700080);

    /* An IDLE in the delay slots of a branch ends when the branch completes
     * (printed page 274).  B .S2 B3 is 0x000C0362 by the branch-register
     * encoding; five delay slots put the target in E1 on cycle 6 (Table 4-11,
     * printed page 594). */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0x00000362u | (3u << 18);
    memory[1] = 0x0001E000u;
    c.r[1][3] = 0x1080;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.branch_due == 6);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 2 && c.idle_cycles == CDJ_C674X_IDLE_FOREVER);
    while (c.cycles < 6) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.pc == 0x1080 && !c.idle_cycles && !c.branch_due);

    /* Printed page 83, 3.8.11.4: IDLE "can be placed in parallel with the NOP
     * instruction" and with ordinary single-cycle work, but with no other
     * multicycle-NOP generator. */
    const uint32_t legal[] = {0x00000000u, 0x008000A8u /* MVK .S1 1,A1 */};
    for (unsigned i = 0; i < sizeof(legal) / sizeof(legal[0]); ++i) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[0] = 0x0001E001u; memory[1] = legal[i];
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 1 && c.idle_cycles == CDJ_C674X_IDLE_FOREVER);
        assert(c.r[0][1] == (i ? 1u : 0u));
    }
    const uint32_t illegal[] = {
        0x00002000u,          /* NOP 2 */
        0x0001E000u,          /* IDLE */
        (3u << 13) | 0x120u,  /* BNOP label, 3 */
        (1u << 13) | 0x162u,  /* ADDKPC label, B3, 1 */
        0x10004000u,          /* DINT */
        0x10006000u,          /* RINT */
    };
    for (unsigned i = 0; i < sizeof(illegal) / sizeof(illegal[0]); ++i)
    for (unsigned order = 0; order < 2; ++order) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        memory[order] = 0x0001E000u | (order ? 0u : 1u);
        memory[order ^ 1] = illegal[i] | (order ? 1u : 0u);
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0 && !c.idle_cycles);
    }
}

static void test_immediate_spkernelr_reload(void)
{
    /* SPRUFE8B 7.9.6 and Ghidra's assembled Example 7-15 fixture: an
     * invocation starts the cycle after the last kernel boundary while the
     * preceding epilog continues to issue. Use independent .L1/.L2 adds so
     * the overlap is observable without relying on delayed load effects. */
    CdjC674x c;
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 5;
    c.r[0][1] = 1;
    memory[0] = (4u << 29) | 0x38000u; /* [A1] SPLOOP 1 */
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[2] = 0;                    /* one idle source cycle */
    memory[3] = 0x36001u;             /* SPKERNELR || */
    memory[4] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_active);
    for (unsigned t = 0; t < 5; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 4) {
            assert(c.control[13] == 4 && c.loop.iterations == 5);
            c.r[0][1] = 0; /* sample false four cycles before second end */
        }
    }
    assert(c.loop_active && c.control_ready[30] == 5);
    for (unsigned t = 5; t < 12; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 5) assert(c.r[0][4] == 6 && c.r[1][4] == 4);
    }
    assert(c.r[0][4] == 10 && c.r[1][4] == 10);
    assert(c.control[13] == 0);
    assert(!c.loop_active);

    /* A post-body branch holds its target PC when its delay slots expire,
     * while the second invocation keeps running from the loop buffer. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 5; c.control[14] = 10; c.r[0][1] = 1;
    memory[0] = (4u << 29) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[3] = 0x36001u;
    memory[4] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    memory[5] = (6u << 7) | 0x10u; /* B 0x1018, five delay slots */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t <= 11; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 4) c.r[0][1] = 0;
    }
    assert(c.loop_active && (c.control[26] & (1u << 14)));
    assert(c.pc == 0x1018 && c.control_ready[27] == 8);
    for (unsigned t = 12; t <= 14; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.pc == 0x1018);
    }
    assert(c.loop_active);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.pc == 0x101c);

    /* Two successive reloads retain exactly one predecessor epilog each.
     * The outer predicate falls before the third termination boundary. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 5; c.r[0][1] = 1;
    memory[0] = (4u << 29) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[3] = 0x36001u;
    memory[4] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 17; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 9) c.r[0][1] = 0;
    }
    assert(c.control_ready[30] == 10 && !c.loop_active);
    assert(c.r[0][4] == 15 && c.r[1][4] == 15 && c.control[13] == 0);

    /* A post-body MVC can replace RILC for the next reload after its four
     * cycle availability latency. The second reload uses three iterations. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 5;
    c.r[0][1] = 1; c.r[1][2] = 3;
    memory[0] = (4u << 29) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[3] = 0x36001u;
    memory[4] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    memory[5] = (14u << 23) | (2u << 18) | 0x3a2u;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 15; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 9) {
            assert(c.loop.iterations == 3 && c.control[13] == 2 &&
                   c.control[14] == 3);
        }
        if (t == 7) c.r[0][1] = 0;
    }
    assert(!c.loop_active && c.r[0][4] == 13 && c.r[1][4] == 13);

    /* At a four-cycle first boundary, the sampled predicate belongs to the
     * SPLOOP packet itself. A same-boundary MVC updates RILC for later use;
     * the current reload takes its pre-packet visible value. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 4;
    c.r[0][1] = 1; c.r[1][2] = 6;
    memory[0] = (4u << 29) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[4] = 0x36001u;
    memory[5] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    memory[9] = (14u << 23) | (2u << 18) | 0x3a2u;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 8; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 4) c.r[0][1] = 0;
        if (t == 7) {
            assert(c.loop.iterations == 4 && c.control[13] == 3 &&
                   c.control[14] == 6);
        }
    }

    /* A false outer condition executes one invocation and falls through. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 5;
    memory[0] = (4u << 29) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[3] = 0x36001u;
    memory[4] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 7; ++t)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.loop_active && c.r[0][4] == 5 && c.r[1][4] == 5);

    /* II=2, five source cycles: the old epilog and new prolog index the
     * buffer from different LBC origins during the same global cycle. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 3; c.r[0][1] = 1;
    memory[0] = (4u << 29) | (1u << 23) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[5] = 0x36001u;
    memory[6] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 15; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 5) c.r[0][1] = 0;
        if (t == 6) assert(c.r[0][4] == 4 && c.r[1][4] == 2);
    }
    assert(!c.loop_active && c.r[0][4] == 6 && c.r[1][4] == 6);

    /* A source shorter than II still holds SPLX until its last stage
     * boundary. Reload begins there, even though its lone operation issued
     * four cycles earlier. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 1; c.r[0][1] = 1;
    memory[0] = (4u << 29) | (4u << 23) | 0x38000u; /* [A1] SPLOOP 5 */
    memory[1] = 0x36001u;
    memory[2] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 10; ++t) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (t == 4) c.r[0][1] = 0;
        if (t < 9) assert(c.loop_active);
    }
    assert(!c.loop_active && c.control_ready[30] == 5 && c.r[1][4] == 2);

    /* A zero RILC would skip the next invocation, which this subset does
     * not model. The final-boundary packet must fail before it commits. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 5; c.r[0][1] = 1;
    memory[0] = (4u << 29) | 0x38000u;
    memory[1] = (4u << 23) | (4u << 18) | (1u << 13) | 0x58u;
    memory[3] = 0x36001u;
    memory[4] = (4u << 23) | (4u << 18) | (1u << 13) | 0x5au;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned t = 0; t < 4; ++t)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    uint64_t before = c.cycles;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.fault && !strcmp(c.fault,
        "SPKERNELR reload count or overlap unsupported"));
    assert(c.cycles == before && c.loop.cycle == 4 && c.r[0][4] == 4);

    /* Eligible interrupts during reload require a separate restart model. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = c.control[14] = 5; c.r[0][1] = 1;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    memory[0] = (4u << 29) | 0x38000u;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.fault && !strcmp(c.fault,
        "SPKERNELR interrupt restart not implemented"));

    /* SPKERNELR has meaning only after a predicated SPLOOP setup. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 5;
    memory[0] = 0x38000u;
    memory[1] = 0x36000u;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    before = c.cycles;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.fault && !strcmp(c.fault, "unsupported SPLOOP reload boundary"));
    assert(c.cycles == before && !c.loop.sealed);
}

int main(void)
{
    test_immediate_spkernelr_reload();
    test_fetch_headers();
    test_packet_preserves_loop_storage();
    test_protected_fetch_packet_expands_once();
    test_protected_loop_body_expands_once();
    test_equal_count_parallel_nops();
    test_multicycle_nop_spmask_conflict();
    test_spmaskr_outside_loop();
    test_idle_waits_for_an_interrupt_or_a_branch();
    CdjC674x c;
    /* Board clocks advance on every cycle, including PROT/NOP delays;
     * E3 captures the value on that edge, not the step's final value. */
    unsigned ticks = 0;
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
    c.r[0][5] = 0x10c0;
    memory[0] = 0x01940264; memory[7] = 0xe0100000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 5 && c.cycles == 5 && c.r[0][3] == 3);
    memory[1] = 7u << 13; /* NOP 8 */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 13 && c.cycles == 13);
    memory[2] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 13 && c.cycles == 13);
    cdj_c674x_reset(&c, 0x1000);
    assert(!c.cycle_tick && !c.cycle_opaque);
    /* A taken BNOP truncates both inserted NOPs and board ticks. */
    memset(memory, 0, sizeof(memory)); ticks = 0;
    c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
    memory[0] = (7u << 13) | 0x120;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 6 && c.cycles == 6);
    /* PROT in a software loop is equivalent cycle by cycle to LD; NOP 4.
     * Cover overlapping replay (II < 5), masked one-shot loads, and false
     * predicates with invalid addresses. Mutate RAM between E1/E3/E5 to
     * check actual sampling, not merely the final instruction count. */
    for (unsigned ii = 1; ii <= 7; ++ii)
    for (unsigned masked = 0; masked < 2; ++masked)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned compact = 0; compact < 2; ++compact)
    for (unsigned rs = 0; rs <= compact; ++rs) {
        if (compact && !enabled) continue; /* this compact form is unconditional */
        CdjC674x reference[24];
        for (unsigned prot = 0; prot < 2; ++prot) {
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            ticks = 0; c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
            c.r[1][1] = 1; c.r[1][0] = enabled;
            c.r[0][5] = enabled ? 0x10c0 : 0xffffffff;
            memory[0] = 0x4003e000 | (ii - 1) << 23; /* [B1] SPLOOPW */
            unsigned slot = 1;
            if (masked) memory[slot++] = 0x430001; /* SPMASK D1 || */
            unsigned load_slot = slot;
            /* [B0] LDW *A5,A3, or compact LDW *A5,A3/A19; NOP 1 */
            memory[slot++] = compact ? 0x0c6e00bc : 0x21940264;
            if (!prot) memory[slot++] = 3u << 13; /* NOP 4 */
            memory[slot++] = 0; /* separate SPKERNEL from multicycle op */
            memory[slot] = 0x34000;
            memory[7] = 0xe0000000 | prot << 20 | rs << 19 |
                        (compact ? 1u << (21 + load_slot) : 0);
            for (unsigned cycle = 0; cycle < 24; ++cycle) {
                assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
                assert(c.cycles == cycle + 1 && !c.idle_cycles);
                assert(ticks == c.cycles);
                if (!prot) reference[cycle] = c;
                else {
                    const CdjC674x *r = &reference[cycle];
                    assert(!memcmp(c.r, r->r, sizeof(c.r)));
                    assert(c.load_count == r->load_count);
                    assert(!memcmp(c.loads, r->loads, c.load_count * sizeof(c.loads[0])));
                    assert(c.loop.cycle == r->loop.cycle && c.loop.length == r->loop.length);
                    assert(c.loop.sealed == r->loop.sealed && c.loop_tags == r->loop_tags);
                    assert(c.loop_pred_history == r->loop_pred_history);
                }
            }
            assert(c.loop_tags == (masked ? 0u : 1u));
            if (!enabled) assert(c.r[0][3 + rs * 16] == 0);
            else if (masked) assert(c.r[0][3 + rs * 16] == 4);
            else assert(c.r[0][3 + rs * 16] > 4);
        }
    }
    /* SPKERNEL cannot share a protected-load packet (SPRUFE8B p481).
     * Nor may this implementation silently combine two multicycle ops. */
    for (unsigned kernel = 0; kernel < 2; ++kernel) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        assert(cdj_c674x_loop_init(&c.loop, 1, 2)); c.loop_active = true;
        c.r[0][5] = 0x10c0;
        memory[0] = kernel ? 0x34001 : 0x01940265;
        memory[1] = kernel ? 0x01940264 : 1u << 13;
        memory[7] = 0xe0100000;
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0 && c.loop_tags == 0 && c.loop.length == 0 && !c.load_count);
    }
    /* Full-width immediate BNOP: signed displacement bounds, both units,
     * both fetch layouts and all N counts, with true/false predicates. */
    const int displacements[] = {-2048,-1,0,1,2047};
    for (unsigned layout = 0; layout < 2; ++layout)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned taken = 0; taken < 2; ++taken)
    for (unsigned n = 0; n < 8; ++n)
    for (unsigned d = 0; d < 5; ++d) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1004);
        c.r[1][0] = taken;
        memory[1] = (1u << 29) | (((uint32_t)displacements[d] & 4095) << 16) |
                    (n << 13) | 0x120 | (side << 1);
        if (layout) memory[7] = 0xe0000000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        unsigned elapsed = taken && n > 5 ? 6 : n + 1;
        assert(c.cycles == elapsed);
        if (taken) {
            while (c.cycles < 6) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(c.pc == 0x1000u + (uint32_t)(displacements[d] * (layout ? 2 : 4)));
        } else assert(c.pc == 0x1008 && !c.branch_due);
    }
    /* Long-offset scalar loads/stores: all eight opcodes, both data banks,
     * both fixed B bases and displacement boundaries. Reuse real E3/E5 bus. */
    const unsigned long_offsets[] = {0, 1, 31, 256, 32767};
    for (unsigned op = 0; op < 8; ++op)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned y = 0; y < 2; ++y)
    for (unsigned n = 0; n < 5; ++n) {
        unsigned size = op >= 6 ? 4 : (op == 0 || op == 4 || op == 5) ? 2 : 1;
        bool store = op == 3 || op == 5 || op == 7;
        uint32_t base = 0x1080u - long_offsets[n] * size;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][14+y] = base; c.r[side][3] = 0xaabbccdd;
        memory[32] = 0x92348081;
        memory[0] = (3u << 23) | (long_offsets[n] << 8) |
                    (y << 7) | (op << 4) | 12 | (side << 1);
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(memory[32] == 0x92348081 && c.r[side][3] == 0xaabbccdd);
        for (unsigned step = 0; step < 4; ++step)
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (store) {
            uint32_t expected = size == 1 ? 0x923480dd : size == 2 ? 0x9234ccdd : 0xaabbccdd;
            assert(memory[32] == expected);
        } else {
            const uint32_t results[] = {0x8081,0x81,0xffffff81,0,0xffff8081,0,0x92348081,0};
            assert(c.r[side][3] == results[op]);
        }
        assert(c.r[1][14+y] == base);
    }
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 3; ++op)
    for (unsigned n = 0; n < 32; ++n) {
        unsigned reg = 4 + subset * 16;
        uint32_t source = 0x96a55aa5u;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][reg] = source;
        memory[0] = ((n & 7) << 13) | ((n >> 3) << 11) |
                    (4u << 7) | (op << 5) | 2 | side;
        memory[7] = 0xe0200000 | (subset << 19);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        uint32_t expected = op == 0 ? (source >> (31-n)) & 1 :
                            op == 1 ? source | (1u << n) : source & ~(1u << n);
        assert(c.r[side][op ? reg : 0] == expected);
        if (!op) assert(c.r[side][reg] == source);
    }
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 4; ++op) {
        const uint32_t expected[] = {0xffff80a5, 0xffffffa5, 0x80a5, 0xa5};
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][4 + subset*16] = 0x123480a5;
        memory[0] = (5u << 13) | (op << 11) | (4u << 7) | 0x62 | side;
        memory[7] = 0xe0200000 | (subset << 19);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][5 + subset*16] == expected[op]);
    }
    /* Figure D-10 compact signed/unsigned comparisons use constants 0/1,
     * honor RS for source and destination, and cover all four relations. */
    const uint32_t compact_compare_sources[] = {0, 1, 2, 0xffffffffu, 0x80000000u};
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 4; ++op)
    for (unsigned constant = 0; constant < 2; ++constant)
    for (unsigned j = 0; j < sizeof(compact_compare_sources) /
                              sizeof(compact_compare_sources[0]); ++j) {
        unsigned src = 5 + subset * 16, dst = (j & 1) + subset * 16;
        uint32_t source = compact_compare_sources[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][src] = source;
        memory[0] = op << 14 | constant << 13 | (dst & 1) << 11 |
                    5u << 7 | 0x1026 | side;
        memory[7] = 0xe0200000 | subset << 19;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        uint32_t expected = op == 0 ? (int32_t)constant < (int32_t)source :
                            op == 1 ? (int32_t)constant > (int32_t)source :
                            op == 2 ? constant < source : constant > source;
        assert(c.r[side][dst] == expected);
    }
    /* Full .S bit-field family: all 1024 parameter pairs, both banks,
     * immediate/register operands and both register cross paths. Expected
     * results use a bit-by-bit oracle rather than the implementation masks. */
    const uint32_t field_ops[] = {0xae0, 0xbe0, 0xee0, 0xfe0};
    for (unsigned op = 0; op < 4; ++op)
    for (unsigned mode = 0; mode < 3; ++mode)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned left = 0; left < 32; ++left)
    for (unsigned right = 0; right < 32; ++right) {
        uint32_t source = 0xa5367e91u, expected = 0;
        for (unsigned bit = 0; bit < 32; ++bit) {
            unsigned set;
            if (op < 2) {
                unsigned from = bit + right;
                if (from >= 32) set = op == 1 ? (source >> (31-left)) & 1 : 0;
                else set = from >= left ? (source >> (from-left)) & 1 : 0;
            } else {
                set = (source >> bit) & 1;
                if (bit >= left && bit <= right) set = op == 2;
            }
            expected |= (uint32_t)set << bit;
        }
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side ^ (mode == 2)][2] = source;
        c.r[side][3] = left * 32 + right;
        memory[0] = (4u << 23) | (2u << 18) | (side << 1) |
            (mode == 0 ? (left << 13) | (right << 8) | (op << 6) | 8 :
             (3u << 13) | ((mode == 2) << 12) | field_ops[op]);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][4] == expected && c.cycles == 1);
    }
    for (unsigned op = 0; op < 4; ++op) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][3] = 1024;
        memory[0] = (4u << 23) | (2u << 18) | (3u << 13) | field_ops[op];
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 0 && c.r[0][4] == 0);
        cdj_c674x_reset(&c, 0x1000); c.r[0][3] = 1024;
        memory[0] |= 2u << 29; /* false [B1] does not fault on unused count */
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][4] == 0);
    }
    /* OR/XOR on all three units, immediates/registers, banks and cross paths. */
    const unsigned logic_ops[] = {0xfd8,0xff8,0x6a0,0x6e0,0x8f0,0x8b0,
                                  0xdd8,0xdf8,0x2a0,0x2e0,0xbf0,0xbb0};
    for (unsigned op = 0; op < 12; ++op)
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned cross = 0; cross < 2; ++cross) {
                cdj_c674x_reset(&c, 0x1000);
                c.r[side][31] = 0x12345678; c.r[side ^ cross][5] = 0x87654321;
                CdjC674xPacket p = {.count=1, .next_pc=0x1004,
                    .instructions={{.pc=0x1000, .word=6u<<23 | 5u<<18 | 31u<<13 |
                        cross<<12 | logic_ops[op] | side<<1}}};
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                uint32_t a = op & 1 ? 0x12345678 : UINT32_MAX;
                assert(c.r[side][6] == (op < 6 ? (a | 0x87654321) : (a ^ 0x87654321)));
            }
    /* PROT/BR are fetch-header selectors, not attributes of every opcode. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 1;
    memory[0] = 0x38000; memory[1] = mvk(0, 4, 99);
    memory[2] = 0x0c6e0012; /* compact MVK 0,A0; NOP */
    memory[3] = 0x34000; memory[7] = 0xe0908000; /* PROT, BR, compact slot 2 */
    unsigned header_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++header_steps < 12);
    } while (c.loop_active);
    assert(c.r[0][4] == 99);
    /* Address arithmetic families: unsigned constants, signed-register
     * bit patterns, scaling and modular wrap, on both banks. */
    for (unsigned op = 0x30; op <= 0x3d; ++op)
        for (unsigned side = 0; side < 2; ++side) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[side][5] = 2; c.r[side][31] = 0xffffffff;
            memory[0] = 6u << 23 | 5u << 18 | 31u << 13 | op << 7 | 0x40 | side << 1;
            uint32_t offset = ((op >= 0x3c ? op & 1 : op & 2) ? 31u : 0xffffffffu) * (1u << ((op - 0x30) / 4));
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.r[side][6] == ((op < 0x3c && (op & 1)) ? 2 - offset : 2 + offset));
        }
    /* Complete scalar ADD/SUB .D family: same-bank register/unsigned
     * constant forms and cross-path register/signed-constant forms. */
    for (unsigned op = 0x10; op <= 0x13; ++op)
        for (unsigned side = 0; side < 2; ++side) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[side][3] = 5; c.r[side][6] = 2;
            memory[0] = 1u << 23 | 6u << 18 | 3u << 13 |
                        op << 7 | 0x40 | side << 1;
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            uint32_t right = op & 2 ? 3 : 5;
            assert(c.r[side][1] == ((op & 1) ? 2 - right : 2 + right));
        }
    const unsigned d_cross_ops[] = {0xab0, 0xaf0, 0xb30};
    for (unsigned n = 0; n < 3; ++n)
        for (unsigned side = 0; side < 2; ++side) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[side][3] = 0xfffffffdu;
            c.r[side ^ 1][6] = 5;
            memory[0] = 1u << 23 | 6u << 18 | 3u << 13 | 1u << 12 |
                        d_cross_ops[n] | side << 1;
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            uint32_t left = n == 1 ? 3 : 0xfffffffdu;
            assert(c.r[side][1] == (n == 2 ? left - 5 : left + 5));
        }
    /* The captured [A0] SUB .D1 A3,A6,A1 is atomic when false and wraps
     * exactly like 32-bit C674x integer arithmetic when true. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 99; c.r[0][3] = 5; c.r[0][6] = 2;
    memory[0] = 0xc09868c0;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == 99);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][0] = 1; c.r[0][3] = 5; c.r[0][6] = 2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == UINT32_MAX - 2);
    /* CMPLTU scalar forms compare unsigned values, including the captured
     * CMPLTU .L1 15,A1,A0 at 0x118044a4. */
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
            for (unsigned immediate = 0; immediate < 2; ++immediate) {
                cdj_c674x_reset(&c, 0x1000);
                c.r[side][3] = 15;
                c.r[side ^ cross_path][6] = 16;
                memory[0] = 1u << 23 | 6u << 18 | 3u << 13 |
                            cross_path << 12 | (immediate ? 0xbd8 : 0xbf8) |
                            side << 1;
                assert(cdj_c674x_step(&c, read_word, NULL, NULL));
                assert(c.r[side][1] == 1);
                cdj_c674x_reset(&c, 0x1000);
                c.r[side][1] = 99;
                memory[0] |= 6u << 29; /* false A0 predicate */
                assert(cdj_c674x_step(&c, read_word, NULL, NULL));
                assert(c.r[side][1] == 99);
            }
    /* Figure H-5 / GNU nfu_uspl: compact SPLOOP scatters ii-1 across
     * bits 9:7 and 14 and shares the full-width loop scheduler. */
    for (unsigned ii = 1; ii <= 16; ++ii) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = 1;
        unsigned encoded = ii - 1;
        memory[0] = 0x1c660000 | 0x0c66 |
                    (encoded & 7) << 7 | (encoded & 8) << 11;
        memory[7] = 0xe0200000; /* compact SPLOOP; compact SPKERNEL 0,0 */
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.loop_active && c.loop.ii == ii && c.control[13] == 0);
        unsigned steps = 0;
        while (c.loop_active) {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++steps < 64);
        }
    }
    /* H-5 SPLOOPD supports all compact II values.  Its initial condition
     * is forced false and ILC is not decremented for the first three loop
     * cycles, giving ceil(4/II) guaranteed iterations even for ILC=0. */
    for (unsigned ii = 1; ii <= 16; ++ii) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control_ready[13] = 999;
        unsigned encoded = ii - 1, minimum = (4 + ii - 1) / ii;
        memory[0] = 0x1c660000 | 0x0c67 |
                    (encoded & 7) << 7 | (encoded & 8) << 11;
        memory[7] = 0xe0200000;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.loop_active && c.loop.delayed_count && c.loop.ii == ii &&
               c.loop.iterations == minimum && c.control[13] == 0 &&
               (c.control[26] & (1u << 14)));
        unsigned steps = 0;
        while (c.loop_active) {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++steps < 80);
        }
        assert(!(c.control[26] & (1u << 14)));
    }
    /* Full SPLOOPD can load ILC in its own execute packet.  The scheduler
     * observes that E1 value after setup and adds the documented minimum;
     * ILC itself remains unchanged until the first eligible boundary. */
    for (unsigned ii = 1; ii <= 14; ++ii) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][2] = 2;
        unsigned minimum = (4 + ii - 1) / ii;
        memory[0] = (ii - 1) << 23 | 0x3a001; /* SPLOOPD || */
        memory[1] = 13u << 23 | 2u << 18 | 0x13a2; /* MVC A2,ILC */
        memory[2] = 3u << 23 | 3u << 18 | 1u << 13 | 0x58; /* ADD 1,A3,A3 */
        memory[3] = 0x34000;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.loop_active && c.loop.delayed_count &&
               c.loop.iterations == 2 + minimum && c.control[13] == 2 &&
               c.control_ready[13] == 4);
        unsigned steps = 0;
        while (c.loop_active) {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++steps < 100);
        }
        assert(c.r[0][3] == 2 + minimum && c.control[13] == 0);
    }
    /* Post-loop packets can prepare ILC for a following software loop.
     * Once SPLOOPD reaches its post phase, it must not consume the new value
     * written by that packet as though it belonged to the completed loop. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.loop_active = true; c.control[26] = 1u << 14;
    c.loop.ii = 1; c.loop.iterations = 5; c.loop.delayed_count = true;
    c.loop.sealed = true; c.loop.cycle = 4;
    c.loop.post_cycle = c.loop.end_cycle = 4;
    c.r[0][2] = 8;
    memory[0] = 13u << 23 | 2u << 18 | 0x13a2; /* MVC A2,ILC */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.loop_active && c.control[13] == 8);
    /* H-6 conditional SPLOOPD requests reload/nested-loop behavior, which
     * remains fail-closed and leaves the setup packet atomic.  SPRUFE8B's
     * SPLOOPD description (printed page 485) is what makes this a reload
     * rather than a plain predicate: "When the SPLOOPD instruction is
     * predicated, it indicates that the loop is a nested loop using the
     * SPLOOP reload capability."  Retained-buffer reload is not claimed.
     *
     * Sweep the whole format rather than two samples, because
     * analysis/dsp/audit_sweeps.json now claims exactly 32 words here:
     * Figure H-6 (printed page 766) fixes bit 15 = 1, bits 13-12 = 00,
     * bits 11-10 = 11 and bits 6-1 = 110011, leaving ii3 (bit 14), ii2-0
     * (bits 9-7) and op (bit 0) free - 2 * 8 * 2 = 32 encodings, every one of
     * which must fault with the same text and change nothing.  ii is
     * irrelevant to the refusal; the predicate is the whole reason for it. */
    for (unsigned encoded = 0; encoded < 16; ++encoded) {
        for (unsigned op = 0; op < 2; ++op) {
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            c.control[13] = 3; c.r[0][1] = 99;
            memory[0] = 0x8c66 | (encoded & 8) << 11 | (encoded & 7) << 7 | op;
            memory[7] = 0xe0200000;
            assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.cycles == 0 && !c.loop_active && c.r[0][1] == 99 &&
                   c.pc == 0x1000 && c.control[13] == 3);
            assert(!strcmp(c.fault, "SPLOOPD reload not implemented"));
        }
    }
    /* More than 14 source packets fit when they occupy no functional slots. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 1;
    memory[0] = 0x06838000; /* SPLOOP 14 */
    memory[21] = 0x34000;
    unsigned long_body_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++long_body_steps < 50);
    } while (c.loop_active);
    assert(c.loop_packets == 21 && c.loop_tags == 0);
    /* Figure G-3 predicate MVK: all predicate polarities, units, sides,
     * register subsets and constants. Predicates always use low A0/B0. */
    for (unsigned cc = 0; cc < 4; ++cc)
        for (unsigned unit = 0; unit < 3; ++unit)
            for (unsigned side = 0; side < 2; ++side)
                for (unsigned rs = 0; rs < 2; ++rs)
                    for (unsigned value = 0; value < 2; ++value)
                        for (unsigned predicate = 0; predicate < 2; ++predicate) {
                            cdj_c674x_reset(&c, 0x1000);
                            c.r[cc >> 1][0] = predicate;
                            c.r[side][3 + 16 * rs] = 55;
                            CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
                                .instructions = {{.compact = true, .pc = 0x1000,
                                    .header = rs << 19, .word = 0x0866 | cc << 14 |
                                        value << 13 | 3u << 7 | unit << 3 | side}}};
                            assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                            assert(c.r[side][3 + 16 * rs] == ((predicate ^ (cc & 1)) ? value : 55));
                        }
    /* SPMASK suppresses an existing buffered S1 write before merging a
     * program-memory replacement. Exercise full and compact encodings. */
    for (unsigned compact = 0; compact < 2; ++compact) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = 3;
        memory[0] = 0x38000;
        memory[1] = 4u << 23 | 4u << 18 | 1u << 13 | 0x1a0; /* ADD.S1 1,A4,A4 */
        memory[2] = compact ? 0x0c6e2d66 : 0x130001; /* SPMASK S1 */
        memory[3] = mvk(0, 4, 100);
        memory[4] = 0x34000;
        if (compact) memory[7] = 0xe0800030; /* SPMASK || NOP || MVK */
        for (unsigned j = 0; j < 4; ++j)
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][4] == 101 && c.loop_tags == 1);
        for (unsigned j = 0; j < 3 && c.loop_active; ++j)
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(!c.loop_active && c.r[0][4] == 101);
    }
    /* During the epilog, a program-memory SPMASK replaces a draining S1
     * operation without changing the buffer; it executes again next cycle. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 3;
    memory[0] = 0x38000; memory[1] = 2u << 13; /* SPLOOP 1; NOP 3 */
    memory[2] = 0x34001;
    memory[3] = 4u << 23 | 4u << 18 | 1u << 13 | 0x1a0;
    memory[4] = 0x130001; memory[5] = mvk(0, 4, 100);
    unsigned mask_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++mask_steps < 12);
    } while (c.loop_active);
    assert(c.r[0][4] == 101);
    /* Zero masks and idle masks are legal; a misplaced mask is atomic. */
    for (unsigned compact = 0; compact < 2; ++compact) {
        cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 2, .next_pc = 0x1008,
            .instructions = {{.pc = 0x1000, .compact = compact,
                               .word = compact ? 0x2c66 : 0x30000},
                              {.pc = 0x1004, .word = mvk(0, 4, 99)}}};
        assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.r[0][4] == 99);
        CdjC674xInstruction first = p.instructions[0];
        p.instructions[0] = p.instructions[1]; p.instructions[1] = first;
        cdj_c674x_reset(&c, 0x1000);
        assert(!cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.r[0][4] == 0 && c.cycles == 0);
    }
    /* All compact L/S/D mask bits: masked moves execute once; unmasked
     * moves remain buffered, including the other side of the same unit. */
    for (unsigned unit = 0; unit < 3; ++unit)
        for (unsigned side = 0; side < 2; ++side) {
            static const unsigned bits[] = {1,128,256,512,16384,32768};
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            c.control[13] = 3; c.r[side][1] = 99; c.r[side ^ 1][1] = 77;
            memory[0] = 0x838000; /* SPLOOP 2 */
            unsigned move = 0x46 | 4u << 13 | 1u << 7 | unit << 3;
            memory[1] = (move | side) << 16 | 0x2c66 | bits[2 * unit + side];
            memory[2] = (move | (side ^ 1)) | 0x0c6e0000;
            memory[3] = 0x34000;
            memory[7] = 0xe0c0000c; /* mask || move || opposite move */
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.r[side][4] == 99 && c.r[side ^ 1][4] == 77 && c.loop_tags == 1);
            c.r[side][1] = 55; c.r[side ^ 1][1] = 66;
            unsigned steps = 0;
            while (c.loop_active) {
                assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
                assert(++steps < 12);
            }
            assert(c.r[side][4] == 99 && c.r[side ^ 1][4] == 66);
        }
    /* Figure F-24 scatters an unsigned byte across four fields. Exercise all
     * constants, both banks and register subsets (including high-bit values). */
    for (unsigned rs = 0; rs < 2; ++rs)
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned k = 0; k < 256; ++k) {
                cdj_c674x_reset(&c, 0x1000);
                unsigned dst = k & 7;
                CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
                    .instructions = {{.compact = true, .pc = 0x1000,
                        .header = rs << 19,
                        .word = 0x12 | bank | dst << 7 | (k & 7) << 13 |
                            ((k >> 3) & 3) << 11 | ((k >> 5) & 3) << 5 |
                            ((k >> 7) & 1) << 10}}};
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(c.r[bank][dst + rs * 16] == k && c.cycles == 1);
                assert(c.r[bank ^ 1][dst + rs * 16] == 0);
            }
    for (unsigned k = 0; k < 16; ++k) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][20] = 0xffffffff;
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000, .header = 1u << 19,
                .word = 0x1441 | (k & 7) << 13 | (k >> 3) << 11 | 4u << 7}}};
        int32_t offset = k & 8 ? (int32_t)(k & 7) - 8 : (k ? (int32_t)k : 8);
        assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.r[1][20] == 0xffffffffu + (uint32_t)offset);
        assert(c.r[0][20] == 0xffffffff);
    }
    /* Appendix H compact NOP stores cycles - 1 in N3. In particular the
     * firmware blocker 0xec6e is NOP 8, not a one-cycle empty operation. */
    for (unsigned n3 = 0; n3 < 8; ++n3) {
        cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000,
                .word = 0x0c6e | n3 << 13}}};
        assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.cycles == n3 + 1 && c.pc == 0x1002);
    }
    /* Compact immediate-offset transfers share E3/E5 timing with full words.
     * Cover all header size selections and both register subsets. */
    for (unsigned rs = 0; rs < 2; ++rs)
        for (unsigned dsz = 0; dsz < 8; ++dsz)
            for (unsigned secondary = 0; secondary < 2; ++secondary) {
                static const unsigned sizes[8] = {1,1,2,2,4,1,4,2};
                unsigned size = secondary ? sizes[dsz] : (dsz & 4 ? 8 : 4);
                memset(memory, 0, sizeof(memory));
                cdj_c674x_reset(&c, 0x1000);
                c.r[1][4] = 0x1040; c.r[0][2 + rs * 16] = 0x87654321;
                c.r[0][3 + rs * 16] = 0x12345678;
                CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
                    .instructions = {{.compact = true, .pc = 0x1000,
                        .header = rs << 19 | dsz << 16,
                        .word = 0x25 | secondary << 9 | 1u << 13}}};
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(memory[16] == 0 && memory[17] == 0 && memory[18] == 0);
                p.count = 0;
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(memory[(0x40 + size) / 4] != 0);
                c.r[0][2 + rs * 16] = 0;
                c.r[0][3 + rs * 16] = 0;
                p.count = 1; p.instructions[0].word |= 8;
                p.instructions[0].header |= 1u << 20; /* PROT drains load. */
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                uint32_t expected = size == 1 ? 0x21 : size == 2 ? 0x4321 : 0x87654321;
                assert(c.r[0][2 + rs * 16] == expected);
                if (size == 8) assert(c.r[0][3 + rs * 16] == 0x12345678);
                assert(c.r[1][4] == 0x1040);
            }
    /* Doff4DW nonaligned offsets are bytes, with an even register pair. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0x1040; c.r[0][2] = 0x88776655; c.r[0][3] = 0xccbbaa99;
    CdjC674xPacket compact_mem = {.count = 1, .next_pc = 0x1002,
        .instructions = {{.compact = true, .pc = 0x1000, .header = 4u << 16,
            .word = 0x6035}}};
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    compact_mem.count = 0;
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(memory[16] == 0x55000000 && memory[17] == 0x99887766 && memory[18] == 0x00ccbbaa);
    compact_mem.count = 1; compact_mem.instructions[0].word |= 8;
    compact_mem.instructions[0].header |= 1u << 20;
    c.r[0][2] = c.r[0][3] = 0;
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(c.r[0][2] == 0x88776655 && c.r[0][3] == 0xccbbaa99);
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0x10fc;
    assert(!cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(c.fault_word == 0x603d && c.cycles == 0);
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(1, 15, -8);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 0xfffffff8);
    memory[1] = (15u << 23) | (0x1180u << 7) | 0x6a;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 0x1180fff8);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][0] = 7;
    memory[0] = mvk(1, 0, 9) | 1;
    memory[1] = (1u << 23) | (31u << 13) | (1u << 12) | 0xf58;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == 7 && c.r[1][0] == 9);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362;
    for (unsigned i = 1; i <= 6; ++i) memory[i] = mvk(0, 0, i);
    for (unsigned i = 0; i < 6; ++i) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.r[0][0] == 5 && c.cycles == 6);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362; memory[1] = 8u << 13;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x1234567f;
    memory[0] = (15u << 23) | (15u << 18) | (24u << 13) | 0x9f2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 0x12345678);
    memory[1] = mvk(0, 3, 99) | (6u << 29);
    memory[2] = mvk(0, 3, 42) | (6u << 29) | (1u << 28);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][3] == 0);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][3] == 42);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362;
    memory[1] = (3u << 23) | (3u << 16) | (4u << 13) | 0x162;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6 && c.r[1][3] == 0x100c);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(0, 0, 99) | 1; memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][0] == 0 && c.pc == 0x1000 && c.packets == 0);
    /* Header p bits, not opcode bit zero, join compact instructions. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 7; c.r[1][2] = 5;
    uint32_t add = (1u << 13) | (1u << 12) | (2u << 7) | (3u << 4);
    uint32_t sub = (3u << 13) | (1u << 12) | (2u << 7) | (4u << 4) | 0x800;
    memory[0] = add | (sub << 16);
    memory[7] = 0xe0200001; /* word 0 compact; first half parallel */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 12 && c.r[0][4] == (uint32_t)-5);
    assert(c.pc == 0x1004 && c.cycles == 1);

    /* RS applies to both operands and result; sequential halfword PCs. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][17] = 9; c.r[1][18] = 4;
    memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][19] == 13 && c.pc == 0x1002);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][20] == 9 && c.pc == 0x1004);

    /* Full instructions retain their p bit; packets skip the header. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1018);
    memory[6] = mvk(0, 1, 42) | 1;
    memory[7] = 0xe0000000;
    memory[8] = mvk(0, 2, 73);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1024 && c.r[0][1] == 42 && c.r[0][2] == 73);

    /* Signed immediate equality, cross path and predicate. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0xffffffff;
    memory[0] = (4u << 18) | (31u << 13) | (1u << 12) | 0xa58;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][0] == 1);
    memory[1] = (6u << 29) | (1u << 23) | (4u << 18) | (1u << 12) | 0xa78;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][1] == 0);

    /* Scalar .L comparisons share adjacent immediate/register opfields.
     * The signed relations sign-extend scst5; unsigned relations do not. */
    const unsigned compare_immediate[] = {0xa58, 0x8d8, 0x9d8, 0xad8, 0xbd8};
    const unsigned compare_expected[] = {0, 0, 1, 1, 0};
    for (unsigned relation = 0; relation < 5; ++relation)
    for (unsigned immediate = 0; immediate < 2; ++immediate)
    for (unsigned side = 0; side < 2; ++side) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned left = immediate ? 31 : 3, right = 4, dst = 5;
        c.r[side][left] = 0xffffffff;
        c.r[side ^ 1][right] = 1;
        memory[0] = dst << 23 | right << 18 | left << 13 | 1u << 12 |
                    compare_immediate[relation] | ((!immediate) << 5) | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == compare_expected[relation]);
    }

    /* INTSP/INTSPU form one four-cycle .L conversion family.  Results and
     * sticky FADCR INEX appear in E4; all rounding is integer-derived so the
     * tests do not depend on the host floating-point environment. */
    struct IntSpCase {
        uint32_t source, expected;
        unsigned rmode;
        bool unsigned_source, inexact;
    } int_sp_cases[] = {
        {0,          0x00000000, 0, false, false},
        {1,          0x3f800000, 0, false, false},
        {0xffffffff, 0xbf800000, 0, false, false},
        {0x80000000, 0xcf000000, 0, false, false},
        {0x7fffffff, 0x4f000000, 0, false, true},
        {0x7fffffff, 0x4effffff, 1, false, true},
        {0x7fffffff, 0x4f000000, 2, false, true},
        {0x7fffffff, 0x4effffff, 3, false, true},
        {0xffffffff, 0x4f800000, 0, true,  true},
        {0xffffffff, 0x4f7fffff, 1, true,  true},
        {0xffffffff, 0x4f800000, 2, true,  true},
        {0xffffffff, 0x4f7fffff, 3, true,  true},
        {0x01000001, 0x4b800000, 0, false, true},
        {0x01000003, 0x4b800002, 0, false, true},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(int_sp_cases) / sizeof(int_sp_cases[0]); ++j) {
        struct IntSpCase tc = int_sp_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned dst = 4, src = 3, shift = side ? 25 : 9;
        c.r[side][dst] = 0xdeadbeef;
        c.r[side ^ cross_path][src] = tc.source;
        c.control[18] = tc.rmode << shift;
        memory[0] = dst << 23 | src << 18 | cross_path << 12 |
                    (tc.unsigned_source ? 0x938 : 0x958) | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.load_count == 1 &&
               c.loads[0].due == 4 && c.loads[0].size == 0 &&
               c.r[side][dst] == 0xdeadbeef &&
               c.control[18] == tc.rmode << shift);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == tc.expected && !c.load_count);
        assert(c.control[18] == ((tc.rmode << shift) |
               (tc.inexact ? 1u << (side ? 23 : 7) : 0)));
    }

    /* MPYSP uses FMCR, flushes denormalized operands and underflow outputs as
     * specified by TI, and implements all four directed rounding modes. */
    struct MpySpCase {
        uint32_t left, right, expected, status;
        unsigned rmode;
    } mpy_sp_cases[] = {
        {0x3fc00000, 0x40000000, 0x40400000, 0x000, 0}, /* 1.5 * 2 */
        {0xc0200000, 0x4109999a, 0xc1ac0000, 0x080, 0}, /* TI -2.5 * 8.6 */
        {0x7fc00001, 0x40000000, 0x7fffffff, 0x001, 0}, /* QNaN */
        {0x7f800001, 0x40000000, 0x7fffffff, 0x011, 0}, /* SNaN */
        {0x7f800000, 0x40000000, 0x7f800000, 0x020, 0}, /* infinity */
        {0x7f800000, 0x00000000, 0x7fffffff, 0x010, 0}, /* infinity * zero */
        {0x80000001, 0x40000000, 0x80000000, 0x084, 0}, /* denormal * normal */
        {0x00000001, 0x00000000, 0x00000000, 0x004, 0}, /* denormal * zero */
        {0x7f800000, 0x00000001, 0x7fffffff, 0x018, 0}, /* infinity * denormal */
        {0x7f7fffff, 0x40000000, 0x7f800000, 0x0e0, 0}, /* overflow nearest */
        {0x7f7fffff, 0x40000000, 0x7f7fffff, 0x0c0, 1}, /* overflow truncate */
        {0xff7fffff, 0x40000000, 0xff7fffff, 0x0c0, 2}, /* negative toward +inf */
        {0xff7fffff, 0x40000000, 0xff800000, 0x0e0, 3}, /* negative toward -inf */
        {0x00800000, 0x3f000000, 0x00000000, 0x180, 0}, /* underflow nearest */
        {0x00800000, 0x3f000000, 0x00800000, 0x180, 2}, /* underflow upward */
        {0x80800000, 0x3f000000, 0x80800000, 0x180, 3}, /* underflow downward */
        {0x3f800001, 0x3f800001, 0x3f800002, 0x080, 0}, /* rounded nearest */
        {0x3f800001, 0x3f800001, 0x3f800003, 0x080, 2}, /* rounded upward */
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(mpy_sp_cases) / sizeof(mpy_sp_cases[0]); ++j) {
        struct MpySpCase tc = mpy_sp_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned dst = 5, left = 3, right = 4, shift = side ? 16 : 0;
        c.r[side][left] = tc.left;
        c.r[side ^ cross_path][right] = tc.right;
        c.r[side][dst] = 0xdeadbeef;
        c.control[20] = tc.rmode << (shift + 9);
        memory[0] = dst << 23 | right << 18 | left << 13 |
                    cross_path << 12 | 0xe00 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.load_count == 1 && c.loads[0].due == 4 &&
               !c.loads[0].size && c.loads[0].sign_extend &&
               c.r[side][dst] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == tc.expected && !c.load_count);
        assert(c.control[20] == ((tc.rmode << (shift + 9)) |
                                (tc.status << shift)));
        assert(!c.control[18]);
    }

    /* SPINT follows FADCR rounding and SPTRUNC ignores it.  NaNs, infinity,
     * denormals, overflow, ties and signed directed rounding set the exact
     * documented sticky status bits with the E4 result. */
    struct SpIntCase {
        uint32_t source, expected, status;
        unsigned rmode;
    } sp_int_cases[] = {
        {0x4109999a, 9,          0x080, 0}, /* 8.6 nearest */
        {0x4109999a, 8,          0x080, 1},
        {0x4109999a, 9,          0x080, 2},
        {0x4109999a, 8,          0x080, 3},
        {0xc109999a, 0xfffffff7, 0x080, 0}, /* -8.6 nearest */
        {0xc109999a, 0xfffffff8, 0x080, 1},
        {0xc109999a, 0xfffffff8, 0x080, 2},
        {0xc109999a, 0xfffffff7, 0x080, 3},
        {0x3f000000, 0,          0x080, 0}, /* 0.5 ties to even */
        {0x3fc00000, 2,          0x080, 0}, /* 1.5 ties to even */
        {0x40200000, 2,          0x080, 0}, /* 2.5 ties to even */
        {0x4effffff, 0x7fffff80, 0x000, 0}, /* largest exact in-range SP */
        {0xcf000000, 0x80000000, 0x000, 0}, /* -2^31 is valid */
        {0x4f000000, 0x7fffffff, 0x0c0, 0}, /* positive overflow */
        {0xcf000001, 0x80000000, 0x0c0, 0}, /* negative overflow */
        {0x7f800000, 0x7fffffff, 0x0c0, 0}, /* infinity */
        {0xff800000, 0x80000000, 0x0c0, 0},
        {0x7fc00001, 0x7fffffff, 0x012, 0}, /* NaN */
        {0xffc00001, 0x80000000, 0x012, 0},
        {0x00000001, 0,          0x088, 0}, /* denormal */
        {0x80000000, 0,          0x000, 0}, /* negative zero */
    };
    for (unsigned truncate = 0; truncate < 2; ++truncate)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(sp_int_cases) / sizeof(sp_int_cases[0]); ++j) {
        struct SpIntCase tc = sp_int_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned dst = 5, src = 4, shift = side ? 16 : 0;
        /* For SPTRUNC, derive the expected finite rounded value by selecting
         * the matching toward-zero row when this table has mode variants. */
        uint32_t expected = tc.expected;
        if (truncate && j < 4) expected = 8;
        if (truncate && j >= 4 && j < 8) expected = 0xfffffff8;
        if (truncate && (j == 9 || j == 10)) expected = j == 9 ? 1 : 2;
        c.r[side ^ cross_path][src] = tc.source;
        c.r[side][dst] = 0xdeadbeef;
        c.control[18] = tc.rmode << (shift + 9);
        memory[0] = dst << 23 | src << 18 | cross_path << 12 |
                    (truncate ? 0x178 : 0x158) | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.load_count == 1 && c.loads[0].due == 4 &&
               !c.loads[0].size && !c.loads[0].sign_extend);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == expected && !c.load_count);
        assert(c.control[18] == ((tc.rmode << (shift + 9)) |
                                (tc.status << shift)));
    }

    /* False predicates create no delayed result or warning; an unsupported
     * parallel instruction rolls the whole issue packet back. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 99; c.r[0][3] = 0x7fffffff;
    memory[0] = 2u << 29 | 4u << 23 | 3u << 18 | 0x958;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 99 && !c.load_count && !c.control[18]);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 99; c.r[0][3] = 7;
    memory[0] = 4u << 23 | 3u << 18 | 0x959;
    memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.r[0][4] == 99 && !c.load_count && !c.control[18]);

    /* A same-cycle E1 write to an E4 destination is rejected before either
     * operation commits; the pending conversion remains checkpointable. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 99; c.r[0][3] = 7;
    memory[0] = 4u << 23 | 3u << 18 | 0x958;
    memory[3] = mvk(0, 4, 1);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 3 && c.r[0][4] == 99 && c.load_count == 1 &&
           c.loads[0].size == 0 && c.loads[0].value == 0x40e00000);

    /* Non-saturating 16x16 scalar multiplies cover every signed/unsigned
     * high/low permutation plus both signed-constant forms.  SPRUFE8B makes
     * these E1-read/E2-write operations with one delay slot. */
    struct ScalarMpyCase { unsigned op; uint32_t expected; } scalar_mpy_cases[] = {
        {0x19, 0x00007ffe}, {0x01, 0x0000fffe}, {0x09, 0x3ffe8002},
        {0x0f, 0x40018002}, {0x0b, 0xbfff8002}, {0x03, 0x8001fffe},
        {0x07, 0x7ffffffe}, {0x0d, 0xc0008002}, {0x05, 0xfffefffe},
        {0x11, 0x00000002}, {0x17, 0xfffd0002}, {0x13, 0xffff0002},
        {0x15, 0xfffe0002}, {0x1b, 0xffff7ffe}, {0x1f, 0x80017ffe},
        {0x1d, 0x80027ffe}, {0x18, 0x00017ffa}, {0x1e, 0xfffe7ffa},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(scalar_mpy_cases) /
                              sizeof(scalar_mpy_cases[0]); ++j) {
        unsigned op = scalar_mpy_cases[j].op;
        unsigned src1 = op == 0x18 || op == 0x1e ? 29 : 4;
        unsigned src2 = 3, dst = 6;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][4] = 0x8001ffff;
        c.r[side ^ cross_path][src2] = 0xfffe8002;
        c.r[side][dst] = 0xdeadbeef;
        memory[0] = dst << 23 | src2 << 18 | src1 << 13 |
                    cross_path << 12 | op << 7 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.load_count == 1 &&
               c.loads[0].due == 2 && c.r[side][dst] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == scalar_mpy_cases[j].expected && !c.load_count);
    }

    /* A false predicate creates no delayed scalar product. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0xffff; c.r[0][3] = 0x8002; c.r[0][6] = 99;
    memory[0] = 2u << 29 | 6u << 23 | 3u << 18 | 4u << 13 | 0x1du << 7;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][6] == 99 && !c.load_count);

    /* MPY32 and the signedness variants are four-cycle 32x32 operations.
     * The scalar MPY32 keeps the low word; every other form publishes a
     * complete low/high register pair in E4. */
    struct Mpy32Case {
        uint32_t left, right;
        uint64_t expected;
        unsigned op;
        bool compound, pair;
    } mpy32_cases[] = {
        {0x80000000, 0xffffffff, UINT64_C(0x0000000080000000),
         0x10, false, false},
        {0x7fffffff, 0x7fffffff, UINT64_C(0x0000000000000001),
         0x10, false, false},
        {0x80000000, 0xffffffff, UINT64_C(0x0000000080000000),
         0x14, false, true},
        {0x80000000, 0x80000000, UINT64_C(0x4000000000000000),
         0x14, false, true},
        {0xffffffff, 0xffffffff, UINT64_C(0xffffffff00000001),
         0x16, false, true},
        {0x80000000, 0xffffffff, UINT64_C(0x8000000080000000),
         0x16, false, true},
        {0xffffffff, 0xffffffff, UINT64_C(0xfffffffe00000001),
         0x18, true, true},
        {0xffffffff, 0xffffffff, UINT64_C(0xffffffff00000001),
         0x19, true, true},
        {0xffffffff, 0x80000000, UINT64_C(0x8000000080000000),
         0x19, true, true},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(mpy32_cases) /
                              sizeof(mpy32_cases[0]); ++j) {
        struct Mpy32Case tc = mpy32_cases[j];
        unsigned dst = 6, src1 = 4, src2 = 3;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][src1] = tc.left;
        c.r[side ^ cross_path][src2] = tc.right;
        c.r[side][dst] = c.r[side][dst + 1] = 0xdeadbeef;
        memory[0] = dst << 23 | src2 << 18 | src1 << 13 |
                    cross_path << 12 | side << 1 |
                    (tc.compound ? tc.op << 6 | 0x30 : tc.op << 7);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.load_count == 1 &&
               c.loads[0].due == 4 &&
               c.loads[0].size == (tc.pair ? 16u : 0u));
        assert(c.r[side][dst] == 0xdeadbeef &&
               c.r[side][dst + 1] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == (uint32_t)tc.expected && !c.load_count);
        assert(c.r[side][dst + 1] ==
               (tc.pair ? (uint32_t)(tc.expected >> 32) : 0xdeadbeefu));
    }

    /* Every encoding observes predicates before queueing a result.  Pair
     * destinations must nevertheless be even: malformed register-pair
     * encodings fail closed, including when their predicate is false. */
    for (unsigned j = 0; j < sizeof(mpy32_cases) /
                              sizeof(mpy32_cases[0]); ++j) {
        struct Mpy32Case tc = mpy32_cases[j];
        unsigned dst = 6;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][dst] = c.r[0][dst + 1] = 99;
        memory[0] = 2u << 29 | dst << 23 | 3u << 18 | 4u << 13 |
                    (tc.compound ? tc.op << 6 | 0x30 : tc.op << 7);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][dst] == 99 && c.r[0][dst + 1] == 99 &&
               !c.load_count);
        if (tc.pair) {
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            memory[0] = 2u << 29 | 5u << 23 | 3u << 18 | 4u << 13 |
                        (tc.compound ? tc.op << 6 | 0x30 : tc.op << 7);
            assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(!c.cycles && !c.load_count);
        }
    }

    /* Delayed E4 publication detects writes to either half of a pending
     * pair, and the scalar form protects its one destination register. */
    for (unsigned high_half = 0; high_half < 2; ++high_half) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = 0x80000000; c.r[0][3] = 0xffffffff;
        memory[0] = 6u << 23 | 3u << 18 | 4u << 13 | 0x14u << 7;
        memory[3] = mvk(0, 6 + high_half, 1);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 3 && c.load_count == 1 &&
               c.loads[0].size == 16 &&
               c.loads[0].value == UINT64_C(0x0000000080000000));
    }
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x80000000; c.r[0][3] = 0xffffffff;
    memory[0] = 6u << 23 | 3u << 18 | 4u << 13 | 0x10u << 7;
    memory[3] = mvk(0, 6, 1);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 3 && c.load_count == 1 && !c.loads[0].size &&
           c.loads[0].value == UINT64_C(0x80000000));

    /* MPYIH/MPYHI, MPYIL/MPYLI and their rounded forms are one M-unit
     * compound family.  The full products are signed, sign-extended 64-bit
     * register pairs; rounded variants add 0x4000 before arithmetic >> 15. */
    struct CompoundMpyCase {
        uint32_t half_source, full_source;
        uint64_t expected;
        unsigned op;
    } compound_mpy_cases[] = {
        /* TI MPYLI examples 1 and 2. */
        {0x6a321193, 0xb1746ca4, UINT64_C(0xfffffa9ba111462c), 0x15},
        {0x12343497, 0x21ff50a7, UINT64_C(0x000006fbe9fa7e81), 0x15},
        {0xfffe1234, 0x40000001, UINT64_C(0xffffffff7ffffffe), 0x14},
        {0x80000000, 0x80000000, UINT64_C(0x0000400000000000), 0x14},
        {0x00000001, 0x00003fff, UINT64_C(0x00000000), 0x0e},
        {0x00000001, 0x00004000, UINT64_C(0x00000001), 0x0e},
        {0x0000ffff, 0x00004000, UINT64_C(0x00000000), 0x0e},
        {0xffff0000, 0x00004001, UINT64_C(0xffffffff), 0x10},
        {0x7fff0000, 0x7fffffff, UINT64_C(0x7ffeffff), 0x10},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(compound_mpy_cases) /
                              sizeof(compound_mpy_cases[0]); ++j) {
        struct CompoundMpyCase tc = compound_mpy_cases[j];
        bool pair = tc.op == 0x14 || tc.op == 0x15;
        unsigned dst = 6, half_src = 4, full_src = 3;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][half_src] = tc.half_source;
        c.r[side ^ cross_path][full_src] = tc.full_source;
        c.r[side][dst] = c.r[side][dst + 1] = 0xdeadbeef;
        memory[0] = dst << 23 | full_src << 18 | half_src << 13 |
                    cross_path << 12 | tc.op << 6 | 0x30 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.load_count == 1 &&
               c.loads[0].due == 4 && c.loads[0].size == (pair ? 16u : 0u));
        assert(c.r[side][dst] == 0xdeadbeef &&
               c.r[side][dst + 1] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == (uint32_t)tc.expected && !c.load_count);
        assert(c.r[side][dst + 1] ==
               (pair ? (uint32_t)(tc.expected >> 32) : 0xdeadbeefu));
    }

    /* Pair alignment and both halves of an E4/E1 collision are checked
     * before state changes.  A false predicate queues no multiply result. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 5u << 23 | 3u << 18 | 4u << 13 | 0x15u << 6 | 0x30;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && !c.load_count);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 9; c.r[0][4] = 7;
    memory[0] = 6u << 23 | 3u << 18 | 4u << 13 | 0x15u << 6 | 0x30;
    memory[3] = mvk(0, 7, 1);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 3 && c.load_count == 1 && c.loads[0].size == 16 &&
           c.r[0][6] == 0 && c.r[0][7] == 0);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 0; c.r[0][6] = c.r[0][7] = 99;
    memory[0] = 2u << 29 | 6u << 23 | 3u << 18 | 4u << 13 |
                0x15u << 6 | 0x30;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][6] == 99 && c.r[0][7] == 99 && !c.load_count);

    /* ABSSP is a bit-exact single-cycle .S operation.  Its NaN, denormal and
     * infinity warnings go to FAUCR; normal values and signed zero only have
     * their sign bit cleared. */
    struct AbsSpCase { uint32_t source, expected, status; } abs_sp_cases[] = {
        {0xc0200000, 0x40200000, 0x00},
        {0x80000000, 0x00000000, 0x00},
        {0x00000001, 0x00000000, 0x88},
        {0x807fffff, 0x00000000, 0x88},
        {0x7f800000, 0x7f800000, 0x20},
        {0xff800000, 0x7f800000, 0x20},
        {0x7fc00001, 0x7fffffff, 0x02},
        {0xff800001, 0x7fffffff, 0x12},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(abs_sp_cases) / sizeof(abs_sp_cases[0]); ++j) {
        struct AbsSpCase tc = abs_sp_cases[j];
        unsigned dst = 12, src = 4, shift = side ? 16 : 0;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side ^ cross_path][src] = tc.source;
        memory[0] = dst << 23 | src << 18 | cross_path << 12 |
                    0xf20 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.r[side][dst] == tc.expected);
        assert(c.control[19] == tc.status << shift);
        assert(!c.control[18] && !c.control[20]);
    }
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 0; c.r[0][4] = 0x7f800001; c.r[0][12] = 99;
    memory[0] = 2u << 29 | 12u << 23 | 4u << 18 | 0xf20;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][12] == 99 && !c.control[19]);

    /* The complete scalar SP comparison family treats denormals as signed
     * zero, makes NaNs unordered, and writes only the documented FAUCR
     * NANn/DENn/UNORD/INVAL bits. */
    struct CompareSpCase {
        uint32_t left, right, expected, status;
        unsigned relation;
    } compare_sp_cases[] = {
        {0xc0200000, 0x4109999a, 0, 0x000, 0}, /* -2.5 == 8.6 */
        {0xc0200000, 0x4109999a, 0, 0x000, 1},
        {0xc0200000, 0x4109999a, 1, 0x000, 2},
        {0x4109999a, 0xc0200000, 1, 0x000, 1},
        {0x80000000, 0x00000000, 1, 0x000, 0},
        {0x00000001, 0x80000000, 1, 0x004, 0},
        {0x80000001, 0x00000001, 1, 0x00c, 0},
        {0x00000001, 0x00000000, 0, 0x004, 1},
        {0xff800000, 0x7f800000, 1, 0x000, 2},
        {0x7f800000, 0x3f800000, 1, 0x000, 1},
        {0x7fc00001, 0x3f800000, 0, 0x201, 0},
        {0x3f800000, 0x7f800001, 0, 0x212, 1},
        {0x7fc00001, 0xffc00001, 0, 0x213, 2},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(compare_sp_cases) /
                              sizeof(compare_sp_cases[0]); ++j) {
        struct CompareSpCase tc = compare_sp_cases[j];
        unsigned dst = 6, left = 4, right = 3, shift = side ? 16 : 0;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][left] = tc.left;
        c.r[side ^ cross_path][right] = tc.right;
        c.control[19] = 1u << shift; /* warnings are sticky */
        memory[0] = dst << 23 | right << 18 | left << 13 |
                    cross_path << 12 | (0x38u + tc.relation) << 6 |
                    0x20 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.r[side][dst] == tc.expected);
        assert(c.control[19] == (1u | tc.status) << shift);
    }

    /* ADDSP/SUBSP cover both .L/.S encodings and both reverse-subtract
     * layouts.  All forms read in E1 and commit result plus FADCR warnings
     * together in E4. */
    const unsigned add_sub_sp_forms[] = {0x218, 0xe18, 0x238, 0x2b8, 0xe38, 0xeb8};
    for (unsigned form = 0; form < 6; ++form)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path) {
        unsigned encoding = add_sub_sp_forms[form];
        bool add = form < 2, l_reverse = form == 3, s_reverse = form == 5;
        unsigned dst = 6, src1 = 4, src2 = 3;
        uint32_t left = 0x4109999a, right = 0xc0200000;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        if (l_reverse) {
            c.r[side ^ cross_path][src1] = left;
            c.r[side][src2] = right;
        } else if (s_reverse) {
            c.r[side][src1] = right;
            c.r[side ^ cross_path][src2] = left;
        } else {
            c.r[side][src1] = left;
            c.r[side ^ cross_path][src2] = right;
        }
        c.r[side][dst] = 0xdeadbeef;
        memory[0] = dst << 23 | src2 << 18 | src1 << 13 |
                    cross_path << 12 | encoding | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.load_count == 1 && c.loads[0].due == 4 &&
               c.loads[0].size == 0 && c.r[side][dst] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == (add ? 0x40c33334u : 0x4131999au));
        assert(!c.load_count && !c.control[18]);
    }

    struct AddSubSpCase {
        uint32_t left, right, expected, status;
        unsigned encoding, rmode;
    } add_sub_sp_cases[] = {
        {0x3f800000, 0x3f800000, 0x40000000, 0x000, 0x218, 0},
        {0x3f800000, 0x33800000, 0x3f800000, 0x080, 0x218, 0},
        {0x3f800000, 0x33800000, 0x3f800001, 0x080, 0x218, 2},
        {0x3f800000, 0x33c00000, 0x3f800001, 0x080, 0x218, 0},
        {0x7f7fffff, 0x7f7fffff, 0x7f800000, 0x0e0, 0x218, 0},
        {0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x0c0, 0x218, 1},
        {0xff7fffff, 0xff7fffff, 0xff7fffff, 0x0c0, 0x218, 2},
        {0xff7fffff, 0xff7fffff, 0xff800000, 0x0e0, 0x218, 3},
        {0x00800000, 0x80800001, 0x80000000, 0x180, 0x218, 0},
        {0x00800000, 0x80800001, 0x80800000, 0x180, 0x218, 3},
        {0x3f800000, 0xbf800000, 0x00000000, 0x000, 0x218, 0},
        {0x3f800000, 0xbf800000, 0x80000000, 0x000, 0x218, 3},
        {0x80000000, 0x80000000, 0x80000000, 0x000, 0x218, 0},
        {0x7fc00001, 0x3f800000, 0x7fffffff, 0x001, 0x218, 0},
        {0x3f800000, 0x7f800001, 0x7fffffff, 0x012, 0x218, 0},
        {0x7f800000, 0xff800000, 0x7fffffff, 0x010, 0x218, 0},
        {0xff800000, 0xbf800000, 0xff800000, 0x020, 0x218, 0},
        {0x00000001, 0x3f800000, 0x3f800000, 0x084, 0x218, 0},
        {0x80000001, 0x7f800000, 0x7f800000, 0x024, 0x218, 0},
        {0x3f800000, 0x3f800000, 0x00000000, 0x000, 0x238, 0},
        {0x3f800000, 0x3f800000, 0x80000000, 0x000, 0x238, 3},
        {0x80000000, 0x00000000, 0x80000000, 0x000, 0x238, 0},
        {0x00000000, 0x80000000, 0x00000000, 0x000, 0x238, 0},
        {0x7f800000, 0x7f800000, 0x7fffffff, 0x010, 0x238, 0},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned j = 0; j < sizeof(add_sub_sp_cases) /
                              sizeof(add_sub_sp_cases[0]); ++j) {
        struct AddSubSpCase tc = add_sub_sp_cases[j];
        unsigned dst = 6, src1 = 4, src2 = 3, shift = side ? 16 : 0;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][src1] = tc.left; c.r[side][src2] = tc.right;
        c.control[18] = tc.rmode << (shift + 9);
        memory[0] = dst << 23 | src2 << 18 | src1 << 13 |
                    tc.encoding | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == tc.expected);
        assert(c.control[18] == ((tc.rmode << (shift + 9)) |
                                (tc.status << shift)));
    }

    /* Reverse .S subtraction reports source-specific warnings according to
     * encoded fields: the assembly left operand is encoded as src2. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x3f800000; /* encoded src1 / assembly right */
    c.r[1][3] = 0x00000001; /* encoded src2 / assembly left */
    memory[0] = 6u << 23 | 3u << 18 | 4u << 13 | 1u << 12 | 0xeb8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][6] == 0xbf800000 && c.control[18] == 0x88);

    /* A later unknown compact instruction rolls back the whole packet.
     * fff7h is not an instruction at all: TI's own disassembler prints it as
     * ".word 0x0000fff7".  (ffffh used to serve here, but it is the Figure E-5
     * M3 word MPYHL .M2X B7,A7,B6 and is now executed.) */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(0, 0, 99) | 1;
    memory[1] = 0xfff7; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.fault_pc == 0x1004 && c.fault_word == 0xfff7);
    assert(c.r[0][0] == 0 && c.pc == 0x1000 && c.cycles == 0);
    /* Stack stores sample old registers and update B15 in E1, RAM in E3.
     * RS is ignored by Dpp. Two pushes can be in flight simultaneously. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 0x10f8; c.r[1][10] = 0x12345678;
    c.r[0][12] = 0x89abcdef; c.r[0][13] = 0x76543210;
    memory[0] = 0x86773577; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f0 && c.store_count == 1 && memory[62] == 0);
    c.r[1][10] = 0; /* subsequent changes cannot alter the pending value */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10e8 && c.store_count == 2 && memory[62] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[62] == 0x12345678 && memory[60] == 0 && c.store_count == 1);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[60] == 0x89abcdef && memory[61] == 0x76543210 && !c.store_count);

    /* The paired Dpp loads pre-increment B15, sample RAM in E3 and publish
     * word/doubleword destinations in E5. Register side comes from t; RS is
     * ignored just as it is for the store forms. */
    memory[60] = 0x13579bdf; memory[62] = 0x2468ace0;
    memory[63] = 0xfdb97531;
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10e8;
    memory[0] = 0xc17771f7; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f0 && c.r[1][3] == 0 && c.load_count == 1);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f8 && c.r[0][2] == 0 && c.load_count == 2);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][3] == 0 && c.r[0][2] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][3] == 0 && c.r[0][2] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][3] == 0x13579bdf && c.r[0][2] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[0][2] == 0x2468ace0 && c.r[0][3] == 0xfdb97531 &&
           !c.load_count);

    /* PROT covers every load in a compact fetch packet.  In particular,
     * the stage-two firmware uses this exact Dpp encoding to restore B3;
     * the four added cycles must publish B3 before the following branch can
     * sample it.  Dstk takes the same protected-load path despite returning
     * early from its format-specific decoder. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 0x10e8; memory[60] = 0x12345678;
    memory[0] = 0x71f7; memory[7] = 0xe0300000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 5 && c.r[1][15] == 0x10f0 &&
           c.r[1][3] == 0x12345678 && !c.load_count);

    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 0x10e8; memory[59] = 0x89abcdef;
    memory[0] = 0xbc4d; memory[7] = 0xe0300000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.cycles == 5 && c.r[1][15] == 0x10e8 &&
           c.r[1][4] == 0x89abcdef && !c.load_count);

    /* Dstk uses B15 plus an unsigned scaled five-bit constant without base
     * update. Cover captured STW *+B15[1],B4 and the matching RS load. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 0x10e8; c.r[1][4] = 0xa5a55a5a;
    memory[0] = 0xbc45bc4d; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10e8 && c.load_count == 1 && c.store_count == 0);
    for (unsigned j = 0; j < 4; ++j)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    /* The following store captures the old B4 in E1 before the load's E5
     * writeback, and commits it one cycle before that writeback. */
    assert(c.r[1][4] == 0 && c.r[1][15] == 0x10e8 &&
           !c.load_count && !c.store_count);
    assert(memory[(0x10ec - 0x1000) / 4] == 0xa5a55a5a);

    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10e8;
    memory[(0x10ec - 0x1000) / 4] = 0xface1234;
    memory[0] = 0xbc4d; memory[7] = 0xe0280000 | (1u << 19);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned j = 0; j < 4; ++j)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][20] == 0xface1234 && c.r[1][4] == 0 &&
           c.r[1][15] == 0x10e8);

    /* Unsupported parallel operation must not enqueue the earlier store.
     * fff7h, not ffffh: see the M3 note above. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10f8;
    memory[0] = 0xfff73577; memory[7] = 0xe0200001;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f8 && !c.store_count && !c.cycles && !memory[62]);

    /* Bounds and doubleword alignment fail before changing the stack. */
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x1100;
    memory[0] = 0x3577; memory[7] = 0xe0200000;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x1100 && !c.store_count);
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10f4;
    memory[0] = 0x8677;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f4 && !c.store_count);
    /* Relative branches use fetch-packet base, signed offsets and five
     * delay cycles. A backward target must not use the opcode address. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1028);
    memory[10] = (0x1ffffcu << 7) | 0x10; /* fetch 1020 - 16 = 1010 */
    memory[11] = 8u << 13; /* NOP 9 ends early at the branch */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x102c && c.branch_target == 0x1010);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1010 && c.cycles == 6 && !c.branch_due);
    cdj_c674x_reset(&c, 0x1028);
    memory[10] |= 6u << 29; /* false A0 predicate */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.branch_due && c.pc == 0x102c);

    /* BDEC: signed counter, independent predicate, fetch-relative word
     * displacement (also in header packets), and five delay slots. */
    const uint32_t counters[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff};
    const int bdec_displacements[] = {-512, -1, 0, 3, 511};
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned ci = 0; ci < 5; ++ci)
    for (unsigned di = 0; di < 5; ++di)
    for (unsigned pred = 0; pred < 3; ++pred)
    for (unsigned header = 0; header < 2; ++header) {
        cdj_c674x_reset(&c, 0x1028);
        c.r[side][10] = counters[ci];
        CdjC674xPacket bp = {.count = 1, .next_pc = 0x102c};
        bp.instructions[0] = (CdjC674xInstruction){
            .pc = 0x1028, .header = header ? 0xe0000000 : 0,
            .word = 0x1020 | (side << 1) | (10u << 23) |
                    (((uint32_t)bdec_displacements[di] & 1023) << 13) |
                    (pred ? (6u << 29) : 0) | (pred == 2 ? 1u << 28 : 0)
        };
        bool taken = pred != 1 && ci < 3;
        assert(cdj_c674x_execute(&c, &bp, read_word, NULL, NULL));
        assert(c.r[side][10] == counters[ci] - (taken ? 1u : 0u));
        assert(c.branch_due == (taken ? 6u : 0u));
        if (taken) {
            assert(c.branch_target == 0x1020u + (uint32_t)(bdec_displacements[di] * 4));
            memset(memory, 0, sizeof(memory));
            for (unsigned delay = 0; delay < 5; ++delay) {
                assert(cdj_c674x_step(&c, read_word, NULL, NULL));
                if (delay < 4) assert(c.pc == 0x1030u + delay * 4);
            }
            assert(c.pc == 0x1020u + (uint32_t)(bdec_displacements[di] * 4));
        }
    }
    /* The actual failing firmware instruction reads A0 before decrement. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x100c); c.r[0][0] = 3;
    memory[3] = 0xc0007020;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][0] == 2 && c.branch_target == 0x100c);
    /* Forbidden ADDKPC pairing is rejected atomically in either order. */
    for (unsigned order = 0; order < 2; ++order) {
        cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 7;
        CdjC674xPacket bp = {.count = 2, .next_pc = 0x1008};
        bp.instructions[order] = (CdjC674xInstruction){.pc = 0x1000, .word = 0x05001020};
        bp.instructions[1-order] = (CdjC674xInstruction){.pc = 0x1004, .word = 0x00800162};
        assert(!cdj_c674x_execute(&c, &bp, read_word, NULL, NULL));
        assert(c.r[0][10] == 7 && c.r[1][1] == 0 && !c.cycles && !c.branch_due);
    }

    /* MVD reads at issue and writes at E4, including cross-path sources. */
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned pred = 0; pred < 2; ++pred) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000);
        c.r[side ^ cross][4] = 0x89abcdef;
        c.r[side][8] = 0x12345678;
        memory[0] = 0x340f0 | (4u << 18) | (8u << 23) |
                    (side << 1) | (cross << 12) | (pred ? 6u << 29 : 0);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][8] == 0x12345678);
        c.r[side ^ cross][4] = 0;
        for (unsigned delay = 0; delay < 3; ++delay) {
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(c.r[side][8] == (delay == 2 && !pred ? 0x89abcdef : 0x12345678));
        }
    }

    /* Compact moves in both directions between full and subset registers.
     * Parallel source reads see the old value, including across register files. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][27] = 0xabcdef01; c.r[0][17] = 0x1234;
    uint32_t to = (1u << 13) | (1u << 12) | (3u << 10) | (3u << 7) | 6;
    uint32_t from = (6u << 13) | (3u << 10) | (1u << 7) | 0x46;
    memory[0] = to | from << 16; memory[7] = 0xe0280001;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][17] == 0xabcdef01 && c.r[0][30] == 0x1234);

    /* Firmware's low-bank L and cross-bank D move forms. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][0] = 7; c.r[1][3] = 0x11223344;
    memory[0] = 0xb5d62046; memory[7] = 0xe0200001;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == 7 && c.r[0][13] == 0x11223344);

    /* SPRUFE8B Figures C-18/C-19: compact word-address arithmetic uses
     * fixed B15 and an unsigned five-bit constant scaled by four. Dx5's
     * destination observes header RS; Dx5p is the fixed D2/B15 form. */
    static const struct {
        unsigned constant, side, rs, dst;
    } compact_dx5_cases[] = {
        {0, 0, 0, 0}, {1, 1, 0, 7}, {4, 0, 0, 4},
        {17, 1, 1, 18}, {31, 0, 1, 23},
    };
    for (unsigned j = 0; j < sizeof(compact_dx5_cases) /
                                    sizeof(compact_dx5_cases[0]); ++j) {
        unsigned constant = compact_dx5_cases[j].constant;
        unsigned side = compact_dx5_cases[j].side;
        unsigned rs = compact_dx5_cases[j].rs;
        unsigned dst = compact_dx5_cases[j].dst;
        uint32_t word = 0x0436 | (constant & 7) << 13 |
                        (constant >> 3) << 11 | (dst & 7) << 7 | side;
        if (constant == 4 && side == 0 && !rs && dst == 4)
            assert(word == 0x8636); /* firmware ADDAW .D1X B15,4,A4 */
        cdj_c674x_reset(&c, 0x1000);
        c.r[1][15] = 0xfffffff0;
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000,
                              .header = rs << 19, .word = word}}};
        assert(cdj_c674x_execute(&c, &p, read_word, NULL, NULL));
        assert(c.r[side][dst] == 0xfffffff0u + constant * 4 && c.cycles == 1);
    }
    static const struct {
        unsigned constant;
        bool subtract;
    } compact_dx5p_cases[] = {
        {0, false}, {1, false}, {6, true}, {19, true}, {31, false},
    };
    for (unsigned j = 0; j < sizeof(compact_dx5p_cases) /
                                    sizeof(compact_dx5p_cases[0]); ++j) {
        unsigned constant = compact_dx5p_cases[j].constant;
        bool subtract = compact_dx5p_cases[j].subtract;
        uint32_t word = 0x0c77 | (constant & 7) << 13 |
                        (constant >> 3) << 8 | (unsigned)subtract << 7;
        if (constant == 6 && subtract)
            assert(word == 0xccf7); /* firmware SUBAW .D2 B15,6,B15 */
        cdj_c674x_reset(&c, 0x1000);
        c.r[1][15] = 0x1000;
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000,
                              .header = 1u << 19, .word = word}}};
        assert(cdj_c674x_execute(&c, &p, read_word, NULL, NULL));
        assert(c.r[1][15] == (subtract ? 0x1000u - constant * 4
                                      : 0x1000u + constant * 4));
        assert(c.cycles == 1);
    }
    /* C-19 fixes s=1. The otherwise matching s=0 encoding is reserved, for
     * both values of op: Figure C-19 (printed page 730) draws bit 0 as a
     * literal "1" with "s=1" written beneath it, where Figure C-18 above it
     * draws an unconstrained "s", and the note is "src2 = dst = B15".  The
     * ADDAW description (printed page 123) gives the reason: "s = 1 indicates
     * the unit is D2 and dst is in the B register file", so a B15 destination
     * forces s = 1, and Dx5p carries no x bit to cross with.  cl6x -mv6740
     * refuses "ADDAW .D1 B15,4,B15" (E0800, functional unit specifier
     * disagrees with operation) and "SUBAW .D1 B15,4,B15" (E0800, unit side
     * does not match side needed) while assembling both on .D2.  GNU
     * libopcodes disassembles 0x0c76/0x0cf6 as "addaw/subaw .D1X b15,0,b15"
     * all the same - a cross-path WRITE - which is why the compact sweep once
     * counted these 64 words as a decode gap.  They are not one, and nothing
     * here may be narrowed to admit them. */
    for (unsigned j = 0; j < 2; ++j) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[1][15] = 0x2000;
        CdjC674xPacket reserved_dx5p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000,
                              .word = 0x0c76 | j << 7}}};
        assert(!cdj_c674x_execute(&c, &reserved_dx5p, read_word, NULL, NULL));
        assert(c.cycles == 0 && c.r[1][15] == 0x2000 && c.r[0][15] == 0);
    }

    /* Figures C-8 through C-15 share the compact .D transfer layout.
     * Exercise every DSZ scalar interpretation plus aligned/nonaligned
     * doublewords across immediate, register, postincrement and predecrement
     * addressing. RS applies to data/register offsets but not A/B4-7 ptrs. */
    struct CompactMemoryCase {
        unsigned dsz, sz, na, size;
        bool pair, sign_extend;
    } compact_memory_cases[] = {
        {0, 0, 0, 4, false, false},
        {0, 1, 0, 1, false, false}, {1, 1, 0, 1, false, true},
        {2, 1, 0, 2, false, false}, {3, 1, 0, 2, false, true},
        {4, 1, 0, 4, false, false}, {5, 1, 0, 1, false, true},
        {6, 1, 0, 4, false, false}, {7, 1, 0, 2, false, true},
        {4, 0, 0, 8, true, false}, {4, 0, 1, 8, true, false},
    };
    for (unsigned family = 0; family < 4; ++family)
    for (unsigned load = 0; load < 2; ++load)
    for (unsigned j = 0; j < sizeof(compact_memory_cases) /
                                    sizeof(compact_memory_cases[0]); ++j) {
        struct CompactMemoryCase tc = compact_memory_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned reg_field = tc.pair ? 2 : 3;
        unsigned reg = reg_field + 16; /* header RS=1 */
        unsigned scale = tc.size;
        if (tc.pair && tc.na && family < 2) scale = 1; /* C-9/C-11 */
        uint32_t target = tc.na ? 0x10c1 : 0x10c0;
        uint32_t base = family == 3 ? target + 2 * scale : target;
        uint32_t address = family < 2 ? base + 2 * scale : target;
        c.r[0][5] = base; c.r[0][18] = 2;
        c.r[1][reg] = 0x12345678;
        if (tc.pair) c.r[1][reg + 1] = 0x9abcdef0;
        unsigned fixed = family == 0 ? 0x0004 : family == 1 ? 0x0404 :
                         family == 2 ? 0x0c04 : 0x4c04;
        unsigned offset_field = family < 2 ? 2 : 1; /* offset 2 / ucst0=1 */
        uint32_t word = fixed | offset_field << 13 | 1u << 12 |
                        tc.sz << 9 | 1u << 7 | reg_field << 4 |
                        tc.na << 4 | load << 3;
        memory[0] = word | 0x0c6e0000;
        memory[7] = 0xe0200000 | tc.dsz << 16 | 1u << 19;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][5] == (family < 2 ? base :
                             family == 2 ? base + 2 * scale : target));
        if (load) {
            assert(c.load_count == 1 && !c.store_count);
            assert(c.loads[0].address == address && c.loads[0].size == tc.size);
            assert(c.loads[0].bank == 1 && c.loads[0].dst == reg);
            assert(c.loads[0].sign_extend == tc.sign_extend);
        } else {
            assert(c.store_count == 1 && !c.load_count);
            assert(c.stores[0].address == address && c.stores[0].size == tc.size);
            assert(c.stores[0].value == (tc.pair ?
                UINT64_C(0x9abcdef012345678) : UINT64_C(0x12345678)));
        }
    }

    /* The genuine next word is STDW .D2 B5:B4,*B6[2]++.  It updates B6
     * in E1, captures both old source registers and commits the pair in E3. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0x11223344; c.r[1][5] = 0xaabbccdd; c.r[1][6] = 0x10c0;
    memory[0] = 0x0c6e3d45; memory[7] = 0xe0240000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][6] == 0x10d0 && c.store_count == 1 && memory[48] == 0);
    c.r[1][4] = c.r[1][5] = 0;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[48] == 0x11223344 && memory[49] == 0xaabbccdd &&
           !c.store_count);

    /* A later packet failure rolls compact base updates and queues back.
     * fff7h, not ffffh: see the M3 note above. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][6] = 0x10c0; c.r[1][4] = 0x12345678;
    memory[0] = 0xfff73d45; memory[7] = 0xe0240001;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][6] == 0x10c0 && !c.store_count && !c.cycles);
    /* LDW postincrement updates its pointer in E1, samples RAM in E3,
     * and makes its destination visible only after E5. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0; c.r[1][2] = 99;
    memory[0] = (2u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x66;
    memory[48] = 123;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][10] == 0x10c4 && c.r[1][2] == 99 && c.load_count == 1);
    memory[48] = 456;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    memory[48] = 789; /* E3 already captured 456 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][2] == 99);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][2] == 456 && !c.load_count && c.cycles == 5);

    /* PROT applies to full-width loads in a header-bearing fetch packet. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][10] = 0x10c8; c.r[1][4] = 2;
    memory[0] = (3u << 23) | (10u << 18) | (4u << 13) | (12u << 9) | 0xe4;
    memory[7] = 0xe0100000; memory[48] = 0x12345678;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][10] == 0x10c0 && c.r[0][3] == 0x12345678 && c.cycles == 5);

    /* False loads cannot touch an unmapped address or modify a pointer. */
    cdj_c674x_reset(&c, 0x1000);
    memory[0] |= 6u << 29; memory[7] = 0;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.load_count && c.r[1][10] == 0);

    /* A parallel decode failure must roll back pointer and load queue. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0;
    memory[0] = (10u << 18) | (1u << 13) | (11u << 9) | 0x65;
    memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][10] == 0x10c0 && !c.load_count && !c.cycles);
    /* An E1 write must not silently overwrite a simultaneous E5 result. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0;
    memory[0] = (2u << 23) | (10u << 18) | 0x264;
    memory[4] = mvk(0, 2, 99);
    for (unsigned j = 0; j < 4; ++j)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 4 && c.load_count == 1 && c.r[0][2] == 0);

    /* A load and store accessing the same address in E3 are unsupported,
     * rather than silently assuming a RAM arbitration order. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = c.r[0][10] = 0x10c0;
    memory[0] = (2u << 23) | (10u << 18) | 0x265;
    memory[1] = 0x3577; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && !c.store_count && !c.cycles && c.r[1][15] == 0x10c0);
    /* Compact BNOP uses halfword displacement and waits on false predicates. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = (5u << 13) | (17u << 6) | 0x2a; /* [A0], +34 bytes */
    memory[7] = 0xe0208000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1002 && c.cycles == 6 && !c.branch_due);
    cdj_c674x_reset(&c, 0x1000); c.r[0][0] = 1;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1022 && c.cycles == 6 && !c.branch_due);

    /* Negative signed offset, B0 inverted predicate and five NOPs. */
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = (5u << 13) | (127u << 6) | 0x3b;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0xffe && c.cycles == 6);

    /* Unsigned 8-bit offset must not sign extend values above 127. */
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0xc00a | (200u << 6);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1190 && c.cycles == 6);

    /* Full-width ADD wraps at 32 bits and samples parallel operands. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0xffffffff; c.r[0][1] = 2;
    memory[0] = (2u << 23) | (4u << 18) | (1u << 13) | 0x1059;
    memory[1] = (3u << 23) | (4u << 18) | (1u << 13) | 0x1078;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][2] == 0 && c.r[0][3] == 1);
    memory[2] = (4u << 23) | (31u << 13) | 0x58;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 0xffffffff);
    /* Little-endian byte lanes and signed/unsigned extension. PROT waits
     * for E5, while postincrement scales by the loaded element size. */
    for (unsigned lane = 0; lane < 4; ++lane) {
        for (unsigned sign = 0; sign < 2; ++sign) {
            memset(memory, 0, sizeof(memory));
            cdj_c674x_reset(&c, 0x1000); c.r[1][10] = 0x10c0 + lane;
            memory[48] = 0xff807f01;
            memory[0] = (2u << 23) | (10u << 18) | (1u << 13) |
                        (11u << 9) | (sign ? 0xa4 : 0x94);
            memory[7] = 0xe0100000;
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            const uint32_t unsigned_values[] = {1, 127, 128, 255};
            const uint32_t signed_values[] = {1, 127, 0xffffff80, 0xffffffff};
            assert(c.r[0][2] == (sign ? signed_values[lane] : unsigned_values[lane]));
            assert(c.r[1][10] == 0x10c1 + lane && c.cycles == 5);
        }
    }
    for (unsigned sign = 0; sign < 2; ++sign) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0;
        memory[48] = 0x8001ffff;
        memory[0] = (3u << 23) | (10u << 18) | (1u << 13) |
                    (9u << 9) | (sign ? 0x46 : 0x06);
        memory[7] = 0xe0100000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[1][3] == (sign ? 0xffff8001u : 0x8001u));
        assert(c.r[0][10] == 0x10c2);
    }
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c1;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.load_count && c.r[0][10] == 0x10c1);

    /* OR's immediate-zero form is the firmware's full-width move alias. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0x87654321;
    memory[0] = 0x04901fd8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][9] == 0x87654321);
    memory[1] = (2u << 23) | (31u << 13) | 0xfd8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][2] == 0xffffffff);
    /* Register shifts mask to six bits; counts >=32 must not invoke C UB. */
    const unsigned counts[] = {0, 1, 31, 32, 40, 63, 64};
    const uint32_t right[] = {0x80000001, 0x40000000, 1, 0, 0, 0, 0x80000001};
    const uint32_t arithmetic[] = {0x80000001, 0xc0000000, 0xffffffff, 0xffffffff,
                                  0xffffffff, 0xffffffff, 0x80000001};
    const uint32_t left[] = {0x80000001, 2, 0x80000000, 0, 0, 0, 0x80000001};
    for (unsigned j = 0; j < 7; ++j) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][1] = counts[j]; c.r[1][2] = 0x80000001;
        memory[0] = (3u << 23) | (2u << 18) | (1u << 13) | 0x19e0;
        memory[1] = (4u << 23) | (2u << 18) | (1u << 13) | 0x1de0;
        memory[2] = (5u << 23) | (2u << 18) | (1u << 13) | 0x1ce0;
        for (unsigned k = 0; k < 3; ++k) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][3] == right[j] && c.r[0][4] == arithmetic[j] && c.r[0][5] == left[j]);
    }
    /* STB/STH/STW preserve neighboring bytes and scale pointer increments. */
    for (unsigned size = 1; size <= 4; size *= 2) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][3] = 0x12345678; c.r[1][10] = 0x10c0 + size;
        memory[48] = memory[49] = 0xaabbccdd;
        unsigned op = size == 1 ? 0x34 : size == 2 ? 0x54 : 0x74;
        memory[0] = (3u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x80 | op;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[1][10] == 0x10c0 + 2 * size && memory[48] == 0xaabbccdd);
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(memory[48] == (size == 1 ? 0xaabb78dd : size == 2 ? 0x5678ccdd : 0xaabbccdd));
        assert(memory[49] == (size == 4 ? 0x12345678 : 0xaabbccdd));
    }
    /* LDNW joins adjacent little-endian words at every possible alignment. */
    const uint32_t joined[] = {0x44332211, 0x55443322, 0x66554433, 0x77665544};
    for (unsigned lane = 0; lane < 4; ++lane) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][10] = 0x10c0 + lane;
        memory[48] = 0x44332211; memory[49] = 0x88776655;
        memory[0] = (2u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1b4;
        memory[7] = 0xe0100000;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][2] == joined[lane] && c.r[1][10] == 0x10c4 + lane && c.cycles == 5);
    }
    /* STNW crosses a word boundary without changing surrounding bytes. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x12345678; c.r[1][10] = 0x10c3;
    memory[48] = 0xaabbccdd; memory[49] = 0xeeff0011;
    memory[0] = (3u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1d4;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][10] == 0x10c7 && memory[48] == 0xaabbccdd);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[48] == 0x78bbccdd && memory[49] == 0xee123456);

    /* A missing second word is detected before modifying the pointer. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][10] = 0x10fd;
    memory[0] = (2u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1b4;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && !c.cycles && c.r[1][10] == 0x10fd);

    /* Nonaligned access excludes all other memory operations in its packet. */
    cdj_c674x_reset(&c, 0x1000); c.r[1][10] = 0x10c1; c.r[0][10] = 0x10d0;
    memory[0] |= 1;
    memory[1] = (3u << 23) | (10u << 18) | 0x264;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && c.r[1][10] == 0x10c1 && !c.cycles);
    /* LDNDW spans three bus words; bit 23 scales the offset, not the
     * destination pair. Both halves appear together in E5. */
    for (unsigned scaled = 0; scaled < 2; ++scaled) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][10] = 0x10c1;
        memory[48] = 0x44332211; memory[49] = 0x88776655; memory[50] = 0xccbbaa99;
        memory[0] = (6u << 23) | (scaled << 23) | (10u << 18) |
                    (1u << 13) | (11u << 9) | 0x1a4;
        memory[7] = 0xe0100000;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][6] == 0x55443322 && c.r[0][7] == 0x99887766);
        assert(c.r[1][10] == 0x10c1 + (scaled ? 8 : 1) && c.cycles == 5);
    }
    /* STNDW samples the pair and preserves bytes on either side. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][6] = 0x44332211; c.r[0][7] = 0x88776655; c.r[1][10] = 0x10c3;
    memory[48] = memory[49] = memory[50] = 0xaaaaaaaa;
    memory[0] = (6u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1f4;
    for (unsigned j = 0; j < 3; ++j) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][10] == 0x10c4 && memory[48] == 0x11aaaaaa);
    assert(memory[49] == 0x55443322 && memory[50] == 0xaa887766);

    /* Aligned LDDW scales by eight and rejects odd register-pair indices. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][10] = 0x10b8; memory[48] = 0x12345678; memory[49] = 0x90abcdef;
    memory[0] = (6u << 23) | (10u << 18) | (1u << 13) | (9u << 9) | 0x1e4;
    memory[7] = 0xe0100000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][10] == 0x10c0 && c.r[0][6] == 0x12345678 && c.r[0][7] == 0x90abcdef);
    cdj_c674x_reset(&c, 0x1000); memory[0] |= 1u << 23;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && !c.cycles);

    /* The high half participates in write-hazard checks. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][10] = 0x10c0;
    memory[0] = (6u << 23) | (10u << 18) | 0x3e4;
    memory[4] = mvk(0, 7, 9);
    for (unsigned j = 0; j < 4; ++j) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 4 && c.load_count == 1 && c.r[0][7] == 0);
    /* Firmware decrement alias: ADD.S -1, B0, B0. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 1; memory[0] = 0x2003e1a3; memory[1] = 0;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][0] == 0);
    memory[2] = (2u << 23) | (31u << 13) | 0x42; /* MVK.D2 -1,B2 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][2] == 0xffffffff);

    /* SUB uses src1-src2, including immediate-first and cross-path forms. */
    const unsigned subops[] = {0xd8, 0x5a0, 0xf8, 0x5e0};
    for (unsigned j = 0; j < 4; ++j) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][1] = 0x80000000; c.r[1][2] = 1;
        memory[0] = (3u << 23) | (2u << 18) | ((j < 2 ? 31u : 1u) << 13) | 0x1000 | subops[j];
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][3] == (j < 2 ? 0xfffffffe : 0x7fffffff));
    }
    /* Reverse-cross .L SUB plus the complete signed/unsigned 40-bit
     * ADD/ADDU/SUB/SUBU family.  Long values use low register bits 31:0
     * and only bits 7:0 of the following register. */
    for (unsigned side = 0; side < 2; ++side) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side ^ 1][3] = 2; c.r[side][5] = 7;
        memory[0] = 6u << 23 | 5u << 18 | 3u << 13 |
                    1u << 12 | 0x17u << 5 | 0x18 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][6] == 0xfffffffbu);
    }
    struct LongArithmeticCase {
        unsigned op, a, b;
        uint32_t a_value, b_low, b_high;
        uint64_t expected;
        bool pair_source;
    } long_arithmetic_cases[] = {
        {0x20, 31, 4, 0,          0xfffffffe, 0xff, UINT64_C(0xfffffffffd), true},
        {0x21, 3,  4, 0xfffffffd, 0xfffffffe, 0xff, UINT64_C(0xfffffffffb), true},
        {0x23, 3,  5, 0xfffffffd, 5,          0,    UINT64_C(0x0000000002), false},
        {0x24, 31, 4, 0,          0xfffffffe, 0xff, UINT64_C(0x0000000001), true},
        {0x27, 3,  5, 0xfffffffd, 5,          0,    UINT64_C(0xfffffffff8), false},
        {0x29, 3,  4, 7,          0xfffffffc, 0xff, UINT64_C(0x0000000003), true},
        {0x2b, 3,  5, 0xffffffff, 2,          0,    UINT64_C(0x0100000001), false},
        {0x2f, 3,  5, 1,          2,          0,    UINT64_C(0xffffffffff), false},
        {0x37, 3,  5, 0xfffffffd, 5,          0,    UINT64_C(0xfffffffff8), false},
        {0x3f, 3,  5, 1,          2,          0,    UINT64_C(0xffffffffff), false},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(long_arithmetic_cases) /
                              sizeof(long_arithmetic_cases[0]); ++j) {
        struct LongArithmeticCase tc = long_arithmetic_cases[j];
        if ((tc.op == 0x20 || tc.op == 0x24) && cross_path) continue;
        unsigned cross_bank = side ^ cross_path;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        if (tc.op == 0x20 || tc.op == 0x24) {
            c.r[side][tc.b] = tc.b_low;
            c.r[side][tc.b + 1] = tc.b_high | 0xdeadbe00;
        } else if (tc.op == 0x21 || tc.op == 0x29) {
            c.r[cross_bank][tc.a] = tc.a_value;
            c.r[side][tc.b] = tc.b_low;
            c.r[side][tc.b + 1] = tc.b_high | 0xdeadbe00;
        } else if (tc.op == 0x37 || tc.op == 0x3f) {
            c.r[cross_bank][tc.a] = tc.a_value;
            c.r[side][tc.b] = tc.b_low;
        } else {
            c.r[side][tc.a] = tc.a_value;
            c.r[cross_bank][tc.b] = tc.b_low;
        }
        c.r[side][6] = c.r[side][7] = 0xdeadbeef;
        memory[0] = 6u << 23 | tc.b << 18 | tc.a << 13 |
                    cross_path << 12 | tc.op << 5 | 0x18 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][6] == (uint32_t)tc.expected);
        assert(c.r[side][7] == (uint32_t)(tc.expected >> 32));
        assert(c.cycles == 1 && !c.load_count);
    }
    /* Invalid source/destination pairs and parallel writes fail atomically;
     * a false predicate leaves both destination registers unchanged. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 7u << 23 | 5u << 18 | 3u << 13 | 0x2bu << 5 | 0x18;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.r[0][7] == 0);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 6u << 23 | 5u << 18 | 3u << 13 | 0x21u << 5 | 0x18;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.r[0][6] == 0);
    /* ADD/SUB immediate-to-long have no x-path form.  A set x bit must
     * reject atomically rather than being ignored while the local pair is
     * consumed. */
    for (unsigned op = 0x20; op <= 0x24; op += 4) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = 7; c.r[0][5] = 0;
        c.r[0][6] = c.r[0][7] = 99;
        memory[0] = 6u << 23 | 4u << 18 | 1u << 13 |
                    1u << 12 | op << 5 | 0x18;
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(!c.cycles && c.r[0][6] == 99 && c.r[0][7] == 99);
    }
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][6] = c.r[0][7] = 99;
    memory[0] = 2u << 29 | 6u << 23 | 5u << 18 | 3u << 13 |
                0x2bu << 5 | 0x18;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][6] == 99 && c.r[0][7] == 99);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 1; c.r[0][5] = 2;
    memory[0] = 6u << 23 | 5u << 18 | 3u << 13 |
                0x2bu << 5 | 0x19;
    memory[1] = mvk(0, 7, 4);
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.r[0][6] == 0 && c.r[0][7] == 0);
    /* ADDK .S1/.S2 adds a signed cst16 to its destination in place. Cover
     * both register files, signed boundaries, modular wrap, and the exact
     * firmware blocker ADDK .S2 24,B15 at 0xc003b4d8. */
    struct AddkCase { int32_t constant; uint32_t initial; } addk_cases[] = {
        {-32768, 0}, {-1, 0}, {0, 0x89abcdef},
        {1, UINT32_MAX}, {32767, 0xffff8001},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned j = 0; j < sizeof(addk_cases) / sizeof(addk_cases[0]); ++j) {
        unsigned dst = j & 1 ? 31 : 15;
        uint32_t word = dst << 23 |
                        ((uint32_t)addk_cases[j].constant & 0xffff) << 7 |
                        0x50 | side << 1;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][dst] = addk_cases[j].initial;
        memory[0] = word;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == addk_cases[j].initial +
                                  (uint32_t)addk_cases[j].constant);
        assert(c.cycles == 1);
    }
    assert((15u << 23 | 24u << 7 | 0x50 | 1u << 1) == 0x07800c52);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 100; memory[0] = 0x07800c52;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 124 && c.cycles == 1);

    /* A false predicate performs no write and does not conflict with a
     * parallel ADDK. When it becomes true, the duplicate write rejects the
     * complete packet without committing the earlier result. */
    cdj_c674x_reset(&c, 0x1000); c.r[0][5] = 10;
    CdjC674xPacket addk_parallel = {.count = 2, .next_pc = 0x1008,
        .instructions = {
            {.pc = 0x1000, .word = 2u << 29 | 5u << 23 | 1u << 7 | 0x51},
            {.pc = 0x1004, .word = 5u << 23 | 2u << 7 | 0x50},
        }};
    assert(cdj_c674x_execute(&c, &addk_parallel, read_word, NULL, NULL));
    assert(c.r[0][5] == 12 && c.cycles == 1);
    cdj_c674x_reset(&c, 0x1000); c.r[0][5] = 10; c.r[1][1] = 1;
    assert(!cdj_c674x_execute(&c, &addk_parallel, read_word, NULL, NULL));
    assert(c.r[0][5] == 10 && !c.cycles);

    /* The compact Sx5 sibling uses an unsigned cst5 and the header-selected
     * low/high register subset on both S sides. */
    const unsigned compact_addk_constants[] = {0, 1, 31};
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned j = 0; j < sizeof(compact_addk_constants) /
                              sizeof(compact_addk_constants[0]); ++j) {
        unsigned constant = compact_addk_constants[j];
        unsigned dst = 4 + subset * 16;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][dst] = UINT32_MAX - constant;
        memory[0] = (constant & 7) << 13 | ((constant >> 3) & 3) << 11 |
                    4u << 7 | 0x042e | side;
        memory[7] = 0xe0200000 | subset << 19;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == UINT32_MAX && c.cycles == 1);
    }
    /* ANDN has the same src1 & ~src2 behavior on all three functional-unit
     * encodings, both sides and both cross-path selections. */
    const unsigned andn_encodings[] = {0xf98, 0xdb0, 0x830};
    for (unsigned unit = 0; unit < 3; ++unit)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][3] = 0x195721ab;
        c.r[side ^ cross_path][5] = 0x081c17e6;
        memory[0] = 6u << 23 | 5u << 18 | 3u << 13 |
                    cross_path << 12 | andn_encodings[unit] | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][6] == 0x11432009 && c.cycles == 1);
        cdj_c674x_reset(&c, 0x1000); c.r[side][6] = 99;
        memory[0] |= 2u << 29; /* false B0 predicate */
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][6] == 99);
    }
    /* Figure G-4 compact LSDx1 family: zero/one, negate/decrement,
     * increment and XOR-one, including RS-selected registers. */
    const unsigned lsdx1_ops[] = {0, 1, 2, 3, 5, 7};
    for (unsigned unit = 0; unit < 3; ++unit)
    for (unsigned op_index = 0; op_index < sizeof(lsdx1_ops) /
                                           sizeof(lsdx1_ops[0]); ++op_index)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset) {
        unsigned op = lsdx1_ops[op_index];
        if (op == 2 && unit == 2) continue;
        unsigned reg = 5 + subset * 16;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][reg] = 0x80000000;
        memory[0] = op << 13 | 5u << 7 | unit << 3 | 0x1866 | side;
        memory[7] = 0xe0200000 | subset << 19;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        uint32_t expected = op == 0 ? 0 : op == 1 ? 1 :
                            op == 2 ? 0x80000000 :
                            op == 3 ? 0x7fffffff :
                            op == 5 ? 0x80000001 : 0x80000001;
        assert(c.r[side][reg] == expected && c.cycles == 1);
    }
    const unsigned reserved_lsdx1[] = {
        4u << 13 | 0x1866,                 /* op 4 on .L */
        2u << 13 | 2u << 3 | 0x1866,      /* op 2 on .D */
    };
    for (unsigned j = 0; j < sizeof(reserved_lsdx1) /
                              sizeof(reserved_lsdx1[0]); ++j) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        memory[0] = reserved_lsdx1[j]; memory[7] = 0xe0200000;
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(!c.cycles);
    }
    /* Figure G-4's unit field (printed page 761, bits 4-3) has no 11b
     * encoding, and 11b there puts 1111b in bits 4-1, which is exactly the
     * Figure E-5 M3 signature (printed page 744).  So what used to look like a
     * reserved LSDx1 unit is really a compact multiply, and TI's disassembler
     * agrees: 187eh inside a .fphead-framed packet is "MPYHL.M1X A0,B0,A4".
     * The reserved-unit guard in the core is therefore unreachable, not gone.
     * A0 x B0 is 0 x 0, so the product is 0 and the register is already 0;
     * what this asserts is that the packet is executed rather than rejected. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0u << 13 | 3u << 3 | 0x1866; memory[7] = 0xe0200000;
    c.r[0][4] = 0xdeadbeef;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.load_count == 1 && c.r[0][4] == 0xdeadbeefu);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 0);
    /* The genuine blocker is MVK .D2 0,B5 in a mixed fetch packet. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1010);
    c.r[1][5] = 99; memory[4] = 0x1af7; memory[7] = 0xe2000200;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][5] == 0);
    /* Compact register ADD/SUB on .S and in-place .D complete the
     * non-saturating arithmetic batch across both sides, RS subsets and
     * cross paths. Saturating variants share the full-width semantic path. */
    for (unsigned unit = 0; unit < 2; ++unit)
    for (unsigned subtract = 0; subtract < 2; ++subtract)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path) {
        unsigned dst = 5 + subset * 16;
        unsigned left = unit ? dst : 7 + subset * 16;
        unsigned right = 6 + subset * 16;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][left] = 0x80000001;
        c.r[side ^ cross_path][right] = 3;
        if (unit) memory[0] = 5u << 13 | cross_path << 12 |
                              subtract << 11 | 6u << 7 | 0x36 | side;
        else memory[0] = 7u << 13 | cross_path << 12 |
                         subtract << 11 | 6u << 7 | 5u << 4 | 0x0a | side;
        memory[7] = 0xe0200000 | subset << 19;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == (subtract ? 0x7ffffffe : 0x80000004));
    }
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][7] = 0x7fffffff;
    memory[0] = 7u << 13 | 7u << 7 | 5u << 4 | 0x0a;
    memory[7] = 0xe0204000; /* SAT makes the ADD a SADD. */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.r[0][5] == 0x7fffffff);
    assert(!(c.control[1] & 0x200) && c.load_count == 1);
    /* SUB ignores the header SAT selector and retains modular arithmetic. */
    cdj_c674x_reset(&c, 0x1000); c.r[0][7] = 0x7fffffff;
    memory[0] |= 1u << 11;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][5] == 0);
    /* Compact .S shift formats: translated immediate counts, all in-place
     * constant counts, and register counts through the six-bit boundary. */
    const uint32_t compact_shift_source = 0x87654321;
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 2; ++op)
    for (unsigned encoded = 0; encoded < 8; ++encoded) {
        unsigned src = 4 + subset * 16, dst = 5 + subset * 16;
        unsigned count = encoded == 0 ? 16 : encoded == 7 ? 8 : encoded;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side ^ 1][src] = compact_shift_source;
        memory[0] = encoded << 13 | 1u << 12 | op << 11 |
                    4u << 7 | 5u << 4 | 0x40a | side;
        memory[7] = 0xe0200000 | subset << 19;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        uint32_t expected = op == 0 ? compact_shift_source << count :
                            (compact_shift_source >> count) |
                            (UINT32_MAX << (32 - count));
        assert(c.r[side][dst] == expected);
    }
    const unsigned compact_shift_counts[] = {0, 1, 8, 16, 31, 32, 63};
    for (unsigned form = 0; form < 2; ++form)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 3; ++op)
    for (unsigned j = 0; j < sizeof(compact_shift_counts) /
                              sizeof(compact_shift_counts[0]); ++j) {
        unsigned count = compact_shift_counts[j];
        if (!form && count >= 32) continue;
        unsigned srcdst = 4 + subset * 16;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][srcdst] = compact_shift_source;
        if (form) {
            c.r[side][5 + subset * 16] = count;
            memory[0] = 5u << 13 | op << 11 | 4u << 7 | 0x462 | side;
        } else {
            memory[0] = (count & 7) << 13 | ((count >> 3) & 3) << 11 |
                        4u << 7 | op << 5 | 0x402 | side;
        }
        memory[7] = 0xe0200000 | subset << 19;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        uint32_t expected;
        if (op == 0) expected = count >= 32 ? 0 : compact_shift_source << count;
        else if (op == 2) expected = count >= 32 ? 0 : compact_shift_source >> count;
        else expected = count >= 32 ? UINT32_MAX : count == 0 ? compact_shift_source :
                        (compact_shift_source >> count) | (UINT32_MAX << (32 - count));
        assert(c.r[side][srcdst] == expected);
    }
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 1; memory[0] = 4u << 7 | 2u << 5 | 0x402;
    memory[7] = 0xe0204000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.r[0][4] == 1 && !c.load_count);
    /* Signed comparison at both extremes, and a negative immediate. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x7fffffff; c.r[1][2] = 0x80000000;
    memory[0] = (3u << 23) | (2u << 18) | (1u << 13) | 0x18f8;
    memory[1] = (4u << 23) | (2u << 18) | (31u << 13) | 0x18d8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 1 && c.r[0][4] == 1);
    c.r[1][2] = 0;
    memory[2] = memory[1];
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 0);
    /* Compact MVC reads B0 (or B16 with RS), with four-cycle availability
     * for the loop engine. Parallel writes still see pre-packet registers. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 7; memory[0] = mvk(1, 0, 99) | 1;
    memory[1] = 0xd86f; memory[7] = 0xe0400000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 7 && c.control_ready[13] == 4 && c.r[1][0] == 99);
    cdj_c674x_reset(&c, 0x1004); c.r[1][16] = 123;
    memory[7] |= 1u << 19;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 123 && c.control_ready[13] == 4);

    /* Full-width MVC to ILC/RILC preserves predicates and source cross path. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][2] = 17;
    memory[0] = (13u << 23) | (2u << 18) | 0x13a2;
    memory[1] = (14u << 23) | (2u << 18) | 0x13a2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 17 && c.control[14] == 17);
    assert(c.control_ready[13] == 4 && c.control_ready[14] == 5);

    /* Two writes to ILC in one packet cannot silently choose a winner. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (13u << 23) | 0x3a3;
    memory[1] = 0xd86f; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && !c.control_ready[13]);

    /* C674x interrupt/control MVC family. Reset exposes the architectural
     * CPU/endian identity, reset interrupt enable, and C6747 ROM IST base. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    assert(c.control[1] == 0x14000100 && c.control[4] == 1 &&
           c.control[5] == 0x00700000);

    /* DINT/RINT are unconditional no-unit operations, so 0x10004000 must
     * bypass the normal predicate decoder. Exercise the exact firmware DINT
     * and its nearby RINT || OR packet. CSR.PGIE remains unchanged while the
     * physically shared CSR/TSR GIE and TSR.SGIE bits change in E1. */
    c.control[1] |= 3; c.control[26] = 0xa4;
    memory[0] = 0x10004000; /* DINT at firmware PC 0xc003b49c. */
    memory[1] = 0x10006001; /* RINT || at firmware PC 0xc003b4c8. */
    memory[2] = 0x01901fd8; /* OR .L1X 0,B4,A3. */
    c.r[1][4] = 0xdeadbeef;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.control[1] == 0x14000102 &&
           c.control[26] == 0xa6);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 2 && c.control[1] == 0x14000103 &&
           c.control[26] == 0xa5 && c.r[0][3] == 0xdeadbeef);

    /* A documented DINT || MVC reg,CSR conflict rejects the whole packet
     * before either CSR or TSR state can change. */
    cdj_c674x_reset(&c, 0x1000); c.control[1] |= 3; c.control[26] = 0xa4;
    CdjC674xPacket gate_conflict = {.count = 2, .next_pc = 0x1008,
        .instructions = {{.pc = 0x1000, .word = 0x10004001},
                         {.pc = 0x1004, .word =
                             (1u << 23) | (3u << 18) |
                             (1u << 12) | 0x3a2}}};
    assert(!cdj_c674x_execute(&c, &gate_conflict, read_word, NULL, NULL));
    assert(!c.cycles && c.control[1] == 0x14000103 &&
           c.control[26] == 0xa4);

    /* IER maskable bits are RW, reset is fixed one, and NMIE is set-only. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0xfff2;
    memory[0] = 4u << 23 | 4u << 18 | 0x3a2; /* MVC B4,IER */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[4] == 0xfff3);
    c.r[1][4] = 0;
    memory[1] = 4u << 23 | 4u << 18 | 0x3a2;
    memory[2] = 5u << 23 | 4u << 18 | 0x3e2; /* MVC IER,B5 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[4] == 3);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][5] == 3);

    /* ISTP writes only its aligned base. HPEINT is the lowest-numbered
     * pending interrupt enabled in IER and is synthesized on each read. */
    c.r[1][4] = 0x008003ff;
    memory[3] = 5u << 23 | 4u << 18 | 0x3a2; /* MVC B4,ISTP */
    memory[4] = 6u << 23 | 5u << 18 | 0x3e2; /* MVC ISTP,B6 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[5] == 0x00800000);
    c.control[2] = (1u << 7) | (1u << 4);
    c.control[4] = (1u << 7) | (1u << 4) | 1;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][6] == 0x00800080);

    /* ISR and ICR have one delay slot: the following MVC IFR still sees the
     * old flags, while the next one sees the set or clear. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0x90;
    memory[0] = 2u << 23 | 4u << 18 | 0x3a2; /* MVC B4,ISR */
    memory[1] = 5u << 23 | 2u << 18 | 0x3e2; /* MVC IFR,B5 */
    memory[2] = 6u << 23 | 2u << 18 | 0x3e2; /* MVC IFR,B6 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.control[2] && c.load_count == 1 &&
           c.loads[0].due == 2 &&
           c.loads[0].size == CDJ_C674X_DELAYED_IFR_SET);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][5] == 0 && c.control[2] == 0x90 && !c.load_count);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][6] == 0x90);
    c.r[1][4] = 0x10;
    memory[3] = 3u << 23 | 4u << 18 | 0x3a2; /* MVC B4,ICR */
    memory[4] = 7u << 23 | 2u << 18 | 0x3e2; /* MVC IFR,B7 */
    memory[5] = 8u << 23 | 2u << 18 | 0x3e2; /* MVC IFR,B8 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[2] == 0x90 && c.load_count == 1 &&
           c.loads[0].size == CDJ_C674X_DELAYED_IFR_CLEAR);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][7] == 0x90 && c.control[2] == 0x80);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][8] == 0x80);

    /* A simultaneous interrupt set wins over clear. This also checks that
     * control-effect queue entries never collide with or write A0. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][0] = 0x12345678; c.control[2] = 0x10;
    c.loads[0] = (CdjC674xLoad){.due = 1, .address = 0x10,
                               .size = CDJ_C674X_DELAYED_IFR_CLEAR};
    c.loads[1] = (CdjC674xLoad){.due = 1, .address = 0x10,
                               .size = CDJ_C674X_DELAYED_IFR_SET};
    c.load_count = 2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[2] == 0x10 && c.r[0][0] == 0x12345678 && !c.load_count);

    /* The genuine reset code writes -4 to CSR: fixed CPU/endian fields and
     * SAT survive, ignored cache/power fields do not, and GIE/PGIE clear.
     * A later MVC with low bits set updates GIE/PGIE and clears SAT. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[1] |= 0x200; c.r[0][3] = 0xfffffffc;
    memory[0] = 1u << 23 | 3u << 18 | 1u << 12 | 0x3a2; /* MVC A3,CSR */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[1] == 0x14000300);
    c.r[0][3] = 3; memory[1] = memory[0];
    memory[2] = 4u << 23 | 1u << 18 | 0x3e2; /* MVC CSR,B4 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[1] == 0x14000103);
    assert((c.control[26] & 1) == 1);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][4] == 0x14000103);

    /* IRP/NRP are full-width storage. Write-only ICR and unsupported control
     * IDs remain fail-closed without advancing architectural time. */
    for (unsigned id = 6; id <= 7; ++id) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][4] = 0x12345678 + id;
        memory[0] = id << 23 | 4u << 18 | 0x3a2;
        memory[1] = 5u << 23 | id << 18 | 0x3e2;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.control[id] == 0x12345678 + id);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[1][5] == 0x12345678 + id);
    }
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 4u << 23 | 3u << 18 | 0x3e2; /* MVC ICR,B4 */
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.control[1] == 0x14000100);
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 8u << 23 | 4u << 18 | 0x3a2;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.control[8] == 0);

    /* ITSR is control register 27 (REP is 15). Its defined task-state fields
     * are writable in supervisor mode, with ITSR.GIE physically aliased to
     * CSR.PGIE. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = UINT32_MAX;
    memory[0] = 27u << 23 | 4u << 18 | 0x3a2; /* MVC B4,ITSR */
    memory[1] = 5u << 23 | 27u << 18 | 0x3e2; /* MVC ITSR,B5 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[27] == 0xc6df && (c.control[1] & 2));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][5] == 0xc6df);

    /* FADCR (18), FAUCR (19) and FMCR (20) reserve bits 31-27 and 15-11 in
     * every one of SPRUFE8B Tables 2-25 (printed page 59), 2-26 (printed
     * page 61) and 2-27 (printed page 63): "The reserved bit location is
     * always read as 0. A value written to this field has no effect."  Every
     * other bit is R/W by MVC in Figures 2-29/2-30/2-31, so writing all ones
     * and reading back must give 0x07ff07ff on all three.  The MVC encodings
     * are asm6x -mv6740 output: "MVC .S2 B4, FADCR" = 0x091003a2,
     * "MVC .S2 B4, FAUCR" = 0x099003a2, "MVC .S2 B4, FMCR" = 0x0a1003a2,
     * "MVC .S2 FADCR, B5" = 0x02c803e2, FAUCR = 0x02cc03e2,
     * FMCR = 0x02d003e2. */
    static const struct { uint32_t write, read; } fp_status[] = {
        {0x091003a2u, 0x02c803e2u}, {0x099003a2u, 0x02cc03e2u},
        {0x0a1003a2u, 0x02d003e2u},
    };
    for (unsigned i = 0; i < 3; ++i) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][4] = UINT32_MAX;
        memory[0] = fp_status[i].write;
        memory[1] = fp_status[i].read;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.control[18 + i] == 0x07ff07ffu);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[1][5] == 0x07ff07ffu);
        /* A reserved bit already set in stored state still reads back as 0. */
        c.control[18 + i] = UINT32_MAX;
        memory[2] = fp_status[i].read;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[1][5] == 0x07ff07ffu);
    }

    /* Maskable CPU interrupts enter only at an execute-packet boundary.
     * Requests latch in IFR while globally/individually masked and are
     * recognized after all three architectural enables become true. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.pc == 0x1000 && c.control[2] == (1u << 4));
    c.control[1] |= 1;                         /* GIE, but NMIE remains 0. */
    c.control[26] |= 1;
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x1000 && c.control[2] == (1u << 4));
    c.control[4] = (1u << 4) | 3u;            /* NMIE and IE4. */
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x1080 && c.control[6] == 0x1000 && !c.control[2]);
    assert((c.control[1] & 3) == 2 && (c.control[27] & 1) == 1);
    assert(c.control[26] == ((1u << 15) | (1u << 9)));
    assert(!c.cycles && !c.packets);

    /* A disabled higher-priority flag remains pending while an enabled lower
     * priority interrupt vectors; the selector operates on IFR intersect IER. */
    cdj_c674x_reset(&c, 0x1040); c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 7) | 3u;
    assert(cdj_c674x_interrupt(&c, (1u << 4) | (1u << 7)));
    assert(c.pc == 0x10e0 && c.control[6] == 0x1040);
    assert(c.control[2] == (1u << 4));

    /* INT4 is the highest maskable priority. Acceptance clears only its IFR
     * bit and saves the complete supported TSR context in ITSR. */
    cdj_c674x_reset(&c, 0x1040);
    c.control[5] = 0x1000;
    c.control[1] |= 1;
    c.control[4] = (1u << 7) | (1u << 4) | 3u;
    c.control[26] = (1u << 10) | (1u << 6) |
                    (1u << 4) | (1u << 3) | (1u << 2) | 3u;
    assert(cdj_c674x_interrupt(&c, (1u << 7) | (1u << 4)));
    assert(c.pc == 0x1080 && c.control[6] == 0x1040);
    assert(c.control[2] == (1u << 7));
    assert(c.control[27] == 0x45f);
    assert(c.control[26] == ((1u << 15) | (1u << 9) |
                             (1u << 4) | (1u << 2)));

    /* B IRP restores ITSR in E1, leaves PGIE set, and reaches the saved IRP
     * after five delay slots. The zero words are architectural NOP 1s. */
    memory[32] = 0x001800e2;                  /* B .S2 IRP at IST+0x80. */
    finish_interrupt_entry(&c);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 10 && c.pc == 0x1084 && c.branch_due == 15);
    assert(c.control[26] == 0x45f && (c.control[1] & 3) == 3);
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 15 && c.pc == 0x1040 && !c.branch_due);

    /* The sole idle SPLX state is an interrupt return.  B IRP preserves the
     * restored bit through redirect; a return SPLOOPD then has ordinary
     * SPLOOP counting and suppresses its parallel setup operation (7.13.2). */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    cdj_c674x_loop_set_functional_timing(true);
    c.control[27] = 1u << 14; c.control[6] = 0x1040; c.control[13] = 2;
    c.r[0][4] = 11;
    memory[0] = 0x001800e2;                  /* B .S2 IRP. */
    memory[16] = 0x0003a001;                 /* SPLOOPD 1 || */
    memory[17] = mvk(0, 4, 77);              /* suppressed on return */
    memory[18] = 0; memory[19] = 0x00034000; /* NOP; SPKERNEL 0,0 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.loop_active && (c.control[26] & (1u << 14)));
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && !c.loop_active &&
           (c.control[26] & (1u << 14)));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop_active && !c.loop.delayed_count &&
           c.loop.iterations == 2 && c.control[13] == 1 &&
           c.r[0][4] == 11 && (c.control[26] & (1u << 14)));
    unsigned return_steps = 0;
    while (c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(++return_steps < 20);
    }
    assert(!(c.control[26] & (1u << 14)) &&
           !(c.loop_pred_history & 8) && c.r[0][4] == 11);
    cdj_c674x_loop_set_functional_timing(false);

    /* Returned immediate BNOP is a timed NOP regardless of its predicate.
     * Exercise every full-width N count and both sides without allowing the
     * otherwise-taken branch to redirect or enter the retained loop tags. */
    cdj_c674x_loop_set_functional_timing(true);
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned pred = 0; pred < 2; ++pred)
    for (unsigned n = 0; n < 8; ++n) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[26] = 1u << 14; c.control[13] = 20;
        c.r[1][0] = pred;
        memory[0] = 0x38000; /* returned SPLOOP 1 */
        memory[1] = (1u << 29) | (16u << 16) | (n << 13) |
                    0x120 | (side << 1);
        memory[2] = 0x34000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        for (unsigned cycle = 0; cycle <= n; ++cycle) {
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(!c.branch_due && !c.loop_tags && c.pc == 0x1008);
        }
        assert(c.cycles == n + 2 && !c.loop.sealed);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.loop.sealed && !(c.loop_pred_history & 8));
    }
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned form = 0; form < 4; ++form) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[26] = 1u << 14; c.control[13] = 20;
        memory[0] = 0x38000;
        unsigned branch = (form & 1 ? 0xc000 : 3u << 13) |
                          (form & 2 ? 0x2a : 0x0a) | side;
        memory[1] = branch | (0x1c66u << 16);
        memory[7] = 0xe0408000; /* word 1 compact, BR header */
        unsigned cycles = form & 1 ? 6 : 4;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        for (unsigned cycle = 0; cycle < cycles; ++cycle) {
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(!c.branch_due && !c.loop_tags && c.pc == 0x1006);
        }
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.loop.sealed && !(c.loop_pred_history & 8));
    }
    cdj_c674x_loop_set_functional_timing(false);

    /* SPRUFE8B 7.13.2 (printed page 698) resumes an interrupted loop by
     * re-executing the SPLOOP(D/W) and its prolog out of program memory, and
     * 7.13.1 (printed page 697) lists the entire save/restore contract an ISR
     * owes - "the ITSR or NTSR, ILC, and RILC registers" - with no loop
     * buffer in it. So a return with no retained provenance at all (a legacy
     * or partial checkpoint, or an ISR that ran its own SPLOOP) must rebuild,
     * not fail. Expected values are read off 7.13.2's bullet list: the
     * SPLOOPD "executes as an SPLOOP instruction", so no four-cycle count
     * delay is added and the trip count is exactly ILC; and the rebuilt
     * buffer starts empty because nothing was retained. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[26] = 1u << 14; c.control[13] = 2;
    memory[0] = 0x0003a000;                 /* returned SPLOOPD 1 */
    memory[1] = 3u << 23 | 3u << 18 | 1u << 13 |
                0x12u << 7 | 0x40;          /* ADD .D1 A3,1,A3 */
    memory[2] = 0x00034000;                 /* SPKERNEL 0,0 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.fault && c.loop_active && !c.loop.delayed_count &&
           c.loop.iterations == 2 && !c.loop_tags);
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.fault && c.loop_tags == 1 && c.loop.length == 2 &&
           !(c.loop_pred_history & 8));

    /* SPMASK on both sides of an interrupt, in strict timing and in breadth.
     * Every expected value here comes from SPRUFE8B chapter 7, not from a
     * trace. Loading: 7.11.1/7.11.2 (printed pages 693-694) - the SPMASKed
     * ADD .D1 A4 "is executed only once and is not loaded to the SPLOOP
     * buffer", and on that one cycle the buffered .D1 operation does not
     * issue (7.15.1 resource conflict), so after three loading cycles the
     * buffered ADD .D1 A3 has run on cycles 0 and 2 only: A3 == 2, A4 == 7,
     * one buffered tag. Return: 7.13.2/7.11.5 (printed pages 698, 696) -
     * "SPMASKed instructions from program memory execute as a NOP" and
     * "SPMASKed instructions in the loop buffer execute as normal", so A3
     * advances on both pipe-up cycles while A4 never moves again. 7.13
     * (printed page 697) puts the SPLOOP execute-packet address in IRP. */
    for (unsigned breadth = 0; breadth < 2; ++breadth) {
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    cdj_c674x_loop_set_functional_timing(breadth != 0);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 7) | 3u;
    c.control[13] = 12;
    memory[0] = 0x00038000;                 /* SPLOOP 1 */
    memory[1] = 3u << 23 | 3u << 18 | 1u << 13 |
                0x12u << 7 | 0x40;          /* ADD .D1 A3,1,A3 */
    memory[2] = 0x00430001;                 /* SPMASK D1 || */
    memory[3] = 4u << 23 | 4u << 18 | 7u << 13 |
                0x12u << 7 | 0x40;          /* ADD .D1 A4,7,A4 */
    memory[4] = 0x00034000;
    memory[56] = 0x001800e2;                /* INT7 handler: B IRP */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 2 && c.r[0][4] == 7 && c.loop_tags == 1);
    assert(cdj_c674x_interrupt(&c, 1u << 7));
    while (c.loop_active)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x10e0 && c.control[6] == 0x1000);
    /* Strict timing inserts the fixed nine-cycle entry interval (5.5.1,
     * printed page 648) ahead of the handler, so wait for the return rather
     * than counting cycles: B IRP plus its five delay slots land on the
     * SPLOOP packet, which activates the loop again. */
    unsigned return_guard = 0;
    while (!c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(++return_guard < 40);
    }
    uint32_t before_return_a3 = c.r[0][3], before_return_a4 = c.r[0][4];
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == before_return_a3 + 1 &&
           c.r[0][4] == before_return_a4);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == before_return_a3 + 2 &&
           c.r[0][4] == before_return_a4);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop.sealed && !c.fault && !(c.loop_pred_history & 8));
    }
    cdj_c674x_loop_set_functional_timing(false);

    /* An interrupt service routine may use the loop buffer itself. SPRUFE8B
     * 7.13.1 (printed page 697) states the whole contract - "Interrupt
     * service routines must save and restore the ITSR or NTSR, ILC, and RILC
     * registers" - and the loop buffer is not in it, because 7.13.2 (printed
     * page 698) resumes by re-executing the SPLOOP(D/W) and its prolog from
     * program memory. So the handler's own software loop overwrites the buffer
     * and the interrupted loop still resumes, rebuilding from memory.
     * Runs in strict timing and in breadth. */
    for (unsigned breadth = 0; breadth < 2; ++breadth) {
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    cdj_c674x_loop_set_functional_timing(breadth != 0);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.control[13] = 12;
    memory[0] = 0x00038000;                 /* SPLOOP 1 */
    memory[1] = 3u << 23 | 3u << 18 | 1u << 13 |
                0x12u << 7 | 0x40;          /* ADD .D1 A3,1,A3 */
    memory[2] = 0x00034000;                 /* SPKERNEL 0,0 */
    /* The handler uses SPLOOPW, because 7.10 (printed page 690) says a
     * SPLOOPW "ILC and RILC are not accessed or modified" - so the handler
     * honours 7.13.2's requirement to leave the interrupted loop's ILC alone
     * without needing an explicit save/restore. B0 is zero, so by 7.10.1 and
     * 7.10.2 (printed page 691) the loop must execute at least one iteration,
     * the termination condition "is always false for the first 3 cycles of the
     * loop", and the cycle-4 stage boundary - which evaluates the condition
     * "3 cycles before the stage boundary", i.e. cycle 1 - terminates it. Four
     * loop cycles execute, so A5 == 4. Execution then resumes at the
     * instruction after the loop body (7.10.3), which is the return branch -
     * and the loop buffer is
     * already idle there, so 7.14's "taken branch idles the loop buffer" and
     * B IRP's restore of TSR from ITSR cannot contend for TSR.SPLX. */
    memory[32] = 0x2003e000;                /* INT4 handler: [B0] SPLOOPW 1 */
    memory[33] = 5u << 23 | 5u << 18 | 1u << 13 |
                 0x12u << 7 | 0x40;         /* ADD .D1 A5,1,A5 */
    memory[34] = 0x00034000;                /* SPKERNEL 0,0 */
    memory[35] = 0x001800e2;                /* B .S2 IRP */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (c.loop.cycle < 5)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    while (c.loop_active)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x1080 && c.control[6] == 0x1000 &&
           (c.control[27] & (1u << 14)));
    /* The handler's loop runs, overwriting the buffer, and B IRP returns. */
    unsigned nested_guard = 0;
    uint32_t interrupted_a3 = c.r[0][3];
    while (c.pc != 0x1000 || c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(++nested_guard < 200);
    }
    assert(!c.fault && c.r[0][5] == 4 && c.r[0][3] == interrupted_a3 &&
           (c.control[26] & (1u << 14)));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); /* returned setup */
    assert(c.loop_active && !c.loop.delayed_count && !c.loop_tags);
    uint32_t nested_a5 = c.r[0][5];
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == interrupted_a3 + 1 && c.r[0][5] == nested_a5);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.fault && c.loop.sealed && c.loop_tags == 1 &&
           !(c.loop_pred_history & 8));
    }
    cdj_c674x_loop_set_functional_timing(false);

    /* SPRUFE8B 7.7.3.3, printed page 679, verbatim: "The NOP cycles
     * associated with ADDKPC, BNOP, or protected LD instructions that are
     * masked, are always executed when resuming an interrupted SPLOOP(D)."
     * So on return the masked protected LDW is annulled - page 679's "SPMASKed
     * instructions from program memory execute like a NOP" - while its four
     * cycles of PROT expansion (printed page 93) still lengthen the loop.
     * Expected length is counted off the manual, not measured: loading cycle 0
     * is the SPMASK/LDW packet, cycles 1-4 are the PROT expansion, cycle 5 is
     * the ADD packet and cycle 6 the SPKERNEL, so dynlen is 7 against 3 for
     * the same program without PROT. Encodings from asm6x -mv6740, as recorded
     * at test_protected_loop_body_expands_once.
     * SPMASK D1 || -> 00430001   LDW .D1 *A4,A5 -> 02900264
     * ADD .L1 A5,A5,A6 -> 0314A078   SPKERNEL 0,0 -> 00034000 */
    for (unsigned prot = 0; prot < 2; ++prot) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[26] = 1u << 14;           /* returning: ITSR.SPLX was 1 */
        c.control[13] = 12;
        memory[0] = 0x00038000u;            /* returned SPLOOP 1 */
        memory[1] = 0x00430001u;            /* SPMASK D1 || */
        memory[2] = 0x02900264u;            /* LDW .D1 *A4,A5 */
        memory[3] = 0x0314A078u;            /* ADD .L1 A5,A5,A6 */
        memory[4] = 0x00034000u;            /* SPKERNEL 0,0 */
        memory[7] = 0xe0000000u | (prot ? (1u << 20) : 0u);
        memory[16] = 0x00000101;            /* 0x1040 */
        c.r[0][4] = 0x1040;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        unsigned loading = 0;
        while (!c.loop.sealed) {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++loading < 16);
        }
        assert(loading == (prot ? 7u : 3u) && c.loop.length == loading);
        /* The annulled load never reaches A5 and never enters the buffer. */
        assert(!c.r[0][5] && !c.load_count && c.loop_tags == 1);
    }

    /* In-flight results retire during entry, before handler instructions. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.loads[0] = (CdjC674xLoad){.due = 1, .value = 0xfeedface,
                                .bank = 0, .dst = 9, .size = 0};
    c.load_count = 1;
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.pc == 0x1080 && c.load_count == 1 && c.r[0][9] == 0);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][9] == 0xfeedface && !c.load_count);

    /* Figure 5-4 has nine empty E1 slots even with an empty pipeline. Older
     * results through the final slot retire, board clocks tick, and no ISR
     * instruction is fetched until the tenth step. Pending IRQs still latch
     * but cannot restart entry or replace IRP while GIE is cleared. */
    for (unsigned due = 0; due <= 9; ++due) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[5] = 0x1000;
        c.control[1] |= 1; c.control[4] = (1u << 4) | (1u << 7) | 3u;
        ticks = 0; c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
        if (due) {
            c.loads[0] = (CdjC674xLoad){.due = due, .value = 0xfeedface,
                .bank = 1, .dst = 0, .size = 0};
            c.load_count = 1;
        }
        memory[32] = 0x0008c06a;             /* MVKH .S2 0x1180,B0 */
        assert(cdj_c674x_interrupt(&c, 1u << 4));
        assert(c.idle_cycles == 9 && c.control[6] == 0x1000);
        for (unsigned i = 1; i <= 9; ++i) {
            assert(cdj_c674x_interrupt(&c, 1u << 7));
            assert(c.idle_cycles == 10 - i && c.control[6] == 0x1000);
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.pc == 0x1080 && c.cycles == i && ticks == i);
            assert(c.load_count == (due > i));
            assert(c.r[1][0] == (due && due <= i ? 0xfeedface : 0));
        }
        assert(c.control[2] == (1u << 7));
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 10 && c.pc == 0x1084 && ticks == 10);
        assert(c.r[1][0] == (due ? 0x1180face : 0x11800000));
    }

    /* Older stores also retire without issuing a handler packet. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.stores[0] = (CdjC674xStore){.due = 3, .address = 0x10c0,
        .value = 0x12345678, .size = 4};
    c.store_count = 1;
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    finish_interrupt_entry(&c);
    assert(!c.store_count && memory[48] == 0x12345678);

    /* Do not replace the fixed interval with "drain until safe": an invalid
     * later result colliding with ISR E1 must still fail closed. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.loads[0] = (CdjC674xLoad){.due = 10, .value = 0xfeedface,
        .bank = 1, .dst = 0, .size = 0};
    c.load_count = 1;
    memory[32] = 0x0008c06a;
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    finish_interrupt_entry(&c);
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!strcmp(c.fault, "delayed-result write conflict"));
    assert(c.cycles == 9 && c.pc == 0x1080 && c.load_count == 1 && !c.r[1][0]);

    /* Breadth mode preserves the same older writeback but inserts the
     * minimum empty interval before fetching an ISR instruction that writes
     * the same register. This is deliberately not an exact interrupt-latency
     * claim; strict mode instead uses the fixed architectural interval. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    cdj_c674x_loop_set_functional_timing(true);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.loads[0] = (CdjC674xLoad){.due = 5, .value = 0xfeedface,
                                .bank = 1, .dst = 0, .size = 0};
    c.load_count = 1;
    memory[32] = 0x0008c06a;                 /* MVKH .S2 0x1180,B0. */
    assert(cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.pc == 0x1080 && c.idle_cycles == 5 && c.load_count == 1);
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 5 && !c.idle_cycles && !c.load_count &&
           c.r[1][0] == 0xfeedface && c.pc == 0x1080);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.fault && c.cycles == 6 && c.pc == 0x1084 &&
           c.r[1][0] == 0x1180face);
    cdj_c674x_loop_set_functional_timing(false);

    /* A live branch pipeline defers recognition but not IFR latching. Once
     * the delay slots drain, the current branch target becomes IRP. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 5) | 3u;
    c.branch_due = 2; c.branch_target = 0x1040;
    assert(cdj_c674x_interrupt(&c, 1u << 5));
    assert(c.pc == 0x1000 && c.control[2] == (1u << 5));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && !c.branch_due);
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x10a0 && c.control[6] == 0x1040);

    /* Invalid request bits, nested entry and a legacy active-loop checkpoint
     * without its setup address fail closed rather than inventing state. */
    cdj_c674x_reset(&c, 0x1000);
    assert(!cdj_c674x_interrupt(&c, 1u << 3));
    assert(!c.cycles && c.pc == 0x1000 && !c.control[2]);
    cdj_c674x_reset(&c, 0x1000);
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.control[26] = (1u << 9) | 1u;
    assert(!cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.control[2] == (1u << 4));
    cdj_c674x_reset(&c, 0x1000);
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.loop_active = true; c.loop.sealed = true; c.loop.ii = 1;
    c.loop.length = 1; c.loop.iterations = 10; c.loop.cycle = 3;
    c.control[13] = 9;
    assert(!cdj_c674x_interrupt(&c, 1u << 4));
    assert(c.control[2] == (1u << 4));

    /* A loop that contains an SPMASK is still interruptible: SPRUFE8B 7.13.1
     * (printed page 697) enumerates every condition that blocks interrupt
     * draining and the presence of an SPMASK is not one of them. The loop
     * therefore drains its epilog, IRP names the SPLOOP execute packet and
     * ITSR records SPLX (7.13, 7.13.4), in strict timing as in breadth. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 4) | 3u;
    c.control[13] = 12;
    memory[0] = 0x38000; memory[1] = 0x130001; /* SPLOOP; SPMASK S1. */
    memory[2] = 0; memory[3] = 0x34000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (c.loop.cycle < 3)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_interrupt(&c, 1u << 4) && !c.fault);
    assert(c.loop_active && c.control[2] == (1u << 4));
    unsigned spmask_drain = 0;
    while (c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(++spmask_drain < 32);
    }
    assert(cdj_c674x_interrupt(&c, 0) && !c.fault);
    assert(c.pc == 0x1080 && c.control[6] == 0x1000 &&
           (c.control[27] & (1u << 14)) && !(c.control[26] & (1u << 14)));

    /* A maskable interrupt detected on a legal SPLOOP boundary executes that
     * boundary, freezes ILC, drains only the buffered epilog and vectors only
     * after the loop is idle. IRP names the setup packet and ITSR records
     * SPLX, so the real B IRP path restarts it as an interrupted loop. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1;
    c.control[4] = (1u << 4) | (1u << 7) | 3u;
    c.control[13] = 12;
    memory[0] = 0x38000;                    /* SPLOOP 1. */
    memory[1] = 3u << 23 | 3u << 18 | 1u << 13 | 0x58;
    memory[2] = 0; memory[3] = 0x34000;     /* NOP; SPKERNEL 0,0. */
    memory[56] = 0x001800e2;                /* INT7 handler: B IRP. */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop.cycle == 3 && c.control[13] == 8);
    assert(cdj_c674x_interrupt(&c, 1u << 7));
    assert(c.loop_active && c.pc == 0x1010 && c.control[2] == (1u << 7));
    c.loads[0] = (CdjC674xLoad){.due = c.cycles + 5,
        .value = 0x12345678, .bank = 0, .dst = 9, .size = 0};
    c.load_count = 1;
    uint32_t frozen_ilc = c.control[13];
    unsigned drain_steps = 0;
    do {
        /* A later higher-priority request remains pending; the INT7 chosen
         * at drain detection is stable through the epilog. */
        assert(cdj_c674x_interrupt(&c,
            drain_steps ? 0 : (1u << 4)));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.control[13] == frozen_ilc);
        assert(++drain_steps < 10);
    } while (c.loop_active);
    assert(drain_steps == 5 && c.pc == 0x1010 &&
           c.r[0][9] == 0x12345678);
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x10e0 && c.control[6] == 0x1000 &&
           c.control[2] == (1u << 4));
    assert((c.control[27] & (1u << 14)) && !(c.control[26] & (1u << 14)));
    finish_interrupt_entry(&c);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1000 && (c.control[26] & (1u << 14)));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop_active && c.control[13] == frozen_ilc - 1);

    /* SPLOOPW uses the same bounded epilog drain but no ILC minimum.  After
     * B IRP, the returned setup keeps the predicate termination false for
     * the first three cycles even when the current condition is false. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 7) | 3u;
    c.control[13] = 0x55aa55aa; c.r[1][1] = 1;
    memory[0] = 0x4003e000;                  /* [B1] SPLOOPW 1. */
    memory[1] = mvk(0, 8, 9);
    memory[2] = 0; memory[3] = 0x34000;      /* NOP; SPKERNEL 0,0. */
    memory[56] = 0x001800e2;                 /* INT7 handler: B IRP. */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop.cycle == 3 && c.control[13] == 0x55aa55aa);
    assert(cdj_c674x_interrupt(&c, 1u << 7));
    unsigned while_drain_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.control[13] == 0x55aa55aa);
        assert(++while_drain_steps < 10);
    } while (c.loop_active);
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x10e0 && c.control[6] == 0x1000 &&
           (c.control[27] & (1u << 14)));
    c.r[1][1] = 0;
    finish_interrupt_entry(&c);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1000 && (c.control[26] & (1u << 14)));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop_active && (c.loop_pred_history & 8));
    unsigned returned_while_steps = 0;
    while (c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(++returned_while_steps < 12);
    }
    assert(returned_while_steps >= 4 && c.control[13] == 0x55aa55aa);

    /* If SPLOOPW's delayed condition becomes true while its interrupt epilog
     * is draining, section 7.10.3 resumes after SPKERNEL and interrupts that
     * post-loop packet instead of restarting the loop setup address. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[5] = 0x1000;
    c.control[1] |= 1; c.control[4] = (1u << 7) | 3u;
    c.r[1][1] = 1;
    memory[0] = 0x4003e000;                  /* [B1] SPLOOPW 1. */
    for (unsigned i = 1; i < 7; ++i) memory[i] = 0;
    memory[7] = 0x34000;                     /* SPKERNEL 0,0. */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    while (!c.loop.sealed)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop.cycle == 7 && c.pc == 0x1020);
    assert(cdj_c674x_interrupt(&c, 1u << 7));
    c.r[1][1] = 0;
    unsigned terminating_drain_steps = 0;
    while (c.loop_active) {
        assert(cdj_c674x_interrupt(&c, 0));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(++terminating_drain_steps < 10);
    }
    assert(terminating_drain_steps == 4 && c.pc == 0x1020);
    assert(cdj_c674x_interrupt(&c, 0));
    assert(c.pc == 0x10e0 && c.control[6] == 0x1020 &&
           !(c.control[27] & (1u << 14)));

    /* Execute TI's copy-loop schedule with the real instruction core. The
     * replayed load, move and store share the same pre-cycle register state. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x1080; c.r[1][0] = 0x10c0;
    for (unsigned j = 0; j < 8; ++j) memory[32 + j] = 0x12340000 + j;
    CdjC674xInstruction loop_insns[] = {
        {.word = (2u << 23) | (1u << 18) | (1u << 13) | (11u << 9) | 0x64, .pc = 0x1000},
        {.word = (2u << 23) | (2u << 18) | 0x1fda, .pc = 0x1010},
        {.word = (2u << 23) | (1u << 13) | (11u << 9) | 0xf6, .pc = 0x1014}
    };
    CdjC674xLoop loop;
    assert(cdj_c674x_loop_init(&loop, 1, 8));
    for (unsigned t = 0; t < 17; ++t) {
        uint32_t tag = t == 0 ? 0 : t == 5 ? 1 : 2;
        if (t <= 6)
            assert(cdj_c674x_loop_load(&loop, &tag, (t == 0 || t >= 5) ? 1 : 0, t == 6, 6));
        uint32_t tags[8]; unsigned count; bool post, drained;
        assert(cdj_c674x_loop_issue(&loop, tags, &count, &post, &drained));
        CdjC674xPacket packet = {.count = count, .next_pc = 0x1020};
        for (unsigned j = 0; j < count; ++j) packet.instructions[j] = loop_insns[tags[j]];
        assert(cdj_c674x_execute(&c, &packet, read_word, write_memory, NULL));
    }
    assert(c.r[0][1] == 0x10a0 && c.r[1][0] == 0x10e0);
    for (unsigned j = 0; j < 8; ++j) assert(memory[48 + j] == memory[32 + j]);
    assert(!c.load_count && !c.store_count);

    /* An overlaid fault names its original program PC, not the replay PC,
     * and rolls back earlier instructions in the composite packet. */
    cdj_c674x_reset(&c, 0x1040);
    CdjC674xPacket composite = {.count = 2, .next_pc = 0x1044};
    composite.instructions[0] = (CdjC674xInstruction){.word = mvk(0, 0, 99), .pc = 0x1000};
    composite.instructions[1] = (CdjC674xInstruction){.word = 0xffffffff, .pc = 0x1010};
    assert(!cdj_c674x_execute(&c, &composite, read_word, write_memory, NULL));
    assert(c.fault_pc == 0x1010 && c.pc == 0x1040 && c.r[0][0] == 0 && !c.cycles);
    /* SPLOOPW records its predicate even if initially false, then checks
     * the value three cycles before an end-of-stage boundary. ILC is unused. */
    for (unsigned ii = 1; ii <= 14; ++ii)
        for (unsigned invert = 0; invert < 2; ++invert) {
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            c.control[13] = 91; c.control[14] = 27; c.control_ready[13] = 999;
            c.r[1][1] = invert; /* false condition for both polarities */
            memory[0] = 0x4003e000 | (ii - 1) << 23 | invert << 28;
            memory[1] = 0x34000; /* empty body ending in SPKERNEL 0,0 */
            memory[2] = mvk(0, 10, 42);
            unsigned steps = 0;
            do {
                assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
                assert(++steps < 20);
            } while (c.loop_active);
            assert(c.cycles == 1 + ((4 + ii - 1) / ii) * ii);
            assert(c.control[13] == 91 && c.control[14] == 27 && c.r[0][10] == 0);
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.r[0][10] == 42);
        }
    /* A late predicate update is not visible at the imminent boundary. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][1] = 1;
    memory[0] = 0x4183e000; /* [B1] SPLOOPW 4 */
    memory[1] = 0; memory[2] = 0;
    memory[3] = mvk(1, 1, 0); memory[4] = 0x34000;
    for (unsigned j = 0; j < 5; ++j) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_active && c.cycles == 5);
    for (unsigned j = 0; j < 4; ++j) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.loop_active && c.cycles == 9);
    /* NOP cycles are empty loop-buffer cycles. Exercise the exact compact
     * NOP 8 in a synthetic [B1] SPLOOPW 4, then change B1 late:
     * the stage boundary still observes the condition from three cycles ago. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][1] = 1;
    memory[0] = 0x4183e000; /* [B1] SPLOOPW 4 */
    memory[1] = 0x0c6eec6e; /* compact NOP 8; compact NOP 1 */
    memory[2] = mvk(1, 1, 0);
    memory[3] = 0x34000; /* SPKERNEL 0,0 */
    memory[4] = mvk(0, 10, 42);
    memory[7] = 0xe0400000; /* slot 1 contains compact instructions */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_active && c.cycles == 1);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_wait == 7 && c.loop.length == 1 && c.loop_tags == 0);
    for (unsigned j = 0; j < 7; ++j)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_wait == 0 && c.loop.length == 8 && c.loop.cycle == 8);
    unsigned compact_steps = 0;
    while (c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++compact_steps < 12);
    }
    assert(c.cycles == 17 && c.r[0][10] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[0][10] == 42);
    /* Decode SPLOOP/SPKERNEL from RAM and execute the complete copy loop,
     * without manually feeding the scheduler. Also exercise zero iterations. */
    for (unsigned iterations = 0; iterations <= 8; iterations += 8) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = iterations; c.r[0][1] = 0x1080; c.r[1][0] = 0x10c0;
        for (unsigned j = 0; j < 8; ++j) memory[32 + j] = 0x56780000 + j;
        memory[0] = 0x38000; /* SPLOOP 1 */
        memory[1] = loop_insns[0].word;
        memory[2] = 3u << 13; /* NOP 4 */
        memory[3] = loop_insns[1].word;
        memory[4] = (24u << 22) | 0x34001; /* SPKERNEL 6,0: reversed stage bits */
        memory[5] = loop_insns[2].word;
        memory[6] = mvk(0, 10, 42);
        unsigned steps = 0;
        do {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++steps < 30);
        } while (c.loop_active || c.r[0][10] != 42 || c.store_count);
        assert(c.control[13] == 0 && c.r[0][1] == 0x1080 + iterations * 4);
        assert(c.r[1][0] == 0x10c0 + iterations * 4);
        for (unsigned j = 0; j < 8; ++j)
            assert(memory[48 + j] == (iterations ? memory[32 + j] : 0));
    }
    /* ILC availability is enforced before starting a loop. */
    cdj_c674x_reset(&c, 0x1000); c.control_ready[13] = 4;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.loop_active && !c.cycles);

    /* BNOP must insert its NOP cycles even if its predicate is false. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (6u << 29) | 0x008ca362;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 6 && c.pc == 0x1004 && !c.branch_due);
    /* Six consecutive taken branches fill the pipeline. Each redirects on
     * its own cycle, even after earlier branches have changed the fetch PC. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    for (unsigned j = 0; j < 6; ++j) {
        c.r[1][j + 1] = 0x1040 + j * 4;
        memory[j] = ((j + 1) << 18) | 0x362;
    }
    for (unsigned j = 0; j < 6; ++j) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.branch_count == 4 && c.branch_due == 7);
    c.r[1][2] = 0x1080; /* in-flight target must already be captured */
    for (unsigned j = 1; j < 6; ++j) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.pc == 0x1040 + j * 4 && c.cycles == 6 + j);
    }
    assert(!c.branch_due && !c.branch_count);

    /* Two taken branches in one execute packet remain an explicit fault. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (1u << 18) | 0x363; memory[1] = (2u << 18) | 0x362;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && !c.branch_due && !c.branch_count);

    /* TI section 7.14: a branch started before SPLOOP cancels its buffer
     * when the fifth delay slot completes. It must not replay at the target. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][3] = 0x1040; c.control[13] = 10;
    memory[0] = (3u << 18) | 0x362;
    memory[1] = 2u << 13; /* NOP 3 */
    memory[2] = 0x38000;
    memory[3] = mvk(0, 4, 7);
    memory[16] = mvk(0, 5, 9);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop_active && c.cycles == 5);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.loop_active && c.pc == 0x1040 && c.cycles == 6 && c.r[0][4] == 7);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][5] == 9);
    /* Compact MVK uses a split signed immediate and the selected subset. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0xfe27; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][4] == 0xffffffff);
    cdj_c674x_reset(&c, 0x1000); memory[7] |= 1u << 19;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][20] == 0xffffffff && c.r[1][4] == 0);

    /* All L2c operations: high-subset operands, low predicate destination. */
    const uint32_t l2c_results[] = {0, 0x80000001, 0x80000001, 0, 1, 0, 0, 1};
    for (unsigned op = 0; op < 8; ++op) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][17] = 0x80000000; c.r[1][18] = 1;
        memory[0] = (1u << 13) | (1u << 12) | ((op & 4) << 9) |
                    (2u << 7) | ((op & 3) << 5) | 0x418;
        memory[7] = 0xe0280000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][1] == l2c_results[op] && c.r[0][17] == 0x80000000);
    }
    /* Immediate comparison also writes a low predicate register with RS=1. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][18] = 7;
    memory[0] = (7u << 13) | (1u << 11) | (2u << 7) | 0x27;
    memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][1] == 1);

    /* Compact register BNOP ignores RS and captures its B0-B15 target. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][5] = 0x1040; c.r[1][21] = 0x1080;
    memory[0] = (5u << 13) | 0x2ef; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6);
    /* SPRUFE8B Figure F-32 Sx1b (printed page 756) register BNOP with s = 0 is
     * an OPEN QUESTION and stays fail-closed.  The manual contradicts itself:
     * Figure F-32 draws s as an unconstrained field with no "(s = 1)"
     * parenthetical - contrast Figure F-31 op 110 on the same page - and
     * Table B-1 (printed page 715) footnotes ADDKPC, "B register", "B IRP" and
     * "B NRP" as S2-only while pointedly not footnoting "BNOP register"; but
     * the BNOP-register entry on printed page 168 is headed "unit = .S2", its
     * 32-bit figure hardwires bit 1 = 1, and cl6x refuses "BNOP .S1 B4,3" with
     * W0005 "Branch to register requires .S2 unit".  dis6x does decode 0xa2ee
     * as "BNOP.S1 B5,5", but dis6x is not authoritative on s-bit legality - it
     * also decodes 0xda6e as MVC.S1 where Figure F-31 says (s = 1).  Until the
     * question is settled, refusing an encoding hardware may reject is the
     * lesser error, so 0xa2ee must still fault. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][5] = 0x1040;
    memory[0] = 0xa2ee; memory[7] = 0xe0200000;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.fault && c.pc == 0x1000 && c.cycles == 0);
    /* The s = 1 twin of the same word is unaffected and still branches. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][5] = 0x1040;
    memory[0] = 0xa2ef; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6);

    /* SPRUFE8B Figure F-29 Sx2op (printed page 755): compact in-place .S ADD
     * and SUB, "src1 = dst" and "dst = src1 - src2", src2 of type xsint.  The
     * words come from TI's dis6x -mv6740, which names 0x622e "ADD.S1
     * A3,A4,A3", 0x6a2e "SUB.S1 A3,A4,A3", 0x622f "ADD.S2 B3,B4,B3", 0xb32e
     * "ADD.S1X A5,B6,A5" and, under an RS=1 header, 0x622e
     * "ADD.S1 A19,A20,A19"; the results below are hand-computed from those. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x00001000; c.r[0][4] = 0x00000234;
    memory[0] = 0x622e; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 0x00001234 && c.r[0][4] == 0x00000234);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x00001000; c.r[0][4] = 0x00000234;
    memory[0] = 0x6a2e;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 0x00000dcc && c.r[0][4] == 0x00000234);
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][3] = 0x00000010; c.r[1][4] = 0x00000007;
    memory[0] = 0x622f;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][3] == 0x00000017 && c.r[0][3] == 0);
    /* Bit 12 crosses src2 only; src1/dst stays on the s side. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][5] = 0x00000100; c.r[1][6] = 0x00000020; c.r[0][6] = 0x0badf00d;
    memory[0] = 0xb32e;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][5] == 0x00000120);
    /* Both three-bit register fields observe header RS. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][19] = 0x00001000; c.r[0][20] = 0x00000234;
    c.r[0][3] = 0x0badf00d; c.r[0][4] = 0x0badf00d;
    memory[0] = 0x622e; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][19] == 0x00001234 && c.r[0][3] == 0x0badf00d);
    /* Figure F-29's mnemonic table carries neither a BR nor a SAT column,
     * unlike Figures F-22 and F-25, so a saturating fetch packet leaves this
     * ADD alone: 0x7fffffff + 1 wraps instead of clamping, and no CSR.SAT
     * update is queued.  TI's compressor agrees - compiling "SADD .S1 A3,A4,A5"
     * next to "ADD .S1 A3,A4,A3" emits header SAT=1 with 0x622e for the ADD,
     * and dis6x reads that word back as ADD, not SADD. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x7fffffff; c.r[0][4] = 1;
    memory[0] = 0x622e; memory[7] = 0xe0204000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 0x80000000 && !c.load_count && !(c.control[1] & 0x200));

    /* The two neighbours of the newly opened space are unmoved.  Figure F-30
     * Sx5 differs from Sx2op only in bit 10: dis6x names 0xa5ae "ADDK.S1 5,A3"
     * and 0xfdae "ADDK.S1 31,A3", the top of the five-bit constant. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x00000100;
    memory[0] = 0xa5ae; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 0x00000105);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x00000100;
    memory[0] = 0xfdae;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 0x0000011f);
    /* Figure F-31 op 110 keeps its "(s = 1)" requirement: 0xda6f writes ILC
     * and its s = 0 twin 0xda6e is still refused.  Sx1b cannot claim 0xda6e
     * because Figure F-32 fixes bits 12-11 to 00 and this word has them set. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0x0000002a;
    memory[0] = 0xda6f;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 0x0000002a);
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0xda6e;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.fault && !strcmp(c.fault, "compact instruction not implemented") &&
           c.fault_word == 0xda6e);
    c.fault = NULL;
    /* The same holds across the whole src field, which is the other end of the
     * eight words the sweep used to count as a gap: Figure F-31 leaves bits 9-7
     * free and the s = 0 restriction is independent of them.  cl6x refuses
     * "MVC .S1 B0,ILC" with W0005 "Operation requires .S2 unit"; GNU names
     * 0xd86e "mvc .S1 b0,ilc" anyway, a B-file read on an A-side unit with no
     * cross path in the format.  src = 0 reads B0, so ILC takes 0x0000002a. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 0x0000002a;
    memory[0] = 0xd86f;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 0x0000002a);
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 0x0000002a;
    memory[0] = 0xd86e;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.fault && !strcmp(c.fault, "compact instruction not implemented") &&
           c.fault_word == 0xd86e && c.control[13] == 0 && c.cycles == 0);
    c.fault = NULL;

    /* CALLP writes the next execute-packet address and takes six cycles.
     * A parallel operation observes the old link register. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][3] = 0x2222;
    memory[0] = 0x10000813; /* CALLP .S2 1040,B3 || */
    memory[1] = (4u << 23) | (3u << 18) | 0x1058;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.r[1][3] == 0x1008 && c.r[0][4] == 0x2222 && c.cycles == 6);

    /* Compact CALLP retains its word-scaled displacement while its return
     * address may be a halfword instruction boundary. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (17u << 6) | 0x1a; memory[7] = 0xe0208000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1044 && c.r[0][3] == 0x1002 && c.cycles == 6);
    /* Captured firmware Scs10 at 0x118028e4 calls the aligned helper at
     * 0x118027ec. Halfword scaling would incorrectly target 0x11802866. */
    CdjC674xPacket captured_call = {.count = 1, .next_pc = 0x118028e6,
        .instructions = {{.compact = true, .pc = 0x118028e4,
            .header = 0xe8c08000, .word = 0xf0db}}};
    cdj_c674x_reset(&c, 0x118028e4);
    assert(cdj_c674x_execute(&c, &captured_call, read_word, NULL, NULL));
    assert(c.pc == 0x118027ec && c.r[1][3] == 0x118028e6 && c.cycles == 6);

    /* The 16-bit halfword pack family has parallel .L/.S encodings. Test
     * every operation on both sides with the cross path selected. */
    static const unsigned pack_l[] = {0x018, 0x3d8, 0x398, 0x378};
    static const unsigned pack_s[] = {0xff0, 0x260, 0x220, 0x420};
    static const uint32_t pack_result[] = {
        0x3344ccdd, 0x1122aabb, 0x1122ccdd, 0x3344aabb
    };
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned unit = 0; unit < 2; ++unit) {
                cdj_c674x_reset(&c, 0x1000);
                c.r[bank][1] = 0x11223344;
                c.r[bank ^ 1][2] = 0xaabbccdd;
                uint32_t encoding = unit ? pack_s[op] : pack_l[op];
                CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
                    .instructions = {{.pc = 0x1000, .word =
                        3u << 23 | 2u << 18 | 1u << 13 | 1u << 12 |
                        encoding | bank << 1}}};
                assert(cdj_c674x_execute(&c, &p, read_word, NULL, NULL));
                assert(c.r[bank][3] == pack_result[op]);
                assert(c.r[bank ^ 1][3] == 0 && c.cycles == 1);
            }
    static const unsigned pack4[] = {0xd18, 0xd38};
    static const uint32_t pack4_result[] = {0x2244bbdd, 0x1133aacc};
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned op = 0; op < 2; ++op) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[bank][5] = 0x11223344; c.r[bank ^ 1][6] = 0xaabbccdd;
            CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
                .instructions = {{.pc = 0x1000, .word =
                    7u << 23 | 6u << 18 | 5u << 13 | 1u << 12 |
                    pack4[op] | bank << 1}}};
            assert(cdj_c674x_execute(&c, &p, read_word, NULL, NULL));
            assert(c.r[bank][7] == pack4_result[op]);
        }

    /* CALLP cannot be issued behind another pending branch. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362; memory[1] = 0x10000812;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.r[1][3] == 0 && c.branch_target == 0x1040);
    /* TSCL/TSCH, SPRUFE8B 2.9.14: reset-disabled, a write to TSCL starts
     * counting on the following cycle, TSCL reads latch the high half, and
     * the counter includes idle CPU clocks. */
    cdj_c674x_reset(&c, 0x1000);
    assert(cdj_c674x_control_read(&c, 10) == 0 &&
           cdj_c674x_control_read(&c, 11) == 0);
    c.r[1][0] = 0xdeadbeef;
    CdjC674xPacket tsc = {.count = 1, .next_pc = 0x1004,
        .instructions = {{.pc = 0x1000,
            .word = (10u << 23) | 0x3a2u}}}; /* MVC B0,TSCL */
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.control_ready[16] == 1);
    tsc.next_pc += 4; tsc.instructions[0].pc += 4;
    tsc.instructions[0].word = (10u << 18) | 0x3e2u; /* MVC TSCL,B0 */
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.r[1][0] == 0 && c.cycles == 2 && c.control[16] == 0);
    tsc.next_pc += 4; tsc.instructions[0].pc += 4;
    tsc.instructions[0].word = (1u << 23) | (10u << 18) | 0x3e2u;
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.r[1][1] == 1 && c.cycles == 3);
    /* A later TSCL write cannot reset or disable an enabled counter. */
    uint64_t origin = c.control_ready[16];
    tsc.instructions[0].word = (10u << 23) | 0x3a2u;
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.control_ready[16] == origin && cdj_c674x_timestamp(&c) == 3);
    /* Force a low-half rollover and prove TSCH is the TSCL-time snapshot,
     * not a live view observed by the following MVC. */
    c.cycles = origin + (UINT64_C(1) << 32) + 7u;
    tsc.instructions[0].word = (2u << 23) | (10u << 18) | 0x3e2u;
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.r[1][2] == 7u && c.control[16] == 1u);
    c.cycles += UINT64_C(1) << 32;
    tsc.instructions[0].word = (3u << 23) | (11u << 18) | 0x3e2u;
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.r[1][3] == 1u);
    c.idle_cycles = 1;
    uint64_t before_idle = cdj_c674x_timestamp(&c);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_timestamp(&c) == before_idle + 1u);
    /* False predicates and rejected packets cannot start or snapshot TSC. */
    cdj_c674x_reset(&c, 0x1000);
    tsc.instructions[0].word = (1u << 29) | (10u << 23) | 0x3a2u;
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.control_ready[16] == 0);
    c.control_ready[16] = 1; c.cycles = UINT64_C(1) << 33;
    c.control[16] = 0x55aa55aa; c.r[1][4] = 0x12345678;
    tsc.instructions[0].word = (1u << 29) | (4u << 23) |
                                (10u << 18) | 0x3e2u;
    assert(cdj_c674x_execute(&c, &tsc, read_word, NULL, NULL));
    assert(c.control[16] == 0x55aa55aa && c.r[1][4] == 0x12345678);
    /* A conflicting packet cannot enable TSCL partially. */
    cdj_c674x_reset(&c, 0x1000);
    CdjC674xPacket rejected_enable = {.count = 2, .next_pc = 0x1010,
        .instructions = {
            {.pc = 0x1008, .word = (10u << 23) | 0x3a2u},
            {.pc = 0x100c, .word = (10u << 23) | 0x3a2u}}};
    assert(!cdj_c674x_execute(&c, &rejected_enable, read_word, NULL, NULL));
    assert(c.control_ready[16] == 0 && c.cycles == 0);
    c.fault = NULL;
    c.control_ready[16] = 1; c.cycles = UINT64_C(1) << 33;
    c.control[16] = 0x55aa55aa;
    CdjC674xPacket rejected = {.count = 2, .next_pc = 0x1010,
        .instructions = {
            {.pc = 0x1008, .word = (10u << 18) | 0x3e2u},
            {.pc = 0x100c, .word = (10u << 18) | 0x3e2u}}};
    assert(!cdj_c674x_execute(&c, &rejected, read_word, NULL, NULL));
    assert(c.control[16] == 0x55aa55aa &&
           c.cycles == (UINT64_C(1) << 33));

    puts("C674x sign extension, parallel reads, branch delay, NOP, TSC and atomic fault passed");
}
