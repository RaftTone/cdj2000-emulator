"""Agent observations stay bounded and distinguish stopped runs from booting."""
import json
import struct

import pytest

from tools.cdj_main import run_state


def record(words):
    body = struct.pack('<' + 'H' * len(words), *words)
    return b'SPRX' + len(body).to_bytes(4, 'little') + body


def browser(text):
    return record([0x11, 0, 1, 0, 0, 0, 0, 0, 1, 0x55, 0, len(text),
                   *map(ord, text)])


def test_incremental_observer_retains_split_records_and_resynchronizes(tmp_path):
    path = tmp_path / 'main-link.bin'
    payload = browser('TESTTONE.WAV')
    path.write_bytes(b'noise' + payload[:-4])
    observer = run_state.LinkObserver()
    first = observer.poll(path, budget=100)
    assert first['records'] == 0
    with path.open('ab') as stream:
        stream.write(payload[-4:])
    final = observer.poll(path)
    assert final['browser']['rows'][0]['text'] == 'TESTTONE.WAV'
    assert final['records'] == 1
    assert final['skipped_bytes'] == 5
    assert final['pending_bytes'] == 0


def test_observer_reports_backlog_and_resets_when_file_replaced(tmp_path):
    path = tmp_path / 'main-link.bin'
    path.write_bytes(browser('NO CARD') + browser('TESTTONE.WAV'))
    observer = run_state.LinkObserver()
    first = observer.poll(path, budget=len(browser('NO CARD')))
    assert not first['caught_up']
    assert 'earlier' in run_state.progress_text(first)
    assert observer.poll(path)['browser']['rows'][0]['text'] == 'TESTTONE.WAV'
    replacement = tmp_path / 'replacement'
    replacement.write_bytes(browser('NEW'))
    replacement.replace(path)
    final = observer.poll(path)
    assert final['records'] == 1
    assert final['browser']['rows'][0]['text'] == 'NEW'


def test_one_shot_observe_reads_latest_tail_of_large_link_dump(tmp_path):
    path = tmp_path / 'main-link.bin'
    old = browser('OLD.WAV')
    latest = browser('LATEST.WAV')
    # Keep the fixture comfortably above the one MiB observer budget.  The
    # current browser reply must win even though startup history is much larger.
    path.write_bytes(old * 30000 + latest)
    result = run_state.observe(tmp_path)
    link = result['link']
    assert link['browser']['rows'][0]['text'] == 'LATEST.WAV'
    assert link['caught_up']
    assert link['bytes_read'] <= 1024 * 1024
    assert link['skipped_prefix_bytes'] > 0
    assert 'local to tail' in link['record_scope']
    assert link['records'] < 30001


def test_explicit_observer_keeps_incremental_full_file_scope(tmp_path):
    (tmp_path / 'main-link.bin').write_bytes(browser('TRACK.WAV'))
    observer = run_state.LinkObserver()
    link = run_state.observe(tmp_path, observer)['link']
    assert link['browser']['rows'][0]['text'] == 'TRACK.WAV'
    assert link['record_scope'].startswith('incremental full file')
    assert link['skipped_prefix_bytes'] == 0


def test_observer_survives_malformed_decodable_record_and_reports_it(tmp_path):
    path = tmp_path / 'main-link.bin'
    # A list command with a header claiming one row but no row payload.
    malformed = record([0x11, 0, 1, 0, 0, 0, 0, 0, 1])
    path.write_bytes(malformed + browser('RECOVERED.WAV'))
    result = run_state.observe(tmp_path)
    assert result['link']['malformed_records'] == 1
    assert result['link']['browser']['rows'][0]['text'] == 'RECOVERED.WAV'


def test_failed_startup_exposes_reason_endpoints_and_diagnostics(tmp_path):
    run_state.write_json(tmp_path / 'run.json', {
        'endpoints': {'panel_port': 6284}, 'debug': {'enabled': True},
        'media': {'images': {'sd_image': '/fixture.img'}}})
    run_state.write_json(tmp_path / 'session.json', {'state': 'failed'})
    run_state.write_json(tmp_path / 'result.json', {'error': 'MAIN exited'})
    (tmp_path / 'main-stderr.log').write_text('Failed to bind socket: Operation not permitted\n')
    result = run_state.observe(tmp_path)
    assert result['progress'] == 'Run failed: MAIN exited'
    assert result['endpoints']['panel_port'] == 6284
    assert result['debug']['enabled']
    assert result['recent_fault_lines'][0]['file'] == 'main-stderr.log'


