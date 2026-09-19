"""Pure bounded deferred DSP scheduler state and checkpoint ABI."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_dsp_scheduler_state_machine(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'dsp-scheduler'
    subprocess.run([
        cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/dsp-scheduler.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_scheduler.c'),
        '-o', str(binary), '-lm',
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
