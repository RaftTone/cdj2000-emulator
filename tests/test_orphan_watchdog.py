"""SIGKILL of a launcher must take every emulator process with it.

Boots the real NXS pair briefly under nxs_vm, kills the launcher with SIGKILL
(no cleanup code runs), and checks that qemu-system-sh4 and cdj-run are gone and
that nothing keeps writing DSP checkpoints.  The second case puts a wrapper in
between, as tools/emulator_ui_session.py in an external session wrapper does, and kills the
wrapper.  Needs the built qemu, bin/cdj-run and firmware/nxs; skipped without.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
QEMU = Path(os.environ.get("CDJ_QEMU") or ROOT / "build/qemu/build/qemu-system-sh4")

pytestmark = pytest.mark.skipif(
    sys.platform == "win32" or not QEMU.is_file()
    or not (ROOT / "bin/cdj-run").is_file()
    or not (ROOT / "firmware/nxs/main-firmware.bin").is_file(),
    reason="needs POSIX, the built qemu, bin/cdj-run and firmware/nxs")

NXS_VM = [sys.executable, "-m", "tools.cdj_main.nxs_vm"]
WRAPPER = ("import subprocess, sys; "
           "subprocess.run([sys.executable, '-m', 'tools.cdj_main.nxs_vm'] + sys.argv[1:])")


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def checkpoints(run):
    return len(list((run / "dsp-checkpoints").glob("*.cdjdsp")))


@pytest.mark.parametrize("wrapped", [False, True], ids=["launcher", "launcher-under-wrapper"])
def test_sigkill_launcher_leaves_no_orphans(tmp_path, wrapped):
    run = tmp_path / "run"
    args = [str(run), "--seconds", "120", "--qemu", str(QEMU), "--port", "29480"]
    command = [sys.executable, "-c", WRAPPER, *args] if wrapped else [*NXS_VM, *args]
    top = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, start_new_session=True)
    pids = []
    try:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                session = json.loads((run / "session.json").read_text())
            except (OSError, ValueError):
                session = {}
            pids = list(session.get("processes", {}).values())
            if len(pids) >= 2 and checkpoints(run) >= 2:
                break
            time.sleep(0.5)
        else:
            pytest.fail("emulator did not start writing checkpoints")
        launcher = session["launcher_pid"]
        assert all(alive(pid) for pid in pids)

        os.kill(top.pid, signal.SIGKILL)  # the wrapper, or the launcher itself
        top.wait()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and any(alive(p) for p in [*pids, launcher]):
            time.sleep(0.2)
        assert not [p for p in [*pids, launcher] if alive(p)], "orphans survived"

        count = checkpoints(run)
        time.sleep(3)
        assert checkpoints(run) == count, "checkpoints still being written"
    finally:
        for pid in [*pids, top.pid]:
            if alive(pid):
                os.kill(pid, signal.SIGKILL)
