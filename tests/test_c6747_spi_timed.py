"""Timed schematic-backed SPI1/WM8740 transfer behavior."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_timed_spi1_wm8740(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'spi-timed'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tests/cstub/c6747-spi-timed.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
