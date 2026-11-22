"""The DSP model's fill-level accounting for PCM streams (cdj2000_dsp_levels.h)."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_pcm_level_accounting(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'dsp-levels'
    subprocess.run([
        cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/dsp-levels.c'), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
