"""Regression coverage for the C674x LMBD constant source form."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_c674x_lmbd_cst5_executes(tmp_path):
    """LMBD cst5 (the form emitted by stock NXS DSP code) is executable."""
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("requires C compiler")

    source = tmp_path / "lmbd-cst5.c"
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
    cdj_c674x_reset(&cpu, 0x1000);

    /* LMBD .L2X 1,A4,B1: cst5=1, src2=A4, dst=B1. */
    cpu.r[0][4] = 0x08000000u;
    assert(issue(&cpu, 0x00903d5bu));
    assert(cpu.r[1][1] == 4u);

    /* The cst5 encoding is five bits, while LMBD consumes only its LSB. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][4] = 0x40000000u;
    assert(issue(&cpu, 0x00901d5bu)); /* cst5=0 */
    assert(cpu.r[1][1] == 0u);

    /* .L1 uses the same-bank A4 source, while .L2X above uses A4 across
     * from the B unit.  Both forms must select the source bank from x/s. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][4] = 0x08000000u;
    assert(issue(&cpu, 0x00902d58u)); /* LMBD .L1 1,A4,A1 */
    assert(cpu.r[0][1] == 4u);

    /* cst5 is five bits, but LMBD consumes only its LSB: cover an even and
     * odd value at both ends of the field, plus the architected no-match 32. */
    const struct { uint32_t word, source, expected; } constants[] = {
        {0x00901d5bu, 0x40000000u, 0u},  /* cst5=0 */
        {0x00903d5bu, 0x08000000u, 4u},  /* cst5=1 */
        {0x0093dd5bu, 0x40000000u, 0u},  /* cst5=30 */
        {0x0093fd5bu, 0x08000000u, 4u},  /* cst5=31 */
        {0x00903d5bu, 0x00000000u, 32u}, /* no matching 1 */
    };
    for (unsigned i = 0; i < sizeof(constants) / sizeof(constants[0]); ++i) {
        cdj_c674x_reset(&cpu, 0x1000);
        cpu.r[0][4] = constants[i].source;
        assert(issue(&cpu, constants[i].word));
        assert(cpu.r[1][1] == constants[i].expected);
    }

    /* A false predicate suppresses the write, as for the register form. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][0] = 0;
    cpu.r[0][4] = 0x08000000u;
    cpu.r[1][1] = 0xdeadbeefu;
    assert(issue(&cpu, 0xc0903d5bu)); /* [A0] LMBD .L2X 1,A4,B1 */
    assert(cpu.r[1][1] == 0xdeadbeefu);

    /* The true form uses the same A0 creg with z cleared. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][0] = 1u;
    cpu.r[0][4] = 0x08000000u;
    cpu.r[1][1] = 0xdeadbeefu;
    assert(issue(&cpu, 0xc0903d5bu)); /* [A0] LMBD .L2X 1,A4,B1 */
    assert(cpu.r[1][1] == 4u);

    /* Keep the existing register-src1 form working while adding cst5. */
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][4] = 1u;
    cpu.r[0][6] = 0x08000000u;
    assert(issue(&cpu, 0x02988d78u)); /* LMBD .L1 A4,A6,A5 */
    assert(cpu.r[0][5] == 4u);

    return 0;
}
''',
        encoding="utf-8",
    )
    binary = tmp_path / "lmbd-cst5"
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
