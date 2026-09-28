/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c674x_multicycle.h"
#include "cdj_c674x_uncond.h"
#include "cdj_c674x_mpy.h"
#include "cdj_c674x_dotp.h"
#include "cdj_c674x_packed8.h"
#include "cdj_c674x_packbits.h"
#include "cdj_c674x_mpy32.h"
#include "cdj_c674x_sp.h"
#include "cdj_c674x_dp.h"
#include "cdj_c674x_approx.h"
#include "cdj_c674x_control.h"

/* This scratch CPU is initialized by the prefix copy below.  Its loop tail
 * must never be accessed by packet execution.  Avoid compiler-injected tail
 * clearing while retaining automatic initialization everywhere else. */
#if defined(__has_attribute)
# if __has_attribute(uninitialized)
#  define CDJ_C674X_UNINITIALIZED __attribute__((uninitialized))
# endif
#endif
#ifndef CDJ_C674X_UNINITIALIZED
# define CDJ_C674X_UNINITIALIZED
#endif


#define CDJ_C674X_TSR_SPLX (1u << 14)
#define CDJ_C674X_LOOP_RETURNING (1u << 3)
#define CDJ_C674X_LOOP_CONTEXT 31u
#define CDJ_C674X_LOOP_SETUP_PC UINT64_C(0xffffffff)
#define CDJ_C674X_LOOP_INTERRUPT_SHIFT 32u
#define CDJ_C674X_LOOP_INTERRUPT_MASK (UINT64_C(15) << CDJ_C674X_LOOP_INTERRUPT_SHIFT)
#define CDJ_C674X_LOOP_RETAINED_TAG_SHIFT 36u
#define CDJ_C674X_LOOP_RETAINED_TAG_MASK (UINT64_C(127) << CDJ_C674X_LOOP_RETAINED_TAG_SHIFT)
#define CDJ_C674X_LOOP_RETAINED_LENGTH_SHIFT 43u
#define CDJ_C674X_LOOP_RETAINED_LENGTH_MASK (UINT64_C(63) << CDJ_C674X_LOOP_RETAINED_LENGTH_SHIFT)
#define CDJ_C674X_LOOP_RETAINED_II_SHIFT 49u
#define CDJ_C674X_LOOP_RETAINED_II_MASK (UINT64_C(31) << CDJ_C674X_LOOP_RETAINED_II_SHIFT)
#define CDJ_C674X_LOOP_RETAINED_VALID (UINT64_C(1) << 54)
#define CDJ_C674X_LOOP_RETAINED_MASK \
    (CDJ_C674X_LOOP_RETAINED_TAG_MASK | CDJ_C674X_LOOP_RETAINED_LENGTH_MASK | \
     CDJ_C674X_LOOP_RETAINED_II_MASK | CDJ_C674X_LOOP_RETAINED_VALID)
#define CDJ_C674X_LOOP_CONTEXT_VALID (UINT64_C(1) << 56)
#define CDJ_C674X_LOOP_HAS_SPMASK (UINT64_C(1) << 57)
#define CDJ_C674X_LOOP_INTERRUPT_ARMED (UINT64_C(1) << 62)
#define CDJ_C674X_LOOP_INTERRUPT_DRAINING (UINT64_C(1) << 63)

/* SPRUFE8B 7.7.3.2 makes TSR.SPLX hardware-owned loop-buffer state.  Keep
 * it synchronized here instead of adding a second checkpointed state bit. */
static void loop_set_active(CdjC674x *cpu, bool active)
{
    cpu->loop_active = active;
    if (active) cpu->control[26] |= CDJ_C674X_TSR_SPLX;
    else cpu->control[26] &= ~CDJ_C674X_TSR_SPLX;
}

static uint32_t loop_setup_pc(const CdjC674x *cpu)
{
    return cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
           CDJ_C674X_LOOP_SETUP_PC;
}

static bool loop_interrupt_draining(const CdjC674x *cpu)
{
    return cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
           CDJ_C674X_LOOP_INTERRUPT_DRAINING;
}

static bool loop_interrupt_armed(const CdjC674x *cpu)
{
    return cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
           CDJ_C674X_LOOP_INTERRUPT_ARMED;
}

static unsigned loop_selected_interrupt(const CdjC674x *cpu)
{
    return (cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
            CDJ_C674X_LOOP_INTERRUPT_MASK) >>
           CDJ_C674X_LOOP_INTERRUPT_SHIFT;
}

static bool loop_retained_valid(const CdjC674x *cpu)
{
    return cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
           CDJ_C674X_LOOP_RETAINED_VALID;
}

static unsigned loop_retained_tags(const CdjC674x *cpu)
{
    return (cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
            CDJ_C674X_LOOP_RETAINED_TAG_MASK) >>
           CDJ_C674X_LOOP_RETAINED_TAG_SHIFT;
}

static unsigned loop_retained_length(const CdjC674x *cpu)
{
    return (cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
            CDJ_C674X_LOOP_RETAINED_LENGTH_MASK) >>
           CDJ_C674X_LOOP_RETAINED_LENGTH_SHIFT;
}

static unsigned loop_retained_ii(const CdjC674x *cpu)
{
    return (cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
            CDJ_C674X_LOOP_RETAINED_II_MASK) >>
           CDJ_C674X_LOOP_RETAINED_II_SHIFT;
}

static void loop_clear_retained(CdjC674x *cpu)
{
    cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &=
        ~CDJ_C674X_LOOP_RETAINED_MASK;
}

static bool loop_capture_retained(CdjC674x *cpu)
{
    if (cpu->loop_tags > 112 || cpu->loop.length > 48 ||
        !cpu->loop.ii || cpu->loop.ii > 16)
        return false;
    loop_clear_retained(cpu);
    cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] |=
        CDJ_C674X_LOOP_RETAINED_VALID |
        (uint64_t)cpu->loop_tags << CDJ_C674X_LOOP_RETAINED_TAG_SHIFT |
        (uint64_t)cpu->loop.length << CDJ_C674X_LOOP_RETAINED_LENGTH_SHIFT |
        (uint64_t)cpu->loop.ii << CDJ_C674X_LOOP_RETAINED_II_SHIFT;
    return true;
}

static void loop_set_setup(CdjC674x *cpu, uint32_t setup_pc,
                           bool preserve_retained)
{
    uint64_t retained = preserve_retained ?
        cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] &
        CDJ_C674X_LOOP_RETAINED_MASK : 0;
    cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] =
        CDJ_C674X_LOOP_CONTEXT_VALID | retained | setup_pc;
}

static void loop_set_interrupt_phase(CdjC674x *cpu, unsigned interrupt,
                                     uint64_t phase)
{
    uint64_t context = cpu->control_ready[CDJ_C674X_LOOP_CONTEXT];
    context &= ~(CDJ_C674X_LOOP_INTERRUPT_MASK |
                 CDJ_C674X_LOOP_INTERRUPT_ARMED |
                 CDJ_C674X_LOOP_INTERRUPT_DRAINING);
    context |= (uint64_t)interrupt << CDJ_C674X_LOOP_INTERRUPT_SHIFT;
    cpu->control_ready[CDJ_C674X_LOOP_CONTEXT] = context | phase;
}

static void loop_clear_interrupt_phase(CdjC674x *cpu)
{
    loop_set_interrupt_phase(cpu, 0, 0);
}

static bool interrupt_pipe_down(CdjC674x *cpu)
{
    if (!cdj_c674x_loop_functional_timing()) {
        /* SPRUFE8B Figure 5-4: current PC denotes the first annulled E1
         * (cycle 6), and ISR E1 is cycle 15. Issue nothing in cycles 6..14;
         * normal empty steps still retire older E-stages and clock devices.
         * This fixed architectural interval is not an unbounded queue drain. */
        cpu->idle_cycles = 9;
        return true;
    }
    uint64_t latest = cpu->cycles;
    for (unsigned i = 0; i < cpu->load_count; ++i)
        if (cpu->loads[i].due > latest) latest = cpu->loads[i].due;
    for (unsigned i = 0; i < cpu->store_count; ++i)
        if (cpu->stores[i].due > latest) latest = cpu->stores[i].due;
    uint64_t drain = latest - cpu->cycles;
    /* >= , not > : a drain of exactly UINT32_MAX would alias
     * CDJ_C674X_IDLE_FOREVER and stop counting down. */
    if (drain >= UINT32_MAX) return false;
    /* SPRUFE8B printed page 274: IDLE "terminates upon servicing an
     * interrupt", so its unbounded wait is replaced by the entry interval
     * rather than kept as the larger of the two. Ordinary idle padding still
     * survives, because only the sentinel is cleared here. */
    if (cpu->idle_cycles == CDJ_C674X_IDLE_FOREVER) cpu->idle_cycles = 0;
    if (cpu->idle_cycles < drain) cpu->idle_cycles = drain;
    return true;
}

static int32_t sx(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

static bool queued_memory_load(const CdjC674xLoad *load)
{
    unsigned size = load->size & 255;
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static unsigned queued_result_registers(const CdjC674xLoad *load)
{
    if (load->size == CDJ_C674X_DELAYED_IFR_SET ||
        load->size == CDJ_C674X_DELAYED_IFR_CLEAR ||
        load->size == CDJ_C674X_DELAYED_SAT ||
        load->size == CDJ_C674X_DELAYED_FAUCR) return 0;
    return (load->size & 255) == 8 || load->size == 16 ? 2 : 1;
}

static uint32_t saturate32(int64_t value, bool *saturated)
{
    *saturated = value > INT32_MAX || value < INT32_MIN;
    return value > INT32_MAX ? INT32_MAX : value < INT32_MIN ?
           (uint32_t)INT32_MIN : (uint32_t)value;
}

/* SPRUFE8B 2.8.3/3.9.2: only the selected base register controls
 * circular arithmetic. Width 0 is linear; 1..32 are low address bits. */
static bool address_width(const CdjC674x *cpu, unsigned bank, unsigned reg,
                           unsigned *width)
{
    *width = 0;
    if (reg < 4 || reg > 7) return true;
    if (cpu->control_ready[0] > cpu->cycles) return false;
    unsigned mode = (cpu->control[0] >> (bank * 8 + (reg - 4) * 2)) & 3;
    if (mode == 3) return false;
    if (mode) *width = ((cpu->control[0] >> (mode == 1 ? 16 : 21)) & 31) + 1;
    return true;
}

static uint32_t circular_address(uint32_t base, uint32_t result, unsigned width)
{
    if (!width || width == 32) return result;
    uint32_t mask = (1u << width) - 1;
    return (base & ~mask) | (result & mask);
}

static uint32_t saturating_shift32(uint32_t source, unsigned count,
                                   bool *saturated)
{
    /* SSHL's six-bit register count includes 32..63. Avoid signed shifts
     * and overflow: multiplication fits int64_t for all counts below 32. */
    if (count < 32)
        return saturate32((int64_t)(int32_t)source * (INT64_C(1) << count),
                          saturated);
    *saturated = source != 0;
    return !source ? 0 : source & 0x80000000u ? 0x80000000u : 0x7fffffffu;
}

static uint64_t arithmetic_shift_right64(uint64_t value, unsigned count)
{
    uint64_t shifted = value >> count;
    if (value >> 63) shifted |= UINT64_MAX << (64 - count);
    return shifted;
}

static uint64_t register_long40(const CdjC674x *cpu, unsigned bank,
                                unsigned reg)
{
    return ((uint64_t)(cpu->r[bank][reg + 1] & 0xff) << 32) |
           cpu->r[bank][reg];
}

static uint32_t shift_32(uint32_t source, unsigned count, unsigned operation)
{
    if (operation == 0) return count >= 32 ? 0 : source << count;
    if (operation == 2) return count >= 32 ? 0 : source >> count;
    if (count >= 32) return (source & 0x80000000u) ? UINT32_MAX : 0;
    if (!count) return source;
    return (source >> count) |
           ((source & 0x80000000u) ? UINT32_MAX << (32 - count) : 0);
}

/* Return the number of cycles inserted by a NOP encoding, or zero when the
 * instruction is not a NOP. The compact H-9 N3 field encodes count - 1;
 * SPRUFE8B labels the operand N3, but TI dis6x and GNU binutils agree on the
 * +1 translation used here. */
static unsigned nop_cycles(const CdjC674xInstruction *insn)
{
    if (insn->compact)
        return (insn->word & 0x1fffu) == 0x0c6eu ? (insn->word >> 13) + 1 : 0;
    if ((insn->word & 0xfffe1ffeu) == 0)
        return ((insn->word >> 13) & 15) + 1;
    return 0;
}

typedef enum {
    CDJ_C674X_INTERRUPT_GATE_NONE,
    CDJ_C674X_INTERRUPT_GATE_DISABLE,
    CDJ_C674X_INTERRUPT_GATE_RESTORE,
} CdjC674xInterruptGate;

/* DINT/RINT are unconditional no-unit instructions. Their high nibble is
 * not a normal predicate field (SPRUFE8B DINT/RINT entries). GNU's no-unit
 * formats expose an s bit for table uniformity, but both require s=0; only
 * the parallel bit is variable. */
static CdjC674xInterruptGate interrupt_gate_decode(
    const CdjC674xInstruction *insn)
{
    if (insn->compact) return CDJ_C674X_INTERRUPT_GATE_NONE;
    switch (insn->word & ~1u) {
    case 0x10004000u: return CDJ_C674X_INTERRUPT_GATE_DISABLE;
    case 0x10006000u: return CDJ_C674X_INTERRUPT_GATE_RESTORE;
    default: return CDJ_C674X_INTERRUPT_GATE_NONE;
    }
}

/* SPRUFE8B 3.8.11.3/6 list the operations which may not share an execute
 * packet with DINT or RINT. Keep this format check separate from execution:
 * several members intentionally remain unsupported, but must still reject
 * the whole packet atomically rather than being hidden by decode order. */
static bool interrupt_gate_parallel_conflict(
    CdjC674xInterruptGate gate, const CdjC674xInstruction *insn)
{
    CdjC674xInterruptGate other = interrupt_gate_decode(insn);
    if (other != CDJ_C674X_INTERRUPT_GATE_NONE) return other != gate;

    uint32_t w = insn->word;
    if (nop_cycles(insn) > 1) return true; /* Includes full-width IDLE. */
    if (insn->compact) {
        return (w & 0xbc7eu) == 0x0c66u || /* SPLOOP(D), reload form. */
               (w & 0x3c7eu) == 0x1c66u || /* SPKERNEL. */
               (w & 0x3c7eu) == 0x2c66u || /* SPMASK. */
               (w & 0x3c7eu) == 0x3c66u;   /* SPMASKR. */
    }
    if ((w & ~1u) == 0x10000000u || /* SWE. */
        (w & ~1u) == 0x10002000u || /* SWENR. */
        (w & ~1u) == 0x00036000u)   /* SPKERNELR. */
        return true;
    if ((w & 0xf03ffffeu) == 0x00034000u) return true; /* SPKERNEL. */
    if ((w & 0x007ffffcu) == 0x00038000u || /* SPLOOP. */
        (w & 0x007ffffcu) == 0x0003a000u || /* SPLOOPD. */
        (w & 0x007ffffeu) == 0x0003e000u)   /* SPLOOPW. */
        return true;
    if ((w & 0xfc03fffeu) == 0x00030000u || /* SPMASK. */
        (w & 0xfc03fffeu) == 0x00032000u)   /* SPMASKR. */
        return true;
    if ((w & 0x0ffffffeu) == 0x001800e2u || /* B IRP. */
        (w & 0x0ffffffeu) == 0x001c00e2u)   /* B NRP. */
        return true;
    if ((w & 0xffeu) == 0x3a2u && ((w >> 13) & 31) == 0) {
        unsigned control = (w >> 23) & 31;
        if (control == 1 || control == 26) return true; /* MVC reg,CSR/TSR. */
    }
    return false;
}

/* SPMASK unit bits are L1,L2,S1,S2,D1,D2,M1,M2. GNU's opcode table
 * corrects the full-width opcode in SPRUFE8B; compact is Figure H-8. */
static bool spmask_decode(const CdjC674xInstruction *insn, unsigned *mask)
{
    uint32_t w = insn->word;
    if (insn->compact && (w & 0x3c7e) == 0x2c66) {
        *mask = (w & 1) | ((w >> 6) & 14) | ((w >> 10) & 48);
        return true;
    }
    if (!insn->compact && (w & 0xfc03fffe) == 0x30000) {
        *mask = (w >> 18) & 255;
        return true;
    }
    return false;
}

/* Format-level unit classification (SPRUFE8B appendices C-G). Zero means
 * unknown: never infer that an unknown operation is safe to mask or replay.
 * This classifies units, not opcode validity; execution still validates ISA. */
static unsigned instruction_unit(const CdjC674xInstruction *insn)
{
    uint32_t w = insn->word;
    unsigned side = insn->compact ? w & 1 : (w >> 1) & 1;
    if (!insn->compact) {
        if ((w & 0x0c) == 12) return 32u; /* Long offsets always use .D2. */
        if ((w & 0x1c) == 0x18) return 1u << side;
        if ((w & 0x0c) == 4 || (w & 0x0c) == 12 ||
            (w & 0x7c) == 0x40 || (w & 0xc3c) == 0x830) return 16u << side;
        if ((w & 0x3c) == 0x20 || (w & 0x3c) == 0x28 ||
            (w & 0x3c) == 8 || (w & 0x7c) == 0x10 ||
            (w & 0x7c) == 0x50 || (w & 0xc3c) == 0xc30) return 4u << side;
        if ((w & 0x7c) == 0 || (w & 0x83c) == 0x30) return 64u << side;
        return 0;
    }
    if (((w & 0x26) == 6 || (w & 0x1c66) == 0x0866 ||
         (w & 0x1c66) == 0x1866) && ((w >> 3) & 3) != 3)
        return 1u << (2 * ((w >> 3) & 3) + side);
    if ((w & 6) == 4 || (w & 0x087f) == 0x0077 ||
        (w & 0x047e) == 0x0036 || (w & 0x047e) == 0x0436 ||
        (w & 0x1c7e) == 0x0c76) return 16u << side;
    if ((w & 0x040e) == 0 || (w & 0x040e) == 0x400 ||
        (w & 0x040e) == 0x408 || (w & 0x047e) == 0x426 ||
        (w & 0x047e) == 0x26) return 1u << side;
    if ((w & 0x001e) == 0x001e) return 64u << side;
    if ((w & 0x040e) == 0xa || (w & 0x040e) == 0x40a ||
        (w & 0x001e) == 0x12 || (w & 0x001e) == 2 ||
        (w & 0x047e) == 0x62 || (w & 0x047e) == 0x462 ||
        (w & 0x047e) == 0x2e || (w & 0x047e) == 0x42e ||
        (w & 0x187e) == 0x6e || (w & 0x1c7e) == 0x186e) return 4u << side;
    return 0;
}

typedef struct { CdjC674x *cpu; unsigned mask; bool unknown; } LoopMask;
static bool compact_branch(const CdjC674xInstruction *i)
{
    unsigned w = i->word;
    return i->compact && ((w & 0x187f) == 0x006f ||
        ((i->header & 0x8000) && ((w & 0x3e) == 0x0a ||
         (w & 0x3e) == 0x1a || (w & 0x2e) == 0x2a)));
}
static bool protected_load(const CdjC674xInstruction *i)
{
    if (!(i->header & (1u << 20))) return false;
    uint32_t w = i->word;
    if (i->compact) {
        /* Figure C-21 Dpp uses bit 14 for load/store, unlike the other
         * compact .D formats which use bit 3.  PROT applies to every LD in
         * the fetch packet regardless of compact format (section 3.10.2.2). */
        if ((w & 0x087f) == 0x0077) return (w & 0x4000) != 0;
        return (w & 6) == 4 && (w & 8);
    }
    if ((w & 0x0c) == 12) {
        unsigned op = (w >> 4) & 7;
        return op != 3 && op != 5 && op != 7;
    }
    if ((w & 0x10c) != 4 && (w & 0x17c) != 0x134 &&
        (w & 0x17c) != 0x154 && (w & 0x17c) != 0x124 &&
        (w & 0x17c) != 0x174 && (w & 0x17c) != 0x164 &&
        (w & 0x17c) != 0x144) return false;
    unsigned op = (w >> 4) & 7;
    return op != 5 && op != 7 && op != ((w & 0x100) ? 4u : 3u);
}
static bool loop_allow(void *opaque, uint32_t tag)
{
    LoopMask *context = opaque;
    if (!context->mask) return true;
    unsigned unit = instruction_unit(&context->cpu->loop_instructions[tag]);
    if (!unit) context->unknown = true;
    return !(unit & context->mask);
}

static bool loop_retained_tag(const CdjC674x *cpu,
                              const CdjC674xInstruction *insn,
                              uint32_t *tag)
{
    unsigned matches = 0, found = 0, limit = loop_retained_tags(cpu);
    if (!loop_retained_valid(cpu) || limit > 112) return false;
    for (unsigned i = 0; i < limit; ++i) {
        const CdjC674xInstruction *old = &cpu->loop_instructions[i];
        if (old->pc == insn->pc && old->word == insn->word &&
            old->compact == insn->compact && old->header == insn->header) {
            found = i;
            ++matches;
        }
    }
    if (matches != 1) return false;
    *tag = found;
    return true;
}

static bool loop_retained_schedule_complete(const CdjC674x *cpu)
{
    bool seen[112] = {0};
    unsigned limit = loop_retained_tags(cpu);
    if (!loop_retained_valid(cpu) || limit > 112 ||
        cpu->loop.length != loop_retained_length(cpu) ||
        cpu->loop.ii != loop_retained_ii(cpu))
        return false;
    for (unsigned origin = 0; origin < cpu->loop.length; ++origin)
        for (unsigned i = 0; i < cpu->loop.count[origin]; ++i) {
            uint32_t tag = cpu->loop.tags[origin][i];
            if (tag >= limit || seen[tag]) return false;
            seen[tag] = true;
        }
    for (unsigned i = 0; i < limit; ++i)
        if (!seen[i]) return false;
    return true;
}

/* Side-effect-free RAM reads; nonaligned words may span two bus words. */
static bool read_scalar(CdjC674xRead read, void *opaque, uint32_t address,
                        unsigned size, uint64_t *value)
{
    if ((uint64_t)address + size > UINT64_C(0x100000000)) return false;
    *value = 0;
    for (unsigned done = 0; done < size;) {
        uint32_t word, current = address + done;
        if (!read(opaque, current & ~3u, &word)) return false;
        unsigned lane = current & 3, n = 4 - lane;
        if (n > size - done) n = size - done;
        word >>= lane * 8;
        if (n < 4) word &= (1u << (n * 8)) - 1;
        *value |= (uint64_t)word << (done * 8);
        done += n;
    }
    return true;
}

/* High size byte retains issue-time circular width for nonaligned memory.
 * Delayed E3 transactions must not re-read a subsequently changed AMR. */
static bool read_transfer(CdjC674xRead read, void *opaque, uint32_t address,
                          unsigned encoded_size, uint64_t *value)
{
    unsigned size = encoded_size & 255, width = encoded_size >> 8;
    if (!width) return read_scalar(read, opaque, address, size, value);
    *value = 0;
    for (unsigned i = 0; i < size; ++i) {
        uint64_t byte;
        if (!read_scalar(read, opaque, circular_address(address, address + i, width), 1, &byte))
            return false;
        *value |= byte << (8 * i);
    }
    return true;
}

static bool write_transfer(CdjC674xWrite write, void *opaque, uint32_t address,
                           uint64_t value, unsigned encoded_size, bool commit)
{
    if (!write) return false;
    unsigned size = encoded_size & 255, width = encoded_size >> 8;
    if (!width || circular_address(address, address + size - 1, width) ==
                  (uint64_t)address + size - 1)
        return write(opaque, address, value, size, commit);
    for (unsigned i = 0; i < size; ++i)
        if (!write(opaque, circular_address(address, address + i, width),
                   (value >> (8 * i)) & 255, 1, commit)) return false;
    return true;
}

void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry)
{
    memset(cpu, 0, sizeof(*cpu));
    /* C674x CPU ID 0x14, little endian, and reset interrupt enabled.
     * C6747 HOST1CFG resets the interrupt table base to DSP ROM 0x00700000. */
    cpu->control[1] = 0x14000100;
    cpu->control[4] = 1;
    cpu->control[5] = 0x00700000;
    cpu->control[24] = 0x0700001du; /* GFPGFR SIZE=7, POLY=1Dh. */
    cpu->pc = entry;
}

static bool stop(CdjC674x *cpu, uint32_t pc, uint32_t word, const char *why)
{
    cpu->fault = why;
    cpu->fault_pc = pc;
    cpu->fault_word = word;
    return false;
}

bool cdj_c674x_interrupt(CdjC674x *cpu, uint32_t pending)
{
    const uint32_t maskable = 0x0000fff0u;
    if (cpu->fault) return false;
    if (pending & ~maskable)
        return stop(cpu, cpu->pc, 0, "invalid CPU interrupt request mask");

    /* This interface starts after system-event selection/INTMUX. The caller
     * presents CPU INT4..15 requests, not raw C6747 events. SPRUFE8B 5.4.1
     * makes IFR sticky until ICR or acceptance clears a bit. */
    cpu->control[2] = (cpu->control[2] | pending) & maskable;
    uint32_t eligible = cpu->control[2] & cpu->control[4] & maskable;
    if (!(cpu->control[1] & 1u) || !(cpu->control[4] & 2u) || !eligible) {
        if (!cpu->loop_active && loop_interrupt_draining(cpu))
            loop_clear_interrupt_phase(cpu);
        return true;
    }

    /* Section 5.4.2 forbids recognition in a branch's five delay packets.
     * The interpreter represents taken branches explicitly, so defer while
     * one is live. False conditional branches do not yet have pipeline state.
     */
    if (cpu->branch_due || cpu->branch_count) return true;
    if (cpu->loop_active) {
        if (loop_interrupt_armed(cpu) || loop_interrupt_draining(cpu))
            return true;

        /* SPRUFE8B 7.13.1: wait for a stage boundary with a false normal
         * termination condition, after loading and the protected opening
         * cycles.  SPLOOP(D) additionally needs enough ILC to restart its
         * complete prolog after B IRP. */
        uint64_t boundary_cycle = cpu->loop.cycle + 1;
        bool boundary = cpu->loop.sealed && cpu->loop.ii &&
                        boundary_cycle % cpu->loop.ii == 0;
        /* The two-cycle pre-SPLOOP automatic-disable window is not modeled
         * independently. Waiting through the documented four-cycle opening
         * is conservative for every supported unconditional form. */
        bool opening = boundary_cycle < 4;
        bool terminating = cpu->loop.predicate_loop ?
            (boundary_cycle >= 4 && !(cpu->loop_pred_history & 4u)) :
            cpu->loop.cycle >= (uint64_t)cpu->loop.iterations * cpu->loop.ii;
        uint32_t loading_stages =
            (cpu->loop.length + cpu->loop.ii - 1) / cpu->loop.ii;
        bool enough_ilc = cpu->loop.predicate_loop ||
                          cpu->control[13] >= loading_stages;
        if (!boundary || opening || terminating || !enough_ilc) return true;
        uint64_t context = cpu->control_ready[CDJ_C674X_LOOP_CONTEXT];
        if (!(context & CDJ_C674X_LOOP_CONTEXT_VALID))
            return stop(cpu, cpu->pc, 0,
                        "SPLOOP interrupt setup address unavailable");
        /* SPRUFE8B 7.13.1 (printed page 697) enumerates every condition that
         * blocks interrupt draining and an SPMASK in the loop is not one of
         * them, so CDJ_C674X_LOOP_HAS_SPMASK is recorded state and no longer
         * a refusal. The resume rule it used to stand in for is stated in
         * full by 7.11.5 (printed page 696) and 7.13.2 (printed page 698) and
         * is applied in loop_step: the SPMASKed program-memory operation is a
         * NOP while the loop-buffer operation on the masked unit executes. An
         * SPMASK whose unit this core cannot classify still fails closed
         * during loading ("SPMASK unit not implemented"), and the reload form
         * of the substitution (7.11.3) is still refused with reload itself. */
        if (!loop_capture_retained(cpu))
            return stop(cpu, cpu->pc, 0,
                        "SPLOOP retained buffer state invalid");
        unsigned interrupt = 4;
        while (!(eligible & (1u << interrupt))) ++interrupt;
        /* Detection is on this stage boundary; draining begins on the next
         * cycle. Keep the selected request stable while the epilog drains. */
        loop_set_interrupt_phase(cpu, interrupt,
                                 CDJ_C674X_LOOP_INTERRUPT_ARMED);
        return true;
    }
    if (cpu->control[26] & (1u << 9))
        return stop(cpu, cpu->pc, 0,
                    "nested maskable interrupt not implemented");

    bool interrupted_loop = loop_interrupt_draining(cpu);
    unsigned interrupt = interrupted_loop ? loop_selected_interrupt(cpu) : 0;
    if (interrupted_loop &&
        (interrupt < 4 || interrupt > 15 || !(eligible & (1u << interrupt)))) {
        /* Section 7.13.6: if loop code disables the request while draining,
         * finish the epilog and continue after SPKERNEL without vectoring. */
        loop_clear_interrupt_phase(cpu);
        loop_clear_retained(cpu);
        interrupted_loop = false;
        if (!eligible) return true;
    }

    /* INT4 has highest maskable priority (Table 5-1). The interpreter has no
     * speculative fetch pipeline, so the current PC is exactly the first
     * execute packet annulled by the interrupt and therefore the IRP value.
     * Existing delayed E2..E5 effects belong to older, non-annulled packets
     * and remain queued to mature during the entry interval (5.4.4). */
    if (!interrupted_loop) {
        interrupt = 4;
        while (!(eligible & (1u << interrupt))) ++interrupt;
    }
    uint32_t saved_tsr = (cpu->control[26] & 0x0000c6deu) |
                         (cpu->control[1] & 1u);
    saved_tsr &= ~(1u << 15);
    if (interrupted_loop) saved_tsr |= CDJ_C674X_TSR_SPLX;
    else saved_tsr &= ~CDJ_C674X_TSR_SPLX;
    cpu->control[27] = saved_tsr;
    cpu->control[6] = interrupted_loop ? loop_setup_pc(cpu) : cpu->pc;
    cpu->control[2] &= ~(1u << interrupt);
    loop_clear_interrupt_phase(cpu);

    /* Table 5-3: save TSR in ITSR, enter supervisor interrupt context, retain
     * GEE/DBGM, clear GIE/SGIE/XEN/CXM/EXC/SPLX, and assert INT/IB. PGIE and
     * ITSR.GIE are the same physical bit. */
    cpu->control[1] = (cpu->control[1] & ~3u) |
                      ((saved_tsr & 1u) << 1);
    cpu->control[26] = (saved_tsr & ((1u << 4) | (1u << 2))) |
                       (1u << 15) | (1u << 9);
    cpu->pc = (cpu->control[5] & 0xfffffc00u) + interrupt * 32u;
    /* Figure 5-4 keeps older, non-annulled E-stages ahead of the forced ISR
     * branch. Strict mode inserts the nine empty issue cycles before ISR E1.
     * Breadth mode retains its historical minimum-drain approximation. */
    if (!interrupt_pipe_down(cpu))
        return stop(cpu, cpu->pc, 0,
                    "interrupt pipe-down interval overflow");
    return true;
}

/* One taken branch may enter E1 each cycle; all six pipeline positions can
 * be occupied. Preserve later branches when an older branch redirects fetch. */
static bool queue_branch(CdjC674x *out, uint64_t due, uint32_t target)
{
    if (!out->branch_due) {
        out->branch_due = due; out->branch_target = target;
        return true;
    }
    if (out->branch_due == due || out->branch_count == 5 ||
        (out->branch_count && out->branch_queue[out->branch_count - 1].due == due)) return false;
    out->branch_queue[out->branch_count].due = due;
    out->branch_queue[out->branch_count++].target = target;
    return true;
}

bool cdj_c674x_fetch(CdjC674x *cpu, CdjC674xRead read, void *opaque,
                     CdjC674xPacket *packet)
{
    CdjC674xPacket result = {0};
    uint32_t next = cpu->pc;
    uint32_t header = 0, header_address = 0;
    bool have_header = false;
    unsigned count = 0;
    bool parallel = true;
    if (cpu->fault) return false;
    /* Validate the entire packet before committing any architectural state. */
    do {
        uint32_t word;
        if (next & 1) return stop(cpu, next, 0, "unaligned instruction fetch");
        uint32_t block_header = (next & ~31u) + 28;
        /* Fetch has no clock edges or writes. Reuse the side-effect-free
         * header read within this packet only; the next fetch reads anew. */
        if (!have_header || block_header != header_address) {
            if (!read(opaque, block_header, &header))
                return stop(cpu, next, 0, "unmapped instruction fetch");
            header_address = block_header;
            have_header = true;
        }
        bool mixed = (header >> 28) == 14;
        /* The header occupies no execution slot. */
        if (mixed && (next & 31) == 28) {
            next += 4;
            continue;
        }
        if (mixed && (next & 31) == 30)
            return stop(cpu, next, header, "instruction points into compact header");
        unsigned slot = (next & 31) / 4;
        bool short_word = mixed && ((header >> (21 + slot)) & 1);
        if (!short_word && (next & 3))
            return stop(cpu, next, 0, "unaligned full instruction fetch");
        if (!read(opaque, next & ~3u, &word))
            return stop(cpu, next, 0, "unmapped instruction fetch");
        if (count == 8) return stop(cpu, next, word, "execute packet exceeds eight instructions");
        if (short_word) word = (word >> ((next & 2) * 8)) & 0xffff;
        parallel = short_word ? ((header >> ((next & 31) / 2)) & 1) : (word & 1);
        result.instructions[count++] = (CdjC674xInstruction){
            .pc = next, .word = word, .compact = short_word, .header = mixed ? header : 0
        };
        next += short_word ? 2 : 4;
        if (mixed && (next & 31) == 28) next += 4;
    } while (parallel);
    result.count = count;
    result.next_pc = next;
    *packet = result;
    return true;
}

/* ---- conditional-instruction dispatch -----------------------------------
 *
 * cdj_c674x_execute used to decode every conditional 32-bit instruction in one
 * if/else-if ladder nearly nine hundred lines long.  It is now a table: an
 * instruction family is one row of cdj_c674x_arms[] plus one arm_* function,
 * whose own arithmetic belongs in a pure file of its own (cdj_c674x_sp.c,
 * cdj_c674x_mpy.c) rather than here.  Nothing else has to change to add one.
 *
 * Selection is first-match-wins, in table order, which is the order the ladder
 * tested in.  As the table stands, no two rows can both match the same word:
 * that was verified by sweeping every word the rows can discriminate (no
 * predicate reads bits 31-28, so 2^28 words covers it) and finding no word
 * claimed twice.  So the current order is NOT load-bearing, and an earlier
 * version of this comment was wrong to say it was.
 *
 * What IS load-bearing is that the property keeps holding.  A new row whose
 * mask/match overlaps an existing row's would be silently shadowed by whichever
 * comes first, and nothing in the build would complain.  Before adding a row,
 * re-run the sweep - tests/test_dsp_isa_audit.py's probe and compact sweeps are
 * the cheap version, and they compare every verdict - rather than reasoning
 * about where the row belongs.  A row matches when
 * (w & mask) == match and its `also` predicate, if any, holds; mask/match is
 * the leading factor of the ladder's condition and `also` is the rest of it
 * verbatim.  No match at all is the ladder's final else: not implemented.
 *
 * Every arm receives CdjC674xArm and nothing else.  That struct is exactly
 * what used to be the ladder's locals, so the arm bodies are unchanged apart
 * from naming.  `out` is the execute packet's transactional copy: as in the
 * ladder, no arm may touch anything at or past offsetof(CdjC674x, loop), and
 * an arm that rejects an encoding calls stop() on the committed cpu - which is
 * what records the fault string - and returns false, leaving `out` discarded.
 * Returning false without calling stop() would reject a packet with no reason,
 * so arms never do.
 */
typedef struct {
    CdjC674x *cpu;                      /* committed state: read only */
    CdjC674x *out;                      /* transactional copy: prefix only */
    const CdjC674xPacket *packet;
    const CdjC674xInstruction *insn;
    CdjC674xRead read;
    CdjC674xWrite write;
    void *opaque;
    CdjC674xPacketTiming *timing;
    /* Per-packet conflict and accounting state, shared with the rest of the
     * packet loop, so these stay pointers to the caller's own variables. */
    bool (*written)[32];
    bool *controls;
    unsigned *memory_count;
    bool *nonaligned_memory;
    bool *bdec_issued;
    /* Fields the ladder decoded before it branched.  value, dst, side,
     * reg_write and control_write are what the commit step reads back out. */
    uint32_t w, pc, value;
    unsigned side, dst, a, b, cross, scalar_sat_op;
    bool enabled, reg_write, control_write, long_offset;
} CdjC674xArm;

typedef struct {
    uint32_t mask, match;
    bool (*also)(const CdjC674xArm *);
    bool (*run)(CdjC674xArm *);
} CdjC674xArmEntry;

static bool match_d_adda(const CdjC674xArm *x)
{
    return ((x->w >> 7) & 63) >= 0x30 && ((x->w >> 7) & 63) <= 0x3d;
}

static bool match_d_addsub(const CdjC674xArm *x)
{
    return ((x->w >> 7) & 63) >= 0x10 && ((x->w >> 7) & 63) <= 0x13;
}

static bool match_l_long_addsub(const CdjC674xArm *x)
{
    return ((x->w >> 5) & 0x7f) == 0x20 ||
           ((x->w >> 5) & 0x7f) == 0x21 ||
           ((x->w >> 5) & 0x7f) == 0x23 ||
           ((x->w >> 5) & 0x7f) == 0x24 ||
           ((x->w >> 5) & 0x7f) == 0x27 ||
           ((x->w >> 5) & 0x7f) == 0x29 ||
           ((x->w >> 5) & 0x7f) == 0x2b ||
           ((x->w >> 5) & 0x7f) == 0x2f ||
           ((x->w >> 5) & 0x7f) == 0x37 ||
           ((x->w >> 5) & 0x7f) == 0x3f;
}

static bool match_mpy32_scalar(const CdjC674xArm *x)
{
    return ((x->w >> 7) & 31) == 0x10 ||
           ((x->w >> 7) & 31) == 0x14 ||
           ((x->w >> 7) & 31) == 0x16;
}

static bool match_mpy32_packed(const CdjC674xArm *x)
{
    return ((x->w >> 6) & 31) == 0x18 ||
           ((x->w >> 6) & 31) == 0x19;
}

static bool match_mpy16(const CdjC674xArm *x)
{
    return ((x->w >> 7) & 31) == 0x19 || ((x->w >> 7) & 31) == 0x18 ||
           ((x->w >> 7) & 31) == 0x01 || ((x->w >> 7) & 31) == 0x09 ||
           ((x->w >> 7) & 31) == 0x0f || ((x->w >> 7) & 31) == 0x0b ||
           ((x->w >> 7) & 31) == 0x03 || ((x->w >> 7) & 31) == 0x07 ||
           ((x->w >> 7) & 31) == 0x0d || ((x->w >> 7) & 31) == 0x05 ||
           ((x->w >> 7) & 31) == 0x11 || ((x->w >> 7) & 31) == 0x17 ||
           ((x->w >> 7) & 31) == 0x13 || ((x->w >> 7) & 31) == 0x15 ||
           ((x->w >> 7) & 31) == 0x1b || ((x->w >> 7) & 31) == 0x1e ||
           ((x->w >> 7) & 31) == 0x1f || ((x->w >> 7) & 31) == 0x1d;
}

static bool match_smpy16_scalar(const CdjC674xArm *x)
{
    return ((x->w >> 7) & 31) == 0x1a || ((x->w >> 7) & 31) == 0x02 ||
           ((x->w >> 7) & 31) == 0x0a || ((x->w >> 7) & 31) == 0x12;
}

static bool match_smpy16_packed(const CdjC674xArm *x)
{
    return ((x->w >> 6) & 31) == 0x01;
}

static bool match_mpy_half32(const CdjC674xArm *x)
{
    return ((x->w >> 6) & 31) == 0x0e ||
           ((x->w >> 6) & 31) == 0x10 ||
           ((x->w >> 6) & 31) == 0x14 ||
           ((x->w >> 6) & 31) == 0x15;
}

static bool match_cmpsp(const CdjC674xArm *x)
{
    return ((x->w >> 6) & 63) >= 0x38 && ((x->w >> 6) & 63) <= 0x3a;
}

static bool match_src1_zero(const CdjC674xArm *x)
{
    return x->a == 0;
}

static bool arm_sat_long(CdjC674xArm *x)
{
    /* SADD signed32/scst5 + signed40, SSUB scst5 - signed40.
     * Long operands are local even/odd pairs; their high 24 bits
     * are not part of the signed 40-bit arithmetic value. */
    x->reg_write = false;
    if ((x->dst & 1) || (x->b & 1))
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    if (x->scalar_sat_op != 0x638 && (x->w & 0x1000))
        return stop(x->cpu, x->pc, x->insn->word, "cross-path long operand not supported");
    if (x->enabled) {
        int64_t left = x->scalar_sat_op == 0x638 ?
            (int32_t)x->cpu->r[x->cross][x->a] : sx(x->a, 5);
        uint64_t raw = register_long40(x->cpu, x->side, x->b);
        int64_t right = (int64_t)raw - ((raw & (UINT64_C(1) << 39)) ?
                                      (INT64_C(1) << 40) : 0);
        int64_t result = x->scalar_sat_op == 0x598 ? left - right : left + right;
        int64_t limit = INT64_C(1) << 39;
        bool saturated = result >= limit || result < -limit;
        if (result >= limit) result = limit - 1;
        if (result < -limit) result = -limit;
        if (x->written[x->side][x->dst] || x->written[x->side][x->dst + 1])
            return stop(x->cpu, x->pc, x->insn->word, "parallel register write conflict");
        x->out->r[x->side][x->dst] = (uint32_t)result;
        x->out->r[x->side][x->dst + 1] = ((uint64_t)result >> 32) & 0xffu;
        x->written[x->side][x->dst] = x->written[x->side][x->dst + 1] = true;
        if (saturated) {
            if (x->out->load_count == 40)
                return stop(x->cpu, x->pc, x->insn->word, "delayed-status queue full");
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = x->cpu->cycles + 2, .address = 1u << x->side,
                .size = CDJ_C674X_DELAYED_SAT
            };
        }
    }
    return true;
}

