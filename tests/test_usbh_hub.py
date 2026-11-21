"""Enumeration through a hub on the SH7764 USB host module (cdj2000_usbh.c).

No firmware: the guest CPU never runs (qtest accelerator) and the test is the
host driver, programming the module's registers the way the Cente driver does
(DCPMAXP DEVSEL, DEVADDn, USBREQ..USBLENG, SUREQ, the CFIFO) and stepping the
virtual clock.  The devices are QEMU's: usb-hub (with high-speed=on from
patches/qemu-usb-hub-high-speed.patch), usb-storage and usb-kbd.

What it checks is DEVADDn (hardware manual 21.3.39): a device behind a hub is
reached when USBSPD is its speed and, for a full-speed device behind a
high-speed hub (split transactions), when UPPHUB/HUBPORT name that hub and
port; otherwise the SETUP is not answered (SIGN).  Build QEMU with
scripts/build-qemu-sh4.sh first, or set CDJ_QEMU to the binary.
"""

from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess

import pytest

from tests.qtest_pipe import read_reply

ROOT = Path(__file__).resolve().parents[1]
USBH = 0x1E400000           # area 7; the firmware uses the P4 alias 0xfe400000
SYSCFG, DVSTCTR, CFIFO, CFIFOSEL, CFIFOCTR = 0x00, 0x08, 0x14, 0x20, 0x22
INTSTS1, BRDYSTS, NRDYSTS, BEMPSTS = 0x42, 0x46, 0x48, 0x4A
USBREQ, DCPCFG, DCPMAXP, DCPCTR, DEVADD0 = 0x54, 0x5C, 0x5E, 0x60, 0xD0
SACK, SIGN = 0x0010, 0x0020
FULL, HIGH = 0x0080, 0x00C0  # DEVADDn USBSPD
STEP = 5_000_000            # ns of guest time per stage: retries are 1 ms


def devadd(speed, hub=0, port=0):
    return hub << 11 | port << 8 | speed


@pytest.fixture
def deck(tmp_path):
    binary = Path(os.environ.get("CDJ_QEMU", ROOT / "build/qemu/build/qemu-system-sh4"))
    if not binary.is_file():
        pytest.skip("build the CDJ QEMU board or set CDJ_QEMU")
    bios = tmp_path / "dummy.bin"
    bios.write_bytes(bytes(16))
    stick = tmp_path / "stick.img"
    stick.write_bytes(bytes(1 << 20))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CDJ_", "BFIN_"))}
    procs = []

    def start(*devices):
        log = (tmp_path / "qemu.log").open("w")
        proc = subprocess.Popen(
            [str(binary), "-M", "cdj2000-main", "-bios", str(bios), "-nodefaults",
             "-display", "none", "-accel", "qtest", "-qtest", "stdio",
             "-qtest-log", os.devnull,
             "-drive", f"if=none,id=stick,format=raw,file={stick}", *devices],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=env,
            bufsize=0,
        )
        procs.append((proc, log))
        return Host(proc)

    yield start
    for proc, log in procs:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stdin.close()
        proc.stdout.close()
        log.close()


class Host:
    """Just enough of a host driver for control transfers on the DCP."""

    def __init__(self, proc):
        self.proc = proc

    def command(self, line):
        self.proc.stdin.write((line + "\n").encode())
        response = read_reply(self.proc.stdout, 5)
        assert response.startswith("OK"), response
        return response[2:].strip()

    def w(self, off, value):
        self.command(f"writew {USBH + off:#x} {value:#x}")

    def r(self, off):
        return int(self.command(f"readw {USBH + off:#x}"), 16)

    def step(self):
        self.command(f"clock_step {STEP}")

    def power_up(self):
        self.w(SYSCFG, 0x04E1)          # SCKE HSE DCFM DRPD USBE
        self.w(DVSTCTR, 0x0040)         # USBRST
        self.step()
        self.w(DVSTCTR, 0x0010)         # UACT
        self.step()
        return self.r(DVSTCTR) & 7      # RHST: 2 full speed, 3 high speed

    def control(self, address, route, request_type, request, value=0,
                index=0, length=0):
        """One control transfer; the IN data, b"" for none, None if it failed."""
        self.w(DEVADD0 + 2 * address, route)
        self.w(DCPMAXP, address << 12 | 64)
        for off, word in zip(range(USBREQ, USBREQ + 8, 2),
                             struct.unpack("<4H", struct.pack(
                                 "<BBHHH", request_type, request, value,
                                 index, length))):
            self.w(off, word)
        for off in (INTSTS1, BRDYSTS, NRDYSTS, BEMPSTS):
            self.w(off, 0)
        self.w(DCPCTR, 0x4000)          # SUREQ (and PID NAK)
        self.step()
        if self.r(INTSTS1) & SIGN:
            return None
        assert self.r(INTSTS1) & SACK
        data = b""
        if length and request_type & 0x80:
            self.w(DCPCFG, 0)           # receive
            self.w(CFIFOSEL, 0)
            self.w(DCPCTR, 1)           # PID BUF
            self.step()
            if not self.r(BRDYSTS) & 1:
                return None
            count = self.r(CFIFOCTR) & 0xFFF
            data = bytes(int(self.command(f"readb {USBH + CFIFO:#x}"), 16)
                         for _ in range(count))
            # Status stage: a zero-length OUT.
            self.w(DCPCFG, 0x10)
            self.w(CFIFOSEL, 0x20)      # ISEL
            self.w(CFIFOCTR, 0x8000)    # BVAL
            self.w(DCPCTR, 1)
            self.step()
            return data if self.r(BEMPSTS) & 1 else None
        # Status stage: a zero-length IN.
        self.w(DCPCFG, 0)
        self.w(CFIFOSEL, 0)
        self.w(DCPCTR, 1)
        self.step()
        return data if self.r(BRDYSTS) & 1 else None

    def device_descriptor(self, address, route):
        return self.control(address, route, 0x80, 6, 0x0100, 0, 18)

    def hub_up(self, route, port, child_route):
        """Address the hub as 1, configure it, reset `port`; its port status."""
        assert self.control(0, route, 0x00, 5, 1) == b""        # SET_ADDRESS 1
        hub = self.device_descriptor(1, route)
        assert hub and hub[4] == 9                              # class hub
        assert self.control(1, route, 0x00, 9, 1) == b""        # SET_CONFIGURATION
        assert self.control(1, route, 0xA0, 6, 0x2900, 0, 9)[1] == 0x29
        assert self.control(1, route, 0x23, 3, 8, port) == b""  # PORT_POWER
        # The child is not on the bus before its hub port is reset.
        assert self.device_descriptor(0, child_route) is None
        assert self.control(1, route, 0x23, 3, 4, port) == b""  # PORT_RESET
        status = self.control(1, route, 0xA3, 0, 0, port, 4)
        return hub, struct.unpack("<HH", status)[0]


