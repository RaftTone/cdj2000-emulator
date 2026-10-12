/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential test of the C674x packet cache and fast path.
 *
 * Two identical systems run random programs in lockstep, one with the cache
 * and fast path (mode 2) and one with neither (mode 0).  After every step the
 * whole CPU and the whole memory must be byte-identical, faults included.
 * The programs store into their own code (self-modifying code), the harness
 * rewrites code between steps (an HPI/EDMA upload), the fetch-block hook
 * withdraws and restores a code window (a remap), and the bus fails a store
 * commit or a load's E3 read on chosen addresses (retirement faults after
 * the fast path has committed).  A second set of programs runs SPLOOP
 * loops of random bodies (loads, stores, NOP n, parallel packets, every II),
 * whose loop-buffer cycles take execute_fast in mode 2.  A few directed
 * cases check the same properties with known outcomes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c674x.h"

#define BASE 0x1000u
#define SIZE 0x1000u
#define CODE_END 0x1400u
#define BAD_COMMIT 0x1ff0u   /* store check passes, commit fails */
#define FLAKY_READ 0x1fe0u   /* readable only when the tick count % 3 != 0 */

typedef struct {
    uint8_t ram[SIZE];
    unsigned ticks;
    bool hide;               /* fetch-block hook refuses 0x1100..0x11ff */
    bool swap;               /* 0x1100..0x11ff maps to alt[] instead */
    uint8_t alt[0x100];
    /* Committed RAM writes while test_direct is off: the side effect a
     * board's write callback may have only while it says so (the idle
     * skip's write log on the NXS board). */
    uint32_t write_log;
} System;

/* The fetch epoch (cdj_c674x_set_fetch_epoch): moved whenever hide or swap
 * changes what a fetch block maps to. */
static uint64_t test_epoch;

static bool test_direct;

/* cdj_c674x_set_code_writes, as the NXS board keeps it, in half the seeds:
 * every write the system makes itself that may reach code counts. */
static uint64_t test_code_writes;
static void note_write(const void *host, size_t size)
{
    if (cdj_c674x_may_hold_code(host, size))
        __atomic_add_fetch(&test_code_writes, 1, __ATOMIC_RELEASE);
}

/* Tick counting (CdjC674xHorizon.count_ticks), as the NXS board does it:
 * in seeds that use it, system A's tick, once it has run, lets the
 * compiled paths count the ticks that follow instead of calling it, and
 * every callback, between() and comparison first applies what they
 * counted (tick_flush).  System B never counts. */
static System *count_sys;
static void tick_flush(void);

static uint8_t *sys_at(System *s, uint32_t address)
{
    if (s->swap && address >= 0x1100 && address < 0x1200)
        return s->alt + (address - 0x1100);
    return s->ram + (address - BASE);
}

static bool sys_read(void *opaque, uint32_t address, uint32_t *value)
{
    System *s = opaque;
    if (s == count_sys) tick_flush();
    if (address < BASE || address > BASE + SIZE - 4 || (address & 3)) return false;
    if (address == FLAKY_READ && s->ticks % 3 == 0) return false;
    memcpy(value, sys_at(s, address), 4);
    return true;
}

static bool sys_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    System *s = opaque;
    if (s == count_sys) tick_flush();
    if (address < BASE || (uint64_t)address + size > BASE + SIZE ||
        (address & (size - 1)))
        return false;
    if (commit) {
        if (address == BAD_COMMIT) return false;
        if (!test_direct) s->write_log = s->write_log * 31 + address;
        if (s->swap && address < 0x1200 && address + size > 0x1100 &&
            (address < 0x1100 || address + size > 0x1200)) {
            for (unsigned i = 0; i < size; ++i) {
                *sys_at(s, address + i) = value >> (8 * i);
                note_write(sys_at(s, address + i), 1);
            }
        } else {
            memcpy(sys_at(s, address), &value, size);
            note_write(sys_at(s, address), size);
        }
    }
    return true;
}

static const uint8_t *sys_block(void *opaque, uint32_t block)
{
    System *s = opaque;
    if (block < BASE || block >= BASE + SIZE) return NULL;
    if (s->hide && block >= 0x1100 && block < 0x1200) return NULL;
    /* Not plain memory: the read callback refuses it now and then. */
    if (block == (FLAKY_READ & ~31u)) return NULL;
    return sys_at(s, block);
}

/* The RAM windows (cdj_c674x_set_ram_window): plain memory except the
 * flaky and failing-commit block at the top; 0x1100..0x11ff follows the
 * swap.  test_direct: stores may bypass sys_write (toggled per seed and
 * at random during runs). */

static bool sys_window(void *opaque, uint32_t address, uint32_t *lo,
                       uint32_t *hi, uint8_t **host)
{
    System *s = opaque;
    if (address >= 0x1100 && address < 0x1200) {
        *lo = 0x1100; *hi = 0x1200;
        *host = s->swap ? s->alt : s->ram + 0x100;
        return true;
    }
    if (address >= BASE && address < 0x1100) {
        *lo = BASE; *hi = 0x1100; *host = s->ram;
        return true;
    }
    if (address >= 0x1200 && address < (FLAKY_READ & ~31u)) {
        *lo = 0x1200; *hi = FLAKY_READ & ~31u; *host = s->ram + 0x200;
        return true;
    }
    return false;
}

static CdjC674xHorizon test_horizon;

static void sys_tick(void *opaque)
{
    System *s = opaque;
    if (s == count_sys) tick_flush();
    ++s->ticks;
    memcpy(s->ram + SIZE - 4, &s->ticks, 4);
    note_write(s->ram + SIZE - 4, 4);
    if (s == count_sys) test_horizon.count_ticks = true;
}

static void tick_flush(void)
{
    uint64_t n = test_horizon.ticks;
    test_horizon.ticks = 0;
    test_horizon.count_ticks = false;
    if (!n) return;
    count_sys->ticks += n;
    memcpy(count_sys->ram + SIZE - 4, &count_sys->ticks, 4);
    note_write(count_sys->ram + SIZE - 4, 4);
}

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static unsigned pick_dst(void)
{
    unsigned r;
    do r = rnd() & 15; while (r == 10);   /* A10/B10 are the data bases */
    return r;
}

static uint32_t predicate(void)
{
    static const uint32_t creg[] = {1, 2, 3, 4, 5, 6};
    if (rnd() % 4) return 0;
    return creg[rnd() % 6] << 29 | (rnd() & 1) << 28;
}

/* One random 32-bit instruction at pc (parallel bit added by the caller). */
static uint32_t random_instruction(uint32_t pc)
{
    unsigned s = rnd() & 1, dst = pick_dst(), a = rnd() & 15, b = rnd() & 15;
    unsigned x = rnd() & 1;
    static const uint32_t l_ops[] = {0x78, 0xf8, 0xf78, 0xff8, 0xdf8, 0xa78,
                                     0x8f8, 0xaf8, 0x58};
    switch (rnd() % 15) {
    case 14: {                            /* CALLP .S1/.S2 into the code */
        int32_t target = (int32_t)(BASE + (rnd() % ((CODE_END - BASE) / 32)) * 32);
        int32_t disp = (target - (int32_t)(pc & ~31u)) / 4;
        return 0x10000010u | ((uint32_t)disp & 0x1fffff) << 7 | s << 1;
    }
    case 13:                              /* MVK .L scst5 */
        return predicate() | dst << 23 | (rnd() & 31) << 18 | x << 12 |
               0xa358 | s << 1;
    case 12: {                            /* more register-only forms */
        static const uint32_t ops[] = {
            0xa58, 0x8d8, 0x9d8, 0x9f8, 0xad8, 0xbd8, 0xbf8,  /* CMPxx(U) */
            0xf98, 0xdb0, 0x830,                              /* ANDN */
            0xab0, 0xaf0, 0xb30,                              /* ADD/SUB .D x */
            0x7a0, 0xf58, 0x9f0, 0x7e0, 0x9b0,                /* AND */
            0x1a0, 0x1e0, 0x0d8, 0x5a0, 0x5e0, 0x2f8, 0xd70,  /* ADD/SUB */
            0xfd8, 0x6a0, 0x8f0, 0x6e0, 0x8b0,                /* OR */
            0xdd8, 0x2a0, 0xbf0, 0x2e0, 0xbb0,                /* XOR */
            0x10u << 7 | 0x40, 0x11u << 7 | 0x40,             /* ADD/SUB .D */
            0x12u << 7 | 0x40, 0x13u << 7 | 0x40,             /* ... ucst5 */
        };
        /* src1 over all 32 values: the constant forms sign-extend or
         * not from bit 4. */
        return predicate() | dst << 23 | b << 18 | (rnd() & 31) << 13 |
               x << 12 | ops[rnd() % (sizeof ops / sizeof ops[0])] | s << 1;
    }
    case 0: return predicate() | dst << 23 | (rnd() & 0xffff) << 7 | 0x28 | s << 1;
    case 1: return predicate() | dst << 23 | (rnd() & 0xffff) << 7 | 0x68 | s << 1;
    case 2: case 3:
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               l_ops[rnd() % 9] | s << 1;
    case 4:                               /* SHL .S immediate */
        return predicate() | dst << 23 | b << 18 | (rnd() & 31) << 13 |
               0xca0 | s << 1;
    case 5: case 6: {                     /* LD/ST *+A10/B10[cst] */
        static const unsigned ops[] = {6, 7, 4, 5, 2, 3};
        unsigned y = rnd() & 1, op = ops[rnd() % 6];
        unsigned off = rnd() % 8 == 0 ? 31 : rnd() & 15;
        return predicate() | dst << 23 | 10u << 18 | off << 13 | 1u << 9 |
               y << 7 | op << 4 | 4 | s << 1;
    }
    case 7: {                             /* B .S1/.S2 into the code */
        int32_t target = (int32_t)(BASE + (rnd() % ((CODE_END - BASE) / 32)) * 32);
        int32_t disp = (target - (int32_t)(pc & ~31u)) / 4;
        return predicate() | ((uint32_t)disp & 0x1fffff) << 7 | 0x10 | s << 1;
    }
    case 8: {                             /* BNOP disp,n */
        int32_t target = (int32_t)(BASE + (rnd() % ((CODE_END - BASE) / 32)) * 32);
        int32_t disp = (target - (int32_t)(pc & ~31u)) / 4;
        return predicate() | ((uint32_t)disp & 0xfff) << 16 | (rnd() % 6) << 13 |
               0x120 | s << 1;
    }
    case 9: return (rnd() % 6) << 13;     /* NOP n */
    case 10: {                            /* MVC to AMR (slow path) or read */
        if (rnd() & 1) return 0u << 23 | 3u << 18 | 0x3a2;   /* MVC B3,AMR */
        return dst << 23 | 1u << 18 | 0x3e2;                 /* MVC CSR,Bdst */
    }
    default:                              /* ADDK */
        return predicate() | dst << 23 | (rnd() & 0xffff) << 7 | 0x50 | s << 1;
    }
}

