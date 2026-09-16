"""Agent-facing run control stays bounded, observable, and reversible."""

from __future__ import annotations

import json
import fcntl
from pathlib import Path
import socket
import threading
from unittest import mock

from tools.cdj_main import dev, panel_control


def _run(tmp_path: Path, *, state: str = "running", debug: bool = True,
         panel_port: int = 6124) -> Path:
    run = tmp_path / "run"
    run.mkdir()
    (run / "session.json").write_text(json.dumps({"state": state}))
    (run / "run.json").write_text(json.dumps({
        "endpoints": {
            "panel_host": "127.0.0.1", "panel_port": panel_port,
            "qmp": "qmp.sock" if debug else None,
            "gdb_host": "127.0.0.1" if debug else None,
            "gdb_port": 6123 if debug else None,
        }
    }))
    return run


def _actions(run: Path) -> list[dict]:
    return [json.loads(line) for line in
            (run / "actions.jsonl").read_text().splitlines()]


def _checkpoint_run(tmp_path: Path, *, state: str = "running") -> Path:
    run = _run(tmp_path, state=state)
    manifest = json.loads((run / "run.json").read_text())
    manifest["main_environment"] = {
        "CDJ_NXS_DSP_CHECKPOINT_REQUEST": str(run / "dsp-checkpoint-request.json"),
        "CDJ_NXS_DSP_FUNCTIONAL_TIMING": "0",
        "CDJ_NXS_DSP_FUNCTIONAL_AUDIO": "0",
        "CDJ_NXS_DSP_SCHEDULER": "strict",
    }
    (run / "run.json").write_text(json.dumps(manifest))
    (run / "dsp-checkpoints").mkdir()
    return run


def test_checkpoint_rejects_concurrent_client_without_touching_completion(tmp_path, capsys):
    run = _checkpoint_run(tmp_path)
    done = run / "dsp-checkpoint-request.json.done"
    done.write_text('{"ok": true}')
    with (run / "dsp-checkpoint-client.lock").open("a") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert dev.main([str(run), "checkpoint"]) == 2
    assert done.read_text() == '{"ok": true}'
    assert not (run / "dsp-checkpoint-request.json").exists()
    assert "another DSP checkpoint client" in capsys.readouterr().err


def test_press_resolves_nxs_name_uses_short_default_and_logs(tmp_path, capsys):
    run = _run(tmp_path)
    panel = mock.Mock()
    panel.send.return_value = "ok queued"
    with mock.patch.object(dev, "_panel", return_value=panel):
        assert dev.press(run, "sd", None) == 0
    panel.send.assert_called_once_with(panel_control.encode_press(19, 0x08, 100))
    panel.close.assert_called_once()
    output = json.loads(capsys.readouterr().out)
    assert output["ok"] and output["hold_ms"] == 100
    assert {entry["outcome"] for entry in _actions(run)} == {"attempted", "replied"}


def test_reverse_switch_sets_electrical_level_and_rejects_key_pulse(tmp_path, capsys):
    run = _run(tmp_path)
    panel = mock.Mock()
    panel.send.return_value = "ok level"
    with mock.patch.object(dev, "_panel", return_value=panel):
        assert dev.main([str(run), "switch", "rev", "off"]) == 0
        assert dev.main([str(run), "switch", "reverse", "on"]) == 0
        assert dev.main([str(run), "press", "rev"]) == 2
    assert [call.args[0] for call in panel.send.call_args_list] == [
        panel_control.encode_level(15, 2, True),
        panel_control.encode_level(15, 2, False)]
    assert "active-low switch" in capsys.readouterr().err


def test_panel_malformed_reply_is_a_failed_command(tmp_path, capsys):
    run = _run(tmp_path)
    panel = mock.Mock()
    panel.send.return_value = "unexpected"
    with mock.patch.object(dev, "_panel", return_value=panel):
        assert dev.main([str(run), "press", "sd"]) == 1
    assert json.loads(capsys.readouterr().out)["ok"] is False


def test_panel_success_requires_an_exact_ok_token():
    assert dev._reply_ok(" OK queued")
    assert not dev._reply_ok("okay queued")
    assert not dev._reply_ok("ok-not-protocol")
    assert not dev._reply_ok(None)