static bool arm_sat_scalar(CdjC674xArm *x)
{
    /* SADD/SSUB/SSHL scalar forms, SPRUFE8B pp422,493,499.
     * Result E1; CSR.SAT and per-unit SSR flag in E2. */
    if (x->enabled) {
        bool saturated;
        unsigned unit_bit = (x->scalar_sat_op & 0x1c) == 0x18 ? x->side : 2 + x->side;
        if (x->scalar_sat_op == 0x8e0 || x->scalar_sat_op == 0x8a0) {
            unsigned count = x->scalar_sat_op == 0x8a0 ? x->a : x->cpu->r[x->side][x->a] & 63;
            x->value = saturating_shift32(x->cpu->r[x->cross][x->b], count, &saturated);
        } else {
            int64_t left = x->scalar_sat_op == 0x258 || x->scalar_sat_op == 0x1d8 ?
                sx(x->a, 5) : (int32_t)x->cpu->r[x->scalar_sat_op == 0x3f8 ? x->cross : x->side][x->a];
            int64_t right = (int32_t)x->cpu->r[x->scalar_sat_op == 0x3f8 ? x->side : x->cross][x->b];
            bool subtract = x->scalar_sat_op == 0x1f8 || x->scalar_sat_op == 0x3f8 ||
                            x->scalar_sat_op == 0x1d8;
            x->value = saturate32(subtract ? left - right : left + right, &saturated);
        }
        if (saturated) {
            if (x->out->load_count == 40)
                return stop(x->cpu, x->pc, x->insn->word, "delayed-status queue full");
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = x->cpu->cycles + 2, .address = 1u << unit_bit,
                .size = CDJ_C674X_DELAYED_SAT
            };
        }
    }
    return true;
}

static bool arm_scalar_memory(CdjC674xArm *x)
{
    /* Scalar memory: address E1, RAM access E3, load destination E5. */
    unsigned op = (x->w >> 4) & 7;
    bool extended = !x->long_offset && (x->w & 0x100) != 0;
    bool pair = extended && (op == 2 || op == 4 || op == 6 || op == 7);
    bool nonaligned = extended && op != 4 && op != 6;
    unsigned size = pair ? 8 : nonaligned ? 4 : op >= 6 ? 4 : (op == 0 || op == 4 || op == 5) ? 2 : 1;
    bool is_store = extended ? (op == 4 || op == 5 || op == 7) : (op == 3 || op == 5 || op == 7);
    unsigned scale = size;
    if (pair && nonaligned) {
        scale = (x->w & (1u << 23)) ? 8 : 1;
        x->dst &= ~1u;
    } else if (pair && (x->dst & 1)) {
        return stop(x->cpu, x->pc, x->insn->word, "invalid doubleword register pair");
    }
    unsigned bank = (x->w >> 7) & 1, mode = (x->w >> 9) & 15;
    if (x->long_offset) {
        /* Figure C-5 / 3.9.3: unsigned scaled 15-bit displacement,
         * fixed B14/B15 base, no base update, data bank from s. */
        x->b = 14 + bank; bank = 1; mode = 1;
    }
    x->reg_write = false;
    if (!(mode & 8) && (mode & 2))
        return stop(x->cpu, x->pc, x->insn->word, "reserved memory addressing mode");
    if (x->b >= 4 && x->b <= 7 && x->cpu->control_ready[0] > x->cpu->cycles)
        return stop(x->cpu, x->pc, x->insn->word, "AMR use interlock not implemented");
    if (x->enabled) {
        ++(*x->memory_count);
        (*x->nonaligned_memory) |= nonaligned;
        unsigned width;
        if (!address_width(x->cpu, bank, x->b, &width))
            return stop(x->cpu, x->pc, x->insn->word, x->cpu->control_ready[0] > x->cpu->cycles ?
                        "AMR use interlock not implemented" : "reserved circular addressing mode");
        if (nonaligned && width && width < 5)
            return stop(x->cpu, x->pc, x->insn->word, "nonaligned circular buffer smaller than 32 bytes");
        uint32_t offset = (x->long_offset ? (x->w >> 8) & 32767 :
                           (mode & 4) ? x->cpu->r[bank][x->a] : x->a) * scale;
        uint32_t base = x->cpu->r[bank][x->b];
        uint32_t updated = circular_address(base,
            (mode & 1) ? base + offset : base - offset, width);
        uint32_t address = ((mode & 10) == 10) ? base : updated;
        unsigned encoded_size = size | ((nonaligned ? width : 0) << 8);
        uint64_t dummy, store_value = x->cpu->r[x->side][x->dst];
        if (pair) store_value |= (uint64_t)x->cpu->r[x->side][x->dst + 1] << 32;
        if ((!nonaligned && (address & (size - 1))) ||
            (is_store ? !write_transfer(x->write, x->opaque, address, store_value, encoded_size, false)
                      : !read_transfer(x->read, x->opaque, address, encoded_size, &dummy)))
            return stop(x->cpu, x->pc, x->insn->word, "unaligned or unmapped scalar memory access");
        if (is_store) {
            if (x->out->store_count == 24) return stop(x->cpu, x->pc, x->insn->word, "store queue full");
            x->out->stores[x->out->store_count++] = (CdjC674xStore){
                .due = x->cpu->cycles + 3, .address = address,
                .value = store_value, .size = encoded_size
            };
        } else {
            if (x->out->load_count == 40) return stop(x->cpu, x->pc, x->insn->word, "load queue full");
            for (unsigned j = 0; j < x->out->load_count; ++j)
                if (x->out->loads[j].due == x->cpu->cycles + 5 &&
                    x->out->loads[j].bank == x->side &&
                    x->out->loads[j].dst < x->dst + (pair ? 2 : 1) &&
                    x->dst < x->out->loads[j].dst + queued_result_registers(&x->out->loads[j]))
                    return stop(x->cpu, x->pc, x->insn->word, "parallel load write conflict");
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = x->cpu->cycles + 5, .address = address, .bank = x->side, .dst = x->dst,
                .size = encoded_size, .sign_extend = !extended && (op == 2 || op == 4)
            };
        }
        if (mode & 8) {
            if (x->written[bank][x->b]) return stop(x->cpu, x->pc, x->insn->word, "parallel register write conflict");
            x->out->r[bank][x->b] = updated; x->written[bank][x->b] = true;
        }
    }
    return true;
}

static bool arm_addk(CdjC674xArm *x)
{
    /* ADDK .S1/.S2 is an in-place modular add of a signed
     * sixteen-bit constant, with an E1 read and E1 write. */
    x->value = x->cpu->r[x->side][x->dst] +
            (uint32_t)sx((x->w >> 7) & 0xffff, 16);
    return true;
}

static bool arm_mvk_s(CdjC674xArm *x)
{
    x->value = sx((x->w >> 7) & 0xffff, 16);
    return true;
}

static bool arm_mvkh(CdjC674xArm *x)
{
    x->value = (x->cpu->r[x->side][x->dst] & 0xffff) | (((x->w >> 7) & 0xffff) << 16);
    return true;
}

static bool arm_d_adda(CdjC674xArm *x)
{
    /* ADDAB/H/W and SUBAB/H/W: same-bank operands, unsigned
     * five-bit immediate or register offset, scaled by 1/2/4. */
    unsigned op = (x->w >> 7) & 63;
    if (x->b >= 4 && x->b <= 7 && x->cpu->control_ready[0] > x->cpu->cycles)
        return stop(x->cpu, x->pc, x->insn->word, "AMR use interlock not implemented");
    /* ADDAD uses op 3c/3d; there is no SUBAD (TI page 117). */
    uint32_t offset = (op >= 0x3c ? op & 1 : op & 2) ? x->a : x->cpu->r[x->side][x->a];
    offset <<= (op - 0x30) / 4;
    x->value = (op < 0x3c && (op & 1)) ? x->cpu->r[x->side][x->b] - offset : x->cpu->r[x->side][x->b] + offset;
    if (x->enabled) {
        unsigned width;
        if (!address_width(x->cpu, x->side, x->b, &width))
            return stop(x->cpu, x->pc, x->insn->word, x->cpu->control_ready[0] > x->cpu->cycles ?
                        "AMR use interlock not implemented" : "reserved circular addressing mode");
        x->value = circular_address(x->cpu->r[x->side][x->b], x->value, width);
    }
    return true;
}

static bool arm_d_addsub(CdjC674xArm *x)
{
    /* ADD/SUB .D without a cross path, SPRUFE8B pp110,529.
     * The assembler operand order is src2,src1 because the D-unit
     * hardware subtracts the src1 field from the src2 field. */
    unsigned op = (x->w >> 7) & 63;
    uint32_t left = x->cpu->r[x->side][x->b];
    uint32_t right = (op & 2) ? x->a : x->cpu->r[x->side][x->a];
    x->value = (op & 1) ? left - right : left + right;
    return true;
}

static bool arm_d_addsub_cross(CdjC674xArm *x)
{
    /* Cross-path ADD/SUB .D, including ADD's signed constant form.
     * Cross SUB uses conventional src1-src2 ordering (SPRUFE8B
     * pp110-111,529-530), unlike non-cross D-unit SUB above. */
    uint32_t left = (x->w & 0xffc) == 0xaf0 ? (uint32_t)sx(x->a, 5)
                                         : x->cpu->r[x->side][x->a];
    uint32_t right = x->cpu->r[x->cross][x->b];
    x->value = (x->w & 0xffc) == 0xb30 ? left - right : left + right;
    return true;
}

static bool arm_mvk_d(CdjC674xArm *x)
{
    x->value = sx(x->a, 5); /* MVK .D */
    return true;
}

static bool arm_mvk_l(CdjC674xArm *x)
{
    x->value = sx(x->b, 5); /* MVK .L */
    return true;
}

static bool arm_l_long_addsub(CdjC674xArm *x)
{
    /* ADD/ADDU/SUB/SUBU extended .L forms write a 40-bit long in
     * an even/odd register pair.  The low register holds bits 31:0;
     * only bits 7:0 of the high register are architecturally part
     * of the value (SPRUFE8B ADD/ADDU/SUB/SUBU). */
    unsigned op = (x->w >> 5) & 0x7f;
    bool pair_source = op == 0x20 || op == 0x21 ||
                       op == 0x24 || op == 0x29;
    x->reg_write = false;
    /* The immediate-long forms have no cross-path variant: their
     * 40-bit src2 consumes the local .L long-data input.  Cross
     * paths carry only one 32-bit operand (SPRUFE8B 2.3, ADD/SUB
     * opcode maps).  Keep the otherwise format-shaped x=1 words
     * fail-closed instead of silently reading the local pair. */
    bool immediate_long = op == 0x20 || op == 0x24;
    if (immediate_long && (x->w & (1u << 12)))
        return stop(x->cpu, x->pc, x->insn->word,
                    "cross-path long operand not supported");
    if ((x->dst & 1) || (pair_source && (x->b & 1)))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid long register pair");
    if (x->enabled) {
        uint64_t left, right, result;
        switch (op) {
        case 0x20: /* ADD signed 5-bit, signed long. */
            left = (uint64_t)(int64_t)sx(x->a, 5);
            right = register_long40(x->cpu, x->side, x->b);
            result = left + right;
            break;
        case 0x21: /* ADD cross signed 32-bit, signed long. */
            left = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->cross][x->a];
            right = register_long40(x->cpu, x->side, x->b);
            result = left + right;
            break;
        case 0x23: /* ADD signed 32-bit, cross signed 32-bit. */
            left = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->side][x->a];
            right = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->cross][x->b];
            result = left + right;
            break;
        case 0x24: /* SUB signed 5-bit, signed long. */
            left = (uint64_t)(int64_t)sx(x->a, 5);
            right = register_long40(x->cpu, x->side, x->b);
            result = left - right;
            break;
        case 0x27: /* SUB signed 32-bit, cross signed 32-bit. */
            left = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->side][x->a];
            right = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->cross][x->b];
            result = left - right;
            break;
        case 0x29: /* ADDU cross unsigned 32-bit, unsigned long. */
            left = x->cpu->r[x->cross][x->a];
            right = register_long40(x->cpu, x->side, x->b);
            result = left + right;
            break;
        case 0x2b: /* ADDU unsigned 32-bit, cross unsigned 32-bit. */
            left = x->cpu->r[x->side][x->a];
            right = x->cpu->r[x->cross][x->b];
            result = left + right;
            break;
        case 0x2f: /* SUBU unsigned 32-bit, cross unsigned 32-bit. */
            left = x->cpu->r[x->side][x->a];
            right = x->cpu->r[x->cross][x->b];
            result = left - right;
            break;
        case 0x37: /* SUB cross signed 32-bit, signed 32-bit. */
            left = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->cross][x->a];
            right = (uint64_t)(int64_t)(int32_t)x->cpu->r[x->side][x->b];
            result = left - right;
            break;
        default:   /* SUBU cross unsigned 32-bit, unsigned 32-bit. */
            left = x->cpu->r[x->cross][x->a];
            right = x->cpu->r[x->side][x->b];
            result = left - right;
            break;
        }
        result &= UINT64_C(0xffffffffff);
        if (x->written[x->side][x->dst] || x->written[x->side][x->dst + 1])
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel register write conflict");
        x->out->r[x->side][x->dst] = (uint32_t)result;
        x->out->r[x->side][x->dst + 1] = (uint32_t)(result >> 32);
        x->written[x->side][x->dst] = x->written[x->side][x->dst + 1] = true;
    }
    return true;
}

static bool arm_mpy32(CdjC674xArm *x)
{
    /* The C674x 32x32 .M family samples both operands in E1 and
     * writes in E4.  MPY32 has scalar (low 32 bits) and signed
     * full-product forms; the SU/U/US variants always write the
     * complete 64-bit product to an even/odd register pair. */
    bool mpy_encoding = (x->w & 0x7c) == 0;
    unsigned op = mpy_encoding ? (x->w >> 7) & 31 : (x->w >> 6) & 31;
    bool pair = !mpy_encoding || op != 0x10;
    x->reg_write = false;
    if (pair && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        uint32_t left = x->cpu->r[x->side][x->a];
        uint32_t right = x->cpu->r[x->cross][x->b];
        uint64_t result;
        if (mpy_encoding && (op == 0x10 || op == 0x14)) {
            result = (uint64_t)((int64_t)(int32_t)left *
                                (int64_t)(int32_t)right);
        } else if (mpy_encoding) {       /* MPY32SU */
            result = (uint64_t)((int64_t)(int32_t)left *
                                (int64_t)(uint64_t)right);
        } else if (op == 0x18) {         /* MPY32U */
            result = (uint64_t)left * (uint64_t)right;
        } else {                         /* MPY32US */
            result = (uint64_t)((int64_t)(uint64_t)left *
                                (int64_t)(int32_t)right);
        }
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        unsigned count = pair ? 2 : 1;
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned old_count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + count &&
                x->dst < x->out->loads[j].dst + old_count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due,
            .value = pair ? result : (uint32_t)result,
            .bank = x->side, .dst = x->dst, .size = pair ? 16 : 0
        };
    }
    return true;
}