/* Loop-body extras: the operations the NXS audio kernels pipeline - MPYSP,
 * ADDSP/SUBSP (every encoding), and loads/stores of every addressing mode
 * and width (LDNDW/STNDW, LDDW/STDW, LDNW/STNW included) through A4-A7 and
 * B4-B7, which init_cpu points into data RAM, some of them circular. */
static unsigned pick_body_dst(void)
{
    unsigned r;
    do r = rnd() & 15; while (r == 10 || (r >= 4 && r <= 7));
    return r;
}

/* Kernel-shaped bodies: only unpredicated SP and memory operations (and
 * NOPs), the shape steady-state kernels compile. */
static bool kernel_body, direct_extras, loop_extras;
static bool direct_programs;            /* dt_lockstep is running */

/* Direct-trace extras: CMPSP (FAUCR in place), 16x16 and half-by-word
 * multiplies (delayed results through the generic arm path), ADDA/SUBA
 * .D, the long ADDAB/H/W B14/B15 form and ADDKPC (multicycle). */
static uint32_t random_extra(void)
{
    unsigned s = rnd() & 1, x = rnd() & 1, dst = pick_body_dst();
    unsigned a = rnd() & 15, b = rnd() & 15;
    if (rnd() % 24 == 0)                  /* B .S2 A12/B12 (code addresses) */
        return predicate() | 12u << 18 | (rnd() & 1) << 12 | 0x362;
    if (rnd() % 7 == 0) {
        /* MVC to CSR or IER (written in place), or a read of what they
         * write (CSR, IER, TSR, ITSR): a packet holding both stays with
         * the interpreter. */
        static const unsigned reads[] = {1, 4, 26, 27};
        if (rnd() % 3)
            return predicate() | (rnd() % 2 ? 4u : 1u) << 23 | b << 18 | 0x3a2;
        return predicate() | dst << 23 | reads[rnd() % 4] << 18 | 0x3e2;
    }
    switch (rnd() % 6) {
    case 0:
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               (0x38 + rnd() % 3) << 6 | 0x20 | s << 1;        /* CMPxxSP */
    case 1: {
        static const unsigned ops[] = {0x19, 0x18, 0x01, 0x09, 0x0f, 0x03};
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               ops[rnd() % 6] << 7 | s << 1;                   /* MPY16 */
    }
    case 2: {
        static const unsigned ops[] = {0x0e, 0x10};
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               ops[rnd() % 2] << 6 | 0x30 | s << 1;            /* MPYIH/IL */
    }
    case 3:
        return predicate() | dst << 23 | (4 + rnd() % 4) << 18 |
               (rnd() & 7) << 13 | (0x30 + rnd() % 14) << 7 | 0x40 |
               s << 1;                                          /* ADDA .D */
    case 4:
        return 1u << 28 | dst << 23 | (rnd() & 0x7fff) << 8 |
               (rnd() & 1) << 7 | (rnd() % 3 == 0 ? 0x3c : 0x7c) |
               s << 1;                                          /* ADDAW */
    default:
        return predicate() | dst << 23 | (rnd() & 127) << 16 |
               (rnd() % 6) << 13 | 0x162 | 2;                   /* ADDKPC */
    }
}

static uint32_t random_body(uint32_t pc)
{
    if (direct_extras && rnd() % 4 == 0) return random_extra();
    /* Loop bodies: ABSSP and CMPxxSP, which write FAUCR in place (on a
     * NaN or a denormal: the registers hold random bits). */
    if (loop_extras && rnd() % 6 == 0) {
        /* Sources often A16-A19/B16-B19, which hold NaNs and denormals
         * (jit_lockstep): FAUCR then changes on most issues. */
        unsigned s = rnd() & 1, x = rnd() & 1, dst = pick_body_dst();
        unsigned a = rnd() % 2 ? 16 + rnd() % 4 : rnd() & 15;
        unsigned b = rnd() % 2 ? 16 + rnd() % 4 : rnd() & 15;
        return rnd() % 2 ? predicate() | dst << 23 | b << 18 | x << 12 |
                           0xf20 | s << 1                       /* ABSSP */
                         : predicate() | dst << 23 | b << 18 | a << 13 |
                           x << 12 | (0x38 + rnd() % 3) << 6 | 0x20 |
                           s << 1;                              /* CMPxxSP */
    }
    if (kernel_body) {
        if (rnd() % 4 == 0) return rnd() % 3 ? 0 : (rnd() % 4) << 13;
    } else if (rnd() % 2) return random_instruction(pc);
    unsigned s = rnd() & 1, x = rnd() & 1, dst = pick_body_dst();
    unsigned a = rnd() & 15, b = rnd() & 15;
    switch (rnd() % 4) {
    case 0:
        return (kernel_body ? 0 : predicate()) | dst << 23 | b << 18 |
               a << 13 | x << 12 | 0xe00 | s << 1;             /* MPYSP */
    case 1: {
        static const uint32_t enc[] = {0x218, 0xe18, 0x238, 0x2b8, 0xe38,
                                       0xeb8};
        return (kernel_body ? 0 : predicate()) | dst << 23 | b << 18 |
               a << 13 | x << 12 | enc[rnd() % 6] | s << 1;    /* ADD/SUBSP */
    }
    default: {
        static const unsigned modes[] = {0, 1, 8, 9, 10, 11, 1, 9, 11, 5, 13};
        unsigned mode = modes[rnd() % 11];
        unsigned offset = (mode & 4) ? 1 + rnd() % 2 : rnd() % 8; /* A1/A2.. */
        bool extended = rnd() % 3 == 0;
        unsigned op = rnd() & 7;
        if (extended && op < 2) op += 2;
        if ((extended && op != 3 && op != 5) && (dst & 1)) dst &= ~1u;
        return (kernel_body ? 0 : predicate()) | dst << 23 |
               (4 + rnd() % 4) << 18 |
               offset << 13 | mode << 9 | (extended ? 0x100u : 0) |
               (rnd() & 1) << 7 | op << 4 | 4 | s << 1;
    }
    }
}

static void build(System *s)
{
    memset(s->ram, 0, sizeof s->ram);
    for (uint32_t pc = BASE; pc < CODE_END; pc += 4) {
        uint32_t w = random_instruction(pc);
        if ((pc & 31) != 28 && rnd() % 4 == 0) w |= 1;
        memcpy(s->ram + (pc - BASE), &w, 4);
    }
    for (uint32_t i = CODE_END - BASE; i < SIZE - 4; i += 4) {
        uint32_t v = rnd();
        memcpy(s->ram + i, &v, 4);
    }
}

