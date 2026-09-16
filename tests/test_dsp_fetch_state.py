"""Coverage fetch must be independent of registers, pipelines and loop state."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_fetch_requires_only_pc_and_fault(tmp_path):
    source = tmp_path / 'fetch.c'
    source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "cdj_c674x.h"
static uint32_t memory[24];
static unsigned reads;
static bool read_word(void *opaque, uint32_t address, uint32_t *value) {
    (void)opaque;
    ++reads;
    if (address >= sizeof(memory)) return false;
    *value = memory[address / 4];
    return true;
}
int main(void) {
    for (unsigned mode=0; mode<4; ++mode) {
        for (unsigned i=0; i<24; ++i)
            memory[i] = mode == 1 ? 1 : 0;
        if (mode == 2) memory[7] = memory[15] = memory[23] = 0xefffffff;
        for (unsigned pc=0; pc<=100; ++pc) {
            CdjC674x full, sparse;
            memset(&full, 0, sizeof(full));
            memset(&sparse, 1, sizeof(sparse));
            full.pc = sparse.pc = pc;
            full.fault = sparse.fault = mode == 3 ? "prior fault" : NULL;
            full.fault_pc = sparse.fault_pc = 123;
            full.fault_word = sparse.fault_word = 456;
            CdjC674xPacket a = {0}, b = {0};
            reads = 0;
            bool accepted = cdj_c674x_fetch(&full, read_word, NULL, &a);
            unsigned count = reads;
            reads = 0;
            assert(accepted == cdj_c674x_fetch(&sparse, read_word, NULL, &b));
            assert(count == reads);
            assert(full.pc == sparse.pc);
            assert(full.fault_pc == sparse.fault_pc);
            assert(full.fault_word == sparse.fault_word);
            assert((full.fault == NULL) == (sparse.fault == NULL));
            if (full.fault) assert(!strcmp(full.fault, sparse.fault));
            assert(a.count == b.count && a.next_pc == b.next_pc);
            for (unsigned i=0; i<a.count; ++i) {
                assert(a.instructions[i].pc == b.instructions[i].pc);
                assert(a.instructions[i].word == b.instructions[i].word);
                assert(a.instructions[i].header == b.instructions[i].header);
                assert(a.instructions[i].compact == b.instructions[i].compact);
            }
        }
    }
}
''')
    binary = tmp_path / 'fetch'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(source),
                    str(ROOT / 'emulator/qemu/cdj_c674x.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_direct_packet_observer_preserves_execution_and_reads(tmp_path):
    source = tmp_path / 'observe.c'
    source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "cdj_c674x.h"
static uint32_t memory[16];
static unsigned reads;
static bool read_word(void *opaque, uint32_t address, uint32_t *value) {
    (void)opaque;
    ++reads;
    if (address >= sizeof(memory)) return false;
    *value = memory[address / 4];
    return true;
}
static bool write_word(void *p, uint32_t a, uint64_t v, unsigned n, bool c) {
    (void)p; (void)a; (void)v; (void)n; (void)c;
    assert(0); return false;
}
int main(void) {
    for (unsigned mode=0; mode<5; ++mode) {
        memset(memory, 0, sizeof(memory));
        /* Changed source, loop setup, inserted idle, decode fault, fetch fault. */
        memory[0] = mode == 1 ? 0x38000 : mode == 3 ? 0xffffffff : 0;
        CdjC674x a, b;
        cdj_c674x_reset(&a, mode == 4 ? 64 : 0);
        a.control[13] = 2;
        if (mode == 2) a.idle_cycles = 1;
        b = a;
        CdjC674xPacket captured;
        reads = 0;
        bool accepted = cdj_c674x_step(&a, read_word, write_word, NULL);
        unsigned count = reads;
        reads = 0;
        assert(accepted == cdj_c674x_step_capture_direct(
            &b, read_word, write_word, NULL, &captured));
        assert(count == reads);
        assert(!memcmp(&a, &b, sizeof(a)));
        if (accepted) {
            assert(captured.count == (mode == 2 ? 0 : 1));
            if (captured.count) assert(captured.instructions[0].word == memory[0]);
        }
    }
    /* No cached instruction survives a source-memory change between steps. */
    CdjC674x cpu;
    cdj_c674x_reset(&cpu, 0);
    memory[0] = 0;
    CdjC674xPacket packet;
    assert(cdj_c674x_step_capture_direct(&cpu, read_word, write_word, NULL, &packet));
    cpu.pc = 0;
    memory[0] = 0x38000;
    cpu.control[13] = 2;
    assert(cdj_c674x_step_capture_direct(&cpu, read_word, write_word, NULL, &packet));
    assert(packet.count == 1 && packet.instructions[0].word == 0x38000);
}
''')
    binary = tmp_path / 'observe'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(source),
                    str(ROOT / 'emulator/qemu/cdj_c674x.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
