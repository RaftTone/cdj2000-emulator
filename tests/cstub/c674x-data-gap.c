/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Data loads from the reserved gap above L2 RAM (0x11840000-0x11DFFFFF)
 * complete with zero; stores, fetches and every other unmapped address still
 * fault.  Stock's bit reversal (0xC0030D80) loads one table entry past its
 * table in a branch delay slot and issues an LDDW through it (docs in
 * cdj_c674x.h).  Encodings are TI asm6x output (LDW .D1T1 *A4,A5 = 02900264,
 * NOP 4 = 00006000, STW .D1T1 A5,*A4 = 02900274, LDDW .D1T1 *A4,A7:A6 =
 * 03100364). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static uint32_t code[8];
static bool read_word(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address >= 0x1000 && address < 0x1020 && !(address & 3)) {
        *value = code[(address - 0x1000) / 4];
        return true;
    }
    if (address == 0x2000) { *value = 0x600dcafe; return true; }
    return false;
}
static bool write_word(void *unused, uint32_t a, uint64_t v, unsigned size, bool commit)
{
    (void)unused; (void)v; (void)size; (void)commit;
    return a == 0x2000;
}
static void run(CdjC674x *c, unsigned packets)
{
    for (unsigned i = 0; i < packets; ++i)
        if (!cdj_c674x_step(c, read_word, write_word, NULL)) return;
}

int main(void)
{
    CdjC674x c;
    code[0] = 0x02900264; code[1] = 0x00006000;   /* LDW A5,*A4 ; NOP 4 */
    code[2] = 0x03100364; code[3] = 0x00006000;   /* LDDW A7:A6,*A4 ; NOP 4 */
    code[4] = 0x02900274;                         /* STW A5,*A4 */

    /* Mapped data is untouched by the gap. */
    cdj_c674x_set_data_gap(true);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x2000;
    run(&c, 2);
    assert(!c.fault && c.r[0][5] == 0x600dcafe && !cdj_c674x_data_gap_reads());

    /* Both ends of the gap: LDW and LDDW complete with zero and are counted. */
    static const uint32_t inside[] = {0x11840000, 0x11842740, 0x11dffff8};
    for (unsigned i = 0; i < 3; ++i) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = inside[i]; c.r[0][5] = c.r[0][6] = c.r[0][7] = 0xdeadbeef;
        run(&c, 2);
        assert(!c.fault && c.r[0][5] == 0);
        c.pc = 0x1008;
        run(&c, 2);
        assert(!c.fault && c.r[0][6] == 0 && c.r[0][7] == 0);
    }
    /* One count per gap word a completed load read: 3 x (LDW 1 + LDDW 2). */
    assert(cdj_c674x_data_gap_reads() == 9);

    /* Just outside the gap, a store into it, and strict mode all fault. */
    static const uint32_t outside[] = {0x11e00000, 0x11700000, 0x00000004};
    for (unsigned i = 0; i < 3; ++i) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][4] = outside[i];
        run(&c, 1);
        assert(c.fault && strstr(c.fault, "unmapped"));
    }
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x11842740; c.r[0][5] = 1;
    c.pc = 0x1010;
    run(&c, 1);
    assert(c.fault && strstr(c.fault, "unmapped"));          /* store */
    cdj_c674x_set_data_gap(false);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 0x11842740;
    run(&c, 1);
    assert(c.fault && strstr(c.fault, "unmapped"));          /* strict */
    cdj_c674x_set_data_gap(true);
    cdj_c674x_reset(&c, 0x11842740);
    run(&c, 1);
    assert(c.fault);                                         /* fetch */
    puts("ok");
    return 0;
}