/* MVK n,B3; MVC B3,ILC; NOP 4; SPLOOP ii; body; SPKERNEL; post; B BASE. */
static void build_loop(System *s)
{
    build(s);
    uint32_t code[CODE_END - BASE], *w = code;
    unsigned n = 1 + (rnd() % 4 ? rnd() % 12 : rnd() % 200), len = 1 + rnd() % 20;
    unsigned ii = 1 + rnd() % (len < 14 ? len + 1 : 14);
    *w++ = 3u << 23 | n << 7 | 0x28 | 2;           /* MVK.S2 n,B3 */
    *w++ = 13u << 23 | 3u << 18 | 0x3a2;            /* MVC.S2 B3,ILC */
    *w++ = 3u << 13;                                /* NOP 4 */
    *w++ = (ii - 1) << 23 | 0x38000;                /* SPLOOP ii */
    for (unsigned i = 0; i < len; ++i) {
        uint32_t insn;
        do insn = random_body(BASE + 4 * (uint32_t)(w - code));
        while ((insn & 0x7c) == 0x10 || (insn & 0x1ffc) == 0x120 ||
               (insn & 0xffe) == 0x3a2 || (insn & 0xffe) == 0x3e2);
        if ((insn & 0x1ffff) == 0 && rnd() % 3) insn = 0;   /* mostly NOP 1 */
        if (i + 1 < len && ((BASE + 4 * (uint32_t)(w - code)) & 31) != 28 &&
            rnd() % 3 == 0)
            insn |= 1;
        *w++ = insn;
    }
    *w++ = 0x34000;                                 /* SPKERNEL 0,0 */
    for (unsigned i = rnd() % 4; i; --i) {
        uint32_t insn;
        do insn = random_instruction(BASE + 4 * (uint32_t)(w - code));
        while ((insn & 0x7c) == 0x10 || (insn & 0x1ffc) == 0x120 ||
               (insn & 0xffe) == 0x3a2);
        *w++ = insn;
    }
    uint32_t pc = BASE + 4 * (uint32_t)(w - code);
    *w++ = ((uint32_t)(-(int32_t)((pc & ~31u) - BASE) / 4) & 0x1fffff) << 7 |
           0x10;                                    /* B.S1 BASE */
    *w++ = 4u << 13;                                /* NOP 5 */
    memcpy(s->ram, code, 4 * (size_t)(w - code));
}

static bool loop_bases;

static void init_cpu(CdjC674x *cpu, System *s, uint32_t a10, uint32_t b10)
{
    cdj_c674x_reset(cpu, BASE);
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned r = 0; r < 32; ++r) cpu->r[side][r] = rnd();
    cpu->r[0][10] = a10;
    cpu->r[1][10] = b10;
    cpu->r[1][3] = 0;   /* MVC B3,AMR leaves every register linear */
    if (loop_bases) {
        /* Data pointers for random_body: A4-A7/B4-B7 into 0x1800-0x1bff,
         * small index registers A1/A2/B1/B2, and circular addressing for
         * some of the pointers (BK0 64..512 bytes, BK1 32..1024). */
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned r = 4; r <= 7; ++r)
                cpu->r[side][r] = 0x1800 + (rnd() % 0x80) * 8;
            cpu->r[side][1] = rnd() % 4;
            cpu->r[side][2] = rnd() % 4;
        }
        uint32_t amr = (5 + rnd() % 4) << 16 | (4 + rnd() % 6) << 21;
        for (unsigned field = 0; field < 8; ++field)
            if (rnd() % 3) amr |= (1 + rnd() % 2) << (field * 2);
        cpu->control[0] = amr;
        /* FADCR/FMCR rounding modes for both units (bits 10:9, 26:25). */
        cpu->control[18] = (rnd() & 3) << 9 | (rnd() & 3) << 25;
        cpu->control[20] = (rnd() & 3) << 9 | (rnd() & 3) << 25;
    }
    cpu->cycle_tick = sys_tick;
    cpu->cycle_opaque = s;
}

static void same(const CdjC674x *a, const System *sa, const CdjC674x *b,
                 const System *sb, unsigned seed, unsigned step)
{
    if (count_sys) tick_flush();
    CdjC674x x = *a, y = *b;
    x.cycle_opaque = y.cycle_opaque = NULL;
    bool fault = a->fault != b->fault ||
                 (a->fault && strcmp(a->fault, b->fault));
    x.fault = y.fault = NULL;
    if (fault || memcmp(&x, &y, sizeof x) || memcmp(sa, sb, sizeof *sa)) {
        fprintf(stderr, "seed %u step %u: cached and uncached diverge "
                "(fault %s / %s, pc %08x / %08x)\n", seed, step,
                a->fault ? a->fault : "-", b->fault ? b->fault : "-",
                a->pc, b->pc);
        abort();
    }
}

static unsigned packets, faults_seen, loop_steps;

static void lockstep(unsigned seed, bool loops)
{
    count_sys = NULL;
    static System sa, sb;
    static CdjC674x a, b;
    rng_state = seed * 2654435761u + 1;
    if (loops) build_loop(&sa);
    else build(&sa);
    sa.ticks = 0;
    sa.hide = false;
    ++test_epoch;
    sb = sa;
    uint32_t state = rng_state;
    /* Half the programs point B10 into their own code: self-modifying code. */
    uint32_t b10 = seed & 1 ? BASE + (rnd() % 0x380) * 4 : 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    state = rng_state;
    loop_bases = loops;
    init_cpu(&a, &sa, a10, b10);
    rng_state = state;
    init_cpu(&b, &sb, a10, b10);
    same(&a, &sa, &b, &sb, seed, 0);
    for (unsigned step = 1; step <= 3000; ++step) {
        if (!loops && rnd() % 97 == 0) {  /* host upload into code */
            uint32_t pc = BASE + (rnd() % ((CODE_END - BASE) / 4)) * 4;
            uint32_t w = random_instruction(pc);
            memcpy(sa.ram + (pc - BASE), &w, 4);
            memcpy(sb.ram + (pc - BASE), &w, 4);
            note_write(sa.ram + (pc - BASE), 4);
        }
        if (rnd() % 89 == 0) {
            sa.hide = sb.hide = !sa.hide;
            ++test_epoch;
        }
        cdj_c674x_set_packet_cache(2);
        uint64_t before = a.packets;
        loop_steps += a.loop_active;
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        cdj_c674x_set_packet_cache(0);
        bool rb = cdj_c674x_step(&b, sys_read, sys_write, &sb);
        assert(ra == rb);
        same(&a, &sa, &b, &sb, seed, step);
        packets += a.packets != before;
        if (!ra) {
            ++faults_seen;
            return;
        }
    }
}

/* Directed: a store into the next packet's code is seen when it executes. */
static void self_modifying(void)
{
    static System s;
    CdjC674x cpu;
    memset(&s, 0, sizeof s);
    uint32_t code[] = {
        2u << 23 | 0x1234u << 7 | 0x28,                      /* MVK.S1 0x1234,A2 */
        3u << 23 | 10u << 18 | 3u << 13 | 1u << 9 | 7u << 4 | 4, /* STW A3,*+A10[3] */
        4u << 13,                                            /* NOP 5 */
        0x10,                                                /* B.S1 0x1000 */
        4u << 13,                                            /* NOP 5 */
    };
    memcpy(s.ram, code, sizeof code);
    cdj_c674x_set_packet_cache(2);
    cdj_c674x_reset(&cpu, BASE);
    cpu.r[0][10] = BASE;                  /* *+A10[3] is the B at 0x100c */
    cpu.r[0][3] = code[3];                /* first pass stores the B itself */
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&cpu, sys_read, sys_write, &s));
    assert(cpu.pc == BASE);               /* looped: every packet now cached */
    cpu.r[0][3] = 7u << 23 | 0x55u << 7 | 0x28;   /* MVK.S1 0x55,A7 */
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&cpu, sys_read, sys_write, &s));
    /* The cached branch was overwritten by the DSP's own store. */
    assert(cpu.r[0][7] == 0x55 && cpu.pc == 0x1014);
    /* Second pass over unchanged code is served from the cache; a host
     * rewrite of the first word must still be seen. */
    uint32_t mvk2 = 2u << 23 | 0x4321u << 7 | 0x28;
    memcpy(s.ram, &mvk2, 4);
    cpu.pc = BASE;
    assert(cdj_c674x_step(&cpu, sys_read, sys_write, &s));
    assert(cpu.r[0][2] == 0x4321);
}

/* JIT lockstep: system A runs loop-buffer cycles through cdj_c674x_run
 * (compiled), system B steps the interpreter with the cache off.  B takes
 * its step inside A's between callback, right after A's packet, so the two
 * are compared after every packet; both then get the same random interrupt
 * request (GIE and IER are set, so loops drain and vector).  Random run
 * limits and between() refusals cover every cdj_c674x_run exit. */
typedef struct {
    CdjC674x *a, *b;
    System *sa, *sb;
    unsigned seed, step;
} JitPair;

