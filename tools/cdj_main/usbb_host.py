"""A computer on the CDJ-2000's rear USB-B socket: enumerate the deck, read HID.

    CDJ_USBB_HOST=5990 qemu-system-sh4 -M cdj2000-main ...        # the deck
    python -m tools.cdj_main.usbb_host --port 5990 --reports 5    # the host

The emulator's M66291GP model (emulator/qemu/cdj2000_usb_model.c) is the chip on
that socket; with CDJ_USBB_HOST=<port> it listens on UDP 127.0.0.1:<port> and
carries out one USB transaction per datagram.  This program is the host on the
other side: it plugs the cable in, resets the bus, and does what an operating
system does -- GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION, the HID report
descriptor, then polling the interrupt IN endpoints.  It prints descriptor
*lengths*, the VID/PID and the report lengths; it does not print descriptor
contents (pass --hex for report bytes).

The framing is documented at the top of emulator/qemu/cdj2000_usb.c.  The idea of
a loopback UDP host on the rear port follows Stijn Jacobs' CDJ-2000NXS model
(CDJ_USBF_HOST, hw/cdj/boards/nxs2/usbf_host.c) and P2GR's HID handshake host on
it (cdj-nxs2-qemu PR #15, scripts/net/hid_handshake_host.py); the HID
handshake itself is Nikolaus Einhauser's (swiftb0y, CDJHidProtocol).  Nothing
of theirs is copied: this host speaks transactions to a chip model, they talk to
a relay.  Written by RaftTone.

Copyright (C) 2026 RaftTone
SPDX-License-Identifier: GPL-2.0-or-later
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
from dataclasses import dataclass, field

OP_STATUS, OP_ATTACH, OP_DETACH, OP_RESET, OP_SETUP, OP_IN, OP_OUT, \
    OP_SUSPEND, OP_RESUME = range(9)
ACK, NAK, STALL, NORESP, BADREQ = range(5)
RESULT_NAMES = ("ACK", "NAK", "STALL", "NORESP", "BADREQ")

STATUS_LEN = 56


class UsbError(RuntimeError):
    pass


class Timeout(UsbError):
    pass


class Stalled(UsbError):
    pass


@dataclass
class Status:
    vbus: bool
    attached: bool
    enabled: bool
    tron: int
    dvsq: int
    ctsq: int
    address: int
    pins: int
    resets: int
    setups: int
    ins: int
    outs: int
    naks: int
    endpoints: list = field(default_factory=list)   # (type, is_in, mxps)

    DVSQ = ("powered", "default", "address", "configured")

    @property
    def state(self) -> str:
        name = self.DVSQ[self.dvsq & 3]
        return name + (" (suspended)" if self.dvsq & 4 else "")


def parse_status(payload: bytes) -> Status:
    if len(payload) < STATUS_LEN:
        raise UsbError(f"short status ({len(payload)} bytes)")
    vbus, attached, enabled, tron, dvsq, ctsq, address, pins = payload[:8]
    resets, setups, ins, outs, naks = struct.unpack_from("<5I", payload, 8)
    eps = [(payload[28 + 4 * i], bool(payload[29 + 4 * i]),
            struct.unpack_from("<H", payload, 30 + 4 * i)[0]) for i in range(7)]
    return Status(bool(vbus), bool(attached), bool(enabled), tron, dvsq, ctsq,
                  address, pins, resets, setups, ins, outs, naks, eps)


class Bus:
    """The datagram protocol: one request, one reply."""

    def __init__(self, port: int, host: str = "127.0.0.1", timeout: float = 5.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.peer = (host, port)
        self.seq = 0
        self.log = None             # a callable(str), or None

    def close(self) -> None:
        self.sock.close()

    def request(self, op: int, addr: int = 0, ep: int = 0,
                payload: bytes = b"", length: int | None = None):
        self.seq = (self.seq + 1) & 0xff
        n = len(payload) if length is None else length
        packet = struct.pack("<BBBBH", op, self.seq, addr, ep, n) + payload
        self.sock.sendto(packet, self.peer)
        while True:
            try:
                reply, _ = self.sock.recvfrom(2048)
            except socket.timeout as error:
                raise Timeout("the emulator did not answer on UDP "
                              f"{self.peer[1]}") from error
            except ConnectionResetError:
                # Windows reports the ICMP port-unreachable of an earlier
                # datagram on the next receive: nobody was listening then.
                # Keep waiting, so this ends in the Timeout above.
                continue
            if len(reply) >= 6 and reply[0] == (op | 0x80) and reply[1] == self.seq:
                break               # a stale reply to an earlier request is skipped
        result = reply[2]
        size = struct.unpack_from("<H", reply, 4)[0]
        return result, reply[6:6 + size]

    def wait_ready(self, seconds: float = 60.0) -> Status:
        """Ask for the status until the emulator answers (it may still be starting)."""
        deadline = time.monotonic() + seconds
        old = self.sock.gettimeout()
        self.sock.settimeout(0.5)
        try:
            while True:
                try:
                    return self.status()
                except Timeout:
                    if time.monotonic() > deadline:
                        raise
        finally:
            self.sock.settimeout(old)

    def status(self) -> Status:
        result, payload = self.request(OP_STATUS)
        if result != ACK:
            raise UsbError(f"status: {RESULT_NAMES[result]}")
        return parse_status(payload)

    def attach(self) -> None:
        self.request(OP_ATTACH)

    def detach(self) -> None:
        self.request(OP_DETACH)

    def reset(self) -> bool:
        return self.request(OP_RESET)[0] == ACK


class Host:
    """A host controller: retries NAKs the way one does, gives up on silence."""

    def __init__(self, bus: Bus, nak_timeout: float = 20.0, poll: float = 0.005):
        self.bus = bus
        self.nak_timeout = nak_timeout
        self.poll = poll
        self.address = 0
        self.mxps0 = 8

    def _retry(self, op, ep, payload=b"", length=None, timeout=None, addr=None):
        deadline = time.monotonic() + (self.nak_timeout if timeout is None
                                       else timeout)
        silent = 0
        while True:
            result, data = self.bus.request(
                op, self.address if addr is None else addr, ep, payload, length)
            if result == ACK or result == STALL:
                return result, data
            if result == NORESP:
                silent += 1
                if silent > 50:
                    raise Timeout("no response from the deck (not attached, "
                                  "or the address is wrong)")
            if time.monotonic() > deadline:
                if result == NAK:
                    return NAK, b""
                raise Timeout(f"{RESULT_NAMES[result]} until the deadline")
            time.sleep(self.poll)

    def in_packet(self, ep: int, size: int, timeout=None):
        return self._retry(OP_IN, ep, length=size, timeout=timeout)

    def control(self, request_type: int, request: int, value: int, index: int,
                length: int, data: bytes = b"") -> bytes:
        """One control transfer, all three stages; returns the IN data."""
        setup = struct.pack("<BBHHH", request_type, request, value, index, length)
        result, _ = self._retry(OP_SETUP, 0, setup)
        if result != ACK:
            raise Stalled("SETUP")
        received = bytearray()
        if request_type & 0x80:
            while len(received) < length:
                result, packet = self._retry(OP_IN, 0, length=self.mxps0)
                if result == STALL:
                    raise Stalled(f"IN data stage of request {request:#x}")
                if result == NAK:
                    raise Timeout(f"request {request:#x}: no data from the deck")
                received += packet
                if len(packet) < self.mxps0:
                    break
            result, _ = self._retry(OP_OUT, 0)
            if result == STALL:
                raise Stalled(f"status stage of request {request:#x}")
        else:
            for off in range(0, len(data), self.mxps0):
                result, _ = self._retry(OP_OUT, 0, data[off:off + self.mxps0])
                if result == STALL:
                    raise Stalled(f"OUT data stage of request {request:#x}")
            result, _ = self._retry(OP_IN, 0, length=0)
            if result == STALL:
                raise Stalled(f"status stage of request {request:#x}")
        if result == NAK:
            raise Timeout(f"request {request:#x}: the status stage never completed")
        return bytes(received)

    def get_descriptor(self, kind: int, index: int, length: int,
                       recipient: int = 0, windex: int = 0, class_req: bool = False):
        request_type = 0x80 | (0x20 if class_req else 0) | recipient
        return self.control(request_type, 6, kind << 8 | index, windex, length)


def walk_descriptors(config: bytes):
    """Yield (type, bytes) over a configuration descriptor's whole block."""
    off = 0
    while off + 2 <= len(config) and config[off]:
        yield config[off + 1], config[off:off + config[off]]
        off += config[off]


