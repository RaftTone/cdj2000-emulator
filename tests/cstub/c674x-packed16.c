/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed 16-bit arithmetic, packed compares and packed shifts.
 *
 * Every expected value below is TRANSCRIBED from the "N cycles after
 * instruction" block of the named SPRUFE8B (July 2010) printed page, in the
 * manual's own hex, except the two cases marked DERIVED, which say where
 * their value comes from.  Nothing here was produced by running this
 * emulator.  Every instruction word was produced by assembling the manual's
 * own example line with ti-cgt-c6000 8.5.0 asm6x -mv6740, so the operand
 * slots are the assembler's, not ours.
 *
 * Printed pages: ABS2 103-104, ADD2 137-139, SUB2 548-550, SADD2 425-426,
 * SSUB2 502-503, SADDSU2 431-432, SADDUS2 433-434, MAX2 306-308,
 * MIN2 311-313, AVG2 147-148, SHR2 453-454, SHRU2 459-460, CMPEQ2 179-180,
 * CMPGT2 191-192, CMPLT2 205-206, SPACK2 472-473, SSHVL 495-496,
 * SSHVR 497-498.  RPACK2 (416-417) is deliberately NOT covered: it is a
 * nonconditional encoding and still reports "instruction not implemented",
 * which this file asserts.  */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static void issue(CdjC674x *c, uint32_t word)
{
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = c->pc}},
        .count = 1, .next_pc = c->pc + 4,
    };
    if (!cdj_c674x_execute(c, &p, NULL, NULL, NULL)) {
        fprintf(stderr, "packed16 fault word=%08x: %s\n", word, c->fault);
        assert(false);
    }
}

struct Case {
    const char *note;
    uint32_t word;      /* asm6x -mv6740 encoding of the manual's own line */
    uint32_t src1;      /* value the manual puts in the src1 register */
    uint32_t src2;      /* value the manual puts in the src2 register */
    uint32_t expected;  /* the manual's "N cycles after instruction" dst */
    unsigned cycles;    /* 1 for single-cycle, 2 for the .M two-cycle forms */
    unsigned ssr;       /* SSR bit mask the manual shows set, 0 for none */
};

