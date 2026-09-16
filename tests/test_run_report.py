import json

from tools.cdj_main.run_report import build_report


def _record(body: bytes) -> bytes:
    return b"SPRX" + len(body).to_bytes(4, "little") + body


def _list_reply(text: str, command: int = 0x10) -> bytes:
    # command, total, cursor, word5, highlight, flags, entries, then row
    words = [command, 0, 1, 0, 0, 0, 0, 0, 1, 0x55, 0, len(text)]
    words.extend(ord(char) for char in text)
    return b"".join(value.to_bytes(2, "little") for value in words)


def _status() -> bytes:
    words = [0] * 32
    words[13], words[18], words[19], words[26] = 1, 3, 0, 0x1000
    return b"".join(value.to_bytes(2, "little") for value in words)


def test_report_extracts_media_and_fault_evidence(tmp_path):
    (tmp_path / "run.json").write_text(json.dumps({"profile": "experimental NXS",
        "media": {"images": {"sd_image": "/tmp/card.img"}}}))
    (tmp_path / "main-link.bin").write_bytes(
        _record(_status()) + _record(_list_reply("TESTTONE.WAV")) +
        _record(_list_reply("NO CARD")) + _record(_list_reply("TRACK 01", 5)))
    (tmp_path / "main-stderr.log").write_text(
        "phase budget exhausted\n"
        "qemu: nxs-c674x pc=0xc0051100 word=0x903d5b "
        "stop=instruction not implemented B15=0x11805ad8\n")
    report = build_report(tmp_path)
    assert report["profile"] == "experimental NXS"
    assert report["media"]["wav_entries"] == ["TESTTONE.WAV"]
    assert report["media"]["no_card_entries"] == 1
    assert report["media"]["load_responses"] == 1
    assert report["interpretation"] == {
        "track_list_observed": True,
        "native_load_response_observed": True,
        "native_counter_playback_proven": False,
        "audio_playback_proven": False,
    }
    assert report["faults"][0]["file"] == "main-stderr.log"
    assert report["fault_count"] == 1
    assert "instruction not implemented" in report["faults"][0]["line"]


def test_report_handles_an_incomplete_run(tmp_path):
    report = build_report(tmp_path)
    assert report["link"]["status"] == "missing"
    assert report["media"]["wav_entries"] == []
    assert not report["interpretation"]["track_list_observed"]


def test_report_exposes_fault_only_dsp_capture_metadata(tmp_path):
    (tmp_path / "run.json").write_text(json.dumps({
        "dsp_capture_enabled": True,
        "dsp_capture_mode": "fault-only",
        "dsp_event_capture_enabled": False,
        "dsp_checkpoint_policy": "fault",
    }))
    checkpoint_dir = tmp_path / "dsp-checkpoints"
    checkpoint_dir.mkdir()
    (checkpoint_dir / "manifest.json").write_text(json.dumps({
        "complete": False,
        "architectural_validation_eligible": False,
    }))
    capture = build_report(tmp_path)["dsp_capture"]
    assert capture == {
        "dsp_capture_enabled": True,
        "dsp_capture_mode": "fault-only",
        "dsp_event_capture_enabled": False,
        "dsp_checkpoint_policy": "fault",
        "checkpoint_manifest_exists": True,
        "checkpoint_manifest_complete": False,
        "checkpoint_architectural_validation_eligible": False,
    }


def test_report_marks_successful_fresh_counter_playback(tmp_path):
    (tmp_path / "actions.jsonl").write_text(
        json.dumps({"command": "wait-playback", "outcome": "advanced",
                    "ok": True, "advanced_frames": 150}) + "\n")
    report = build_report(tmp_path)
    assert report["interpretation"]["native_counter_playback_proven"] is True
    assert report["interpretation"]["audio_playback_proven"] is False


def test_report_streams_long_logs_and_bounds_rows_without_losing_counts(tmp_path, monkeypatch):
    from pathlib import Path

    (tmp_path / 'main-link.bin').write_bytes(
        _record(_list_reply('TESTTONE.WAV')) +
        (_record(_status()) + _record(_list_reply('NO CARD'))) * 1200 +
        b'SPRX\xff\xff\xff\xff' + _record(_list_reply('LAST.WAV')) +
        _record(_list_reply('PARTIAL.WAV'))[:-5])
    (tmp_path / 'gui.log').write_text('fatal diagnostic\n' * 1001)

    def reject_bulk_read(*args, **kwargs):
        raise AssertionError('binary evidence must be streamed')

    monkeypatch.setattr(Path, 'read_bytes', reject_bulk_read)
    report = build_report(tmp_path)
    assert report['media']['entry_count'] == 1202
    assert len(report['media']['entries_seen']) == 100
    assert report['media']['no_card_entries'] == 1200
    assert report['media']['wav_entries'] == ['LAST.WAV', 'TESTTONE.WAV']
    assert report['link_status_count'] == 1200
    assert len(report['link']['statuses']) == 100
    assert report['fault_count'] == 1001
    assert len(report['faults']) == 100
