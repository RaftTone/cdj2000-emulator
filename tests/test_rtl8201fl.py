"""Standalone virtual PHY and bit-banged Clause22 tests."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_rtl8201fl(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("requires C compiler")
    binary = tmp_path / "rtl8201fl"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "emulator/qemu"),
                    str(ROOT / "tests/cstub/rtl8201fl.c"),
                    str(ROOT / "emulator/qemu/cdj_rtl8201fl.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
