import json
import struct

import pytest

from tools.cdj_main.ethernet_peer import checksum
from tools.cdj_main.network_inventory import classify, inventory


def packet(fragment=0):
    udp = struct.pack('!HHHH', 319, 320, 8, 0)
    ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 28, 0,
                              fragment, 64, 17, 0, b'\x01'*4, b'\x02'*4))
    struct.pack_into('!H', ip, 10, checksum(ip))
    return bytes(12) + b'\x08\x00' + ip + udp


def test_ports_and_fragments():
    assert classify(packet())['destination_port'] == 320
    assert 'destination_port' not in classify(packet(0x2000))
    with pytest.raises(ValueError):
        classify(packet()[:-1])


def test_capture_errors_are_not_silently_dropped(tmp_path):
    path = tmp_path / 'frames.jsonl'
    path.write_text(json.dumps({'direction': 'guest-to-peer', 'hex': packet().hex()})
                    + '\n{}\n')
    report = inventory(path)
    assert report['records'] == 2
    assert report['flows'][0]['count'] == 1
    assert report['invalid_records'][0]['line'] == 2