static const struct Case cases[] = {
/* ABS2 .L1 A0,A2 - p104 ex1 and ex2.  src1 (bits 17-13) is part of the
 * opcode here, so the src1 column is unused. */
{"ABS2 p104 ex1",      0x01008358, 0, 0xff684e3d, 0x00984e3d, 1, 0},
{"ABS2 p104 ex2",      0x01008358, 0, 0x3ff6f105, 0x3ff60efb, 1, 0},
/* ADD2 .S1X A1,B1,A2 - p138 ex1; ADD2 .L1 A0,A1,A2 - p139 ex2. */
{"ADD2 p138 ex1 .S",   0x01043060, 0x002137e1, 0x039ae4b8, 0x03bb1c99, 1, 0},
{"ADD2 p139 ex2 .L",   0x010400b8, 0x002137e1, 0x039ae4b8, 0x03bb1c99, 1, 0},
/* ADD2 .D1 A4,A6,A5 - DERIVED only in the sense that the manual prints no
 * .D example: the values and the result are p139 ex2's, and p138's Pipeline
 * row "Unit in use .S, .L, .D" plus the single Execution block make the .D
 * opfield the same operation.  No arithmetic was re-derived. */
{"ADD2 p137 .D form",  0x02988930, 0x002137e1, 0x039ae4b8, 0x03bb1c99, 1, 0},
/* SUB2 - p550 ex1 (.S1), ex2 (.D2) and ex3 (.S2X). */
{"SUB2 p550 ex1 .S",   0x02906460, 0x11056e30, 0x11056980, 0x000004b0, 1, 0},
{"SUB2 p550 ex2 .D",   0x07a04972, 0xf23a3789, 0x04b86732, 0xed82d057, 1, 0},
{"SUB2 p550 ex3 .SX",  0x01003462, 0x003a1b48, 0x00213271, 0x0019e8d7, 1, 0},
/* SUB2 .L1 A0,A1,A2 - DERIVED only in the same sense: p549's Pipeline row
 * "Unit in use .L, .S, .D" and single Execution block, with p550 ex1's
 * values and result carried onto the .L opfield. */
{"SUB2 p548 .L form",  0x01040098, 0x11056e30, 0x11056980, 0x000004b0, 1, 0},
/* SADD2 .S1 A2,A8,A9 - p426 ex1; SADD2 .S2 B2,B8,B12 - p426 ex2. */
{"SADD2 p426 ex1",     0x04a04c30, 0x5789f23a, 0x74b84975, 0x7fff3baf, 1, 0},
{"SADD2 p426 ex2",     0x06204c32, 0x0124847c, 0x01a6a051, 0x02ca8000, 1, 0},
/* SSUB2 .L1 A0,A1,A2 - p503 ex1 and ex2. */
{"SSUB2 p503 ex1",     0x01040c98, 0x00070005, 0xffffffff, 0x00080006, 1, 0},
{"SSUB2 p503 ex2",     0x01040c98, 0x00070005, 0x8000ffff, 0x7fff0006, 1, 0},
/* SADDUS2 .S1 A2,A8,A9 - p434 ex1; SADDUS2 .S2 B2,B8,B12 - p434 ex2.
 * SADDSU2 (p431) is the same encoding with the operands written the other
 * way round, which is what the assembler emitted for both spellings. */
{"SADDUS2 p434 ex1",   0x04a04c70, 0x5789f23a, 0x74b84975, 0xcc41ffff, 1, 0},
{"SADDUS2 p434 ex2",   0x06204c72, 0x147c0124, 0xa05101a6, 0x000002ca, 1, 0},
/* MAX2 - p307 ex1/ex2 (.L) and p308 ex3/ex4 (.S).  In the X forms asm6x put
 * the cross operand in the src2 slot, so src1 carries the B-file value. */
{"MAX2 p307 ex1 .L",   0x04a04858, 0x3789f23a, 0x04b84975, 0x37894975, 1, 0},
{"MAX2 p307 ex2 .LX",  0x0609185a, 0x01a6a051, 0x01242451, 0x01a62451, 1, 0},
{"MAX2 p308 ex3 .S",   0x04a04f70, 0x3789f23a, 0x04b84975, 0x37894975, 1, 0},
{"MAX2 p308 ex4 .SX",  0x06091f72, 0x01a6a051, 0x01242451, 0x01a62451, 1, 0},
/* MIN2 - p312 ex1/ex2 (.L) and p313 ex3/ex4 (.S). */
{"MIN2 p312 ex1 .L",   0x04a04838, 0x3789f23a, 0x04b84975, 0x04b8f23a, 1, 0},
{"MIN2 p312 ex2 .LX",  0x0609183a, 0x0a378001, 0x01248003, 0x01248001, 1, 0},
{"MIN2 p313 ex3 .S",   0x04a04f30, 0x3789f23a, 0x04b84975, 0x04b8f23a, 1, 0},
{"MIN2 p313 ex4 .SX",  0x06091f32, 0x0a378001, 0x01248003, 0x01248001, 1, 0},
/* AVG2 .M1 A0,A1,A2 - p148, "2 cycles after instruction". */
{"AVG2 p148 ex",       0x010404f0, 0x61984357, 0x7582ae15, 0x6b8df8b6, 2, 0},
/* AVG2 DERIVED.  The only printed AVG2 example cannot see the "+1": both of
 * its halfword sums are even, so (s + 1) >> 1 and s >> 1 agree.  This case is
 * computed from p147's Execution block, "((lsb16(src1) + lsb16(src2) + 1) >>
 * 1) -> lsb16(dst); ((msb16(src1) + msb16(src2) + 1) >> 1) -> msb16(dst)":
 * lsb (1 + 0 + 1) >> 1 = 1, msb (0 + 0 + 1) >> 1 = 0, so dst = 0000 0001h.
 * No printed value contradicts it; the manual simply prints none. */
{"AVG2 p147 derived",  0x010404f0, 0x00000001, 0x00000000, 0x00000001, 2, 0},
/* SHR2 - p454 ex1 (register count) and ex2 (the ucst5 form, shift 15).
 * In ex2 src1 is the constant 0fh, so the src1 column is unused. */
{"SHR2 p454 ex1",      0x02888df2, 0x14583b69, 0xa6e2c179, 0xffd3ffe0, 1, 0},
{"SHR2 p454 ex2 cst",  0x0291e620, 0, 0x000a87af, 0x0000ffff, 1, 0},
/* SHRU2 - p460 ex1 (register count) and ex2 (ucst5, shift 15). */
{"SHRU2 p460 ex1",     0x02888e32, 0x14583b69, 0xa6e2c179, 0x00530060, 1, 0},
{"SHRU2 p460 ex2 cst", 0x0291e660, 0, 0x000a87af, 0x00000001, 1, 0},
/* p454 NOTE: "If the shift amount specified in src1 is in the range 16 to 31,
 * the behavior is identical to a shift value of 15."  Both the ucst5 form
 * (1fh and 10h) and the register form (src1 = 1Fh) therefore owe p454 ex2's
 * own printed result, 0000 FFFFh, on p454 ex2's own src2. */
{"SHR2 p454 note 1fh",  0x0293e620, 0, 0x000a87af, 0x0000ffff, 1, 0},
{"SHR2 p454 note 10h",  0x02920620, 0, 0x000a87af, 0x0000ffff, 1, 0},
{"SHR2 p454 note reg",  0x02888df2, 0x0000001f, 0x000a87af, 0x0000ffff, 1, 0},
/* p459 NOTE: "If the shift amount specified in src1 is in the range of 16 to
 * 31, the dst will be cleared to all zeros." */
{"SHRU2 p459 note",     0x0293e660, 0, 0x000a87af, 0x00000000, 1, 0},
{"SHRU2 p459 note reg", 0x02888e32, 0x0000001f, 0x000a87af, 0x00000000, 1, 0},
/* CMPEQ2 - p180 ex1, ex2 and ex3. */
{"CMPEQ2 p180 ex1",    0x02906760, 0x11056e30, 0x11056980, 0x00000002, 1, 0},
{"CMPEQ2 p180 ex2",    0x07a04762, 0xf23a3789, 0x04b83789, 0x00000001, 1, 0},
{"CMPEQ2 p180 ex3",    0x07a04762, 0x01b62451, 0x01b62451, 0x00000003, 1, 0},
/* CMPGT2 - p192 ex1, ex2 and ex3; CMPLT2 p206 ex1 is the same encoding,
 * which is how the assembler renders the pseudo-operation. */
{"CMPGT2 p192 ex1",    0x02906520, 0x11056e30, 0x11056980, 0x00000001, 1, 0},
{"CMPGT2 p192 ex2",    0x07a04522, 0xf3483789, 0x04b84975, 0x00000000, 1, 0},
{"CMPGT2 p192 ex3",    0x07a04522, 0x01a62451, 0x0124a051, 0x00000003, 1, 0},
{"CMPLT2 p206 ex1",    0x02906520, 0x11056e30, 0x11056980, 0x00000001, 1, 0},
/* SPACK2 - p473 ex1 and ex2. */
{"SPACK2 p473 ex1",    0x04a04cb0, 0x3789f23a, 0x04b84975, 0x7fff7fff, 1, 0},
{"SPACK2 p473 ex2",    0x06204cb2, 0xa1242451, 0x01a6a051, 0x80007fff, 1, 0},
/* SSHVL - p496 ex1 (src1 = -31, right shift) and ex2 (src1 = 31, saturating
 * left shift; the manual's "Saturated to most negative value" note, with
 * SSR.M1 = bit 4 per 2.9.13 printed page 54).  p496 ex3 is NOT covered; see
 * the comment at the end of this file. */
{"SSHVL p496 ex1",     0x02888732, 0xffffffe1, 0xfffff000, 0xffffffff, 2, 0},
{"SSHVL p496 ex2",     0x02888730, 0x0000001f, 0xf14c2108, 0x80000000, 2, 0x10},
/* SSHVR - p498 ex1 (src1 = -31, saturating left shift), ex2 (src1 = 31,
 * right shift) and ex3 (src1 = -1, left shift by one).  SSR.M2 = bit 5. */
{"SSHVR p498 ex1",     0x028886b2, 0xffffffe1, 0xfffff000, 0x80000000, 2, 0x20},
{"SSHVR p498 ex2",     0x028886b0, 0x0000001f, 0xf14c2108, 0xffffffff, 2, 0},
{"SSHVR p498 ex3",     0x0cb306b2, 0xffffffff, 0x187a65fc, 0x30f4cbf8, 2, 0},
/* Out-of-range shift counts.  p496's Execution block states the equality
 * directly - "if (src1 > 31), sat(src2 << 31) -> dst; if (src1 < -31),
 * (src2 >> 31) -> dst" - and p498's states the mirror image, so each case
 * below owes the printed result of the matching ex1/ex2 above. */
{"SSHVL p496 src1>31",  0x02888730, 0x00000020, 0xf14c2108, 0x80000000, 2, 0x10},
{"SSHVL p496 src1<-31", 0x02888732, 0xffffffc0, 0xfffff000, 0xffffffff, 2, 0},
{"SSHVR p498 src1>31",  0x028886b0, 0x00000020, 0xf14c2108, 0xffffffff, 2, 0},
{"SSHVR p498 src1<-31", 0x028886b2, 0xffffffc0, 0xfffff000, 0x80000000, 2, 0x20},
};

