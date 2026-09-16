/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Decoder-acceptance probe for the C674x core.
 *
 * Reads one hexadecimal 32-bit instruction word per line on stdin and reports
 * whether emulator/qemu/cdj_c674x.c fetches and executes it as a one-instruction
 * execute packet.  Output, one line per input:
 *
 *     <word> accept
 *     <word> fetch-reject <fault text>
 *     <word> execute-reject <fault text>
 *
 * Modes, selected by argv[1]:
 *
 *   decode <pointer|small>   the stdin sweep above (default mode)
 *   compact                  every one of the 65,536 16-bit words, under each
 *                            documented compact-header configuration
 *   control                  MVC reachability and read-mask fidelity for all 32
 *                            control register ids
 *   atomicity                a rejected execute packet must leave CPU state
 *                            bit-identical (packet rollback)
 *
 * The last three replace measurements that earlier audit passes made with
 * throwaway scratch programs.  An audit number nobody can regenerate is not
 * evidence, so they live here and tests/test_dsp_isa_audit.py asserts the
 * load-bearing invariants.
 *
 * This measures DECODE ACCEPTANCE ONLY.  An "accept" line says the core did not
 * reject the encoding; it says nothing about whether the architectural result is
 * correct.  Semantic fidelity is established by the reference-backed tests in
 * tests/cstub/c674x*.c, never by this probe.
 *
 * tools/cdj_dsp/isa_probe.py drives it with encodings that the TI assembler
 * produced from the SPRUFE8B syntax of each instruction, so the instruction
 * inventory comes from the manual rather than from our own decoder.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c674x_loop.h"

/* Code and data are far apart so a store through a pointer register can never
 * overwrite the instruction under test. */
#define CODE_BASE 0x00010000u
#define CODE_WORDS 16u                 /* one 32-byte fetch block, twice over */
/* The data window is deliberately large and the pointer registers sit in its
 * middle, so that a compact .D form with any documented offset, scale or DSZ
 * lands inside it.  With a small window the sweep reports thousands of
 * "unaligned or unmapped scalar memory access" rejections that are a property of
 * the probe, not of the decoder. */
#define DATA_BASE 0x00020000u
#define DATA_WORDS 4096u
#define DATA_MIDDLE (DATA_BASE + DATA_WORDS * 2u)

enum { PROFILE_POINTER, PROFILE_SMALL, PROFILE_ALL_POINTERS, PROFILE_COUNT };

static const char *const PROFILE_NAMES[PROFILE_COUNT] = {
    "pointer", "small", "all-pointers"
};

static uint32_t code[CODE_WORDS];
static uint32_t data[DATA_WORDS];

static bool in_range(uint32_t address, unsigned size, uint32_t base,
                     unsigned words)
{
    uint64_t end = (uint64_t)base + (uint64_t)words * 4u;

    return address >= base && (uint64_t)address + size <= end;
}

static bool probe_read(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address & 3) {
        return false;
    }
    if (in_range(address, 4, CODE_BASE, CODE_WORDS)) {
        *value = code[(address - CODE_BASE) / 4];
        return true;
    }
    if (in_range(address, 4, DATA_BASE, DATA_WORDS)) {
        *value = data[(address - DATA_BASE) / 4];
        return true;
    }
    return false;
}

static bool probe_write(void *unused, uint32_t address, uint64_t value,
                        unsigned size, bool commit)
{
    (void)unused;
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return false;
    }
    if (!in_range(address, size, DATA_BASE, DATA_WORDS)) {
        return false;
    }
    if (commit) {
        for (unsigned i = 0; i < size; ++i) {
            unsigned offset = address - DATA_BASE + i, shift = (offset & 3) * 8;

            data[offset / 4] = (data[offset / 4] & ~(255u << shift)) |
                               ((uint32_t)((value >> (8 * i)) & 255) << shift);
        }
    }
    return true;
}

