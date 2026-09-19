"""Shared connected/replay SYSCLK2 clock adapter, not a full boot test."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_spi_core_clock(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'spi-clock'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tests/cstub/c6747-spi-clock.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_pll.c'),
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