def test_press_uses_the_real_panel_wire_protocol(tmp_path, capsys):
    client, listener = socket.socketpair()
    received = []

    def serve():
        with listener:
            listener.sendall(b"ok cdj2000-input v1\n")
            received.append(listener.recv(4096).decode())
            listener.sendall(b"ok queued\n")

    thread = threading.Thread(target=serve)
    thread.start()
    run = _run(tmp_path)
    try:
        with mock.patch.object(dev.panel_control.socket, "create_connection",
                               return_value=client):
            assert dev.main([str(run), "press", "sd"]) == 0
    finally:
        thread.join(timeout=2)
        client.close()
    assert received == [panel_control.encode_press(19, 0x08, 100)]
    assert json.loads(capsys.readouterr().out)["ok"]


def test_press_can_request_sampling_length_explicitly(tmp_path):
    run = _run(tmp_path)
    panel = mock.Mock()
    panel.send.return_value = "ok"
    with mock.patch.object(dev, "_panel", return_value=panel):
        assert dev.press(run, "sd", 2800) == 0
    assert panel.send.call_args.args[0] == panel_control.encode_press(19, 0x08, 2800)


def test_rotary_uses_the_verified_encoder_field_and_signed_delta(tmp_path):
    run = _run(tmp_path)
    panel = mock.Mock()
    panel.send.return_value = "ok"
    with mock.patch.object(dev, "_panel", return_value=panel):
        assert dev.rotary(run, -3) == 0
    panel.send.assert_called_once_with(panel_control.encode_rotary(7, -3))


def test_mutation_is_refused_after_run_stops_and_logs_error(tmp_path, capsys):
    run = _run(tmp_path, state="stopped")
    assert dev.main([str(run), "press", "sd"]) == 2
    assert json.loads(capsys.readouterr().err)["ok"] is False
    assert _actions(run)[-1]["outcome"] == "error"


def test_qmp_pause_resume_and_status_are_mapped_and_pause_is_guarded(tmp_path):
    run = _run(tmp_path)
    qmp = mock.MagicMock()
    qmp.__enter__.return_value = qmp
    qmp.command.return_value = {"running": True}
    with mock.patch.object(dev, "Qmp", return_value=qmp) as factory:
        assert dev.qmp(run, "status") == 0
        assert dev.qmp(run, "pause") == 0
        assert dev.qmp(run, "resume") == 0
    assert factory.call_args_list[0].args[0] == run / "qmp.sock"
    assert [call.args[0] for call in qmp.command.call_args_list] == [
        "query-status", "human-monitor-command", "stop", "cont"]
    stopped = tmp_path / "stopped"
    stopped.mkdir()
    (stopped / "session.json").write_text(json.dumps({"state": "stopped"}))
    (stopped / "run.json").write_text(json.dumps({"endpoints": {
        "qmp": "qmp.sock", "panel_port": 6124}}))
    with mock.patch.object(dev, "Qmp") as factory:
        assert dev.main([str(stopped), "qmp", "pause"]) == 2
    factory.assert_not_called()


def test_qmp_client_speaks_to_a_fake_unix_qmp_server(tmp_path, capsys):
    run = _run(tmp_path)
    client, listener = socket.socketpair()
    commands = []

    def serve():
        with listener:
            stream = listener.makefile("rwb", buffering=0)
            stream.write(b'{"QMP":{"version":{}}}\n')
            for _ in range(3):
                request = json.loads(stream.readline())
                commands.append(request["execute"])
                stream.write(json.dumps({"return": {}, "id": request["id"]}).encode() + b"\n")

    thread = threading.Thread(target=serve)
    thread.start()
    try:
        class ConnectedSocket:
            def settimeout(self, timeout):
                client.settimeout(timeout)
            def connect(self, _path):
                return None
            def recv(self, size):
                return client.recv(size)
            def sendall(self, data):
                return client.sendall(data)
            def close(self):
                return client.close()
        with mock.patch("tools.cdj_main.qmp.socket.socket",
                        return_value=ConnectedSocket()):
            assert dev.main([str(run), "qmp", "status"]) == 0
    finally:
        thread.join(timeout=2)
        client.close()
    assert commands == ["qmp_capabilities", "query-status", "human-monitor-command"]
    assert json.loads(capsys.readouterr().out)["ok"]