static unsigned jit_packets, jit_runs, jit_between_exits, jit_stops,
                jit_armed, quiet_checks, loud_checks;

/* The board horizon (cdj_c674x.h): after a presentation without a request,
 * half the time A may skip between() for a few packets (or until a random
 * break PC); B then catches up those steps, each followed by the no-op
 * presentation the skip stood for, and the two are compared after the
 * last.  Seeds with horizon_off run without one. */
static unsigned horizon_skips;
/* What bounded the horizon: a request the board will present when the
 * packet count reaches the bound, or at the break PC.  B's catch-up
 * presents it wherever B reaches it, A only where its between() runs, so a
 * core that skipped past the bound diverges. */
static uint64_t event_packets;
static uint32_t event_pc, event_mask;

static uint32_t horizon_event(const CdjC674x *cpu)
{
    if (!event_mask ||
        !(cpu->packets == event_packets || (event_pc && cpu->pc == event_pc)))
        return 0;
    uint32_t mask = event_mask;
    event_mask = 0;
    return mask;
}

/* B's interpreter step, outside A's horizon. */
static CdjC674xHorizon *current_horizon;
static bool b_step(JitPair *p)
{
    cdj_c674x_set_horizon(NULL);
    bool r = cdj_c674x_step(p->b, sys_read, sys_write, p->sb);
    cdj_c674x_set_horizon(current_horizon);
    return r;
}

static void catch_up(JitPair *p)
{
    unsigned skipped = test_horizon.skipped;
    test_horizon.skipped = 0;
    horizon_skips += skipped;
    for (unsigned k = 0; k < skipped; ++k) {
        cdj_c674x_set_packet_cache(0);
        assert(b_step(p));
        cdj_c674x_set_packet_cache(2);
        ++p->step;
        uint32_t mask = horizon_event(p->b);
        assert(cdj_c674x_interrupt(p->b, mask));
        if (mask) event_mask = mask;    /* A's turn to look for it */
    }
}

static void horizon_open(JitPair *p, uint32_t pending)
{
    test_horizon.until = 0;
    test_horizon.break_pc = 0;
    event_mask = 0;
    if (pending || rnd() % 2) return;
    test_horizon.until = p->a->packets + 1 +
        (rnd() % 4 ? rnd() % 12 : rnd() % 2000);   /* some long, as a board's */
    if (rnd() % 4 == 0) test_horizon.break_pc = BASE + (rnd() % 64) * 4;
    if (rnd() % 2) {
        event_packets = test_horizon.until;
        event_pc = test_horizon.break_pc;
        event_mask = 1u << (4 + rnd() % 12);
    }
}

static void present(JitPair *p)
{
    /* Interrupt recognition never reads the queues while a loop is active,
     * which is the only time steady execution lasts across between(). */
    if (rnd() % 97 == 0) test_direct = !test_direct;
    uint32_t pending = rnd() % 23 == 0 ? (1u << (4 + rnd() % 12)) : 0;
    pending |= horizon_event(p->a);
    uint64_t armed = UINT64_C(1) << 62;     /* loop interrupt armed */
    bool was = p->a->control_ready[31] & armed;
    /* cdj_c674x_interrupt_quiet: a no-op presentation, exactly. */
    static CdjC674x probe;
    cdj_c674x_view(p->a, &probe);
    if (cdj_c674x_interrupt_quiet(&probe)) {
        static CdjC674x after;
        after = probe;
        assert(cdj_c674x_interrupt(&after, 0) &&
               !memcmp(&after, &probe, sizeof probe));
        ++quiet_checks;
    } else ++loud_checks;
    bool ra = cdj_c674x_interrupt(p->a, pending);
    bool rb = cdj_c674x_interrupt(p->b, pending);
    assert(ra == rb);
    horizon_open(p, pending);
    jit_armed += !was && (p->a->control_ready[31] & armed);
    static CdjC674x view;
    cdj_c674x_view(p->a, &view);
    same(&view, p->sa, p->b, p->sb, p->seed, p->step);
}

static void step_b(JitPair *p, bool expect)
{
    catch_up(p);
    cdj_c674x_set_packet_cache(0);
    bool rb = b_step(p);
    cdj_c674x_set_packet_cache(2);
    assert(rb == expect);
    ++p->step;
    /* Inside a run A may be in a steady-state kernel, whose queue arrays
     * are rebuilt only when it ends: compare the rebuilt view. */
    static CdjC674x view;
    cdj_c674x_view(p->a, &view);
    same(&view, p->sa, p->b, p->sb, p->seed, p->step);
}

/* A run that ends with CDJ_C674X_RUN_BETWEEN after a skipped between()
 * leaves B behind: the skip stood for that between(). */
static void catch_up_compare(JitPair *p)
{
    if (!test_horizon.skipped) return;
    catch_up(p);
    static CdjC674x view;
    cdj_c674x_view(p->a, &view);
    same(&view, p->sa, p->b, p->sb, p->seed, p->step);
}

/* dt_directed_toggle: the word between() flips between two encodings. */
static uint32_t toggle_at, toggle_word[2];

static bool jit_between(void *opaque)
{
    JitPair *p = opaque;
    ++jit_packets;
    step_b(p, true);
    if (rnd() % 61 == 0) return false;
    if (toggle_at && rnd() % 5 == 0) {
        uint32_t w;
        memcpy(&w, sys_at(p->sa, toggle_at), 4);
        w = toggle_word[w == toggle_word[0]];
        memcpy(sys_at(p->sa, toggle_at), &w, 4);
        memcpy(sys_at(p->sb, toggle_at), &w, 4);
        note_write(sys_at(p->sa, toggle_at), 4);
    }
    /* What a board's between-step work may do: write code (an EDMA
     * transfer into it, say), in direct-trace programs. */
    if (direct_programs && rnd() % 53 == 0) {
        uint32_t pc = BASE + (rnd() % ((CODE_END - BASE) / 4)) * 4;
        uint32_t w = random_instruction(pc);
        memcpy(sys_at(p->sa, pc), &w, 4);
        memcpy(sys_at(p->sb, pc), &w, 4);
        note_write(sys_at(p->sa, pc), 4);
    }
    present(p);
    return true;
}

static void jit_lockstep(unsigned seed)
{
    static System sa, sb;
    static CdjC674x a, b;
    rng_state = seed * 2654435761u + 7;
    kernel_body = seed % 3 == 1;
    loop_extras = seed % 3 == 2;
    build_loop(&sa);
    kernel_body = loop_extras = false;
    /* Functional timing: interrupt entry then sizes its pipe-down from the
     * queues, which between() reads right after a loop ends. */
    cdj_c674x_loop_set_functional_timing(seed % 4 == 3);
    sa.ticks = 0;
    sa.hide = false;
    ++test_epoch;
    sb = sa;
    uint32_t b10 = 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    uint32_t state = rng_state;
    loop_bases = true;
    init_cpu(&a, &sa, a10, b10);
    rng_state = state;
    init_cpu(&b, &sb, a10, b10);
    static const uint32_t special[] = {0x7fc00000u, 0x7f800001u, 0x00000123u,
                                       0x80400000u};
    for (unsigned side = 0; seed % 3 == 2 && side < 2; ++side)
        for (unsigned r = 16; r < 20; ++r)
            a.r[side][r] = b.r[side][r] = special[(r + side) % 4];
    if (seed & 1) {                       /* interrupts recognized */
        a.control[1] |= 1; b.control[1] |= 1;          /* GIE */
        a.control[4] = b.control[4] = 0xfff3;          /* IER */
        a.control[5] = b.control[5] = BASE;            /* ISTP */
    }
    JitPair p = {&a, &b, &sa, &sb, seed, 0};
    count_sys = current_horizon && seed % 3 == 0 ? &sa : NULL;
    bool pre_done = false;
    while (p.step < 4000) {
        if (!pre_done) present(&p);
        pre_done = false;
        if (a.loop_active && a.packets == b.packets) {
            unsigned status, limit = 1 + rnd() % 64;
            uint64_t before = a.packets;
            unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, limit,
                                       jit_between, &p, &status);
            assert(n <= limit && a.packets == before + n);
            jit_runs += n != 0;
            if (status == CDJ_C674X_RUN_FAULT) {
                step_b(&p, false);
                ++faults_seen;
                return;
            }
            if (status == CDJ_C674X_RUN_BETWEEN) {
                catch_up_compare(&p);
                ++jit_between_exits;
                pre_done = true;
                continue;
            }
            if (status == CDJ_C674X_RUN_STOPPED) {
                ++jit_stops;
                continue;
            }
            if (n) {
                ++jit_packets;
                step_b(&p, true);
                continue;
            }
        }
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        step_b(&p, ra);
        if (!ra) {
            ++faults_seen;
            return;
        }
    }
}