@dataclass
class Report:
    vid: int = 0
    pid: int = 0
    bcd_usb: int = 0
    mxps0: int = 0
    device_len: int = 0
    config_len: int = 0
    config_value: int = 0
    interfaces: list = field(default_factory=list)   # (num, class, endpoints[])
    hid_report_len: dict = field(default_factory=dict)
    reports: list = field(default_factory=list)      # (endpoint, bytes)
    notes: list = field(default_factory=list)
    configured: bool = False
    final_state: str = ""

    def as_dict(self) -> dict:
        return {
            "vid": f"{self.vid:04x}", "pid": f"{self.pid:04x}",
            "bcdUSB": f"{self.bcd_usb:04x}", "ep0_packet": self.mxps0,
            "device_descriptor_bytes": self.device_len,
            "configuration_descriptor_bytes": self.config_len,
            "configuration_value": self.config_value,
            "interfaces": [{"number": n, "class": c,
                            "endpoints": [{"address": f"{a:02x}", "type": t,
                                           "max_packet": m, "interval": i}
                                          for a, t, m, i in eps]}
                           for n, c, eps in self.interfaces],
            "hid_report_descriptor_bytes": self.hid_report_len,
            "configured": self.configured,
            "reports": [{"endpoint": f"{e:02x}", "bytes": len(b)}
                        for e, b in self.reports],
            "state": self.final_state, "notes": self.notes,
        }


