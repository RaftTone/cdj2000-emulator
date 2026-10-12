import pytest

from tools.cdj_main.media_readiness import (parse_word, readiness_snapshot,
                                            source_key_ready)


def test_parse_word_ignores_hmp_echo():
    assert parse_word("xp /1wx 0x04cf2180\n04cf2180: 0x00000003\n") == 3


def test_readiness_reports_arms_latch_and_browse_limit():
    values = {0x04CF2180: 3, 0x04CF2994: 0, 0x04CF222C: 1,
              0x049832EC: 1, 0xFFE4001C: 0, 0x051E21D0: 0, 0x049832F0: 0}
    result = readiness_snapshot(values.__getitem__)
    assert result["ready"] is True
    assert result["mount_latched"] is True
    assert result["card_present"] is True
    assert result["browse_proven"] is False
    assert result["ok"] is True
    assert result["browser_source_ready"] is False
    assert source_key_ready(result) is False


def test_sd_source_key_waits_for_browser_table_not_mount_gate():
    values = {0x04CF2180: 2, 0x04CF2994: 1, 0x04CF222C: 0,
              0x049832EC: 1, 0xFFE4001C: 0, 0x051E21D0: 0,
              0x049832F0: 0}
    intermediate = readiness_snapshot(values.__getitem__)
    assert intermediate['gate_ready'] is True
    assert intermediate['browser_source_ready'] is False
    assert source_key_ready(intermediate) is False
    values[0x04CF2180] = 3
    values[0x04CF2994] = 2
    ready = readiness_snapshot(values.__getitem__)
    assert ready['browser_source_ready'] is True
    assert source_key_ready(ready) is True


def test_readiness_is_not_ok_without_card_even_if_firmware_arm_is_true():
    values = {0x04CF2180: 3, 0x04CF2994: 0, 0x04CF222C: 1,
              0x049832EC: 1, 0xFFE4001C: 0x20, 0x051E21D0: 0,
              0x049832F0: 0}
    result = readiness_snapshot(values.__getitem__)
    assert result["ready"] is True
    assert result["ok"] is False


def test_readiness_rejects_malformed_memory_reply():
    with pytest.raises(ValueError):
        parse_word("(qemu) ")


def test_all_false_readiness_arms_cannot_pass_even_with_card_and_latch():
    values = {0x04CF2180: 4, 0x04CF2994: 0, 0x04CF222C: 0,
              0x049832EC: 1, 0xFFE4001C: 0, 0x051E21D0: 2, 0x049832F0: 0}
    result = readiness_snapshot(values.__getitem__)
    assert not result['ready']
    assert not result['ok']


def test_observe_run_times_out_without_mutating_guest(tmp_path, monkeypatch):
    import json
    from unittest.mock import MagicMock
    from tools.cdj_main import media_readiness as media
    (tmp_path / 'run.json').write_text(json.dumps({
        'profile': 'experimental NXS', 'endpoints': {'qmp': 'qmp.sock'}}))
    client = MagicMock()
    client.__enter__.return_value = client
    # Mode 4 with no table entry means not ready; no card, no mount latch.
    def command(name, arguments):
        assert name == 'human-monitor-command'
        address = int(arguments['command-line'].split()[-1], 16)
        value = {media.MODE_STATE: 4, media.SDHI_INFO1: 32}.get(address, 0)
        return f'{address:08x}: 0x{value:08x}\n'
    client.command.side_effect = command
    monkeypatch.setattr(media, 'Qmp', lambda *a, **kw: client)
    now = [0.0]
    monkeypatch.setattr(media.time, 'monotonic', lambda: now[0])
    monkeypatch.setattr(media.time, 'sleep', lambda delay: now.__setitem__(0, now[0] + delay))
    result = media.observe_run(tmp_path, timeout=2, poll=1)
    assert not result['ok']
    assert now[0] == 2
    assert client.command.call_count == 14
    now[0] = 0
    client.reset_mock()
    assert not media.observe_run(tmp_path, timeout=0)['ok']
    assert client.command.call_count == 7


