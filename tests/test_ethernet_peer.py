import struct
import json
import socket
import subprocess
import sys

import pytest

from tools.cdj_main.ethernet_peer import Framing, MAC, checksum, respond

IP = bytes([192, 168, 42, 1])
GUEST = bytes.fromhex("020000000002")
GUEST_IP = bytes([192, 168, 42, 2])


def arp():
    return b"\xff" * 6 + GUEST + bytes.fromhex("08060001080006040001") + GUEST + GUEST_IP + bytes(6) + IP


def echo():
    payload = bytearray(bytes.fromhex("0800000012340001") + b"odd")
    struct.pack_into("!H", payload, 2, checksum(payload))
    header = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 99, 0, 64, 1, 0, GUEST_IP, IP))
    struct.pack_into("!H", header, 10, checksum(header))
    return MAC + GUEST + b"\x08\0" + header + payload


def test_arp_exact_reply():
    reply = respond(arp(), IP)
    assert reply == (GUEST + MAC + bytes.fromhex("08060001080006040002") + MAC + IP + GUEST + GUEST_IP).ljust(60, b"\0")
    assert respond(arp()[:-1], IP) is None
    assert respond(arp(), GUEST_IP) is None


def test_echo_exact_payload_and_checksums():
    reply = respond(echo(), IP)
    assert reply[:12] == GUEST + MAC
    assert reply[26:34] == IP + GUEST_IP
    assert checksum(reply[14:34]) == 0
    length = int.from_bytes(reply[16:18], "big")
    assert checksum(reply[34:14 + length]) == 0
    assert reply[34:36] == bytes(2)
    assert reply[38:45] == bytes.fromhex("12340001") + b"odd"


@pytest.mark.parametrize("offset", [14, 20, 24, 34, 40])
def test_corruption_rejected(offset):
    frame = bytearray(echo())
    frame[offset] ^= 1
    assert respond(frame, IP) is None


def test_fragment_rejected_with_valid_checksum():
    frame = bytearray(echo())
    frame[20] = 0x20
    frame[24:26] = bytes(2)
    struct.pack_into("!H", frame, 24, checksum(frame[14:34]))
    assert respond(frame, IP) is None


def test_incremental_and_coalesced_framing():
    frame = arp()
    encoded = struct.pack("!I", len(frame)) + frame
    parser = Framing()
    actual = []
    for byte in encoded:
        actual += parser.feed(bytes([byte]))
    assert actual == [frame]
    assert parser.feed(encoded * 2) == [frame, frame]
    parser.finish()


@pytest.mark.parametrize("size", [0, 13, 1515, 0xffffffff])
def test_bad_length(size):
    with pytest.raises(ValueError):
        Framing().feed(struct.pack("!I", size))


def test_truncation():
    parser = Framing()
    parser.feed(b"\0")
    with pytest.raises(ValueError):
        parser.finish()


def test_local_socket_capture(tmp_path):
    directory = tmp_path / "peer"
    process = subprocess.Popen(
        [sys.executable, "-m", "tools.cdj_main.ethernet_peer", str(directory),
         "--seconds", "5"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True)
    try:
        endpoint = json.loads(process.stdout.readline())
        assert endpoint["host"] == "127.0.0.1"
        with socket.create_connection((endpoint["host"], endpoint["port"]), timeout=2) as conn:
            request = arp()
            conn.sendall(struct.pack("!I", len(request)) + request)
            parser = Framing()
            frames = []
            while not frames:
                data = conn.recv(4096)
                assert data
                frames += parser.feed(data)
            assert frames == [respond(request, IP)]
        _, error = process.communicate(timeout=5)
        assert process.returncode == 0, error
        assert json.loads((directory / "summary.json").read_text()) == {
            "received": 1, "sent": 1, "unsupported_or_invalid": 0}
        records = [json.loads(line) for line in (directory / "frames.jsonl").read_text().splitlines()]
        assert [record["direction"] for record in records] == ["guest-to-peer", "peer-to-guest"]
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