static bool arm_mpy16(CdjC674xArm *x)
{
    /* Complete non-saturating 16x16 .M scalar family: MPY/H/HL/LH,
     * signed/unsigned permutations, and the two signed-constant
     * forms. Operands are sampled E1 and the scalar result is E2. */
    unsigned op = (x->w >> 7) & 31;
    uint32_t left_word = x->cpu->r[x->side][x->a];
    uint32_t right_word = x->cpu->r[x->cross][x->b];
    uint32_t result;
    switch (op) {
    case 0x18:
        result = (uint32_t)((int32_t)sx(x->a, 5) *
                            (int32_t)(int16_t)right_word); break;
    case 0x1e:
        result = (uint32_t)((int64_t)sx(x->a, 5) *
                            (uint16_t)right_word); break;
    case 0x01:
        result = (uint32_t)((int32_t)(int16_t)(left_word >> 16) *
                            (int32_t)(int16_t)(right_word >> 16)); break;
    case 0x09:
        result = (uint32_t)((int32_t)(int16_t)(left_word >> 16) *
                            (int32_t)(int16_t)right_word); break;
    case 0x0f:
        result = (uint32_t)((uint32_t)(uint16_t)(left_word >> 16) *
                            (uint16_t)right_word); break;
    case 0x0b:
        result = (uint32_t)((int64_t)(int16_t)(left_word >> 16) *
                            (uint16_t)right_word); break;
    case 0x03:
        result = (uint32_t)((int64_t)(int16_t)(left_word >> 16) *
                            (uint16_t)(right_word >> 16)); break;
    case 0x07:
        result = (uint32_t)((uint32_t)(uint16_t)(left_word >> 16) *
                            (uint16_t)(right_word >> 16)); break;
    case 0x0d:
        result = (uint32_t)((int64_t)(uint16_t)(left_word >> 16) *
                            (int16_t)right_word); break;
    case 0x05:
        result = (uint32_t)((int64_t)(uint16_t)(left_word >> 16) *
                            (int16_t)(right_word >> 16)); break;
    case 0x11:
        result = (uint32_t)((int32_t)(int16_t)left_word *
                            (int32_t)(int16_t)(right_word >> 16)); break;
    case 0x17:
        result = (uint32_t)((uint32_t)(uint16_t)left_word *
                            (uint16_t)(right_word >> 16)); break;
    case 0x13:
        result = (uint32_t)((int64_t)(int16_t)left_word *
                            (uint16_t)(right_word >> 16)); break;
    case 0x15:
        result = (uint32_t)((int64_t)(uint16_t)left_word *
                            (int16_t)(right_word >> 16)); break;
    case 0x1b:
        result = (uint32_t)((int64_t)(int16_t)left_word *
                            (uint16_t)right_word); break;
    case 0x1f:
        result = (uint32_t)((uint32_t)(uint16_t)left_word *
                            (uint16_t)right_word); break;
    case 0x1d:
        result = (uint32_t)((int64_t)(uint16_t)left_word *
                            (int16_t)right_word); break;
    default:
        result = (uint32_t)((int32_t)(int16_t)left_word *
                            (int32_t)(int16_t)right_word); break;
    }
    x->reg_write = false;
    if (x->enabled) {
        uint64_t due = x->cpu->cycles + 2;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + 1 && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = result, .bank = x->side, .dst = x->dst,
            .size = 0
        };
    }
    return true;
}

static bool arm_smpy16(CdjC674xArm *x)
{
    /* Saturating 16x16 .M family: SMPY (SPRUFE8B printed page 461,
     * opfield 1a), SMPYH (463, 02), SMPYHL (464, 0a), SMPYLH (466,
     * 12) and packed SMPY2 (468, Figure E-1 op 01).  All five
     * opfields came out of asm6x -mv6740.  As in the non-saturating
     * family just above, op bit 4 selects src1's low halfword and op
     * bit 3 src2's low halfword.  Scalar forms are single-cycle and
     * write dst in E2 (one delay slot); SMPY2 is four-cycle and
     * writes dst_o:dst_e in E4 (three delay slots).  Saturation sets
     * CSR.SAT and SSR.M1/M2 one cycle after dst is written
     * (SPRUFE8B 2.9.13, printed page 54: SSR bit 4 is M1, bit 5 M2). */
    unsigned op = (x->w >> 7) & 31;
    bool packed = (x->w & 0x7c) != 0;
    x->reg_write = false;
    if (packed && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        uint32_t left_word = x->cpu->r[x->side][x->a];
        uint32_t right_word = x->cpu->r[x->cross][x->b];
        CdjC674xMpyResult low = cdj_c674x_smpy16(
            left_word, right_word,
            !packed && !(op & 0x10), !packed && !(op & 8));
        CdjC674xMpyResult high = packed ?
            cdj_c674x_smpy16(left_word, right_word, true, true) : low;
        unsigned count = packed ? 2 : 1;
        uint64_t due = x->cpu->cycles + (packed ? 4 : 2);
        bool saturated = low.saturated || high.saturated;
        if (x->out->load_count + (saturated ? 2u : 1u) > 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned old_count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + count &&
                x->dst < x->out->loads[j].dst + old_count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due,
            .value = packed ? ((uint64_t)high.value << 32) | low.value
                            : low.value,
            .bank = x->side, .dst = x->dst, .size = packed ? 16u : 0u
        };
        if (saturated)
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = due + 1, .address = 1u << (4 + x->side),
                .size = CDJ_C674X_DELAYED_SAT
            };
    }
    return true;
}

static bool arm_mpy_half32(CdjC674xArm *x)
{
    /* MPYIH/MPYHI and MPYIL/MPYLI multiply a signed high/low
     * halfword by a signed 32-bit operand.  Their R variants add
     * 0x4000 and arithmetically shift by 15.  All sample in E1 and
     * write either one register or a full pair in E4. */
    unsigned op = (x->w >> 6) & 31;
    bool high = op == 0x10 || op == 0x14;
    bool pair = op == 0x14 || op == 0x15;
    x->reg_write = false;
    if (pair && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        int16_t half = high ? (int16_t)(x->cpu->r[x->side][x->a] >> 16)
                            : (int16_t)x->cpu->r[x->side][x->a];
        int64_t product = (int64_t)half * (int32_t)x->cpu->r[x->cross][x->b];
        uint64_t result = pair ? (uint64_t)product :
            arithmetic_shift_right64((uint64_t)(product + 0x4000), 15);
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        unsigned count = pair ? 2 : 1;
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned old_count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + count &&
                x->dst < x->out->loads[j].dst + old_count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = result, .bank = x->side, .dst = x->dst,
            .size = pair ? 16 : 0
        };
    }
    return true;
}

static bool arm_mvd(CdjC674xArm *x)
{
    /* MVD, SPRUFE8B p379: multiplier-path move, E1 source
     * sampled now, E4 destination written after three delay slots. */
    x->reg_write = false;
    if (x->enabled) {
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->w, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst <= x->dst && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->w, "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = x->cpu->r[x->cross][x->b],
            .bank = x->side, .dst = x->dst, .size = 0
        };
    }
    return true;
}

static bool arm_intsp(CdjC674xArm *x)
{
    /* INTSP/INTSPU read the integer in E1 and write binary32 in E4.
     * The FADCR rounding mode is sampled at issue; INEX becomes
     * sticky with the delayed result only when precision is lost. */
    x->reg_write = false;
    if (x->enabled) {
        bool inexact;
        bool signed_source = (x->w & 0x3cffc) == 0x958;
        unsigned rmode = (x->cpu->control[18] >> (x->side ? 25 : 9)) & 3;
        uint32_t result = cdj_c674x_integer_to_sp(x->cpu->r[x->cross][x->b],
                                        signed_source, rmode, &inexact);
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + 1 && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = result,
            .address = inexact ? 1u << (x->side ? 23 : 7) : 0,
            .bank = x->side, .dst = x->dst, .size = 0
        };
    }
    return true;
}

static bool arm_spint(CdjC674xArm *x)
{
    /* SPINT obeys FADCR; SPTRUNC always rounds toward zero.  Both
     * read in E1 and publish the integer and status in E4. */
    x->reg_write = false;
    if (x->enabled) {
        unsigned shift = x->side ? 16 : 0;
        unsigned rmode = (x->w & 0x20) ? 1 :
            (x->cpu->control[18] >> (shift + 9)) & 3;
        CdjC674xSpResult result = cdj_c674x_sp_to_integer(x->cpu->r[x->cross][x->b], rmode);
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + 1 && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = result.value,
            .address = result.status << shift,
            .bank = x->side, .dst = x->dst, .size = 0
        };
    }
    return true;
}

static bool arm_abssp(CdjC674xArm *x)
{
    /* ABSSP is the single-cycle .S auxiliary absolute operation.
     * It treats denormals as zero and records its warnings in FAUCR,
     * not FADCR.  No host floating-point operation is involved. */
    uint32_t source = x->cpu->r[x->cross][x->b];
    unsigned exponent = (source >> 23) & 255;
    uint32_t fraction = source & 0x7fffff;
    uint32_t status = 0;
    if (exponent == 255 && fraction) {
        x->value = 0x7fffffffu;
        status = 1u << 1;
        if (!(fraction & 0x400000)) status |= 1u << 4;
    } else if (!exponent && fraction) {
        x->value = 0;
        status = (1u << 7) | (1u << 3);
    } else {
        x->value = source & 0x7fffffffu;
        if (exponent == 255) status = 1u << 5;
    }
    if (x->enabled && status) {
        if (x->controls[19])
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel FAUCR status write conflict");
        x->out->control[19] |= status << (x->side ? 16 : 0);
        x->controls[19] = true;
    }
    return true;
}

static bool arm_cmpsp(CdjC674xArm *x)
{
    /* CMPEQSP/CMPGTSP/CMPLTSP are bit-exact single-cycle .S
     * comparisons.  Signed denormals compare as signed zero; NaNs
     * are unordered.  Warning bits are sticky in FAUCR. */
    unsigned relation = ((x->w >> 6) & 63) - 0x38;
    CdjC674xSpResult result = cdj_c674x_compare_sp(x->cpu->r[x->side][x->a],
                                 x->cpu->r[x->cross][x->b], relation);
    x->value = result.value;
    if (x->enabled && result.status) {
        if (x->controls[19])
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel FAUCR status write conflict");
        x->out->control[19] |= result.status << (x->side ? 16 : 0);
        x->controls[19] = true;
    }
    return true;
}

static bool arm_addsubsp(CdjC674xArm *x)
{
    /* ADDSP/SUBSP use FADCR on both .L and .S.  The .L reverse
     * subtract places the cross source in src1; the .S reverse form
     * computes encoded src2-src1.  Results and warnings appear E4. */
    unsigned encoding = x->w & 0xffc;
    unsigned operation = encoding == 0x218 || encoding == 0xe18 ? 0 :
                         encoding == 0xeb8 ? 2 : 1;
    uint32_t source1 = x->cpu->r[x->side][x->a];
    uint32_t source2 = x->cpu->r[x->cross][x->b];
    if (encoding == 0x2b8) {
        source1 = x->cpu->r[x->cross][x->a];
        source2 = x->cpu->r[x->side][x->b];
    }
    unsigned shift = x->side ? 16 : 0;
    unsigned rmode = (x->cpu->control[18] >> (shift + 9)) & 3;
    CdjC674xSpResult result = cdj_c674x_add_sub_sp(source1, source2, operation, rmode);
    x->reg_write = false;
    if (x->enabled) {
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + 1 && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = result.value,
            .address = result.status << shift,
            .bank = x->side, .dst = x->dst, .size = 0
        };
    }
    return true;
}

static bool arm_mpysp(CdjC674xArm *x)
{
    /* MPYSP uses FMCR and the same E1-read/E4-write timing as the
     * other four-cycle scalar operations. */
    x->reg_write = false;
    if (x->enabled) {
        unsigned shift = x->side ? 16 : 0;
        unsigned rmode = (x->cpu->control[20] >> (shift + 9)) & 3;
        CdjC674xSpResult result = cdj_c674x_multiply_sp(x->cpu->r[x->side][x->a],
                                      x->cpu->r[x->cross][x->b], rmode);
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + 1 && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = result.value,
            .address = result.status << shift,
            .bank = x->side, .dst = x->dst, .size = 0,
            .sign_extend = true
        };
    }
    return true;
}

static bool arm_pack(CdjC674xArm *x)
{
    /* PACK2/PACKH2/PACKHL2/PACKLH2 (.L/.S) and PACKL4/PACKH4
     * (.L), SPRUFE8B.
     * src1 supplies the upper result half and src2 the lower. */
    uint32_t left = x->cpu->r[x->side][x->a], right = x->cpu->r[x->cross][x->b];
    switch (x->w & 0xffc) {
    case 0x018: case 0xff0: /* PACK2 */
        x->value = left << 16 | (right & 0xffff); break;
    case 0x3d8: case 0x260: /* PACKH2 */
        x->value = (left & 0xffff0000) | (right >> 16); break;
    case 0x398: case 0x220: /* PACKHL2 */
        x->value = (left & 0xffff0000) | (right & 0xffff); break;
    case 0x378: case 0x420: /* PACKLH2 */
        x->value = left << 16 | (right >> 16); break;
    case 0xd18:             /* PACKL4 */
        x->value = (left & 0x00ff0000) << 8 | (left & 0xff) << 16 |
                (right & 0x00ff0000) >> 8 | (right & 0xff); break;
    default:                /* PACKH4 */
        x->value = (left & 0xff000000) | (left & 0x0000ff00) << 8 |
                (right & 0xff000000) >> 16 |
                (right & 0x0000ff00) >> 8; break;
    }
    return true;
}

