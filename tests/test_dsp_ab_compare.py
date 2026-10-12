# SPDX-License-Identifier: GPL-2.0-or-later
import json
import struct

import pytest

from tools.cdj_dsp.ab_compare import main as compare_main
from tools.cdj_dsp.replay import OVERLAY_MAGIC, parse_overlay


def _write(directory, mcasp1, mcasp2, host):
    directory.mkdir()
    (directory / 'mcasp1.bin').write_bytes(struct.pack(f'<{len(mcasp1)}I', *mcasp1))
    (directory / 'mcasp2.bin').write_bytes(struct.pack(f'<{len(mcasp2)}I', *mcasp2))
    (directory / 'host.bin').write_bytes(b''.join(struct.pack('<4I', *r) for r in host))


def test_compare_identical_and_first_divergence(tmp_path, capsys):
    host = [(1, 10, 0x11800000, 5), (2, 11, 0x01c0, 0x14a)]
    _write(tmp_path / 'a', [0, 1, 2, 3], [7, 7], host)
    _write(tmp_path / 'b', [0, 1, 2, 3], [7, 7], host)
    assert compare_main([str(tmp_path / 'a'), str(tmp_path / 'b')]) == 0
    assert json.loads(capsys.readouterr().out)['identical']

    _write(tmp_path / 'c', [0, 1, 9, 3, 4], [7, 7], [host[0], (2, 11, 0x01c0, 0x14b)])
    assert compare_main([str(tmp_path / 'a'), str(tmp_path / 'c')]) == 1
    files = json.loads(capsys.readouterr().out)['files']
    assert files['mcasp1.bin']['first_divergence']['word'] == 2
    assert files['mcasp1.bin']['differing_items'] == 2      # one word + length
    assert files['mcasp2.bin']['first'] is None
    assert files['host.bin']['first_divergence']['b']['value'] == 0x14b

    _write(tmp_path / 'd', [0, 1, 2, 3], [7, 7], host[:1])
    assert compare_main([str(tmp_path / 'a'), str(tmp_path / 'd')]) == 1
    assert json.loads(capsys.readouterr().out)['files']['host.bin']['first'] == 1


def test_compare_rejects_non_ab_directory(tmp_path):
    (tmp_path / 'x').mkdir()
    _write(tmp_path / 'a', [0], [0], [])
    assert compare_main([str(tmp_path / 'a'), str(tmp_path / 'x')]) == 2


def test_overlay_parsing():
    native = parse_overlay(json.dumps({
        'schema': 1, 'trigger_pc': '0x118030a0',
        'writes': [{'address': '0xC0047758', 'hex': '0102'},
                   {'address': '0x11800000', 'hex': 'ff'}]}).encode())
    assert native['trigger_pc'] == 0x118030a0 and native['bytes'] == 3
    assert native['native'] == (struct.pack('<4I', OVERLAY_MAGIC, 1, 0x118030a0, 2) +
                                struct.pack('<2I', 0xC0047758, 2) + b'\1\2' +
                                struct.pack('<2I', 0x11800000, 1) + b'\xff')
    start = parse_overlay(b'{"schema":1,"trigger_pc":null,"writes":[{"address":"0x0","hex":"00"}]}')
    assert start['trigger_pc'] is None and start['native'][4:12] == bytes(8)
    for bad in (b'{"schema":2,"trigger_pc":null,"writes":[{"address":"0","hex":"00"}]}',
                b'{"schema":1,"writes":[{"address":"0","hex":"00"}]}',
                b'{"schema":1,"trigger_pc":null,"writes":[]}',
                b'{"schema":1,"trigger_pc":null,"writes":[{"address":"0xffffffff","hex":"0000"}]}',
                b'{"schema":1,"trigger_pc":null,"writes":[{"address":"0","hex":""}]}',
                b'{"schema":1,"trigger_pc":"0x0","writes":[{"address":"0","hex":"00"}]}',
                b'not json'):
        with pytest.raises(ValueError):
            parse_overlay(bad)
