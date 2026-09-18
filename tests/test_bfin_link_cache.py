"""Compile the actual patched SPORT cache function; requires a simulator build."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def cache_harness(tmp_path_factory):
    source = ROOT / 'build/gdb-17.2/sim/bfin/dv-bfin_ppi.c'
    cc = shutil.which('cc')
    if not cc or not source.is_file():
        pytest.skip('build the patched Blackfin simulator and install a C compiler')
    text = source.read_text()
    start = text.index('static int\nbfin_sport_link_fresh_only (void)')
    end = text.index('/* Is any entry of this length', start)
    directory = tmp_path_factory.mktemp('bfin-cache')
    (directory / 'bfin-link-take.inc').write_text(text[start:end])
    output = directory / 'cache-test'
    subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(directory),
                    str(ROOT / 'tests/cstub/bfin-link-cache.c'), '-o', str(output), '-lm'], check=True)
    return output


@pytest.mark.parametrize('fresh_only', [False, True])
def test_fresh_records_are_consumed_once_without_changing_legacy_default(cache_harness, fresh_only):
    env = dict(os.environ)
    env.pop('BFIN_LINK_FRESH_ONLY', None)
    if fresh_only:
        env['BFIN_LINK_FRESH_ONLY'] = '1'
    subprocess.run([str(cache_harness), str(int(fresh_only))], env=env, check=True)
