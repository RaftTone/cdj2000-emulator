/* SPDX-License-Identifier: GPL-2.0-or-later
 * The C6727 core is strict C67x+ (c66x_set_isa): the two C64x "mvk .L" words
 * gas emitted for "zero .L1" in NEW FIRMWARE 1.2-test15 (they hung a real
 * deck) stop the core at their PC, and their C67x+ forms ("sub .L1") run.
 */
#include <stdio.h>
#include <string.h>
#include "c66x.h"

#define BASE 0x10000000u
static uint8_t ram[4096];

static uint32_t bus_read(void *o, uint32_t a, unsigned s) { (void)o; (void)a; (void)s; return 0; }
static void bus_write(void *o, uint32_t a, uint32_t v, unsigned s) { (void)o; (void)a; (void)v; (void)s; }

static int failed;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failed = 1; } } while (0)

/* Run word (one execute packet, then NOPs) with A1 = A6 = 0x1234, A2 = 0. */
static c66x_stop run(uint32_t word, unsigned isa, c66x_core **out)
{
    memset(ram, 0, sizeof ram);             /* 0 = nop */
    memcpy(ram + 0x20, &word, 4);
    c66x_bus bus = { NULL, bus_read, bus_write };
    c66x_core *c = c66x_new(&bus);
    c66x_set_isa(c, isa);
    c66x_map_ram(c, BASE, sizeof ram, ram);
    c66x_reset(c, BASE + 0x20);
    c66x_set_reg(c, 1, 0x1234);
    c66x_set_reg(c, 6, 0x1234);
    c66x_set_reg(c, 2, 0);
    uint64_t done = 0;
    c66x_stop stop = c66x_step(c, 8, &done);
    *out = c;
    return stop;
}

int main(void)
{
    static const struct { uint32_t bad, good; unsigned reg; } t[] = {
        { 0xB080A358, 0xB08000F8, 1 },      /* [!a2] mvk .L1 0,a1 / [!a2] sub .L1 a0,a0,a1 */
        { 0x0300A358, 0x030000F8, 6 },      /* mvk .L1 0,a6 / sub .L1 a0,a0,a6 */
    };
    for (unsigned i = 0; i < 2; i++) {
        c66x_core *c;
        c66x_stop s = run(t[i].bad, C66X_ISA_C67XP, &c);
        CHECK(s == C66X_STOP_UNDEF && c66x_trap_pc(c) == BASE + 0x20,
              "0x%08x: stop %d at 0x%08x, want undefined at 0x%08x",
              t[i].bad, s, c66x_trap_pc(c), BASE + 0x20);
        CHECK(c66x_get_reg(c, t[i].reg) == 0x1234, "0x%08x ran under C67x+", t[i].bad);
        c66x_free(c);

        s = run(t[i].good, C66X_ISA_C67XP, &c);
        CHECK(s == C66X_STOP_BUDGET && c66x_get_reg(c, t[i].reg) == 0,
              "0x%08x: stop %d, A%u = 0x%x, want budget and 0", t[i].good, s, t[i].reg,
              c66x_get_reg(c, t[i].reg));
        c66x_free(c);

        /* Without the restriction the core is a C66x and runs the C64x form. */
        s = run(t[i].bad, 0, &c);
        CHECK(s == C66X_STOP_BUDGET && c66x_get_reg(c, t[i].reg) == 0,
              "0x%08x as C66x: stop %d, A%u = 0x%x", t[i].bad, s, t[i].reg,
              c66x_get_reg(c, t[i].reg));
        c66x_free(c);
    }
    if (!failed)
        printf("ok\n");
    return failed;
}
