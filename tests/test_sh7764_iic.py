"""Standalone SH7764 IIC empty-bus tests; no firmware or identity fixtures."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_sh7764_iic_empty_bus(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("requires C compiler")
    binary = tmp_path / "sh7764-iic"
    subprocess.run([
        cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "emulator/qemu"),
        str(ROOT / "tests/cstub/sh7764-iic.c"),
        str(ROOT / "emulator/qemu/cdj_sh7764_iic.c"),
        "-o", str(binary), '-lm',
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
