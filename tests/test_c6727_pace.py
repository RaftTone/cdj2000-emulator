"""The CDJ-2000 DSP core's pace against MAIN: the hold policy and the McASP clock."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _cc():
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    return cc


def test_c6727_mcasp_clock_keeps_time(tmp_path):
    cc = _cc()
    pkg_config = shutil.which('pkg-config')
    if not pkg_config:
        pytest.skip('requires pkg-config')
    glib = subprocess.check_output(
        [pkg_config, '--cflags', '--libs', 'glib-2.0'], text=True).split()
    # The two QEMU headers the dMAX file needs beyond tests/cstub/qemu.
    stub = tmp_path / 'qemu'
    stub.mkdir()
    (stub / 'log.h').write_text('')
    (stub / 'bswap.h').write_text(
        '#include <string.h>\n'
        'static inline uint32_t ldl_le_p(const void *p)\n'
        '{ const uint8_t *b = p; return b[0] | b[1] << 8 | b[2] << 16'
        ' | (uint32_t)b[3] << 24; }\n')
    binary = tmp_path / 'dmax-clock'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DARRAY_SIZE(a)=(sizeof(a) / sizeof((a)[0]))',
                    '-I', str(tmp_path), '-I', str(ROOT / 'tests/cstub'),
                    '-I', str(ROOT / 'emulator/qemu'),
                    str(ROOT / 'tests/cstub/c6727-dmax-clock.c'),
                    str(ROOT / 'emulator/qemu/cdj_c6727_dmax.c'),
                    '-o', str(binary), *glib], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