/* Direct-trace lockstep: random direct programs (build()'s instruction mix
 * with half the slots replaced by random_body's MPYSP, ADDSP/SUBSP and
 * every load/store form through A4-A7/B4-B7) run through cdj_c674x_run
 * (system A) against the uncached interpreter (B), compared after every
 * packet, with random interrupts, run limits and between() refusals, host
 * code uploads, a withdrawn code window and failing bus operations. */
static uint32_t random_compact(void)
{
    switch (rnd() % 10) {
    case 0: case 1: case 2:
        return 0x8c05u | (rnd() & 0x73f8u);         /* B15 word (C-16) */
    case 3: case 4: case 9:
        return 0x0077u | (rnd() & 0xf780u);         /* Dpp (C-21) */
    case 5: return 0x0866u | (rnd() & 0xe399u);     /* MVK01 (G-3) */
    case 6: case 7:
        return 0x0006u | (rnd() & 0xffd9u);         /* moves (G-1/G-2) */
    case 8: return rnd() & 0xffffu;                 /* anything */
    default: return 0x042eu | (rnd() & 0xfb81u);    /* ADDK */
    }
}

static void build_direct(System *s)
{
    build(s);
    direct_extras = true;
    for (uint32_t pc = BASE; pc < CODE_END; pc += 4) {
        uint32_t w;
        memcpy(&w, s->ram + (pc - BASE), 4);
        /* Keep the data pointers A4-A7/B4-B7 mostly intact, so programs
         * run long enough to matter (a few still clobber them). */
        unsigned dst = (w >> 23) & 31;
        if ((w & 0x7c) != 0x10 && (w & 0x1ffc) != 0x120 && dst >= 4 &&
            dst <= 7 && rnd() % 8)
            w = (w & ~(31u << 23)) | (8u + (dst & 1)) << 23;
        memcpy(s->ram + (pc - BASE), &w, 4);
        if (rnd() % 2) continue;
        uint32_t body;
        do body = random_body(pc);
        while ((body & 0x7c) == 0x10 || (body & 0x1ffc) == 0x120);
        w = (body & ~1u) | (w & 1);
        memcpy(s->ram + (pc - BASE), &w, 4);
    }
    /* One fetch packet in eight compact (SPRUFE8B 3.10): a header in
     * word 7 with a random layout, expansion field and p-bits, and the
     * words it marks holding two 16-bit instructions - the B15 word and
     * Dpp stack transfers, MVK01, ADDK, the moves, or anything. */
    for (uint32_t block = BASE; block < CODE_END; block += 32) {
        if (rnd() % 8) continue;
        uint32_t layout = rnd() & 0x7f;
        uint32_t header = 0xe0000000u | layout << 21 | (rnd() & 0x7f) << 14 |
                          (rnd() & 0x3fff);
        for (unsigned i = 0; i < 7; ++i) {
            if (!((layout >> i) & 1)) continue;
            uint32_t w = random_compact() | (uint32_t)random_compact() << 16;
            memcpy(s->ram + (block + 4 * i - BASE), &w, 4);
        }
        memcpy(s->ram + (block + 28 - BASE), &header, 4);
    }
    direct_extras = false;
}

static unsigned dt_runs;

static void dt_lockstep(unsigned seed)
{
    static System sa, sb;
    static CdjC674x a, b;
    rng_state = seed * 2654435761u + 11;
    build_direct(&sa);
    /* Other code behind the swappable window. */
    for (unsigned i = 0; i < sizeof sa.alt; i += 4) {
        uint32_t w = random_instruction(0x1100 + i) | (rnd() & 1);
        memcpy(sa.alt + i, &w, 4);
    }
    sa.swap = false;
    cdj_c674x_loop_set_functional_timing(seed % 4 == 3);
    sa.ticks = 0;
    sa.hide = false;
    ++test_epoch;
    sb = sa;
    uint32_t b10 = seed & 1 ? BASE + (rnd() % 0x380) * 4 : 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    uint32_t state = rng_state;
    loop_bases = true;
    init_cpu(&a, &sa, a10, b10);
    rng_state = state;
    init_cpu(&b, &sb, a10, b10);
    /* B15 into data RAM, for the compact stack transfers; A12/B12 at
     * fetch packets, for the register branches. */
    a.r[1][15] = b.r[1][15] = 0x1c00 + (rnd() % 128) * 4;
    for (unsigned side = 0; side < 2; ++side)
        a.r[side][12] = b.r[side][12] =
            BASE + (rnd() % ((CODE_END - BASE) / 32)) * 32;
    if (seed & 2) {                       /* interrupts recognized */
        a.control[1] |= 1; b.control[1] |= 1;
        a.control[4] = b.control[4] = 0xfff3;
        a.control[5] = b.control[5] = BASE;
    }
    JitPair p = {&a, &b, &sa, &sb, seed, 0};
    count_sys = current_horizon && seed % 3 == 0 ? &sa : NULL;
    bool pre_done = false;
    while (p.step < 3000) {
        if (!pre_done) {
            if (rnd() % 97 == 0) {        /* host upload into code */
                uint32_t pc = BASE + (rnd() % ((CODE_END - BASE) / 4)) * 4;
                uint32_t w = random_instruction(pc);
                memcpy(sa.ram + (pc - BASE), &w, 4);
                memcpy(sb.ram + (pc - BASE), &w, 4);
                note_write(sa.ram + (pc - BASE), 4);
            }
            if (rnd() % 89 == 0) {
                sa.hide = sb.hide = !sa.hide;
                ++test_epoch;
            }
            if (rnd() % 83 == 0) {
                sa.swap = sb.swap = !sa.swap;
                ++test_epoch;
            }
            present(&p);
        }
        pre_done = false;
        if (a.packets == b.packets) {
            unsigned status, limit = 1 + rnd() % 64;
            uint64_t before = a.packets;
            unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, limit,
                                       jit_between, &p, &status);
            assert(n <= limit && a.packets == before + n);
            dt_runs += n != 0;
            if (status == CDJ_C674X_RUN_FAULT) {
                step_b(&p, false);
                ++faults_seen;
                return;
            }
            if (status == CDJ_C674X_RUN_BETWEEN) {
                catch_up_compare(&p);
                pre_done = true;
                continue;
            }
            if (status == CDJ_C674X_RUN_STOPPED) continue;
            if (n) {
                ++jit_packets;
                step_b(&p, true);
                continue;
            }
        }
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        step_b(&p, ra);
        if (!ra) {
            ++faults_seen;
            return;
        }
    }
}

/* Directed direct-trace declines after state the plan changes in place:
 * a delayed-result append (MPY), FAUCR (CMPGTSP on NaN) and ILC are each
 * followed in their packet by a load that faults at issue (unaligned), so
 * the trace must put everything back before the interpreter faults. */
/* Fill the stack the next call will use, so a slot restored from an
 * unrecorded undo entry cannot happen to hold the right bytes. */
static __attribute__((noinline)) void stack_poison(void)
{
    volatile uint8_t junk[64 * 1024];
    for (size_t i = 0; i < sizeof junk; ++i) junk[i] = 0xa5;
}

static void dt_directed(void)
{
    static System sa, sb;
    static CdjC674x a, b;
    const uint32_t nan = 0x7fc00000u;
    const uint32_t first[] = {
        8u << 23 | 2u << 18 | 3u << 13 | 0x19u << 7 | 1,          /* MPY .M1 */
        8u << 23 | 2u << 18 | 3u << 13 | 0x39u << 6 | 0x20 | 1,   /* CMPGTSP */
    };
    for (unsigned k = 0; k < 2; ++k) {
        memset(&sa, 0, sizeof sa);
        uint32_t code[8] = {
            first[k],
            9u << 23 | 10u << 18 | 1u << 13 | 1u << 9 | 6u << 4 | 4,  /* LDW */
            4u << 13,                                             /* NOP 5 */
        };
        memcpy(sa.ram, code, sizeof code);
        sb = sa;
        for (unsigned side = 0; side < 2; ++side) {
            CdjC674x *cpu = side ? &b : &a;
            cdj_c674x_reset(cpu, BASE);
            cpu->r[0][2] = cpu->r[0][3] = nan;
            cpu->r[0][10] = 0x1801;                  /* unaligned base */
            cpu->control[19] = 0x00000005u;
            cpu->cycle_tick = sys_tick;
            cpu->cycle_opaque = side ? &sb : &sa;
            /* A queued entry in the slot the append will use: its bytes
             * must survive the decline. */
            cpu->loads[0] = (CdjC674xLoad){.due = 1000, .dst = 7,
                                           .address = 0xabcd, .size = 4};
        }
        /* Fill the packet cache with this code (the plan comes from it). */
        static System sc;
        static CdjC674x c;
        sc = sa;
        c = a;
        c.cycle_opaque = &sc;
        c.loads[0].address = 0x5555;    /* other bytes on the stack */
        assert(!cdj_c674x_step(&c, sys_read, sys_write, &sc));
        CdjC674xJitStats before, after;
        cdj_c674x_jit_stats(&before);
        stack_poison();
        unsigned status;
        unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, 1, NULL,
                                   NULL, &status);
        cdj_c674x_jit_stats(&after);
        assert(!n && !status && after.direct_plans == before.direct_plans + 1);
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        cdj_c674x_set_packet_cache(0);
        bool rb = cdj_c674x_step(&b, sys_read, sys_write, &sb);
        cdj_c674x_set_packet_cache(2);
        assert(!ra && !rb);
        same(&a, &sa, &b, &sb, 0, k);
    }
}

