"""C674x packet cache, fast path and decoder cross-check; no firmware needed
except for the optional whole-image cross-check."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
HELPERS = ('uncond', 'mpy', 'dotp', 'packed8', 'packed16', 'packbits', 'mpy32',
           'dp', 'approx', 'sp', 'control', 'loop')
CORE = [str(ROOT / 'emulator/qemu/cdj_c674x.c'),
        *[str(ROOT / f'emulator/qemu/cdj_c674x_{h}.c') for h in HELPERS]]


def test_c674x_packet_cache_matches_uncached_execution(tmp_path):
    """Random programs, self-modifying code, host code uploads, fetch-window
    remaps and retirement faults: cached+fast and uncached runs stay
    byte-identical (CPU and memory) after every step."""
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'c674x-packet-cache'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tests/cstub/c674x-packet-cache.c'), *CORE,
                    '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=120)


def test_c674x_benchmark_core_builds_and_runs(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    binary = tmp_path / 'bench'
    subprocess.run([cc, '-std=c11', '-O2', '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tools/cdj_dsp/benchmark_core.c'), *CORE,
                    '-o', str(binary), '-lm'], check=True)
    out = subprocess.run([str(binary)], check=True, timeout=120,
                         capture_output=True, text=True).stdout
    assert 'step-alu' in out and 'step-mem' in out


def test_decode_crosscheck_whole_image(tmp_path):
    """Set CDJ_DECODE_CROSSCHECK_CHECKPOINT to an NXS DSP checkpoint taken
    after stage2 is loaded (and have Homebrew binutils' gobjdump)."""
    checkpoint = os.environ.get('CDJ_DECODE_CROSSCHECK_CHECKPOINT')
    objdump = shutil.which('gobjdump') or shutil.which('tic6x-elf-objdump')
    cc = shutil.which('cc')
    if not checkpoint or not objdump or not cc:
        pytest.skip('requires CDJ_DECODE_CROSSCHECK_CHECKPOINT, gobjdump and cc')
    from tools.cdj_dsp.replay import SOURCES
    tool = tmp_path / 'decode-crosscheck'
    subprocess.run([cc, '-O2', '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tools/cdj_dsp/decode_crosscheck.c'),
                    *[str(p) for p in SOURCES if p.name != 'replay.c'],
                    '-o', str(tool), '-lm'], check=True)
    subprocess.run(['python3', '-m', 'tools.cdj_dsp.decode_crosscheck', checkpoint,
                    str(tmp_path / 'report.json'), '--tool', str(tool),
                    '--objdump', objdump], cwd=ROOT, check=True, timeout=600)
