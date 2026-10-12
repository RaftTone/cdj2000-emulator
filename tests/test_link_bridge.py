import struct

from tools.cdj_main.link_hub import bpf_frames, crosses


def record(frame: bytes, caplen=None) -> bytes:
    caplen = len(frame) if caplen is None else caplen
    head = struct.pack("=iiIIH", 0, 0, caplen, len(frame), 18)
    body = head + frame[:caplen]
    return body + b"\0" * (-len(body) % 4)


def test_bpf_records_are_aligned_and_truncation_is_dropped():
    a, b, c = b"\x01" * 61, b"\x02" * 60, b"\x03" * 70
    assert bpf_frames(record(a) + record(b, caplen=40) + record(c)) == [a, c]


def test_bridge_policy():
    deck, other = bytes.fromhex("000000000001"), bytes.fromhex("745e1c58de9c")
    bcast, mcast = b"\xff" * 6, bytes.fromhex("01005e7f8621")
    assert crosses(bcast + other, {deck}, False, False)
    assert crosses(deck + other, {deck}, False, False)
    assert not crosses(other + deck, {deck}, False, False)   # wire unicast not for a deck
    assert crosses(other + deck, set(), True, False)         # deck unicast goes out
    assert not crosses(mcast + deck, set(), True, False)     # Dante/mDNS stay home
    assert crosses(mcast + deck, set(), True, True)
