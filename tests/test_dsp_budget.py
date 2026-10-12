"""Host-fairness quota parsing for the legacy connected DSP callback."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_legacy_dsp_budget_parser(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    pkg_config = shutil.which('pkg-config')
    if not pkg_config:
        pytest.skip('requires pkg-config')
    glib_flags = subprocess.check_output(
        [pkg_config, '--cflags', '--libs', 'glib-2.0'], text=True).split()
    binary = tmp_path / 'dsp-budget'
    subprocess.run([
        cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'tests/cstub'),
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/dsp-budget.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_budget.c'),
        '-o', str(binary), *glib_flags,
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
