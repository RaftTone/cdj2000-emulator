"""Exercise the actual cold helper with a synthetic, non-firmware flash bank."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_cold_helper_success_and_decline(tmp_path):
    source = ROOT / 'build/gdb-17.2/sim/bfin/bfin-sim.c'
    cc = shutil.which('cc')
    if not source.exists() or not cc:
        pytest.skip('requires patched simulator source and compiler')
    text = source.read_text()
    begin = text.index('static int __attribute__ ((noinline))\nbfin_fast_lzss')
    end = text.index('\nbu32\ninterp_insn_bfin', begin)
    helper = text[begin:end]
    # A run of backreferences to the space-filled window produces spaces.
    bank = bytes([0, *([0, 15] * 8)]) * 14000
    flash = tmp_path / 'synthetic-flash'
    flash.write_bytes(bytes(0xebba0) + bank[:0x39d55])
    harness = tmp_path / 'test.c'
    harness.write_text('''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t bu32;
typedef struct { bu32 r[3], pc, rets; bool did_jump; } SIM_CPU;
typedef SIM_CPU *SIM_DESC;
#define CPU_STATE(cpu) (cpu)
#define BFIN_CPU_STATE (*cpu)
#define DREG(n) (cpu->r[n])
#define RETSREG (cpu->rets)
#define SET_PCREG(v) (cpu->pc = (v))
#define xmalloc malloc
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
static unsigned char written[0xf8853];
static size_t total;
static size_t sim_write(SIM_DESC s, uint32_t address, const void *bytes, size_t n) {
    (void)s;
    assert(address >= 0x1000 && address - 0x1000 + n <= sizeof(written));
    memcpy(written + address - 0x1000, bytes, n); total += n; return n;
}
''' + helper + '''
int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("BFIN_FAST_LZSS", argv[1], 1);
    SIM_CPU cpu = {.r={0x200ebba0, 0x1000, 0xf8853}, .pc=123, .rets=456};
    /* Unsupported source declines without changing guest registers/memory. */
    cpu.r[0]++;
    assert(!bfin_fast_lzss(&cpu));
    assert(cpu.pc == 123 && !cpu.did_jump && total == 0);
    cpu.r[0]--;
    assert(bfin_fast_lzss(&cpu) == 2);
    assert(cpu.pc == 456 && cpu.did_jump && total == sizeof(written));
    for (size_t i=0; i<sizeof(written); i++) assert(written[i] == ' ');
    return 0;
}
''')
    binary = tmp_path / 'test'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(harness), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary), str(flash)], check=True)
