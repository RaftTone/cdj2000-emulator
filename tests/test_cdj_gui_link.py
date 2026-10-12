# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj-gui-run's MAIN link and SPORT1 DMA against a fake MAIN on loopback.

tests/cstub/cdj-gui-link.c has the cases: framing and resync, per-length
slots, fresh-only (patch 05), no canned record after MAIN spoke (13), zero-200
housekeeping, native partial bursts (32/33), captures (10/11), the
co-simulation wire (14), and the receive DMA's partial/retry/IRQ behaviour.
"""

from __future__ import annotations

import os
import shutil
import subprocess

import pytest

from tools.paths import REPO_ROOT

BFIN = REPO_ROOT / "emulator" / "bfin"
SOURCES = ["cdj_link.c", "bf531.c", "bfin_core.c", "bfin_exec.c", "bfin_dsp.c"]


def test_link_and_sport_dma(tmp_path):
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path / "cdj-gui-link"
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", str(BFIN),
                    str(REPO_ROOT / "tests/cstub/cdj-gui-link.c"),
                    *(str(BFIN / s) for s in SOURCES), "-o", str(exe)], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("BFIN_")}
    result = subprocess.run([str(exe), str(tmp_path)], env=env, capture_output=True,
                            text=True, timeout=60, stdin=subprocess.DEVNULL)
    assert result.returncode == 0 and result.stdout.strip() == "ok", result.stderr