static bool arm_andn(CdjC674xArm *x)
{
    /* ANDN is available on .L/.S/.D with identical single-cycle
     * semantics: src1 AND the bitwise inverse of src2. */
    x->value = x->cpu->r[x->side][x->a] & ~x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_and_imm(CdjC674xArm *x)
{
    x->value = (uint32_t)sx(x->a, 5) & x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_and_reg(CdjC674xArm *x)
{
    x->value = x->cpu->r[x->side][x->a] & x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_add_imm(CdjC674xArm *x)
{
    x->value = (uint32_t)sx(x->a, 5) + x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_add_reg(CdjC674xArm *x)
{
    x->value = x->cpu->r[x->side][x->a] + x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_sub_imm(CdjC674xArm *x)
{
    x->value = (uint32_t)sx(x->a, 5) - x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_sub_reg(CdjC674xArm *x)
{
    x->value = x->cpu->r[x->side][x->a] - x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_sub_reverse(CdjC674xArm *x)
{
    /* Reverse-cross .L SUB encodes its cross source in src1 and its local
     * source in src2.  The .S reverse encoding instead follows the printed
     * page 529 operand map: src2(xsint) - src1(sint), so its cross source is
     * the b field and its local source is the a field. */
    if ((x->w & 0xffc) == 0xd70)
        x->value = x->cpu->r[x->cross][x->b] - x->cpu->r[x->side][x->a];
    else
        x->value = x->cpu->r[x->cross][x->a] - x->cpu->r[x->side][x->b];
    return true;
}

static bool arm_cmp(CdjC674xArm *x)
{
    /* CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU scalar .L forms.  Even
     * opfields use a signed or unsigned five-bit constant; odd
     * opfields read src1 from the local register file. */
    unsigned encoding = x->w & 0xffc;
    bool immediate = !(encoding & 0x20);
    uint32_t left = immediate ? x->a : x->cpu->r[x->side][x->a];
    uint32_t right = x->cpu->r[x->cross][x->b];
    switch (encoding & ~0x20u) {
    case 0xa58: x->value = (immediate ? (uint32_t)sx(x->a, 5) : left) == right; break;
    case 0x8d8: x->value = (immediate ? sx(x->a, 5) : (int32_t)left) >
                        (int32_t)right; break;
    case 0x9d8: x->value = left > right; break;
    case 0xad8: x->value = (immediate ? sx(x->a, 5) : (int32_t)left) <
                        (int32_t)right; break;
    default:    x->value = left < right; break;
    }
    return true;
}

static bool arm_or_imm(CdjC674xArm *x)
{
    x->value = (uint32_t)sx(x->a, 5) | x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_or_reg(CdjC674xArm *x)
{
    x->value = x->cpu->r[x->side][x->a] | x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_xor_imm(CdjC674xArm *x)
{
    x->value = (uint32_t)sx(x->a, 5) ^ x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_xor_reg(CdjC674xArm *x)
{
    x->value = x->cpu->r[x->side][x->a] ^ x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_bitfield(CdjC674xArm *x)
{
    /* SPRUFE8B CLR/SET/EXT/EXTU: constant .S field format or
     * register counts packed as src1[9:5],src1[4:0]. EXT counts
     * describe left/right shifts, NOT low/high bit indices. */
    bool immediate = (x->w & 0x3c) == 8;
    unsigned op = immediate ? (x->w >> 6) & 3 : ((x->w >> 9) & 2) | ((x->w >> 8) & 1);
    uint32_t fields = x->cpu->r[x->side][x->a];
    if (!immediate && x->enabled && (fields & ~1023u))
        return stop(x->cpu, x->pc, x->insn->word, "invalid bit-field register counts");
    unsigned left = immediate ? x->a : (fields >> 5) & 31;
    unsigned right = immediate ? (x->w >> 8) & 31 : fields & 31;
    uint32_t source = x->cpu->r[immediate ? x->side : x->cross][x->b];
    if (op < 2) {
        uint32_t shifted = source << left;
        x->value = shifted >> right;
        if (op == 1 && right && (shifted & 0x80000000u))
            x->value |= UINT32_MAX << (32 - right);
    } else {
        uint32_t mask = right < left ? 0 :
            (UINT32_MAX << left) & (UINT32_MAX >> (31 - right));
        x->value = op == 2 ? source | mask : source & ~mask;
    }
    return true;
}

static bool arm_shift_s(CdjC674xArm *x)
{
    /* Scalar .S shifts; register counts use only six low bits. */
    unsigned n = (x->w & 0x40) ? (x->cpu->r[x->side][x->a] & 63) : x->a;
    uint32_t source = x->cpu->r[x->cross][x->b];
    unsigned op = x->w & 0xfbc;
    x->value = shift_32(source, n, op == 0xca0 ? 0 : op == 0x9a0 ? 2 : 1);
    return true;
}

static bool arm_mvc_read(CdjC674xArm *x)
{
    /* MVC control-register to B-register.  The crhi field is zero
     * for every implemented C674x control register ID. */
    if (!cdj_c674x_control_read_supported(x->b))
        return stop(x->cpu, x->pc, x->insn->word,
                    "control register read not implemented");
    x->value = cdj_c674x_control_read(x->cpu, x->b);
    /* SPRUFE8B 2.9.14.4: reading TSCL snapshots the simultaneously observed
     * high half into TSCH. Keep it in the transactional copy so a later
     * packet rejection, or a false predicate, has no architectural effect. */
    if (x->enabled && x->b == 10)
        x->out->control[16] = (uint32_t)(cdj_c674x_timestamp(x->cpu) >> 32);
    return true;
}

static bool arm_mvc_write(CdjC674xArm *x)
{
    /* MVC register to control-register.  Interrupt-control writes
     * below apply architectural masks rather than acting as storage. */
    if (!cdj_c674x_control_write_supported(x->dst))
        return stop(x->cpu, x->pc, x->insn->word,
                    "control register write not implemented");
    x->control_write = true; x->reg_write = false; x->value = x->cpu->r[x->cross][x->b];
    return true;
}

static bool arm_b_irp(CdjC674xArm *x)
{
    /* B IRP, SPRUFE8B pp.155-156 and 5.3.4.3. The return branch has
     * five delay slots. ITSR is restored to TSR in E1; ITSR.GIE is
     * the physical CSR.PGIE bit, and PGIE itself remains unchanged. */
    x->reg_write = false;
    if (x->enabled) {
        if (x->controls[1] || x->controls[26] || x->controls[27])
            return stop(x->cpu, x->pc, x->insn->word,
                        "B IRP parallel task-state write conflict");
        if (!queue_branch(x->out, x->cpu->cycles + 6, x->cpu->control[6]))
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel taken branches or branch queue overflow");
        uint32_t restored = (x->cpu->control[27] & 0x0000c6deu) |
                            ((x->cpu->control[1] >> 1) & 1u);
        x->out->control[26] = restored;
        x->out->control[1] = (x->out->control[1] & ~1u) | (restored & 1u);
        x->controls[1] = x->controls[26] = x->controls[27] = true;
    }
    return true;
}

static bool arm_b_disp(CdjC674xArm *x)
{
    x->reg_write = false;
    if (x->enabled) {
        if (!queue_branch(x->out, x->cpu->cycles + 6, (x->pc & ~31u) + (uint32_t)(sx((x->w >> 7) & 0x1fffff, 21) * 4)))
            return stop(x->cpu, x->pc, x->insn->word, "parallel taken branches or branch queue overflow");
    }
    return true;
}

static bool arm_bdec(CdjC674xArm *x)
{
    /* BDEC, SPRUFE8B pp159-160: signed nonnegative counter,
     * word-scaled fetch-relative target, and five delay slots.
     * Both operands are read before any parallel packet writes. */
    for (unsigned j = 0; j < x->packet->count; ++j) {
        const CdjC674xInstruction *other = &x->packet->instructions[j];
        if (!other->compact && (other->word & 0x1ffe) == 0x162)
            return stop(x->cpu, x->pc, x->w, "BDEC parallel with ADDKPC");
    }
    x->reg_write = false;
    if (x->enabled) {
        if ((*x->bdec_issued))
            return stop(x->cpu, x->pc, x->w, "multiple BDEC instructions");
        (*x->bdec_issued) = true;
        if (!(x->cpu->r[x->side][x->dst] & 0x80000000u)) {
            if (!queue_branch(x->out, x->cpu->cycles + 6,
                    (x->pc & ~31u) + (uint32_t)(sx((x->w >> 13) & 1023, 10) * 4)))
                return stop(x->cpu, x->pc, x->w, "parallel taken branches or branch queue overflow");
            x->value = x->cpu->r[x->side][x->dst] - 1u;
            x->reg_write = true;
        }
    }
    return true;
}

static bool arm_bnop_disp(CdjC674xArm *x)
{
    /* SPRUFE8B BNOP displacement, pp165-167: NOPs are unconditional.
     * A 32-bit BNOP in a header-based fetch packet uses halfword
     * displacement units; without a header it uses words. */
    unsigned n = (x->w >> 13) & 7;
    x->reg_write = false;
    if (!cdj_c674x_packet_multicycle(x->timing, n + 1))
        return stop(x->cpu, x->pc, x->insn->word, "multiple multicycle instructions");
    if (x->enabled && !queue_branch(x->out, x->cpu->cycles + 6,
            (x->pc & ~31u) + (uint32_t)(sx((x->w >> 16) & 4095, 12) *
                                   (x->insn->header ? 2 : 4))))
        return stop(x->cpu, x->pc, x->insn->word, "parallel taken branches or branch queue overflow");
    return true;
}

static bool arm_bnop_reg(CdjC674xArm *x)
{
    unsigned n = (x->w >> 13) & 7;
    x->reg_write = false;
    if (!cdj_c674x_packet_multicycle(x->timing, n + 1))
        return stop(x->cpu, x->pc, x->insn->word, "multiple multicycle instructions");
    if (x->enabled) {
        if (!queue_branch(x->out, x->cpu->cycles + 6, x->cpu->r[x->cross][x->b]))
            return stop(x->cpu, x->pc, x->insn->word, "parallel taken branches or branch queue overflow");
    }
    return true;
}

static bool arm_b_reg(CdjC674xArm *x)
{
    x->reg_write = false;
    if (x->enabled) {
        if (!queue_branch(x->out, x->cpu->cycles + 6, x->cpu->r[((x->w >> 12) & 1) ^ 1][x->b]))
            return stop(x->cpu, x->pc, x->insn->word, "parallel taken branches or branch queue overflow");
    }
    return true;
}

static bool arm_addkpc(CdjC674xArm *x)
{
    if (!x->side) return stop(x->cpu, x->pc, x->insn->word, "ADDKPC requires S2");
    x->value = (x->pc & ~31u) + (uint32_t)(sx((x->w >> 16) & 127, 7) * 4);
    unsigned n = 1 + ((x->w >> 13) & 7);
    if (x->enabled && !cdj_c674x_packet_multicycle(x->timing, n))
        return stop(x->cpu, x->pc, x->insn->word, "multiple multicycle instructions");
    return true;
}

/* Shared helpers defined further down, declared here because the family
 * markers below come before them: dp_queue is the delayed double-precision
 * pair write, and queue_saturation is the CSR.SAT / SSR[unit] effect of a
 * saturating .L instruction. */
static bool dp_queue(CdjC674xArm *x, uint64_t due, unsigned dst,
                     uint32_t value, uint32_t status, bool multiplier);
static bool queue_saturation(CdjC674xArm *x);

/* ---- wave 5 arm bodies ---------------------------------------------------
 *
 * One anchor per instruction family, so independently developed families insert
 * at distinct lines and their patches do not collide.  Arithmetic goes in the
 * family's own pure file (see cdj_c674x_mpy.c for the pattern): an arm here only
 * moves operands in and the result out, and must not touch anything at or past
 * offsetof(CdjC674x, loop).  Add the matching table row under the SAME family
 * marker inside cdj_c674x_arms[] below, then run
 * tests/test_c674x.py::test_c674x_dispatch_table_has_no_shadowed_rows - a row
 * that overlaps an existing one is otherwise silently unreachable. */
/* wave5-arms: dot-product and complex-multiply */
static bool arm_cmpy(CdjC674xArm *x)
{
    /* The nonconditional .M group of Figure E-3 (printed page 743): CMPY,
     * CMPYR, CMPYR1, DDOTP4 and the four DDOTP*2 forms.  Reached through
     * CDJ_C674X_UNCOND_ARM_TABLE, because bits 31-28 are the literal 0001
     * opcode field rather than creg/z, so these are unconditional and the
     * predicate path never applies to them.
     *
     * All are four-cycle with three delay slots (printed pages 215, 217, 219,
     * 221, 223, 225, 227, 229), queued through the same delayed-result path
     * arm_dotp uses.  Which registers are read and written depends on the
     * opfield alone, so the shapes are taken from a probe rather than guessed:
     * the DDOTP*2 forms read a src1 PAIR, and CMPY, DDOTP4, DDOTPH2 and
     * DDOTPL2 write a dst pair while the rounding forms pack two rounded
     * halves into a 32-bit dst. */
    unsigned op = (x->w >> 6) & 31;
    bool mpy32 = op == CDJ_C674X_SMPY32 || op == CDJ_C674X_MPY2IR;
    CdjC674xCmpyResult shape = mpy32
        ? cdj_c674x_mpy32_nonconditional(op, 0, 0)
        : cdj_c674x_cmpy(op, 0, 0, 0);
    x->reg_write = false;
    if (!shape.valid)
        return stop(x->cpu, x->pc, x->insn->word,
                    "nonconditional .M opfield not implemented");
    if (shape.pair_dst && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (shape.pair_src1 && (x->a & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply source register pair");
    if (x->enabled) {
        uint32_t src1 = x->cpu->r[x->side][x->a];
        uint32_t src1_hi = shape.pair_src1 ? x->cpu->r[x->side][x->a + 1] : 0;
        CdjC674xCmpyResult r = mpy32
            ? cdj_c674x_mpy32_nonconditional(op, src1, x->cpu->r[x->cross][x->b])
            : cdj_c674x_cmpy(op, src1, src1_hi, x->cpu->r[x->cross][x->b]);
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        unsigned count = r.pair_dst ? 2 : 1;
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned old_count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + count &&
                x->dst < x->out->loads[j].dst + old_count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due,
            .value = r.pair_dst ? r.value : (uint32_t)r.value,
            .bank = x->side, .dst = x->dst, .size = r.pair_dst ? 16u : 0u
        };
    }
    return true;
}

/* Figure E-3: bit 11 is 0, opfield bits 10-6, bits 5-2 are 1100, and bits
 * 31-28 are the literal 0001.  The opfield set is the one cdj_c674x_cmpy
 * implements; anything else in that shape is still classified UNIMPLEMENTED
 * by cdj_c674x_uncond_classify and never reaches this row. */
static bool match_cmpy(const CdjC674xArm *x)
{
    unsigned op = (x->w >> 6) & 31;
    return cdj_c674x_cmpy(op, 0, 0, 0).valid ||
           cdj_c674x_mpy32_nonconditional(op, 0, 0).valid;
}

/* Included here rather than in the file's header block so that this family's
 * whole edit to cdj_c674x.c stays inside its own anchor. */

/* The seven packed dot-product opfields of Figure E-1's compound .M format
 * (bit 11 zero, opfield bits 10-6, bits 5-2 = 1100).  Disjoint from the
 * opfields the earlier compound-.M rows claim - 01 SMPY2, 0e/10/14/15 the
 * MPYIH/MPYHI/MPYIL/MPYLI family, 18/19 MPY32U/MPY32US, 03 MVD - so this row
 * is reachable behind all of them. */
static bool match_dotp(const CdjC674xArm *x)
{
    switch ((x->w >> 6) & 31) {
    case CDJ_C674X_DOTPSU4:   /* printed page 249, also DOTPUS4 page 251 */
    case CDJ_C674X_DOTPU4:    /* printed page 252 */
    case CDJ_C674X_DOTPNRSU2: /* printed page 240, also DOTPNRUS2 page 242 */
    case CDJ_C674X_DOTPN2:    /* printed page 238 */
    case CDJ_C674X_DOTP2L:    /* printed page 235, dst_o:dst_e */
    case CDJ_C674X_DOTP2:     /* printed page 235, 32-bit dst */
    case CDJ_C674X_DOTPRSU2:  /* printed page 244, also DOTPRUS2 page 247 */
        return true;
    default:
        return false;
    }
}

static bool arm_dotp(CdjC674xArm *x)
{
    /* Every entry in this family reads src1 and src2 in E1 and writes dst in
     * E4: "Instruction Type Four-cycle, Delay Slots 3" on printed pages 236,
     * 238, 241, 243, 245, 247, 250, 251 and 253.  src1 is the local operand
     * and src2 the cross-path one (operand types s2/xs2, s4/xu4, u4/xu4 in
     * every opcode map). */
    unsigned op = (x->w >> 6) & 31;
    bool pair = op == CDJ_C674X_DOTP2L;
    x->reg_write = false;
    if (pair && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        CdjC674xDotpResult r = cdj_c674x_dotp(op, x->cpu->r[x->side][x->a],
                                              x->cpu->r[x->cross][x->b]);
        if (!r.valid)
            return stop(x->cpu, x->pc, x->insn->word,
                        "dot-product opfield not implemented");
        if (r.undefined)
            return stop(x->cpu, x->pc, x->insn->word,
                        "DOTPRSU2/DOTPNRSU2 intermediate overflows: SPRUFE8B "
                        "printed page 244 leaves the result undefined");
        uint64_t due = x->cpu->cycles + 4;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        unsigned count = pair ? 2 : 1;
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned old_count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + count &&
                x->dst < x->out->loads[j].dst + old_count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due,
            .value = pair ? r.value : (uint32_t)r.value,
            .bank = x->side, .dst = x->dst, .size = pair ? 16 : 0
        };
    }
    return true;
}
/* wave5-arms: packed 16-bit */
/* The family header is included here rather than at the top of the file so
 * that the whole packed 16-bit change stays inside this family's anchor. */
#include "cdj_c674x_packed16.h"

static bool arm_packed16(CdjC674xArm *x)
{
    /* Packed 16-bit single-cycle operations.  Every one of these reads
     * src1/src2 in E1 and writes dst in E1 with zero delay slots, and none
     * of them touches CSR.SAT or SSR (SPRUFE8B printed pages: ABS2 103-104,
     * ADD2 137-139, SUB2 548-550, SADD2 425-426, SSUB2 502-503,
     * SADDUS2 433-434, MAX2 306-308, MIN2 311-313, SHR2 453-454,
     * SHRU2 459-460, CMPEQ2 179-180, CMPGT2 191-192, SPACK2 472-473).
     * src1 is always the local register file and src2 the cross path.
     * ADD2 and SUB2 decode on .S, .L and .D; MAX2 and MIN2 on .L and .S. */
    uint32_t left = x->cpu->r[x->side][x->a];
    uint32_t right = x->cpu->r[x->cross][x->b];
    switch (x->w & 0xffc) {
    case 0x358:                                   /* ABS2  .L        p103 */
        x->value = cdj_c674x_abs2(right); break;
    case 0x060: case 0x0b8: case 0x930:           /* ADD2  .S/.L/.D  p137 */
        x->value = cdj_c674x_add2(left, right); break;
    case 0x460: case 0x098: case 0x970:           /* SUB2  .S/.L/.D  p548 */
        x->value = cdj_c674x_sub2(left, right); break;
    case 0xc30:                                   /* SADD2 .S        p425 */
        x->value = cdj_c674x_sadd2(left, right); break;
    case 0xc98:                                   /* SSUB2 .L        p502 */
        x->value = cdj_c674x_ssub2(left, right); break;
    case 0xc70:                                   /* SADDUS2 .S      p433 */
        x->value = cdj_c674x_saddus2(left, right); break;
    case 0x858: case 0xf70:                       /* MAX2  .L/.S     p306 */
        x->value = cdj_c674x_max2(left, right); break;
    case 0x838: case 0xf30:                       /* MIN2  .L/.S     p311 */
        x->value = cdj_c674x_min2(left, right); break;
    case 0xdf0:                                   /* SHR2  .S uint   p453 */
        x->value = cdj_c674x_shr2(right, left); break;
    case 0x620:                                   /* SHR2  .S ucst5  p453 */
        x->value = cdj_c674x_shr2(right, x->a); break;
    case 0xe30:                                   /* SHRU2 .S uint   p459 */
        x->value = cdj_c674x_shru2(right, left); break;
    case 0x660:                                   /* SHRU2 .S ucst5  p459 */
        x->value = cdj_c674x_shru2(right, x->a); break;
    case 0x760:                                   /* CMPEQ2 .S       p179 */
        x->value = cdj_c674x_cmpeq2(left, right); break;
    case 0x520:                                   /* CMPGT2 .S       p191 */
        x->value = cdj_c674x_cmpgt2(left, right); break;
    case 0xcb0:                                   /* SPACK2 .S       p472 */
        x->value = cdj_c674x_spack2(left, right); break;
    default:
        /* Unreachable through cdj_c674x_arms[], whose rows match this
         * opfield exactly.  Fail closed rather than silently computing some
         * other member's arithmetic if a row is ever added without a case. */
        return stop(x->cpu, x->pc, x->insn->word, "instruction not implemented");
    }
    return true;
}

static bool arm_packed16_m(CdjC674xArm *x)
{
    /* Two-cycle .M forms: AVG2 (printed pages 147-148) and SSHVL/SSHVR
     * (495-498) read src1/src2 in E1 and write dst in E2, one delay slot.
     * SSHVL/SSHVR saturate a left shift; printed page 495 NOTE: "If the
     * shifted value is saturated, then the SAT bit is set in CSR one cycle
     * after the result is written to dst."  SPRUFE8B 2.9.13 (printed page
     * 54) makes the same cycle set this unit's SSR flag: bit 4 is M1 and
     * bit 5 is M2.  AVG2 states "No overflow conditions exist". */
    unsigned op = x->w & 0xffc;
    x->reg_write = false;
    if (x->enabled) {
        uint32_t left = x->cpu->r[x->side][x->a];
        uint32_t right = x->cpu->r[x->cross][x->b];
        uint32_t value;
        bool saturated = false;
        if (op == 0x4f0) {                        /* AVG2  .M        p147 */
            value = cdj_c674x_avg2(left, right);
        } else {                       /* SSHVL 0x730 p495, SSHVR 0x6b0 p497 */
            CdjC674xPacked16Sat shift =
                cdj_c674x_sshv(right, left, op == 0x6b0);
            value = shift.value;
            saturated = shift.saturated;
        }
        uint64_t due = x->cpu->cycles + 2;
        if (x->out->load_count + (saturated ? 2u : 1u) > 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst <= x->dst && x->dst < x->out->loads[j].dst + count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = value, .bank = x->side, .dst = x->dst,
            .size = 0
        };
        if (saturated)
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = due + 1, .address = 1u << (4 + x->side),
                .size = CDJ_C674X_DELAYED_SAT
            };
    }
    return true;
}
/* wave5-arms: packed 8-bit */
/* Packed 8-bit (4x8) operands on .L, .S and .M.  The arithmetic is in
 * cdj_c674x_packed8.c; what belongs here is the opfield-to-operation map, the
 * register-pair rule and the pipeline latency, all of which differ between
 * near-identical mnemonics in this family.
 *
 * Every entry in the family reads src1 and src2 in E1 (their Pipeline tables).
 * The write stage is NOT uniform, so it is carried per opfield below:
 *   E1, "Delay Slots 0"  ADD4 141, SUB4 552, SUBABS4 535, SADDU4 436,
 *                        MAXU4 310, MINU4 315, CMPEQ4 182, CMPGTU4 200,
 *                        SPACKU4 475
 *   E2, "Delay Slots 1"  AVGU4 150 ("Two-cycle")
 *   E4, "Delay Slots 3"  MPYU4 361, MPYSU4 358 ("Four-cycle"), which also
 *                        write the 64-bit dst_o:dst_e register pair.
 * The opfield values are bits 11-2 of each entry's 32-bit Opcode figure,
 * which is also what the table rows below match on. */
static bool arm_packed8(CdjC674xArm *x)
{
    uint32_t src1 = x->cpu->r[x->side][x->a];
    uint32_t src2 = x->cpu->r[x->cross][x->b];
    uint64_t result;
    unsigned stage;
    bool pair = false;
    switch (x->w & 0xffc) {
    case 0xcb8: result = cdj_c674x_add4(src1, src2);     stage = 1; break;
    case 0xcd8: result = cdj_c674x_sub4(src1, src2);     stage = 1; break;
    case 0xb58: result = cdj_c674x_subabs4(src1, src2);  stage = 1; break;
    case 0xcf0: result = cdj_c674x_saddu4(src1, src2);   stage = 1; break;
    case 0x878: result = cdj_c674x_maxu4(src1, src2);    stage = 1; break;
    case 0x918: result = cdj_c674x_minu4(src1, src2);    stage = 1; break;
    case 0x720: result = cdj_c674x_cmpeq4(src1, src2);   stage = 1; break;
    case 0x560: result = cdj_c674x_cmpgtu4(src1, src2);  stage = 1; break;
    case 0xd30: result = cdj_c674x_spacku4(src1, src2);  stage = 1; break;
    case 0x4b0: result = cdj_c674x_avgu4(src1, src2);    stage = 2; break;
    case 0x130: result = cdj_c674x_mpyu4(src1, src2);
        stage = 4; pair = true; break;
    case 0x170: result = cdj_c674x_mpysu4(src1, src2);
        stage = 4; pair = true; break;
    default:
        /* Unreachable while the table rows below enumerate the opfields, and
         * fail-closed rather than silently computing something else if a
         * future row widens a mask. */
        return stop(x->cpu, x->pc, x->insn->word, "instruction not implemented");
    }
    if (stage == 1) {              /* E1: the commit step writes x->value. */
        x->value = (uint32_t)result;
        return true;
    }
    x->reg_write = false;
    /* MPYU4/MPYSU4 dst is dst_o:dst_e, an even/odd pair (printed pages 360,
     * 357), so an odd dst names no architectural pair. */
    if (pair && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        uint64_t due = x->cpu->cycles + stage;
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
        unsigned count = pair ? 2 : 1;
        for (unsigned j = 0; j < x->out->load_count; ++j) {
            unsigned old_count = queued_result_registers(&x->out->loads[j]);
            if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
                x->out->loads[j].dst < x->dst + count &&
                x->dst < x->out->loads[j].dst + old_count)
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel delayed-result write conflict");
        }
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = due, .value = pair ? result : (uint32_t)result,
            .bank = x->side, .dst = x->dst, .size = pair ? 16 : 0
        };
    }
    return true;
}
/* wave5-arms: pack, unpack, shuffle and bit manipulation */

/* Queue one 32-bit result for the E2 write of a two-cycle .M instruction, the
 * way the other E2 .M arms above do: SPRUFE8B gives BITC4 (printed page 162),
 * BITR (163), DEAL (232), SHFL (444), XPND2 (569), XPND4 (571) and ROTL (415)
 * all "Instruction Type Two-cycle / Delay Slots 1", with dst written in E2 and
 * every Example headed "2 cycles after instruction". */
static bool packbits_queue_e2(CdjC674xArm *x, uint32_t result)
{
    uint64_t due = x->cpu->cycles + 2;
    if (x->out->load_count == 40)
        return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
    for (unsigned j = 0; j < x->out->load_count; ++j) {
        unsigned count = queued_result_registers(&x->out->loads[j]);
        if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
            x->out->loads[j].dst < x->dst + 1 &&
            x->dst < x->out->loads[j].dst + count)
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel delayed-result write conflict");
    }
    x->out->loads[x->out->load_count++] = (CdjC674xLoad){
        .due = due, .value = result, .address = 0,
        .bank = x->side, .dst = x->dst, .size = 0
    };
    return true;
}

static bool arm_packbits_m(CdjC674xArm *x)
{
    /* The .M-unit src2-only group - bits 17-13 select the operation while bits
     * 11-2 are the shared 0F0h - plus ROTL, whose opfield lives in bits 10-6.
     * BITR 163, BITC4 161, DEAL 231, SHFL 443, XPND2 568, XPND4 570, ROTL 414.
     * src2 is the cross-capable operand on all seven; ROTL's src1 is local. */
    uint32_t src2 = x->cpu->r[x->cross][x->b], result;
    if ((x->w & 0xffc) == 0x770 || (x->w & 0xffc) == 0x7b0) {
        /* ROTL opfield 11101 reads src1 from a register, 11110 takes a ucst5
         * in the same field (printed page 414 opcode map). */
        uint32_t src1 = (x->w & 0xffc) == 0x770 ?
            x->cpu->r[x->side][x->a] : x->a;
        result = cdj_c674x_rotl(src2, src1);
    } else switch ((x->w >> 13) & 31) {
    case 0x1f: result = cdj_c674x_bitr(src2); break;
    case 0x1e: result = cdj_c674x_bitc4(src2); break;
    case 0x1d: result = cdj_c674x_deal(src2); break;
    case 0x1c: result = cdj_c674x_shfl(src2); break;
    case 0x19: result = cdj_c674x_xpnd2(src2); break;
    default:   result = cdj_c674x_xpnd4(src2); break;
    }
    x->reg_write = false;
    return !x->enabled || packbits_queue_e2(x, result);
}

static bool arm_packbits_unpack(CdjC674xArm *x)
{
    /* UNPKHU4 (printed page 559), UNPKLU4 (561) and SWAP4 (555).  All three are
     * "Single cycle / Delay Slots 0" with src2 read and dst written in E1, so
     * the ordinary E1 register write applies.  Bits 17-13 pick the operation
     * and are identical between the .L and .S opcode figures. */
    uint32_t src2 = x->cpu->r[x->cross][x->b];
    switch ((x->w >> 13) & 31) {
    case 3:  x->value = cdj_c674x_unpkhu4(src2); break;
    case 2:  x->value = cdj_c674x_unpklu4(src2); break;
    default: x->value = cdj_c674x_swap4(src2); break;
    }
    return true;
}

static bool arm_packbits_mergebyte(CdjC674xArm *x)
{
    /* SHLMB (printed page 449) and SHRMB (455) on .L and .S: single-cycle,
     * zero delay slots, src1 local u4 and src2 the cross-capable xu4. */
    uint32_t src1 = x->cpu->r[x->side][x->a];
    uint32_t src2 = x->cpu->r[x->cross][x->b];
    bool left = (x->w & 0xffc) == 0xc38 || (x->w & 0xffc) == 0xe70;
    x->value = left ? cdj_c674x_shlmb(src1, src2)
                    : cdj_c674x_shrmb(src1, src2);
    return true;
}

static bool arm_packbits_lmbd(CdjC674xArm *x)
{
    /* LMBD .L (printed page 304): single-cycle, zero delay slots.  The
     * register-src1 form uses opfield 110 1011; the cst5 form uses 110 1010
     * and carries the search bit in the same five-bit field as x->a.  LMBD
     * only consumes the source's LSB, so the unsigned constant's value is
     * represented directly by that field. */
    uint32_t src1 = (x->w & 0xffc) == 0xd58 ? x->a
                                             : x->cpu->r[x->side][x->a];
    x->value = cdj_c674x_lmbd(src1,
                              x->cpu->r[x->cross][x->b]);
    return true;
}

static bool arm_packbits_norm(CdjC674xArm *x)
{
    /* NORM .L (printed page 390): single-cycle, zero delay slots.  Opfield
     * 110 0011 normalizes an xsint src2; 110 0000 normalizes a 40-bit slong in
     * an even/odd pair. */
    if (((x->w >> 5) & 0x7f) == 0x63) {
        x->value = cdj_c674x_norm32(x->cpu->r[x->cross][x->b]);
        return true;
    }
    if (x->b & 1)
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    /* A 40-bit operand consumes the .L unit's local long-data input; a cross
     * path carries only one 32-bit operand (SPRUFE8B 2.3), which is why the
     * other long .L forms above refuse x = 1 rather than read the local pair. */
    if (x->w & 0x1000)
        return stop(x->cpu, x->pc, x->insn->word,
                    "cross-path long operand not supported");
    x->value = cdj_c674x_norm40(register_long40(x->cpu, x->side, x->b));
    return true;
}

static bool arm_packbits_dual(CdjC674xArm *x)
{
    /* DPACK2 (printed page 254), DPACKX2 (256) and SHFL3 (445): nonconditional
     * .L encodings - bits 31-28 are the literal 0001 opcode field, so creg is
     * zero and the instruction always executes - that are "Single-cycle / Delay
     * Slots 0" and write dst_o:dst_e in E1.  DPACK2 and DPACKX2 spend only bits
     * 27-24 on dst and reserve bit 23 as 0, so the 5-bit dst the decoder hands
     * us is already the even register of the pair; SHFL3 uses all five bits and
     * must still name one. */
    uint32_t src1 = x->cpu->r[x->side][x->a];
    uint32_t src2 = x->cpu->r[x->cross][x->b];
    uint64_t result;
    x->reg_write = false;
    if (x->dst & 1)
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    /* The four dual ADD/SUB forms share this arm: same unit, same format, same
     * single-cycle dst_o:dst_e write.  Only SADDSUB reports saturation - its
     * page states the CSR.SAT and SSR effect positively, while SADDSUB2's note
     * exempts the packed form from both. */
    unsigned dual = (x->w >> 5) & 0x7fu;
    bool saturated = false;
    switch (x->w & 0xffc) {
    case 0x698: result = cdj_c674x_dpack2(src1, src2); break;
    case 0x678: result = cdj_c674x_dpackx2(src1, src2); break;
    case 0x6d8: result = cdj_c674x_shfl3(src1, src2); break;
    default: {
        CdjC674xAddsubResult r = cdj_c674x_addsub(dual, src1, src2);
        if (!r.valid)
            return stop(x->cpu, x->pc, x->insn->word,
                        "nonconditional .L opfield not implemented");
        result = r.value;
        saturated = r.saturated;
        break;
    }
    }
    if (x->written[x->side][x->dst] || x->written[x->side][x->dst + 1])
        return stop(x->cpu, x->pc, x->insn->word,
                    "parallel register write conflict");
    x->out->r[x->side][x->dst] = (uint32_t)result;
    x->out->r[x->side][x->dst + 1] = (uint32_t)(result >> 32);
    x->written[x->side][x->dst] = x->written[x->side][x->dst + 1] = true;
    /* SADDSUB, printed page 427: "If either result saturates, the L1 or L2 bit
     * in SSR and the SAT bit in CSR are written one cycle after the results are
     * written to dst_o:dst_e." */
    if (saturated && !queue_saturation(x)) return false;
    return true;
}
/* wave5-arms: double-precision floating point */
static bool arm_approx(CdjC674xArm *x)
{
    /* RCPSP (printed pages 411-412), RSQRSP (420-421), RCPDP (409-410) and
     * RSQRDP (418-419): the .S-unit reciprocal and reciprocal-square-root
     * APPROXIMATIONS.  Arithmetic and every special case in
     * cdj_c674x_approx.c, which also carries the declaration that the mantissa
     * below the eighth binary position is not hardware-exact.
     *
     * Latency differs between the two widths and is taken from each entry:
     * the single-precision forms are "Delay Slots 0" and write dst on E1; the
     * double-precision forms are "Delay Slots 1" with dst_l on E1 and dst_h on
     * E2, the same shape arm_absdp uses. */
    unsigned encoding = x->w & 0xffcu;
    CdjC674xApproxKind kind = encoding == 0xf60u ? CDJ_C674X_RCPSP :
                              encoding == 0xfa0u ? CDJ_C674X_RSQRSP :
                              encoding == 0xb60u ? CDJ_C674X_RCPDP :
                                                   CDJ_C674X_RSQRDP;
    bool pair = cdj_c674x_approx(kind, 0).pair;
    if (pair && ((x->dst & 1) || !(x->b & 1)))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision register pair");
    uint64_t src2 = pair
        ? (uint64_t)x->cpu->r[x->cross][x->b] << 32 | x->cpu->r[x->cross][x->b - 1]
        : x->cpu->r[x->cross][x->b];
    CdjC674xApproxResult r = cdj_c674x_approx(kind, src2);
    x->value = (uint32_t)r.value;                 /* dst, or dst_l on E1 */
    if (x->enabled) {
        if (pair && !dp_queue(x, x->cpu->cycles + 2, x->dst + 1,
                              (uint32_t)(r.value >> 32), 0, false))
            return false;
        /* The notes put every warning bit these four raise in FAUCR (Table
         * 2-26, printed page 61), including DIV0, which is the one bit only
         * these instructions set: "Source to reciprocal operation for .S1." */
        if (r.status) {
            if (x->controls[19])
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel FAUCR status write conflict");
            x->out->control[19] |= r.status << (x->side ? 16 : 0);
            x->controls[19] = true;
        }
    }
    return true;
}


/* A 64-bit DP operand whose encoded register field names the EVEN register of
 * the pair.  Every instruction that reads src_l one cycle before src_h -
 * ADDDP, SUBDP, MPYDP, MPYSPDP and the DP compares - is encoded that way, as
 * read back from TI's assembler (ADDDP .L1 A5:A4,A7:A6,A9:A8 = 04188318h has
 * src1 = 4 and src2 = 6). */
static uint64_t dp_pair(const CdjC674x *cpu, unsigned bank, unsigned reg)
{
    return (uint64_t)cpu->r[bank][reg + 1] << 32 | cpu->r[bank][reg];
}

/* Queue one already-computed 32-bit delayed result with the same
 * parallel-write rejection every other multi-cycle arm performs.  status is
 * the FP warning mask, already shifted into the unit's half; multiplier
 * selects FMCR over FADCR, as CdjC674xLoad.sign_extend does for size 0. */
static bool dp_queue(CdjC674xArm *x, uint64_t due, unsigned dst,
                     uint32_t value, uint32_t status, bool multiplier)
{
    if (x->out->load_count == 40)
        return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
    for (unsigned j = 0; j < x->out->load_count; ++j) {
        unsigned count = queued_result_registers(&x->out->loads[j]);
        if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
            x->out->loads[j].dst <= dst && dst < x->out->loads[j].dst + count)
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel delayed-result write conflict");
    }
    x->out->loads[x->out->load_count++] = (CdjC674xLoad){
        .due = due, .value = value, .address = status,
        .bank = x->side, .dst = dst, .size = 0, .sign_extend = multiplier
    };
    return true;
}

/* Queue the low and high halves of a DP result at the two cycles the manual
 * gives for them: dst_l on `low` and dst_h on the cycle after, with the
 * warning bits accompanying dst_l.  That split is not cosmetic - every DP
 * instruction page says "the number of delay slots can be reduced by one"
 * for a consumer that reads the low word first, so publishing both halves
 * together would make dst_h visible a cycle early. */
static bool dp_queue_pair(CdjC674xArm *x, unsigned low,
                          CdjC674xDpResult result, bool multiplier)
{
    unsigned shift = x->side ? 16 : 0;
    return dp_queue(x, x->cpu->cycles + low, x->dst, (uint32_t)result.value,
                    result.status << shift, multiplier) &&
           dp_queue(x, x->cpu->cycles + low + 1, x->dst + 1,
                    (uint32_t)(result.value >> 32), 0, multiplier);
}

static bool arm_two_cycle_dp(CdjC674xArm *x)
{
    /* ABSDP (printed pages 105-106) and SPDP (printed pages 477-478) are
     * "Two-cycle DP" with 1 delay slot.  Table 4-12 (printed page 596) fixes
     * the timing for the whole class: the sources are read on E1, dst_l is
     * written on E1, dst_h on E2, and "the status is written to the FAUCR on
     * E1" - so the warning bits land in the issue cycle, like ABSSP's.
     *
     * ABSDP's encoded src2 names the ODD register of the pair, because "the
     * 64-bit double-precision operand is read in one cycle by using the src2
     * port for the 32 MSBs and the src1 port for the 32 LSBs" (printed page
     * 105); TI's assembler emits src2 = 7 for ABSDP .S1 A7:A6.  SPDP's src2
     * is a plain single-precision register. */
    unsigned encoding = x->w & 0xffc;
    if (x->dst & 1)
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision result register pair");
    CdjC674xDpResult result;
    if (encoding == 0xb20) {                      /* ABSDP */
        if (!(x->b & 1))
            return stop(x->cpu, x->pc, x->insn->word,
                        "invalid double-precision source register pair");
        result = cdj_c674x_abs_dp(
            (uint64_t)x->cpu->r[x->cross][x->b] << 32 |
            x->cpu->r[x->cross][x->b - 1]);
    } else {                                      /* SPDP */
        result = cdj_c674x_sp_to_dp(x->cpu->r[x->cross][x->b]);
    }
    x->value = (uint32_t)result.value;            /* dst_l, written on E1 */
    if (x->enabled) {
        if (!dp_queue(x, x->cpu->cycles + 2, x->dst + 1,
                      (uint32_t)(result.value >> 32), 0, false))
            return false;
        if (result.status) {
            if (x->controls[19])
                return stop(x->cpu, x->pc, x->insn->word,
                            "parallel FAUCR status write conflict");
            x->out->control[19] |= result.status << (x->side ? 16 : 0);
            x->controls[19] = true;
        }
    }
    return true;
}

static bool arm_cmpdp(CdjC674xArm *x)
{
    /* CMPEQDP (printed pages 184-185), CMPGTDP (193-194) and CMPLTDP
     * (207-208).  Table 4-15 (printed page 598): src1_l/src2_l on E1,
     * src1_h/src2_h on E2, dst written on E2, "the status is written to the
     * floating-point auxiliary register (FAUCR) on E2".  Delay Slots 1 on
     * all three pages, and CMPLTDP's example is headed "2 cycles after
     * instruction"; the CMPEQDP and CMPGTDP examples are headed "7 cycles
     * after instruction", which contradicts their own Delay Slots 1 and
     * Table 4-15 and is taken as a transcription slip from the ADDDP page. */
    unsigned relation = ((x->w >> 5) & 0x7f) == 0x51 ? 0 :
                        ((x->w >> 5) & 0x7f) == 0x53 ? 1 : 2;
    if ((x->a & 1) || (x->b & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision source register pair");
    x->reg_write = false;
    if (x->enabled) {
        CdjC674xDpResult result = cdj_c674x_compare_dp(
            dp_pair(x->cpu, x->side, x->a),
            dp_pair(x->cpu, x->cross, x->b), relation);
        uint64_t due = x->cpu->cycles + 2;
        if (x->out->load_count + (result.status ? 2u : 1u) > 40)
            return stop(x->cpu, x->pc, x->insn->word,
                        "delayed-result queue full");
        if (!dp_queue(x, due, x->dst, (uint32_t)result.value, 0, false))
            return false;
        if (result.status)
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = due,
                .address = result.status << (x->side ? 16 : 0),
                .size = CDJ_C674X_DELAYED_FAUCR
            };
    }
    return true;
}

static bool arm_addsubdp(CdjC674xArm *x)
{
    /* ADDDP (printed pages 125-126) and SUBDP (printed pages 541-543), on
     * .L and .S alike.  Both take the rounding mode from and set the warning
     * bits in FADCR, "not in the floating-point auxiliary configuration
     * register (FAUCR) as for other .S unit instructions" (ADDDP note 1), so
     * this reads control[18] on either unit.  Table 4-16 (printed page 599):
     * sources on E1/E2, dst_l on E6, dst_h on E7, status to FADCR on E6;
     * Delay Slots 6, and both examples are headed "7 cycles after
     * instruction", which is the E7 high-word write.
     *
     * Opfields, read back from TI's assembler: ADDDP 001 1000 on .L and
     * 111 0010 on .S; SUBDP 001 1001 and its cross-src1 reverse 001 1101 on
     * .L, 111 0011 and the src2-src1 form 111 0111 on .S.  The reverse forms
     * are the same shape as SUBSP's and are handled the same way: 001 1101
     * cross-paths src1 instead of src2, and 111 0111 subtracts the encoded
     * src1 from the encoded src2. */
    unsigned encoding = x->w & 0xffc;
    unsigned operation = encoding == 0x318 || encoding == 0xe58 ? 0 :
                         encoding == 0xef8 ? 2 : 1;
    if ((x->a & 1) || (x->b & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision source register pair");
    if (x->dst & 1)
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision result register pair");
    x->reg_write = false;
    if (x->enabled) {
        uint64_t source1 = dp_pair(x->cpu, x->side, x->a);
        uint64_t source2 = dp_pair(x->cpu, x->cross, x->b);
        if (encoding == 0x3b8) {
            source1 = dp_pair(x->cpu, x->cross, x->a);
            source2 = dp_pair(x->cpu, x->side, x->b);
        }
        unsigned rmode = (x->cpu->control[18] >>
                          ((x->side ? 16u : 0u) + 9)) & 3;
        if (x->out->load_count + 2 > 40)
            return stop(x->cpu, x->pc, x->insn->word,
                        "delayed-result queue full");
        if (!dp_queue_pair(x, 6, cdj_c674x_add_sub_dp(source1, source2,
                                                      operation, rmode), false))
            return false;
    }
    return true;
}

static bool arm_mpydp(CdjC674xArm *x)
{
    /* MPYDP (printed pages 318-319, Table 4-19 printed page 600): dst_l on
     * E9, dst_h on E10, status to FMCR on E9, Delay Slots 9.
     * MPYSPDP (printed pages 352-353, Table 4-20 printed page 601): src1 is
     * single-precision, dst_l on E6, dst_h on E7, Delay Slots 6.
     * MPYSP2DP (printed pages 354-355, Table 4-21 printed page 601): both
     * sources single-precision, dst_l on E4, dst_h on E5, Delay Slots 4.
     *
     * Sections 4.2.15 and 4.2.16 do NOT say where MPYSPDP's and MPYSP2DP's
     * warning bits go.  Section 2.10.3 (printed page 63) does fix the
     * register - FMCR holds the status "for instructions that use the .M
     * functional units", and these two are .M - so only the cycle is
     * inferred: every stated case (MPYDP E9, INTDP E4, ADDDP/SUBDP E6, the
     * four-cycle class E4, two-cycle DP E1) writes status in the dst_l
     * cycle, and that rule is applied here.  This is the one inference in
     * this family that the manual does not state outright. */
    unsigned encoding = x->w & 0xffc;
    bool pair_src1 = encoding == 0x700;           /* MPYDP only */
    bool pair_src2 = encoding != 0x5f0;           /* not MPYSP2DP */
    if ((pair_src1 && (x->a & 1)) || (pair_src2 && (x->b & 1)))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision source register pair");
    if (x->dst & 1)
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision result register pair");
    x->reg_write = false;
    if (x->enabled) {
        uint64_t left = pair_src1 ? dp_pair(x->cpu, x->side, x->a)
            : cdj_c674x_sp_operand_to_dp(x->cpu->r[x->side][x->a]);
        uint64_t right = pair_src2 ? dp_pair(x->cpu, x->cross, x->b)
            : cdj_c674x_sp_operand_to_dp(x->cpu->r[x->cross][x->b]);
        unsigned rmode = (x->cpu->control[20] >>
                          ((x->side ? 16u : 0u) + 9)) & 3;
        unsigned low = encoding == 0x700 ? 9u : encoding == 0x5b0 ? 6u : 4u;
        if (x->out->load_count + 2 > 40)
            return stop(x->cpu, x->pc, x->insn->word,
                        "delayed-result queue full");
        if (!dp_queue_pair(x, low,
                           cdj_c674x_multiply_dp(left, right, rmode), true))
            return false;
    }
    return true;
}

static bool arm_dp_convert(CdjC674xArm *x)
{
    /* DPSP (printed pages 260-261), DPINT (258-259) and DPTRUNC (262-263)
     * are four-cycle .L instructions: section 4.2.8 and Table 4-13 (printed
     * page 597) read the sources on E1, write dst on E4 and write the status
     * to FADCR on E4; Delay Slots 3 on all three pages.  DPTRUNC "operates
     * like DPINT except that the rounding modes in the floating-point adder
     * configuration register (FADCR) are ignored; round toward zero
     * (truncate) is always used" (printed page 262).
     *
     * All three name the ODD register of the source pair, for the same
     * reason ABSDP does: "the operand is read in one cycle by using the src2
     * port for the 32 MSBs and the src1 port for the 32 LSBs". */
    unsigned encoding = x->w & 0xffc;
    if (!(x->b & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision source register pair");
    x->reg_write = false;
    if (x->enabled) {
        unsigned shift = x->side ? 16u : 0u;
        unsigned rmode = encoding == 0x038 ? 1u :
            (x->cpu->control[18] >> (shift + 9)) & 3;
        uint64_t source = (uint64_t)x->cpu->r[x->cross][x->b] << 32 |
                          x->cpu->r[x->cross][x->b - 1];
        CdjC674xDpResult result = encoding == 0x138 ?
            cdj_c674x_dp_to_sp(source, rmode) :
            cdj_c674x_dp_to_integer(source, rmode);
        if (!dp_queue(x, x->cpu->cycles + 4, x->dst, (uint32_t)result.value,
                      result.status << shift, false))
            return false;
    }
    return true;
}

static bool arm_intdp(CdjC674xArm *x)
{
    /* INTDP (printed page 275) and INTDPU (printed page 276).  Section 4.2.9
     * and Table 4-14 (printed page 598): src2 on E1, dst_l on E4, dst_h on
     * E5, Delay Slots 4.  Both pages say "You cannot set configuration bits
     * with this instruction", and every 32-bit integer is exact in a 53-bit
     * significand, so nothing is written to FADCR despite 4.2.9's general
     * sentence about it. */
    if (x->dst & 1)
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid double-precision result register pair");
    x->reg_write = false;
    if (x->enabled) {
        uint64_t value = cdj_c674x_integer_to_dp(x->cpu->r[x->cross][x->b],
                                                 (x->w & 0xffc) == 0x738);
        if (x->out->load_count + 2 > 40)
            return stop(x->cpu, x->pc, x->insn->word,
                        "delayed-result queue full");
        if (!dp_queue_pair(x, 4, (CdjC674xDpResult){value, 0}, false))
            return false;
    }
    return true;
}
/* wave5-arms: 32-bit multiply, Galois, dual-result and 40-bit long forms */
/* ---- 32-bit multiply, Galois, dual-result and 40-bit long forms ----------
 *
 * Arithmetic lives in cdj_c674x_mpy32.c; the arms below only move operands in
 * and results out.  Printed pages are SPRUFE8B July 2010.
 *
 * Two opfields are deliberately rejected rather than implemented: MPYI 00110
 * and MPYID 01100 take their src1 from a five-bit constant field that the
 * opcode maps on printed pages 334 and 335 type as bare "cst5".  Table 3-2
 * (printed page 68) defines "scstn" and "ucstn" but gives "cst" only as
 * "constant", so the manual never fixes whether that field is sign extended -
 * and the register forms of the same instructions, which it does fix, are
 * unaffected.  Guessing would invent an architectural result.
 */
static bool match_mpyi(const CdjC674xArm *x)
{
    unsigned op = (x->w >> 7) & 31;
    return op == 0x04 || op == 0x06 ||   /* MPYI  reg / cst5, page 334 */
           op == 0x08 || op == 0x0c;     /* MPYID reg / cst5, page 335 */
}

static bool match_mpy2_gmpy4(const CdjC674xArm *x)
{
    unsigned op = (x->w >> 6) & 31;
    return op == 0x00 ||                 /* MPY2,  printed page 365 */
           op == 0x11;                   /* GMPY4, printed page 272 */
}

static bool match_gmpy_word(const CdjC674xArm *x)
{
    unsigned op = (x->w >> 6) & 31u;
    return op == 0x1bu || op == 0x1fu; /* XORMPY / GMPY, Figure E-3 */
}

/* Queue one already-computed delayed result, rejecting a same-cycle overlap
 * with another delayed write to the same registers exactly as the existing .M
 * arms do.  count is 1 for a scalar result and 2 for a register pair. */
static bool queue_delayed_result(CdjC674xArm *x, uint64_t due, uint64_t value,
                                 unsigned dst, unsigned count)
{
    if (x->out->load_count == 40)
        return stop(x->cpu, x->pc, x->insn->word, "delayed-result queue full");
    for (unsigned j = 0; j < x->out->load_count; ++j) {
        unsigned old_count = queued_result_registers(&x->out->loads[j]);
        if (x->out->loads[j].due == due && x->out->loads[j].bank == x->side &&
            x->out->loads[j].dst < dst + count &&
            dst < x->out->loads[j].dst + old_count)
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel delayed-result write conflict");
    }
    x->out->loads[x->out->load_count++] = (CdjC674xLoad){
        .due = due, .value = value, .bank = x->side, .dst = dst,
        .size = count == 2 ? 16u : 0u
    };
    return true;
}

static bool arm_gmpy_word(CdjC674xArm *x)
{
    /* Figure E-3's unconditional GMPY and XORMPY use the M1/M2 polynomial
     * register (GPLYA/GPLYB) or zero respectively.  Both write in E4. */
    unsigned op = (x->w >> 6) & 31u;
    x->reg_write = false;
    if (x->enabled) {
        uint32_t poly = op == 0x1fu ? x->cpu->control[22 + x->side] : 0u;
        uint32_t value = cdj_c674x_gmpy_word(x->cpu->r[x->side][x->a],
                                              x->cpu->r[x->cross][x->b], poly);
        return queue_delayed_result(x, x->cpu->cycles + 4, value, x->dst, 1);
    }
    return true;
}

/* Write a 40-bit long into an even/odd pair in E1, as arm_l_long_addsub does:
 * the low register holds bits 31-0 and only bits 7-0 of the high register are
 * architecturally part of the value. */
static bool write_long40(CdjC674xArm *x, uint64_t result)
{
    if (x->written[x->side][x->dst] || x->written[x->side][x->dst + 1])
        return stop(x->cpu, x->pc, x->insn->word,
                    "parallel register write conflict");
    x->out->r[x->side][x->dst] = (uint32_t)result;
    x->out->r[x->side][x->dst + 1] =
        (uint32_t)((result & CDJ_C674X_LONG40_MASK) >> 32);
    x->written[x->side][x->dst] = x->written[x->side][x->dst + 1] = true;
    return true;
}

static bool arm_mpyi(CdjC674xArm *x)
{
    /* MPYI (printed page 334) and MPYID (335) sample src1 and src2 in E1-E4.
     * MPYI has 8 delay slots and writes dst in E9; MPYID has 9 and writes
     * dst_l in E9 and dst_h in E10, which its pipeline table spells out and
     * its "10 cycles after instruction" example confirms.  The documented
     * functional-unit latency of 4 is a scheduling constraint on following .M
     * instructions that this core does not model, as for every other .M
     * instruction here. */
    unsigned op = (x->w >> 7) & 31;
    bool pair = op == 0x08 || op == 0x0c;
    x->reg_write = false;
    if (op == 0x06 || op == 0x0c)
        return stop(x->cpu, x->pc, x->insn->word,
                    "MPYI constant operand signedness not specified");
    if (pair && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        uint64_t product = cdj_c674x_mpyi(x->cpu->r[x->side][x->a],
                                          x->cpu->r[x->cross][x->b]);
        if (!pair)
            return queue_delayed_result(x, x->cpu->cycles + 9,
                                        (uint32_t)product, x->dst, 1);
        return queue_delayed_result(x, x->cpu->cycles + 9,
                                    (uint32_t)product, x->dst, 1) &&
               queue_delayed_result(x, x->cpu->cycles + 10,
                                    product >> 32, x->dst + 1, 1);
    }
    return true;
}

static bool arm_mpy2_gmpy4(CdjC674xArm *x)
{
    /* MPY2 (printed pages 365-366) and GMPY4 (272-274) are both four-cycle .M
     * instructions with 3 delay slots, writing in E4.  MPY2's destination is a
     * register pair, GMPY4's a single register.
     *
     * GMPY4's opcode figure on printed page 272 prints a literal 1 where every
     * other predicable .M figure prints z; ti-cgt-c6000 8.5.0 asm6x -mv6740
     * emits 0 there for "GMPY4 .M1 A4, A6, A5" (02988470h), so that 1 is a
     * transcription artifact and bit 28 is the ordinary z of Table 3-9.
     *
     * GFPGFR selects GMPY4's field size and polynomial (printed page 272).
     * Section 2.7.1 says an MVC change controls GMPY4 in the next execute
     * packet, which the transactional E1 MVC write already provides. */
    unsigned op = (x->w >> 6) & 31;
    bool pair = op == 0x00;
    x->reg_write = false;
    if (pair && (x->dst & 1))
        return stop(x->cpu, x->pc, x->insn->word,
                    "invalid multiply result register pair");
    if (x->enabled) {
        uint32_t src1 = x->cpu->r[x->side][x->a];
        uint32_t src2 = x->cpu->r[x->cross][x->b];
        uint32_t gfpgfr = x->cpu->control[24];
        uint64_t value = pair ? cdj_c674x_mpy2(src1, src2)
                              : cdj_c674x_gmpy4(src1, src2, gfpgfr & 0xffu,
                                                (gfpgfr >> 24) & 7u);
        return queue_delayed_result(x, x->cpu->cycles + 4, value, x->dst,
                                    pair ? 2 : 1);
    }
    return true;
}

static bool arm_dmv(CdjC674xArm *x)
{
    /* DMV, printed page 234: "src2 -> dst_e; src1 -> dst_o", single cycle,
     * 0 delay slots, so both halves are written in E1. */
    x->reg_write = false;
    if (x->dst & 1)
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    if (x->enabled) {
        if (x->written[x->side][x->dst] || x->written[x->side][x->dst + 1])
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel register write conflict");
        x->out->r[x->side][x->dst] = x->cpu->r[x->cross][x->b];
        x->out->r[x->side][x->dst + 1] = x->cpu->r[x->side][x->a];
        x->written[x->side][x->dst] = x->written[x->side][x->dst + 1] = true;
    }
    return true;
}

static bool arm_sat40(CdjC674xArm *x)
{
    /* SAT, printed pages 437-439: a 40-bit local pair saturated into a 32-bit
     * dst in E1, with CSR.SAT and the per-unit SSR bit set one cycle after dst
     * is written.  Example 1 on printed page 438 shows SSR 0000 0002h for
     * SAT .L2, which is SSR.L2 (SPRUFE8B 2.9.13, printed page 54). */
    if (x->b & 1)
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    /* A cross path carries one 32-bit operand only (SPRUFE8B 2.3), so the
     * otherwise format-shaped x = 1 words stay fail-closed rather than
     * silently reading the local pair, as in arm_sat_long. */
    if (x->w & 0x1000)
        return stop(x->cpu, x->pc, x->insn->word,
                    "cross-path long operand not supported");
    if (x->enabled) {
        bool saturated;
        x->value = cdj_c674x_sat40(register_long40(x->cpu, x->side, x->b),
                                   &saturated);
        if (saturated) {
            if (x->out->load_count == 40)
                return stop(x->cpu, x->pc, x->insn->word,
                            "delayed-status queue full");
            x->out->loads[x->out->load_count++] = (CdjC674xLoad){
                .due = x->cpu->cycles + 2, .address = 1u << x->side,
                .size = CDJ_C674X_DELAYED_SAT
            };
        }
    }
    return true;
}

static bool arm_rpack2(CdjC674xArm *x)
{
    /* SPRUFE8B pp.416-417: saturate each signed source after a one-bit
     * shift, then pack their high halfwords.  The worked example's FDBA
     * conflicts with its execution rule (FEDCBA98 << 1 gives FDB9). */
    bool sat1, sat2;
    uint32_t src1 = saturate32((int64_t)(int32_t)x->cpu->r[x->side][x->a] * 2,
                               &sat1);
    uint32_t src2 = saturate32((int64_t)(int32_t)x->cpu->r[x->cross][x->b] * 2,
                               &sat2);
    x->value = (src1 & 0xffff0000u) | (src2 >> 16);
    if (x->enabled && (sat1 || sat2)) {
        if (x->out->load_count == 40)
            return stop(x->cpu, x->pc, x->insn->word,
                        "delayed-status queue full");
        x->out->loads[x->out->load_count++] = (CdjC674xLoad){
            .due = x->cpu->cycles + 2, .address = 1u << (2 + x->side),
            .size = CDJ_C674X_DELAYED_SAT
        };
    }
    return true;
}

static bool arm_subc(CdjC674xArm *x)
{
    /* SUBC, printed pages 539-540: unsigned, single cycle, E1 write. */
    x->value = cdj_c674x_subc(x->cpu->r[x->side][x->a],
                              x->cpu->r[x->cross][x->b]);
    return true;
}

/* Queue the CSR.SAT / SSR[unit] effect of a saturating .L instruction.
 * CSR Table 2-9 (printed page 39): "The SAT bit is set one full cycle (one
 * delay slot) after a saturate occurs."  SSR 2.9.13 (printed page 54) sets the
 * unit flag "in the cycle following the writing of the result".  address is the
 * SSR unit mask, L1 = bit 0 and L2 = bit 1 (Table 2-22, printed page 55), which
 * is what 1u << side gives for a .L instruction. */
static bool queue_saturation(CdjC674xArm *x)
{
    if (x->out->load_count == 40)
        return stop(x->cpu, x->pc, x->insn->word, "delayed-status queue full");
    x->out->loads[x->out->load_count++] = (CdjC674xLoad){
        .due = x->cpu->cycles + 2, .address = 1u << x->side,
        .size = CDJ_C674X_DELAYED_SAT
    };
    return true;
}

static bool arm_abs(CdjC674xArm *x)
{
    /* ABS, printed pages 101-102: single cycle, 0 delay slots, E1 write.
     * Opfield 001 1010 is the sint form and 011 1000 the slong form, and the
     * sint form's three cases are the slong form's at 32 bits.
     *
     * CSR.SAT AND SSR ARE UNRESOLVED HERE, NOT DECIDED.  An earlier version of
     * this comment claimed printed page 102 says neither is affected.  It does
     * not: the whole ABS entry contains no occurrence of "SAT", "CSR" or "SSR".
     * That sentence belongs to ABS2 (printed page 105), and the packed forms
     * are the ones that carry the exemption note.  The general rule points the
     * other way - CSR Table 2-9 (printed page 33) defines bit 9 SAT as "one or
     * more functional units performed an arithmetic operation which resulted in
     * saturation", and SSR 2.9.13 (printed page 54) says instructions resulting
     * in saturation set the unit flag - and ABS rule 3 (-2^31 -> 2^31-1,
     * -2^39 -> 2^39-1) is such a saturation.
     *
     * SO THE ANSWER COMES FROM A GENERAL RULE, NOT THE ENTRY.  Table 4-1
     * (printed page 581), phase E2: "Single-cycle instructions that saturate
     * results set the SAT bit in the control status register (CSR) if
     * saturation occurs."  ABS is stated Single-cycle (printed page 102) and
     * its rule 3 saturates (printed page 101), so the three stated facts chain.
     * SSR 2.9.13 (printed page 54) is unqualified in the same direction:
     * "Instructions resulting in saturation set the appropriate unit flag in
     * SSR in the cycle following the writing of the result to the register
     * file."
     *
     * The exemption notes point the same way rather than against it.  All EIGHT
     * "does not affect the SAT bit" notes in the manual are on packed forms
     * (ABS2 103, SADD2 425, SADDSUB2 429, SADDUS2 433, SADDU4 435, SPACK2 472,
     * SPACKU4 474, SSUB2 502) and every one justifies itself the same way - the
     * operation is performed on each lane separately, so there is no single
     * result to flag.  That rationale does not transfer to a scalar form.
     * Meanwhile every non-packed saturating instruction states positively that
     * it DOES set SAT (SADD 423, SAT 437, SSUB 499, SSHL 493, SMPY 461,
     * SADDSUB 427, SSHVL 495, SSHVR 497); ABS is the only scalar saturating
     * instruction with neither a positive statement nor an exemption.
     * Packedness alone is not sufficient for exemption - SMPY2 (printed page
     * 468) is packed and sets SAT - so the implication runs only one way.
     *
     * An earlier version of this comment left both flags alone and called that
     * the status quo.  It was not: doing nothing diverges from a stated general
     * rule, which is the less conservative choice, not the safer one.  Still
     * not stated for ABS by name, and recorded as an inference in
     * DSP_ARCHITECTURE_COVERAGE.md. */
    bool pair = ((x->w >> 5) & 0x7f) == 0x38;
    if (!pair) {
        uint32_t src2 = x->cpu->r[x->cross][x->b];
        x->value = cdj_c674x_abs32(src2);
        /* Rule 3, printed page 101: only -2^31 saturates. */
        if (x->enabled && src2 == 0x80000000u && !queue_saturation(x))
            return false;
        return true;
    }
    x->reg_write = false;
    if ((x->dst & 1) || (x->b & 1))
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    if (x->w & 0x1000)
        return stop(x->cpu, x->pc, x->insn->word,
                    "cross-path long operand not supported");
    if (x->enabled) {
        uint64_t src2 = register_long40(x->cpu, x->side, x->b);
        if (!write_long40(x, cdj_c674x_abs40(src2))) return false;
        /* The slong rule 3 saturates only at -2^39. */
        if (src2 == (UINT64_C(1) << 39) && !queue_saturation(x)) return false;
    }
    return true;
}

static bool arm_cmp_long(CdjC674xArm *x)
{
    /* The 40-bit src2 forms of CMPEQ (printed pages 177-178), CMPGT (188-190),
     * CMPGTU (197-198), CMPLT (202-204) and CMPLTU (211-212).  In each opcode
     * map src2 is the local slong/ulong pair and src1 is the 32-bit operand
     * that may cross (xsint/xuint) or be a five-bit constant - scst5 for the
     * signed compares, ucst5 for CMPGTU/CMPLTU.  The opfield's low bit selects
     * the register form, so both members of each pair land here.  Single cycle,
     * 0 delay slots, E1 write.
     *
     * src1 is compared at the full 40-bit width: printed page 178's Example 3,
     * CMPEQ .L2X A1,B3:B2,B1 with A1 = F23A 3789h and B3:B2 = 0000 00FFh
     * F23A 3789h, writes 1 (true), which only holds if the 32-bit src1 is
     * sign extended to 40 bits before the comparison.  That example is also
     * why x has to route src1 here: see the guard below. */
    unsigned op = (x->w >> 5) & 0x7f;
    bool immediate = !(op & 1);
    bool unsigned_compare = op == 0x4c || op == 0x4d ||  /* CMPGTU */
                            op == 0x5c || op == 0x5d;    /* CMPLTU */
    if (x->b & 1)
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    /* No cross path can carry the 40-bit src2 (SPRUFE8B 2.3), so for these
     * opfields x routes src1 instead - exactly as arm_sat_long does for SADD's
     * xsint + slong form.  Printed page 178's Example 3, CMPEQ .L2X
     * A1,B3:B2,B1, is that case, and ti-cgt-c6000 8.5.0 asm6x -mv6740 assembles
     * it as 00883A3Ah with x = 1 and src1 = A1.  The constant-src1 opfields
     * have nothing to cross, so x = 1 there stays fail-closed. */
    if (immediate && (x->w & 0x1000))
        return stop(x->cpu, x->pc, x->insn->word,
                    "cross-path long operand not supported");
    if (x->enabled) {
        uint64_t raw = register_long40(x->cpu, x->side, x->b);
        if (unsigned_compare) {
            uint64_t left = immediate ? x->a : x->cpu->r[x->cross][x->a];
            uint64_t right = raw & CDJ_C674X_LONG40_MASK;
            x->value = (op == 0x4c || op == 0x4d) ? left > right : left < right;
        } else {
            int64_t left = immediate ? sx(x->a, 5) :
                                       (int32_t)x->cpu->r[x->cross][x->a];
            int64_t right = cdj_c674x_sx40(raw);
            x->value = op == 0x50 || op == 0x51 ? left == right :
                       op == 0x44 || op == 0x45 ? left > right : left < right;
        }
    }
    return true;
}

static bool arm_shift_long(CdjC674xArm *x)
{
    /* The 40-bit forms of SHL (printed pages 447-448), SHR (451-452) and SHRU
     * (457-458).  Opfields, reading bits 11-6: SHL 11 0000/11 0001 shift a
     * slong pair into a slong pair, SHL 01 0010/01 0011 shift an xuint into a
     * ulong pair, SHR 11 0100/11 0101 and SHRU 10 0100/10 0101 shift a
     * slong/ulong pair into a pair.  As for the scalar .S shifts, the opfield's
     * bit 0 (word bit 6) selects a register count over the ucst5 field, and a
     * register count uses only its six low bits.  Single cycle, E1 write. */
    unsigned op = (x->w >> 6) & 63;
    bool pair_source = op != 0x12 && op != 0x13;
    unsigned count = (x->w & 0x40) ? (x->cpu->r[x->side][x->a] & 63) : x->a;
    unsigned operation = op == 0x34 || op == 0x35 ? CDJ_C674X_SHIFT40_ARITHMETIC
                       : op == 0x24 || op == 0x25 ? CDJ_C674X_SHIFT40_LOGICAL
                       : CDJ_C674X_SHIFT40_LEFT;
    x->reg_write = false;
    if ((x->dst & 1) || (pair_source && (x->b & 1)))
        return stop(x->cpu, x->pc, x->insn->word, "invalid long register pair");
    /* No cross path can carry the 40-bit src2 (SPRUFE8B 2.3); the xuint source
     * form does cross, so only the pair-source forms reject x = 1. */
    if (pair_source && (x->w & 0x1000))
        return stop(x->cpu, x->pc, x->insn->word,
                    "cross-path long operand not supported");
    if (x->enabled) {
        /* The xuint source is zero extended into the 40-bit shifter: printed
         * page 447's opcode map types it xuint with a ulong dst. */
        uint64_t source = pair_source ?
            register_long40(x->cpu, x->side, x->b) : x->cpu->r[x->cross][x->b];
        return write_long40(x, cdj_c674x_shift40(source, count, operation));
    }
    return true;
}

static bool arm_b_nrp(CdjC674xArm *x)
{
    /* B NRP, printed pages 157-158: "NRP is placed in the program fetch
     * counter (PFC).  This instruction also sets the NMIE bit.  The PGIE bit is
     * unchanged."  Five delay slots, so the branch completes in the sixth cycle
     * exactly as B IRP and B displacement do here.  NRP is control register 7
     * and NMIE is IER bit 1 (cdj_c674x_control_read and the MVC IER write mask
     * below both treat them that way).
     *
     * THE INSTRUCTION PAGE IS NOT THE WHOLE RULE.  5.3.4.2 (printed page 639)
     * adds "The NTSR register will be copied back into the TSR register during
     * the transfer of control out of the interrupt" - the exact counterpart of
     * the ITSR -> TSR restore arm_b_irp performs above.  NTSR is control
     * register 28, which cdj_c674x_control_read_supported and
     * write_supported both exclude and which nothing in this core ever writes,
     * so there is no NTSR here to restore FROM.  Writing the restore anyway
     * would zero TSR, which is a fabricated architectural effect, not a
     * conservative one.
     *
     * So this fails closed instead: with TSR at its reset value the restore is
     * a no-op and B NRP behaves exactly as the instruction page describes,
     * while any state where the restore would be observable is refused rather
     * than silently diverging.  This core's own interrupt entry sets TSR bits
     * 9 and 15, so a B NRP reached from inside a maskable ISR lands here.  The
     * mask is arm_b_irp's, the restorable TSR bits. */
    x->reg_write = false;
    if (x->enabled) {
        if (x->cpu->control[26] & 0x0000c6deu)
            return stop(x->cpu, x->pc, x->insn->word,
                        "B NRP with a restorable TSR and no modelled NTSR");
        if (x->controls[4])
            return stop(x->cpu, x->pc, x->insn->word,
                        "B NRP parallel IER write conflict");
        if (!queue_branch(x->out, x->cpu->cycles + 6, x->cpu->control[7]))
            return stop(x->cpu, x->pc, x->insn->word,
                        "parallel taken branches or branch queue overflow");
        x->out->control[4] |= 2u;
        x->controls[4] = true;
    }
    return true;
}

static bool arm_bpos(CdjC674xArm *x)
{
    /* BPOS, printed pages 170-171: "If (dst >= 0), PFC = (PCE1 +
     * (se(scst10) << 2))", five delay slots, dst read in E1 and never written.
     * scst10 is the ten-bit src field, bits 22-13.  Printed page 170 also
     * requires that only one BPOS issue per cycle and that BPOS not share an
     * execute packet with ADDKPC. */
    for (unsigned j = 0; j < x->packet->count; ++j) {
        const CdjC674xInstruction *other = &x->packet->instructions[j];
        if (other->compact || other == x->insn) continue;
        if ((other->word & 0x1ffe) == 0x162)
            return stop(x->cpu, x->pc, x->w, "BPOS parallel with ADDKPC");
        if ((other->word & 0x1ffc) == 0x0020)
            return stop(x->cpu, x->pc, x->w, "multiple BPOS instructions");
    }
    x->reg_write = false;
    if (x->enabled && !(x->cpu->r[x->side][x->dst] & 0x80000000u)) {
        if (!queue_branch(x->out, x->cpu->cycles + 6, (x->pc & ~31u) +
                          (uint32_t)(sx((x->w >> 13) & 1023, 10) * 4)))
            return stop(x->cpu, x->pc, x->w,
                        "parallel taken branches or branch queue overflow");
    }
    return true;
}


static const CdjC674xArmEntry cdj_c674x_arms[] = {
    { 0x00000ffc, 0x00000618, NULL,                  arm_sat_long },
    { 0x00000ffc, 0x00000638, NULL,                  arm_sat_long },
    { 0x00000ffc, 0x00000598, NULL,                  arm_sat_long },
    { 0x00000ffc, 0x00000278, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x00000258, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x000001f8, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x000003f8, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x000001d8, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x00000820, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x000008e0, NULL,                  arm_sat_scalar },
    { 0x00000ffc, 0x000008a0, NULL,                  arm_sat_scalar },
    { 0x0000000c, 0x0000000c, NULL,                  arm_scalar_memory },
    { 0x0000010c, 0x00000004, NULL,                  arm_scalar_memory },
    { 0x0000017c, 0x00000134, NULL,                  arm_scalar_memory },
    { 0x0000017c, 0x00000154, NULL,                  arm_scalar_memory },
    { 0x0000017c, 0x00000124, NULL,                  arm_scalar_memory },
    { 0x0000017c, 0x00000174, NULL,                  arm_scalar_memory },
    { 0x0000017c, 0x00000164, NULL,                  arm_scalar_memory },
    { 0x0000017c, 0x00000144, NULL,                  arm_scalar_memory },
    { 0x0000007c, 0x00000050, NULL,                  arm_addk },
    { 0x0000007c, 0x00000028, NULL,                  arm_mvk_s },
    { 0x0000007c, 0x00000068, NULL,                  arm_mvkh },
    { 0x0000007c, 0x00000040, match_d_adda,          arm_d_adda },
    { 0x0000007c, 0x00000040, match_d_addsub,        arm_d_addsub },
    { 0x00000ffc, 0x00000ab0, NULL,                  arm_d_addsub_cross },
    { 0x00000ffc, 0x00000af0, NULL,                  arm_d_addsub_cross },
    { 0x00000ffc, 0x00000b30, NULL,                  arm_d_addsub_cross },
    { 0x007c1ffc, 0x00000040, NULL,                  arm_mvk_d },
    { 0x0003effc, 0x0000a358, NULL,                  arm_mvk_l },
    { 0x0000001c, 0x00000018, match_l_long_addsub,   arm_l_long_addsub },
    { 0x0000007c, 0x00000000, match_mpy32_scalar,    arm_mpy32 },
    { 0x0000083c, 0x00000030, match_mpy32_packed,    arm_mpy32 },
    { 0x0000007c, 0x00000000, match_mpy16,           arm_mpy16 },
    { 0x0000007c, 0x00000000, match_smpy16_scalar,   arm_smpy16 },
    { 0x0000083c, 0x00000030, match_smpy16_packed,   arm_smpy16 },
    { 0x0000083c, 0x00000030, match_mpy_half32,      arm_mpy_half32 },
    { 0x0003effc, 0x000340f0, NULL,                  arm_mvd },
    { 0x0003cffc, 0x00000958, NULL,                  arm_intsp },
    { 0x0003cffc, 0x00000938, NULL,                  arm_intsp },
    { 0x0003cffc, 0x00000158, NULL,                  arm_spint },
    { 0x0003cffc, 0x00000178, NULL,                  arm_spint },
    { 0x0003effc, 0x00000f20, NULL,                  arm_abssp },
    { 0x0000003c, 0x00000020, match_cmpsp,           arm_cmpsp },
    { 0x00000ffc, 0x00000218, NULL,                  arm_addsubsp },
    { 0x00000ffc, 0x00000e18, NULL,                  arm_addsubsp },
    { 0x00000ffc, 0x00000238, NULL,                  arm_addsubsp },
    { 0x00000ffc, 0x000002b8, NULL,                  arm_addsubsp },
    { 0x00000ffc, 0x00000e38, NULL,                  arm_addsubsp },
    { 0x00000ffc, 0x00000eb8, NULL,                  arm_addsubsp },
    { 0x00000ffc, 0x00000e00, NULL,                  arm_mpysp },
    { 0x00000ffc, 0x00000018, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000ff0, NULL,                  arm_pack },
    { 0x00000ffc, 0x000003d8, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000260, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000398, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000220, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000378, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000420, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000d18, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000d38, NULL,                  arm_pack },
    { 0x00000ffc, 0x00000f98, NULL,                  arm_andn },
    { 0x00000ffc, 0x00000db0, NULL,                  arm_andn },
    { 0x00000ffc, 0x00000830, NULL,                  arm_andn },
    { 0x00000ffc, 0x000007a0, NULL,                  arm_and_imm },
    { 0x00000ffc, 0x00000f58, NULL,                  arm_and_imm },
    { 0x00000ffc, 0x000009f0, NULL,                  arm_and_imm },
    { 0x00000ffc, 0x000007e0, NULL,                  arm_and_reg },
    { 0x00000ffc, 0x00000f78, NULL,                  arm_and_reg },
    { 0x00000ffc, 0x000009b0, NULL,                  arm_and_reg },
    { 0x00000ffc, 0x00000058, NULL,                  arm_add_imm },
    { 0x00000ffc, 0x000001a0, NULL,                  arm_add_imm },
    { 0x00000ffc, 0x00000078, NULL,                  arm_add_reg },
    { 0x00000ffc, 0x000001e0, NULL,                  arm_add_reg },
    { 0x00000ffc, 0x000000d8, NULL,                  arm_sub_imm },
    { 0x00000ffc, 0x000005a0, NULL,                  arm_sub_imm },
    { 0x00000ffc, 0x000000f8, NULL,                  arm_sub_reg },
    { 0x00000ffc, 0x000005e0, NULL,                  arm_sub_reg },
    { 0x00000ffc, 0x000002f8, NULL,                  arm_sub_reverse },
    { 0x00000ffc, 0x00000d70, NULL,                  arm_sub_reverse },
    { 0x00000ffc, 0x00000a58, NULL,                  arm_cmp },
    { 0x00000ffc, 0x00000a78, NULL,                  arm_cmp },
    { 0x00000ffc, 0x000008d8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x000008f8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x000009d8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x000009f8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x00000ad8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x00000af8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x00000bd8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x00000bf8, NULL,                  arm_cmp },
    { 0x00000ffc, 0x00000fd8, NULL,                  arm_or_imm },
    { 0x00000ffc, 0x000006a0, NULL,                  arm_or_imm },
    { 0x00000ffc, 0x000008f0, NULL,                  arm_or_imm },
    { 0x00000ffc, 0x00000ff8, NULL,                  arm_or_reg },
    { 0x00000ffc, 0x000006e0, NULL,                  arm_or_reg },
    { 0x00000ffc, 0x000008b0, NULL,                  arm_or_reg },
    { 0x00000ffc, 0x00000dd8, NULL,                  arm_xor_imm },
    { 0x00000ffc, 0x000002a0, NULL,                  arm_xor_imm },
    { 0x00000ffc, 0x00000bf0, NULL,                  arm_xor_imm },
    { 0x00000ffc, 0x00000df8, NULL,                  arm_xor_reg },
    { 0x00000ffc, 0x000002e0, NULL,                  arm_xor_reg },
    { 0x00000ffc, 0x00000bb0, NULL,                  arm_xor_reg },
    { 0x0000003c, 0x00000008, NULL,                  arm_bitfield },
    { 0x00000afc, 0x00000ae0, NULL,                  arm_bitfield },
    { 0x00000fbc, 0x000009a0, NULL,                  arm_shift_s },
    { 0x00000fbc, 0x00000da0, NULL,                  arm_shift_s },
    { 0x00000fbc, 0x00000ca0, NULL,                  arm_shift_s },
    { 0x00000ffe, 0x000003e2, match_src1_zero,       arm_mvc_read },
    { 0x00000ffe, 0x000003a2, match_src1_zero,       arm_mvc_write },
    { 0x0ffffffe, 0x001800e2, NULL,                  arm_b_irp },
    { 0x0000007c, 0x00000010, NULL,                  arm_b_disp },
    { 0x00001ffc, 0x00001020, NULL,                  arm_bdec },
    { 0x00001ffc, 0x00000120, NULL,                  arm_bnop_disp },
    { 0x0f830ffe, 0x00800362, NULL,                  arm_bnop_reg },
    { 0x0f83effe, 0x00000362, NULL,                  arm_b_reg },
    { 0x00001ffe, 0x00000162, NULL,                  arm_addkpc },
    /* wave5-rows: dot-product and complex-multiply */
    { 0xf000083c, 0x10000030, match_cmpy,           arm_cmpy },
    { 0x0000083c, 0x00000030, match_dotp,            arm_dotp },
    /* wave5-rows: packed 16-bit */
    /* ABS2 fixes src1 (bits 17-13) at 00100b, so its row masks that field
     * too rather than claiming the encodings the manual reserves. */
    { 0x0003effc, 0x00008358, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000060, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x000000b8, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000930, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000460, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000098, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000970, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000c30, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000c98, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000c70, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000858, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000f70, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000838, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000f30, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000df0, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000620, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000e30, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000660, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000760, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000520, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x00000cb0, NULL,                  arm_packed16 },
    { 0x00000ffc, 0x000004f0, NULL,                  arm_packed16_m },
    { 0x00000ffc, 0x00000730, NULL,                  arm_packed16_m },
    { 0x00000ffc, 0x000006b0, NULL,                  arm_packed16_m },
    /* wave5-rows: packed 8-bit */
    /* Bits 11-2 of each entry's 32-bit Opcode figure; s (bit 1) and p (bit 0)
     * stay free, as does the x bit.  CMPLTU4 (printed page 213) and MPYUS4
     * (printed page 363) are pseudo-operations assembled as CMPGTU4 and
     * MPYSU4 with exchanged operands, so they share those rows and add none. */
    { 0x00000ffc, 0x00000cb8, NULL,                  arm_packed8 }, /* ADD4    */
    { 0x00000ffc, 0x00000cd8, NULL,                  arm_packed8 }, /* SUB4    */
    { 0x00000ffc, 0x00000b58, NULL,                  arm_packed8 }, /* SUBABS4 */
    { 0x00000ffc, 0x00000cf0, NULL,                  arm_packed8 }, /* SADDU4  */
    { 0x00000ffc, 0x00000878, NULL,                  arm_packed8 }, /* MAXU4   */
    { 0x00000ffc, 0x00000918, NULL,                  arm_packed8 }, /* MINU4   */
    { 0x00000ffc, 0x00000720, NULL,                  arm_packed8 }, /* CMPEQ4  */
    { 0x00000ffc, 0x00000560, NULL,                  arm_packed8 }, /* CMPGTU4 */
    { 0x00000ffc, 0x00000d30, NULL,                  arm_packed8 }, /* SPACKU4 */
    { 0x00000ffc, 0x000004b0, NULL,                  arm_packed8 }, /* AVGU4   */
    { 0x00000ffc, 0x00000130, NULL,                  arm_packed8 }, /* MPYU4   */
    { 0x00000ffc, 0x00000170, NULL,                  arm_packed8 }, /* MPYSU4  */
    /* wave5-rows: pack, unpack, shuffle and bit manipulation */
    { 0xf0000ffc, 0x10000198, NULL,                  arm_packbits_dual },
    { 0xf0000ffc, 0x100001b8, NULL,                  arm_packbits_dual },
    { 0xf0000ffc, 0x100001d8, NULL,                  arm_packbits_dual },
    { 0xf0000ffc, 0x100001f8, NULL,                  arm_packbits_dual },
    /* Masks read off each entry's own Opcode figure: bits 17-13 and 11-2 for
     * the .L/.S/.M src2-only forms, bits 11-2 alone where 17-13 carry src1, and
     * bits 31-28 plus 11-2 for the three nonconditional .L forms. */
    { 0x0003effc, 0x00006358, NULL,                  arm_packbits_unpack },
    { 0x0003effc, 0x00006f20, NULL,                  arm_packbits_unpack },
    { 0x0003effc, 0x00004358, NULL,                  arm_packbits_unpack },
    { 0x0003effc, 0x00004f20, NULL,                  arm_packbits_unpack },
    { 0x0003effc, 0x00002358, NULL,                  arm_packbits_unpack },
    { 0x0003effc, 0x0003e0f0, NULL,                  arm_packbits_m },
    { 0x0003effc, 0x0003c0f0, NULL,                  arm_packbits_m },
    { 0x0003effc, 0x0003a0f0, NULL,                  arm_packbits_m },
    { 0x0003effc, 0x000380f0, NULL,                  arm_packbits_m },
    { 0x0003effc, 0x000320f0, NULL,                  arm_packbits_m },
    { 0x0003effc, 0x000300f0, NULL,                  arm_packbits_m },
    { 0x00000ffc, 0x00000770, NULL,                  arm_packbits_m },
    { 0x00000ffc, 0x000007b0, NULL,                  arm_packbits_m },
    { 0x00000ffc, 0x00000d58, NULL,                  arm_packbits_lmbd },
    { 0x00000ffc, 0x00000d78, NULL,                  arm_packbits_lmbd },
    { 0x0003effc, 0x00000c78, NULL,                  arm_packbits_norm },
    { 0x0003effc, 0x00000c18, NULL,                  arm_packbits_norm },
    { 0x00000ffc, 0x00000c38, NULL,                  arm_packbits_mergebyte },
    { 0x00000ffc, 0x00000e70, NULL,                  arm_packbits_mergebyte },
    { 0x00000ffc, 0x00000c58, NULL,                  arm_packbits_mergebyte },
    { 0x00000ffc, 0x00000eb0, NULL,                  arm_packbits_mergebyte },
    { 0xf0800ffc, 0x10000698, NULL,                  arm_packbits_dual },
    { 0xf0800ffc, 0x10000678, NULL,                  arm_packbits_dual },
    { 0xf0000ffc, 0x100006d8, NULL,                  arm_packbits_dual },
    /* wave5-rows: double-precision floating point */
    { 0x00000ffc, 0x00000f60, NULL,                  arm_approx },
    { 0x00000ffc, 0x00000fa0, NULL,                  arm_approx },
    { 0x00000ffc, 0x00000b60, NULL,                  arm_approx },
    { 0x00000ffc, 0x00000ba0, NULL,                  arm_approx },
    { 0x0003effc, 0x00000b20, NULL,                  arm_two_cycle_dp },
    { 0x0003effc, 0x000000a0, NULL,                  arm_two_cycle_dp },
    { 0x00000ffc, 0x00000a20, NULL,                  arm_cmpdp },
    { 0x00000ffc, 0x00000a60, NULL,                  arm_cmpdp },
    { 0x00000ffc, 0x00000aa0, NULL,                  arm_cmpdp },
    { 0x00000ffc, 0x00000318, NULL,                  arm_addsubdp },
    { 0x00000ffc, 0x00000e58, NULL,                  arm_addsubdp },
    { 0x00000ffc, 0x00000338, NULL,                  arm_addsubdp },
    { 0x00000ffc, 0x000003b8, NULL,                  arm_addsubdp },
    { 0x00000ffc, 0x00000e78, NULL,                  arm_addsubdp },
    { 0x00000ffc, 0x00000ef8, NULL,                  arm_addsubdp },
    { 0x00000ffc, 0x00000700, NULL,                  arm_mpydp },
    { 0x00000ffc, 0x000005b0, NULL,                  arm_mpydp },
    { 0x00000ffc, 0x000005f0, NULL,                  arm_mpydp },
    { 0x0003effc, 0x00000138, NULL,                  arm_dp_convert },
    { 0x0003effc, 0x00000118, NULL,                  arm_dp_convert },
    { 0x0003effc, 0x00000038, NULL,                  arm_dp_convert },
    { 0x0003effc, 0x00000738, NULL,                  arm_intdp },
    { 0x0003effc, 0x00000778, NULL,                  arm_intdp },
    /* wave5-rows: 32-bit multiply, Galois, dual-result and 40-bit long forms */
    { 0x0000007c, 0x00000000, match_mpyi,            arm_mpyi },
    { 0x0000083c, 0x00000030, match_mpy2_gmpy4,      arm_mpy2_gmpy4 },
    { 0xf000083c, 0x10000030, match_gmpy_word,       arm_gmpy_word },
    { 0x00000ffc, 0x00000ef0, NULL,                  arm_dmv },
    { 0xf0000ffc, 0x10000ef0, NULL,                  arm_rpack2 },
    { 0x0003effc, 0x00000818, NULL,                  arm_sat40 },
    { 0x00000ffc, 0x00000978, NULL,                  arm_subc },
    { 0x0003effc, 0x00000358, NULL,                  arm_abs },
    { 0x0003effc, 0x00000718, NULL,                  arm_abs },
    { 0x00000fdc, 0x00000a18, NULL,                  arm_cmp_long },
    { 0x00000fdc, 0x00000898, NULL,                  arm_cmp_long },
    { 0x00000fdc, 0x00000998, NULL,                  arm_cmp_long },
    { 0x00000fdc, 0x00000a98, NULL,                  arm_cmp_long },
    { 0x00000fdc, 0x00000b98, NULL,                  arm_cmp_long },
    { 0x00000fbc, 0x00000c20, NULL,                  arm_shift_long },
    { 0x00000fbc, 0x000004a0, NULL,                  arm_shift_long },
    { 0x00000fbc, 0x00000d20, NULL,                  arm_shift_long },
    { 0x00000fbc, 0x00000920, NULL,                  arm_shift_long },
    { 0x0ffffffe, 0x001c00e2, NULL,                  arm_b_nrp },
    { 0x00001ffc, 0x00000020, NULL,                  arm_bpos },
};

unsigned cdj_c674x_arm_table_rows(void)
{
    return sizeof(cdj_c674x_arms) / sizeof(cdj_c674x_arms[0]);
}

bool cdj_c674x_arm_table_predicates_are_word_only(uint32_t word)
{
    /* cdj_c674x_arm_table_row_claims evaluates `also` predicates against an arm
     * carrying only w and a.  That is only sound while no predicate reads
     * anything else.  Rather than assert it in a comment, evaluate every
     * predicate twice for the same word with every OTHER field set to two
     * different patterns: a predicate that reads one of them can only agree by
     * coincidence, and across a sweep of words it will not.  Returns false the
     * first time a predicate disagrees with itself. */
    for (unsigned i = 0; i < cdj_c674x_arm_table_rows(); ++i) {
        const CdjC674xArmEntry *entry = &cdj_c674x_arms[i];
        if (!entry->also) continue;
        CdjC674x cpu_a, cpu_b;
        memset(&cpu_a, 0x00, sizeof cpu_a);
        memset(&cpu_b, 0xff, sizeof cpu_b);
        CdjC674xArm one = {
            .cpu = &cpu_a, .out = &cpu_a, .w = word, .a = (word >> 13) & 31,
            .pc = 0, .value = 0, .side = 0, .dst = 0, .b = 0, .cross = 0,
            .scalar_sat_op = 0, .enabled = false, .reg_write = false,
            .control_write = false, .long_offset = false,
        };
        CdjC674xArm two = {
            .cpu = &cpu_b, .out = &cpu_b, .w = word, .a = (word >> 13) & 31,
            .pc = 0xffffffffu, .value = 0xffffffffu, .side = 1, .dst = 31,
            .b = 31, .cross = 1, .scalar_sat_op = 7, .enabled = true,
            .reg_write = true, .control_write = true, .long_offset = true,
        };
        if (entry->also(&one) != entry->also(&two)) return false;
    }
    return true;
}

bool cdj_c674x_arm_table_row_claims(unsigned index, uint32_t word)
{
    if (index >= cdj_c674x_arm_table_rows()) return false;
    const CdjC674xArmEntry *entry = &cdj_c674x_arms[index];
    /* Mirror cdj_c674x_arm_lookup exactly, including its format rule: a word
     * carrying the nonconditional 0001 opcode field in bits 31-28 is claimed
     * only by a row that constrains those bits.  If this drifts from the
     * lookup, the sweep stops measuring real selection. */
    if ((word >> 28) == 1u && (entry->mask >> 28) != 0xfu) return false;
    if ((word & entry->mask) != entry->match) return false;
    if (!entry->also) return true;
    /* Every `also` predicate is a pure function of the instruction word: a
     * sweep over them is therefore well defined without a CPU.  They read only
     * x->w and x->a, and a is the src1 field exactly as the execute loop
     * decodes it (see the `unsigned a = (w >> 13) & 31` there).  Anything a
     * predicate does not read stays zeroed, so if one ever grows a dependency
     * on further state this call would evaluate it against zeros - which is
     * why cdj_c674x_arm_table_predicates_are_word_only() below exists to fail
     * the moment that stops being true. */
    CdjC674xArm probe = { .w = word, .a = (word >> 13) & 31 };
    return entry->also(&probe);
}

bool cdj_c674x_arm_table_row(unsigned index, uint32_t *mask, uint32_t *match,
                             bool *has_also)
{
    if (index >= cdj_c674x_arm_table_rows()) return false;
    const CdjC674xArmEntry *entry = &cdj_c674x_arms[index];
    if (mask) *mask = entry->mask;
    if (match) *match = entry->match;
    if (has_also) *has_also = entry->also != NULL;
    return true;
}

static const CdjC674xArmEntry *cdj_c674x_arm_lookup(const CdjC674xArm *x)
{
    for (unsigned i = 0; i < sizeof(cdj_c674x_arms) /
                             sizeof(cdj_c674x_arms[0]); ++i) {
        const CdjC674xArmEntry *entry = &cdj_c674x_arms[i];
        /* A row whose mask leaves bits 31-28 free is a CONDITIONAL-format row:
         * for it those bits are creg and z.  A word carrying the nonconditional
         * 0001 opcode field there (Figure C-3/D-3/E-3/F-14/H-1) belongs to a
         * different format that merely shares the low opcode bits, so only a
         * row that constrains bits 31-28 explicitly may claim it.
         *
         * Without this the collision is silent and specific: DDOTP4's opfield
         * is 11000, the same five bits MPY32U uses in Figure E-1, and the
         * conditional MPY32 row comes first - so a DDOTP4 word executed as an
         * unsigned 32x32 multiply.  Nothing caught it before because the
         * nonconditional words never used to reach this table at all; they were
         * stopped as unimplemented one step earlier.  Measured with
         * tests/cstub/c674x-arm-claims.c: 5 row pairs over 1,835,008 words. */
        if ((x->w >> 28) == 1u && (entry->mask >> 28) != 0xfu) continue;
        if ((x->w & entry->mask) == entry->match &&
            (!entry->also || entry->also(x)))
            return entry;
    }
    return NULL;
}

bool cdj_c674x_execute(CdjC674x *cpu, const CdjC674xPacket *packet,
                       CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    unsigned memory_count = 0;
    /* Multicycle-NOP duration and conflict state for this execute packet.
     * cdj_c674x_multicycle.h holds the SPRUFE8B rules; timing.cycles replaces
     * the old local "elapsed". */
    CdjC674xPacketTiming timing = CDJ_C674X_PACKET_TIMING_INIT;
    bool nonaligned_memory = false, bdec_issued = false;
    /* Packet execution changes only the scalar/pipeline prefix. The loop
     * schedule and retained instructions are owned by loop_step/setup and
     * remain untouched here, including on branches that idle the loop.
     * Keep a transactional copy, but do not copy that large immutable tail.
     * No helper called with &out may inspect loop or loop_instructions. */
    CdjC674x out CDJ_C674X_UNINITIALIZED;
    memcpy(&out, cpu, offsetof(CdjC674x, loop));
    bool written[2][32] = {{false}}, controls[32] = {false};
    if (cpu->fault) return false;
    if (packet->count > 8) return stop(cpu, cpu->pc, 0, "execute packet exceeds eight instructions");
    for (unsigned i = 0; i < packet->count; ++i) {
        const CdjC674xInstruction *insn = &packet->instructions[i];
        uint32_t w = insn->word, pc = insn->pc, value = 0;
        bool compact = insn->compact;
        unsigned ignored_mask;
        if (spmask_decode(insn, &ignored_mask)) {
            if (i) return stop(cpu, pc, w, "SPMASK must start packet");
            continue; /* Idle loop buffer: SPMASK is a NOP, section 7.15. */
        }
        unsigned nop = nop_cycles(insn);
        if (nop) {
            /* SPRUFE8B printed page 274 gives IDLE bits 16-13 = 1111 with
             * every other bit zero except p, which nop_cycles reports as
             * count 16.  NOP's own entry, printed page 388, says "The maximum
             * value for count is 9", so src 9..14 stay reserved: this narrows
             * the reserved-count rejection to those, it does not remove it.
             * Compact Unop (Figure H-9) cannot reach src 15 at all. */
            if (nop == 16 && !compact) {
                if (!cdj_c674x_packet_idle(&timing))
                    return stop(cpu, pc, insn->word,
                                "multiple multicycle instructions");
                continue;
            }
            if (nop > 9) return stop(cpu, pc, insn->word, "reserved NOP count");
            if (!cdj_c674x_packet_nop(&timing, nop))
                return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            continue;
        }
        /* Compact-header PROT inserts four cycles after every load in the
         * fetch packet, including Dpp/Dstk forms handled by early exits
         * below and 32-bit loads in a mixed packet.  Establish the packet's
         * multicycle duration before format-specific lowering so all load
         * families receive identical timing.  The four cycles are counted once
         * per execute packet: see cdj_c674x_packet_protected_load. */
        if (protected_load(insn) && !cdj_c674x_packet_protected_load(&timing))
            return stop(cpu, pc, insn->word,
                        "multiple multicycle instructions");
        /* Figures C-8 through C-15: lower the compact .D memory families to
         * the existing E1/E3/E5 scalar pipeline.  Pointer registers are
         * always A/B4-7 and ignore RS; data and register-offset operands use
         * RS.  Dinc is scaled post-increment, Ddec scaled pre-decrement.
         * The nonaligned doubleword offset is unscaled for Doff/Dind but
         * scaled for Dinc/Ddec, as called out by Figures C-9/11/13/15. */
        bool compact_doff = compact && (w & 0x0406) == 0x0004;
        bool compact_dind = compact && (w & 0x0c06) == 0x0404;
        bool compact_dinc = compact && (w & 0xcc06) == 0x0c04;
        bool compact_ddec = compact && (w & 0xcc06) == 0x4c04;
        if (compact_doff || compact_dind || compact_dinc || compact_ddec) {
            unsigned dsz = (insn->header >> 16) & 7;
            bool load = (w & 8) != 0, secondary = (w & 0x200) != 0;
            unsigned reg = ((w >> 4) & 7) + ((insn->header & 0x80000) ? 16 : 0);
            unsigned op, extended = 0;
            bool nonaligned_pair = false;
            if (!secondary && (dsz & 4)) {
                bool nonaligned = (w & 0x10) != 0;
                reg &= ~1u;
                op = load ? (nonaligned ? 2 : 6) : (nonaligned ? 7 : 4);
                extended = 0x100;
                nonaligned_pair = nonaligned;
            } else if (!secondary) op = load ? 6 : 7;
            else {
                static const unsigned loads[8] = {1, 2, 0, 4, 6, 2, 3, 4};
                static const unsigned stores[8] = {3, 3, 5, 5, 7, 3, 5, 5};
                op = load ? loads[dsz] : stores[dsz];
                if (dsz == 6) extended = 0x100;
            }
            unsigned offset, mode;
            if (compact_doff) {
                offset = ((w >> 13) & 7) | (((w >> 11) & 1) << 3);
                mode = 1;              /* positive constant offset */
            } else if (compact_dind) {
                offset = ((w >> 13) & 7) +
                         ((insn->header & 0x80000) ? 16 : 0);
                mode = 5;              /* positive register offset */
            } else {
                offset = ((w >> 13) & 1) + 1;
                mode = compact_dinc ? 11 : 8; /* postincrement / predecrement */
            }
            unsigned scaled_nonaligned = nonaligned_pair &&
                (compact_dinc || compact_ddec) ? 1u << 23 : 0;
            w = reg << 23 | (4 + ((w >> 7) & 3)) << 18 | offset << 13 |
                mode << 9 | scaled_nonaligned | extended | (w & 1) << 7 |
                op << 4 | 4 | ((w >> 12) & 1) << 1;
            compact = false;
        }
        if (compact) {
            /* SPRUFE8B D-4/F-22/F-25/F-26: expand saturating compact
             * scalar forms into the same semantic path as full encodings.
             * .S SUB ignores SAT, whereas .L SUB becomes SSUB. */
            unsigned rs = (insn->header & 0x80000) ? 16 : 0;
            unsigned s = w & 1, d = ((w >> 4) & 7) + rs;
            unsigned left = ((w >> 13) & 7) + rs;
            unsigned right = ((w >> 7) & 7) + rs;
            bool sat = (insn->header & 0x4000) != 0;
            unsigned opcode = 0;
            if (sat && (w & 0x040e) == 0)
                opcode = (w & 0x0800) ? 0x1f8 : 0x278;
            else if (sat && !(insn->header & 0x8000) &&
                     (w & 0x0c0e) == 0x000a)
                opcode = 0x820;
            if (opcode) {
                w = d << 23 | right << 18 | left << 13 |
                    (w & 0x1000) | opcode | s << 1;
                compact = false;
            } else if (sat && (w & 0x047e) == 0x0442) {
                unsigned count = ((w >> 13) & 7) | (((w >> 11) & 3) << 3);
                w = right << 23 | right << 18 | count << 13 | 0x8a0 | s << 1;
                compact = false;
            } else if ((w & 0x1c7e) == 0x1c62) {
                w = right << 23 | right << 18 | left << 13 | 0x8e0 | s << 1;
                compact = false;
            } else if ((w & 0x001e) == 0x001e) {
                /* SPRUFE8B Figure E-5 "M3", printed page 744: the only
                 * compact .M format.  src1 bits 15-13, x bit 12, dst bits
                 * 11-10, src2 bits 9-7, op bits 6-5.  The header SAT bit
                 * picks the non-saturating half of the table (MPY, MPYH,
                 * MPYLH, MPYHL) or the saturating one (SMPY, SMPYH, SMPYLH,
                 * SMPYHL).  dst is two bits of an even register - [A0, A2,
                 * A4, A6] with RS = 0 and [A16, A18, A20, A22] with RS = 1
                 * per the figure's note - while src1 and src2 are the usual
                 * three-bit compact fields.  Every mnemonic, opfield and
                 * register mapping here was read back from TI's own
                 * disassembler (dis6x -i on .fphead-framed words). */
                static const unsigned m3_op[2][4] = {
                    {0x19, 0x01, 0x11, 0x09},   /* MPY MPYH MPYLH MPYHL */
                    {0x1a, 0x02, 0x12, 0x0a},   /* SMPY SMPYH SMPYLH SMPYHL */
                };
                w = ((((w >> 10) & 3) * 2) + rs) << 23 | right << 18 |
                    left << 13 | (w & 0x1000) |
                    m3_op[sat][(w >> 5) & 3] << 7 | s << 1;
                compact = false;
            }
        }
        unsigned side = (w >> 1) & 1, dst = (w >> 23) & 31;
        unsigned a = (w >> 13) & 31, b = (w >> 18) & 31;
        unsigned cross = side ^ ((w >> 12) & 1);
        bool callp = (!compact && (w & 0xf000007c) == 0x10000010) ||
                     (compact && (insn->header & 0x8000) && (w & 0x3e) == 0x1a);
        if (callp) {
            side = compact ? w & 1 : (w >> 1) & 1;
            /* CALLP retains a word-scaled PC-relative displacement in its
             * compact Scs10 form (SPRUFE8B CALLP description / Figure F-19).
             * Compact BNOP is the branch family that uses halfword scaling. */
            int32_t offset = compact ? sx(w >> 6, 10) * 4
                                          : sx((w >> 7) & 0x1fffff, 21) * 4;
            if (out.branch_due || !cdj_c674x_packet_multicycle(&timing, 6))
                return stop(cpu, pc, insn->word, "CALLP with pending branch or multicycle instruction");
            for (unsigned j = 0; j < packet->count; ++j) {
                if (j == i) continue;
                CdjC674xInstruction other = packet->instructions[j];
                if ((!other.compact && ((other.word & 0x7c) == 0x10 ||
                     (other.word & 0x1ffe) == 0x162 || (other.word & 0xffe) == 0x362 ||
                     (other.word & 0x1ffc) == 0x120)) ||
                    compact_branch(&other))
                    return stop(cpu, pc, insn->word, "CALLP with parallel control instruction");
            }
            if (written[side][3]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][3] = packet->next_pc; written[side][3] = true;
            if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)offset))
                return stop(cpu, pc, insn->word, "CALLP branch queue conflict");
            continue;
        }
        if (compact) {
            unsigned rs = (insn->header & 0x80000) ? 16 : 0;
            bool simple = true;
            side = w & 1;
            cross = side ^ ((w >> 12) & 1);
            if ((w & 0x1c66) == 0x0866 && ((w >> 3) & 3) != 3) {
                /* Figure G-3: [A0/!A0/B0/!B0] MVK 0/1 on L/S/D. */
                unsigned cc = w >> 14;
                if (!((cpu->r[cc >> 1][0] != 0) ^ (cc & 1))) continue;
                dst = ((w >> 7) & 7) + rs;
                value = (w >> 13) & 1;
            } else if (!(insn->header & (1u << 15)) &&
                       (w & 0x040e) == 0x000a) {
                /* Figure F-22: nonsaturating compact .S ADD/SUB.
                 * SAT-selected SADD was expanded above; SUB ignores SAT. */
                bool subtract = (w & 0x0800) != 0;
                dst = ((w >> 4) & 7) + rs;
                uint32_t left = cpu->r[side][((w >> 13) & 7) + rs];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                value = subtract ? left - right : left + right;
            } else if ((w & 0x047e) == 0x002e) {
                /* SPRUFE8B Figure F-29, Sx2op (printed page 755): compact
                 * in-place .S ADD/SUB, op (bit 11) 0 = ADD, 1 = SUB with
                 * dst = src1 - src2.  Bit 10 is the only bit separating this
                 * format from Sx5 below, so the two masks are disjoint and
                 * neither steals encodings from the other.  Unlike F-22 and
                 * F-25, Figure F-29's table has neither a BR nor a SAT
                 * column: the header bits do not redecode it, so ADD stays
                 * ADD in a saturating fetch packet.  Both three-bit register
                 * fields observe RS and bit 12 crosses src2 (xsint). */
                dst = ((w >> 13) & 7) + rs;
                uint32_t left = cpu->r[side][dst];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                value = (w & 0x0800) ? left - right : left + right;
            } else if ((w & 0x047e) == 0x042e) {
                /* SPRUFE8B Figure F-30, Sx5: compact ADDK adds its
                 * unsigned five-bit constant to the destination in place.
                 * The three-bit destination observes header RS. */
                unsigned constant = ((w >> 13) & 7) |
                                    (((w >> 11) & 3) << 3);
                dst = ((w >> 7) & 7) + rs;
                value = cpu->r[side][dst] + constant;
            } else if ((w & 0x047e) == 0x0436) {
                /* SPRUFE8B Figure C-18, Dx5: B15 plus a word-scaled
                 * unsigned five-bit constant.  B15 is fixed and ignores
                 * RS; the three-bit destination observes RS. */
                unsigned constant = ((w >> 13) & 7) |
                                    (((w >> 11) & 3) << 3);
                dst = ((w >> 7) & 7) + rs;
                value = cpu->r[1][15] + constant * 4;
            } else if ((w & 0x1c7f) == 0x0c77) {
                /* SPRUFE8B Figure C-19, Dx5p: the only architectural
                 * side is D2 (s=1), and both operands are the fixed B15.
                 * Rejecting s=0 below keeps the reserved encoding closed. */
                unsigned constant = ((w >> 13) & 7) |
                                    (((w >> 8) & 3) << 3);
                side = 1;
                dst = 15;
                value = (w & 0x0080) ? cpu->r[1][15] - constant * 4
                                     : cpu->r[1][15] + constant * 4;
            } else if ((w & 0x047e) == 0x0036) {
                /* Figure C-17: compact .D in-place ADD/SUB. */
                dst = ((w >> 13) & 7) + rs;
                uint32_t left = cpu->r[side][dst];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                value = (w & 0x0800) ? left - right : left + right;
            } else if (!(insn->header & (1u << 15)) &&
                       (w & 0x040e) == 0x040a) {
                /* Figure F-23: compact .S SHL/SHR with the special
                 * 0->16 and 7->8 constant translation. */
                unsigned encoded = (w >> 13) & 7;
                unsigned count = encoded == 0 ? 16 : encoded == 7 ? 8 : encoded;
                dst = ((w >> 4) & 7) + rs;
                value = shift_32(cpu->r[cross][((w >> 7) & 7) + rs],
                                 count, (w >> 11) & 1);
            } else if ((w & 0x041e) == 0x0402 &&
                       (w & 0x047e) != 0x0462) {
                /* Figure F-25: compact in-place constant shifts.  SAT
                 * changes op 2 from SHRU into delayed-status SSHL. */
                unsigned op = (w >> 5) & 3;
                if (op == 3)
                    return stop(cpu, pc, insn->word,
                                "reserved compact Ssh5 instruction");
                unsigned count = ((w >> 13) & 7) | (((w >> 11) & 3) << 3);
                dst = ((w >> 7) & 7) + rs;
                value = shift_32(cpu->r[side][dst], count, op);
            } else if ((w & 0x047e) == 0x0462) {
                /* Figure F-26: compact in-place register-count shifts.
                 * As for full-width scalar shifts, only the low six count
                 * bits participate. */
                unsigned op = (w >> 11) & 3;
                dst = ((w >> 7) & 7) + rs;
                unsigned count = cpu->r[side][((w >> 13) & 7) + rs] & 63;
                value = shift_32(cpu->r[side][dst], count, op);
            } else if ((w & 0x040e) == 0x0400) { /* Figure D-5, ADD.L immediate */
                dst = ((w >> 4) & 7) + rs;
                unsigned imm = (w >> 13) & 7;
                int32_t offset = (w & 0x800) ? (int32_t)imm - 8 : (imm ? (int32_t)imm : 8);
                value = cpu->r[cross][((w >> 7) & 7) + rs] + (uint32_t)offset;
            } else if ((w & 0x041e) == 2) { /* Figures F-27/F-28 */
                unsigned op = (w >> 5) & 3;
                unsigned src = ((w >> 7) & 7) + rs;
                uint32_t source = cpu->r[side][src];
                if (op == 3) {
                    unsigned kind = (w >> 11) & 3;
                    unsigned bits = (kind & 1) ? 8 : 16;
                    dst = ((w >> 13) & 7) + rs;
                    value = source & ((1u << bits) - 1);
                    if (!(kind & 2)) value = sx(value, bits);
                } else {
                    unsigned n = ((w >> 13) & 7) | (((w >> 11) & 3) << 3);
                    dst = op ? src : 0; /* EXTU always writes A0/B0, even RS=1. */
                    value = op == 0 ? (source >> (31 - n)) & 1 :
                            op == 1 ? source | (1u << n) : source & ~(1u << n);
                }
            } else if ((w & 0x001e) == 0x0012) { /* Figure F-24, unsigned MVK.S */
                dst = ((w >> 7) & 7) + rs;
                value = ((w >> 13) & 7) | (((w >> 11) & 3) << 3) |
                        (((w >> 5) & 3) << 5) | (((w >> 10) & 1) << 7);
            } else if ((w & 0x1c66) == 0x1866 &&
                       ((w >> 13) & 7) != 6) {
                /* Figure G-4 joins the compact .L/.S/.D zero, one,
                 * increment and XOR-one forms.  Unit-specific op 2 is
                 * integer negation on .L/.S and reserved on .D; op 3 is
                 * decrement on every unit.  Op 6 is the separate .S2 MVC
                 * to ILC handled below.
                 * The unit == 3 arm is kept but is now unreachable: Figure
                 * G-4 has no 11b unit encoding, and 11b there sets bits 4-1
                 * to 1111b, which is the Figure E-5 M3 signature lowered to a
                 * full-width multiply above (TI's disassembler decodes those
                 * 112 words as MPY/SMPY, not as an LSDx1 form). */
                unsigned unit = (w >> 3) & 3;
                unsigned op = (w >> 13) & 7;
                if (unit == 3 || op == 4 || (op == 2 && unit == 2))
                    return stop(cpu, pc, insn->word,
                                "reserved compact LSDx1 instruction");
                dst = ((w >> 7) & 7) + rs;
                uint32_t source = cpu->r[side][dst];
                switch (op) {
                case 0: value = 0; break;
                case 1: value = 1; break;
                case 2: value = 0u - source; break;
                case 3: value = source - 1; break;
                case 5: value = source + 1; break;
                default: value = source ^ 1; break;
                }
            } else if ((w & 0x047e) == 0x0426) { /* Figure D-8, MVK.L */
                dst = ((w >> 7) & 7) + rs;
                value = sx(((w >> 13) & 7) | (((w >> 11) & 3) << 3), 5);
            } else if ((w & 0x147e) == 0x0026) { /* Figure D-9, CMPEQ immediate */
                dst = (w >> 11) & 1;
                value = ((w >> 13) & 7) == cpu->r[side][((w >> 7) & 7) + rs];
            } else if ((w & 0x147e) == 0x1026) { /* Figure D-10, ordered compare */
                dst = ((w >> 11) & 1) + rs;
                uint32_t constant = (w >> 13) & 1;
                uint32_t source = cpu->r[side][((w >> 7) & 7) + rs];
                switch (w >> 14) {
                case 0: value = (int32_t)constant < (int32_t)source; break;
                case 1: value = (int32_t)constant > (int32_t)source; break;
                case 2: value = constant < source; break;
                default: value = constant > source; break;
                }
            } else if ((w & 0x040e) == 0x0408) { /* Figure D-7, L2c */
                dst = (w >> 4) & 1;
                uint32_t left = cpu->r[side][((w >> 13) & 7) + rs];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                unsigned op = ((w >> 9) & 4) | ((w >> 5) & 3);
                switch (op) {
                case 0: value = left & right; break;
                case 1: value = left | right; break;
                case 2: value = left ^ right; break;
                case 3: value = left == right; break;
                case 4: value = (int32_t)left < (int32_t)right; break;
                case 5: value = (int32_t)left > (int32_t)right; break;
                case 6: value = left < right; break;
                default: value = left > right; break;
                }
            } else simple = false;
            if (simple) {
                if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                out.r[side][dst] = value; written[side][dst] = true;
                continue;
            }
            /* SPRUFE8B Figure F-32, Sx1b (printed page 756): register BNOP,
             * s = 1 only.  Whether s = 0 is architecturally legal is an OPEN
             * QUESTION and the manual contradicts itself: Figure F-32 draws s
             * as an unconstrained field and carries no "(s = 1)" parenthetical
             * (unlike Figure F-31 op 110 on the same page), and Table B-1
             * (printed page 715) footnotes ADDKPC, "B register", "B IRP" and
             * "B NRP" as S2-only while pointedly not footnoting "BNOP
             * register" - but the BNOP-register entry on printed page 168 is
             * headed "unit = .S2", its 32-bit figure hardwires bit 1 = 1, and
             * cl6x refuses "BNOP .S1 B4,3" with W0005 "Branch to register
             * requires .S2 unit".  Until that is settled this stays fail-closed
             * like every other unresolved case in this core; accepting an
             * encoding hardware may reject is the worse error.  Opening it is
             * one bit here and in compact_branch(). */
            if ((w & 0x187f) == 0x006f) {
                unsigned n = w >> 13;
                if (!cdj_c674x_packet_multicycle(&timing, n + 1))
                    return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[1][(w >> 7) & 15]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
                continue;
            }
            /* Figure F-31: compact MVC to ILC. SPLOOP observes a four-cycle
             * availability latency (section 7.4.3), tracked separately. */
            if ((w & 0xfc7f) == 0xd86f) {
                unsigned src = ((w >> 7) & 7) + ((insn->header & 0x80000) ? 16 : 0);
                if (controls[13]) return stop(cpu, pc, insn->word, "parallel control write conflict");
                out.control[13] = cpu->r[1][src];
                out.control_ready[13] = cpu->cycles + 4;
                controls[13] = true;
                continue;
            }
            /* Figures F-17/18/20/21: compact BNOP uses halfword offsets.
             * Predicate controls the branch, never the inserted NOPs. */
            if ((insn->header & 0x8000) &&
                ((w & 0x3e) == 0x0a || (w & 0x2e) == 0x2a)) {
                bool unsigned_offset = (w & 0xc000) == 0xc000;
                unsigned n = unsigned_offset ? 5 : w >> 13;
                int32_t displacement = unsigned_offset ? (int32_t)((w >> 6) & 255)
                                                       : sx((w >> 6) & 127, 7);
                bool enabled = !(w & 0x20) ||
                    ((cpu->r[w & 1][0] != 0) ^ ((w >> 4) & 1));
                if (!cdj_c674x_packet_multicycle(&timing, n + 1))
                    return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                if (enabled) {
                    if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)(displacement * 2)))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
                }
                continue;
            }
            /* SPRUFE8B Figure C-16: compact word transfer at a positive
             * constant offset from B15. Unlike Dpp, this form does not
             * update B15 and its three-bit register observes header RS. */
            if ((w & 0x8c07) == 0x8c05) {
                ++memory_count;
                bool load = (w & 8) != 0;
                unsigned bank = (w >> 12) & 1;
                unsigned reg = ((w >> 4) & 7) + rs;
                unsigned offset = ((w >> 13) & 3) | (((w >> 7) & 7) << 2);
                uint32_t address = cpu->r[1][15] + offset * 4;
                uint64_t dummy;
                if ((address & 3) ||
                    (load ? !read_scalar(read, opaque, address, 4, &dummy) :
                            (!write || !write(opaque, address,
                                              cpu->r[bank][reg], 4, false))))
                    return stop(cpu, pc, insn->word, "unmapped B15 stack transfer");
                if (load) {
                    if (out.load_count == 40)
                        return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == bank && out.loads[j].dst == reg)
                            return stop(cpu, pc, insn->word,
                                        "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address,
                        .bank = bank, .dst = reg, .size = 4
                    };
                } else {
                    if (out.store_count == 24)
                        return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .value = cpu->r[bank][reg],
                        .address = address, .size = 4
                    };
                }
                continue;
            }
            /* SPRUFE8B Figure C-21: compact B15 stack forms. Stores use
             * post-decrement and commit in E3; loads use pre-increment and
             * write their destination in E5. Dpp ignores header RS. */
            if ((w & 0x087f) == 0x0077) {
                ++memory_count;
                unsigned bank = (w >> 12) & 1, reg = (w >> 7) & 15;
                unsigned size = (w & 0x8000) ? 8 : 4;
                bool load = (w & 0x4000) != 0;
                uint32_t old_sp = cpu->r[1][15];
                uint32_t delta = size * (((w >> 13) & 1) + 1);
                uint32_t address = load ? old_sp + delta : old_sp;
                uint64_t data = cpu->r[bank][reg], dummy;
                if ((address & (size - 1)) || (size == 8 && (reg & 1)))
                    return stop(cpu, pc, insn->word,
                                "unaligned stack access or invalid register pair");
                if (size == 8) data |= (uint64_t)cpu->r[bank][reg + 1] << 32;
                if (load ? !read_scalar(read, opaque, address, size, &dummy) :
                           (!write || !write(opaque, address, data, size, false)))
                    return stop(cpu, pc, insn->word, "unmapped stack access");
                if (written[1][15]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                if (load) {
                    if (out.load_count == 40)
                        return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == bank &&
                            out.loads[j].dst < reg + (size == 8 ? 2 : 1) &&
                            reg < out.loads[j].dst + queued_result_registers(&out.loads[j]))
                            return stop(cpu, pc, insn->word, "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address, .bank = bank,
                        .dst = reg, .size = size
                    };
                } else {
                    if (out.store_count == 24)
                        return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .value = data,
                        .address = address, .size = size
                    };
                }
                out.r[1][15] = load ? address : old_sp - delta;
                written[1][15] = true;
                continue;
            }
            /* Figures G-1/G-2: one operand is a full 5-bit register,
             * the other uses the header-selected register subset. */
            if ((w & 0x0026) == 0x0006 && ((w >> 3) & 3) != 3) {
                unsigned rs = (insn->header & (1u << 19)) ? 16 : 0;
                unsigned ms = ((w >> 10) & 3) << 3;
                side = w & 1;
                cross = side ^ ((w >> 12) & 1);
                dst = (w >> 13) & 7;
                b = (w >> 7) & 7;
                if (w & 0x40) { dst += ms; b += rs; }
                else { dst += rs; b += ms; }
                if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                out.r[side][dst] = cpu->r[cross][b]; written[side][dst] = true;
                continue;
            }
            /* SPRUFE8B Figure D-4: compact .L ADD/SUB. */
            if ((w & 0x040e) != 0 || (insn->header & (1u << 14)))
                return stop(cpu, pc, insn->word, "compact instruction not implemented");
            rs = (insn->header & (1u << 19)) ? 16 : 0;
            side = w & 1;
            dst = ((w >> 4) & 7) + rs;
            a = ((w >> 13) & 7) + rs;
            b = ((w >> 7) & 7) + rs;
            cross = side ^ ((w >> 12) & 1);
            value = (w & 0x0800) ? cpu->r[side][a] - cpu->r[cross][b]
                                  : cpu->r[side][a] + cpu->r[cross][b];
            if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
            continue;
        }
        CdjC674xInterruptGate interrupt_gate = interrupt_gate_decode(insn);
        if (interrupt_gate != CDJ_C674X_INTERRUPT_GATE_NONE) {
            for (unsigned j = 0; j < packet->count; ++j) {
                if (j != i && interrupt_gate_parallel_conflict(
                                  interrupt_gate, &packet->instructions[j]))
                    return stop(cpu, pc, w,
                                "DINT/RINT parallel instruction conflict");
            }
            /* DINT and RINT change interruptibility in their E1 cycle. The
             * execute packet commits as one architectural operation here;
             * no delayed-result entry is appropriate. CSR.PGIE is unchanged.
             * TSR.GIE and CSR.GIE are one physical bit, while TSR.SGIE holds
             * the saved state used by RINT. */
            if (interrupt_gate == CDJ_C674X_INTERRUPT_GATE_DISABLE) {
                out.control[26] = (out.control[26] & ~3u) |
                                  ((cpu->control[1] & 1u) << 1);
                out.control[1] &= ~1u;
            } else {
                uint32_t gie = (cpu->control[26] >> 1) & 1u;
                out.control[26] = (out.control[26] & ~3u) | gie;
                out.control[1] = (out.control[1] & ~1u) | gie;
            }
            continue;
        }
        unsigned creg = w >> 29, z = (w >> 28) & 1;
        bool enabled = true, reg_write = true, control_write = false;
        /* Bits 31-28 = 0001 is an opcode field, not creg/z, for the C64x+
         * nonconditional encodings; classify it before applying Table 3-9's
         * reserved combination (printed page 77), which belongs only to the
         * formats that really carry creg. CALLP and DINT/RINT continued
         * above; the rest are reached here. */
        CdjC674xUncondKind uncond = cdj_c674x_uncond_classify(w);
        if (creg == 7 || (!creg && z && uncond == CDJ_C674X_UNCOND_NONE))
            return stop(cpu, pc, insn->word, "reserved predicate");
        if (uncond == CDJ_C674X_UNCOND_UNIMPLEMENTED)
            return stop(cpu, pc, insn->word, "instruction not implemented");
        if (uncond != CDJ_C674X_UNCOND_NONE &&
            uncond != CDJ_C674X_UNCOND_ARM_TABLE) {
            /* ADDAB/ADDAH/ADDAW B14/B15, ucst15, dst: a single-cycle E1
             * register write with no memory access and no AMR involvement
             * (printed pages 115, 120, 123). Handled here because bits 3-2
             * would otherwise select Figure C-5's 15-bit-offset transfer. */
            CdjC674xAddaLong adda =
                cdj_c674x_adda_long(uncond, w, cpu->r[1][14], cpu->r[1][15]);
            if (written[adda.side][adda.dst])
                return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[adda.side][adda.dst] = adda.result;
            written[adda.side][adda.dst] = true;
            continue;
        }
        if (creg) {
            static const unsigned bank[] = {0,1,1,1,0,0,0};
            static const unsigned index[] = {0,0,1,2,1,2,0};
            enabled = (cpu->r[bank[creg]][index[creg]] != 0) ^ z;
        }
        CdjC674xArm arm = {
            .cpu = cpu, .out = &out, .packet = packet, .insn = insn,
            .read = read, .write = write, .opaque = opaque,
            .timing = &timing, .written = written, .controls = controls,
            .memory_count = &memory_count,
            .nonaligned_memory = &nonaligned_memory,
            .bdec_issued = &bdec_issued,
            .w = w, .pc = pc, .value = value,
            .side = side, .dst = dst, .a = a, .b = b, .cross = cross,
            .enabled = enabled, .reg_write = reg_write,
            .control_write = control_write,
            /* Two decoded fields more than one arm reads, as the ladder had. */
            .long_offset = (w & 0x0c) == 12,
            .scalar_sat_op = w & 0xffc,
        };
        const CdjC674xArmEntry *entry = cdj_c674x_arm_lookup(&arm);
        if (!entry)
            return stop(cpu, pc, insn->word, "instruction not implemented");
        if (!entry->run(&arm)) return false;
        value = arm.value; dst = arm.dst; side = arm.side;
        reg_write = arm.reg_write; control_write = arm.control_write;
        if (enabled && reg_write) {
            if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
        }
        if (enabled && control_write) {
            if (controls[dst]) return stop(cpu, pc, insn->word, "parallel control write conflict");
            if (dst == 2 || dst == 3) {
                /* ISR/ICR update IFR after one intervening execute packet.
                 * Reuse the ABI-stable delayed-result queue so old checkpoint
                 * layouts retain the in-flight effect. */
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "load queue full");
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = cpu->cycles + 2, .address = value & 0xfff0u,
                    .size = dst == 2 ? CDJ_C674X_DELAYED_IFR_SET
                                     : CDJ_C674X_DELAYED_IFR_CLEAR
                };
            } else if (dst == 0) {
                out.control[0] = value & 0x03ffffffu;
                out.control_ready[0] = cpu->cycles + 2;
            } else if (dst == 1) {
                /* PCC/DCC are documented as ignored on C674x. PWRD support is
                 * device-specific; actual C6747 CPU power-down is not yet
                 * modeled, so ignoring that field is an explicit approximation.
                 * SAT is clear-only through MVC; CPU ID, revision and endian
                 * mode are read-only. */
                out.control[1] = (out.control[1] & 0xffff0100u) |
                                 (out.control[1] & value & 0x200u) |
                                 (value & 3u);
                /* CSR.GIE and TSR.GIE are the same physical bit. */
                out.control[26] = (out.control[26] & ~1u) | (value & 1u);
                /* CSR.PGIE and ITSR.GIE are also one physical bit. */
                out.control[27] = (out.control[27] & ~1u) |
                                  ((value >> 1) & 1u);
            } else if (dst == 24) {
                /* GFPGFR reserves bits 31-27 and 23-8.  Its SIZE and POLY
                 * fields are ordinary MVC R/W bits (Figure 2-6, page 40). */
                out.control[24] = value & 0x070000ffu;
            } else if (dst == 18 || dst == 19 || dst == 20) {
                /* FADCR/FAUCR/FMCR bits 31-27 and 15-11: "A value written to
                 * this field has no effect" (SPRUFE8B Tables 2-25/2-26/2-27,
                 * printed pages 59, 61 and 63), so MVC drops them rather than
                 * storing bits a read must then hide.  The warning bits the FP
                 * instructions OR in all live in the unreserved ranges. */
                out.control[dst] = value & 0x07ff07ffu;
            } else if (dst == 21) {
                out.control[21] = value & 0x3fu;
            } else if (dst == 10) {
                /* SPRUFE8B 2.9.14.1-.3, printed page 56: reset clears the
                 * counter and disables it; writing TSCL enables it, the value
                 * is ignored, counting starts in the following cycle, and
                 * once enabled it cannot be disabled under program control.
                 * Therefore a later write has no counter state to change. */
                if (out.control_ready[16] == 0)
                    out.control_ready[16] = cpu->cycles + 1u;
            } else if (dst == 4) {
                /* Reset remains enabled. NMIE can be set by MVC but not
                 * manually cleared; maskable enables are ordinary RW bits. */
                out.control[4] = (value & 0xfff0u) |
                                 ((out.control[4] | value) & 2u) | 1u;
            } else if (dst == 5) {
                /* HPEINT is derived on read; only the aligned IST base writes. */
                out.control[5] = value & 0xfffffc00u;
            } else if (dst == 27) {
                out.control[27] = value & 0x0000c6dfu;
                out.control[1] = (out.control[1] & ~2u) |
                                 ((value & 1u) << 1);
            } else if (dst == 26) {
                /* Privilege and hardware-owned TSR fields need a later
                 * execution-mode model. Preserve them while allowing the
                 * GIE/SGIE pair used by interrupt-critical firmware. */
                out.control[26] = (out.control[26] & ~3u) | (value & 3u);
                out.control[1] = (out.control[1] & ~1u) | (value & 1u);
            } else {
                out.control[dst] = value;
            }
            controls[dst] = true;
            if (dst == 13 || dst == 14) out.control_ready[dst] = cpu->cycles + 4;
        }
    }
    /* Issue only appends queue entries; retirement below can modify and
     * remove them.  Commit the pre-retirement extent, including slots that
     * become inactive, to preserve checkpoint bytes and struct padding. */
    unsigned store_peak = out.store_count, load_peak = out.load_count;
    if (nonaligned_memory && memory_count > 1)
        return stop(cpu, cpu->pc, 0, "parallel access with nonaligned memory instruction");
    /* Same-cycle overlapping RAM reads/writes need bus arbitration that
     * this core does not yet model. Do not choose an invented ordering.
     * SPRUFE8B defines no order for them: Table 4-8 (printed page 590) puts a
     * store's "memory write" in E3 and Table 4-10 (printed page 593) puts a
     * load's "memory read at that address" in E3, so a parallel .D1 load and
     * .D2 store to one address access memory in the same cycle.  Section 4.2.5
     * (printed page 594) defines only the sequential case - "a load following a
     * store accesses the value placed in memory by that store in the cycle
     * after the store is completed" - and 3.8.5 (printed page 80), Table 4-40
     * and Table 4-41 (printed pages 618-619) state no same-address rule.  The
     * C6745/C6747 device manual SPRUH91D describes no L1D arbitration for it
     * either.  Settling this needs hardware or a TI statement, not a reading of
     * the CPU manual, so this halts rather than guess read-before-write. */
    for (unsigned j = 0; j < out.load_count; ++j) {
        if (!queued_memory_load(&out.loads[j])) continue;
        for (unsigned k = 0; k < out.store_count; ++k) {
            if (out.loads[j].due - 2 != out.stores[k].due) continue;
            const CdjC674xLoad *load = &out.loads[j];
            const CdjC674xStore *store = &out.stores[k];
            for (unsigned l = 0; l < (load->size & 255); ++l)
            for (unsigned s = 0; s < (store->size & 255); ++s)
                if (circular_address(load->address, load->address + l, load->size >> 8) ==
                    circular_address(store->address, store->address + s, store->size >> 8))
                    return stop(cpu, cpu->pc, 0, "simultaneous overlapping RAM accesses not implemented");
        }
    }
    /* Reject E5/E1 register collisions before any RAM transaction commits. */
    for (unsigned j = 0; j < out.load_count; ++j) {
        unsigned count = queued_result_registers(&out.loads[j]);
        if (count && out.loads[j].due == cpu->cycles + 1 &&
            (written[out.loads[j].bank][out.loads[j].dst] ||
             (count == 2 && written[out.loads[j].bank][out.loads[j].dst + 1])))
            return stop(cpu, cpu->pc, 0, "delayed-result write conflict");
        if (!out.loads[j].size && out.loads[j].address &&
            out.loads[j].due == cpu->cycles + 1 &&
            controls[out.loads[j].sign_extend ? 20 : 18])
            return stop(cpu, cpu->pc, 0, "delayed FP-status write conflict");
        if (out.loads[j].size == CDJ_C674X_DELAYED_FAUCR &&
            out.loads[j].due == cpu->cycles + 1 && controls[19])
            return stop(cpu, cpu->pc, 0, "delayed FP-status write conflict");
    }
    if (packet->single_cycle && timing.cycles > 1) {
        out.idle_cycles = timing.cycles - 1;
        timing.cycles = 1;
    }
    /* SPRUFE8B printed page 274: IDLE performs "an infinite multicycle NOP
     * that terminates upon servicing an interrupt, or a branch occurs due to
     * an IDLE instruction being in the delay slots of a branch".  The packet
     * still issues its one cycle below; the unbounded wait afterwards is the
     * sentinel, which the branch-completion arm clears and which
     * interrupt_pipe_down replaces with the entry interval. */
    if (timing.idle) out.idle_cycles = CDJ_C674X_IDLE_FOREVER;
    out.pc = packet->next_pc;
    for (unsigned i = 0; i < timing.cycles; ++i) {
        ++out.cycles;
        if (out.cycle_tick) out.cycle_tick(out.cycle_opaque);
        for (unsigned j = 0; j < out.store_count;) {
            CdjC674xStore *store = &out.stores[j];
            if (store->due > out.cycles) { ++j; continue; }
            if (!write_transfer(write, opaque, store->address, store->value, store->size, true))
                return stop(cpu, cpu->pc, 0, "RAM store callback broke commit guarantee");
            memmove(store, store + 1, (--out.store_count - j) * sizeof(*store));
        }
        for (unsigned j = 0; j < out.load_count;) {
            CdjC674xLoad *load = &out.loads[j];
            if (queued_memory_load(load) && load->due == out.cycles + 2) {
                uint64_t data;
                if (!read_transfer(read, opaque, load->address, load->size, &data))
                    return stop(cpu, cpu->pc, 0, "RAM load mapping changed during execution");
                if (load->sign_extend) data = sx(data, (load->size & 255) * 8);
                load->value = data;
            }
            if (load->due > out.cycles) { ++j; continue; }
            if (load->size == CDJ_C674X_DELAYED_SAT) {
                /* Functional-unit set wins a simultaneous MVC write/clear
                 * (SPRUFE8B 2.8.3 and 2.9.13). Parallel units accumulate. */
                out.control[1] |= 0x200u;
                out.control[21] |= load->address & 0x3fu;
                memmove(load, load + 1, (--out.load_count - j) * sizeof(*load));
                continue;
            }
            if (load->size == CDJ_C674X_DELAYED_FAUCR) {
                /* DP compare warning bits, due with dst on E2 (SPRUFE8B
                 * 4.2.10, printed page 598).  Sticky, and parallel .S units
                 * accumulate into their own halves. */
                out.control[19] |= load->address;
                memmove(load, load + 1, (--out.load_count - j) * sizeof(*load));
                continue;
            }
            if (load->size == CDJ_C674X_DELAYED_IFR_SET ||
                load->size == CDJ_C674X_DELAYED_IFR_CLEAR) {
                uint32_t sets = 0, clears = 0;
                for (unsigned k = j; k < out.load_count; ++k) {
                    CdjC674xLoad *effect = &out.loads[k];
                    if (effect->due > out.cycles) continue;
                    if (effect->size == CDJ_C674X_DELAYED_IFR_SET)
                        sets |= effect->address;
                    else if (effect->size == CDJ_C674X_DELAYED_IFR_CLEAR)
                        clears |= effect->address;
                }
                /* Incoming/set requests win over a simultaneous clear. */
                out.control[2] = ((out.control[2] & ~clears) | sets) & 0xfff2u;
                for (unsigned k = 0; k < out.load_count;) {
                    CdjC674xLoad *effect = &out.loads[k];
                    if (effect->due <= out.cycles &&
                        (effect->size == CDJ_C674X_DELAYED_IFR_SET ||
                         effect->size == CDJ_C674X_DELAYED_IFR_CLEAR)) {
                        memmove(effect, effect + 1,
                                (--out.load_count - k) * sizeof(*effect));
                    } else ++k;
                }
                j = 0;
                continue;
            }
            out.r[load->bank][load->dst] = load->value;
            if (queued_result_registers(load) == 2)
                out.r[load->bank][load->dst + 1] = load->value >> 32;
            if (!load->size)
                out.control[load->sign_extend ? 20 : 18] |= load->address;
            memmove(load, load + 1, (--out.load_count - j) * sizeof(*load));
        }
        if (out.branch_due && out.cycles == out.branch_due) {
            out.pc = out.branch_target;
            /* SPRUFE8B 7.14: a taken branch idles an active loop buffer.
             * Do not clear the special idle SPLX state restored by B IRP;
             * section 7.7.3.2 requires it until the return SPLOOP starts. */
            if (out.loop_active) {
                /*
                 * One corner the manual does not settle, kept fail-closed.
                 *
                 * If an interrupted loop is still pending return (its retained
                 * metadata is live) while the handler's OWN loop buffer is
                 * active, idling that buffer here clears TSR.SPLX - this core's
                 * only return-window proxy.  The interrupted loop would then
                 * resume down the non-returning path and silently lose all five
                 * differences 7.13.2 and 7.13.5 (printed page 698) require on a
                 * return: the packet parallel with SPLOOP would execute, the
                 * SPMASKed program-memory operations would not be annulled, the
                 * buffer SPMASK would not be ignored, BNOP would not be
                 * neutralised, and SPLOOPD would re-add its four-cycle count
                 * delay.  7.7.3.2 does not say whether SPLX survives a taken
                 * branch out of a nested loop, so there is no defensible
                 * behaviour to implement and a silent divergence is the worst
                 * available option.  Refuse instead.
                 *
                 * This is narrower than the "nested SPLOOP would overwrite
                 * retained buffer" refusal it replaces: that one rejected every
                 * ISR-local SPLOOP, which 7.7.3.1 and 7.13.1 permit.  A scan of
                 * 400,000 traced strict steps from runs/dsp-wm8740-fault-replay-1
                 * finds no step with both loop_active and branch_due, so the
                 * firmware this was fixed for does not reach it.
                 */
                if (loop_retained_valid(&out))
                    return stop(cpu, out.pc, 0,
                                "taken branch out of a nested loop with an "
                                "interrupted loop pending");
                loop_set_active(&out, false);
                loop_clear_interrupt_phase(&out);
            }
            if (out.branch_count) {
                out.branch_due = out.branch_queue[0].due;
                out.branch_target = out.branch_queue[0].target;
                memmove(out.branch_queue, out.branch_queue + 1,
                        --out.branch_count * sizeof(out.branch_queue[0]));
            } else out.branch_due = 0;
            out.idle_cycles = 0;
            break;
        }
    }
    ++out.packets;
    if (store_peak > 24 || load_peak > 40) {
        /* Keep the original copy boundary for an invalid incoming state. */
        memcpy(cpu, &out, offsetof(CdjC674x, loop));
    } else {
        /* Copy scalar ranges including padding; untouched queue capacity
         * already has its original bytes in cpu.  The full copy-in above
         * preserves old operand reads and padding in newly appended slots. */
        memcpy(cpu, &out, offsetof(CdjC674x, stores));
        memcpy(cpu->stores, out.stores, store_peak * sizeof(out.stores[0]));
        memcpy((char *)cpu + offsetof(CdjC674x, store_count),
               (const char *)&out + offsetof(CdjC674x, store_count),
               offsetof(CdjC674x, loads) - offsetof(CdjC674x, store_count));
        memcpy(cpu->loads, out.loads, load_peak * sizeof(out.loads[0]));
        memcpy((char *)cpu + offsetof(CdjC674x, load_count),
               (const char *)&out + offsetof(CdjC674x, load_count),
               offsetof(CdjC674x, loop) - offsetof(CdjC674x, load_count));
    }
    return true;
}

static bool loop_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    CdjC674x out = *cpu;
    /* Also reconciles legacy checkpoints written before SPLX tracking. */
    out.control[26] |= CDJ_C674X_TSR_SPLX;
    CdjC674xPacket combined = {.next_pc = cpu->pc, .single_cycle = true};
    CdjC674xPacket direct = {0};
    LoopMask masking = {.cpu = &out};
    bool loading = !out.loop.sealed;
    bool post = out.loop.sealed && out.loop.cycle >= out.loop.post_cycle;
    if (loading && !out.loop_wait) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        /* Section 7.7.3.3 indexes storage by LBC (cycle modulo II), not
         * the number of source fetches. NOP/setup packets can exceed 14.
         * The 48-cycle, 112-tag and simultaneous-issue limits still apply. */
        ++out.loop_packets;
        bool finish = false;
        /* The PROT expansion belongs to the execute packet, not to each load
         * in it: see cdj_c674x_packet_protected_load and printed page 93. */
        bool protect = false;
        unsigned delay = 0, count = 0;
        uint32_t tags[8];
        bool has_mask = spmask_decode(&source.instructions[0], &masking.mask);
        if (has_mask)
            out.control_ready[CDJ_C674X_LOOP_CONTEXT] |=
                CDJ_C674X_LOOP_HAS_SPMASK;
        bool returning =
            (out.loop_pred_history & CDJ_C674X_LOOP_RETURNING) != 0;
        /* The interrupt-return SPMASK rule is not a timing approximation: it
         * is stated outright by SPRUFE8B 7.7.3.3 (printed page 679), 7.11.5
         * (printed page 696) and 7.13.2 (printed page 698), and is applied
         * below in both timing modes. */
        for (unsigned i = 0; i < source.count; ++i) {
            CdjC674xInstruction insn = source.instructions[i];
            uint32_t w = insn.word;
            unsigned mask;
            if (spmask_decode(&insn, &mask)) {
                if (i) return stop(cpu, insn.pc, w, "SPMASK must start packet");
                continue;
            }
            bool full_kernel = !insn.compact &&
                (w & 0xf03ffffc) == 0x34000;
            bool compact_kernel = insn.compact &&
                (w & 0x3c7e) == 0x1c66;
            if (full_kernel || compact_kernel) {
                if (i != 0) return stop(cpu, insn.pc, w, "SPKERNEL must start packet");
                /* SPRUFE8B Figure H-7: bit 0 is field[5], bits 9:7
                 * are field[2:0], and bits 15:14 are field[4:3].
                 * Table 3-29's stage-bit reversal is applied below,
                 * after reconstructing this combined field. */
                unsigned field = compact_kernel ?
                    ((w & 1) << 5) | ((w >> 7) & 7) |
                    (((w >> 14) & 3) << 3) : (w >> 22) & 63;
                unsigned cbits = 0, stage = 0;
                while ((1u << cbits) < out.loop.ii) ++cbits;
                for (unsigned j = 5; j >= cbits && j < 6; --j)
                    stage |= ((field >> j) & 1) << (5 - j);
                unsigned cycle = field & ((1u << cbits) - 1);
                if (cycle >= out.loop.ii) return stop(cpu, insn.pc, w, "invalid SPKERNEL cycle");
                delay = stage * out.loop.ii + cycle;
                if (out.loop.predicate_loop && delay)
                    return stop(cpu, insn.pc, w, "SPLOOPW requires zero SPKERNEL fetch delay");
                finish = true;
                continue;
            }
            if (finish && interrupt_gate_decode(&insn) !=
                              CDJ_C674X_INTERRUPT_GATE_NONE)
                return stop(cpu, insn.pc, w,
                            "DINT/RINT cannot share SPKERNEL packet");
            unsigned n = nop_cycles(&insn);
            /* SPRUFE8B 7.13.2: returned-loop BNOP label,n is NOP n+1.
             * Decode only immediate forms, leaving register branches under
             * the existing fail-closed control-instruction check. */
            if (returning && !insn.compact && (w & 0x1ffc) == 0x120) {
                if ((w >> 29) == 7 || (!(w >> 29) && (w & (1u << 28))))
                    return stop(cpu, insn.pc, w, "reserved predicate");
                n = ((w >> 13) & 7) + 1;
            } else if (returning && insn.compact && (insn.header & 0x8000) &&
                       ((w & 0x3e) == 0x0a || (w & 0x2e) == 0x2a)) {
                n = (w & 0xc000) == 0xc000 ? 6 : (w >> 13) + 1;
            }
            if (n) {
                if (n > 9 || (n > 1 && (finish || out.loop_wait)))
                    return stop(cpu, insn.pc, w, "invalid loop NOP packet");
                if (n > 1) out.loop_wait = n - 1;
                continue;
            }
            bool spmasked = false;
            if (has_mask && masking.mask) {
                unsigned unit = instruction_unit(&insn);
                if (!unit) return stop(cpu, insn.pc, w, "SPMASK unit not implemented");
                spmasked = (unit & masking.mask) != 0;
                /* Section 7.7.3.3 (printed page 679): "When returning to an
                 * SPLOOP(D) instruction with the SPLX bit in TSR set to 1,
                 * SPMASKed instructions from program memory execute like a
                 * NOP", and they are not stored in the loop buffer either
                 * way; 7.13.2 adds that the loop-buffer operation on the
                 * masked unit then executes normally (the masking.mask = 0
                 * below). For the 7.11.1 shape - an SPMASK inside the loop
                 * body, whose masked operation "is executed only once and is
                 * not loaded to the SPLOOP buffer" (printed page 693) - the
                 * one-shot setup already ran before the interrupt. */
                if (spmasked && returning) {
                    /* Same paragraph: "The NOP cycles associated with
                     * ADDKPC, BNOP, or protected LD instructions that are
                     * masked, are always executed when resuming an
                     * interrupted SPLOOP(D)." The operation is annulled, its
                     * four loading cycles are not. Multicycle NOPs already
                     * took the branch above, before masking is consulted. */
                    if (protected_load(&insn)) {
                        if (finish || (out.loop_wait && !protect))
                            return stop(cpu, insn.pc, w,
                                        "invalid protected loop load packet");
                        out.loop_wait = 4;
                        protect = true;
                    }
                    continue;
                }
            }
            /* SPRUFE8B 3.10 and 7.7.3.3: PROT expands the program stream
             * with four empty loading cycles. Buffered instructions continue
             * issuing during those cycles, just as for explicit NOP 4.
             * Do not reinsert fetch delays when the load is replayed. This
             * expansion applies even when predicated false or SPMASKed. */
            if (protected_load(&insn)) {
                /* Parallel protected loads share the packet's one issue cycle
                 * and therefore its single four-cycle expansion.  A wait left
                 * by any other instruction, in this packet or an earlier one,
                 * and SPKERNEL (printed page 481) still reject. */
                if (finish || (out.loop_wait && !protect))
                    return stop(cpu, insn.pc, w, "invalid protected loop load packet");
                out.loop_wait = 4;
                protect = true;
                insn.header &= ~(1u << 20);
            }
            if ((!insn.compact && ((w & 0x1ffe) == 0x162 || (w & 0x7c) == 0x10 ||
                                  (w & 0xffe) == 0x362 || (w & 0x1ffc) == 0x120)) ||
                compact_branch(&insn))
                return stop(cpu, insn.pc, w, "loop body control instruction not implemented");
            if (spmasked) {
                direct.instructions[direct.count++] = insn;
                continue;
            }
            /* Section 7.18: MVC may execute from memory when masked but
             * cannot enter the loop buffer. */
            if ((!insn.compact && (w & 0xffe) == 0x3a2) ||
                (insn.compact && (w & 0xfc7f) == 0xd86f))
                return stop(cpu, insn.pc, w, "unmasked loop MVC not permitted");
            if (returning && loop_retained_valid(&out)) {
                uint32_t tag;
                if (!loop_retained_tag(&out, &insn, &tag))
                    return stop(cpu, insn.pc, w,
                                "SPLOOP retained instruction mismatch");
                tags[count++] = tag;
            } else {
                if (out.loop_tags == 112)
                    return stop(cpu, insn.pc, w,
                                "loop instruction capacity exceeded");
                tags[count++] = out.loop_tags;
                out.loop_instructions[out.loop_tags++] = insn;
            }
        }
        if (!cdj_c674x_loop_load(&out.loop, tags, count, finish, delay))
            return stop(cpu, cpu->pc, 0, "invalid loop buffer load");
        if (finish && returning) {
            if (loop_retained_valid(&out) &&
                !loop_retained_schedule_complete(&out))
                return stop(cpu, cpu->pc, 0,
                            "SPLOOP retained schedule mismatch");
            /* An ISR-local SPLOOP may have replaced the retained metadata in
             * functional mode. Its return reconstructs from stable program
             * memory, but still finishes the one-time pipe-up phase here. */
            out.loop_pred_history &= ~CDJ_C674X_LOOP_RETURNING;
            loop_clear_retained(&out);
        }
        combined.next_pc = source.next_pc;
    } else if (loading) {
        --out.loop_wait;
        if (!cdj_c674x_loop_load(&out.loop, NULL, 0, false, 0))
            return stop(cpu, cpu->pc, 0, "loop dynamic length exceeded");
    }
    bool interrupt_armed = loop_interrupt_armed(&out);
    bool interrupt_draining = loop_interrupt_draining(&out);
    if (post && !interrupt_draining && !out.idle_cycles) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        bool has_mask = spmask_decode(&source.instructions[0], &masking.mask);
        for (unsigned i = has_mask ? 1 : 0; i < source.count; ++i) {
            unsigned mask;
            if (spmask_decode(&source.instructions[i], &mask))
                return stop(cpu, source.instructions[i].pc, source.instructions[i].word,
                            "SPMASK must start packet");
            direct.instructions[direct.count++] = source.instructions[i];
        }
        combined.next_pc = source.next_pc;
    } else if (out.idle_cycles) {
        /* The IDLE sentinel never counts down; only an interrupt or a branch
         * ends it (SPRUFE8B printed page 274). */
        if (out.idle_cycles != CDJ_C674X_IDLE_FOREVER) --out.idle_cycles;
    }
    uint32_t tags[8]; unsigned count; bool scheduler_post, drained;
    if (out.loop_pred_history & CDJ_C674X_LOOP_RETURNING)
        masking.mask = 0;
    if (!cdj_c674x_loop_issue_filtered(&out.loop, tags, &count, &scheduler_post,
                                     &drained, loop_allow, &masking))
        return stop(cpu, cpu->pc, 0, "loop issue capacity exceeded");
    if (masking.unknown) return stop(cpu, cpu->pc, 0, "buffered SPMASK unit not implemented");
    if (count + direct.count > 8) return stop(cpu, cpu->pc, 0, "loop/direct packet capacity exceeded");
    for (unsigned i = 0; i < count; ++i)
        combined.instructions[combined.count++] = out.loop_instructions[tags[i]];
    for (unsigned i = 0; i < direct.count; ++i)
        combined.instructions[combined.count++] = direct.instructions[i];
    /* SPRUFE8B 7.10: sample the selected condition each cycle. At the end
     * of a stage, use its value three cycles earlier; the first three loop
     * cycles cannot terminate. No ILC/RILC access and no epilog. */
    bool end_while = out.loop.predicate_loop && out.loop.cycle >= 4 &&
        out.loop.cycle % out.loop.ii == 0 && !(out.loop_pred_history & 4);
    if (end_while && !out.loop.sealed)
        return stop(cpu, cpu->pc, 0, "SPLOOPW termination during loading not implemented");
    bool condition = (cpu->r[out.loop_pred_bank][out.loop_pred_reg] != 0) ^ out.loop_pred_invert;
    out.loop_pred_history =
        (out.loop_pred_history & CDJ_C674X_LOOP_RETURNING) |
        ((((out.loop_pred_history & 7) << 1) | condition) & 7);
    if (!cdj_c674x_execute(&out, &combined, read, write, opaque))
        return stop(cpu, out.fault_pc, out.fault_word, out.fault);
    uint64_t launched = 1 + out.loop.cycle / out.loop.ii;
    if (out.loop.delayed_count) {
        /* SPLOOPD forces termination false and suppresses ILC decrement
         * during the first three loop cycles.  At later stage boundaries,
         * test ILC before conditionally decrementing it (7.9.2/7.9.3). */
        if (!scheduler_post && !interrupt_armed && !interrupt_draining &&
            out.loop.cycle >= 4 && out.loop.cycle % out.loop.ii == 0 &&
            out.control[13])
            --out.control[13];
    } else if (!out.loop.predicate_loop &&
               !interrupt_armed && !interrupt_draining)
        out.control[13] = launched < out.loop.iterations ? out.loop.iterations - launched : 0;
    if (interrupt_armed) {
        if (!cdj_c674x_loop_interrupt_drain(&out.loop))
            return stop(cpu, cpu->pc, 0,
                        "SPLOOP interrupt drain schedule invalid");
        loop_set_interrupt_phase(&out, loop_selected_interrupt(&out),
                                 CDJ_C674X_LOOP_INTERRUPT_DRAINING);
    }
    if (end_while) {
        loop_set_active(&out, false);
        /* SPLOOPW may terminate while interrupt draining.  Section 7.10.3
         * then interrupts the post-loop packet rather than restarting the
         * loop setup address. */
        if (interrupt_draining)
            loop_clear_interrupt_phase(&out);
        if (interrupt_draining)
            loop_clear_retained(&out);
    }
    if (drained && scheduler_post &&
        (!interrupt_draining || (!out.load_count && !out.store_count)))
        loop_set_active(&out, false);
    *cpu = out;
    return true;
}

