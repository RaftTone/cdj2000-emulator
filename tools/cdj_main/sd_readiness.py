"""Read the SD mount-gate state out of a running MAIN, without changing it.

    python -m tools.cdj_main.sd_readiness --sd runs/wav-test/test-track.img

NXS_SD_READINESS.md ends by asking for exactly this and nothing more:

    Capture all three readiness globals, the latch, device pointer, callback
    and status byte from stopped RAM.  If readiness is true and the latch is 1,
    follow callback4 and the mount task instead of changing card-detect
    polarity or injecting a media-ready value.

So this observes and reports.  It writes nothing into the guest, sets no
media-ready value and does not touch card-detect polarity - the two shortcuts
that document names.  Every address below is that document's, and the predicate
is evaluated here the way the document states it so the report says which arm
is false rather than only that the gate is shut.

`0x04238922` (readiness) is true if ANY of:
  * u32 0x04cf2180 == 3
  * u32 0x04cf2994 != 0
  * media-mode 0x042a0348 returns 0, where media-mode returns 1 when
    0x04cf2180 is 4 or 5 and otherwise returns u32 0x04cf222c

`0x04238b64` (the gate) calls readiness, reads SDHI INFO1 at 0xffe4001c whose
bit 5 is ACTIVE-LOW card detect, and if present AND ready AND the latch at
0x049832ec is not already 1, sets the latch, sets 0x40 in byte device+0x66 and
calls the callback at device+0x1c with argument 4.  The device pointer is u32
0x049832f0.
"""
from __future__ import annotations

import argparse
import re
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

QEMU = ROOT / "build/qemu/build/qemu-system-sh4"
FIRMWARE = ROOT / "firmware/nxs"
PORT = 55701

MODE_STATE = 0x04CF2180      # 3 -> ready; 4 or 5 -> media-mode returns 1
TABLE_ENTRY = 0x04CF2994     # nonzero -> ready
MEDIA_MODE_FALLBACK = 0x04CF222C   # media-mode's value when state is not 4/5
LATCH = 0x049832EC           # 1 once the gate has fired
DEVICE_POINTER = 0x049832F0  # -> device; +0x1c callback, +0x66 status byte
SDHI_INFO1 = 0xFFE4001C      # bit 5, active low, is card detect
FLAGS_BYTE = 0x051E21D0      # r14+76; bit 1 set => media mode 1 => not ready


# The telnet monitor ECHOES the command, so "xp /1wx 0xffe4001c" puts the
# ADDRESS on the wire in the same 0x%08x shape as a value.  A reader that
# greps for 0x[0-9a-f]{8} therefore reports the address it just asked about as
# though it were the contents - which is exactly what happened on the first
# run of this tool, where the mode-state global appeared to hold 0xffe4001c.
# Only cells that follow an "address:" prefix are data, so match those.
CELL = re.compile(r"^[0-9a-f]{8,16}:((?:\s+0x[0-9a-f]{8})+)\s*$", re.MULTILINE)


def read_words(monitor: socket.socket, address: int, count: int) -> list[int]:
    """`xp` reads physical memory without disturbing the guest."""
    try:                                  # drop anything still in flight
        while monitor.recv(1 << 16):
            pass
    except socket.timeout:
        pass
    monitor.sendall(("xp /%dwx %#x\n" % (count, address)).encode())
    deadline = time.time() + 5
    text = ""
    while time.time() < deadline:
        try:
            text += monitor.recv(1 << 16).decode("utf-8", "replace")
        except socket.timeout:
            pass
        values = [int(v, 16)
                  for row in CELL.findall(text)
                  for v in re.findall(r"0x([0-9a-f]{8})", row)]
        if len(values) >= count:
            return values[:count]
    raise RuntimeError("monitor did not answer for %#x (got %r)"
                       % (address, text[-200:]))


def byte_of(word: int, address: int) -> int:
    """Byte at `address` out of the aligned word containing it.

    SH4 here is LITTLE-endian: byte n of a word is bits 8n..8n+7.  Reading it
    big-endian is what made the media-mode flag look like 0x00 for a whole run
    while the firmware was acting on a set bit 1 - see NXS_SD_READINESS.md.
    """
    return (word >> (8 * (address & 3))) & 0xFF