def test_memory_reads_physical_bytes_with_read_only_hmp_command(tmp_path, capsys):
    run = _run(tmp_path)
    qmp = mock.MagicMock()
    qmp.__enter__.return_value = qmp
    qmp.command.return_value = "00001000: 01 02 03 04"
    with mock.patch.object(dev, "Qmp", return_value=qmp):
        assert dev.memory(run, "0x1000", 4) == 0
    qmp.command.assert_called_once_with(
        "human-monitor-command", {"command-line": "xp /4bx 0x1000"})
    result = json.loads(capsys.readouterr().out)
    assert result["raw"] == "00001000: 01 02 03 04"
    assert result["address"] == 0x1000 and result["length"] == 4
    assert _actions(run)[-1]["outcome"] == "replied"


def test_memory_rejects_malformed_address_and_length_bounds(tmp_path, capsys):
    run = _run(tmp_path)
    qmp = mock.MagicMock()
    qmp.__enter__.return_value = qmp
    with mock.patch.object(dev, "Qmp", return_value=qmp):
        for address in ("nope", "-1", "0x100000000"):
            assert dev.main([str(run), "memory", address]) == 2
        for length in ("0", "4097"):
            assert dev.main([str(run), "memory", "0x0", "--length", length]) == 2
    assert qmp.command.call_count == 0
    assert all(json.loads(line)["ok"] is False
               for line in capsys.readouterr().err.splitlines())


def test_memory_rejects_closed_session_before_connecting(tmp_path):
    run = _run(tmp_path, state="stopped")
    with mock.patch.object(dev, "Qmp") as factory:
        assert dev.main([str(run), "memory", "0x0"]) == 2
    factory.assert_not_called()


def test_stop_writes_atomic_request_and_reports_requested_not_stopped(tmp_path, capsys):
    run = _run(tmp_path)
    assert dev.stop(run) == 0
    request = json.loads((run / "stop-request.json").read_text())
    assert request["requested"] is True and request["requested_unix"] > 0
    output = json.loads(capsys.readouterr().out)
    assert output["ok"] and output["requested"] is True
    assert "stopped" not in output
    assert _actions(run)[-1]["outcome"] == "requested"
    # Repeated requests remain safe and replace the marker atomically.
    assert dev.main([str(run), "stop"]) == 0
    assert json.loads(capsys.readouterr().out)["requested"] is True


def test_stop_rejects_closed_session_without_request_file(tmp_path, capsys):
    run = _run(tmp_path, state="stopped")
    assert dev.main([str(run), "stop"]) == 2
    assert not (run / "stop-request.json").exists()
    assert json.loads(capsys.readouterr().err)["ok"] is False


def test_screenshot_rejects_incomplete_ppm_and_accepts_complete(tmp_path, capsys):
    run = _run(tmp_path)
    (run / "screen.ppm").write_bytes(b"P6\n2 1\n255\n\0")
    assert dev.main([str(run), "screenshot", str(tmp_path / "bad.png")]) == 2
    assert not (tmp_path / "bad.png").exists()
    (run / "screen.ppm").write_bytes(b"P6\n480 234\n255\n" + bytes(480 * 234 * 3))
    assert dev.screenshot(run, tmp_path / "good.png") == 0
    assert (tmp_path / "good.png").exists()
    assert dev.main([str(run), "screenshot", str(tmp_path / "good.png")]) == 2
    output = capsys.readouterr().out
    assert '"width": 480' in output


def test_wait_browser_matches_text_and_times_out_nonzero(tmp_path, capsys):
    run = _run(tmp_path)
    body = [0x10, 0, 1, 0, 0, 0, 0, 0, 1, 0x55, 0, 4]
    body += list(map(ord, "BEAT"))
    words = b"".join(word.to_bytes(2, "little") for word in body)
    (run / "main-link.bin").write_bytes(b"SPRX" + len(words).to_bytes(4, "little") + words)
    assert dev.wait_browser(run, "beat", timeout=0.1, poll=0.01) == 0
    assert dev.wait_browser(run, "missing", timeout=0.01, poll=0.005) == 1
    assert [entry["outcome"] for entry in _actions(run)][-2:] == ["matched", "timeout"]
    assert "\"ok\": false" in capsys.readouterr().out


def _transport_snapshot(record, remaining, duration=1500, caught_up=True):
    return {"caught_up": caught_up, "transport": {
        "record": record, "remaining_frames": remaining,
        "duration_frames": duration}}