/*
 * Register profiles the probe hands every instruction.
 *
 * "pointer" makes A4/B4 and B14/B15 address the data window so .D unit forms
 * have a legal pointer, with A6/B6 holding a small scaled register offset that
 * also stays inside the window.  The manual fixes the long-offset loads and
 * stores to B14/B15.
 *
 * "small" puts a small value in every register except B14/B15.  Instructions
 * that read a register as a field specifier rather than as data - EXT, EXTU,
 * CLR and SET in their register forms - get an out-of-range specifier under the
 * pointer profile, and rejecting those would be a property of the probe's
 * register values, not of the decoder.
 *
 * "all-pointers" puts the data window address in all four of A/B4-7, which the
 * compact .D formats use interchangeably as the pointer register.
 *
 * A candidate counts as accepted if ANY profile accepts it, and the report
 * records which one did.  Every remaining rejection is therefore either a decode
 * gap or a genuine architectural refusal, not a missing mapping.
 */
static void probe_reset(CdjC674x *cpu, unsigned profile)
{
    cdj_c674x_reset(cpu, CODE_BASE);
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned r = 0; r < 32; ++r) {
            cpu->r[side][r] = 3;
        }
        if (profile == PROFILE_POINTER) {
            cpu->r[side][4] = DATA_MIDDLE;
            cpu->r[side][5] = 0;
            cpu->r[side][6] = 2;
            cpu->r[side][7] = 0;
        } else if (profile == PROFILE_ALL_POINTERS) {
            /* Compact .D forms take their pointer from any of A/B4-7, so a
             * profile that holds all four is needed before an "unmapped" verdict
             * can be read as anything but a property of the probe. */
            for (unsigned r = 4; r < 8; ++r) {
                cpu->r[side][r] = DATA_MIDDLE;
            }
        }
        cpu->r[side][14] = DATA_MIDDLE;
        cpu->r[side][15] = DATA_MIDDLE;
    }
    /* A0/B0 nonzero so predicated forms take their true path. */
    cpu->r[0][0] = 1;
    cpu->r[1][0] = 1;
}


/* ---------------------------------------------------------------- decode ---- */

static const char *classify(uint32_t word, unsigned profile,
                            uint32_t header, const char **fault)
{
    CdjC674x cpu;
    CdjC674xPacket packet;

    memset(code, 0, sizeof(code));
    memset(data, 0, sizeof(data));
    probe_reset(&cpu, profile);
    code[0] = word;
    code[7] = header;

    *fault = "";
    if (!cdj_c674x_fetch(&cpu, probe_read, NULL, &packet)) {
        *fault = cpu.fault ? cpu.fault : "(no fault text)";
        return "fetch-reject";
    }
    if (!cdj_c674x_execute(&cpu, &packet, probe_read, probe_write, NULL)) {
        *fault = cpu.fault ? cpu.fault : "(no fault text)";
        return "execute-reject";
    }
    return "accept";
}

static int mode_decode(unsigned profile)
{
    char line[64];

    while (fgets(line, sizeof(line), stdin)) {
        unsigned long word;
        char *end;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        word = strtoul(line, &end, 16);
        if (end == line) {
            printf("- parse-error\n");
            continue;
        }
        const char *fault;
        /* Clear p: a one-instruction packet.  No compact header. */
        const char *verdict = classify((uint32_t)word & ~1u, profile, 0,
                                      &fault);
        if (*fault) {
            printf("%08lx %s %s\n", word, verdict, fault);
        } else {
            printf("%08lx %s\n", word, verdict);
        }
    }
    return 0;
}

/* --------------------------------------------------------------- compact ---- */

/*
 * Compact-header configurations.
 *
 * A compact instruction only exists inside a "mixed" fetch packet, which
 * cdj_c674x_fetch recognises by the top nibble of the header word being 0xE.
 * Bit 21 marks slot 0 as a short word.  The remaining bits select the
 * per-fetch-packet state that changes how a compact word decodes: SAT and BR
 * (SPRUFE8B section 3.10 header layout), the RS register-set bit, the three-bit
 * DSZ data-size field and PROT.  Every configuration is named so that an
 * accepted word can be traced back to the state that accepted it; a word
 * counted as accepted under "any configuration" with the configurations left
 * unnamed is not a defensible measurement.
 */
#define COMPACT_BASE (0xE0000000u | (1u << 21))