def hub_args(high_speed, child):
    return ["-device", f"usb-hub,id=hub0,bus=usb-bus.0,port=1,ports=4,"
                       f"high-speed={'on' if high_speed else 'off'}",
            "-device", child + ",bus=usb-bus.0,port=1.2"]


def test_high_speed_stick_behind_high_speed_hub(deck):
    host = deck(*hub_args(True, "usb-storage,drive=stick,removable=on"))
    assert host.power_up() == 3
    hub, status = host.hub_up(devadd(HIGH), 2, devadd(HIGH, 1, 2))
    assert hub[:4] == bytes([18, 1, 0x00, 0x02]) and hub[6] == 1   # USB 2.0, one TT
    assert status & 0x0403 == 0x0403                    # connected, enabled, HS
    # A high-speed target needs no split: UPPHUB/HUBPORT are not looked at.
    child = host.device_descriptor(0, devadd(HIGH, 1, 2))
    assert child and child[8:10] == bytes([0xF4, 0x46])  # QEMU's usb-storage
    assert host.device_descriptor(0, devadd(FULL, 1, 2)) is None   # wrong speed
    assert host.control(0, devadd(HIGH, 1, 2), 0x00, 5, 2) == b""  # SET_ADDRESS 2
    assert host.device_descriptor(2, devadd(HIGH, 1, 2)) == child
    assert host.device_descriptor(2, 0) is None         # USBSPD 00: not used


def test_full_speed_device_behind_high_speed_hub_needs_the_split_route(deck):
    host = deck(*hub_args(True, "usb-kbd,usb_version=1"))
    assert host.power_up() == 3
    _, status = host.hub_up(devadd(HIGH), 2, devadd(FULL, 1, 2))
    assert status & 0x0603 == 0x0003                    # connected, enabled, FS
    assert host.device_descriptor(0, devadd(FULL)) is None         # no split
    assert host.device_descriptor(0, devadd(FULL, 1, 1)) is None   # wrong port
    assert host.device_descriptor(0, devadd(FULL, 2, 2)) is None   # wrong hub
    assert host.device_descriptor(0, devadd(HIGH, 1, 2)) is None   # wrong speed
    child = host.device_descriptor(0, devadd(FULL, 1, 2))
    assert child and child[8:10] == bytes([0x27, 0x06])  # QEMU's usb-kbd, USB 1.1


def test_stick_behind_full_speed_hub(deck):
    host = deck(*hub_args(False, "usb-storage,drive=stick,removable=on"))
    assert host.power_up() == 2                         # the root link is FS
    assert host.device_descriptor(0, devadd(HIGH)) is None
    hub, status = host.hub_up(devadd(FULL), 2, devadd(FULL))
    assert hub[2:4] == bytes([0x10, 0x01])              # QEMU's USB 1.1 hub
    assert status & 0x0603 == 0x0003
    # No split on a full-speed bus: the hub repeats, UPPHUB/HUBPORT unused.
    child = host.device_descriptor(0, devadd(FULL))
    assert child and child[8:10] == bytes([0xF4, 0x46])
    assert host.device_descriptor(0, devadd(HIGH, 1, 2)) is None


def test_stick_on_the_root_port(deck):
    host = deck("-device", "usb-storage,drive=stick,removable=on,bus=usb-bus.0,port=1")
    assert host.power_up() == 3
    assert host.device_descriptor(0, devadd(HIGH))[8:10] == bytes([0xF4, 0x46])
    assert host.device_descriptor(0, devadd(FULL)) is None