def test_wait_playback_requires_fresh_counter_decreases(tmp_path, capsys):
    run = _run(tmp_path)
    rows = [_transport_snapshot(1, 1490, caught_up=False),
            _transport_snapshot(2, 1480, caught_up=False),
            _transport_snapshot(3, 1470), _transport_snapshot(3, 1470),
            _transport_snapshot(4, 1390), _transport_snapshot(5, 1300)]
    observer = mock.Mock()
    observer.poll.side_effect = rows
    with mock.patch.object(dev, "LinkObserver", return_value=observer), \
            mock.patch.object(dev.time, "sleep"):
        assert dev.wait_playback(run) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["first"]["record"] == 3
    assert result["fresh_samples"] == 3
    assert result["decreases"] == 2
    assert result["audio_verified"] is False


def test_wait_playback_resets_for_seek_and_duration_change(tmp_path, capsys):
    run = _run(tmp_path)
    rows = [_transport_snapshot(1, 1490), _transport_snapshot(2, 1480),
            _transport_snapshot(3, 1495), _transport_snapshot(4, 1490),
            _transport_snapshot(5, 1400, 1600),
            _transport_snapshot(6, 1390, 1600), _transport_snapshot(7, 1380, 1600)]
    observer = mock.Mock()
    observer.poll.side_effect = rows
    with mock.patch.object(dev, "LinkObserver", return_value=observer), \
            mock.patch.object(dev.time, "sleep"):
        assert dev.wait_playback(run, min_frames=10) == 0
    assert json.loads(capsys.readouterr().out)["first"]["record"] == 5


def test_wait_playback_stationary_or_repeated_records_timeout(tmp_path, capsys):
    run = _run(tmp_path)
    observer = mock.Mock()
    observer.poll.return_value = _transport_snapshot(10, 1500)
    with mock.patch.object(dev, "LinkObserver", return_value=observer):
        assert dev.wait_playback(run, timeout=.01, poll=.002) == 1
    result = json.loads(capsys.readouterr().out)
    assert result["fresh_samples"] == 1
    assert result["counter_advanced"] is False


def test_wait_playback_rejects_brief_movement_followed_by_stall(tmp_path, capsys):
    run = _run(tmp_path)
    rows = iter([_transport_snapshot(1, 1500), _transport_snapshot(2, 1496),
                 _transport_snapshot(3, 1492)])
    observer = mock.Mock()
    observer.poll.side_effect = lambda *_: next(rows, _transport_snapshot(3, 1492))
    with mock.patch.object(dev, "LinkObserver", return_value=observer):
        assert dev.wait_playback(run, timeout=.01, poll=.002) == 1
    result = json.loads(capsys.readouterr().out)
    assert result["decreases"] == 2
    assert result["advanced_frames"] == 8
    assert result["min_frames"] == 150


def test_wait_playback_refuses_stopped_run_and_invalid_deadlines(tmp_path, capsys):
    run = _run(tmp_path, state="stopped")
    assert dev.main([str(run), "wait-playback"]) == 2
    assert dev.main([str(run), "wait-playback", "--timeout", "nan"]) == 2
    assert dev.main([str(run), "wait-playback", "--poll", "0"]) == 2


def test_wait_browser_does_not_match_partial_list_or_stopped_run(tmp_path, capsys):
    run = _run(tmp_path)
    body = [0x10, 0, 1, 0, 0, 0, 0, 0, 2, 0x55, 0, 4]
    body += list(map(ord, "BEAT"))
    words = b"".join(word.to_bytes(2, "little") for word in body)
    (run / "main-link.bin").write_bytes(b"SPRX" + len(words).to_bytes(4, "little") + words)
    assert dev.wait_browser(run, "beat", timeout=0.01, poll=0.005) == 1
    (run / "session.json").write_text(json.dumps({"state": "stopped"}))
    assert dev.main([str(run), "wait-browser", "beat", "--timeout", "0.01"]) == 2


def test_wait_browser_rejects_nonfinite_deadlines(tmp_path, capsys):
    run = _run(tmp_path)
    assert dev.main([str(run), "wait-browser", "beat", "--timeout", "nan"]) == 2
    assert json.loads(capsys.readouterr().err)["ok"] is False


def _link_record(words: list[int]) -> bytes:
    payload = b"".join(word.to_bytes(2, "little") for word in words)
    return b"SPRX" + len(payload).to_bytes(4, "little") + payload


