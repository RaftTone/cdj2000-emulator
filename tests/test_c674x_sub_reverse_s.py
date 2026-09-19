"""Regression coverage for reverse cross-path SUB .S encodings."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_c674x_sub_reverse_cross_s_executes(tmp_path):
    """SUB .S1X/.S2X reverse forms execute with src1 on the cross path."""
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("requires C compiler")

    source = tmp_path / "sub-reverse-s.c"
    source.write_text(
        r'''#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "cdj_c674x.h"

static bool issue(CdjC674x *cpu, uint32_t word)
{
    CdjC674xPacket packet = {
        .instructions = {{.word = word, .pc = cpu->pc}},
        .count = 1,
        .next_pc = cpu->pc + 4,
    };
    return cdj_c674x_execute(cpu, &packet, NULL, NULL, NULL);
}

int main(void)
{
    CdjC674x cpu;

    /* SUB .S2X A2,B3,B0: cross A2 minus local B3.  Distinct fields catch
     * accidentally swapping src1/src2 with the destination-adjacent form. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][2] = 2u;
    cpu.r[1][3] = 7u;
    assert(issue(&cpu, 0x00087d73u));
    assert(cpu.r[1][0] == 0xfffffffbu);

    /* SUB .S1X B2,A3,A0: the mirrored bank form. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[1][2] = 9u;
    cpu.r[0][3] = 4u;
    assert(issue(&cpu, 0x00087d71u));
    assert(cpu.r[0][0] == 5u);

    /* Arithmetic wraps in the 32-bit destination, and a false predicate
     * leaves the destination untouched. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][2] = 0u;
    cpu.r[1][3] = 1u;
    assert(issue(&cpu, 0x00087d73u));
    assert(cpu.r[1][0] == 0xffffffffu);

    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][0] = 0u;
    cpu.r[0][2] = 2u;
    cpu.r[1][3] = 7u;
    cpu.r[1][0] = 0xdeadbeefu;
    assert(issue(&cpu, 0xc0087d73u)); /* [A0] SUB .S2X A2,B3,B0 */
    assert(cpu.r[1][0] == 0xdeadbeefu);

    /* The matching true predicate uses the same A0 creg with z cleared. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][0] = 1u;
    cpu.r[0][2] = 2u;
    cpu.r[1][3] = 7u;
    cpu.r[1][0] = 0xdeadbeefu;
    assert(issue(&cpu, 0xc0087d73u)); /* [A0] SUB .S2X A2,B3,B0 */
    assert(cpu.r[1][0] == 0xfffffffbu);

    return 0;
}
''',
        encoding="utf-8",
    )
    binary = tmp_path / "sub-reverse-s"
    sources = [
        "cdj_c674x.c", "cdj_c674x_uncond.c", "cdj_c674x_mpy.c",
        "cdj_c674x_dotp.c", "cdj_c674x_packed8.c", "cdj_c674x_packed16.c",
        "cdj_c674x_packbits.c", "cdj_c674x_mpy32.c", "cdj_c674x_dp.c",
        "cdj_c674x_approx.c", "cdj_c674x_sp.c", "cdj_c674x_control.c",
        "cdj_c674x_loop.c",
    ]
    subprocess.run([
        cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "emulator/qemu"), str(source),
        *(str(ROOT / "emulator/qemu" / name) for name in sources),
        "-o", str(binary), '-lm',
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
