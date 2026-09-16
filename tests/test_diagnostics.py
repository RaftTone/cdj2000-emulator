"""Diagnostic handoffs stay bounded and never replace an existing capture."""
import hashlib
import json
from unittest import mock

import pytest

from tools.cdj_main import diagnostics, dev


def test_capture_retains_partial_evidence_and_bounds_logs(tmp_path):
    run = tmp_path / 'run'
    run.mkdir()
    (run / 'run.json').write_text('{"main": ["qemu"]}')
    (run / 'main.log').write_text('old log\n' * 20000 + 'unimplemented opcode\n')
    (run / 'main-link.bin').write_bytes(b'large raw evidence')
    output = tmp_path / 'capture'
    with mock.patch.object(diagnostics, 'observe', return_value={'progress': 'waiting'}):
        result = diagnostics.capture(run, output)
    assert result['ok'] and not result['complete']
    assert 'session.json' in result['errors']
    assert (output / 'main.log.tail').stat().st_size <= diagnostics.TAIL_BYTES
    assert (output / 'main.log.tail').read_text().endswith('unimplemented opcode\n')
    assert not (output / 'main-link.bin').exists()
    manifest = json.loads((output / 'capture.json').read_text())
    assert manifest['source_inventory']['main-link.bin']['bytes'] == 18
    for name, entry in manifest['files'].items():
        assert hashlib.sha256((output / name).read_bytes()).hexdigest() == entry['sha256']


def test_capture_refuses_existing_destination(tmp_path):
    with pytest.raises(FileExistsError):
        diagnostics.capture(tmp_path, tmp_path)
    assert not (tmp_path / 'capture.json').exists()


def test_capture_preserves_frame_and_reports_observer_failure(tmp_path):
    run = tmp_path / 'run'
    run.mkdir()
    raw = b'P6\n1 1\n255\n\x00\x00\x00'
    with mock.patch.object(diagnostics, 'observe', side_effect=ValueError('broken link')), \
         mock.patch.object(diagnostics, 'read_frame', return_value=(raw, {})):
        result = diagnostics.capture(run, tmp_path / 'capture')
    assert result['errors']['status.json'] == 'broken link'
    assert (tmp_path / 'capture' / 'screen.ppm').read_bytes() == raw


def test_diagnose_works_for_stopped_run_without_debug_endpoints(tmp_path, capsys):
    run = tmp_path / 'run'
    run.mkdir()
    (run / 'session.json').write_text('{"state": "stopped"}')
    assert dev.main([str(run), 'diagnose', str(tmp_path / 'capture')]) == 0
    result = json.loads(capsys.readouterr().out)
    assert result['ok']
    assert json.loads((run / 'actions.jsonl').read_text())['command'] == 'diagnose'
