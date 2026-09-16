"""Execute a parallel instruction packet in the built Blackfin simulator."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("writeback", [True, False])
def test_parallel_store_reads_old_register_value(tmp_path, writeback):
    assembler = os.environ.get('BFIN_AS') or shutil.which('bfin-elf-as') or str(ROOT / 'build/bfin-binutils/gas/as-new')
    linker = os.environ.get('BFIN_LD') or shutil.which('bfin-elf-ld') or str(ROOT / 'build/bfin-binutils/ld/ld-new')
    simulator = ROOT / 'bin' / ('cdj-run.exe' if os.name == 'nt' else 'cdj-run')
    if not Path(assembler).is_file() or not Path(linker).is_file() or not simulator.is_file():
        pytest.skip('requires built Blackfin simulator and BFIN_AS/BFIN_LD tools')
    obj, elf = tmp_path / 'parallel.o', tmp_path / 'parallel.elf'
    subprocess.run([assembler, str(ROOT / 'tests/bfin/parallel-store.s'), '-o', str(obj)], check=True)
    subprocess.run([linker, '-Ttext=0x1000', '-e', '_start', str(obj), '-o', str(elf)], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('BFIN_')}
    if writeback:
        env['BFIN_PARALLEL_WRITEBACK'] = '1'
    result = subprocess.run(
        [str(simulator), '--environment', 'user', '--memory-region', '0,64M', str(elf)],
        env=env,
        capture_output=True, text=True, timeout=10,
    )
    output = result.stdout + result.stderr
    if writeback:
        assert result.returncode == 0, output
        assert 'FAIL' not in output
    else:
        # Same guest must detect the original early-register-write bug.
        assert result.returncode != 0, output
        assert 'actual value 0xb' in output and 'FAIL' in output, output
