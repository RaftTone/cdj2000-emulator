"""Exercise real compilation, dependency invalidation, and corrupt cache handling."""
from pathlib import Path
import shutil
import subprocess

import pytest

from tools.cdj_dsp.build_cache import build_native


def test_build_cache_tracks_headers_profiles_and_corruption(tmp_path, monkeypatch):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    include = tmp_path / 'external include'
    include.mkdir()
    header = include / 'value.h'
    header.write_text('#define VALUE 0\n')
    monkeypatch.setenv('CPATH', str(include))
    cache = tmp_path / 'cache'

    def build(name, profile='-O2'):
        directory = tmp_path / name
        directory.mkdir()
        source = directory / 'main.c'
        source.write_text('#include <value.h>\nint main(void){return VALUE;}\n')
        return build_native(cc, directory, [source], cache=cache, optimization=profile)

    first, a = build('first')
    second, b = build('second')
    assert not a['cache_hit'] and b['cache_hit']
    assert a['cache_key'] == b['cache_key']
    assert first.read_bytes() == second.read_bytes()
    assert subprocess.run([str(second)]).returncode == 0
    # Same filename, changed external header content must invalidate the key.
    header.write_text('#define VALUE 7\n')
    third, c = build('third')
    assert not c['cache_hit'] and c['cache_key'] != a['cache_key']
    assert subprocess.run([str(third)]).returncode == 7
    _, debug = build('debug', '-O0')
    assert not debug['cache_hit'] and debug['cache_key'] != c['cache_key']
    (cache / c['cache_key'] / 'replay').write_bytes(b'corrupt')
    repaired, d = build('corrupt')
    assert not d['cache_hit']
    assert subprocess.run([str(repaired)]).returncode == 7


def test_failed_compile_never_publishes_cache_entry(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    source = tmp_path / 'bad.c'
    source.write_text('this is not C;\n')
    cache = tmp_path / 'cache'
    with pytest.raises(subprocess.CalledProcessError):
        build_native(cc, tmp_path, [source], cache=cache)
    assert not cache.exists()
