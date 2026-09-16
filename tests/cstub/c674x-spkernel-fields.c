/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPRUFE8B Figure H-7: compact bit0=field5, bits9:7=field2:0,
 * bits15:14=field4:3. Table3-29 reverses stage bits AFTER field assembly.
 * Compare all field values with separately constructed full-width SPKERNEL. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static uint32_t memory[128];
static bool read_word(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    if (address < 0x1000 || address >= 0x1200 || (address & 3)) return false;
    *value = memory[(address - 0x1000) / 4];
    return true;
}

static uint32_t compact_kernel(unsigned field)
{
    return 0x1c66 | ((field & 32) >> 5) | ((field & 7) << 7) |
           ((field & 24) << 11);
}

static unsigned cycle_bits(unsigned ii)
{
    /* Table3-29 groups:1;2;3-4;5-8;9-16. */
    return ii == 1 ? 0 : ii == 2 ? 1 : ii <= 4 ? 2 : ii <= 8 ? 3 : 4;
}

static unsigned stage_value(unsigned field, unsigned cbits)
{
    /* Reverse all six field bits, then discard cycle bits at the high end. */
    unsigned reversed = ((field & 1) << 5) | ((field & 2) << 3) |
                        ((field & 4) << 1) | ((field & 8) >> 1) |
                        ((field & 16) >> 3) | ((field & 32) >> 5);
    return reversed & ((1u << (6 - cbits)) - 1);
}

static bool decode_schedule(CdjC674x *cpu, unsigned ii, unsigned field,
                            bool compact, unsigned length, bool delayed)
{
    assert(length && length <= 48);
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(cpu, 0x1000);
    cpu->control[13] = 128;
    memory[0] = (ii - 1) << 23 | (delayed ? 0x3a000 : 0x38000);
    /* One scalar NOP per loading cycle; SPKERNEL occupies the last cycle. */
    unsigned kernel_index = length;
    if (compact) {
        memory[kernel_index] = compact_kernel(field);
        unsigned slot = kernel_index & 7;
        /* A compact header replaces a nonexecuted word, never a body NOP. */
        assert(slot != 7);
        memory[(kernel_index & ~7u) + 7] = 0xe0000000u | (1u << (21 + slot));
    } else {
        memory[kernel_index] = field << 22 | 0x34000;
    }
    for (unsigned step = 0; step < length; ++step) {
        if (!cdj_c674x_step(cpu, read_word, NULL, NULL)) {
            fprintf(stderr, "unexpected setup failure ii=%u field=%u: %s\n", ii, field, cpu->fault);
            assert(false);
        }
    }
    assert(cpu->loop.cycle == length - 1 && !cpu->loop.sealed);
    bool ok = cdj_c674x_step(cpu, read_word, NULL, NULL);
    if (!ok) {
        assert(cpu->fault && !strcmp(cpu->fault, "invalid SPKERNEL cycle"));
        assert(cpu->loop.cycle == length - 1 && !cpu->loop.sealed);
        assert(cpu->cycles == length);
    }
    return ok;
}

/* The field decode is checked against an independently constructed full-width
 * SPKERNEL, which is a real oracle. The schedule arithmetic below is derived
 * from the manual rather than from this core's output:
 *
 *   expected (program memory fetch re-enable) - 7.9.4, printed page 686:
 *     "delay = (fstg * ii) + fcyc", and fetch waits until "The draining
 *     counter has reached the delay value specified by fstg and fcyc".
 *     7.7.3.5 (printed page 679) starts the draining counter at 0 "on the
 *     cycle after the loop termination condition is true", which for an
 *     unconditional SPLOOP of ILC iterations is after the last iteration is
 *     launched, at cycle iterations * ii. Hence
 *     iterations * ii + stage * ii + cycle.
 *   loading_end clamp - 7.9.5, printed page 686: when SPKERNEL is not on a
 *     stage boundary "the loop buffer executes until the last loading stage
 *     boundary ... and program memory fetch remains disabled", so fetch
 *     cannot resume before ceil(dynlen / ii) * ii.
 *   drain clamp - 7.9.4's last paragraph, printed page 686: "If the loop
 *     buffer goes to idle (for example, if the epilog is smaller than the
 *     specified delay ...), program memory fetch is enabled and the fetch
 *     enable delay is ignored".
 *   drain (loop idle) - 7.7.3 and 7.7.3.4 (printed pages 678-679) put the
 *     operation at loading offset n on cycles congruent to n modulo ii, so
 *     the last iteration's last offset issues at (iterations - 1) * ii +
 *     dynlen - 1, and by 7.7.3.2 (printed page 678) the buffer is idle once
 *     it "is finished draining", one cycle later.
 *
 * Residual limitation, stated rather than hidden: the same misreading of page
 * 686 in both the test and cdj_c674x_loop_load would still pass. No cycle
 * oracle exists for this core (DSP_ARCHITECTURE_COVERAGE.md section 7). */
static void compare_field(unsigned ii, unsigned field)
{
    CdjC674x full, compact;
    unsigned cbits = cycle_bits(ii);
    unsigned cycle = field & ((1u << cbits) - 1);
    bool valid = cycle < ii;
    assert(decode_schedule(&full, ii, field, false, 48, false) == valid);
    assert(decode_schedule(&compact, ii, field, true, 48, false) == valid);
    if (!valid) return;
    assert(!memcmp(&full.loop, &compact.loop, sizeof(full.loop)));
    assert(compact.loop.length == 48 && compact.loop.iterations == 128);
    uint64_t expected = 128u * ii + stage_value(field, cbits) * ii + cycle;
    uint64_t loading_end = ((48 + ii - 1) / ii) * ii;
    uint64_t drain = 127u * ii + 48;
    if (expected < loading_end) expected = loading_end;
    if (expected > drain) expected = drain;
    assert(compact.loop.post_cycle == expected && compact.loop.end_cycle == drain);
    /* Inspect every subsequent loop-scheduler cycle through exact drain. */
    CdjC674xLoop a = full.loop, b = compact.loop;
    while (a.cycle <= drain) {
        uint32_t atags[8], btags[8];
        unsigned an, bn; bool apost, bpost, adrain, bdrain;
        uint64_t now = a.cycle;
        assert(cdj_c674x_loop_issue(&a, atags, &an, &apost, &adrain));
        assert(cdj_c674x_loop_issue(&b, btags, &bn, &bpost, &bdrain));
        assert(!an && !bn && apost == bpost && adrain == bdrain);
        assert(apost == (now >= expected) && adrain == (now >= drain));
    }
}

int main(void)
{
    static const unsigned intervals[] = {1, 2, 3, 4, 5, 8, 9, 14, 16};
    cdj_c674x_loop_set_functional_timing(false);
    for (unsigned i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i)
        for (unsigned field = 0; field < 64; ++field)
            compare_field(intervals[i], field);
    /* Firmware compact0xdc66: field24 reverses to stage6 at II1, not3.
     * Use eight body cycles to avoid the drain clamp and a real SPLOOPD. */
    assert(compact_kernel(24) == 0xdc66);
    assert(stage_value(24, 0) == 6);
    CdjC674x cpu;
    assert(decode_schedule(&cpu, 1, 24, true, 8, true));
    assert(cpu.loop.post_cycle == cpu.loop.iterations + 6);
    assert(cpu.loop.post_cycle < cpu.loop.end_cycle);
    puts("C674x compact/full SPKERNEL field and schedule tests passed");
    return 0;
}
