"""Execute the actual patched IMASK case bodies, not a replica of their order.

Requires a locally built simulator. The forwarding stub observes the register
and pending sources; this checks mask publication, not the full SIC/CEC model.
"""
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('family', ['52x', '537', '54x', '561'])
def test_forwarding_observes_new_mask(tmp_path, family):
    source = ROOT / 'build/gdb-17.2/sim/bfin/dv-bfin_sic.c'
    compiler = shutil.which('cc')
    if not source.is_file() or not compiler:
        pytest.skip('requires patched Blackfin simulator source and a C compiler')
    match = re.search(
        rf'case mmr_offset\(bf{family}\.imask\w*\)'
        rf'(?: \.\.\. mmr_offset\(bf{family}\.imask\w*\))?:(?P<body>.*?)'
        rf'\n    case mmr_offset\(bf{family}\.iar', source.read_text(), re.S)
    assert match, f'missing {family} IMASK case'
    # Multi-bank families have additional case labels, including a GNU range.
    body = re.sub(r'^.*case mmr_offset.*\n', '', match['body'], flags=re.M)
    c_source = tmp_path / 'mask.c'
    c_source.write_text('''
#include <assert.h>
#include <stdint.h>
struct sic { uint32_t mask, pending, observed; unsigned forwards; };
static void forward(void *me, struct sic *sic) {
    (void)me;
    sic->observed = sic->mask & sic->pending;
    ++sic->forwards;
}
''' + f'#define bfin_sic_{family}_forward_interrupts forward\n' + '''
static void write_mask(struct sic *sic, uint32_t value) {
    void *me = 0;
    uint32_t *value32p = &sic->mask;
    switch (0) { case 0:
''' + body + '''
    }
}
int main(void) {
    struct sic sic = {0};
    /* Mask a pending, previously enabled RX interrupt: no stale re-forward. */
    sic.mask = 0x800; sic.pending = 0x800;
    write_mask(&sic, 0);
    assert(sic.mask == 0 && sic.observed == 0 && sic.pending == 0x800);
    /* Unmask it: pending source must be visible immediately, without an edge. */
    write_mask(&sic, 0x800);
    assert(sic.mask == 0x800 && sic.observed == 0x800);
    /* Mask RX while TX remains pending: preserve the other source. */
    sic.pending = 0x1800;
    write_mask(&sic, 0x1000);
    assert(sic.observed == 0x1000 && sic.pending == 0x1800);
    /* No pending source must not fabricate an interrupt when unmasking. */
    sic.pending = 0;
    write_mask(&sic, 0xffffffff);
    assert(sic.observed == 0 && sic.forwards == 4);
}
''')
    executable = tmp_path / 'mask-test'
    subprocess.run([compiler, '-std=c99', '-Wall', '-Wextra', '-Werror',
                    str(c_source), '-o', str(executable), '-lm'], check=True)
    subprocess.run([str(executable)], check=True)