def test_wait_duration_requires_nonzero_complete_field_and_returns_raw_payload(tmp_path, capsys):
    run = _run(tmp_path)
    zero = _link_record([5, 1, 0xFF, 0, 0, 0, 0, 44571])
    (run / "main-link.bin").write_bytes(zero)
    assert dev.wait_duration(run, timeout=0.01, poll=0.005) == 1
    timeout = json.loads(capsys.readouterr().out)
    assert timeout["observed"] is False

    expected = [5, 1, 0xFF, 0, 2560, 0, 0, 44571]
    with (run / "main-link.bin").open("ab") as stream:
        stream.write(_link_record(expected))
    assert dev.wait_duration(run, timeout=0.1, poll=0.005) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["duration_payload"]["words"] == expected
    assert result["duration_words"] == [0, 2560]
    assert result["duration_field"] == 2560
    assert result["playback_proven"] is False
    assert _actions(run)[-1]["outcome"] == "matched"


def test_wait_duration_does_not_accept_partial_record_or_stopped_run(tmp_path, capsys):
    run = _run(tmp_path)
    record = _link_record([5, 1, 0xFF, 0, 2560, 0, 0, 44571])
    (run / "main-link.bin").write_bytes(record[:12])
    assert dev.wait_duration(run, timeout=0.01, poll=0.005) == 1
    assert json.loads(capsys.readouterr().out)["observed"] is False

    (run / "main-link.bin").write_bytes(record)
    (run / "session.json").write_text(json.dumps({"state": "stopped"}))
    assert dev.main([str(run), "wait-duration", "--timeout", "0.01"]) == 2
    assert json.loads(capsys.readouterr().err)["ok"] is False


def test_wait_duration_rejects_nonfinite_deadlines(tmp_path, capsys):
    run = _run(tmp_path)
    assert dev.main([str(run), "wait-duration", "--timeout", "nan"]) == 2
    assert json.loads(capsys.readouterr().err)["ok"] is False


def test_waits_can_require_a_record_newer_than_stale_evidence(tmp_path, capsys):
    run = _run(tmp_path)
    old_body = [0x10, 0, 1, 0, 0, 0, 0, 0, 1, 0x55, 0, 7]
    old_body += list(map(ord, "OLD.WAV"))
    (run / "main-link.bin").write_bytes(_link_record(old_body))
    assert dev.wait_browser(run, "OLD.WAV", timeout=0.01, poll=0.005,
                            after_record=1) == 1
    assert json.loads(capsys.readouterr().out)["after_record"] == 1

    new_body = [0x10, 0, 1, 0, 0, 0, 0, 0, 1, 0x55, 0, 7]
    new_body += list(map(ord, "NEW.WAV"))
    with (run / "main-link.bin").open("ab") as stream:
        stream.write(_link_record(new_body))
    assert dev.wait_browser(run, "NEW.WAV", timeout=0.1, poll=0.005,
                            after_record=1) == 0
    assert json.loads(capsys.readouterr().out)["matched_record"] == 2


def test_checkpoint_rejects_pending_request_before_clearing_done(tmp_path, capsys):
    run = _checkpoint_run(tmp_path)
    request = run / "dsp-checkpoint-request.json"
    done = run / "dsp-checkpoint-request.json.done"
    request.write_text('{"requested":true}\n')
    done.write_text('{"ok":true}\n')
    assert dev.main([str(run), "checkpoint", "--timeout", "1"]) == 2
    assert request.exists() and done.exists()
    assert json.loads(capsys.readouterr().err)["ok"] is False


def test_checkpoint_timeout_keeps_request_pending(tmp_path, capsys):
    run = _checkpoint_run(tmp_path)
    assert dev.checkpoint(run, timeout=0.01, poll=0.005) == 1
    result = json.loads(capsys.readouterr().out)
    assert result["ok"] is False and result["ready"] is False
    assert result["pending"] is True
    assert json.loads((run / "dsp-checkpoint-request.json").read_text())["reason"] == "debug request"
    assert _actions(run)[-1]["outcome"] == "timeout"


