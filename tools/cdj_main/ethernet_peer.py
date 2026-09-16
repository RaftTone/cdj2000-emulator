"""Isolated QEMU socket-netdev peer; no host interfaces or forwarding.

ARP follows RFC 826; ICMP echo follows RFC 792. Only untagged Ethernet,
IPv4 without options/fragmentation, ARP and echo are supported. Optional DHCP
SELECTING is an isolated single-client fixture. PTP,
Dante subscription and audio validation are deliberately absent.
Run: python -m tools.cdj_main.ethernet_peer NEW_DIRECTORY --seconds 600
Read endpoint.json, then give QEMU -netdev socket,id=net0,connect=127.0.0.1:PORT.
Captures are JSONL raw frames; timestamps are host observations, not wire time.
"""
from __future__ import annotations

import argparse
import ipaddress
import json
from pathlib import Path
import socket
import struct
import time

MAX_FRAME = 1514
MAC = bytes.fromhex("020000000001")


def checksum(data: bytes) -> int:
    data += b"\0" * (len(data) % 2)
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return total ^ 65535


class Framing:
    """Incremental legacy QEMU TCP framing, bounded before accepting payload."""
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        self.buffer.extend(data)
        frames = []
        while len(self.buffer) >= 4:
            size = struct.unpack_from("!I", self.buffer)[0]
            if not 14 <= size <= MAX_FRAME:
                raise ValueError(f"unsupported Ethernet frame length {size}")
            if len(self.buffer) < size + 4:
                break
            frames.append(bytes(self.buffer[4:4 + size]))
            del self.buffer[:4 + size]
        return frames

    def finish(self):
        if self.buffer:
            raise ValueError("truncated QEMU frame stream")


def respond(frame: bytes, address: bytes) -> bytes | None:
    if not 14 <= len(frame) <= MAX_FRAME or len(address) != 4:
        return None
    dst, src, kind = frame[:6], frame[6:12], frame[12:14]
    if src[0] & 1 or src == bytes(6):
        return None
    if kind == b"\x08\x06" and dst in (MAC, b"\xff" * 6):
        arp = frame[14:42]
        if (len(arp) != 28 or arp[:8] != bytes.fromhex("0001080006040001")
                or arp[8:14] != src or arp[24:28] != address):
            return None
        payload = bytes.fromhex("0001080006040002") + MAC + address + src + arp[14:18]
        return (src + MAC + kind + payload).ljust(60, b"\0")
    if kind != b"\x08\x00" or dst != MAC or len(frame) < 42:
        return None
    ip = frame[14:]
    length = int.from_bytes(ip[2:4], "big")
    if (ip[0] != 0x45 or not 28 <= length <= len(ip) or checksum(ip[:20])
            or ip[9] != 1 or ip[16:20] != address or ip[8] == 0
            or int.from_bytes(ip[6:8], "big") & 0xbfff):
        return None
    source = ipaddress.IPv4Address(bytes(ip[12:16]))
    if source.is_multicast or source.is_unspecified or int(source) == 0xffffffff:
        return None
    echo = bytearray(ip[20:length])
    if echo[:2] != b"\x08\0" or checksum(echo):
        return None
    echo[:4] = b"\0" * 4
    struct.pack_into("!H", echo, 2, checksum(echo))
    header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(echo),
                                   0, 0, 64, 1, 0, address, ip[12:16]))
    struct.pack_into("!H", header, 10, checksum(header))
    return (src + MAC + kind + header + echo).ljust(60, b"\0")


def serve(directory: Path, seconds: float, address: str, dhcp_lease=None):
    ip = ipaddress.IPv4Address(address)
    if ip.is_multicast or ip.is_unspecified or int(ip) == 0xffffffff:
        raise ValueError("peer must have a unicast IPv4 address")
    if not 0 < seconds <= 86400:
        raise ValueError("seconds must be in (0, 86400]")
    from tools.cdj_main.dhcp_peer import DhcpPeer
    dhcp = DhcpPeer(address, dhcp_lease) if dhcp_lease else None
    directory.mkdir(parents=True, exist_ok=False)
    counts = {"received": 0, "sent": 0, "unsupported_or_invalid": 0}
    deadline = time.monotonic() + seconds
    parser = Framing()
    with socket.socket() as listener, (directory / "frames.jsonl").open("x") as capture:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        endpoint = {"host": "127.0.0.1", "port": listener.getsockname()[1],
                    "ip": str(ip), "mac": MAC.hex(), "protocols": ["ARP", "ICMP echo"]}
        if dhcp:
            endpoint['protocols'].append('DHCP SELECTING')
            endpoint['dhcp'] = dict(lease=dhcp_lease, prefix=24, lease_seconds=0xffffffff,
                                    scope='single client; no routing/DNS/renewal or guest clock model')
        (directory / "endpoint.json").write_text(json.dumps(endpoint) + "\n")
        print(json.dumps(endpoint), flush=True)
        listener.settimeout(seconds)
        try:
            conn, _ = listener.accept()
            with conn:
                while time.monotonic() < deadline:
                    conn.settimeout(min(1, max(.001, deadline - time.monotonic())))
                    try:
                        data = conn.recv(4096)
                    except socket.timeout:
                        continue
                    if not data:
                        parser.finish()
                        break
                    for frame in parser.feed(data):
                        counts["received"] += 1
                        capture.write(json.dumps({"direction": "guest-to-peer", "time_ns": time.time_ns(), "hex": frame.hex()}) + "\n")
                        reply = respond(frame, ip.packed)
                        if reply is None and dhcp:
                            reply = dhcp.respond(frame)
                        if reply is None:
                            counts["unsupported_or_invalid"] += 1
                        else:
                            conn.sendall(struct.pack("!I", len(reply)) + reply)
                            counts["sent"] += 1
                            capture.write(json.dumps({"direction": "peer-to-guest", "time_ns": time.time_ns(), "hex": reply.hex()}) + "\n")
                        capture.flush()
        except socket.timeout:
            pass
        finally:
            if dhcp:
                (directory / 'dhcp.json').write_text(json.dumps(dhcp.events, indent=2) + '\n')
            (directory / "summary.json").write_text(json.dumps(counts) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--seconds", type=float, default=600)
    parser.add_argument("--ip", default="192.168.42.1")
    parser.add_argument('--dhcp-lease', help='enable isolated DHCP SELECTING with one reserved /24 address')
    args = parser.parse_args()
    serve(args.directory, args.seconds, args.ip, args.dhcp_lease)


if __name__ == "__main__":
    main()
