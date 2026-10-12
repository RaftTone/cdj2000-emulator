# SPDX-License-Identifier: GPL-2.0-or-later
"""The fast Blackfin core (emulator/bfin) on the instructions where it once
disagreed with GNU sim (bin/cdj-run with BFIN_PARALLEL_WRITEBACK=1).

tests/bfin/gnu-parity-cases.txt has one case per class of divergence that
tools/cdj_gui/bfin_diff.py found over the stock and mods GUI ELFs before the
DSP32 groups, ALU2op and CCflag became GNU sim's code and bundles its order:
flags (vector add AC0/AC1, LSHIFT V, compares, MAC V_COPY), the
accumulators, R = [P ++ P] with one register, slot writes. Each line is
  ENCODING MODE SEED REGION RETS OUT MEM
-- the harness's starting state (bfin_diff.state) and GNU sim's result as
index=value changes from it (MEM * where the region was not kept). No
assembler or GNU sim needed.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess

import pytest

from tools.cdj_gui.bfin_diff import state, words
from tools.paths import REPO_ROOT

BFIN = REPO_ROOT / "emulator" / "bfin"
SOURCES = ["bf531.c", "bfin_core.c", "bfin_exec.c", "bfin_dsp.c"]
CASES = REPO_ROOT / "tests" / "bfin" / "gnu-parity-cases.txt"


def records():
    out = []
    for line in CASES.read_text().split("\n"):
        if not line.strip():
            continue
        enc, mode, seed, region, rets, regs, mem = line.split()
        region = int(region, 16)
        s, m = state(enc, mode, int(seed), region)
        s[37] = int(rets, 16)
        o, mo = list(s), bytearray(m)
        for field, target in ((regs, o), (mem, mo)):
            if field not in "-*":
                for item in field.split(","):
                    k, v = item.split("=")
                    target[int(k)] = int(v, 16)
        code = (words(enc) + [0, 0, 0])[:4]
        out.append(struct.pack("<4H40I256s40I256sII", *code, *s, m, *o, bytes(mo), region,
                               mem != "*"))
    return out


def test_fast_core_matches_gnu_sim(tmp_path):
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path / "bfin-gnu-parity"
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", str(BFIN),
                    str(REPO_ROOT / "tests/cstub/bfin-gnu-parity.c"),
                    *(str(BFIN / s) for s in SOURCES), "-o", str(exe)], check=True)
    data = tmp_path / "cases.bin"
    recs = records()
    data.write_bytes(b"".join(recs))
    result = subprocess.run([str(exe), str(data)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr
    assert f"{len(recs)} cases, 0 mismatches" in result.stdout
