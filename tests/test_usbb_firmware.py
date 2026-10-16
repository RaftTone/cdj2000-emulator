"""The real MAIN firmware against the USB-B model, over the host interface.

Boots stock MAIN on QEMU (no GUI board), plugs the host into the rear USB-B
socket (CDJ_USBB_HOST) and enumerates the deck the way a computer does, then
checks MAIN's own device-status array: device 3 is the USB function, and 2
there is `E-7020: USB-B DEVICE ERROR`.  It skips unless firmware/ is populated
(FIRMWARE.md) and the emulator is built (BUILD.md); CDJ_QEMU names another
binary.  It takes about a minute.  Nothing from the firmware is asserted
beyond sizes and the fact that the chain completes.
"""
from __future__ import annotations

import os
import re
import socket
import subprocess
import time
from pathlib import Path

import pytest

from tools.cdj_main import usbb_host as u

ROOT = Path(__file__).resolve().parents[1]
QEMU = Path(os.environ.get("CDJ_QEMU", ROOT / "build/qemu/build/qemu-system-sh4"))
FLASH = ROOT / "firmware/main-firmware.bin"
STATUS_ARRAY = 0x04C0875C           # MAIN's eight-entry device-status array
USB_FUNCTION = 3                    # device 3: 1 up, 2 failed (E-7020)


def free_port(kind=socket.SOCK_DGRAM) -> int:
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def monitor(port: int, command: str) -> str:
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(1.0)
    time.sleep(0.3)
    try:
        sock.recv(4096)
    except socket.timeout:
        pass
    sock.sendall((command + "\n").encode())
    time.sleep(0.5)
    out = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    sock.close()
    return out.decode(errors="replace")


def device_states(mon_port: int) -> list[int]:
    text = monitor(mon_port, f"xp /8wx {STATUS_ARRAY:#x}")
    words = []
    for line in text.replace("\r", "").split("\n"):
        if re.match(r"^[0-9a-f]{8}:", line):
            words += [int(w, 16) for w in line.split()[1:]]
    return words


@pytest.mark.skipif(not QEMU.exists() or not FLASH.exists(),
                    reason="needs the built emulator and firmware/main-firmware.bin")
@pytest.mark.parametrize("absent", [False, True], ids=["model", "absent"])
def test_main_firmware_on_usbb(tmp_path, absent):
    host_port = free_port()
    mon_port = free_port(socket.SOCK_STREAM)
    env = dict(os.environ, CDJ_TMU_FREQ="54000000")
    if absent:
        env["CDJ_USB_ABSENT"] = "1"     # the machine before the chip existed
    else:
        env["CDJ_USBB_HOST"] = str(host_port)
    board = subprocess.Popen(
        [str(QEMU), "-M", "cdj2000-main", "-bios", str(FLASH),
         "-display", "none", "-no-reboot", "-serial", "null", "-serial", "null",
         "-serial", "null", "-monitor",
         f"tcp:127.0.0.1:{mon_port},server,nowait", "-nodefaults"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if absent:
            time.sleep(25)
            assert device_states(mon_port)[USB_FUNCTION] == 2   # E-7020, as before
            return
        bus = u.Bus(host_port, timeout=5.0)
        host, rep = u.enumerate_deck(bus, lambda text: None,
                                     attach_timeout=120, nak_timeout=30.0)
        assert rep.configured
        assert rep.vid and rep.pid
        assert rep.device_len == 18 and rep.config_len > 9
        assert rep.hid_report_len and all(n > 0 for n in rep.hid_report_len.values())
        assert bus.status().dvsq == 3                           # Configured
        assert device_states(mon_port)[USB_FUNCTION] == 1       # no E-7020
        bus.close()
    finally:
        board.kill()
        board.wait(10)
