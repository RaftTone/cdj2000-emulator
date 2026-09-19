"""Compare compact SPKERNEL field scatter with independent full encodings."""
from pathlib import Path
import os
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_c674x_spkernel_fields(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-spkernel-fields'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tests/cstub/c674x-spkernel-fields.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_uncond.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_mpy.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dotp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed8.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packed16.c'), str(ROOT / 'emulator/qemu/cdj_c674x_packbits.c'), str(ROOT / 'emulator/qemu/cdj_c674x_mpy32.c'), str(ROOT / 'emulator/qemu/cdj_c674x_dp.c'), str(ROOT / 'emulator/qemu/cdj_c674x_approx.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_sp.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_control.c'),
                    str(ROOT / 'emulator/qemu/cdj_c674x_loop.c'),
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_ti_compact_spkernel_oracle(tmp_path):
    directory = os.environ.get('C6X_TI_BIN')
    if not directory:
        pytest.skip('set C6X_TI_BIN to independently check TI assembler encodings')
    directory = Path(directory)
    subprocess.run([str(directory / 'cl6x'), '--silicon_version=6740',
                    '--asm_listing', str(ROOT / 'tests/ti/spkernel-oracle.asm')],
                   cwd=tmp_path, check=True, timeout=30)
    listing = (tmp_path / 'spkernel-oracle.lst').read_text()
    for word, stage in [('9c67', 3), ('dc66', 6), ('1f66', 24)]:
        assert re.search(rf'\b{word}\s+SPKERNEL\s+{stage},0\b', listing)
    decoded = subprocess.run([str(directory / 'dis6x'), 'spkernel-oracle.obj'],
                             cwd=tmp_path, check=True, capture_output=True,
                             text=True, timeout=30).stdout
    for word, stage in [('9c67', 3), ('dc66', 6), ('1f66', 24)]:
        assert re.search(rf'\b{word}\s+SPKERNEL\s+{stage},0\b', decoded)
