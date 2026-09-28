"""A Pro DJ Link segment for emulated decks: a hub, a capture, and a replayer.

    python -m tools.cdj_main.link_hub DIR --listen 6580 [--seconds 600]
    python -m tools.cdj_main.link_hub DIR --listen unix:DIR/hub.sock \\
        --replay CAPTURE.pcap [--replay-from 169.254.190.249] [--replay-loop]

Each deck's QEMU connects to it with the framed stream QEMU's socket and stream
netdevs speak (a big-endian 32-bit length, then the Ethernet frame); boot_vm
--link-hub does that. Every frame a deck sends goes to every other deck, as on
a hub or a switch that floods: Pro DJ Link is broadcast, so that is all it
needs. Nothing reaches the host's network.

--replay puts a real capture's Pro DJ Link traffic (UDP 50000, 50001, 50002)
onto the segment, frame for frame and with the capture's own spacing, as if the
decks that sent it were plugged in. Unicast frames in the capture (a player's
status, 50002, goes to each device it knows) are re-addressed to the first
emulated deck that has announced itself, and dropped before one has; the hub
answers a deck's ARP for a replayed sender's address on its behalf. It starts when the first deck connects
(after --replay-delay seconds), so one emulated deck can see a real player's
keep-alives, beats and status without a second emulator.

Timing is the host's. A deck under --cosim runs on its instruction count, so
its guest seconds and these seconds drift apart by the machine's real-time
factor; summary.json records both ends' counts so a run can be judged.

DIR gets link.pcap (every frame on the segment, host timestamps, pcap format
for Wireshark), events.jsonl (connections, the first frame of each kind from
each sender) and summary.json (per sender: MAC, IP, device name, player number,
and counts per port and packet type).
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
import os
import selectors
import signal
import socket
import struct
import sys
import time
from pathlib import Path

MAX_FRAME = 1514
PRODJ_MAGIC = b"Qspt1WmJOL"
PRODJ_PORTS = (50000, 50001, 50002)


def pcap_header() -> bytes:
    return struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)


def pcap_record(stamp: float, frame: bytes) -> bytes:
    seconds = int(stamp)
    return struct.pack("<IIII", seconds, int((stamp - seconds) * 1e6),
                       len(frame), len(frame)) + frame


def read_pcap(path: Path) -> list[tuple[float, bytes]]:
    """Classic pcap (micro- or nanosecond, either byte order), Ethernet only."""
    data = path.read_bytes()
    magic = data[:4]
    orders = {b"\xd4\xc3\xb2\xa1": ("<", 1e-6), b"\xa1\xb2\xc3\xd4": (">", 1e-6),
              b"\x4d\x3c\xb2\xa1": ("<", 1e-9), b"\xa1\xb2\x3c\x4d": (">", 1e-9)}
    if magic not in orders:
        raise SystemExit(f"{path}: not a classic pcap file (pcapng is not read)")
    order, unit = orders[magic]
    if struct.unpack_from(order + "I", data, 20)[0] != 1:
        raise SystemExit(f"{path}: not an Ethernet capture")
    frames, offset = [], 24
    while offset + 16 <= len(data):
        seconds, fraction, included, _ = struct.unpack_from(order + "IIII", data, offset)
        offset += 16
        frames.append((seconds + fraction * unit, data[offset:offset + included]))
        offset += included
    return frames


def udp(frame: bytes):
    """(source IP, destination port, payload) of an IPv4 UDP frame, else None."""
    if len(frame) < 42 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
        return None
    header = (frame[14] & 15) * 4
    if len(frame) < 14 + header + 8:
        return None
    _, port = struct.unpack_from("!HH", frame, 14 + header)
    return socket.inet_ntoa(frame[26:30]), port, frame[14 + header + 8:]


def describe(frame: bytes) -> dict:
    """What a frame is, as far as the summary cares."""
    kind = {"src_mac": frame[6:12].hex(":"), "ethertype": frame[12:14].hex()}
    parsed = udp(frame)
    if parsed is None:
        if frame[12:14] == b"\x08\x06" and len(frame) >= 42:
            kind["arp"] = {"op": frame[21], "sender_ip": socket.inet_ntoa(frame[28:32]),
                           "target_ip": socket.inet_ntoa(frame[38:42])}
        return kind
    ip, port, payload = parsed
    kind.update(ip=ip, port=port)
    if payload[:10] == PRODJ_MAGIC and len(payload) > 11:
        kind["type"] = payload[10]
        # The device name is at 0x0c on port 50000 (after a zero byte) and at
        # 0x0b on 50001/50002; the player number at 0x24 in a keep-alive and
        # at 0x21 in a beat or status packet.
        at = 0x0c if port == 50000 else 0x0b
        kind["name"] = payload[at:at + 20].split(b"\0")[0].decode("latin-1")
        if port == 50000 and payload[10] == 0x06 and len(payload) > 0x24:
            kind["player"] = payload[0x24]
        elif port in (50001, 50002) and len(payload) > 0x21:
            kind["player"] = payload[0x21]
    return kind


class Hub:
    def __init__(self, out: Path, listen: str):
        self.out = out
        self.sel = selectors.DefaultSelector()
        self.clients: dict[socket.socket, dict] = {}
        self.pcap = open(out / "link.pcap", "wb")
        self.pcap.write(pcap_header())
        self.events = open(out / "events.jsonl", "a")
        self.senders: dict[str, dict] = {}
        self.seen: set = set()
        self.started = time.time()
        self.next_id = 1
        if listen.startswith("unix:"):
            path = listen[5:]
            if os.path.exists(path):
                os.unlink(path)
            self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.server.bind(path)
        else:
            host, _, port = listen.rpartition(":")
            self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server.bind((host or "127.0.0.1", int(port)))
        self.server.listen(8)
        self.server.setblocking(False)
        self.sel.register(self.server, selectors.EVENT_READ, "accept")
        self.listen = listen

    def event(self, what: str, **fields) -> None:
        record = dict(t=round(time.time() - self.started, 3), event=what, **fields)
        self.events.write(json.dumps(record) + "\n")
        self.events.flush()
        print(json.dumps(record), flush=True)

    def accept(self) -> None:
        conn, _ = self.server.accept()
        conn.setblocking(False)
        client = {"id": self.next_id, "buffer": bytearray(), "rx": 0, "tx": 0}
        self.next_id += 1
        self.clients[conn] = client
        self.sel.register(conn, selectors.EVENT_READ, "deck")
        self.event("connect", deck=client["id"])

    def drop(self, conn: socket.socket) -> None:
        client = self.clients.pop(conn)
        self.sel.unregister(conn)
        conn.close()
        self.event("disconnect", deck=client["id"], frames_in=client["rx"],
                   frames_out=client["tx"])

    def players(self) -> int:
        """Devices that have sent a keep-alive, replayed or emulated."""
        return sum(1 for sender in self.senders.values() if "50000/0x06" in sender["kinds"])

    def first_deck_address(self) -> tuple[bytes, bytes] | None:
        """MAC and IP of the first emulated deck that has sent a keep-alive."""
        for mac, sender in self.senders.items():
            if sender["origin"].startswith("deck") and sender.get("ip", "0.0.0.0") != "0.0.0.0" \
                    and "50000/0x06" in sender["kinds"]:
                return bytes.fromhex(mac.replace(":", "")), socket.inet_aton(sender["ip"])
        return None

    def record(self, frame: bytes, origin: str, guest_ns: int | None = None) -> None:
        # --sync captures on the decks' common guest time, else the host's.
        stamp = guest_ns / 1e9 if guest_ns is not None else time.time()
        self.pcap.write(pcap_record(stamp, frame))
        self.pcap.flush()           # readable while the run goes on
        kind = describe(frame)
        sender = self.senders.setdefault(kind["src_mac"], {"origin": origin, "frames": 0,
                                                           "kinds": {}})
        sender["frames"] += 1
        for field in ("ip", "name", "player"):
            if field in kind:
                sender[field] = kind[field]
        label = (f"{kind['port']}/0x{kind['type']:02x}" if "type" in kind
                 else f"udp{kind['port']}" if "port" in kind
                 else "arp" if "arp" in kind else kind["ethertype"])
        sender["kinds"][label] = sender["kinds"].get(label, 0) + 1
        key = (kind["src_mac"], label)
        if key not in self.seen:
            self.seen.add(key)
            self.event("first", origin=origin, kind=label,
                       **{k: v for k, v in kind.items() if k != "ethertype"})

    def send_all(self, frame: bytes, exclude=None) -> None:
        # The wire's minimum: the EtherC flags a shorter frame as a runt
        # (RFS2) and MAIN drops it, which is how the first proxy ARP replies,
        # 42 bytes, went unheard.
        frame = frame.ljust(60, b"\0")
        blob = struct.pack("!I", len(frame)) + frame
        for conn, client in list(self.clients.items()):
            if conn is exclude:
                continue
            try:
                conn.setblocking(True)
                conn.sendall(blob)
                conn.setblocking(False)
                client["tx"] += 1
            except OSError:
                self.drop(conn)

    def read(self, conn: socket.socket) -> None:
        client = self.clients[conn]
        try:
            data = conn.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            data = b""
        if not data:
            self.drop(conn)
            return
        buffer = client["buffer"]
        buffer.extend(data)
        while len(buffer) >= 4:
            size = struct.unpack_from("!I", buffer)[0]
            if not 14 <= size <= MAX_FRAME:
                self.event("bad-frame", deck=client["id"], size=size)
                self.drop(conn)
                return
            if len(buffer) < 4 + size:
                break
            frame = bytes(buffer[4:4 + size])
            del buffer[:4 + size]
            client["rx"] += 1
            self.record(frame, f"deck{client['id']}")
            self.send_all(frame, exclude=conn)
            reply = self.proxy_arp(frame)
            if reply is not None:
                self.record(reply, "replay")
                self.send_all(reply)

    def proxy_arp(self, frame: bytes) -> bytes | None:
        """An ARP reply for a replayed sender's IP. A recording cannot answer,
        and without an answer a deck never sends the unicast status (50002)
        a player sends to every other player it knows."""
        if frame[12:14] != b"\x08\x06" or len(frame) < 42 or frame[20:22] != b"\x00\x01":
            return None
        wanted = socket.inet_ntoa(frame[38:42])
        for mac, sender in self.senders.items():
            if sender["origin"] == "replay" and sender.get("ip") == wanted:
                own = bytes.fromhex(mac.replace(":", ""))
                return (frame[6:12] + own + b"\x08\x06" + frame[14:20] + b"\x00\x02"
                        + own + frame[38:42] + frame[22:28] + frame[28:32])
        return None

    def summary(self) -> dict:
        return {"listen": self.listen, "host_seconds": round(time.time() - self.started, 3),
                "decks": {c["id"]: {"in": c["rx"], "out": c["tx"]} for c in self.clients.values()},
                "senders": self.senders}

    def close(self) -> None:
        (self.out / "summary.json").write_text(json.dumps(self.summary(), indent=2) + "\n")
        self.pcap.close()
        self.events.close()


# Where a packet names its own player (dysentery's packet analysis, checked on
# the owner's captures): keep-alive 0x24; beat 0x21 and 0x5f; status 0x21,
# 0x24, and 0x28 for the player the track came from.
PLAYER_FIELDS = {(50000, 0x06): (0x24,), (50001, 0x28): (0x21, 0x5F),
                 (50002, 0x0A): (0x21, 0x24, 0x28)}


def renumber(frame: bytes, old: int, new: int) -> bytes:
    """FRAME with player OLD called NEW, for a capture whose player numbers
    would collide with an emulated deck's. UDP checksums are zeroed, which
    IPv4 allows and MAIN accepts."""
    parsed = udp(frame)
    if parsed is None:
        return frame
    _, port, payload = parsed
    fields = PLAYER_FIELDS.get((port, payload[10] if len(payload) > 10 else -1), ())
    start = len(frame) - len(payload)
    out = bytearray(frame)
    for field in fields:
        if field < len(payload) and payload[field] == old:
            out[start + field] = new
    if out != frame:
        out[start - 2:start] = b"\0\0"
    return bytes(out)


def set_peer_count(frame: bytes, count: int) -> bytes:
    """A keep-alive (50000, 0x06) that counts COUNT players on the link.

    Byte 0x30 is how many devices the sender sees, itself included (2 and 2
    for two emulated decks, runs/link/s3). A recording keeps saying what it
    saw when it was made, which leaves out the emulated deck, and the deck
    answers with its number claim (0x04) again and again (s4: a burst every
    2-5 guest seconds, whether the replay was paced on host or guest time)."""
    parsed = udp(frame)
    if parsed is None or parsed[1] != 50000 or len(parsed[2]) <= 0x30 \
            or parsed[2][10] != 0x06 or parsed[2][0x30] == count:
        return frame
    start = len(frame) - len(parsed[2])
    out = bytearray(frame)
    out[start + 0x30] = count
    out[start - 2:start] = b"\0\0"
    return bytes(out)


def ip_checksum(header: bytes) -> int:
    total = sum(struct.unpack(f"!{len(header) // 2}H", header))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total ^ 0xFFFF


def readdress(frame: bytes, mac: bytes, ip: bytes) -> bytes:
    """A unicast FRAME re-addressed to MAC/IP. The capture's status packets
    (50002) went to the capturing device alone; a real player sends the same
    to every player it knows, so the emulated deck gets them as its own."""
    if frame[0] & 1 or frame[12:14] != b"\x08\x00":
        return frame
    out = bytearray(frame)
    out[0:6] = mac
    out[30:34] = ip
    header = (out[14] & 15) * 4
    out[24:26] = b"\0\0"
    out[24:26] = struct.pack("!H", ip_checksum(bytes(out[14:14 + header])))
    udp_at = 14 + header
    out[udp_at + 6:udp_at + 8] = b"\0\0"       # the UDP checksum covered the old IP
    return bytes(out)


class Replay:
    """A capture's Pro DJ Link frames, due at the capture's own spacing."""

    def __init__(self, path: Path, sources: list[str], loop: bool, speed: float,
                 numbers: dict[int, int] | None = None, ports=PRODJ_PORTS):
        frames = []
        for stamp, frame in read_pcap(path):
            parsed = udp(frame)
            if parsed is None or parsed[1] not in ports:
                continue
            if sources and parsed[0] not in sources:
                continue
            for old, new in (numbers or {}).items():
                frame = renumber(frame, old, new)
            frames.append((stamp, frame))
        if not frames:
            raise SystemExit(f"{path}: no Pro DJ Link frames to replay")
        base = frames[0][0]
        self.frames = [((stamp - base) / speed, frame) for stamp, frame in frames]
        self.span = self.frames[-1][0] + 1.0 / speed
        self.loop = loop
        self.start = None
        self.index = 0
        self.laps = 0
        self.sent = 0

    def begin(self, now: float) -> None:
        self.start = now

    def due(self, now: float) -> list[bytes]:
        out = []
        while self.start is not None:
            if self.index == len(self.frames):
                if not self.loop:
                    self.start = None
                    break
                self.index = 0
                self.laps += 1
                self.start += self.span
            offset, frame = self.frames[self.index]
            if self.start + offset > now:
                break
            out.append(frame)
            self.index += 1
        self.sent += len(out)
        return out

    def next_time(self) -> float | None:
        if self.start is None or self.index == len(self.frames):
            return None
        return self.start + self.frames[self.index][0]


SYNC_HEADER = 20
SYNC_TIME, SYNC_HELLO, SYNC_FRAME = 1, 4, 5
NEVER = 1 << 62


class SyncHub(Hub):
    """--sync: the decks on one guest timeline (emulator/qemu/cdj2000_netsim.c).

    Each deck reports its guest time and a promise (it sends nothing stamped
    earlier); every frame is stamped with the guest time it was sent. A deck
    is granted the least promise of the others, so it never runs more than the
    segment latency past a frame that could still reach it. Replayed frames
    are stamped on the same guest timeline: --replay-delay guest seconds after
    guest time 0, the capture's spacing after that.
    """

    def __init__(self, out: Path, listen: str, decks: int, replay: "Replay | None",
                 replay_delay: float):
        super().__init__(out, listen)
        self.decks_wanted = decks
        self.replay = replay
        self.replay_origin = int(replay_delay * 1e9)
        self.replay_index = 0
        self.replay_lap = 0
        self.replay_sent = 0

    def accept(self) -> None:
        super().accept()
        conn = next(reversed(self.clients))
        self.clients[conn].update(t=0, promise=0, granted=-1, joined=False)

    def send_msg(self, conn: socket.socket, kind: int, t: int, payload: bytes = b"") -> None:
        client = self.clients.get(conn)
        if client is None:
            return
        try:
            conn.setblocking(True)
            conn.sendall(b"CDJC" + bytes([kind, 0, 0, 0]) + struct.pack("<QI", t, len(payload))
                         + payload)
            conn.setblocking(False)
        except OSError:
            self.drop(conn)
            return
        if kind == SYNC_FRAME:
            client["tx"] += 1

    def send_frame(self, conn: socket.socket, t: int, frame: bytes) -> None:
        self.send_msg(conn, SYNC_FRAME, t, frame.ljust(60, b"\0"))

    def read(self, conn: socket.socket) -> None:
        client = self.clients[conn]
        try:
            data = conn.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            data = b""
        if not data:
            self.drop(conn)
            return
        buffer = client["buffer"]
        buffer.extend(data)
        while len(buffer) >= SYNC_HEADER:
            if buffer[:4] != b"CDJC":
                self.event("bad-message", deck=client["id"])
                self.drop(conn)
                return
            kind = buffer[4]
            t, size = struct.unpack_from("<QI", buffer, 8)
            if len(buffer) < SYNC_HEADER + size:
                break
            payload = bytes(buffer[SYNC_HEADER:SYNC_HEADER + size])
            del buffer[:SYNC_HEADER + size]
            self.message(conn, client, kind, t, payload)
            if conn not in self.clients:
                return

    def message(self, conn, client, kind: int, t: int, payload: bytes) -> None:
        client["t"] = max(client["t"], t)
        if kind == SYNC_HELLO:
            client["joined"] = True
            self.event("hello", deck=client["id"],
                       joined=sum(1 for c in self.clients.values() if c.get("joined")))
        elif kind == SYNC_TIME and len(payload) == 8:
            client["promise"] = max(client["promise"], struct.unpack("<Q", payload)[0])
        elif kind == SYNC_FRAME:
            # A deck's own stream is in time order: nothing of its is earlier.
            client["promise"] = max(client["promise"], t)
            client["rx"] += 1
            self.record(payload, f"deck{client['id']}", guest_ns=t)
            for other in list(self.clients):
                if other is not conn:
                    self.send_frame(other, t, payload)
            reply = self.proxy_arp(payload)
            if reply is not None:
                stamp = max(t, client["granted"])
                self.record(reply, "replay", guest_ns=stamp)
                self.send_frame(conn, stamp, reply)

    def push_replay(self) -> None:
        """Replayed frames up to a second of guest time ahead of the decks."""
        if self.replay is None or not self.clients:
            return
        horizon = max(c["t"] for c in self.clients.values()) + 1_000_000_000
        target = self.first_deck_address()
        while True:
            due = self.replay_due()
            if due is None or due > horizon:
                return
            frame = self.replay.frames[self.replay_index][1]
            self.replay_index += 1
            if self.replay_index == len(self.replay.frames) and self.replay.loop:
                self.replay_index = 0
                self.replay_lap += 1
            frame = set_peer_count(frame, self.players())
            if target is not None:
                frame = readdress(frame, *target)
            elif not frame[0] & 1:
                continue            # unicast to a device that has not announced itself
            self.replay_sent += 1
            self.record(frame, "replay", guest_ns=due)
            for conn in list(self.clients):
                self.send_frame(conn, due, frame)

    def replay_due(self) -> int | None:
        if self.replay is None or self.replay_index >= len(self.replay.frames):
            return None
        offset = self.replay.frames[self.replay_index][0] + self.replay_lap * self.replay.span
        return self.replay_origin + int(offset * 1e9)

    def grant(self, conn) -> int:
        joined = [c for c in self.clients.values() if c.get("joined")]
        if len(joined) < self.decks_wanted:
            return 0                # hold everyone at the start until all are in
        grant = NEVER
        for other, client in self.clients.items():
            if other is not conn and client.get("joined"):
                grant = min(grant, client["promise"])
        due = self.replay_due()
        if due is not None:
            grant = min(grant, due)
        return grant

    def update_grants(self) -> None:
        for conn in list(self.clients):
            client = self.clients.get(conn)
            if client is None:
                continue
            grant = self.grant(conn)
            if grant > client["granted"]:
                client["granted"] = grant
                self.send_msg(conn, SYNC_TIME, grant, struct.pack("<Q", grant))

    def summary(self) -> dict:
        summary = super().summary()
        summary["sync"] = {c["id"]: {"guest_s": round(c.get("t", 0) / 1e9, 6),
                                     "frames_in": c["rx"], "frames_out": c["tx"]}
                           for c in self.clients.values()}
        if self.replay is not None:
            summary["replay"] = {"frames_sent": self.replay_sent, "laps": self.replay_lap}
        return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out", type=Path)
    parser.add_argument("--listen", required=True,
                        help="PORT, HOST:PORT or unix:PATH for the decks to connect to")
    parser.add_argument("--seconds", type=float, default=3600)
    parser.add_argument("--replay", type=Path, help="a pcap of real Pro DJ Link traffic")
    parser.add_argument("--replay-from", action="append", default=[], metavar="IP",
                        help="replay only what this sender sent (repeatable)")
    parser.add_argument("--replay-delay", type=float, default=0.0,
                        help="host seconds after the first deck connects "
                             "(with --sync: guest seconds after guest time 0)")
    parser.add_argument("--sync", action="store_true",
                        help="hold the decks on one guest timeline: they connect "
                             "with boot_vm --link-hub sync:PORT (CDJ_NETSIM)")
    parser.add_argument("--decks", type=int, default=1,
                        help="with --sync, hold every deck at the start until "
                             "this many have joined")
    parser.add_argument("--replay-loop", action="store_true")
    parser.add_argument("--replay-speed", type=float, default=1.0)
    parser.add_argument("--replay-ports", default="50000,50001,50002",
                        help="the UDP ports to replay, comma-separated")
    parser.add_argument("--replay-renumber", action="append", default=[], metavar="OLD:NEW",
                        help="call the capture's player OLD player NEW (repeatable), "
                             "so it does not collide with an emulated deck")
    args = parser.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    listen = args.listen if ":" in args.listen else f"127.0.0.1:{args.listen}"
    numbers = dict(tuple(int(x) for x in item.split(":")) for item in args.replay_renumber)
    ports = tuple(int(x) for x in args.replay_ports.split(","))
    replay = (Replay(args.replay, args.replay_from, args.replay_loop, args.replay_speed, numbers,
                     ports) if args.replay else None)
    hub = (SyncHub(args.out, listen, args.decks, replay, args.replay_delay) if args.sync
           else Hub(args.out, listen))
    (args.out / "endpoint.json").write_text(json.dumps({"listen": listen, "pid": os.getpid()}) + "\n")
    hub.event("listening", listen=listen,
              replay=(str(args.replay) if replay else None),
              replay_frames=(len(replay.frames) if replay else 0))
    deadline = time.time() + args.seconds

    def stop(*_):
        raise KeyboardInterrupt
    # A kill by PID still writes summary.json.
    signal.signal(signal.SIGTERM, stop)
    try:
        while args.sync and time.time() < deadline:
            for key, _ in hub.sel.select(0.2):
                if key.data == "accept":
                    hub.accept()
                else:
                    hub.read(key.fileobj)
            hub.push_replay()
            hub.update_grants()
        while not args.sync and time.time() < deadline:
            timeout = 0.5
            if replay is not None:
                if replay.start is None and hub.clients and replay.sent == 0:
                    replay.begin(time.time() + args.replay_delay)
                    hub.event("replay-start", delay=args.replay_delay)
                due_at = replay.next_time()
                if due_at is not None:
                    timeout = max(0.0, min(timeout, due_at - time.time()))
            for key, _ in hub.sel.select(timeout):
                if key.data == "accept":
                    hub.accept()
                else:
                    hub.read(key.fileobj)
            if replay is not None:
                target = hub.first_deck_address()
                for frame in replay.due(time.time()):
                    frame = set_peer_count(frame, hub.players())
                    if target is not None:
                        frame = readdress(frame, *target)
                    elif not frame[0] & 1:
                        continue        # unicast to a device that is not here yet
                    hub.record(frame, "replay")
                    hub.send_all(frame)
    except KeyboardInterrupt:
        pass
    finally:
        summary = hub.summary()
        if replay is not None and not args.sync:
            summary["replay"] = {"frames_sent": replay.sent, "laps": replay.laps}
        hub.close()
        (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
