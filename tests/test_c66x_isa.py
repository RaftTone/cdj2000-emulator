"""The C6727 core is strict C67x+, and the lint finds the same words (no firmware needed)."""
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
C6X = ROOT / 'emulator/c6x'


def _cc():
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    return cc


def test_c66x_core_traps_c64x_words(tmp_path):
    cc = _cc()
    try:
        glib = subprocess.run(['pkg-config', '--cflags', '--libs', 'gmodule-2.0'],
                              capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        pytest.skip('requires pkg-config gmodule-2.0 (the core links it for its JIT loader)')
    binary = tmp_path / 'c66x-isa'
    subprocess.run([cc, '-std=gnu11', '-O1', '-w', '-ffp-contract=off', '-I', str(C6X),
                    str(ROOT / 'tests/cstub/c66x-isa.c'), *map(str, sorted(C6X.glob('*.c'))),
                    *glib, '-lm', '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr


def test_c67x_lint(tmp_path):
    cc = _cc()
    binary = tmp_path / 'c67x-lint'
    subprocess.run([cc, '-std=gnu11', '-O1', '-w', '-I', str(C6X),
                    str(ROOT / 'tools/cdj_dsp/c67x_lint.c'), str(C6X / 'c66x_decode.c'),
                    '-o', str(binary)], check=True)
    bad = tmp_path / 'bad.bin'
    good = tmp_path / 'good.bin'
    # nop, the word, then a table of small integers (reads as C64x packed ops).
    bad.write_bytes(struct.pack('<4I', 0, 0xB080A358, 0x0300A358, 0x13070))
    good.write_bytes(struct.pack('<4I', 0, 0xB08000F8, 0x030000F8, 0x13070))
    run = lambda *a: subprocess.run([str(binary), *a], capture_output=True, text=True)

    r = run(str(bad), '0x1000F510')
    assert r.returncode == 1
    assert '0x1000f514: 0xb080a358' in r.stdout and '0x1000f518: 0x0300a358' in r.stdout
    assert run('--skip', '0x1000F51C:0x1000F520', str(good), '0x1000F510').returncode == 0
    # Against itself as the stock image nothing differs.
    assert run('--stock', str(bad), str(bad), '0x1000F510').returncode == 0
