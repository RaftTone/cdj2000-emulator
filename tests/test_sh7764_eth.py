"""Standalone functional EtherC/E-DMAC regression tests; no firmware fixture."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]

def test_sh7764_eth(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("requires C compiler")
    binary = tmp_path / "sh7764-eth"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "emulator/qemu"),
                    str(ROOT / "tests/cstub/sh7764-eth.c"),
                    str(ROOT / "emulator/qemu/cdj_sh7764_eth.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