static void manual_examples(void)
{
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct Case *t = &cases[i];
        unsigned side = (t->word >> 1) & 1;
        unsigned cross = side ^ ((t->word >> 12) & 1);
        unsigned a = (t->word >> 13) & 31, b = (t->word >> 18) & 31;
        unsigned dst = (t->word >> 23) & 31;
        CdjC674x c;
        cdj_c674x_reset(&c, 0x1000);
        c.r[side][a] = t->src1;
        c.r[cross][b] = t->src2;
        c.r[side][dst] = 0xdeadbeef;
        issue(&c, t->word);
        if (t->cycles == 2) {
            /* One delay slot: dst is still the old value in E1. */
            assert(c.r[side][dst] == 0xdeadbeefu);
            assert(c.load_count >= 1 && c.loads[0].due == 2);
            issue(&c, 0);
        }
        if (c.r[side][dst] != t->expected) {
            fprintf(stderr, "%s: dst=%08x expected %08x\n",
                    t->note, c.r[side][dst], t->expected);
            assert(false);
        }
        /* Saturation status lands one cycle after dst (printed page 495
         * NOTE and 2.9.13, printed page 54); nothing before that. */
        assert(!(c.control[1] & 0x200u) && !c.control[21]);
        issue(&c, 0);
        assert((c.control[1] & 0x200u) == (t->ssr ? 0x200u : 0u));
        assert(c.control[21] == t->ssr);
    }
}