def sample(monitor: socket.socket) -> dict:
    mode_state = read_words(monitor, MODE_STATE, 1)[0]
    table_entry = read_words(monitor, TABLE_ENTRY, 1)[0]
    fallback = read_words(monitor, MEDIA_MODE_FALLBACK, 1)[0]
    latch = read_words(monitor, LATCH, 1)[0]
    device = read_words(monitor, DEVICE_POINTER, 1)[0]
    info1 = read_words(monitor, SDHI_INFO1 & ~3, 1)[0]
    flags_word = read_words(monitor, FLAGS_BYTE & ~3, 1)[0]
    flags = byte_of(flags_word, FLAGS_BYTE)

    media_mode = 1 if mode_state in (4, 5) else fallback
    arms = {
        "state == 3": mode_state == 3,
        "table entry nonzero": table_entry != 0,
        "media mode == 0": media_mode == 0,
    }
    result = dict(
        mode_state=mode_state, table_entry=table_entry,
        media_mode_fallback=fallback, media_mode=media_mode,
        readiness_arms=arms, ready=any(arms.values()),
        latch=latch, device=device, info1_word=info1,
        # INFO1 is a halfword at 0xffe4001c; bit 5 is active low.
        card_detect_bit=(info1 >> 5) & 1,
        card_present=not ((info1 >> 5) & 1),
        flags_byte=flags, flags_bit1=bool(flags & 2),
    )
    if 0x04000000 <= device < 0x05000000:
        result["callback"] = read_words(monitor, device + 0x1C, 1)[0]
        status_word = read_words(monitor, (device + 0x66) & ~3, 1)[0]
        result["status_byte"] = byte_of(status_word, device + 0x66)
        result["status_bit40_set"] = bool(result["status_byte"] & 0x40)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sd", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=90.0)
    parser.add_argument("--every", type=float, default=15.0,
                        help="sample interval, so a transition is visible")
    parser.add_argument("--env", action="append", default=[],
                        metavar="NAME=VALUE")
    args = parser.parse_args()

    import os
    env = dict(os.environ)
    for item in args.env:
        name, _, value = item.partition("=")
        env[name] = value

    log = Path("/tmp/sd-readiness-main.log")
    board = subprocess.Popen(
        [str(QEMU), "-M", "cdj2000nxs-main",
         "-bios", str(FIRMWARE / "main-firmware.bin"),
         "-display", "none", "-no-reboot", "-d", "unimp", "-D", str(log),
         "-serial", "null", "-serial", "null", "-serial", "null",
         "-monitor", f"telnet:127.0.0.1:{PORT},server,nowait",
         "-drive", f"if=sd,format=raw,snapshot=on,file={args.sd.resolve()}"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    monitor = None
    try:
        time.sleep(3)
        monitor = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        monitor.settimeout(1.0)
        monitor.recv(1 << 16)
        elapsed = 3.0
        while elapsed < args.seconds:
            time.sleep(args.every)
            elapsed += args.every
            row = sample(monitor)
            arms = " ".join("%s=%s" % (k.split()[0], "T" if v else "F")
                            for k, v in row["readiness_arms"].items())
            print("t=%5.1f  flags=%#04x bit1=%d  state=%#010x entry=%#010x mode=%#x  [%s]  "
                  "ready=%s  card=%s  latch=%#x  device=%#010x%s"
                  % (elapsed, row["flags_byte"], row["flags_bit1"], row["mode_state"], row["table_entry"],
                     row["media_mode"], arms, row["ready"],
                     row["card_present"], row["latch"], row["device"],
                     ("  cb=%#010x status=%#04x" %
                      (row["callback"], row["status_byte"]))
                     if "callback" in row else ""))
        monitor.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        if monitor:
            monitor.close()
        try:
            board.wait(timeout=20)
        except subprocess.TimeoutExpired:
            board.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
