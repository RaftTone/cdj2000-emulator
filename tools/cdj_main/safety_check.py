"""Will this MAIN image leave a player that can still be recovered?

    python -m tools.cdj_main.safety_check MOD-FLASH.bin [--upd MOD.UPD] [--quick]

Run it on every image before it goes near a real CDJ-2000.  A flag-'0' update
only rewrites the application (0x40000 up); the boot ROM and the loader at
0x10000 stay, and the boot ROM falls back to that loader when the application's
checksum fails -- that is the player's recovery path.  The case it does not
cover is the dangerous one: an application with a *valid* checksum that dies
before its own updater runs.  The boot ROM then never offers the loader.  So a
mod has to prove, in the emulator, that it can still flash stock firmware over
itself.

Checks, in order (the first failure stops the run):

1. static   the boot ROM and loader region (0..0x40000) is byte-identical to
            stock; the application unpacks with a valid checksum and ends
            before 0x3e0000; with --upd, the file's flag is '0', its CRC holds,
            its version is above 4.33 and it carries exactly this image.
2. boot     both boards boot the mod: MAIN's RTOS keeps ticking to the end of
            a 75 s run, the GUI handshake completes and a frame is drawn.
            New caution codes against stock are reported.
3. rollback the mod, booted with RELOOP+USB held and a stick holding stock 4.33
            packed as version 9.99, runs its own updater to
            "*** Update END ! ***"; the flash afterwards holds the stock
            application.
4. recovery the mod with one 64 KiB sector of its application blanked (a
            power cut mid-update) falls back to the loader, which flashes the
            same stick back to stock.

--quick runs 1 and 2 only.  The whole set takes about three minutes; a dead
updater is reported after the rollback limit, about nine.
Artifacts (console logs, frames, flash dumps) land in runs/safety/<name>-<time>/.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

from tools.cdj_gui.main_unpack import decode_srecords, unpack_region
from tools.cdj_main import make_upd
from tools.paths import FIRMWARE, QEMU, qemu_environment

REPO = Path(__file__).resolve().parents[2]
APP = 0x40000
APP_END = 0x3E0000
FLASH_SIZE = 0x400000
PORT = int(os.environ.get("CDJ_LINK_PORT", "5980"))
# RELOOP/EXIT (payload byte 16 bit 2) and USB (byte 19 bit 1) held at power-on:
# the updater's boot mode, as on the real player.
UPDATE_KEYS = "00000000000000000000000000000000040000020000"
RETURN_VERSION = "9.99"
UPDATE_DONE = b"*** Update END ! ***"
LOADER_BANNER = b"Cente USBH-MSC"
STOCK_CAUTION = {"29"}
# A real update takes ~40 s of wall clock here (the app updater) or ~20 s (the
# loader); the limits leave room for a slow host, not for a dead updater.
ROLLBACK_LIMIT_S = 420
RECOVERY_LIMIT_S = 300


class Failed(Exception):
    pass


def static_checks(mod: bytes, stock: bytes, upd: bytes | None) -> list[str]:
    notes = []
    if len(mod) > FLASH_SIZE:
        raise Failed(f"image is 0x{len(mod):x} bytes, larger than the 4 MiB flash")
    if mod[:APP] != stock[:APP]:
        first = next(i for i in range(APP) if mod[i] != stock[i])
        raise Failed(f"boot ROM / loader region differs from stock at 0x{first:x}: "
                     "that is the recovery path, and nothing may change it")
    notes.append("boot ROM and loader (0..0x40000) identical to stock")
    region = unpack_region(mod, APP)
    if not region.checksum_valid:
        raise Failed(f"application checksum 0x{region.checksum_stored:04x} != "
                     f"0x{region.checksum_calculated:04x}: the boot ROM would drop to the loader")
    end = APP + 4 + len(region.packed) + 2
    if end > APP_END:
        raise Failed(f"packed application ends at 0x{end:x}, past 0x{APP_END:x}")
    notes.append(f"application unpacks to 0x{len(region.unpacked):x} bytes, checksum valid, "
                 f"ends at 0x{end:x}")
    if upd is not None:
        version, crc_ok = make_upd.check(upd)
        if not crc_ok:
            raise Failed("the .UPD CRC does not match")
        if upd[0x1F:0x20] != b"0":
            raise Failed(f"the .UPD flag byte is {upd[0x1F:0x20]!r}, not '0': it would "
                         "rewrite the boot ROM and loader")
        if (version[0], version[2], version[3]) <= ("4", "3", "3"):
            raise Failed(f"the .UPD says version {version}; the updater only takes a "
                         "version above the installed 4.33")
        carried = decode_srecords(upd[make_upd.HEADER_SIZE:-2])
        if unpack_region(carried, APP).unpacked != region.unpacked:
            raise Failed("the .UPD does not carry the application in this image")
        notes.append(f".UPD version {version}, flag '0', CRC ok, carries this image")
    return notes


def run_boot(mod_path: Path, out: Path) -> list[str]:
    """Both boards, 75 s, through boot_vm; its milestones decide."""
    env = dict(os.environ, TEMP=str(out), TMPDIR=str(out))
    cmd = [sys.executable, "-m", "tools.cdj_main.boot_vm", "--firmware", str(mod_path),
           "--seconds", "75", "--poll-every", "15", "--caution",
           "--output", str(out / "boot-final.ppm")]
    result = subprocess.run(cmd, cwd=REPO, env=env, capture_output=True, text=True)
    (out / "boot.txt").write_text(result.stdout + result.stderr)
    ticks = [int(m) for m in re.findall(r"rtos=(\d+)", result.stdout)]
    if len(ticks) < 3 or ticks[-1] <= ticks[-2]:
        raise Failed(f"MAIN stopped: RTOS ticks {ticks[-3:]} (a crash or a hang; see "
                     "boot.txt and the EXPEVT/TEA recipe)")
    if not re.search(r"GuiCom=1 ready=1/1", result.stdout):
        raise Failed("the MAIN/GUI handshake never completed")
    frame = out / f"vm-frame{'' if PORT == 5980 else f'-{PORT}'}.ppm"   # boot_vm's own name
    if not frame.exists() or frame.stat().st_size < 1000:
        raise Failed("the GUI drew no frame")
    try:
        from PIL import Image
        image = Image.open(frame)
        image.resize((image.width * 2, image.height * 2)).save(out / "boot-screen.png")
    except Exception:
        pass
    codes = set(re.findall(r"^\s+code (\d+)\s+prio", result.stdout.split(
        "== every code this boot ==")[-1], re.M))
    notes = [f"MAIN alive for the whole run (RTOS {ticks[0]} -> {ticks[-1]}), handshake "
             "done, frame drawn: boot-screen.png"]
    # Stock 4.33 raises code 29 (a PLAYER record, then deleted) on every boot.
    new, gone = sorted(codes - STOCK_CAUTION), sorted(STOCK_CAUTION - codes)
    notes.append("caution codes: " + (", ".join(sorted(codes)) or "none")
                 + (f"; new against stock: {', '.join(new)}" if new else "")
                 + (f"; stock raises {', '.join(gone)} and this did not" if gone else "")
                 + ("" if new or gone else " (as stock)"))
    return notes


def monitor_command(command: str, timeout: float = 10.0) -> None:
    with socket.create_connection(("127.0.0.1", PORT + 1), timeout=timeout) as sock:
        time.sleep(0.3)
        sock.sendall(command.encode() + b"\n")
        time.sleep(1.0)


def run_update(flash: Path, stick: Path, out: Path, name: str,
               expect_loader: bool, limit_s: float) -> bytes:
    """Boot MAIN alone with the update keys held; return the flash when done."""
    console = out / f"{name}-console.txt"
    dump = out / f"{name}-flash-after.bin"
    for path in (console, dump):
        path.unlink(missing_ok=True)
    qemu = subprocess.Popen(
        [str(QEMU), "-M", "cdj2000-main", "-bios", str(flash),
         "-display", "none", "-no-reboot", "-d", "unimp", "-D", str(out / f"{name}-main.log"),
         "-serial", f"tcp:127.0.0.1:{PORT},server,nowait",
         "-serial", f"tcp:127.0.0.1:{PORT + 2},server,nowait",
         "-serial", f"file:{console}",
         "-monitor", f"telnet:127.0.0.1:{PORT + 1},server,nowait",
         "-drive", f"if=none,id=usbstick,format=raw,file={stick}",
         "-device", "usb-storage,drive=usbstick,removable=on"],
        cwd=REPO, env=dict(qemu_environment(), CDJ_TMU_FREQ="54000000",
                           CDJ_PANEL_FRAME=UPDATE_KEYS),
        stdout=subprocess.DEVNULL, stderr=open(out / f"{name}-stderr.log", "wb"))
    started = time.monotonic()
    try:
        while True:
            text = console.read_bytes() if console.exists() else b""
            if UPDATE_DONE in text:
                break
            if qemu.poll() is not None:
                raise Failed(f"QEMU exited ({qemu.returncode}) before the update finished; "
                             f"see {name}-stderr.log")
            if time.monotonic() - started > limit_s:
                raise Failed(f"no '{UPDATE_DONE.decode()}' within {limit_s:.0f} s: the "
                             f"updater never ran or never finished (see {console.name}). "
                             "On a real player this image could not be flashed back")
            time.sleep(2)
        took = time.monotonic() - started
        time.sleep(2)
        monitor_command(f'pmemsave 0 {FLASH_SIZE:#x} "{dump.as_posix()}"')
        for _ in range(30):
            if dump.exists() and dump.stat().st_size == FLASH_SIZE:
                break
            time.sleep(0.5)
        monitor_command("quit")
    finally:
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
    text = console.read_bytes()
    if expect_loader and LOADER_BANNER not in text:
        raise Failed("the update finished, but not through the loader: the blanked "
                     "sector did not make the boot ROM fall back")
    if not dump.exists():
        raise Failed("the flash could not be read back after the update")
    print(f"    update finished in {took:.0f} s wall")
    return dump.read_bytes()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("image", type=Path, help="the mod's MAIN flash image (repack_main output)")
    parser.add_argument("--upd", type=Path, help="the .UPD you intend to put on a stick")
    parser.add_argument("--stock", type=Path, default=FIRMWARE / "main-firmware.bin",
                        help="stock MAIN flash image (default firmware/main-firmware.bin)")
    parser.add_argument("--quick", action="store_true", help="static and boot checks only")
    args = parser.parse_args(argv)

    mod = args.image.read_bytes()
    stock = args.stock.read_bytes()
    stock_app = unpack_region(stock, APP).unpacked
    out = REPO / "runs" / "safety" / f"{args.image.stem}-{dt.datetime.now():%Y%m%d-%H%M%S}"
    out.mkdir(parents=True)
    mod_path = out / "mod-flash.bin"
    mod_path.write_bytes(mod)
    print(f"safety_check {args.image}  ->  {out.relative_to(REPO)}")

    steps = [("static", lambda: static_checks(
                 mod, stock, args.upd.read_bytes() if args.upd else None)),
             ("boot", lambda: run_boot(mod_path, out))]
    if not args.quick:
        stick_dir = out / "stick"
        stick_dir.mkdir()
        (stick_dir / "C2KMAIN.UPD").write_bytes(make_upd.build(stock, RETURN_VERSION))
        stick = out / "stick.img"
        subprocess.run([sys.executable, "-m", "tools.cdj_main.make_sd_image", str(stick_dir),
                        str(stick), "--size", "64M"], cwd=REPO, check=True,
                       stdout=subprocess.DEVNULL)

        def rollback():
            after = run_update(mod_path, stick, out, "rollback", False, ROLLBACK_LIMIT_S)
            region = unpack_region(after, APP)
            if not region.checksum_valid or region.unpacked != stock_app:
                raise Failed("after the update the flash does not hold the stock application")
            if after[:APP] != stock[:APP]:
                raise Failed("the update changed the boot ROM / loader region")
            # "*** Update END ! ***" is printed even when the updater decides
            # there is nothing to do, so only a changed application proves a
            # write -- and an image identical to stock cannot show one.
            if unpack_region(mod, APP).unpacked == stock_app:
                return ["updater reached 'Update END', but this image IS stock, so a "
                        "write cannot be told from a no-op: inconclusive, not a proof"]
            return ["the mod ran its updater and flashed stock 4.33 (sent as "
                    f"{RETURN_VERSION}) over itself; the application changed to stock"]

        def recovery():
            broken = bytearray(mod)
            region = unpack_region(mod, APP)
            sector = ((APP + 4 + len(region.packed) // 2) // 0x10000) * 0x10000
            broken[sector:sector + 0x10000] = b"\xff" * 0x10000
            broken_path = out / "mod-broken-sector.bin"
            broken_path.write_bytes(broken)
            after = run_update(broken_path, stick, out, "recovery", True, RECOVERY_LIMIT_S)
            region = unpack_region(after, APP)
            if not region.checksum_valid or region.unpacked != stock_app:
                raise Failed("the loader finished, but the flash does not hold the stock application")
            return [f"sector 0x{sector:x} blanked -> boot ROM fell back to the loader, "
                    "which flashed stock; stock application verified"]

        steps += [("rollback", rollback), ("recovery", recovery)]

    for number, (name, step) in enumerate(steps, 1):
        print(f"[{number}/{len(steps)}] {name} ...", flush=True)
        try:
            for note in step():
                print(f"    ok  {note}")
        except Failed as failure:
            print(f"    FAIL  {failure}")
            print(f"\nNOT SAFE: {name} failed.  Artifacts: {out.relative_to(REPO)}")
            return 1
    verdict = "PASSED (quick: rollback and recovery not run)" if args.quick else "PASSED"
    print(f"\n{verdict}.  Artifacts: {out.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
