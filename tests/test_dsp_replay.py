"""The replay tool must distinguish faults, limits and pre-execution stops."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import pytest

from tools.cdj_dsp.replay import DEFAULT_FORMATS, write_manifest
from tools.cdj_dsp.tx_capture import tx_capture_metadata

ROOT = Path(__file__).resolve().parents[1]


def test_manifest_never_claims_eligibility_for_an_unfinished_run(tmp_path):
    """Six run directories were left claiming this by a pre-execution write."""
    for complete in (None, False):
        manifest = dict(architectural_validation_eligible=True)
        if complete is not None:
            manifest['complete'] = complete
        write_manifest(tmp_path, manifest)
        written = json.loads((tmp_path / 'manifest.json').read_text())
        assert written['architectural_validation_eligible'] is False
    manifest = dict(complete=True, architectural_validation_eligible=True)
    write_manifest(tmp_path, manifest)
    written = json.loads((tmp_path / 'manifest.json').read_text())
    assert written['architectural_validation_eligible'] is True


def test_aborted_replay_leaves_no_manifest(tmp_path, monkeypatch):
    """A replay binary that exits non-zero must leave no manifest behind.

    The manifest used to be written before execution, so an aborted run kept a
    manifest that claimed architectural validation eligibility.
    """
    from tools.cdj_dsp import replay as replay_module

    _require_replay_toolchain()

    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    struct.pack_into('<I', data, 0x20, (3 << 23) | (123 << 7) | 0x28)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    output = tmp_path / 'aborted'
    real_run = replay_module.subprocess.run

    def fail_the_replay(command, *args, **kwargs):
        # Leave the compiler alone; only the replay binary itself fails.
        if Path(command[0]).name == 'replay':
            return subprocess.CompletedProcess(command, 1)
        return real_run(command, *args, **kwargs)

    monkeypatch.setattr(replay_module.subprocess, 'run', fail_the_replay)
    monkeypatch.setattr(sys, 'argv', ['replay', str(dump), str(output),
                                      '--steps', '1'])
    with pytest.raises(subprocess.CalledProcessError):
        replay_module.main()
    assert output.is_dir() and not (output / 'manifest.json').exists()


def test_compact_trace_preserves_execution_and_coverage(tmp_path, monkeypatch):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    struct.pack_into('<II', data, 0x20, (3 << 23) | (123 << 7) | 0x28, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    # The CLI owns its mode even if the parent has a different environment.
    monkeypatch.setenv('CDJ_DSP_COMPACT_TRACE', '1')
    for mode in ('detailed', 'compact'):
        output = tmp_path / mode
        result = run(dump, output, '--trace-mode', mode, '--verify-repeat')
        assert result.returncode == 0, result.stderr
        gate = json.loads((output / 'gate.json').read_text())
        assert gate['passed'] and gate['trace_mode'] == mode
    detailed = [json.loads(line) for line in (tmp_path / 'detailed/trace.jsonl').read_text().splitlines()]
    compact = [json.loads(line) for line in (tmp_path / 'compact/trace.jsonl').read_text().splitlines()]
    assert any(event['event'] == 'step' for event in detailed)
    assert compact == [event for event in detailed if event['event'] != 'step']
    assert (tmp_path / 'detailed/final.cdjdsp').read_bytes() == (tmp_path / 'compact/final.cdjdsp').read_bytes()
    reports = []
    for mode in ('detailed', 'compact'):
        report = json.loads((tmp_path / mode / 'coverage.json').read_text())
        report.pop('sha256')
        reports.append(report)
    assert reports[0] == reports[1]


def _require_replay_toolchain():
    import shutil
    if not DEFAULT_FORMATS.is_file():
        pytest.skip('requires GNU tic6x-insn-formats.h from a GDB 17.2 build')
    if not (shutil.which('cc') or shutil.which('gcc')):
        pytest.skip('requires C compiler')


def run(dump, output, *args):
    _require_replay_toolchain()
    return subprocess.run([sys.executable, '-m', 'tools.cdj_dsp.replay',
                           str(dump), str(output), *args], cwd=ROOT,
                          text=True, capture_output=True, timeout=20)


def _ti_text_words(asm, tmp_path):
    """Assemble one tests/ti program with cl6x and return its .text words.

    --no_compress keeps every instruction 32 bits wide, so the section can be
    laid straight into an L2 image.  Nothing about the encodings comes from this
    emulator.
    """
    directory = os.environ.get('C6X_TI_BIN')
    if not directory:
        pytest.skip('set C6X_TI_BIN to assemble the TI reference program')
    subprocess.run([str(Path(directory) / 'cl6x'), '-mv6740', '--no_compress',
                    '--abi=eabi', '-c', str(ROOT / 'tests/ti' / asm)],
                   cwd=tmp_path, check=True, timeout=60)
    data = (tmp_path / asm.replace('.asm', '.obj')).read_bytes()
    assert data[:4] == b'\x7fELF' and data[4] == 1 and data[5] == 1
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)

    def section(index):
        base = shoff + index * shentsize
        name, _type, _flags, _addr, off, size = struct.unpack_from(
            '<IIIIII', data, base)
        return name, off, size

    strings = section(shstrndx)[1]
    for index in range(shnum):
        name, off, size = section(index)
        label = data[strings + name:data.index(b'\x00', strings + name)]
        if label == b'.text' and size:
            return list(struct.unpack_from('<%dI' % (size // 4), data, off))
    raise AssertionError('no .text in ' + asm)


def test_timer64p_period_interrupt_reaches_the_cpu_in_replay(tmp_path):
    """T64P0_TINT12 must travel the whole shipped path, not just the unit test.

    The DSP program is tests/ti/timer-tint12.asm, assembled by cl6x, and it
    performs SPRUH91D's own unchained-mode setup procedure (printed page 1238)
    through real MMIO.  What must then happen is fixed by the manuals:
    SPRUH91D Table 2-1 printed page 70 makes the period interrupt event 4
    (T64P0_TINT12); the INTC's reset INTMUX1 selects event 4 for CPUINT4; and
    CSR.GIE is 0 out of reset, so SPRUFE8B 5.4.1 requires the request to LATCH
    in IFR bit 4 rather than vector.  Masked is not dropped.

    This asserts ORDER, never rate.  The counter advances once per emulated CPU
    cycle - an approximation the manifest declares - so the number of cycles the
    run needed to get here carries no timing meaning and is deliberately not
    asserted.
    """
    words = _ti_text_words('timer-tint12.asm', tmp_path)
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x11800020)
    for index, word in enumerate(words):
        struct.pack_into('<I', data, 0x20 + 4 * index, word)
    dump = tmp_path / 'timer.bin'
    dump.write_bytes(data)
    output = tmp_path / 'replay'
    result = run(dump, output, '--steps', '4000')
    assert result.returncode == 0, result.stderr
    stop = json.loads(result.stdout.strip().splitlines()[-1])
    assert stop['reason'] == 'step_limit', stop
    assert not stop['fault'], stop
    assert stop['control']['ifr'] & (1 << 4), stop['control']
    assert not stop['control']['csr'] & 1, 'GIE must still be masked'
    manifest = json.loads((output / 'manifest.json').read_text())
    assert any('Timer64P counts one input clock per emulated CPU cycle' in entry
               for entry in manifest['approximations'])


def test_replay_records_false_and_true_source_predicates(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    struct.pack_into('<III', data, 0x20,
                     (1 << 29) | (3 << 23) | (123 << 7) | 0x28,
                     (1 << 7) | 0x2a,
                     (1 << 29) | (4 << 23) | (123 << 7) | 0x28)
    dump = tmp_path / 'predicates.bin'
    dump.write_bytes(data)
    output = tmp_path / 'replay'
    result = run(dump, output, '--steps', '3', '--verify-repeat')
    assert result.returncode == 0, result.stderr
    report = json.loads((output / 'coverage.json').read_text())
    rows = sorted(report['confirmed_instructions'], key=lambda row: row['pc'])
    assert [row['source_predicate_outcomes'] for row in rows] == [[False], [True], [True]]
    assert report['source_predicate_audit'] == dict(true_observed_addresses=2,
        false_only_addresses=1, unavailable_addresses=0, no_observations_addresses=0)
    stop = json.loads((output / 'trace.jsonl').read_text().splitlines()[-1])
    assert stop['registers'][0][3] == 0
    assert stop['registers'][0][4] == 123


def test_replay_determinism_breakpoints_and_limits(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)  # local L2 alias
    struct.pack_into('<II', data, 0x20, (3 << 23) | (123 << 7) | 0x28, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    traces = []
    for name in ('first', 'second'):
        output = tmp_path / name
        result = run(dump, output)
        assert result.returncode == 0, result.stderr
        traces.append((output / 'trace.jsonl').read_bytes())
    assert traces[0] == traces[1]
    stop = json.loads(traces[0].splitlines()[-1])
    assert stop['reason'] == 'fault' and stop['fault_word'] == 0xffffffff
    assert stop['packets'] == 1 and stop['registers'][0][3] == 123
    for args, reason in [(('--steps', '1'), 'step_limit'),
                         (('--packets', '1'), 'packet_limit'),
                         (('--cycles', '1'), 'cycle_limit'),
                         (('--break-pc', '0x00800024'), 'breakpoint')]:
        result = run(dump, tmp_path / reason, *args)
        assert result.returncode == 0, result.stderr
        stop = json.loads(result.stdout)
        assert stop['reason'] == reason and stop['fault'] == ''
        assert stop['pc'] == 0x00800024 and stop['packets'] == 1
    manifest = json.loads((tmp_path / 'first/manifest.json').read_text())
    assert manifest['boot_rom_executed'] is False and len(manifest['dump_sha256']) == 64
    assert 'emulator/qemu/cdj_c674x.h' in manifest['sources']
    assert manifest['coverage']['counts']['confirmed_source_packets'] == 1
    assert manifest['complete'] and manifest['progress']['packet_delta'] == 1
    assert manifest['progress']['cycle_delta'] == 1
    assert manifest['limits']['packets'] == 0 and manifest['limits']['cycles'] == 0
    # The interrupted-SPLOOP resume caveat is unconditional: SPRUFE8B
    # 7.7.3.1 rebuilds the loop buffer from program memory in every mode.
    # So is the Timer64P one: the counter advances once per CPU cycle_tick in
    # every mode, and cpu->cycles is an issue count no TI page relates to Hz,
    # so a manifest consumer may read a timer period expiring as evidence of
    # SPRUH91D chapter 28 register ORDER and of nothing about elapsed time.
    assert manifest['approximations'] == [
        'EMIFB mirrors populated 32 MiB SDRAM through the C0000000-DFFFFFFF aperture; upper D-window decoding is inferred from MPU2 coverage and unused SDRAM address pins (SPRUH91D 5.2.2 and 19.2.6.10), not hardware-validated; MPU protection and geometry reconfiguration are not modeled',
        'EDMA ICR is write-only (SPRUH91D 16.4.2.6.5); read-zero is a firmware compatibility choice, not a hardware-validated read value',
        'an interrupted SPLOOP resumes by rebuilding the loop buffer from program memory (SPRUFE8B 7.7.3.1); a loop body changed between the interrupt and the return is undetected once an ISR software loop has replaced the retained cross-check',
        'the reciprocal approximations RCPSP/RCPDP/RSQRSP/RSQRDP deliver a correct exponent and a mantissa within the 2^-8 the manual specifies, but their bits below the eighth mantissa position are not hardware-exact; firmware that refines the seed (the documented Newton-Raphson use) converges regardless, firmware that consumes it directly may diverge',
        'Timer64P counts one input clock per emulated CPU cycle (SPRUH91D chapter 28 register order, not rate); the step-to-tick ratio is unrelated to AUXCLK, so no elapsed-time, frequency or audio-rate conclusion may be drawn from a timer period expiring']
    assert manifest['output_checkpoint']['file'] == 'final.cdjdsp'
    assert (tmp_path / 'first/coverage.json').is_file()
    failure = json.loads((tmp_path / 'first/failure.json').read_text())
    assert failure['outcome'] == 'fail_closed_fault'
    assert failure['distinct_unsupported_encodings'] == []
    assert failure['distinct_fault_encodings'] == [{
        'word': 0xffffffff, 'pc': 0x11800024,
        'reason': 'reserved predicate', 'width': None,
        'width_limitation': 'fault latch does not retain compact/full width',
    }]
    assert failure['resumable_checkpoint']['checkpoint_sha256'] == \
        manifest['output_checkpoint']['checkpoint_sha256']
    # Even a single diagnostic run is resumable with explicit (non-repeat)
    # provenance; this does not claim deterministic equivalence.
    resumed = tmp_path / 'resumed'
    result = run(tmp_path / 'first/final.cdjdsp', resumed, '--instructions', '1')
    assert result.returncode == 0, result.stderr
    assert json.loads((resumed / 'manifest.json').read_text())['input_kind'] == \
        'diagnostic_replay_checkpoint'
    for reason in ('packet_limit', 'cycle_limit', 'breakpoint'):
        assert not (tmp_path / reason / 'failure.json').exists()


def test_explicit_strict_resume_preserves_exploratory_ancestry(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    dump = tmp_path / 'nop.bin'
    dump.write_bytes(data)
    first = tmp_path / 'exploratory'
    result = run(dump, first, '--steps', '1', '--functional-dsp-timing',
                 '--verify-repeat')
    assert result.returncode == 0, result.stderr
    for name in ('strict_resume', 'strict_again'):
        output = tmp_path / name
        result = run(first / 'final.cdjdsp', output, '--steps', '1', '--verify-repeat')
        assert result.returncode == 0, result.stderr
        manifest = json.loads((output / 'manifest.json').read_text())
        gate = json.loads((output / 'gate.json').read_text())
        assert manifest['dsp_timing_mode'] == 'strict'
        assert manifest['inherited_exploratory_state']
        assert not manifest['architectural_validation_eligible']
        assert not gate['architectural_validation_eligible']
        assert not gate['coverage_validation_eligible']
        assert gate['passed']
        assert any('SPLOOPD' in item for item in manifest['approximations'])
        first = output
    result = run(first, tmp_path / 'automatic_strict', '--steps', '1')
    assert result.returncode != 0
    assert 'inherits exploratory state' in result.stderr


def test_replay_rejects_invalid_input_without_artifacts(tmp_path):
    dump = tmp_path / 'short.bin'
    dump.write_bytes(b'bad')
    output = tmp_path / 'output'
    result = run(dump, output)
    assert result.returncode != 0 and '256 KiB' in result.stderr
    assert not output.exists()


def test_replay_gate_preserves_faults_and_rejects_changed_baseline(tmp_path):
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x11800020)
    struct.pack_into('<I', data, 0x20, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    first = tmp_path / 'first'
    result = run(dump, first, '--verify-repeat')
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)['reason'] == 'fault'
    gate = json.loads((first / 'gate.json').read_text())
    assert gate['passed'] and gate['repeat_matches']
    assert gate['repeat_coverage_matches']
    assert not gate['coverage_validation_eligible']
    assert gate['trace_sha256'] == gate['repeat_sha256']
    assert 'not architectural correctness or boot' in gate['scope']
    assert gate['limits']['steps'] == 10000
    assert gate['approximations'] == [
        'EMIFB mirrors populated 32 MiB SDRAM through the C0000000-DFFFFFFF aperture; upper D-window decoding is inferred from MPU2 coverage and unused SDRAM address pins (SPRUH91D 5.2.2 and 19.2.6.10), not hardware-validated; MPU protection and geometry reconfiguration are not modeled',
        'EDMA ICR is write-only (SPRUH91D 16.4.2.6.5); read-zero is a firmware compatibility choice, not a hardware-validated read value',
        'an interrupted SPLOOP resumes by rebuilding the loop buffer from program memory (SPRUFE8B 7.7.3.1); a loop body changed between the interrupt and the return is undetected once an ISR software loop has replaced the retained cross-check',
        'the reciprocal approximations RCPSP/RCPDP/RSQRSP/RSQRDP deliver a correct exponent and a mantissa within the 2^-8 the manual specifies, but their bits below the eighth mantissa position are not hardware-exact; firmware that refines the seed (the documented Newton-Raphson use) converges regardless, firmware that consumes it directly may diverge',
        'Timer64P counts one input clock per emulated CPU cycle (SPRUH91D chapter 28 register order, not rate); the step-to-tick ratio is unrelated to AUXCLK, so no elapsed-time, frequency or audio-rate conclusion may be drawn from a timer period expiring']
    # An exactly repeated final checkpoint is a provenance-bearing resume
    # point; normal iteration must not fall back to a connected checkpoint.
    chained = tmp_path / 'chained'
    result = run(first / 'final.cdjdsp', chained, '--verify-repeat')
    assert result.returncode == 0, result.stderr
    chained_manifest = json.loads((chained / 'manifest.json').read_text())
    assert chained_manifest['input_kind'] == 'deterministic_replay_checkpoint'
    assert chained_manifest['input_checkpoint']['checkpoint_sha256'] == \
        gate['final_checkpoint']['checkpoint_sha256']
    assert chained_manifest['input_manifest_sha256'] and \
        chained_manifest['input_gate_sha256']
    # A directory input selects the newest provenance-valid checkpoint and
    # ignores a newer corrupt artifact instead of requiring manual filename selection.
    (first / 'newer-invalid.cdjdsp').write_bytes(b'not a checkpoint')
    newest = tmp_path / 'newest'
    result = run(first, newest, '--steps', '1', '--verify-repeat')
    assert result.returncode == 0, result.stderr
    newest_manifest = json.loads((newest / 'manifest.json').read_text())
    assert newest_manifest['dump_path'] == str((first / 'final.cdjdsp').resolve())
    assert 'Selected newest compatible checkpoint' in result.stderr
    baseline = first / 'trace.jsonl'
    second = tmp_path / 'second'
    result = run(dump, second, '--verify-repeat', '--expect-trace', str(baseline))
    assert result.returncode == 0, result.stderr
    assert json.loads((second / 'gate.json').read_text())['expected_matches']
    # Same deterministic fault, different diagnostic trace: gate must fail,
    # preserving artifacts rather than quietly updating the baseline.
    changed = tmp_path / 'changed.jsonl'
    changed.write_bytes(baseline.read_bytes() + b'\n')
    third = tmp_path / 'third'
    result = run(dump, third, '--verify-repeat', '--expect-trace', str(changed))
    assert result.returncode == 1
    gate = json.loads((third / 'gate.json').read_text())
    assert gate['repeat_matches'] and not gate['expected_matches'] and not gate['passed']
    assert json.loads(result.stdout)['reason'] == 'fault'
    missing = tmp_path / 'missing'
    result = run(dump, missing, '--expect-trace', str(tmp_path / 'absent'))
    assert result.returncode != 0 and not missing.exists()


def test_transmit_capture_metadata_and_repeat_gate(tmp_path):
    capture = tmp_path / 'capture.jsonl'
    capture.write_text(
        '{"sequence":1,"instance":1,"slot":0,"serializer":0,"word":305419896,'
        '"xbuf_sequence":7,"packets":1024,"cycles":2048,'
        '"source":"genuine_xbuf","clock":"functional-coarse-packet-slot"}\n')
    metadata = tx_capture_metadata(capture)
    assert metadata['records'] == 1
    assert metadata['counts'] == {'mcasp1.serializer0': 1}
    assert metadata['synthesized_samples'] is False
    empty = tmp_path / 'empty.jsonl'
    empty.write_bytes(b'')
    assert tx_capture_metadata(empty)['records'] == 0
    truncated = tmp_path / 'truncated.jsonl'
    truncated.write_bytes(capture.read_bytes()[:-1])
    with pytest.raises((ValueError, json.JSONDecodeError)):
        tx_capture_metadata(truncated)
    malformed = tmp_path / 'malformed.jsonl'
    malformed.write_text(capture.read_text().replace('"genuine_xbuf"', '"synthetic"'))
    with pytest.raises(ValueError, match='invalid DSP transmit capture'):
        tx_capture_metadata(malformed)

    data = bytearray(0x40000)
    struct.pack_into('<I', data, 0, 0x00800020)
    struct.pack_into('<I', data, 0x20, 0xffffffff)
    dump = tmp_path / 'dump.bin'
    dump.write_bytes(data)
    output = tmp_path / 'captured-replay'
    result = run(dump, output, '--functional-dsp-audio', '--capture-dsp-tx',
                 '--verify-repeat')
    assert result.returncode == 0, result.stderr
    manifest = json.loads((output / 'manifest.json').read_text())
    gate = json.loads((output / 'gate.json').read_text())
    assert manifest['dsp_tx_capture']['records'] == 0
    assert manifest['dsp_tx_capture']['synthesized_samples'] is False
    assert gate['dsp_tx_capture_matches'] and gate['passed']
    assert (output / 'dsp-tx.jsonl').read_bytes() == \
        (output / 'repeat-dsp-tx.jsonl').read_bytes()

    rejected = run(dump, tmp_path / 'invalid-capture', '--capture-dsp-tx')
    assert rejected.returncode != 0 and 'requires --functional-dsp-audio' in rejected.stderr
