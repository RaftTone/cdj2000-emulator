"""DP source pairs are read half by half (the divf failure); see the cstub."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_c674x_dp_late_high_half(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-dp-late-high'
    q = ROOT / 'emulator/qemu'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(q),
                    str(ROOT / 'tests/cstub/c674x-dp-late-high.c'), str(q / 'cdj_c674x.c'),
                    *[str(q / f'cdj_c674x_{n}.c') for n in (
                        'uncond', 'mpy', 'dotp', 'packed8', 'packed16', 'packbits', 'mpy32',
                        'dp', 'approx', 'sp', 'control', 'loop')],
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def test_ti_rts_divf_matches_ieee_division(tmp_path):
    """1.0f/1024.0f used to return 2^-17.  Needs the user's TI cgt (not in the repository)."""
    import struct
    cgt = Path('/Applications/ti/ti-cgt-c6000_8.5.0.LTS')
    cc = shutil.which('cc')
    if not cc or not (cgt / 'bin/cl6x').exists():
        pytest.skip('requires a C compiler and the TI C6000 compiler')
    (tmp_path / 't.c').write_text('float fdiv(float a, float b) { return a / b; }\n')
    (tmp_path / 't.cmd').write_text(
        '-e fdiv\nMEMORY { P: o = 0x1000 l = 0x20000  D: o = 0x30000 l = 0x8000 }\n'
        'SECTIONS { .text: > P  .const: > D .bss: > D .far: > D .fardata: > D .neardata: > D .cinit: > D }\n')
    subprocess.run([str(cgt / 'bin/cl6x'), '-mv6740', '--abi=eabi', '-O2', f'-I{cgt}/include',
                    str(tmp_path / 't.c'), '-z', str(tmp_path / 't.cmd'), '-o', str(tmp_path / 't.out'),
                    '--rom_model', f'-l{cgt}/lib/rts6740_elf.lib', '-m', str(tmp_path / 't.map')],
                   check=True, cwd=tmp_path, capture_output=True)
    elf = (tmp_path / 't.out').read_bytes()
    phoff, = struct.unpack_from('<I', elf, 0x1c)
    _, off, va, _, size, *_ = struct.unpack_from('<IIIIIIII', elf, phoff)
    assert va == 0x1000
    (tmp_path / 't.bin').write_bytes(elf[off:off + size])
    entry = [l.split()[0] for l in (tmp_path / 't.map').read_text().splitlines()
             if l.strip().endswith(' fdiv') and l.split()[0].startswith('0000')][-1]
    q = ROOT / 'emulator/qemu'
    binary = tmp_path / 'divf'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(q),
                    str(ROOT / 'tests/cstub/c674x-divf.c'), str(q / 'cdj_c674x.c'),
                    *[str(q / f'cdj_c674x_{n}.c') for n in (
                        'uncond', 'mpy', 'dotp', 'packed8', 'packed16', 'packbits', 'mpy32',
                        'dp', 'approx', 'sp', 'control', 'loop')],
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary), str(tmp_path / 't.bin'), '0x' + entry], check=True, timeout=30)