def test_stopped_session_and_frame_publication_are_separate(tmp_path):
    run_state.write_json(tmp_path / 'session.json', {'state': 'stopped'})
    (tmp_path / 'screen.ppm').write_bytes(b'P6\n1 1\n255\n\x01\x02\x03')
    result = run_state.observe(tmp_path)
    assert result['progress'].startswith('Run stopped.')
    assert result['frame']['status'] == 'captured'
    assert result['frame']['age_seconds'] >= 0


def test_action_tail_tolerates_partial_json_and_is_bounded(tmp_path):
    for index in range(30):
        run_state.record_action(tmp_path, {'action': 'press', 'index': index})
    with (tmp_path / 'actions.jsonl').open('a') as stream:
        stream.write('{"partial":')
    actions = run_state.recent_actions(tmp_path)
    assert len(actions) == 20
    assert actions[0]['index'] == 10 and actions[-1]['index'] == 29
    assert actions[-1]['unix'] > 0


def test_periodic_dsp_budget_yield_is_not_a_fault(tmp_path):
    (tmp_path / 'main-stderr.log').write_text(
        'stop=phase budget exhausted\n'
        'stop=deferred slice boundary\n'
        'stop=HINT host-event yield\n' * 1000)
    result = run_state.observe(tmp_path)
    assert result['recent_fault_lines'] == []
    assert len(result['log_tails']['main-stderr.log']) <= 2048


def test_running_session_surfaces_a_halted_dsp(tmp_path):
    run_state.write_json(tmp_path / 'session.json', {'state': 'running'})
    (tmp_path / 'main-stderr.log').write_text(
        'nxs-c674x: pc=0xc0051100 word=0x903d5b stop=instruction not implemented B15=0x1\n')
    result = run_state.observe(tmp_path)
    assert result['progress'].startswith('Emulator fault reported:')
    assert 'instruction not implemented' in result['progress']
    assert result['next_steps'][0].startswith('Capture evidence:')


def test_next_steps_for_stopped_run_review_report():
    steps = run_state.next_steps({}, {'state': 'stopped'})
    assert steps == [
        'Review completed evidence: python -m tools.cdj_main.run_report <run-dir> --json'
    ]


def test_next_steps_for_missing_browser_are_non_definitive():
    steps = run_state.next_steps({}, {'state': 'running'})
    assert any('wait-media' in step for step in steps)
    assert any('inconclusive' in step for step in steps)


def test_next_steps_for_transport_request_fresh_motion():
    steps = run_state.next_steps({'transport': {'remaining_seconds': 1}}, {'state': 'running'})
    assert any('wait-playback' in step for step in steps)


def test_dsp_instruction_halt_is_reported_as_fault(tmp_path):
    (tmp_path / 'main-stderr.log').write_text(
        'qemu: nxs-c674x pc=0xc0051100 word=0x903d5b '
        'stop=instruction not implemented B15=0x11805ad8\n')
    result = run_state.observe(tmp_path)
    assert result['recent_fault_lines'][0]['file'] == 'main-stderr.log'
    assert 'instruction not implemented' in result['recent_fault_lines'][0]['line']


def test_missing_run_cli_is_error(tmp_path):
    with pytest.raises(SystemExit) as error:
        run_state.main([str(tmp_path / 'missing')])
    assert error.value.code == 2


def test_native_transport_counter_decodes_fractional_seconds():
    words = [0] * 32
    words[5:9] = [0, (9 << 8) | 75, 0, 10 << 8]
    result = run_state.transport_observation({'record': 42, 'words': words})
    assert result['record'] == 42
    assert result['remaining_seconds'] == 9.5
    assert result['duration_seconds'] == 10
    assert result['elapsed_frames'] == 75
    assert run_state.progress_text({'transport': result}) == \
        'Native transport paused/stopped: 9.500 s remaining / 10.000 s duration.'


def test_native_play_request_flag_is_separate_from_counter_motion():
    words = [0] * 18
    words[6] = words[8] = 10 << 8
    words[17] = 0x200
    result = run_state.transport_observation({'record': 1, 'words': words})
    assert result['play_requested'] is True
    assert result['elapsed_frames'] == 0
    words[17] = 0
    assert run_state.transport_observation({'words': words})['play_requested'] is False


@pytest.mark.parametrize('time_words', [
    [0xffff, 0, 0, 10 << 8], [0, 0, 0, 0],
    [0, 11 << 8, 0, 10 << 8], [0, (9 << 8) | 150, 0, 10 << 8],
    [0, 60 << 8, 1, 0],
])
def test_native_transport_rejects_unloaded_or_invalid_counters(time_words):
    words = [0] * 32
    words[5:9] = time_words
    assert run_state.transport_observation({'words': words}) is None
