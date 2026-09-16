"""The gdbstub client's framing, which has no other check.

The reply stream carries '+' acks between packets and arrives in arbitrary
chunks, so the reader has to find '$...#xx' across recv boundaries rather than
assume one packet per read.
"""
import socket

from tools.cdj_main.gdbprobe import Rsp


def connected_pair():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    rsp = Rsp(listener.getsockname()[1], timeout=2.0)
    peer, _ = listener.accept()
    listener.close()
    return rsp, peer


def test_packet_skips_acks_and_spans_chunks():
    rsp, peer = connected_pair()
    try:
        peer.sendall(b"+$T05thr")
        peer.sendall(b"ead:01;#aa+")
        assert rsp.packet(timeout=2.0) == "T05thread:01;"
    finally:
        rsp.sock.close()
        peer.close()


def test_send_appends_the_checksum():
    rsp, peer = connected_pair()
    try:
        rsp.send("g")
        assert peer.recv(64) == b"$g#67"          # 0x67 == ord('g')
    finally:
        rsp.sock.close()
        peer.close()
