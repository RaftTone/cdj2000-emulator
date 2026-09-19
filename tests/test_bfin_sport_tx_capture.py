"""Check persistent Blackfin SPORT transmit capture semantics."""
from pathlib import Path
import os
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("abrupt", [False, True])
def test_tx_records_are_visible_before_close(tmp_path, abrupt):
    source = ROOT / "build/gdb-17.2/sim/bfin/dv-bfin_ppi.c"
    cc = shutil.which("cc")
    if not source.exists() or not cc:
        pytest.skip("requires patched simulator source and compiler")
    text = source.read_text()
    begin = text.index("static unsigned\nbfin_sport_tx_dump_stream_close")
    end = text.index("\nstatic unsigned\nbfin_sport_dma_write_buffer", begin)
    helper = text[begin:end]
    harness = tmp_path / "test.c"
    harness.write_text(
        """
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
typedef uint32_t bu32;
static unsigned opens, closes;
static FILE *counted_open(const char *p, const char *m) { opens++; return fopen(p, m); }
static int counted_close(FILE *f) { closes++; return fclose(f); }
#define fopen counted_open
#define fclose counted_close
"""
        + helper
        + """
int main(void) {
    unsigned char bytes[64] = {1, 2, 3};
    struct stat st;
    const char *path = getenv("BFIN_SPORT_TX_OUTPUT");
    for (unsigned i = 0; i < 3; i++) {
        bfin_sport_tx_dump(bfin_sport_tx_dump_path(), 0x12345678, bytes, sizeof(bytes));
        assert(stat(path, &st) == 0 && st.st_size == (off_t)((i + 1) * 76));
    }
    assert(opens == 1 && closes == 0);
    if (getenv("ABRUPT")) _exit(0);
    bfin_sport_tx_dump_close();
    assert(closes == 1);
    return 0;
}
"""
    )
    binary = tmp_path / "test"
    subprocess.run(
        [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(binary), '-lm'],
        check=True,
    )
    output = tmp_path / "capture"
    env = dict(os.environ, BFIN_SPORT_TX_OUTPUT=str(output))
    if abrupt:
        env["ABRUPT"] = "1"
    subprocess.run([str(binary)], env=env, check=True)
    expected = b"SPTX\x78\x56\x34\x12\x40\x00\x00\x00" + bytes([1, 2, 3]) + bytes(61)
    assert output.read_bytes() == expected * 3
