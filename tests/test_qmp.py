"""Debugger replies can arrive split across reads with asynchronous events."""
import json
from pathlib import Path

import pytest

from tools.cdj_main.qmp import Qmp, QmpError


class Socket:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.sent = []
        self.closed = False

    def settimeout(self, timeout):
        assert timeout > 0

    def connect(self, path):
        self.path = path

    def recv(self, size):
        return next(self.chunks, b'')

    def sendall(self, data):
        self.sent.append(json.loads(data))

    def close(self):
        self.closed = True


def test_split_greeting_and_events_do_not_confuse_command_replies(monkeypatch):
    sock = Socket([b'{"Q', b'MP":{}}\n', b'{"return":{},"id":1}\n',
                   b'{"event":"STOP"}\n{"return":{"running":false},',
                   b'"id":2}\n'])
    monkeypatch.setattr('tools.cdj_main.qmp.socket.socket', lambda *a: sock)
    with Qmp(Path('/tmp/emulator.sock')) as qmp:
        assert qmp.command('query-status') == {'running': False}
    assert sock.sent == [
        {'execute': 'qmp_capabilities', 'id': 1},
        {'execute': 'query-status', 'id': 2},
    ]
    assert sock.closed


@pytest.mark.parametrize('reply, error', [
    (b'{"return":{},"id":999}\n', QmpError),
    (b'{"error":{"desc":"command failed"},"id":2}\n', QmpError),
    (b'not json\n', QmpError),
    (b'[]\n', QmpError),
    (b'', ConnectionError),
])
def test_command_errors_never_become_success(monkeypatch, reply, error):
    sock = Socket([b'{"QMP":{}}\n{"return":{},"id":1}\n', reply])
    monkeypatch.setattr('tools.cdj_main.qmp.socket.socket', lambda *a: sock)
    with pytest.raises(error), Qmp(Path('/tmp/emulator.sock')) as qmp:
        qmp.command('stop')
    assert sock.closed


def test_bad_greeting_closes_connection(monkeypatch):
    sock = Socket([b'{"not_qmp":true}\n'])
    monkeypatch.setattr('tools.cdj_main.qmp.socket.socket', lambda *a: sock)
    with pytest.raises(QmpError):
        Qmp(Path('/tmp/emulator.sock'))
    assert sock.closed


@pytest.mark.parametrize('timeout', [0, -1, float('nan'), float('inf')])
def test_invalid_deadline_rejected(timeout):
    with pytest.raises(ValueError):
        Qmp(Path('/tmp/emulator.sock'), timeout=timeout)


def test_tcp_endpoint_uses_create_connection(monkeypatch):
    sock = Socket([b'{"QMP":{}}\n', b'{"return":{},"id":1}\n',
                   b'{"return":{"running":false},"id":2}\n'])
    monkeypatch.setattr('tools.cdj_main.qmp.socket.create_connection',
                        lambda address, timeout=None: sock)
    with Qmp(('127.0.0.1', 5981)) as qmp:
        assert qmp.command('query-status') == {'running': False}
    assert sock.closed


def test_parse_endpoint_keeps_unix_paths_and_host_port():
    from tools.cdj_main.qmp import parse_endpoint
    run = Path('/tmp/run')
    assert parse_endpoint('qmp.sock', relative_to=run) == run / 'qmp.sock'
    assert parse_endpoint('127.0.0.1:5981') == ('127.0.0.1', 5981)
    assert parse_endpoint(('127.0.0.1', 5981)) == ('127.0.0.1', 5981)
