"""Run-aware diagnostics exposed by the interactive firmware viewer."""

import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from tools.cdj_gui import view_ui


def test_diagnostics_text_keeps_observations_and_debug_scope_explicit():
    text = view_ui.run_diagnostics_text({
        "run": "/tmp/stock-run",
        "session": {"state": "running", "processes": {"main": 12}},
        "frame": {"status": "captured", "age_seconds": 1.25,
                  "width": 480, "height": 255},
        "link": {"status": "observed", "records": 7, "caught_up": True,
                 "transport": {"play_requested": True,
                                "remaining_seconds": 8.5,
                                "duration_seconds": 10.0}},
        "progress": "Latest browser reply: TESTTONE.WAV",
        "media": {"images": {"sd_image": "/tmp/card.img"}},
        "endpoints": {"gdb_host": "127.0.0.1", "gdb_port": 6283},
        "debug": {"main_starts_paused": True},
        "recent_fault_lines": [{"file": "gui.log", "line": "double fault"}],
        "recent_actions": [{"action": "status", "outcome": "observed"},
                           {"command": "press 19 04", "ok": False,
                            "error": "queue full"}],
    })

    assert "RUN  /tmp/stock-run" in text
    assert "session: running" in text
    assert "frame: captured, 1.2s old (480x255)" in text
    assert "browser: Latest browser reply: TESTTONE.WAV" in text
    assert "transport: playing; 8.500s remaining / 10.000s duration" in text
    assert "transport evidence: compare newer records for counter movement" in text
    assert "media: sd_image=/tmp/card.img" in text
    assert "GDB: 127.0.0.1:6283 (MAIN only; GUI and host deadlines continue)" in text
    assert "debug pause: MAIN only; GUI and host deadlines continue" in text
    assert "gui.log: double fault" in text
    assert "status [observed]" in text
    assert "FAILED press 19 04" in text


def test_coverage_print_uses_nxs_board(capsys):
    assert view_ui.print_coverage(True) == 0
    first_line = capsys.readouterr().out.splitlines()[0]
    assert first_line == (
        f"{len(view_ui.nxs_panel.input_ids())} of "
        f"{len(view_ui.nxs_panel.input_ids())} inputs have a control")


def test_main_forwards_nxs_coverage_selection(monkeypatch):
    monkeypatch.setattr(view_ui, "parse_args", lambda: SimpleNamespace(
        coverage=True, nxs_panel=True))
    print_coverage = Mock(return_value=0)
    monkeypatch.setattr(view_ui, "print_coverage", print_coverage)

    assert view_ui.main() == 0
    print_coverage.assert_called_once_with(True)


def test_nxs_click_queues_short_pulse_while_legacy_click_keeps_sampling_hold():
    control = view_ui.Control("ENTER", "17.0", "button", "deck", (), "")
    nxs = object.__new__(view_ui.UiViewer)
    nxs.args = SimpleNamespace(nxs_panel=True)
    nxs.press = Mock()
    nxs.click(control)
    nxs.press.assert_called_once_with(
        control, view_ui.NXS_CLICK_HOLD_MS,
        "observe browser status and the framebuffer")

    legacy = object.__new__(view_ui.UiViewer)
    legacy.args = SimpleNamespace(nxs_panel=False)
    legacy.press = Mock()
    legacy.click(control)
    legacy.press.assert_called_once_with(
        control, view_ui.WINDOW_HOLD_MS,
        "the screen follows about 5 s after the click")


def test_ui_actions_record_actual_failure_outcome(tmp_path: Path):
    viewer = view_ui.UiViewer.__new__(view_ui.UiViewer)
    viewer.run_path = tmp_path
    control = view_ui.Control("SD", "19.2", "button", "left", ("press",), "")

    viewer.record_ui_action(control, "press 19 04\n", ok=False,
                            error="connection refused")
    viewer.record_ui_action(control, "press 19 04\n", ok=True, reply="ok queued")

    rows = [json.loads(line) for line in (tmp_path / "actions.jsonl").read_text().splitlines()]
    assert rows[0]["ok"] is False
    assert rows[0]["error"] == "connection refused"
    assert rows[1]["ok"] is True
    assert rows[1]["reply"] == "ok queued"


def test_background_sd_lid_poll_logs_first_change_and_errors(tmp_path: Path):
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(nxs_panel=True)
    viewer.run_path = tmp_path
    viewer.control_note = Mock()
    viewer.last_control_error = None
    viewer.sd_lid_text = Mock()
    channel = Mock()
    channel.send.side_effect = ["ok sd-lid closed", "ok sd-lid closed",
                                "ok sd-lid open", "err unavailable"]
    viewer.control = Mock(return_value=channel)
    viewer.forget_control = Mock()

    assert viewer.sd_lid_command("state", background=True)
    assert viewer.sd_lid_command("state", background=True)
    assert viewer.sd_lid_command("state", background=True)
    assert not viewer.sd_lid_command("state", background=True)

    rows = [json.loads(line) for line in
            (tmp_path / "actions.jsonl").read_text().splitlines()]
    assert len(rows) == 3
    assert [row["ok"] for row in rows] == [True, True, False]
    assert [row["reply"] for row in rows] == [
        "ok sd-lid closed", "ok sd-lid open", "err unavailable"]