def test_checkpoint_copies_validated_snapshot_to_diagnostic_sidecar(tmp_path, capsys):
    run = _checkpoint_run(tmp_path)
    live_manifest = run / "dsp-checkpoints" / "manifest.json"
    live_manifest.write_text('{"capture": "in progress"}\n')
    source = run / "dsp-checkpoints" / "000007.cdjdsp"
    source_bytes = b"validated checkpoint"
    metadata = {"file": source.name, "sha256": "deadbeef", "size": len(source_bytes),
                "schema": 11}
    def complete_request(_seconds):
        source.write_bytes(source_bytes)
        (run / "dsp-checkpoint-request.json.done").write_text(
            json.dumps({"ok": True, "file": source.name}))

    with mock.patch.object(dev, "checkpoint_metadata", return_value=metadata), \
            mock.patch.object(dev.time, "sleep", side_effect=complete_request):
        assert dev.checkpoint(run, timeout=0.5, poll=0.01) == 0
    result = json.loads(capsys.readouterr().out)
    target = Path(result["file"])
    manifest_path = Path(result["manifest"])
    assert result["ready"] is True and result["provenance"] == "diagnostic_connected_checkpoint"
    assert result["snapshot"] == result["file"]
    assert target.read_bytes() == source_bytes
    assert target.parent == run / "debug-checkpoints" / source.name
    sidecar = json.loads(manifest_path.read_text())
    assert sidecar["complete"] is False
    assert sidecar["architectural_validation_eligible"] is False
    assert sidecar["dsp_checkpoint_policy"] == "fault"
    assert sidecar["checkpoint_capture_complete"] is True
    assert sidecar["dsp_timing_mode"] == "strict"
    assert sidecar["checkpoint_metadata"]["sha256"] == "deadbeef"
    assert live_manifest.read_text() == '{"capture": "in progress"}\n'
    assert _actions(run)[-1]["outcome"] == "ready"


def test_checkpoint_returns_board_error_and_validates_timeout_bounds(tmp_path, capsys):
    run = _checkpoint_run(tmp_path)
    (run / "dsp-checkpoint-request.json.done").write_text(
        json.dumps({"ok": False, "error": "DSP halted"}))
    # A stale completion is cleared before the new exclusive request is made.
    assert dev.main([str(run), "checkpoint", "--timeout", "301"]) == 2
    assert json.loads(capsys.readouterr().err)["ok"] is False
    # Simulate the board completing the valid request on the next poll.
    with mock.patch.object(dev.time, "sleep", side_effect=lambda _: (
            run.joinpath("dsp-checkpoint-request.json.done").write_text(
                json.dumps({"ok": False, "error": "DSP halted"})) or None)):
        assert dev.checkpoint(run, timeout=0.5, poll=0.01) == 2
    result = json.loads(capsys.readouterr().out)
    assert result["response"]["error"] == "DSP halted"
    assert _actions(run)[-1]["outcome"] == "error"

    duration_base = tmp_path / "duration"
    duration_base.mkdir()
    duration_run = _run(duration_base)
    duration = _link_record([5, 1, 0xFF, 0, 2560, 0, 0, 44571])
    (duration_run / "main-link.bin").write_bytes(duration)
    assert dev.wait_duration(duration_run, timeout=0.01, poll=0.005,
                             after_record=1) == 1
    capsys.readouterr()
    with (duration_run / "main-link.bin").open("ab") as stream:
        stream.write(duration)
    assert dev.wait_duration(duration_run, timeout=0.1, poll=0.005,
                             after_record=1) == 0
    assert json.loads(capsys.readouterr().out)["matched_record"] == 2


def test_parse_args_exposes_small_agent_surface():
    args = dev.parse_args(["runs/x", "press", "sd", "--hold-ms", "2800"])
    assert (args.command, args.button, args.hold_ms) == ("press", "sd", 2800)
    args = dev.parse_args(["runs/x", "memory", "0x1000", "--length", "12"])
    assert (args.command, args.address, args.length) == ("memory", "0x1000", 12)
    assert dev.parse_args(["runs/x", "stop"]).command == "stop"
    args = dev.parse_args(["runs/x", "wait-duration", "--timeout", "120"])
    assert (args.command, args.timeout, args.poll) == ("wait-duration", 120, 0.25)
    assert args.after_record == 0
    args = dev.parse_args(["runs/x", "checkpoint", "--timeout", "12", "--poll", "1"])
    assert (args.command, args.timeout, args.poll) == ("checkpoint", 12, 1)