def test_observe_run_source_key_waits_past_mount_gate(tmp_path, monkeypatch):
    import json
    from unittest.mock import MagicMock
    from tools.cdj_main import media_readiness as media
    (tmp_path / 'run.json').write_text(json.dumps({
        'profile': 'experimental NXS', 'endpoints': {'qmp': 'qmp.sock'}}))
    client = MagicMock()
    client.__enter__.return_value = client
    phase = [0]

    def command(name, arguments):
        address = int(arguments['command-line'].split()[-1], 16)
        values = {media.MODE_STATE: 2 if phase[0] == 0 else 3,
                  media.TABLE_ENTRY: 0 if phase[0] == 0 else 2,
                  media.LATCH: 1}
        return f'{address:08x}: 0x{values.get(address, 0):08x}\n'

    client.command.side_effect = command
    monkeypatch.setattr(media, 'Qmp', lambda *a, **kw: client)
    now = [0.0]
    monkeypatch.setattr(media.time, 'monotonic', lambda: now[0])

    def sleep(delay):
        now[0] += delay
        phase[0] = 1

    monkeypatch.setattr(media.time, 'sleep', sleep)
    result = media.observe_run(tmp_path, timeout=2, poll=1, for_source_key=True)
    assert result['mode_state'] == 3
    assert result['table_entry'] == 2
    assert source_key_ready(result)
    assert now[0] == 1
    assert client.command.call_count == 14


def test_observe_rejects_wrong_profile_before_connecting(tmp_path, monkeypatch):
    from tools.cdj_main import media_readiness as media
    (tmp_path / 'run.json').write_text('{"profile": "legacy"}')
    monkeypatch.setattr(media, 'Qmp', lambda *a, **kw: pytest.fail('must not connect'))
    with pytest.raises(ValueError, match='NXS profile'):
        media.observe_run(tmp_path)


def test_dev_wait_media_returns_predicate_exit_status_and_records(tmp_path, monkeypatch, capsys):
    import json
    from tools.cdj_main import dev, media_readiness
    (tmp_path / 'session.json').write_text('{"state": "running"}')
    monkeypatch.setattr(media_readiness, 'observe_run', lambda *a, **kw: {'ok': False, 'ready': True, 'card_present': False})
    assert dev.main([str(tmp_path), 'wait-media', '--timeout', '0']) == 1
    assert not json.loads(capsys.readouterr().out)['ok']
    assert json.loads((tmp_path / 'actions.jsonl').read_text())['outcome'] == 'timeout'


def test_usb_source_uses_slot_three_and_only_value_two_is_ready():
    from tools.cdj_main import media_readiness as media
    assert media.TABLE_BASE + 2 * media.SLOT_STRIDE == media.TABLE_ENTRY
    assert media.TABLE_BASE + 3 * media.SLOT_STRIDE == media.USB_TABLE_ENTRY
    values = {media.USB_TABLE_ENTRY: 1}
    intermediate = media.readiness_snapshot(values.__getitem__, "usb")
    assert intermediate["source"] == "usb"
    assert intermediate["slot"] == 3
    assert intermediate["intermediate"] is True
    assert intermediate["ready"] is False
    assert intermediate["ok"] is False
    values[media.USB_TABLE_ENTRY] = 2
    ready = media.readiness_snapshot(values.__getitem__, "usb")
    assert ready["ready"] is True
    assert ready["ok"] is True
    assert source_key_ready(ready) is True


def test_usb_snapshot_does_not_read_sd_mount_globals():
    from tools.cdj_main import media_readiness as media
    seen = []

    def read(address):
        seen.append(address)
        return 2 if address == media.USB_TABLE_ENTRY else 0

    result = media.readiness_snapshot(read, "usb")
    assert result["ok"] is True
    assert seen == [media.USB_TABLE_ENTRY]


def test_observe_run_usb_waits_on_source_entry_only(tmp_path, monkeypatch):
    import json
    from unittest.mock import MagicMock
    from tools.cdj_main import media_readiness as media
    (tmp_path / "run.json").write_text(json.dumps({
        "profile": "experimental NXS", "endpoints": {"qmp": "qmp.sock"}}))
    client = MagicMock()
    client.__enter__.return_value = client
    client.command.return_value = "04cf2a34: 0x00000001\n"
    monkeypatch.setattr(media, "Qmp", lambda *a, **kw: client)
    result = media.observe_run(tmp_path, timeout=0, source="usb")
    assert result["intermediate"] is True
    assert result["ok"] is False
    assert client.command.call_count == 1
    assert "0x4cf2a34" in client.command.call_args.args[1]["command-line"]