/* Directed static-schedule checks: a loop of fixed packets run through
 * cdj_c674x_run (system A) against the interpreter (B) as dt_lockstep
 * does, whose first pass builds the packets' static schedules and whose
 * second runs them on the lean path with data that must decline them
 * there: dt_program runs it; the setups give each its registers. */
static void dt_program(const uint32_t *code, unsigned n,
                       void (*setup)(CdjC674x *), unsigned steps)
{
    static System sa, sb;
    static CdjC674x a, b;
    memset(&sa, 0, sizeof sa);
    memcpy(sa.ram, code, n * 4);
    sb = sa;
    ++test_epoch;
    rng_state = 12345;
    for (unsigned side = 0; side < 2; ++side) {
        CdjC674x *cpu = side ? &b : &a;
        cdj_c674x_reset(cpu, BASE);
        cpu->cycle_tick = sys_tick;
        cpu->cycle_opaque = side ? &sb : &sa;
        setup(cpu);
    }
    JitPair p = {&a, &b, &sa, &sb, 0, 0};
    count_sys = NULL;
    bool pre_done = false;
    while (p.step < steps) {
        if (!pre_done) present(&p);
        pre_done = false;
        if (a.packets == b.packets) {
            unsigned status;
            unsigned m = cdj_c674x_run(&a, sys_read, sys_write, &sa, 64,
                                       jit_between, &p, &status);
            if (getenv("DTDBG")) fprintf(stderr, "run pc %#x -> %u status %u (fault %s idle %u loop %d)\n", a.pc, m, status, a.fault ? a.fault : "-", a.idle_cycles, a.loop_active);
            if (status == CDJ_C674X_RUN_FAULT) {
                step_b(&p, false);
                if (getenv("DTDBG")) fprintf(stderr, "dt_program: run fault %s at step %u\n", a.fault, p.step);
                return;
            }
            if (status == CDJ_C674X_RUN_BETWEEN) {
                catch_up_compare(&p);
                pre_done = true;
                continue;
            }
            if (status == CDJ_C674X_RUN_STOPPED) continue;
            if (m) {
                step_b(&p, true);
                continue;
            }
        }
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        step_b(&p, ra);
        if (!ra) { if (getenv("DTDBG")) fprintf(stderr, "dt_program: step fault %s at step %u faucr %x\n", a.fault, p.step, a.control[19]); return; }
    }
}

/* A load lands in A3 as a predicated MVK writes A3: the first passes (B0
 * = 0) fill the packet cache and build the MVK's schedule, the third (B0 =
 * 1) must refuse it on the lean path (the E1/E5 conflict execute_packet
 * faults on). */
static void setup_landing(CdjC674x *cpu)
{
    cpu->r[0][10] = 0x1800;
    cpu->r[1][0] = 0;
    cpu->r[1][1] = 2;       /* passes before the data changes: one fills
                               the packet cache, one builds the schedules */
}

/* CMPGTSP sets FAUCR on a NaN in parallel with a load that faults on the
 * third pass (A10 made unaligned, A16 made a NaN, after the second): the
 * lean path must put FAUCR back when it declines. */
static void setup_faucr(CdjC674x *cpu)
{
    cpu->r[0][10] = 0x1800;
    cpu->r[0][16] = 0x3f800000u;               /* 1.0 */
    cpu->r[0][17] = 0x40000000u;               /* 2.0 */
    cpu->r[1][1] = 2;
}

/* MVC to CSR/IER is traced in place: a packet that also reads what it
 * writes, or writes one twice (the interpreter's fault), must not be. */
static void setup_ctl(CdjC674x *cpu)
{
    cpu->r[1][4] = 0x30;                /* IER: INT4/INT5 enabled */
    cpu->r[1][6] = 0x2;                 /* CSR: PGIE (ITSR.GIE) */
    cpu->r[1][0] = 0;
    cpu->r[1][1] = 2;                   /* passes before B0 is set */
}

static void dt_directed_ctl(void)
{
    const uint32_t branch_back = (uint32_t)((int32_t)-0 & 0x1fffff) << 7 | 0x10;
    const uint32_t reads[] = {
        0,                                              /* NOP: a run starts */
        4u << 23 | 4u << 18 | 0x3a2 | 1,                /* MVC B4,IER */
        5u << 23 | 4u << 18 | 0x3e2,                    /* || MVC IER,B5 */
        1u << 23 | 6u << 18 | 0x3a2 | 1,                /* MVC B6,CSR */
        7u << 23 | 27u << 18 | 0x3e2,                   /* || MVC ITSR,B7 */
        1u << 23 | 6u << 18 | 0x3a2 | 1,                /* MVC B6,CSR */
        8u << 23 | 1u << 18 | 0x3e2,                    /* || MVC CSR,B8 */
        0x7ffu << 7 | 0x28 | 2 | 1u << 23,              /* MVK 0x7ff,B1 */
        4u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B4 */
        6u << 23 | 1u << 7 | 0x50 | 2,                  /* ADDK 1,B6 */
        (uint32_t)(-8 & 0x1fffff) << 7 | 0x10,          /* B .S1 0x1000 */
        4u << 13,                                       /* NOP 5 */
    };
    dt_program(reads, 12, setup_ctl, 120);
    /* Writing IER twice faults in the interpreter: the first passes (B0 =
     * 0) fill the cache and plan the packet, the third must not trace it. */
    const uint32_t twice[] = {
        0,
        1u << 29 | 4u << 23 | 4u << 18 | 0x3a2 | 1,     /* [B0] MVC B4,IER */
        1u << 29 | 4u << 23 | 6u << 18 | 0x3a2,         /* || [B0] MVC B6,IER */
        2u << 29 | 1u << 28 | 0u << 23 | 1u << 7 | 0x28 | 2, /* [!B1] MVK 1,B0 */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        branch_back,
        4u << 13,
    };
    dt_program(twice, 7, setup_ctl, 200);
}

/* Self-modifying code through the write callback (no direct stores):
 * each pass stores the next ADDK constant into an instruction the same pass
 * then runs, inside horizons that skip between(), so only the code check
 * after a callback commit keeps the run on the new bytes. */
static void setup_smc(CdjC674x *cpu)
{
    cpu->r[1][5] = 3u << 23 | 1u << 7 | 0x50;  /* the target, k = 1 */
    cpu->r[1][10] = 0x1040;
    cpu->r[1][1] = 100;
}

static bool never_between(void *opaque)
{
    (void)opaque;
    assert(!"between() inside an endless horizon");
    return false;
}

