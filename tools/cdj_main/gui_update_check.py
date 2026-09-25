"""The GUI board's update gate: install a C2KGUI.UPD, then roll it back.

    python -m tools.cdj_main.gui_update_check --upd NEW.UPD --out runs/guiupd/check-1

Runs the player's own update path twice, both boards in one guest time
(`boot_vm --cosim`), stock MAIN 4.33 in the update key mode with a USB stick:

  install   the stock GUI 4.20 takes NEW.UPD over the link from MAIN's updater
            and programs its flash;
  rollback  the GUI now running NEW.UPD's body, booted from the flash the
            install left, takes the stock C2KGUI.UPD (or --rollback FILE)
            and programs it back.

After each the simulator's flash is dumped (BFIN_CFI_DUMP) and checked: the
body lands at 0x10000 byte for byte, the 64 KiB in front of it -- the
CDJ-2000's own GUI loader, in no update file -- and the top 16 KiB at 0x1FC000
are untouched, and both boards reported a normal end ("BlackFin update end
(normal)", "*** Update END ! ***").  PASS only if all of that holds both ways.

What this cannot tell: the emulator has no dump of the real sector-0 loader
and does not boot from flash, so "the loader starts the new body" and "a
power cut while programming" stay untested; a stalled transfer leaves the
flash untouched because the GUI takes the whole file into RAM first.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable
UPDATE_KEYS = "00000000000000000000000000000000040000020000"   # RELOOP/EXIT + USB held
BODY = 0x1EC000
HEADER = 32
BODY_AT = 0x10000
TOP = 0x1FC000


def body_of(upd: bytes) -> bytes:
    if len(upd) < HEADER + BODY or upd[:12] != b"CDJ-2000 GUI":
        raise SystemExit("gui_update_check: not a CDJ-2000 GUI update file")
    if upd[0x1F:0x20] != b"1":
        raise SystemExit("gui_update_check: header flag 0x1f is not '1' -- such a "
                         "file would write from 0x0, over the loader; refused")
    return upd[HEADER:HEADER + BODY]


def stick(directory: Path, upd: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(upd, directory / "C2KGUI.UPD")
    image = directory.with_suffix(".img")
    subprocess.run([PY, "-m", "tools.cdj_main.make_sd_image", str(directory), str(image),
                    "--size", "64M"], cwd=ROOT, check=True, capture_output=True)
    return image


def board_with_flash(flash: Path, out: Path) -> Path:
    text = (ROOT / "emulator/cdj2000-gui.hw").read_text()
    text, n = re.subn(r'"firmware/gui-flash-image\.bin"', f'"{flash}"', text)
    if n != 1:
        raise SystemExit("gui_update_check: cannot find the flash line in cdj2000-gui.hw")
    out.write_text(text)
    return out


def run_update(name: str, out: Path, port: int, usb: Path, gui_elf: Path | None,
               board: Path | None, timeout: float) -> tuple[Path, str]:
    """One update run; returns the flash dump and MAIN's console text."""
    temp = ROOT / f"runs/tmp-{port}"
    temp.mkdir(parents=True, exist_ok=True)
    console = temp / f"vm-console-{port}.txt"
    console.unlink(missing_ok=True)
    dump = out / f"{name}-flash-after.bin"
    env = dict(os.environ, CDJ_LINK_PORT=str(port), CDJ_INPUT_PORT=str(port + 4),
               CDJ_DEBUG_CONSOLE="1", CDJ_PANEL_FRAME=UPDATE_KEYS,
               TEMP=str(temp), TMPDIR=str(temp),
               CDJ_QEMU=str(ROOT / "build/qemu/build/qemu-system-sh4"))
    command = [PY, "-m", "tools.cdj_main.boot_vm", "--cosim", "--no-peer",
               "--usb-stick", str(usb), "--seconds", str(int(timeout) + 120),
               "--frames", str(out / f"{name}-frames"), "--frame-every", "10",
               "--stderr", str(out / f"{name}-qemu.err"),
               "--gui-env", f"BFIN_CFI_DUMP={dump}"]
    if gui_elf:
        command += ["--gui-elf", str(gui_elf), "--gui-env", "BFIN_FAST_LZSS="]
    if board:
        command += ["--gui-board", str(board)]
    proc = subprocess.Popen(command, cwd=ROOT, env=env, start_new_session=True,
                            stdout=open(out / f"{name}-boot_vm.out", "w"),
                            stderr=subprocess.STDOUT)
    text = ""
    start = time.time()
    try:
        while time.time() - start < timeout and proc.poll() is None:
            time.sleep(3)
            try:
                text = console.read_bytes().decode("shift_jis", errors="replace")
            except FileNotFoundError:
                continue
            if "Update END" in text:
                time.sleep(8)           # let the GUI finish its screen
                break
        shutil.copyfile(temp / f"vm-frame-{port}.ppm", out / f"{name}-last.ppm") \
            if (temp / f"vm-frame-{port}.ppm").exists() else None
        # Stopping MAIN closes the co-simulation link; the GUI then exits by
        # itself, which is what writes the flash dump.
        qemus = subprocess.run(["pgrep", "-f", f"qemu-system-sh4.*tcp:127.0.0.1:{port},"],
                               capture_output=True, text=True).stdout.split()
        for pid in qemus:
            os.kill(int(pid), signal.SIGTERM)
        for _ in range(60):
            if dump.exists() and dump.stat().st_size:
                break
            time.sleep(1)
        time.sleep(2)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass              # the run's group is already gone
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    (out / f"{name}-console.txt").write_text(text)
    return dump, text


