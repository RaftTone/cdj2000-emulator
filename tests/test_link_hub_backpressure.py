# SPDX-License-Identifier: GPL-2.0-or-later
"""A deck that is not reading must not stop the hub from serving the others.

A deck reads its socket only when its own network timer fires, and a deck held by gdb (or busy in a long
guest call) does not read at all.  The hub used to answer with a blocking sendall(), so one full socket stopped
the whole segment: the hub could not read the next deck, that deck blocked in its own send, and the stand stood
at 0 % CPU with the guest clocks frozen (the two-deck LINK soak, after a handful of NFS loads).
"""

import socket
import struct
import threading
import time

import pytest

from tools.cdj_main import link_hub

FLOOD = 3000                    # frames of 1400 bytes: more than any socket buffer holds
MARKER = bytes.fromhex("02000000000c")


def frame(src: bytes, size: int = 1400) -> bytes:
    return b"\xff" * 6 + src + b"\x88\xb5" + bytes(size - 14)


def plain(fr: bytes, t: int = 0) -> bytes:
    return struct.pack("!I", len(fr)) + fr


def synced(fr: bytes, t: int = 0, kind: int = link_hub.SYNC_FRAME) -> bytes:
    return b"CDJC" + bytes([kind, 0, 0, 0]) + struct.pack("<QI", t, len(fr)) + fr


def plain_frames(buffer: bytearray):
    while len(buffer) >= 4:
        size = struct.unpack_from("!I", buffer)[0]
        if len(buffer) < 4 + size:
            return
        yield bytes(buffer[4:4 + size])
        del buffer[:4 + size]


def synced_frames(buffer: bytearray):
    while len(buffer) >= link_hub.SYNC_HEADER:
        kind = buffer[4]
        size = struct.unpack_from("<I", buffer, 16)[0]
        if len(buffer) < link_hub.SYNC_HEADER + size:
            return
        body = bytes(buffer[link_hub.SYNC_HEADER:link_hub.SYNC_HEADER + size])
        del buffer[:link_hub.SYNC_HEADER + size]
        if kind == link_hub.SYNC_FRAME:
            yield body


@pytest.fixture(params=["plain", "sync"])
def segment(request, tmp_path):
    sync = request.param == "sync"
    hub = (link_hub.SyncHub(tmp_path, "127.0.0.1:0", 3, None, 0.0) if sync
           else link_hub.Hub(tmp_path, "127.0.0.1:0"))
    port = hub.server.getsockname()[1]
    stop = threading.Event()

    def serve():
        while not stop.is_set():
            hub.poll(0.02)
            if sync:
                hub.update_grants()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    socks = []

    def connect(small_buffer=False):
        s = socket.socket()
        if small_buffer:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.connect(("127.0.0.1", port))
        if sync:
            s.sendall(synced(b"", kind=link_hub.SYNC_HELLO))
        socks.append(s)
        return s

    yield sync, connect
    for s in socks:
        s.close()
    stop.set()
    thread.join(5)
    hub.close()


def test_a_deck_that_does_not_read_does_not_stop_the_others(segment):
    sync, connect = segment
    wrap, unwrap = (synced, synced_frames) if sync else (plain, plain_frames)
    a, c = connect(), connect()
    b = connect(small_buffer=True)                      # never reads until the end
    flood = b"".join(wrap(frame(bytes.fromhex("02000000000a"))) for _ in range(FLOOD))
    sender = threading.Thread(target=lambda: a.sendall(flood), daemon=True)
    sender.start()
    sender.join(10)
    assert not sender.is_alive(), "the hub stopped reading the flooding deck"

    c.sendall(wrap(frame(MARKER, 100)))
    a.settimeout(5)
    seen, buffer = False, bytearray()
    while not seen:
        buffer.extend(a.recv(65536))
        seen = any(fr[6:12] == MARKER for fr in unwrap(buffer))
    assert seen

    # nothing was dropped on the way: the deck that did not read gets every frame
    b.settimeout(10)
    got, buffer = [], bytearray()
    while len(got) < FLOOD + 1:
        buffer.extend(b.recv(65536))
        got.extend(fr[6:12] for fr in unwrap(buffer))
    assert got.count(bytes.fromhex("02000000000a")) == FLOOD and MARKER in got
