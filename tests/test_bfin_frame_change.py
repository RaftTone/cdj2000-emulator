"""Exercise the built file-display backend with real scanlines, without firmware."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_frame_change_publication(tmp_path):
    source = ROOT / 'build/gdb-17.2/sim/bfin'
    if not (source / 'gui.c').exists():
        pytest.skip('requires the patched Blackfin source tree')
    compiler = shutil.which('cc')
    if not compiler:
        pytest.skip('requires C compiler')
    for name in ('gui.c', 'gui.h'):
        shutil.copyfile(source / name, tmp_path / name)
    (tmp_path / 'defs.h').write_text('''
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define XNEW(t) ((t *) malloc(sizeof(t)))
#define XNEWVEC(t,n) ((t *) malloc(sizeof(t) * (n)))
#define XCNEWVEC(t,n) ((t *) calloc((n), sizeof(t)))
#define xstrdup strdup
''')
    (tmp_path / 'libiberty.h').write_text('')
    (tmp_path / 'test.c').write_text(r'''
#include <assert.h>
#include <string.h>
static unsigned full_comparisons;
static int counted_compare(const void *a, const void *b, size_t n)
{
    if (n == 12) ++full_comparisons;
    return memcmp(a, b, n);
}
#define memcmp counted_compare
#include "gui.c"
unsigned long bfin_stat_frames, bfin_stat_frames_published;
double bfin_sim_seconds(void) { return 0; }
static void frame(void *gui, unsigned char pixel)
{
    unsigned char line[4] = {pixel, 0, 0, 0};
    assert(bfin_gui_update(gui, line, 4) == 4);
    assert(bfin_gui_update(gui, line, 4) == 4);
}
int main(void)
{
    struct gui_state *gui = bfin_gui_setup(NULL, 1, 2, 2, GUI_COLOR_RGB_555_LE);
    assert(gui);
    frame(gui, 0); /* Publish the initial all-black frame. */
    assert(bfin_stat_frames_published == 1);
    frame(gui, 0);
    assert(bfin_stat_frames_published == 1 && full_comparisons == 0);
    frame(gui, 31);
    assert(bfin_stat_frames_published == 2 && full_comparisons == 1);
    frame(gui, 31);
    assert(bfin_stat_frames_published == 2 && full_comparisons == 1);
    frame(gui, 0); /* Returning to black must publish too. */
    assert(bfin_stat_frames_published == 3 && full_comparisons == 2);
    assert(bfin_stat_frames == 5);
    bfin_gui_setup(gui, 0, 2, 2, GUI_COLOR_RGB_555_LE);
    return 0;
}
''')
    executable = tmp_path / 'frame-test'
    subprocess.run([compiler, '-std=gnu11', '-O2', '-I', str(tmp_path),
                    str(tmp_path / 'test.c'), '-o', str(executable), '-lm'], check=True)
    # Keep diagnostic settings from affecting the standalone fixture.
    import os
    env = {k: v for k, v in os.environ.items() if not k.startswith('BFIN_')}
    env['BFIN_GUI_OUTPUT'] = str(tmp_path / 'screen.ppm')
    subprocess.run([str(executable)], cwd=tmp_path, env=env, check=True, timeout=5)
    assert (tmp_path / 'screen.ppm').read_bytes().endswith(bytes(12))
