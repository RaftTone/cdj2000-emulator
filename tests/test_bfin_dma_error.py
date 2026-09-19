"""Check the Blackfin DMA error latch used by the patched simulator."""

from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_dma_error_clears_run_and_raises_request(tmp_path: Path) -> None:
    source = ROOT / "build/gdb-17.2/sim/bfin/dv-bfin_dma.c"
    compiler = shutil.which("cc")
    if not source.is_file() or not compiler:
        pytest.skip("requires patched Blackfin simulator source and a C compiler")

    match = re.search(
        r"static void\nbfin_dma_latch_error \(struct hw \*me, "
        r"struct bfin_dma \*dma\)\n\{.*?\n\}",
        source.read_text(),
        re.S,
    )
    assert match, "DMA error helper is missing"
    harness = tmp_path / "dma-error.c"
    harness.write_text(
        """
#include <assert.h>
#include <stdint.h>
struct hw { unsigned events; };
struct bfin_dma { uint16_t irq_status; };
#define DMA_RUN 0x0008
#define DMA_ERR 0x0002
static void hw_port_event(struct hw *me, unsigned port, int level)
{
    (void)port;
    me->events += level != 0;
}
"""
        + match.group(0)
        + """
int main(void)
{
    struct hw hw = {0};
    struct bfin_dma dma = {DMA_RUN | 0x0001};
    bfin_dma_latch_error(&hw, &dma);
    assert((dma.irq_status & DMA_RUN) == 0);
    assert((dma.irq_status & DMA_ERR) != 0);
    assert((dma.irq_status & 0x0001) != 0);
    assert(hw.events == 1);
    return 0;
}
"""
    )
    executable = tmp_path / "dma-error"
    subprocess.run(
        [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(executable), '-lm'],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
