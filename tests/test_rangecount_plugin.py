"""The rangecount TCG plugin on a hand-assembled SH-4 loop; no firmware.

Needs a C compiler, glib's pkg-config entry, the QEMU source tree the board was
built from (CDJ_QEMU_SRC, default build/qemu) and qemu-system-sh4 with plugin
support (CDJ_QEMU, see tools/paths.py).  Skips when any of them is missing.
"""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from tools import paths

ROOT = Path(__file__).resolve().parents[1]
QEMU_SRC = Path(os.environ.get("CDJ_QEMU_SRC", ROOT / "build/qemu"))

# At 0xa0000000, the SH-4 reset address:
#   0: mov  #5,r1
#   2: dt   r1        <- the mark
#   4: bf   2
#   6: sleep          (SR.BL is set at reset, so nothing wakes it)
LOOP = bytes.fromhex("05e1 1041 fd8b 1b00".replace(" ", ""))


def build_plugin(tmp_path):
    cc = shutil.which("cc")
    pkg_config = shutil.which("pkg-config")
    header = QEMU_SRC / "include/plugins/qemu-plugin.h"
    if not cc or not pkg_config:
        pytest.skip("requires a C compiler and pkg-config")
    if not header.exists():
        pytest.skip(f"{header} is not there; set CDJ_QEMU_SRC")
    glib = subprocess.run([pkg_config, "--cflags", "glib-2.0"],
                          capture_output=True, text=True)
    if glib.returncode:
        pytest.skip("requires glib-2.0 development files")
    plugin = tmp_path / "rangecount.so"
    subprocess.run([
        cc, "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-Werror", "-shared", "-fPIC", "-I", str(header.parent),
        *glib.stdout.split(),
        str(ROOT / "emulator/qemu-plugins/rangecount.c"),
        "-o", str(plugin),
    ], check=True)
    return plugin


def run_qemu(tmp_path, plugin_args):
    qemu = shutil.which(str(paths.QEMU))
    if not qemu:
        pytest.skip(f"{paths.QEMU} is not there; set CDJ_QEMU")
    (tmp_path / "loop.bin").write_bytes(LOOP)
    process = subprocess.Popen(
        [qemu, "-M", "cdj2000-main", "-bios", "loop.bin", "-display", "none",
         "-monitor", "none", "-serial", "null", "-serial", "null",
         "-serial", "null", "-plugin", plugin_args],
        cwd=tmp_path, env=paths.qemu_environment(),
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        process.terminate()     # QEMU exits cleanly, so the atexit report runs
    _, stderr = process.communicate(timeout=10)
    if "plugin" in stderr and "not supported" in stderr:
        pytest.skip("this qemu-system-sh4 has no plugin support")
    return stderr


def test_marks_and_total(tmp_path):
    plugin = build_plugin(tmp_path)
    stderr = run_qemu(tmp_path, f"{plugin},lo=0,hi=0x10,mark=0xa0000002,"
                                "prof=prof.txt")
    lines = [line.split() for line in stderr.splitlines()
             if line.startswith("rangecount ")]
    marks = [int(count) for _, where, count in lines if where == "0x2"]
    # The mark sees the count with its own instruction included: mov, dt
    # gives 2, and each further pass adds dt and bf.
    assert marks == [2, 4, 6, 8, 10]
    # mov, five dt and bf pairs, sleep.
    assert ["rangecount", "total", "12"] in lines
    # The profile charges whole translation blocks on entry.  QEMU translates
    # the word after sleep into the same block, so it may count one more.
    address, count = (tmp_path / "prof.txt").read_text().split()
    assert address == "0" and int(count) >= 12


@pytest.mark.parametrize("arguments, message", [
    ("lo=0", "both are required"),
    ("lo=0x10,hi=0x10", "hi must be above lo"),
    ("lo=0,hi=0x10,colour=red", "unknown argument"),
    ("lo=0,hi=zero", "not a number"),
])
def test_bad_arguments_stop_qemu(tmp_path, arguments, message):
    plugin = build_plugin(tmp_path)
    stderr = run_qemu(tmp_path, f"{plugin},{arguments}")
    assert message in stderr
