"""Firmware-free PLL/IDLE probes through the installed BF531 simulator.

These validate emulator integration, not physical PLL timing. The pending-lock
case must work; the lock-after-IDLE case records the existing early-resume bug.
"""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('lock_already_pending', [True, False],
                         ids=['pending-before-idle', 'lock-during-idle'])
def test_pll_idle_wake(tmp_path, request, lock_already_pending):
    assembler = os.environ.get('BFIN_AS') or shutil.which('bfin-elf-as') or str(ROOT / 'build/bfin-binutils/gas/as-new')
    linker = os.environ.get('BFIN_LD') or shutil.which('bfin-elf-ld') or str(ROOT / 'build/bfin-binutils/ld/ld-new')
    simulator = Path(os.environ.get('BFIN_WAKE_SIM', str(ROOT / 'bin' / ('cdj-run.exe' if os.name == 'nt' else 'cdj-run'))))
    if not all(Path(p).is_file() for p in (assembler, linker, simulator)):
        pytest.skip('requires the Blackfin assembler, linker and simulator')
    # A 75 ms model deadline is safely beyond the 50 ms catch-up cap. CLI
    # masks normal interrupts: PLL's pending wake flag must suffice by itself.
    # Polling consumes the lock event before IDLE in the positive control.
    poll = '''
    P1.H = 0x0100; P1.L = 0;
poll_lock:
    R0 = W[P0+12] (Z);
    CC = BITTST(R0, 5);
    IF CC JUMP locked;
    P1 += -1;
    R2 = P1;
    CC = R2 == 0;
    IF !CC JUMP poll_lock;
    R0 = 0; DBGAL(R0, 0xffff);
locked:
''' if lock_already_pending else ''
    source = f'''
.global _start
.text
_start:
    CLI R7;
    P0.H = 0xffc0; P0.L = 0;
    R0 = 15; W[P0+4] = R0;
    R0 = 5000 (Z); W[P0+16] = R0;
    R0 = 1; W[P0] = R0;
    {poll}
    IDLE;
    R0 = W[P0+12] (Z);
    R1 = 32; R0 = R0 & R1;
    DBGAL(R0, 32);
    OUTC 'p'; OUTC 'a'; OUTC 's'; OUTC 's'; HLT;
'''
    asm, obj, elf = (tmp_path / name for name in ('wake.s', 'wake.o', 'wake.elf'))
    asm.write_text(source)
    subprocess.run([assembler, str(asm), '-o', str(obj)], check=True, capture_output=True, timeout=15)
    subprocess.run([linker, '-Ttext=0x1000', '-e', '_start', str(obj), '-o', str(elf)], check=True, capture_output=True, timeout=15)
    env = {k: v for k, v in os.environ.items() if not k.startswith('BFIN_')}
    env.update(BFIN_TIME_BASE='wall', BFIN_CCLK_HZ='1000000')
    result = subprocess.run([str(simulator.resolve()), '--model', 'bf531', '--environment',
                             'operating', '--memory-region', '0,64M', str(elf)],
                            env=env, capture_output=True, text=True, timeout=4)
    output = result.stdout + result.stderr
    if not lock_already_pending:
        # Mark only after tool setup/execution, so build errors, timeouts or
        # unrelated simulator failures cannot disappear behind an xfail.
        known_failure = (result.returncode == 2
                         and 'DBGAL (R0, 0x0020); actual value 0' in output)
        success = result.returncode == 0 and 'pass' in output and 'FAIL' not in output
        assert known_failure or success, output
        request.node.add_marker(pytest.mark.xfail(
            strict=True, reason='known wall-clock IDLE early resume before PLL lock delivery; graduate this test when fixed'))
    assert result.returncode == 0 and 'pass' in output and 'FAIL' not in output, output
