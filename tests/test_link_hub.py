# SPDX-License-Identifier: GPL-2.0-or-later
"""link_hub's packet helpers and boot_vm's flash MAC record, on synthetic frames."""

import socket
import struct

from tools.cdj_main import link_hub
from tools.cdj_main.boot_vm import FLASH_SIZE, MAC_SECTOR, flash_with_mac

SRC_MAC = bytes.fromhex("00e036d1bef9")
DECK_MAC = bytes.fromhex("020000000002")


def udp_frame(port, payload, dst_mac=b"\xff" * 6, dst_ip="169.254.255.255"):
    udp = struct.pack("!HHHH", port, port, 8 + len(payload), 0x1234) + payload
    ip = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                               socket.inet_aton("169.254.190.249"), socket.inet_aton(dst_ip)))
    ip[10:12] = struct.pack("!H", link_hub.ip_checksum(bytes(ip)))
    return dst_mac + SRC_MAC + b"\x08\x00" + bytes(ip) + udp


def prodj(kind, size, fields):
    payload = bytearray(link_hub.PRODJ_MAGIC + bytes([kind]) + b"\0" + b"CDJ-2000".ljust(20, b"\0"))
    payload += b"\0" * (size - len(payload))
    for offset, value in fields.items():
        payload[offset] = value
    return bytes(payload)


def test_describe_names_and_players():
    keepalive = udp_frame(50000, prodj(0x06, 0x36, {0x24: 1}))
    kind = link_hub.describe(keepalive)
    assert kind["port"] == 50000 and kind["type"] == 6
    assert kind["name"] == "CDJ-2000" and kind["player"] == 1
    status = udp_frame(50002, link_hub.PRODJ_MAGIC + b"\x0a" + b"CDJ-2000".ljust(20, b"\0")
                       + b"\0" * 0x20)
    assert link_hub.describe(status)["name"] == "CDJ-2000"


def test_renumber_only_player_fields():
    status = udp_frame(50002, prodj(0x0A, 0xD0, {0x21: 1, 0x24: 1, 0x28: 1, 0x2A: 1}))
    moved = link_hub.udp(link_hub.renumber(status, 1, 2))[2]
    assert (moved[0x21], moved[0x24], moved[0x28], moved[0x2A]) == (2, 2, 2, 1)
    beat = udp_frame(50001, prodj(0x28, 0x60, {0x21: 1, 0x5F: 1, 0x26: 1}))
    moved = link_hub.udp(link_hub.renumber(beat, 1, 2))[2]
    assert (moved[0x21], moved[0x5F], moved[0x26]) == (2, 2, 1)
    other = udp_frame(50000, prodj(0x06, 0x36, {0x24: 7}))
    assert link_hub.renumber(other, 1, 2) == other


def test_readdress_keeps_a_valid_ip_header():
    unicast = udp_frame(50002, prodj(0x0A, 0xD0, {}), dst_mac=bytes(6), dst_ip="169.254.17.231")
    out = link_hub.readdress(unicast, DECK_MAC, socket.inet_aton("169.254.0.2"))
    assert out[0:6] == DECK_MAC and out[30:34] == socket.inet_aton("169.254.0.2")
    assert link_hub.ip_checksum(out[14:34]) == 0
    broadcast = udp_frame(50001, prodj(0x28, 0x60, {}))
    assert link_hub.readdress(broadcast, DECK_MAC, b"\0" * 4) == broadcast


def test_proxy_arp_answers_for_replayed_senders(tmp_path):
    hub = link_hub.Hub(tmp_path, "127.0.0.1:0")
    try:
        hub.record(udp_frame(50000, prodj(0x06, 0x36, {0x24: 2})), "replay")
        request = (b"\xff" * 6 + DECK_MAC + b"\x08\x06" + bytes.fromhex("0001080006040001")
                   + DECK_MAC + socket.inet_aton("169.254.0.2") + bytes(6)
                   + socket.inet_aton("169.254.190.249"))
        reply = hub.proxy_arp(request)
        assert reply[0:6] == DECK_MAC and reply[20:22] == b"\x00\x02"
        assert reply[22:28] == SRC_MAC and reply[28:32] == socket.inet_aton("169.254.190.249")
        request = request[:38] + socket.inet_aton("169.254.9.9")
        assert hub.proxy_arp(request) is None
    finally:
        hub.close()


def test_pcap_round_trip(tmp_path):
    frame = udp_frame(50001, prodj(0x28, 0x60, {0x21: 1}))
    path = tmp_path / "x.pcap"
    path.write_bytes(link_hub.pcap_header() + link_hub.pcap_record(1.5, frame))
    assert link_hub.read_pcap(path) == [(1.5, frame)]


def test_flash_with_mac(tmp_path):
    image = tmp_path / "main.bin"
    image.write_bytes(b"\x11" * 1000)
    assert flash_with_mac(image, None, tmp_path / "out.bin") == image
    out = flash_with_mac(image, "02:00:00:00:00:03", tmp_path / "out.bin")
    data = out.read_bytes()
    assert len(data) == FLASH_SIZE and data[:1000] == b"\x11" * 1000
    assert data[MAC_SECTOR:MAC_SECTOR + 6] == struct.pack("<3H", 0x0200, 0x0000, 0x0003)
    assert set(data[MAC_SECTOR + 6:MAC_SECTOR + 0x2000]) == {0xFF}


def test_set_peer_count_only_touches_keepalives():
    keepalive = udp_frame(50000, prodj(0x06, 0x36, {0x24: 2, 0x30: 2}))
    out = link_hub.set_peer_count(keepalive, 3)
    assert link_hub.udp(out)[2][0x30] == 3
    status = udp_frame(50002, prodj(0x0A, 0xD0, {0x30: 2}))
    assert link_hub.set_peer_count(status, 3) == status


def test_sync_grants_the_least_promise_of_the_others(tmp_path):
    hub = link_hub.SyncHub(tmp_path, "127.0.0.1:0", decks=2, replay=None, replay_delay=0)
    try:
        a, b = object(), object()
        hub.clients[a] = dict(id=1, t=5, promise=7_000, granted=-1, joined=True, rx=0, tx=0)
        assert hub.grant(a) == 0            # held until both have joined
        hub.clients[b] = dict(id=2, t=3, promise=4_000, granted=-1, joined=True, rx=0, tx=0)
        assert hub.grant(a) == 4_000 and hub.grant(b) == 7_000
        del hub.clients[b]
        hub.decks_wanted = 1
        assert hub.grant(a) == link_hub.NEVER   # alone: nothing can reach it
    finally:
        hub.clients.clear()
        hub.close()
