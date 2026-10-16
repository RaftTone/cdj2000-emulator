"""The M66291GP model on its own: registers, interrupts, EP0 and endpoints.

No QEMU and no firmware: the C stub plays the driver and the host.
"""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_usb_m66291(tmp_path):
    cc = shutil.which("cc")
    pkg_config = shutil.which("pkg-config")
    if not cc or not pkg_config:
        pytest.skip("requires a C compiler and pkg-config (GLib)")
    glib = subprocess.run([pkg_config, "--cflags", "--libs", "glib-2.0"],
                          capture_output=True, text=True)
    if glib.returncode:
        pytest.skip("requires GLib")
    binary = tmp_path / "usb-m66291"
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/cstub"),
                    "-I", str(ROOT / "emulator/qemu"),
                    str(ROOT / "tests/cstub/usb-m66291.c"),
                    str(ROOT / "emulator/qemu/cdj2000_usb_model.c"),
                    "-o", str(binary), *glib.stdout.split()], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
