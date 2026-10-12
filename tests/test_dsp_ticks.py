"""Batched board ticks (cdj_dsp_ticks.h) against per-cycle ticking."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_batched_ticks_match_per_cycle(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'dsp-ticks'
    qemu = ROOT / 'emulator/qemu'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(qemu), str(ROOT / 'tests/cstub/dsp-ticks.c'),
                    str(qemu / 'cdj_c6747_pll.c'), str(qemu / 'cdj_c6747_timer.c'),
                    str(qemu / 'cdj_c6747_spi.c'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], check=True, timeout=120,
                            capture_output=True, text=True)
    assert 'dsp ticks: ok' in result.stdout
