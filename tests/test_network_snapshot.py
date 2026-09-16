import json
from unittest.mock import Mock

import pytest

from tools.cdj_main import network_snapshot as snapshot


def setup_run(tmp_path, sha=snapshot.MAIN_SHA):
    (tmp_path / 'run.json').write_text(json.dumps({
        'input_artifacts': {'main_firmware': {'sha256': sha}}}))


@pytest.mark.parametrize('tag,sha,existing,error', [
    ('../bad', snapshot.MAIN_SHA, False, ValueError),
    ('good', 'wrong', False, ValueError),
    ('good', snapshot.MAIN_SHA, True, FileExistsError),
])
def test_rejects_before_connect(tmp_path, monkeypatch, tag, sha, existing, error):
    setup_run(tmp_path, sha)
    if existing:
        (tmp_path / 'network-good.json').touch()
    factory = Mock()
    monkeypatch.setattr(snapshot.socket, 'socket', factory)
    with pytest.raises(error):
        snapshot.capture(tmp_path, tag)
    factory.assert_not_called()


@pytest.mark.parametrize('fail', [False, True])
def test_capture_resumes_even_on_failure(tmp_path, monkeypatch, fail):
    setup_run(tmp_path)
    sock = Mock()
    sock.__enter__ = Mock(return_value=sock)
    sock.__exit__ = Mock(return_value=False)
    sock.recv.return_value = b'(qemu)'
    commands = []

    def send(data):
        command = data.decode().strip()
        commands.append(command)
        if command.startswith('pmemsave'):
            if fail:
                raise OSError('capture failed')
            _, address, size, path = command.split(' ', 3)
            contents = bytearray(int(size, 0))
            if int(address, 0) == snapshot.REGIONS['dhcp'][0]:
                contents[0x58] = 1
                contents[0x5c:0x60] = (39).to_bytes(4, 'little')
            if int(address, 0) == snapshot.REGIONS['kernel'][0]:
                contents[156:160] = snapshot.REGIONS['dhcp_task'][0].to_bytes(4, 'little')
                contents[0xb48:0xb4c] = (1901).to_bytes(4, 'little')
            snapshot.Path(path.strip('"')).write_bytes(contents)

    sock.sendall.side_effect = send
    monkeypatch.setattr(snapshot.socket, 'socket', Mock(return_value=sock))
    if fail:
        with pytest.raises(OSError):
            snapshot.capture(tmp_path, 'test')
        assert not (tmp_path / 'network-test.json').exists()
    else:
        snapshot.capture(tmp_path, 'test')
        result = json.loads((tmp_path / 'network-test.json').read_text())
        assert result['decoded']['kernel_tick'] == 1901
        assert result['decoded']['task_pointer_matches']
        assert result['decoded']['dhcp_task_live'] == 1
        assert len(result['regions']) == len(snapshot.REGIONS)
    assert commands[0] == 'stop'
    assert commands[-1] == 'cont'
    assert all(c in ('stop', 'cont') or c.startswith('pmemsave ') for c in commands)
