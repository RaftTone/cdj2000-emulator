"""Data loads from the reserved gap above L2 RAM complete with zero (stock's scramble)."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_c674x_data_gap(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-data-gap'
    q = ROOT / 'emulator/qemu'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(q),
                    str(ROOT / 'tests/cstub/c674x-data-gap.c'), str(q / 'cdj_c674x.c'),
                    *[str(q / f'cdj_c674x_{n}.c') for n in (
                        'uncond', 'mpy', 'dotp', 'packed8', 'packed16', 'packbits', 'mpy32',
                        'dp', 'approx', 'sp', 'control', 'loop')],
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