def test_explicit_sd_lid_state_request_is_always_logged(tmp_path: Path):
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(nxs_panel=True)
    viewer.run_path = tmp_path
    viewer.control_note = Mock()
    viewer.last_control_error = None
    viewer.sd_lid_text = Mock()
    channel = Mock()
    channel.send.return_value = "ok sd-lid closed"
    viewer.control = Mock(return_value=channel)

    assert viewer.sd_lid_command("state")
    assert viewer.sd_lid_command("state")

    rows = (tmp_path / "actions.jsonl").read_text().splitlines()
    assert len(rows) == 2


@pytest.mark.parametrize("reply", ["", "err queue full", "ERR queue full",
                                    "queued", "garbage response"])
def test_unrecognized_control_replies_are_failures(reply):
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(control_port=5984)
    viewer.control_note = Mock()
    channel = Mock()
    channel.send.return_value = reply
    viewer.control = Mock(return_value=channel)
    control = next(control for control in view_ui.controls()
                   if control.input_id == "16.0")

    assert viewer.send(control, "down 16 01\n") is None


def test_uppercase_ok_is_a_successful_control_reply():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(control_port=5984)
    viewer.control_note = Mock()
    channel = Mock()
    channel.send.return_value = "OK queued"
    viewer.control = Mock(return_value=channel)
    control = next(control for control in view_ui.controls()
                   if control.input_id == "16.0")

    assert viewer.send(control, "down 16 01\n") == "OK queued"


def test_nxs_reverse_contact_uses_logical_active_low_level():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(nxs_panel=True)
    viewer.send = Mock(return_value="ok level")
    viewer.deck = Mock()
    viewer.control_note = Mock()
    viewer.held, viewer.momentary, viewer.contact_sources = {}, {}, {}
    control = view_ui.Control("REV", "15.1", "button", "deck", (), "")

    assert viewer.contact(control, True)
    assert viewer.contact(control, False)
    assert [call.args[1] for call in viewer.send.call_args_list] == [
        "level 15 02 0\n", "level 15 02 1\n"]


def test_nxs_reverse_raw_inspector_stays_literal():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(nxs_panel=True)
    raw = next(control for control in view_ui.controls(True)
               if control.input_id == "15.1")

    assert viewer.contact_line(raw, True) == "level 15 02 1\n"
    assert viewer.contact_line(raw, False) == "level 15 02 0\n"


def test_nxs_reverse_close_forces_neutral_high_even_for_raw_inspector():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(nxs_panel=True)
    viewer.send = Mock(return_value="ok level")
    viewer.root = Mock()
    viewer.panel = None
    viewer.process = None
    viewer.log_stream = None
    viewer.diagnostics_executor = None
    raw = next(control for control in view_ui.controls(True)
               if control.input_id == "15.1")
    viewer.held, viewer.momentary = {(15, 2): raw}, {}
    viewer.contact_sources = {}

    viewer.close()

    assert viewer.send.call_args.args[1] == "level 15 02 1\n"


def test_nxs_reverse_click_releases_level_and_is_close_safe():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(nxs_panel=True)
    viewer.send = Mock(return_value="ok level")
    viewer.root = Mock()
    viewer.control_note = Mock()
    viewer.held, viewer.momentary, viewer.contact_sources = {}, {}, {}
    viewer.in_flight = {}
    control = view_ui.Control("REV", "15.1", "button", "deck", (), "")

    viewer.press(control, 100, "reverse")
    assert viewer.send.call_args.args[1] == "level 15 02 0\n"
    assert (15, 2) in viewer.momentary
    release = viewer.root.after.call_args.args[1]
    release()
    assert viewer.send.call_args.args[1] == "level 15 02 1\n"
    assert (15, 2) not in viewer.momentary


def test_failed_reply_does_not_mark_contact_as_held():
    viewer = object.__new__(view_ui.UiViewer)
    viewer.args = SimpleNamespace(control_port=5984, nxs_panel=False)
    viewer.control_note = Mock()
    viewer.held, viewer.momentary, viewer.contact_sources = {}, {}, {}
    viewer.deck = None
    channel = Mock()
    channel.send.return_value = "unexpected"
    viewer.control = Mock(return_value=channel)
    control = next(control for control in view_ui.controls()
                   if control.input_id == "16.0")

    viewer.toggle_hold(control)

    assert viewer.held == {}
    assert viewer.momentary == {}


def test_protocol_ok_requires_the_ok_token():
    assert view_ui.protocol_ok("ok state")
    assert view_ui.protocol_ok(" OK state ")
    assert not view_ui.protocol_ok("")
    assert not view_ui.protocol_ok("ERR state")
    assert not view_ui.protocol_ok(None)


def test_run_defaults_reuse_manifest_endpoint_and_profile(tmp_path: Path):
    (tmp_path / "run.json").write_text(json.dumps({
        "profile": "experimental NXS",
        "endpoints": {"panel_port": 6284},
    }))
    args = view_ui.parse_args(["--run", str(tmp_path)])
    assert args.run == tmp_path
    assert args.attach is True
    assert args.output == tmp_path / "screen.ppm"
    assert args.control_port == 6284
    assert args.nxs_panel is True


def test_explicit_run_viewer_options_win_over_manifest(tmp_path: Path):
    (tmp_path / "run.json").write_text(json.dumps({
        "profile": "experimental NXS",
        "endpoints": {"panel_port": 6284},
    }))
    output = tmp_path / "custom.ppm"
    args = view_ui.parse_args(["--attach", "--run", str(tmp_path),
                               "--output", str(output), "--control-port", "0"])
    assert args.output == output
    assert args.control_port == 0
    assert args.nxs_panel is True
