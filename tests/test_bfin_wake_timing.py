"""Deterministic checks of extracted GNU sim wake code, not hardware timing.

Only host time/select and the device-event backend are test doubles. The tick
boundary rules and parked predicate are extracted too, avoiding a duplicate
scheduler implementation that could bless an off-by-one error.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/gdb-17.2/sim'
CASES = ['deadline', 'interrupt', 'pending', 'same_pc', 'prepaid',
         'unpaid_lag', 'link', 'spin', 'hardware_loop']
MUTATIONS = {
    'ignore_pc_change': ('interrupt', 'interrupt stops catch-up'),
    'discard_prepaid': ('prepaid', 'prepaid protects deliberate wait'),
    'ignore_link': ('link', 'link wakes host wait'),
    'ignore_hardware_loop': ('hardware_loop', 'LC0 excludes self-jump parking'),
    'ignore_same_pc_event': ('same_pc', 'same-PC event returns before distant wait'),
    'early_tick_boundary': ('deadline', 'deadline boundary not yet processed'),
}


def _function(text, name, return_type):
    match = re.search(r'\n' + re.escape(name) + r' \([^;]*?\)\n\{', text)
    assert match, f'missing production function {name}'
    brace = text.index('{', match.start())
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == '{':
            depth += 1
        elif text[pos] == '}':
            depth -= 1
            if not depth:
                return f'static {return_type}\n' + text[match.start()+1:pos+1] + '\n'
    raise AssertionError(f'unterminated production function {name}')


def _replace_once(text, old, new):
    assert text.count(old) == 1, f'production mutation anchor changed: {old}'
    return text.replace(old, new, 1)


@pytest.fixture(scope='module')
def wake_sources():
    compiler = shutil.which('cc')
    if os.name == 'nt':
        pytest.skip('POSIX select fixture; Windows waiter is not covered')
    interp = SOURCE / 'bfin/interp.c'
    common = SOURCE / 'common/sim-events.c'
    if not compiler or not interp.is_file() or not common.is_file():
        pytest.skip('requires prepared Blackfin simulator source and a C compiler')
    text, events = interp.read_text(), common.read_text()
    state = re.search(r'struct bfin_wall\n\{.*?int bfin_idle_wake_pending;', text, re.S)
    parked = re.search(r'int parked = (.*?);', text, re.S)
    assert state and parked, 'wake state/predicate missing from prepared source'
    production = state[0] + '\n'
    for name, ret in [('sim_events_time', 'int64_t'), ('sim_events_tick', 'int'),
                      ('sim_events_tickn', 'int')]:
        production += _function(events, name, ret)
    for name in ['bfin_wall_wait', 'bfin_wall_deliver', 'bfin_wall_sync']:
        production += _function(text, name, 'void')
    production += ('static int classify_parked(SIM_CPU *cpu, bu32 oldpc) { return '
                   + parked[1] + '; }\n')
    return compiler, production


def _run(tmp_path, wake_sources, case, mutation=None, sanitize=False):
    compiler, production = wake_sources
    if mutation == 'ignore_pc_change':
        production = _replace_once(production, 'if (PCREG != pc_before)\n\tbreak;',
                                   'if (PCREG != pc_before && 0)\n\tbreak;')
    elif mutation == 'discard_prepaid':
        production = _replace_once(production, 'bfin_wall.lag_cap + bfin_wall.prepaid',
                                   'bfin_wall.lag_cap + 0 * bfin_wall.prepaid')
    elif mutation == 'ignore_link':
        production = _replace_once(production, 'FD_SET (fd, &rd);', 'FD_CLR (fd, &rd);')
    elif mutation == 'ignore_hardware_loop':
        production = _replace_once(production, 'LCREG (0) == 0', '1')
    elif mutation == 'ignore_same_pc_event':
        production = _replace_once(production, 'if (parked && bfin_stats.events != events_before)',
                                   'if (0 && parked && bfin_stats.events != events_before)')
    elif mutation == 'early_tick_boundary':
        production = _replace_once(production, 'events->time_from_event < n)',
                                   'events->time_from_event <= n)')
    (tmp_path / 'wake-production.inc').write_text(production)
    binary = tmp_path / 'wake-test'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else []
    subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    *flags, '-I', str(tmp_path), str(ROOT / 'tests/cstub/bfin-wake-timing.c'),
                    '-o', str(binary)], check=True, capture_output=True, timeout=30)
    return subprocess.run([str(binary), case], text=True, capture_output=True, timeout=5)


@pytest.mark.parametrize('case', CASES)
def test_wake_contract(tmp_path, wake_sources, case):
    result = _run(tmp_path, wake_sources, case)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == f'PASS {case}'


@pytest.mark.parametrize('mutation', MUTATIONS)
def test_wake_negative_control(tmp_path, wake_sources, mutation):
    case, failure = MUTATIONS[mutation]
    result = _run(tmp_path, wake_sources, case, mutation)
    assert result.returncode == 1 and f'FAIL: {failure}\n' in result.stderr, result.stdout + result.stderr