static const struct { const char *name; uint32_t header; } COMPACT_HEADERS[] = {
    { "plain",   COMPACT_BASE },
    { "sat",     COMPACT_BASE | (1u << 14) },
    { "br",      COMPACT_BASE | (1u << 15) },
    { "sat|br",  COMPACT_BASE | (1u << 14) | (1u << 15) },
    { "rs",      COMPACT_BASE | (1u << 19) },
    { "prot",    COMPACT_BASE | (1u << 20) },
    { "dsz1",    COMPACT_BASE | (1u << 16) },
    { "dsz2",    COMPACT_BASE | (2u << 16) },
    { "dsz3",    COMPACT_BASE | (3u << 16) },
    { "dsz4",    COMPACT_BASE | (4u << 16) },
    { "dsz5",    COMPACT_BASE | (5u << 16) },
    { "dsz6",    COMPACT_BASE | (6u << 16) },
    { "dsz7",    COMPACT_BASE | (7u << 16) },
};
#define COMPACT_HEADER_COUNT \
    (sizeof(COMPACT_HEADERS) / sizeof(COMPACT_HEADERS[0]))

/*
 * One line per 16-bit word:
 *
 *     <word> accept <header name> <register profile>
 *     <word> reject <fault text of the last configuration tried>
 *
 * A word is reported accepted if any configuration accepts it, and the line
 * names which one.  Words are emitted in ascending order so the output is a
 * stable, diffable artefact.
 */
static int mode_compact(void)
{
    for (uint32_t word = 0; word < 0x10000u; ++word) {
        const char *last_fault = "(none)";
        bool done = false;

        for (unsigned h = 0; h < COMPACT_HEADER_COUNT && !done; ++h) {
            for (unsigned profile = 0; profile < PROFILE_COUNT && !done; ++profile) {
                const char *fault;
                const char *verdict = classify(word, profile,
                                               COMPACT_HEADERS[h].header,
                                               &fault);

                if (!strcmp(verdict, "accept")) {
                    printf("%04x accept %s %s\n", word,
                           COMPACT_HEADERS[h].name, PROFILE_NAMES[profile]);
                    done = true;
                } else if (*fault) {
                    last_fault = fault;
                }
            }
        }
        if (!done) {
            printf("%04x reject %s\n", word, last_fault);
        }
    }
    return 0;
}

/* --------------------------------------------------------------- control ---- */

/*
 * MVC reachability and read-mask fidelity, for all 32 control register ids.
 *
 * For each id this drives a real MVC in both directions rather than calling the
 * static helpers, so the answer is the answer firmware would get:
 *
 *   MVC .S2 B4, creg   (write)   dst = creg, src2 = B4, low bits 0x3a2
 *   MVC .S2 creg, B5   (read)    dst = B5,   src2 = creg, low bits 0x3e2
 *
 * Both encodings were taken from TI's assembler (asm6x -mv6740) rather than from
 * our own decoder: "MVC .S2 B4, AMR" assembles to 0x001003a2 and
 * "MVC .S2 AMR, B5" to 0x028003e2.
 *
 * It writes 0xFFFFFFFF and reports what reads back.  Any bit that does not read
 * back as 1 is masked by the model; SPRUFE8B's per-register field tables say
 * which bits must read 0, so the printed mask is directly comparable to the
 * manual.  A register whose mask is 0xffffffff while the manual reserves bits
 * is a fidelity defect, and this mode is what makes that checkable.
 */
static int mode_control(void)
{
    for (unsigned id = 0; id < 32; ++id) {
        CdjC674x cpu;
        CdjC674xPacket packet;
        const char *write_fault = NULL, *read_fault = NULL;
        uint32_t readback = 0;

        memset(code, 0, sizeof(code));
        memset(data, 0, sizeof(data));
        probe_reset(&cpu, PROFILE_POINTER);
        cpu.r[1][4] = 0xffffffffu;

        /* MVC .S2 B4, <creg>: dst field is the control register id. */
        code[0] = (id << 23) | (4u << 18) | 0x3a2u;
        if (!cdj_c674x_fetch(&cpu, probe_read, NULL, &packet) ||
            !cdj_c674x_execute(&cpu, &packet, probe_read, probe_write, NULL)) {
            write_fault = cpu.fault ? cpu.fault : "(no fault text)";
            cpu.fault = NULL;
        }

        /* MVC .S2 <creg>, B5: src2 field is the control register id. */
        memset(code, 0, sizeof(code));
        cpu.pc = CODE_BASE;
        cpu.r[1][5] = 0;
        code[0] = (5u << 23) | (id << 18) | 0x3e2u;
        if (!cdj_c674x_fetch(&cpu, probe_read, NULL, &packet) ||
            !cdj_c674x_execute(&cpu, &packet, probe_read, probe_write, NULL)) {
            read_fault = cpu.fault ? cpu.fault : "(no fault text)";
        } else {
            readback = cpu.r[1][5];
        }

        printf("%2u write=%s read=%s mask=%08x", id,
               write_fault ? "reject" : "accept",
               read_fault ? "reject" : "accept", readback);
        if (write_fault) {
            printf(" write_fault=\"%s\"", write_fault);
        }
        if (read_fault) {
            printf(" read_fault=\"%s\"", read_fault);
        }
        printf("\n");
    }
    return 0;
}