def verify(name: str, dump: Path, before: bytes, body: bytes, console: str) -> list[tuple[str, bool]]:
    checks = [(f"{name}: MAIN's updater ended (*** Update END ! ***)", "Update END" in console),
              (f"{name}: the GUI reported a normal end", "ｱｯﾌﾟﾃﾞｰﾄ終了(正常)" in console)]
    if not dump.exists():
        return checks + [(f"{name}: flash dump written", False)]
    after = dump.read_bytes()
    checks += [
        (f"{name}: body at 0x10000 byte for byte", after[BODY_AT:BODY_AT + BODY] == body),
        (f"{name}: sector 0 (the GUI loader) untouched", after[:BODY_AT] == before[:BODY_AT]),
        (f"{name}: top 16 KiB (0x1FC000) untouched", after[TOP:] == before[TOP:]),
    ]
    return checks


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--upd", type=Path, required=True, help="the C2KGUI.UPD to check")
    parser.add_argument("--rollback", type=Path, default=ROOT / "firmware/C2KGUI.UPD",
                        help="the file that must take the GUI back (stock 4.20)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=6680, help="takes PORT..PORT+5")
    parser.add_argument("--timeout", type=float, default=1200, help="wall seconds per run")
    args = parser.parse_args(argv)

    for port in range(args.port, args.port + 6):
        with socket.socket() as probe:
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                raise SystemExit(f"gui_update_check: port {port} is in use; pick --port")
    if args.out.exists():
        raise SystemExit(f"gui_update_check: {args.out} exists")
    args.out.mkdir(parents=True)
    new_body = body_of(args.upd.read_bytes())
    old_body = body_of(args.rollback.read_bytes())
    stock_flash = (ROOT / "firmware/gui-flash-image.bin").read_bytes()

    print("install: stock GUI 4.20 takes", args.upd.name, flush=True)
    dump_a, console_a = run_update("install", args.out, args.port,
                                   stick(args.out / "stick-install", args.upd),
                                   None, None, args.timeout)
    checks = verify("install", dump_a, stock_flash, new_body, console_a)

    if all(ok for _, ok in checks):
        print("rollback: the installed GUI takes", args.rollback.name, flush=True)
        elf_dir = args.out / "installed-gui"
        subprocess.run([PY, "-m", "tools.cdj_gui.extract", str(args.upd), str(elf_dir)],
                       cwd=ROOT, check=True, capture_output=True)
        flash_a = args.out / "installed-flash.bin"
        shutil.copyfile(dump_a, flash_a)
        board = board_with_flash(flash_a.resolve(), args.out / "installed-board.hw")
        dump_b, console_b = run_update("rollback", args.out, args.port,
                                       stick(args.out / "stick-rollback", args.rollback),
                                       elf_dir / "gui-boot-memory.elf", board, args.timeout)
        checks += verify("rollback", dump_b, dump_a.read_bytes(), old_body, console_b)

    lines = [f"{'ok  ' if ok else 'FAIL'} {what}" for what, ok in checks]
    verdict = "PASS" if checks and all(ok for _, ok in checks) else "NOT SAFE"
    report = "\n".join(lines) + f"\n\n{verdict}: {args.upd}\n"
    (args.out / "REPORT.txt").write_text(report)
    print(report)
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