static void dt_directed_smc(void)
{
    /* The target, alone in its fetch block, is rewritten every other
     * pass (B2 toggles): a pass that leaves it as it was checks it in the
     * run, the next rewrites and runs it again in the same run. */
    const uint32_t code[] = {
        0, 0, 0, 0, 0, 0, 0, 0,                         /* 1000 */
        5u << 23 | 10u << 18 | 1u << 9 | 1u << 7 | 7u << 4 | 4 | 2, /* 1020 STW B5,*B10 */
        3u << 29 | 5u << 23 | 128u << 7 | 0x50 | 2,     /* [B2] ADDK 128,B5 */
        3u << 29 | 2u << 23 | 0u << 7 | 0x28 | 2 | 1,   /* [B2] MVK 0,B2 */
        3u << 29 | 1u << 28 | 2u << 23 | 1u << 7 | 0x28 | 2 | 1, /* || [!B2] MVK 1,B2 */
        9u << 23 | 10u << 18 | 0x3e2,                   /* || MVC TSCL,B9: the
                                                           interpreter's, and
                                                           the store commits
                                                           in it */
        8u << 7 | 0x10,                                 /* B .S1 0x1040 */
        4u << 13,                                       /* NOP 5 */
        0,
        3u << 23 | 1u << 7 | 0x50,                      /* 1040 ADDK k,A3 (target) */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        2u << 29 | (uint32_t)(-8 & 0x1fffff) << 7 | 0x10, /* [B1] B .S1 0x1020 */
        4u << 13,                                       /* NOP 5 */
    };
    static System sa, sb;
    static CdjC674x a, b;
    bool direct = test_direct;
    for (unsigned mode = 0; mode < 8; ++mode) {
        memset(&sa, 0, sizeof sa);
        memcpy(sa.ram, code, sizeof code);
        sb = sa;
        ++test_epoch;
        for (unsigned side = 0; side < 2; ++side) {
            CdjC674x *cpu = side ? &b : &a;
            cdj_c674x_reset(cpu, BASE);
            cpu->cycle_tick = sys_tick;
            cpu->cycle_opaque = side ? &sb : &sa;
            setup_smc(cpu);
        }
        test_direct = mode & 1;
        cdj_c674x_set_code_writes(mode & 2 ? &test_code_writes : NULL);
        cdj_c674x_set_run_steps(mode & 4);
        count_sys = NULL;
        /* A horizon without end: between() never runs inside a run. */
        cdj_c674x_set_horizon(current_horizon = &test_horizon);
        test_horizon.until = UINT64_MAX;
        test_horizon.break_pc = 0;
        event_mask = 0;
        JitPair p = {&a, &b, &sa, &sb, 0, 0};
        while (p.step < 1500 && !a.fault) {
            unsigned status;
            unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, 64,
                                       never_between, NULL, &status);
            if (status == CDJ_C674X_RUN_FAULT) {
                catch_up(&p);
                step_b(&p, false);
                break;
            }
            if (status == CDJ_C674X_RUN_BETWEEN) {
                catch_up_compare(&p);
                continue;
            }
            if (n) {                    /* the limit: the last packet's */
                step_b(&p, true);
                continue;
            }
            bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
            step_b(&p, ra);
            if (!ra) break;
        }
        assert(a.r[0][3] && p.step >= 1500);
    }
    test_horizon.until = 0;
    cdj_c674x_set_horizon(current_horizon = NULL);
    cdj_c674x_set_run_steps(false);
    test_direct = direct;
}

/* The compact stack transfers on the lean path: a Dpp pair turned
 * unaligned after its schedule was built, and a Dpp writing B15 in parallel
 * with a (predicated) ADDK to it (the interpreter's faults; the lean path
 * must decline both). */
static void setup_dpp(CdjC674x *cpu)
{
    cpu->r[1][15] = 0x1c00;
    cpu->r[1][0] = 0;
    cpu->r[1][1] = 2;
}

static void dt_directed_dpp(void)
{
    const uint32_t unaligned[] = {
        (0x0077u | 1u << 15 | 1u << 14 | 4u << 7) |            /* LDDW pop */
        (uint32_t)(0x0077u | 1u << 15 | 4u << 7) << 16,         /* STDW push */
        2u << 29 | 1u << 28 | 15u << 23 | 4u << 7 | 0x50 | 2,   /* [!B1] ADDK 4,B15 */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,                     /* ADDK -1,B1 */
        0x10,                                                   /* B .S1 0x1000 */
        4u << 13,                                               /* NOP 5 */
        0, 0,
        0xe0000000u | 1u << 21,                                 /* header */
    };
    dt_program(unaligned, 8, setup_dpp, 200);
    const uint32_t twice[] = {
        1u << 29 | 15u << 23 | 8u << 7 | 0x50 | 2 | 1,          /* [B0] ADDK 8,B15 */
        (0x0077u | 4u << 7) | 0x0c6eu << 16,                    /* || STW push; NOP */
        2u << 29 | 1u << 28 | 0u << 23 | 1u << 7 | 0x28 | 2,    /* [!B1] MVK 1,B0 */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,                     /* ADDK -1,B1 */
        0x10,                                                   /* B .S1 0x1000 */
        4u << 13,                                               /* NOP 5 */
        0,
        0xe0000000u | 1u << 22,                                 /* header */
    };
    dt_program(twice, 8, setup_dpp, 200);
}

/* A loop whose ADDK between() keeps flipping between two constants (an
 * upload rewriting code): both encodings, and so the loop's other packets
 * in the same fetch block with each, run often enough to be compiled ahead
 * of time, and a region chaining into the flipped packet must notice the
 * new bytes (aot_ready). */
static void setup_toggle(CdjC674x *cpu)
{
    cpu->r[1][1] = 1000;
}

static void dt_directed_toggle(void)
{
    /* The flipped word is in the second fetch block: a region entered in
     * the first (whose bytes never change) reaches it by chaining. */
    const uint32_t branch_back = (uint32_t)((int32_t)-8 & 0x1fffff) << 7 | 0x10;
    toggle_word[0] = 3u << 23 | 1u << 7 | 0x50;        /* ADDK 1,A3 */
    toggle_word[1] = 3u << 23 | 2u << 7 | 0x50;        /* ADDK 2,A3 */
    const uint32_t code[] = {
        0,                                              /* 1000 NOP */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        0, 0, 0, 0, 0, 0,                               /* NOPs */
        toggle_word[0],                                 /* 1020 */
        branch_back,                                    /* B .S1 0x1000 */
        4u << 13,                                       /* NOP 5 */
    };
    toggle_at = BASE + 0x20;
    dt_program(code, 11, setup_toggle, 600);
    toggle_at = 0;
}

static void dt_directed_static(void)
{
    const uint32_t branch_back = (uint32_t)((int32_t)-0 & 0x1fffff) << 7 | 0x10;
    const uint32_t landing[] = {
        0,                                              /* NOP */
        3u << 23 | 10u << 18 | 1u << 9 | 6u << 4 | 4,   /* LDW *+A10[0],A3 */
        2u << 13,                                       /* NOP 3 */
        1u << 29 | 3u << 23 | 5u << 7 | 0x28,           /* [B0] MVK 5,A3 */
        2u << 29 | 1u << 28 | 0u << 23 | 1u << 7 | 0x28 | 2, /* [!B1] MVK 1,B0 */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        branch_back,                                    /* B .S1 0x1000 */
        4u << 13,                                       /* NOP 5 */
    };
    dt_program(landing, 8, setup_landing, 200);
    const uint32_t faucr[] = {
        0,                                              /* NOP: a run starts
                                                           here, so the next
                                                           packet is traced
                                                           inside it */
        6u << 23 | 17u << 18 | 16u << 13 | 0x39u << 6 | 0x20 | 1, /* CMPGTSP */
        7u << 23 | 10u << 18 | 1u << 9 | 6u << 4 | 4,   /* || LDW *+A10[0],A7 */
        4u << 13,                                       /* NOP 5 */
        2u << 29 | 1u << 28 | 16u << 23 | 0x7fc0u << 7 | 0x68, /* [!B1] MVKH */
        2u << 29 | 1u << 28 | 10u << 23 | 1u << 7 | 0x50,      /* [!B1] ADDK 1,A10 */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        branch_back,
        4u << 13,
    };
    /* SADD saturates on the third pass and then appends a delayed SAT
     * effect its schedule was not built with. */
    const uint32_t sat[] = {
        0,                                              /* NOP */
        6u << 23 | 17u << 18 | 16u << 13 | 0x278,       /* SADD A16,A17,A6 */
        4u << 13,                                       /* NOP 5 */
        2u << 29 | 1u << 28 | 16u << 23 | 0xffffu << 7 | 0x28, /* [!B1] MVK -1 */
        2u << 29 | 1u << 28 | 16u << 23 | 0x7fffu << 7 | 0x68, /* [!B1] MVKH */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        branch_back,
        4u << 13,
    };
    dt_program(sat, 8, setup_faucr, 200);
    /* Two writes to A3 in one packet once B0 is set (the interpreter's
     * "parallel register write conflict"): a compiled form must not take
     * the third pass. */
    const uint32_t twice[] = {
        0,                                              /* NOP */
        1u << 29 | 3u << 23 | 1u << 7 | 0x28 | 1,       /* [B0] MVK 1,A3 */
        3u << 23 | 2u << 7 | 0x28,                      /* || MVK 2,A3 */
        2u << 29 | 1u << 28 | 0u << 23 | 1u << 7 | 0x28 | 2, /* [!B1] MVK 1,B0 */
        1u << 23 | 0xffffu << 7 | 0x50 | 2,             /* ADDK -1,B1 */
        branch_back,
        4u << 13,
        0,
    };
    dt_program(twice, 8, setup_landing, 200);
    CdjC674xJitStats f0, f1;
    cdj_c674x_jit_stats(&f0);
    dt_program(faucr, 9, setup_faucr, 200);
    cdj_c674x_jit_stats(&f1);
    if (getenv("DTDBG")) fprintf(stderr, "faucr program: direct %llu plans %llu untraceable %llu lean %llu hits %llu builds %llu\n", (unsigned long long)(f1.direct - f0.direct), (unsigned long long)(f1.direct_plans - f0.direct_plans), (unsigned long long)(f1.direct_untraceable - f0.direct_untraceable),
        (unsigned long long)(f1.static_lean - f0.static_lean), (unsigned long long)(f1.static_hits - f0.static_hits), (unsigned long long)(f1.static_builds - f0.static_builds));
}