def wait_attached(bus: Bus, timeout: float, say) -> Status:
    """Plug the cable in and wait for the deck to put its pull-up on the bus."""
    bus.attach()
    deadline = time.monotonic() + timeout
    last = None
    while True:
        status = bus.status()
        if status.attached:
            return status
        mark = (status.vbus, status.enabled, status.tron)
        if mark != last:
            say(f"waiting for the deck: enabled={status.enabled} "
                f"TrON={status.tron} (it connects after it sees VBUS)")
            last = mark
        if time.monotonic() > deadline:
            raise Timeout("the deck never connected (is the module enabled, "
                          "and did it see VBUS?)")
        time.sleep(0.1)


def enumerate_deck(bus: Bus, say=print, attach_timeout: float = 120.0,
                   settle: float = 0.1, nak_timeout: float = 20.0) -> tuple[Host, Report]:
    host = Host(bus, nak_timeout=nak_timeout)
    rep = Report()
    bus.wait_ready(min(attach_timeout, 60.0))
    wait_attached(bus, attach_timeout, say)
    say("deck connected; settling")
    time.sleep(settle)
    if not bus.reset():
        raise UsbError("bus reset was not accepted")
    say("bus reset")
    time.sleep(0.05)

    head = host.get_descriptor(1, 0, 8)
    if len(head) < 8 or head[1] != 1:
        raise UsbError("GET_DESCRIPTOR(device, 8) returned no device descriptor")
    host.mxps0 = rep.mxps0 = head[7]
    say(f"device descriptor head: bMaxPacketSize0={head[7]}")

    bus.reset()
    time.sleep(0.05)
    host.control(0x00, 5, 1, 0, 0)                # SET_ADDRESS(1)
    host.address = 1
    time.sleep(0.02)
    say("SET_ADDRESS(1) done")

    dev = host.get_descriptor(1, 0, 18)
    rep.device_len = len(dev)
    if len(dev) >= 18:
        rep.bcd_usb = struct.unpack_from("<H", dev, 2)[0]
        rep.vid, rep.pid = struct.unpack_from("<HH", dev, 8)
    say(f"device descriptor: {len(dev)} bytes, VID {rep.vid:04x} PID {rep.pid:04x}")

    head = host.get_descriptor(2, 0, 9)
    total = struct.unpack_from("<H", head, 2)[0] if len(head) >= 4 else 0
    config = host.get_descriptor(2, 0, total) if total else head
    rep.config_len = len(config)
    say(f"configuration descriptor: {len(config)} bytes (wTotalLength {total})")
    if len(config) >= 6:
        rep.config_value = config[5]

    iface = None
    for kind, block in walk_descriptors(config):
        if kind == 4 and len(block) >= 9:
            iface = (block[2], block[5], [])
            rep.interfaces.append(iface)
        elif kind == 5 and len(block) >= 7 and iface:
            iface[2].append((block[2], block[3] & 3,
                             struct.unpack_from("<H", block, 4)[0], block[6]))
    for num, cls, eps in rep.interfaces:
        say(f"interface {num}: class {cls:#04x}, {len(eps)} endpoint(s)")

    host.control(0x00, 9, rep.config_value or 1, 0, 0)   # SET_CONFIGURATION
    rep.configured = True
    say(f"SET_CONFIGURATION({rep.config_value or 1}) done")

    for num, cls, _ in rep.interfaces:
        if cls != 3:
            continue
        # The HID descriptor inside the configuration names the report length.
        want = 0
        walk = list(walk_descriptors(config))
        for k, block in enumerate(walk):
            if block[0:2] == bytes([block[0], 0x21]) and len(block) >= 9:
                want = struct.unpack_from("<H", block, 7)[0]
                break
        try:
            host.control(0x21, 0x0a, 0, num, 0)        # SET_IDLE(0)
        except UsbError as error:
            rep.notes.append(f"SET_IDLE: {error}")
        try:
            report = host.control(0x81, 6, 0x22 << 8, num, want or 512)
            rep.hid_report_len[num] = len(report)
            say(f"HID report descriptor (interface {num}): {len(report)} bytes")
        except UsbError as error:
            rep.notes.append(f"HID report descriptor: {error}")
    rep.final_state = bus.status().state
    return host, rep


