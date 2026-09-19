"""End-to-end gates for connected-event injection into DSP checkpoints."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_newest_checkpoint_filters_modes_and_transcript(tmp_path):
    from tools.cdj_dsp.replay import newest_checkpoint

    _, original = make_checkpoint(tmp_path)
    payload = original.read_bytes()
    candidates = tmp_path / 'candidates'
    for index, (name, timing, audio, transcript) in enumerate([
            ('strict', 'strict', 'stopped-clock', 'wanted'),
            ('other_events', 'strict', 'stopped-clock', 'other'),
            ('audio', 'strict', 'coarse-packet-slots', 'wanted'),
            ('exploratory', 'functional-runahead', 'stopped-clock', 'wanted')]):
        directory = candidates / name
        directory.mkdir(parents=True)
        path = directory / 'state.cdjdsp'
        path.write_bytes(payload)
        os.utime(path, ns=(index + 1, index + 1))
        (directory / 'manifest.json').write_text(json.dumps({
            'complete': True,
            'checkpoints': [{'file': path.name,
                             'sha256': hashlib.sha256(payload).hexdigest()}],
            'dsp_timing_mode': timing, 'dsp_audio_mode': audio,
            'event_transcript': {'sha256': transcript},
        }))
    selected, *_ = newest_checkpoint(candidates, timing_mode='strict',
                                     audio_mode='stopped-clock', event_hash='wanted')
    assert selected.parent.name == 'strict'
    selected, *_ = newest_checkpoint(candidates, timing_mode='functional-runahead',
                                     audio_mode='stopped-clock', event_hash='wanted')
    assert selected.parent.name == 'exploratory'
    with pytest.raises(ValueError, match='no compatible'):
        newest_checkpoint(candidates, event_hash='absent')


def encoded_event(sequence, kind, *, offset=0, address=0, value=0, size=0,
                  boot_phase=0, hint=False, dspint=False, packets=0, cycles=0):
    event = dict(sequence=sequence, event=kind, offset=offset, address=address,
                 value=value, size=size, boot_phase=boot_phase, hint=hint,
                 dspint=dspint, packets=packets, cycles=cycles)
    return json.dumps(event, separators=(',', ':')) + '\n'


def make_checkpoint(tmp_path, stop_reason='boot-phase boundary'):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    checkpoint_dir = tmp_path / 'checkpoints'
    checkpoint_dir.mkdir()
    checkpoint = checkpoint_dir / 'state.cdjdsp'
    maker = tmp_path / 'checkpoint-maker'
    subprocess.run([
        cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'emulator/qemu'),
        str(ROOT / 'tests/cstub/dsp-event-checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_checkpoint.c'),
        str(ROOT / 'emulator/qemu/cdj_dsp_scheduler.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_syscfg.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_intc.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_timer.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_spi.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_cache.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_mcasp.c'),
        str(ROOT / 'emulator/qemu/cdj_c6747_edma.c'),
        '-o', str(maker), '-lm',
    ], check=True)
    subprocess.run([str(maker), str(checkpoint), stop_reason],
                   check=True, timeout=5)
    return checkpoint_dir, checkpoint


def make_running_checkpoint(tmp_path):
    """Replace the deliberate fault with one full-width NOP and re-checksum."""
    from tools.cdj_dsp.replay import CHECKPOINT_HEADER, _fnv1a

    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    raw = bytearray(checkpoint.read_bytes())
    fields = list(CHECKPOINT_HEADER.unpack_from(raw))
    header_size, state_size = fields[3], fields[4]
    raw[header_size + state_size + 0x20:
        header_size + state_size + 0x24] = bytes(4)
    fields[20] = _fnv1a(memoryview(raw)[header_size:])
    CHECKPOINT_HEADER.pack_into(raw, 0, *fields)
    checkpoint.write_bytes(raw)
    return checkpoint_dir, checkpoint


def test_accepts_hashmatched_fault_only_checkpoint_as_diagnostic_provenance(tmp_path):
    from tools.cdj_dsp.replay import checkpoint_info, checkpoint_provenance, newest_checkpoint

    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    digest = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': False,
        'architectural_validation_eligible': False,
        'dsp_checkpoint_policy': 'fault',
        'checkpoint_capture_complete': True,
        'checkpoints': [{'file': checkpoint.name, 'sha256': digest}],
    }))
    info = checkpoint_info(checkpoint.read_bytes())
    provenance = checkpoint_provenance(checkpoint, checkpoint.read_bytes(), info)
    assert provenance['origin'] == 'diagnostic_connected_checkpoint'
    selected, *_ = newest_checkpoint(checkpoint_dir)
    assert selected == checkpoint


def test_diagnostic_checkpoint_rejects_event_injection_even_with_forged_hash(
        tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(encoded_event(1, 'boot_phase'))
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': False,
        'architectural_validation_eligible': False,
        'dsp_checkpoint_policy': 'fault',
        'checkpoint_capture_complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(tmp_path / 'replay'), '--events', str(transcript),
    ], cwd=ROOT, text=True, capture_output=True, timeout=10)
    assert result.returncode != 0
    assert 'diagnostic fault-only checkpoint' in result.stderr


@pytest.mark.parametrize('change', [
    {'checkpoints': [{'file': 'state.cdjdsp', 'sha256': 'wrong'}]},
    {'checkpoint_capture_complete': False},
])
def test_rejects_fault_only_checkpoint_without_hashmatched_complete_snapshot_proof(
        tmp_path, change):
    from tools.cdj_dsp.replay import checkpoint_info, checkpoint_provenance

    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    manifest = {
        'complete': False,
        'architectural_validation_eligible': False,
        'dsp_checkpoint_policy': 'fault',
        'checkpoint_capture_complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
    }
    manifest.update(change)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps(manifest))
    data = checkpoint.read_bytes()
    with pytest.raises(ValueError, match='absent from a complete connected manifest'):
        checkpoint_provenance(checkpoint, data, checkpoint_info(data))


def test_injects_and_repeat_gates_connected_stop(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)

    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800020,
                      value=0xdeadcafe)
    )
    checkpoint_hash = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    event_hash = hashlib.sha256(transcript.read_bytes()).hexdigest()
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{'file': checkpoint.name, 'sha256': checkpoint_hash}],
        'event_transcript': {'sha256': event_hash},
    }))

    output = tmp_path / 'replay'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint), str(output),
        '--steps', '100', '--events', str(transcript), '--verify-repeat',
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    gate = json.loads((output / 'gate.json').read_text())
    assert gate['passed']
    assert gate['verified_connected_stops'] == 1
    assert gate['repeat_matches'] and gate['final_state_and_memory_match']
    failure = json.loads((output / 'failure.json').read_text())
    assert failure['outcome'] == 'fail_closed_fault'
    assert failure['stop']['fault']
    assert failure['stop']['reason'] == failure['stop']['fault']


def test_rejects_event_transcript_without_manifest_provenance(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
    }))
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(encoded_event(1, 'boot_phase'))
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(tmp_path / 'replay'), '--events', str(transcript),
    ], cwd=ROOT, text=True, capture_output=True, timeout=10)
    assert result.returncode != 0
    assert 'event-transcript provenance' in result.stderr


def test_connected_stop_limit_preserves_verified_boundary(tmp_path):
    checkpoint_dir, checkpoint = make_running_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800024, packets=1, cycles=1) +
        encoded_event(3, 'reset_assert'))
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{'file': checkpoint.name,
                        'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest()}],
        'event_transcript': {'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest()},
    }))
    output = tmp_path / 'bounded'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint), str(output),
        '--steps', '1', '--events', str(transcript), '--connected-stops', '1',
        '--observe-pcm', '--verify-repeat'], cwd=ROOT, text=True,
        capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    gate = json.loads((output / 'gate.json').read_text())
    assert gate['passed'] and gate['verified_connected_stops'] == 1
    assert gate['final_state_and_memory_match']
    manifest = json.loads((output / 'manifest.json').read_text())
    assert manifest['connected_stops'] == 1 and manifest['observe_pcm']
    assert (output / 'final.cdjdsp').is_file()


def test_rejects_connected_stop_state_mismatch(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800020,
                      value=0xdeadcafe, packets=1)
    )
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(tmp_path / 'replay'), '--steps', '100', '--events', str(transcript),
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode != 0
    assert 'event replay mismatch at sequence 2 (dsp_stop)' in result.stderr
    assert 'observed pc=' in result.stderr and '; expected pc=' in result.stderr


def test_later_dspint_alone_resumes_event_replay(tmp_path):
    checkpoint_dir, checkpoint = make_checkpoint(
        tmp_path, stop_reason='phase budget exhausted')
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'hpi_host_control_write', address=0x11800000,
                      value=2, size=4, dspint=True) +
        encoded_event(3, 'dsp_stop', address=0x118001e0,
                      value=0xdeadcafe, dspint=True, packets=9, cycles=9)
    )
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    output = tmp_path / 'replay'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(output), '--steps', '100', '--events', str(transcript),
        '--verify-repeat',
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    gate = json.loads((output / 'gate.json').read_text())
    assert gate['passed'] and gate['verified_connected_stops'] == 1
    assert gate['repeat_matches'] and gate['final_state_and_memory_match']


@pytest.mark.parametrize('tail,budget_args,reason', [
    (encoded_event(2, 'boot_phase', value=1, boot_phase=1),
     ['--steps', '1'], 'step_limit'),
    (encoded_event(2, 'dsp_stop', address=0x11800024, packets=2, cycles=2),
     ['--steps', '1'], 'step_limit'),
    (encoded_event(2, 'boot_phase', value=1, boot_phase=1),
     ['--steps', '10', '--packets', '1'], 'packet_limit'),
    (encoded_event(2, 'boot_phase', value=1, boot_phase=1),
     ['--steps', '10', '--cycles', '1'], 'cycle_limit'),
])
def test_event_budget_exhaustion_is_explicit_and_nonresumable(
        tmp_path, tail, budget_args, reason):
    checkpoint_dir, checkpoint = make_running_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(encoded_event(1, 'boot_phase') + tail)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    output = tmp_path / 'replay'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(output), *budget_args, '--events', str(transcript),
        '--verify-repeat',
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 1
    assert 'event budget exhausted' in result.stderr.lower()
    assert 'event replay mismatch' not in result.stderr
    failure = json.loads((output / 'failure.json').read_text())
    assert failure['outcome'] == 'event_budget_exhausted'
    assert failure['diagnostic']['reason'] == reason
    assert failure['diagnostic']['next_sequence'] == 2
    assert failure['resumable_checkpoint'] is None
    manifest = json.loads((output / 'manifest.json').read_text())
    assert manifest['complete'] and manifest['validation_incomplete']
    assert manifest['output_checkpoint'] is None
    gate = json.loads((output / 'gate.json').read_text())
    assert not gate['passed'] and gate['repeat_not_run']
    assert not (output / 'final.cdjdsp').exists()
    assert not (output / 'coverage.json').exists()


def test_limit_coincident_with_exact_recorded_stop_remains_resumable(tmp_path):
    checkpoint_dir, checkpoint = make_running_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800024,
                      packets=1, cycles=1)
    )
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    output = tmp_path / 'replay'
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(output), '--steps', '1', '--events', str(transcript),
        '--verify-repeat',
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    gate = json.loads((output / 'gate.json').read_text())
    assert gate['passed'] and gate['verified_connected_stops'] == 1
    assert gate['final_state_and_memory_match']
    assert (output / 'final.cdjdsp').is_file()
    assert not (output / 'failure.json').exists()


@pytest.mark.parametrize('packets,cycles', [
    (1, 1),  # Exact counters but a bad PC is genuine divergence.
    (0, 0),  # Backward/corrupt counters are never explained by a limit.
])
def test_event_budget_does_not_mask_recorded_stop_divergence(
        tmp_path, packets, cycles):
    checkpoint_dir, checkpoint = make_running_checkpoint(tmp_path)
    transcript = tmp_path / 'events.jsonl'
    transcript.write_text(
        encoded_event(1, 'boot_phase') +
        encoded_event(2, 'dsp_stop', address=0x11800028,
                      packets=packets, cycles=cycles)
    )
    (checkpoint_dir / 'manifest.json').write_text(json.dumps({
        'complete': True,
        'checkpoints': [{
            'file': checkpoint.name,
            'sha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        }],
        'event_transcript': {
            'sha256': hashlib.sha256(transcript.read_bytes()).hexdigest(),
        },
    }))
    result = subprocess.run([
        sys.executable, '-m', 'tools.cdj_dsp.replay', str(checkpoint),
        str(tmp_path / 'replay'), '--steps', '1', '--events', str(transcript),
    ], cwd=ROOT, text=True, capture_output=True, timeout=30)
    assert result.returncode != 0
    assert 'event replay mismatch at sequence 2 (dsp_stop)' in result.stderr
    assert 'event budget exhausted' not in result.stderr.lower()