static bool always(void *opaque) { (void)opaque; return true; }

/* Directed: a CMPEQDP's delayed FAUCR effect lands in the cycle a CMPGTSP
 * writes FAUCR status (both on NaN): the interpreter refuses the second
 * packet, so the trace must decline it. */
static void dt_directed_faucr(void)
{
    static System sa, sb, sc;
    static CdjC674x a, b, c;
    memset(&sa, 0, sizeof sa);
    uint32_t code[8] = {
        8u << 23 | 4u << 18 | 2u << 13 | 0xa20,                  /* CMPEQDP */
        9u << 23 | 2u << 18 | 3u << 13 | 0x39u << 6 | 0x20,      /* CMPGTSP */
        4u << 13,                                                 /* NOP 5 */
    };
    memcpy(sa.ram, code, sizeof code);
    sb = sc = sa;
    CdjC674x *cpus[3] = {&a, &b, &c};
    System *systems[3] = {&sa, &sb, &sc};
    for (unsigned i = 0; i < 3; ++i) {
        cdj_c674x_reset(cpus[i], BASE);
        for (unsigned r = 2; r <= 5; ++r) cpus[i]->r[0][r] = 0x7ff80000u;
        cpus[i]->cycle_tick = sys_tick;
        cpus[i]->cycle_opaque = systems[i];
    }
    /* Fill the packet cache with both packets. */
    assert(cdj_c674x_step(&c, sys_read, sys_write, &sc));
    assert(!cdj_c674x_step(&c, sys_read, sys_write, &sc));
    unsigned status;
    unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, 2, always, NULL,
                               &status);
    assert(n == 1 && status == CDJ_C674X_RUN_BETWEEN);
    bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
    cdj_c674x_set_packet_cache(0);
    bool rb = cdj_c674x_step(&b, sys_read, sys_write, &sb) &&
              cdj_c674x_step(&b, sys_read, sys_write, &sb);
    cdj_c674x_set_packet_cache(2);
    assert(!ra && !rb && !strcmp(a.fault, "delayed FP-status write conflict"));
    same(&a, &sa, &b, &sb, 0, 3);
}

int main(void)
{
    cdj_c674x_set_fetch_block(sys_read, sys_block);
    cdj_c674x_set_fetch_epoch(&test_epoch);
    cdj_c674x_set_ram_window(sys_write, sys_window, &test_direct);
    self_modifying();
    for (unsigned seed = 1; seed <= 3000; ++seed) lockstep(seed, false);
    printf("packet cache lockstep: 3000 programs, %u packets, %u faults\n",
           packets, faults_seen);
    assert(packets > 1000000 && faults_seen > 100);
    packets = faults_seen = 0;
    for (unsigned seed = 1; seed <= 3000; ++seed) lockstep(seed, true);
    printf("SPLOOP lockstep: 3000 programs, %u packets, %u loop-buffer steps,"
           " %u faults\n", packets, loop_steps, faults_seen);
    assert(loop_steps > 300000 && faults_seen > 100);
    faults_seen = 0;
    cdj_c674x_set_jit(1);
    for (unsigned seed = 1; seed <= 3000; ++seed) {
        cdj_c674x_set_horizon(current_horizon = seed % 5 ? &test_horizon : NULL);
        assert(!test_horizon.skipped);
        test_horizon.until = 0;
        test_direct = seed % 4 < 2;
        cdj_c674x_set_code_writes(seed % 3 == 1 ? &test_code_writes : NULL);
        cdj_c674x_set_run_steps(seed % 7 < 3);
        jit_lockstep(seed);
    }
    CdjC674xJitStats stats;
    cdj_c674x_jit_stats(&stats);
    cdj_c674x_loop_set_functional_timing(false);
    printf("JIT lockstep: 3000 programs, %u compiled packets in %u runs "
           "(%u between exits, %u stops; %llu native, %llu generic, "
           "%llu steady), %u loop interrupts, %u faults\n",
           jit_packets, jit_runs, jit_between_exits, jit_stops,
           (unsigned long long)stats.native,
           (unsigned long long)stats.generic,
           (unsigned long long)stats.steady, jit_armed, faults_seen);
    assert(jit_packets > 300000 && jit_armed > 300 && faults_seen > 100 &&
           stats.native > 300000 && stats.generic > 10000 &&
           stats.steady > 100000);
    jit_packets = faults_seen = 0;
    /* PCTEST_AOT_PROFILE=path: these programs' hot lean packets for
     * tools/cdj_dsp/aot_gen.py; a build with CDJ_C674X_AOT_FILE and
     * CDJ_C674X_AOT=1 then runs the same programs through them. */
    const char *aot_profile = getenv("PCTEST_AOT_PROFILE");
    FILE *aot_file = aot_profile ? fopen(aot_profile, "w") : NULL;
    if (aot_file) {
        cdj_c674x_aot_profile(true);
        cdj_c674x_aot_profile_to(aot_file, 1);
    }
    dt_directed();
    dt_directed_faucr();
    dt_directed_ctl();
    dt_directed_smc();
    {
        CdjC674xJitStats s0, s1;
        cdj_c674x_jit_stats(&s0);
        /* Repeated, so their packets are hot enough to compile ahead of
         * time (PCTEST_AOT_PROFILE). */
        for (unsigned h = 0; h < 80; ++h) {
            cdj_c674x_set_horizon(current_horizon = h % 2 ? &test_horizon : NULL);
            test_horizon.until = 0;
            dt_directed_static();
            dt_directed_dpp();
            dt_directed_toggle();
        }
        cdj_c674x_set_horizon(current_horizon = NULL);
        cdj_c674x_jit_stats(&s1);
        assert(s1.static_lean > s0.static_lean);
    }
    for (unsigned seed = 1; seed <= 24000; ++seed) {
        cdj_c674x_set_horizon(current_horizon = seed % 5 ? &test_horizon : NULL);
        assert(!test_horizon.skipped);
        test_horizon.until = 0;
        test_direct = seed % 4 < 2;
        cdj_c674x_set_code_writes(seed % 3 == 1 ? &test_code_writes : NULL);
        cdj_c674x_set_run_steps(seed % 7 < 3);
        direct_programs = true;
        dt_lockstep(seed);
        direct_programs = false;
    }
    cdj_c674x_set_horizon(current_horizon = NULL);
    cdj_c674x_set_run_steps(false);
    cdj_c674x_loop_set_functional_timing(false);
    CdjC674xJitStats after;
    cdj_c674x_jit_stats(&after);
    if (aot_file) {
        cdj_c674x_aot_profile_dump(aot_file, 1);
        cdj_c674x_aot_profile_to(NULL, 0);
        cdj_c674x_aot_profile(false);
        fclose(aot_file);
    }
    if (after.aot) {
        printf("ahead-of-time regions: %llu packets; exits %llu declined, "
               "%llu limit, %llu between, %llu cont, %llu fault, %llu redo\n",
               (unsigned long long)after.aot,
               (unsigned long long)after.aot_exit[0],
               (unsigned long long)after.aot_exit[1],
               (unsigned long long)after.aot_exit[2],
               (unsigned long long)after.aot_exit[3],
               (unsigned long long)after.aot_exit[4],
               (unsigned long long)after.aot_exit[5]);
        assert(after.aot > 2000000 && after.aot_exit[0] > 20 &&
               after.aot_exit[1] > 10000 && after.aot_exit[2] > 100000 &&
               after.aot_exit[3] > 10000 && after.aot_exit[4] > 0 &&
               after.aot_exit[5] > 20);
    }
    printf("direct-trace lockstep: 24000 programs, %llu traced packets in "
           "%llu runs (%u with a packet), %llu plans, %llu untraceable, %u faults\n",
           (unsigned long long)(after.direct - stats.direct),
           (unsigned long long)(after.direct_runs - stats.direct_runs), dt_runs,
           (unsigned long long)(after.direct_plans - stats.direct_plans),
           (unsigned long long)(after.direct_untraceable -
                                stats.direct_untraceable), faults_seen);
    /* (Compact packets end programs sooner: their random forms fault.) */
    assert(after.direct - stats.direct > 2300000 && faults_seen > 1000);
    printf("static schedules: %llu packets from one (%llu lean), %llu built, %llu fitted none\n",
           (unsigned long long)(after.static_hits + after.static_lean),
           (unsigned long long)after.static_lean,
           (unsigned long long)after.static_builds,
           (unsigned long long)after.static_misses);
    assert(after.static_hits + after.static_lean > 2000000 &&
           after.static_lean > 1000000);
    printf("interrupt presentations: %u quiet (checked no-op), %u not\n",
           quiet_checks, loud_checks);
    assert(quiet_checks > 1000000 && loud_checks > 10000);
    printf("board horizon: %u between() calls skipped\n", horizon_skips);
    assert(horizon_skips > 1000000);
    return 0;
}