bool cdj_c674x_step_capture_direct(CdjC674x *cpu, CdjC674xRead read,
                                  CdjC674xWrite write, void *opaque,
                                  CdjC674xPacket *direct)
{
    if (direct) direct->count = 0;
    if (cpu->fault) return false;
    if (cpu->loop_active) return loop_step(cpu, read, write, opaque);
    if (cpu->idle_cycles) {
        CdjC674x out = *cpu;
        CdjC674xPacket idle = {.next_pc = cpu->pc, .single_cycle = true};
        /* The IDLE sentinel never counts down; only an interrupt or a branch
         * ends it (SPRUFE8B printed page 274). */
        if (out.idle_cycles != CDJ_C674X_IDLE_FOREVER) --out.idle_cycles;
        if (!cdj_c674x_execute(&out, &idle, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        *cpu = out;
        return true;
    }
    CdjC674xPacket packet;
    if (!cdj_c674x_fetch(cpu, read, opaque, &packet)) return false;
    if (direct) *direct = packet;
    CdjC674xInstruction first = packet.instructions[0];
    bool compact_sploop = first.compact && (first.word & 0xbc7f) == 0x0c66;
    bool compact_sploopd = first.compact &&
        (first.word & 0xbc7f) == 0x0c67;
    bool compact_sploopd_reload = first.compact &&
        (first.word & 0xbc7e) == 0x8c66;
    bool while_loop = !first.compact && (first.word & 0x007ffffe) == 0x3e000;
    bool full_sploop = !first.compact &&
        (first.word & 0x007ffffc) == 0x38000;
    bool full_sploopd = !first.compact &&
        (first.word & 0x007ffffc) == 0x3a000;
    if (compact_sploopd_reload)
        return stop(cpu, cpu->pc, first.word,
                    "SPLOOPD reload not implemented");
    if (full_sploop || compact_sploop || full_sploopd ||
        compact_sploopd || while_loop) {
        uint32_t w = first.word;
        /* SPRUFE8B 7.7.3.2, printed page 679: "There is one case where the
         * SPLX bit is set to 1 when the loop buffer is idle" - B IRP (or B
         * NRP) restoring a task interrupted in SPLOOP - and hardware consults
         * SPLX only for a loop started "in the branch delay slots" of such a
         * branch. This partial interpreter uses the bit itself as its
         * return-window proxy because it has no separate pipeline provenance
         * in the checkpoint ABI; that is sound here because MVC cannot write
         * TSR.SPLX, all normal loop-idle paths clear it, and an active buffer
         * never reaches this path. Loop-buffer *reload* (7.9.6, printed page
         * 686) is a different feature and is still not claimed: see the
         * SPLOOPD reload refusal above and the SPKERNELR rejection. */
        bool returning = (cpu->control[26] & CDJ_C674X_TSR_SPLX) != 0;
        bool delayed_loop = (full_sploopd || compact_sploopd) && !returning;
        unsigned pred = first.compact ? 0 : w >> 29;
        /* An interrupt service routine may use the loop buffer itself, and
         * resuming the interrupted loop does not depend on the buffer's
         * contents surviving. SPRUFE8B 7.7.3.1 (printed page 678): on return
         * "execution is resumed at the address of the SPLOOP(D/W)
         * instruction, and the loop is piped back up by executing a prolog" -
         * a prolog out of program memory, which is why 7.13.2 has to
         * neutralise the packet parallel with SPLOOP, the SPMASKed
         * program-memory operations and BNOP, and why its note requires the
         * ISR to restore ILC 4 cycles ahead. 7.13.1 (printed page 697) states
         * the whole contract an ISR owes - "must save and restore the ITSR or
         * NTSR, ILC, and RILC registers" - with no loop buffer in it.
         * (7.7.3.3's assembler error for "Another SPLOOP(D) instruction is
         * encountered" is about one appearing while a loop is *loading*;
         * this path is only reached with the loop buffer idle.) So nothing
         * needs retaining: loop_set_setup below drops this core's retained
         * metadata for a non-returning setup and a later return rebuilds from
         * program memory. That metadata is a cross-check, not architectural
         * state, and where it does survive it is still checked ("SPLOOP
         * retained instruction mismatch", "SPLOOP retained schedule
         * mismatch", "SPLOOP interrupt-return interval mismatch"). */
        if (while_loop ? (!pred || pred == 7) : (!first.compact && (w >> 28) != 0))
            return stop(cpu, cpu->pc, w, "unsupported loop predicate");
        if (!while_loop && !delayed_loop &&
            cpu->cycles < cpu->control_ready[13])
            return stop(cpu, cpu->pc, w, "ILC not yet available");
        CdjC674x out = *cpu;
        /* SPRUFE8B Figure H-5 scatters compact ii-1 across bits 9:7 and
         * bit 14. GNU binutils format nfu_uspl independently agrees. */
        unsigned ii = first.compact ? (((w >> 7) & 7) | ((w >> 11) & 8)) + 1
                                    : ((w >> 23) & 31) + 1;
        if (!cdj_c674x_loop_init(&out.loop, ii, cpu->control[13]))
            return stop(cpu, cpu->pc, w, "invalid SPLOOP interval");
        if (returning && loop_retained_valid(cpu) &&
            ii != loop_retained_ii(cpu))
            return stop(cpu, cpu->pc, w,
                        "SPLOOP interrupt-return interval mismatch");
        loop_set_setup(&out, first.pc, returning);
        out.loop.predicate_loop = while_loop;
        out.loop.delayed_count = delayed_loop;
        out.loop_pred_history = returning ? CDJ_C674X_LOOP_RETURNING : 0;
        if (while_loop) {
            static const unsigned banks[] = {0,1,1,1,0,0,0};
            static const unsigned regs[] = {0,0,1,2,1,2,0};
            out.loop_pred_bank = banks[pred]; out.loop_pred_reg = regs[pred];
            out.loop_pred_invert = (w >> 28) & 1;
            out.loop_pred_history =
                (returning ? CDJ_C674X_LOOP_RETURNING : 0) | 7;
        }
        /* Loop setup cannot share a packet with multicycle operations. */
        for (unsigned j = 1; j < packet.count; ++j) {
            CdjC674xInstruction other = packet.instructions[j];
            uint32_t v = other.word;
            unsigned mask;
            if (spmask_decode(&other, &mask))
                return stop(cpu, other.pc, v, "SPMASK cannot share loop setup packet");
            if (protected_load(&other) ||
                (nop_cycles(&other) > 1) ||
                interrupt_gate_decode(&other) !=
                    CDJ_C674X_INTERRUPT_GATE_NONE ||
                (!other.compact && ((v & 0xffe) == 0x362 || (v & 0x7c) == 0x10 ||
                                   (v & 0x1ffc) == 0x120)) ||
                compact_branch(&other))
                return stop(cpu, other.pc, v, "multicycle loop setup packet not implemented");
        }
        memmove(packet.instructions, packet.instructions + 1, (--packet.count) * sizeof(packet.instructions[0]));
        /* Section 7.13.2: operations parallel with the return SPLOOP(D/W)
         * are NOPs.  Executing an empty packet still advances its one cycle
         * and keeps all older delayed effects architectural. */
        if (returning) packet.count = 0;
        if (!cdj_c674x_execute(&out, &packet, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        if (delayed_loop) {
            uint32_t minimum = (4 + ii - 1) / ii;
            if (out.control[13] > UINT32_MAX - minimum)
                return stop(cpu, cpu->pc, w, "SPLOOPD iteration count overflow");
            out.loop.iterations = out.control[13] + minimum;
        }
        bool active = !(cpu->branch_due && out.cycles >= cpu->branch_due);
        if (active) loop_set_active(&out, true);
        else {
            out.loop_active = false;
            if (!returning) out.control[26] &= ~CDJ_C674X_TSR_SPLX;
        }
        out.loop_wait = out.loop_packets = 0;
        out.loop_tags = loop_retained_valid(&out) ?
            loop_retained_tags(&out) : 0;
        if (!while_loop && !delayed_loop && out.control[13]) --out.control[13];
        *cpu = out;
        return true;
    }
    return cdj_c674x_execute(cpu, &packet, read, write, opaque);
}

bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    return cdj_c674x_step_capture_direct(cpu, read, write, opaque, NULL);
}
