# SPDX-License-Identifier: GPL-2.0-or-later
"""A WAV track with analysis files keeps streaming on the DSP model, as it does on the real core.

Needs the things the repository does not ship (see FIRMWARE.md), so it skips without them:

    firmware/       your own C2KMAIN.UPD / C2KGUI.UPD, unpacked
    bin/cdj-run     the Blackfin GUI simulator (scripts/build-bfin-sim.sh)
    CDJ_QEMU        a qemu-system-sh4 built from this tree
    CDJ_TEST_CARD   a card.img whose first playlist track is a 16 bit 44.1 kHz WAV longer than two
                    minutes, with its ANLZ files (tools.cdj_main.rb_card)

and optionally CDJ_TEST_CARD_ARGS, the cosim_scenario arguments that reach the playlist.

Stock MAIN 4.33 keeps a ring of 25 to 30 s in the DSP and reads on in bursts of about 940 kB (100
blocks) as the ring empties; the model used to book 40 level units per block instead of 4 and to
play nothing out of the ring, so MAIN read 60 blocks and then nothing at all.  The test counts the
card's read bursts (CDJ_READ_LOG=1) while the deck plays.  About 6 minutes of wall clock.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
CARD = os.environ.get("CDJ_TEST_CARD")
QEMU = os.environ.get("CDJ_QEMU")

pytestmark = pytest.mark.skipif(
    not (CARD and QEMU and Path(CARD).is_file() and Path(QEMU).is_file()
         and (ROOT / "firmware/main-firmware.bin").is_file()
         and (ROOT / "bin/cdj-run").is_file()),
    reason="needs firmware/, bin/cdj-run, CDJ_QEMU and CDJ_TEST_CARD (see the module docstring)")

PLAY_SECONDS = 60
BLOCK = 9408
READ = re.compile(r"cdj2000-sd: reads t=(\d+\.\d+)\.\.(\d+\.\d+) \([\d.]+ s\): (\d+) bytes")


def free_port():
    for port in range(7900, 8400, 10):
        socks = []
        try:
            for offset in range(6):
                sock = socket.socket()
                sock.bind(("127.0.0.1", port + offset))
                socks.append(sock)
            return port
        except OSError:
            continue
        finally:
            for sock in socks:
                sock.close()
    pytest.skip("no free port range")


def test_wav_with_analysis_files_keeps_streaming(tmp_path):
    card = tmp_path / "card.img"        # the machine writes to its card
    shutil.copyfile(CARD, card)
    command = [sys.executable, "-m", "tools.cdj_main.cosim_scenario", "--card", str(card),
               "--out", str(tmp_path / "run"), "--port", str(free_port()),
               *shlex.split(os.environ.get("CDJ_TEST_CARD_ARGS", "")),
               "--no-play", "--play-seconds", str(PLAY_SECONDS),
               "--env", "CDJ_AUTO_CUE=0", "--env", "CDJ_READ_LOG=1"]
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=3000)
    assert done.returncode == 0, done.stdout[-2000:] + done.stderr[-2000:]
    log = (tmp_path / "run/qemu.err").read_text(errors="replace").splitlines()
    bursts = [(float(m.group(1)), int(m.group(3))) for line in log if (m := READ.match(line))]
    # The start fill is one or two bursts of about 560 blocks, the refills bursts of 100 blocks.
    fills = [t for t, n in bursts if n >= 200 * BLOCK]
    assert fills, f"no start fill: {bursts}"
    refills = [t for t, n in bursts if 90 * BLOCK <= n < 200 * BLOCK and t > fills[-1] + 3]
    # Every 5.3 s of play, about ten in 60 s; the old accounting had none.
    assert len(refills) >= 5, f"the stream stopped after the start fill: {bursts}"
    for t1, t2 in zip(refills, refills[1:]):
        assert t2 - t1 < 8, f"a gap of {t2 - t1:.1f} s between refills at t={t1:.1f}"