static void predication_and_refusals(void)
{
    CdjC674x c;
    /* [!B0] SADD2 .S1 A2,A8,A9 with B0 nonzero writes nothing: creg 001,
     * z = 1 selects B0 and inverts the test (SPRUFE8B Table 3-9). */
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 1; c.r[0][2] = 0x5789f23a; c.r[0][8] = 0x74b84975;
    c.r[0][9] = 0xdeadbeef;
    issue(&c, 0x04a04c30u | 1u << 29 | 1u << 28);
    assert(c.r[0][9] == 0xdeadbeefu);
    /* The same predicate on two-cycle AVG2 queues no delayed result. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 1; c.r[0][0] = 0x61984357; c.r[0][1] = 0x7582ae15;
    c.r[0][2] = 0xdeadbeef;
    issue(&c, 0x010404f0u | 1u << 29 | 1u << 28);
    assert(!c.load_count && c.r[0][2] == 0xdeadbeefu);

    /* RPACK2 .S1 A4,A6,A5 (printed page 416) is a nonconditional encoding
     * reached before the conditional dispatch table, and is not
     * implemented: it must fault rather than produce an invented result. */
    cdj_c674x_reset(&c, 0x1000);
    CdjC674xPacket p = {
        .instructions = {{.word = 0x12988ef0u, .pc = c.pc}},
        .count = 1, .next_pc = c.pc + 4,
    };
    assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(!strcmp(c.fault, "instruction not implemented"));
}

int main(void)
{
    manual_examples();
    predication_and_refusals();
    /* Not covered, deliberately:
     * - SSHVL printed page 496 Example 3 (SSHVL .M2 B12,B24,B25, src2 =
     *   187A 65FCh, src1 = -1) prints B25 = 03CD 32FEh.  The Execution
     *   block on the same page gives src2 >> 1 = 0C3D 32FEh, and SSHVR's
     *   Example 3 on printed page 498 - the same operands, shifted the other
     *   way - agrees with its own pseudocode.  Rather than assert a value we
     *   re-derived against a printed one, the case is left untested; the
     *   right-shift path it would exercise is covered by p496 Example 1.
     * - RPACK2, which fails closed above. */
    puts("C674x packed 16-bit arithmetic, compares and shifts passed");
    return 0;
}
