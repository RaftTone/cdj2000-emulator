"""Check record visibility and descriptor reuse in the built SPORT helper."""
from pathlib import Path
import os
import shutil
import subprocess

import pytest

from tools.cdj_gui.benchmark_capture import capture_source

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('abrupt', [False, True])
def test_records_visible_before_close(tmp_path, abrupt):
    source = ROOT / 'build/gdb-17.2/sim/bfin/dv-bfin_ppi.c'
    cc = shutil.which('cc')
    if not source.exists() or not cc:
        pytest.skip('requires patched simulator source and compiler')
    harness = tmp_path / 'test.c'
    harness.write_text('''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
static unsigned opens, closes;
static FILE *counted_open(const char *p, const char *m) { opens++; return fopen(p,m); }
static int counted_close(FILE *f) { closes++; return fclose(f); }
#define fopen counted_open
#define fclose counted_close
''' + capture_source(source.read_text()) + '''
int main(void) {
    unsigned char bytes[64] = {1,2,3};
    struct stat st;
    const char *path = getenv("BFIN_MAIN_LINK_DUMP");
    for (unsigned i=0; i<3; i++) {
        bfin_sport_link_dump(bytes, sizeof(bytes));
        assert(stat(path, &st) == 0 && st.st_size == (i+1)*72);
    }
    assert(opens == 1 && closes == 0);
    if (getenv("ABRUPT")) _exit(0);
    bfin_sport_link_dump_close();
    assert(closes == 1);
    return 0;
}
''')
    binary = tmp_path / 'test'
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(harness), '-o', str(binary)], check=True)
    output = tmp_path / 'capture'
    env = dict(os.environ, BFIN_MAIN_LINK_DUMP=str(output))
    env.pop('ABRUPT', None)
    if abrupt:
        env['ABRUPT'] = '1'
    subprocess.run([str(binary)], env=env, check=True)
    assert output.read_bytes() == (b'SPRX\x40\x00\x00\x00' + bytes([1, 2, 3]) + bytes(61)) * 3
