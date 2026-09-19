"""Scalar saturating ISA/status tests, independent of firmware availability."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_c674x_scalar_saturation(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-saturation'
    subprocess.run([
        cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/c674x-saturation.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
        str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
        '-o', str(binary), '-lm',
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
