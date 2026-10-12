# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj-gui-run: the GUI board GUI-only on the vendored Blackfin core.

The runner is built from emulator/bfin/ into a temporary directory, so the
test does not depend on bin/.  The firmware tests skip without
firmware/nxs/updates/C2KGUI.UPD.  The A/B gate against bin/cdj-run runs only
with CDJ_GUI_RUN_AB=1 (it costs about a minute); a mods GUI image is booted
when CDJ_MODS_GUI_BODY names its update body (and CDJ_MODS_GUI_RANGE=lo:hi
its extension, to prove the mod's code ran).
"""

from __future__ import annotations

import os
import resource
import shutil
import struct
import subprocess
import time
from pathlib import Path

import pytest

from tools.cdj_gui.gui_run import command
from tools.paths import BFIN_SIM, FIRMWARE, REPO_ROOT

UPD = FIRMWARE / "nxs" / "updates" / "C2KGUI.UPD"
CCLK = 400_000_000

needs_firmware = pytest.mark.skipif(not UPD.is_file(), reason="no NXS GUI update")


@pytest.fixture(scope="module")
def runner(tmp_path_factory) -> Path:
    if not shutil.which("make") or not shutil.which(os.environ.get("CC", "cc")):
        pytest.skip("no C toolchain")
    out = tmp_path_factory.mktemp("cdj-gui-run")
    subprocess.run(["make", "-s", "-C", str(REPO_ROOT / "emulator/bfin"), f"O={out}"],
                   check=True, capture_output=True)
    return out / "cdj-gui-run"


def cpu_seconds() -> float:
    """Children's CPU time: unlike wall time, not inflated by a loaded host."""
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def run(runner: Path, boot: Path, out: Path, seconds: float, *extra: str):
    t0 = cpu_seconds()
    result = subprocess.run(command(boot, out, seconds, runner=runner, extra=["-q", *extra]),
                            capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return result, cpu_seconds() - t0


def ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    magic, size, depth, pixels = data.split(b"\n", 3)
    assert magic == b"P6" and depth == b"255"
    w, h = map(int, size.split())
    assert len(pixels) == w * h * 3
    return w, h, pixels


def e8709_pixels(frame: Path) -> int:
    """Red pixels on the error line (rows 195-215 of the 480x255 capture)."""
    w, _, px = ppm(frame)
    return sum(1 for y in range(195, 216) for x in range(w)
               if px[(y * w + x) * 3] >= 180 and px[(y * w + x) * 3 + 1] < 90
               and px[(y * w + x) * 3 + 2] < 90)


def ldr(blocks: list[tuple[int, bytes, int]]) -> bytes:
    return b"".join(struct.pack("<IIH", dest, len(data), flags) + data
                    for dest, data, flags in blocks)


def test_bare_ldr_boot_and_trap_report(runner, tmp_path):
    # JUMP.S 0 at the BF531 boot entry runs out the budget; 0x0001 (a
    # reserved ProgCtrl form) stops loudly with its PC and word.
    spin, trap = tmp_path / "spin.ldr", tmp_path / "trap.ldr"
    spin.write_bytes(ldr([(0xFFA08000, b"\x00\x20", 0x8000)]))
    trap.write_bytes(ldr([(0xFFA08000, b"\x01\x00", 0x8000)]))
    ok, _ = run(runner, spin, tmp_path / "a.ppm", 0.01)
    assert ok.returncode == 0, ok.stderr
    assert "cycles=4000000 " in ok.stderr
    bad, _ = run(runner, trap, tmp_path / "b.ppm", 0.01)
    assert bad.returncode == 1
    assert "unimplemented at 0xffa08000: 0001" in bad.stderr
    junk = tmp_path / "junk.bin"
    junk.write_bytes(b"\x00" * 7)
    assert run(runner, junk, tmp_path / "c.ppm", 0.01)[0].returncode == 2


# bfin-elf-as of:
#   P0.L = 0x0AAA; P0.H = 0x2000; P1.L = 0x0554; P1.H = 0x2000;
#   P2.L = 0xA000; P2.H = 0x201F;
#   R0 = 0xAA (Z); R1 = 0x55 (Z); R2 = 0x80 (Z); R3 = 0x30 (Z);
#   W[P0] = R0; W[P1] = R1; W[P0] = R2; W[P0] = R0; W[P1] = R1; W[P2] = R3;
#   L: JUMP.S L;
# -- the AMD sector erase of 0x1FA000, then a spin.
ERASE_1FA000 = bytes.fromhex(
    "08e1aa0a48e1002009e1540549e100200ae100a04ae11f2080e1aa0081e1550082e18000"
    "83e1300000970997029700970997139700200000")


def test_flash_boot_at_dump_and_top_boot_erase(runner, tmp_path):
    # A 2 MiB flash with the program's LDR at 0x10000 and a pattern in sector
    # 0 boots from 0x10000 with BFIN_FLASH_BOOT_AT; the erase of the 8 KiB
    # sector at 0x1FA000 leaves its 8 KiB neighbour and the 16 KiB journal
    # sector at 0x1FC000 alone (MX29LV160DT top boot), and BFIN_CFI_DUMP
    # writes the flash at exit, also when the run is ended by SIGTERM.
    flash = bytearray(b"\x5a" * 0x200000)
    stream = ldr([(0xFFA08000, ERASE_1FA000, 0x8000)])
    flash[0x10000:0x10000 + len(stream)] = stream
    boot, dump = tmp_path / "flash.bin", tmp_path / "dump.bin"
    boot.write_bytes(bytes(flash))
    env = dict(os.environ, BFIN_FLASH_BOOT_AT="0x10000", BFIN_CFI_DUMP=str(dump))
    proc = subprocess.Popen([str(runner), "-o", str(tmp_path / "s.ppm"), str(boot)], env=env,
                            stdin=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(1)
    proc.terminate()
    _, err = proc.communicate(timeout=30)
    assert proc.returncode == 0, err
    assert "booting the flash's LDR at 0x10000" in err
    assert "flash erase 0x201fa000 +0x2000" in err
    after = dump.read_bytes()
    assert after[0x1FA000:0x1FC000] == b"\xff" * 0x2000
    assert after[:0x1FA000] == bytes(flash[:0x1FA000])
    assert after[0x1FC000:] == bytes(flash[0x1FC000:])


@needs_firmware
def test_stock_gui_reaches_e8709(runner, tmp_path):
    frames = {}
    for seconds in (20, 60):
        result, _ = run(runner, UPD, tmp_path / f"{seconds}.ppm", seconds)
        assert result.returncode == 0, result.stderr
        frames[seconds] = tmp_path / f"{seconds}.ppm"
    w, h, px = ppm(frames[20])
    assert (w, h) == (480, 255)
    assert e8709_pixels(frames[20]) > 800
    assert px == ppm(frames[60])[2]          # settled, and in the same blink phase
    # The flash image bin/cdj-run's board file maps boots the same picture.
    flash = FIRMWARE / "nxs" / "gui-flash-image.bin"
    if flash.is_file():
        result, _ = run(runner, flash, tmp_path / "flash.ppm", 20)
        assert result.returncode == 0, result.stderr
        assert ppm(tmp_path / "flash.ppm")[2] == px


def gdb_run(out: Path, ticks: int, time_base: str) -> float:
    env = {k: v for k, v in os.environ.items() if not k.startswith("BFIN_")}
    env.update(BFIN_TIME_BASE=time_base, BFIN_EXIT_AFTER_TICKS=str(ticks),
               BFIN_GUI_OUTPUT=str(out), BFIN_GUI_COLOR="rgb555le",
               BFIN_PARALLEL_WRITEBACK="1", BFIN_GPIO5_READY_TOGGLE="1")
    t0 = cpu_seconds()
    subprocess.run([str(BFIN_SIM), "--model", "bf531", "--environment", "operating",
                    "--memory-region", "0,64M", "--hw-board-file",
                    "emulator/cdj2000-gui-nxs.hw",
                    str(FIRMWARE / "nxs" / "gui-boot-memory.elf")],
                   cwd=REPO_ROOT, env=env, check=True, capture_output=True,
                   stdin=subprocess.DEVNULL)
    return cpu_seconds() - t0


@needs_firmware
@pytest.mark.skipif(os.environ.get("CDJ_GUI_RUN_AB") != "1",
                    reason="set CDJ_GUI_RUN_AB=1 for the bin/cdj-run A/B (about a minute)")
def test_ab_against_gdb(runner, tmp_path, record_property):
    if not BFIN_SIM.is_file():
        pytest.skip("no bin/cdj-run")
    # Picture: gdb on its virtual time base (patch 14) at 20 s.  The cursor
    # in the time display blinks (rect 142,150-179,155, about 0.6 s period)
    # and the two boot paths (ELF vs boot stream, different cycle models)
    # put it in phase about 0.2 s apart, so gdb's frame must equal one of
    # ours sampled through a blink period -- exactly, every pixel.
    gdb_frame = tmp_path / "gdb.ppm"
    gdb_virtual = gdb_run(gdb_frame, 20 * CCLK, "virtual")
    ours = []
    for tenth in range(7):
        frame = tmp_path / f"ours-{tenth}.ppm"
        result, wall = run(runner, UPD, frame, 20 + tenth / 10)
        assert result.returncode == 0, result.stderr
        ours.append((ppm(frame)[2], wall))
    gdb_px = ppm(gdb_frame)[2]
    assert any(px == gdb_px for px, _ in ours)
    fast = ours[0][1]
    # Speed at equal virtual time, as CPU seconds of each process.  insn (the evaluation's baseline, one
    # display event per tick) over 1 s of boot; virtual over the 20 s run.
    gdb_insn = gdb_run(tmp_path / "gdb-insn.ppm", CCLK, "insn")
    fast_1s = run(runner, UPD, tmp_path / "ours-1s.ppm", 1)[1]
    record_property("speedup_vs_insn_1s", gdb_insn / fast_1s)
    record_property("speedup_vs_virtual_20s", gdb_virtual / fast)
    print(f"\n20 s virtual: cdj-gui-run {fast:.2f} s CPU, cdj-run virtual {gdb_virtual:.2f} s "
          f"({gdb_virtual / fast:.1f}x); 1 s virtual: cdj-gui-run {fast_1s:.2f} s, "
          f"cdj-run insn {gdb_insn:.1f} s ({gdb_insn / fast_1s:.0f}x)")
    assert gdb_insn / fast_1s >= 20
    assert gdb_virtual / fast >= 5


@needs_firmware
@pytest.mark.skipif(not os.environ.get("CDJ_MODS_GUI_BODY"),
                    reason="set CDJ_MODS_GUI_BODY to a mods GUI update body")
def test_mods_gui_reaches_its_unconnected_screen(runner, tmp_path):
    span = os.environ.get("CDJ_MODS_GUI_RANGE")
    extra = ["-x", span] if span else []
    result, _ = run(runner, Path(os.environ["CDJ_MODS_GUI_BODY"]), tmp_path / "m.ppm",
                    60, *extra)
    assert result.returncode == 0, result.stderr
    assert "unimplemented" not in result.stderr
    assert e8709_pixels(tmp_path / "m.ppm") > 800
    if span:
        assert "lines_run=0\n" not in result.stderr, result.stderr