def poll_reports(host: Host, rep: Report, count: int, seconds: float, say,
                 show_hex: bool = False) -> None:
    ins = [(a & 0x0f, m) for _, _, eps in rep.interfaces
           for a, t, m, _ in eps if a & 0x80 and t in (2, 3)]
    if not ins:
        rep.notes.append("no interrupt or isochronous IN endpoint to read")
        return
    deadline = time.monotonic() + seconds
    while len(rep.reports) < count and time.monotonic() < deadline:
        for ep, mxps in ins:
            result, data = host._retry(OP_IN, ep, length=mxps, timeout=0.2)
            if result == ACK:
                rep.reports.append((0x80 | ep, data))
                text = f"IN {0x80 | ep:02x}: report of {len(data)} bytes"
                if show_hex:
                    text += " " + data.hex()
                say(text)
        time.sleep(0.01)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, required=True,
                        help="the UDP port CDJ_USBB_HOST names")
    parser.add_argument("--reports", type=int, default=3,
                        help="HID reports to read after enumeration (default 3)")
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long to wait for the reports")
    parser.add_argument("--attach-timeout", type=float, default=180.0)
    parser.add_argument("--hex", action="store_true",
                        help="print the report bytes (default: lengths only)")
    parser.add_argument("--json", action="store_true",
                        help="print the outcome as JSON on stdout")
    args = parser.parse_args(argv)

    bus = Bus(args.port)
    say = (lambda text: print(text, file=sys.stderr, flush=True))
    try:
        host, rep = enumerate_deck(bus, say, attach_timeout=args.attach_timeout)
        poll_reports(host, rep, args.reports, args.seconds, say, args.hex)
        status = bus.status()
        rep.final_state = status.state
    except UsbError as error:
        say(f"FAILED: {error}")
        return 1
    finally:
        bus.close()
    if args.json:
        print(json.dumps(rep.as_dict(), indent=2))
    else:
        say(f"done: {len(rep.reports)} report(s), final state {rep.final_state}")
    return 0 if rep.configured and rep.reports else 2


if __name__ == "__main__":
    sys.exit(main())