/* ------------------------------------------------------------- atomicity ---- */

/*
 * Packet rollback.
 *
 * cdj_c674x_execute builds a transactional copy and commits only on success, so
 * a packet it rejects must leave the CPU bit-identical.  The three most recent
 * DSP commits before this audit were about that copy and its ordering, and no
 * audit track opened a requirement row for it, so it is measured here.
 *
 * Each case is a two-instruction parallel packet whose second instruction the
 * core rejects, chosen so the first instruction would otherwise have written
 * state: a register, a control register, and a store.  State is compared over
 * the whole struct except the fault fields the rejection is supposed to set.
 */
static int mode_atomicity(void)
{
    static const struct { const char *name; uint32_t first, second; } CASES[] = {
        /* MVK 123,A3 in parallel with a reserved-predicate word. */
        { "gpr-write-then-reserved-predicate", 0x01803da8u | 1u, 0xffffffffu },
        /* MVC B4,AMR in parallel with a reserved-predicate word. */
        { "control-write-then-reserved-predicate", 0x000203c2u | 1u, 0xffffffffu },
        /* STW .D1 A4,*+A4[4] in parallel with a reserved-predicate word. */
        { "store-then-reserved-predicate", 0x02108274u | 1u, 0xffffffffu },
        /* Two parallel writes to the same register: rejected by the core. */
        { "parallel-write-conflict", 0x01803da8u | 1u, 0x01803da8u },
    };
    int failures = 0;

    for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); ++i) {
        CdjC674x cpu, before;
        CdjC674xPacket packet;

        memset(code, 0, sizeof(code));
        memset(data, 0, sizeof(data));
        probe_reset(&cpu, PROFILE_POINTER);
        code[0] = CASES[i].first;
        code[1] = CASES[i].second;

        if (!cdj_c674x_fetch(&cpu, probe_read, NULL, &packet)) {
            printf("%-40s fetch-reject %s\n", CASES[i].name,
                   cpu.fault ? cpu.fault : "");
            continue;
        }
        uint32_t data_before[DATA_WORDS];
        memcpy(data_before, data, sizeof(data));
        before = cpu;

        bool ok = cdj_c674x_execute(&cpu, &packet, probe_read, probe_write, NULL);

        /* Neutralise the fields a rejection is entitled to change. */
        cpu.fault = before.fault;
        cpu.fault_pc = before.fault_pc;
        cpu.fault_word = before.fault_word;

        bool state_same = !memcmp(&cpu, &before, sizeof(cpu));
        bool memory_same = !memcmp(data_before, data, sizeof(data));

        printf("%-40s execute=%d state_unchanged=%d memory_unchanged=%d\n",
               CASES[i].name, ok, state_same, memory_same);
        if (!ok && !(state_same && memory_same)) {
            ++failures;
        }
    }
    printf("atomicity failures=%d\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "decode";

    if (!strcmp(mode, "compact")) {
        return mode_compact();
    }
    if (!strcmp(mode, "control")) {
        return mode_control();
    }
    if (!strcmp(mode, "atomicity")) {
        return mode_atomicity();
    }
    /* "decode <profile>", and the historical form where argv[1] is the profile. */
    const char *name = !strcmp(mode, "decode") ? (argc > 2 ? argv[2] : "pointer")
                                               : mode;
    for (unsigned i = 0; i < PROFILE_COUNT; ++i) {
        if (!strcmp(name, PROFILE_NAMES[i])) {
            return mode_decode(i);
        }
    }
    fprintf(stderr, "unknown register profile \"%s\"\n", name);
    return 2;
}
