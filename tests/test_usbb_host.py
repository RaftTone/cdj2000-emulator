"""The USB-B host (tools/cdj_main/usbb_host.py) against a stand-in deck.

No QEMU and no firmware: a thread answers the datagram protocol the way the
emulator's M66291 model does, with an invented device, so the host's
enumeration, NAK retry and report polling are exercised on their own.  The
chip model itself is tested in test_usb_m66291.py, and the two meet in the
emulator (see RUNNING.md, "USB-B").
"""
from __future__ import annotations

import socket
import struct
import threading

import pytest

from tools.cdj_main import usbb_host as u

DEVICE = bytes([18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xe4, 0x08, 0x4a, 0x01,
                0x33, 0x04, 1, 2, 3, 1])
CONFIG = (bytes([9, 2, 0, 0, 1, 1, 0, 0xc0, 0])
          + bytes([9, 4, 0, 0, 2, 3, 0, 0, 0])
          + bytes([9, 0x21, 0x10, 1, 0, 1, 0x22, 8, 0])
          + bytes([7, 5, 0x04, 3, 64, 0, 1])
          + bytes([7, 5, 0x85, 3, 20, 0, 1]))
CONFIG = CONFIG[:2] + struct.pack("<H", len(CONFIG)) + CONFIG[4:]
REPORT_DESC = bytes(range(8))


class FakeDeck(threading.Thread):
    """Just enough of the protocol: one device, one configuration, one HID."""

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.stop = False
        self.vbus = False
        self.attached_after = 3     # status polls before the pull-up shows
        self.polls = 0
        self.address = 0
        self.configured = False
        self.pending = b""
        self.control_in = b""
        self.naks_left = 2          # NAK the first data stage IN, like a deck
        self.reports = 0

    def reply(self, to, op, seq, result, payload=b""):
        self.sock.sendto(struct.pack("<BBBBH", op | 0x80, seq, result, 0,
                                     len(payload)) + payload, to)

    def run(self):
        while not self.stop:
            try:
                data, to = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            op, seq, addr, ep, length = struct.unpack_from("<BBBBH", data)
            body = data[6:]
            if op == u.OP_STATUS:
                self.polls += 1
                attached = self.vbus and self.polls > self.attached_after
                tail = bytes(28)
                self.reply(to, op, seq, u.ACK, bytes(
                    [self.vbus, attached, attached, 3 if attached else 0,
                     3 if self.configured else 1, 0, self.address, 0])
                    + struct.pack("<5I", 1, 2, 3, 4, 5) + tail)
            elif op == u.OP_ATTACH:
                self.vbus = True
                self.reply(to, op, seq, u.ACK)
            elif op == u.OP_RESET:
                self.address = 0
                self.configured = False
                self.reply(to, op, seq, u.ACK)
            elif op == u.OP_SETUP:
                if addr != self.address:
                    self.reply(to, op, seq, u.NORESP)
                    continue
                kind, req, value, index, wlen = struct.unpack("<BBHHH", body[:8])
                self.control_in = b""
                self.naks_left = 2
                if kind == 0x80 and req == 6:
                    which = value >> 8
                    self.control_in = (DEVICE if which == 1 else CONFIG)[:wlen]
                elif kind == 0x81 and req == 6 and value >> 8 == 0x22:
                    self.control_in = REPORT_DESC[:wlen]
                elif kind == 0 and req == 5:
                    self.pending_address = value
                elif kind == 0 and req == 9:
                    self.configured = True
                self.setup = (kind, req, value)
                self.reply(to, op, seq, u.ACK)
            elif op == u.OP_IN and ep == 0:
                kind, req, value = self.setup
                if kind & 0x80 and self.control_in:
                    if self.naks_left:
                        self.naks_left -= 1
                        self.reply(to, op, seq, u.NAK)
                        continue
                    chunk, self.control_in = self.control_in[:length], self.control_in[length:]
                    self.reply(to, op, seq, u.ACK, chunk)
                else:
                    if req == 5:
                        self.address = self.pending_address
                    self.reply(to, op, seq, u.ACK)
            elif op == u.OP_OUT and ep == 0:
                self.reply(to, op, seq, u.ACK)
            elif op == u.OP_IN and ep == 5:
                if self.configured:
                    self.reports += 1
                    self.reply(to, op, seq, u.ACK, bytes([self.reports]) * 20)
                else:
                    self.reply(to, op, seq, u.NAK)
            else:
                self.reply(to, op, seq, u.BADREQ)


@pytest.fixture
def deck():
    fake = FakeDeck()
    fake.start()
    yield fake
    fake.stop = True
    fake.join(2)


def test_enumerate_and_read_reports(deck):
    bus = u.Bus(deck.port, timeout=2.0)
    lines = []
    host, rep = u.enumerate_deck(bus, lines.append, attach_timeout=10,
                                 settle=0.0, nak_timeout=2.0)
    assert (rep.vid, rep.pid) == (0x08e4, 0x014a)
    assert rep.mxps0 == 64
    assert rep.device_len == 18 and rep.config_len == len(CONFIG)
    assert rep.configured and rep.config_value == 1
    assert rep.hid_report_len == {0: len(REPORT_DESC)}
    assert deck.address == 1

    u.poll_reports(host, rep, 3, 5.0, lines.append)
    assert len(rep.reports) == 3
    assert all(len(data) == 20 for _, data in rep.reports)
    assert [e for e, _ in rep.reports] == [0x85] * 3
    as_dict = rep.as_dict()
    assert as_dict["vid"] == "08e4" and as_dict["reports"][0]["bytes"] == 20
    # Descriptor contents are never printed, only their sizes.
    assert not any(DEVICE.hex() in line for line in lines)
    bus.close()


def test_a_deck_that_never_connects_times_out(deck):
    deck.attached_after = 10 ** 9
    bus = u.Bus(deck.port, timeout=1.0)
    with pytest.raises(u.Timeout):
        u.enumerate_deck(bus, lambda text: None, attach_timeout=0.5,
                         settle=0.0, nak_timeout=0.5)
    bus.close()


def test_the_emulator_not_listening_is_reported():
    bus = u.Bus(9, timeout=0.3)             # discard port, nobody answers
    with pytest.raises(u.UsbError):
        bus.status()
    bus.close()
