# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay a pre-execution NXS DSP L2 dump and capture a deterministic JSONL trace.

This uses the partial C674x core and SYSCFG model, not the missing boot ROM.
Faults and step limits are diagnostic outcomes, never evidence of boot success.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import sys

from .coverage import build_coverage
from .tx_capture import tx_capture_metadata
from .build_cache import build_native

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / 'tools/cdj_dsp/replay.c', *[
    ROOT / 'emulator/qemu' / name for name in
    ('cdj_c674x.c', 'cdj_c674x_mpy.c', 'cdj_c674x_dotp.c', 'cdj_c674x_packed8.c', 'cdj_c674x_packed16.c', 'cdj_c674x_packbits.c', 'cdj_c674x_mpy32.c', 'cdj_c674x_dp.c', 'cdj_c674x_approx.c', 'cdj_c674x_uncond.c',
     'cdj_c674x_sp.c', 'cdj_c674x_control.c',
     'cdj_c674x_loop.c', 'cdj_c6747_syscfg.c', 'cdj_c6747_psc.c',
     'cdj_c6747_mcasp.c', 'cdj_c6747_gpio.c', 'cdj_c6747_i2c.c', 'cdj_c6747_pll.c',
     'cdj_c6747_hpi.c', 'cdj_c6747_emifb.c', 'cdj_c6747_intc.c',
     'cdj_c6747_timer.c',
     'cdj_c6747_spi.c',
     'cdj_c6747_cache.c',
     'cdj_c6747_edma.c',
     'cdj_dsp_scheduler.c',
     'cdj_dsp_checkpoint.c')]]

CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0', 3: b'CDJDSP3\0',
                    4: b'CDJDSP4\0', 5: b'CDJDSP5\0', 6: b'CDJDSP6\0',
                    7: b'CDJDSP7\0', 8: b'CDJDSP8\0', 9: b'CDJDSP9\0',
                    10: b'CDJDSP10', 11: b'CDJDSP11', 12: b'CDJDSP12'}
SCHEDULER_STATE = struct.Struct('<QQIIBBBB')
SHARED_RAM_SIZE = 0x20000
L1D_SIZE = 0x8000
DEFAULT_FORMATS = ROOT / 'build/gdb-17.2/include/opcode/tic6x-insn-formats.h'
ANALYSIS_SOURCES = [ROOT / 'tools/cdj_dsp/coverage.py',
                    ROOT / 'tools/cdj_dsp/inventory.py',
                    ROOT / 'tools/cdj_dsp/tx_capture.py']


def _fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def _checkpoint_scheduler_mode(data, header_size, state_size, schema):
    if schema < 11:
        return 'legacy'
    if state_size < 32:
        raise ValueError('schema-11 checkpoint scheduler state is incomplete')
    fields = SCHEDULER_STATE.unpack_from(data, header_size + state_size - 32)
    activation, slice_id, remaining, slice_steps, pending, rearm, mode, reserved = fields
    if reserved or pending > 1 or rearm > 1:
        raise ValueError('schema-11 checkpoint scheduler state is invalid')
    if mode == 0:
        valid = not any((activation, slice_id, remaining, slice_steps,
                         pending, rearm))
        name = 'legacy'
    elif mode == 1:
        valid = (slice_steps == 4096 and remaining <= 1000000 and
                 pending == bool(remaining) and (not rearm or pending) and
                 ((activation != 0) or
                  not any((slice_id, remaining, pending, rearm))) and
                 (slice_id != 0 or activation == 0 or remaining == 1000000) and
                 (remaining == 0 or (1000000 - remaining) % slice_steps == 0))
        name = 'deferred-v1'
    else:
        valid = False
        name = None
    if not valid:
        raise ValueError('schema-11 checkpoint scheduler state is invalid')
    return name


def checkpoint_info(data: bytes) -> dict:
    if len(data) < CHECKPOINT_HEADER.size:
        raise ValueError('checkpoint header is incomplete')
    fields = CHECKPOINT_HEADER.unpack_from(data)
    magic, schema, endian, header_size, state_size = fields[:5]
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or len(data) != header_size + payload_size or
            l2_size != 0x40000 or sdram_size != 0x2000000 or page_size != 4096 or
            page_count != sdram_size // page_size or present_pages > page_count or
            _fnv1a(memoryview(data)[header_size:]) != payload_checksum):
        raise ValueError('checkpoint is incompatible or incomplete')
    l2_start = header_size + state_size
    l2 = data[l2_start:l2_start + l2_size]
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = l2_start + l2_size
    shared = data[shared_start:shared_start + shared_size]
    l1d_size = L1D_SIZE if schema >= 12 else 0
    l1d_start = shared_start + shared_size
    l1d = data[l1d_start:l1d_start + l1d_size]
    bitmap_size = (page_count + 7) // 8
    bitmap_start = l1d_start + l1d_size
    bitmap = data[bitmap_start:bitmap_start + bitmap_size]
    pages = memoryview(data)[bitmap_start + bitmap_size:]
    sdram_hash = hashlib.sha256()
    zero_page = bytes(page_size)
    offset = 0
    count = 0
    for page in range(page_count):
        if bitmap[page // 8] & (1 << (page % 8)):
            sdram_hash.update(pages[offset:offset + page_size])
            offset += page_size
            count += 1
        else:
            sdram_hash.update(zero_page)
    if count != present_pages or offset != len(pages):
        raise ValueError('checkpoint sparse SDRAM payload is incomplete')
    return dict(schema=schema, byte_order='little', state_size=state_size,
                component_sizes=list(fields[5:14]),
                checkpoint_sha256=hashlib.sha256(data).hexdigest(),
                l2_sha256=hashlib.sha256(l2).hexdigest(),
                shared_ram_captured=schema >= 2,
                scheduler_state_captured=schema >= 11,
                dsp_scheduler_mode=_checkpoint_scheduler_mode(
                    data, header_size, state_size, schema),
                shared_ram_sha256=(hashlib.sha256(shared).hexdigest()
                                   if schema >= 2 else None),
                l1d_captured=schema >= 12,
                l1d_sha256=(hashlib.sha256(l1d).hexdigest()
                             if schema >= 12 else None),
                sdram_sha256=sdram_hash.hexdigest(), present_pages=present_pages)


def event_budget_diagnostic(path: Path) -> dict:
    """Read the C runner's explicit non-resumable event-budget outcome."""
    diagnostics = []
    with path.open() as trace:
        for line in trace:
            event = json.loads(line)
            if event.get('event') == 'event_budget_exhausted':
                diagnostics.append(event)
    if len(diagnostics) != 1:
        raise ValueError('event-budget exit lacks exactly one diagnostic record')
    diagnostic = diagnostics[0]
    if (diagnostic.get('reason') not in {'step_limit', 'packet_limit', 'cycle_limit'} or
            not isinstance(diagnostic.get('next_sequence'), int) or
            diagnostic['next_sequence'] <= 0 or
            not isinstance(diagnostic.get('next_type'), str) or
            not diagnostic['next_type'] or
            any(not isinstance(diagnostic.get(field), int)
                for field in ('pc', 'packets', 'cycles'))):
        raise ValueError('event-budget diagnostic is malformed')
    return diagnostic


def checkpoint_provenance(path: Path, data: bytes, info: dict) -> dict:
    """Validate a connected or exact-repeat checkpoint's local provenance."""
    manifest_path = path.parent / 'manifest.json'
    if not manifest_path.is_file():
        raise ValueError('checkpoint requires its manifest.json provenance file')
    manifest_data = manifest_path.read_bytes()
    capture_manifest = json.loads(manifest_data)
    matching = [item for item in capture_manifest.get('checkpoints', [])
                if item.get('file') == path.name]
    result = dict(capture_manifest=capture_manifest,
                  manifest_sha256=hashlib.sha256(manifest_data).hexdigest(),
                  gate_sha256=None)
    if (capture_manifest.get('complete') and len(matching) == 1 and
            matching[0].get('sha256') == info['checkpoint_sha256']):
        result['origin'] = 'connected_checkpoint'
        return result
    # Lightweight launcher runs intentionally omit the event transcript.  A
    # checksum-matched, structurally validated checkpoint remains useful for
    # standalone fault diagnosis, but it cannot be promoted to connected-run
    # or architectural evidence.  Require every marker so an old/incomplete
    # manifest cannot accidentally open this weaker replay path.
    if (capture_manifest.get('dsp_checkpoint_policy') == 'fault' and
            capture_manifest.get('checkpoint_capture_complete') is True and
            capture_manifest.get('complete') is False and
            capture_manifest.get('architectural_validation_eligible') is False and
            len(matching) == 1 and
            matching[0].get('sha256') == info['checkpoint_sha256']):
        result['origin'] = 'diagnostic_connected_checkpoint'
        return result
    gate_path = path.parent / 'gate.json'
    gate_data = gate_path.read_bytes() if gate_path.is_file() else b''
    gate = json.loads(gate_data) if gate_data else {}
    key = ('final_checkpoint' if path.name == 'final.cdjdsp' else
           'repeat_final_checkpoint' if path.name == 'repeat-final.cdjdsp' else None)
    recorded = gate.get(key, {}) if key else {}
    if (gate.get('passed') and gate.get('repeat_matches') and
            gate.get('final_state_and_memory_match') and
            recorded.get('checkpoint_sha256') == info['checkpoint_sha256']):
        result.update(origin='deterministic_replay_checkpoint',
                      gate_sha256=hashlib.sha256(gate_data).hexdigest())
        return result
    output = capture_manifest.get('output_checkpoint', {})
    if (capture_manifest.get('complete') and output.get('file') == path.name and
            output.get('checkpoint_sha256') == info['checkpoint_sha256']):
        result.update(origin='diagnostic_replay_checkpoint')
        return result
    raise ValueError('checkpoint is absent from a complete connected manifest or replay provenance')


def write_manifest(output: Path, manifest: dict):
    """Persist the run manifest, never claiming eligibility for an unfinished run.

    `architectural_validation_eligible` is a claim about a run that ran to a
    recorded stop, so it cannot outrank `complete`.  The manifest used to be
    written once before the replay binary started and again afterwards, which
    left six aborted runs in runs/ (dsp-interrupt-entry-fixed-events-1/3/4,
    dsp-schema5-strict-migration-1, dsp-spi-gap-connected-replay-1 and
    dsp-spkernel-h7-candidate-2) claiming eligibility with no output checkpoint,
    no coverage and no terminal stop record.  The pre-execution write is gone;
    this gate keeps the claim honest for whatever write sites come later.
    """
    if not manifest.get('complete'):
        manifest['architectural_validation_eligible'] = False
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def exploratory_ancestry(manifest):
    """Execution mode changes cannot undo approximate state already captured."""
    return bool(manifest and (
        manifest.get('architectural_validation_eligible') is False or
        manifest.get('dsp_timing_mode', 'strict') != 'strict' or
        manifest.get('dsp_audio_mode', 'stopped-clock') != 'stopped-clock' or
        manifest.get('dsp_scheduler_mode', 'legacy') != 'legacy'))


def newest_checkpoint(directory: Path, *, timing_mode=None, audio_mode=None,
                      event_hash=None):
    """Return newest structurally valid, provenance-bearing checkpoint below a directory."""
    candidates = sorted((path for path in directory.rglob('*.cdjdsp')
                         if path.name != 'repeat-final.cdjdsp'),
                        key=lambda path: (path.stat().st_mtime_ns, str(path)), reverse=True)
    failures = []
    for path in candidates:
        try:
            data = path.read_bytes()
            info = checkpoint_info(data)
            provenance = checkpoint_provenance(path, data, info)
            manifest = provenance['capture_manifest']
            if (timing_mode == 'strict' and audio_mode == 'stopped-clock' and
                    exploratory_ancestry(manifest) and
                    provenance['origin'] != 'diagnostic_connected_checkpoint'):
                raise ValueError('checkpoint inherits exploratory state')
            for field, requested, default in (
                    ('dsp_timing_mode', timing_mode, 'strict'),
                    ('dsp_audio_mode', audio_mode, 'stopped-clock')):
                if requested is not None and manifest.get(field, default) != requested:
                    raise ValueError(f'{field} is incompatible with requested {requested}')
            transcript = manifest.get('event_transcript')
            recorded_hash = (transcript.get('sha256') if isinstance(transcript, dict)
                             else manifest.get('event_transcript_sha256'))
            if event_hash is not None and recorded_hash != event_hash:
                raise ValueError('event transcript is incompatible')
            return path, data, info, provenance
        except (OSError, ValueError, json.JSONDecodeError) as error:
            if len(failures) < 3:
                failures.append(f'{path}: {error}')
    detail = '; '.join(failures) if failures else 'no .cdjdsp files found'
    raise ValueError(f'no compatible provenance-bearing checkpoint under {directory}: {detail}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path,
                        help='checkpoint/raw L2 file, or directory whose newest valid checkpoint is selected')
    parser.add_argument('output', type=Path, help='new directory for manifest and trace')
    parser.add_argument('--steps', '--instructions', dest='steps', type=int, default=10000,
                        help='maximum successful core step calls (default: 10000)')
    parser.add_argument('--packets', type=int, default=0,
                        help='maximum packet-count delta from input checkpoint (0 disables)')
    parser.add_argument('--cycles', type=int, default=0,
                        help='maximum cycle-count delta from input checkpoint (0 disables)')
    parser.add_argument('--break-pc', type=lambda value: int(value, 0), default=0,
                        help='stop before executing this program counter (0 disables)')
    parser.add_argument('--boot-phase', type=lambda value: int(value, 0), default=0,
                        help='fixed external MAIN boot-phase GPIO value, 0..7 (default: captured phase 0)')
    parser.add_argument('--verify-repeat', action='store_true',
                        help='run the same compiled binary twice and gate on identical traces')
    parser.add_argument('--build-profile', choices=('optimized', 'debug'), default='optimized',
                        help='optimized (-O2, default) or debug (-O0) native replay build')
    parser.add_argument('--no-build-cache', action='store_true',
                        help='compile a fresh binary instead of reusing the content-addressed cache')
    parser.add_argument('--trace-mode', choices=('detailed', 'compact'), default='detailed',
                        help='compact omits standalone per-step diagnostics; events, stops and coverage remain')
    parser.add_argument('--observe-pcm', action='store_true',
                        help='record at most 64 RAM-only stock PCM PC observations; includes fall-through, not ownership proof')
    parser.add_argument('--connected-stops', type=int, default=0,
                        help='finish after this many verified connected stops (0: entire transcript)')
    parser.add_argument('--expect-trace', type=Path,
                        help='also require byte-identical output to this saved trace; not a boot test')
    parser.add_argument('--events', type=Path,
                        help='inject later events from the checkpoint connected-run transcript')
    parser.add_argument('--functional-dsp-timing', action='store_true',
                        help='use labeled two-cycle SPLOOPD run-ahead; deterministic but not cycle-validation evidence')
    parser.add_argument('--functional-dsp-audio', action='store_true',
                        help='schedule labeled coarse McASP slots; deterministic but not audio-timing evidence')
    parser.add_argument('--capture-dsp-tx', action='store_true',
                        help='capture genuine McASP XBUF words consumed at functional slot boundaries')
    parser.add_argument('--formats', type=Path, default=DEFAULT_FORMATS,
                        help='GNU tic6x-insn-formats.h used for automatic coverage')
    args = parser.parse_args()
    if not 0 <= args.connected_stops <= 0xffffffff or (args.connected_stops and not args.events):
        parser.error('--connected-stops requires --events and a count from 1 to 4294967295')
    if args.capture_dsp_tx and not args.functional_dsp_audio:
        parser.error('--capture-dsp-tx requires --functional-dsp-audio')
    if (not 0 < args.steps <= 100000000 or
            not 0 <= args.packets <= 0xffffffffffffffff or
            not 0 <= args.cycles <= 0xffffffffffffffff or
            not 0 <= args.break_pc <= 0xffffffff or
            not 0 <= args.boot_phase <= 7):
        parser.error('steps must be 1..100000000; packets/cycles must fit 64 bits; '
                     'breakpoint must fit 32 bits; boot phase must be 0..7')
    selected_checkpoint = None
    selected_provenance = None
    if args.dump.is_dir():
        try:
            selected_path, data, selected_checkpoint, selected_provenance = newest_checkpoint(
                args.dump,
                timing_mode='functional-runahead' if args.functional_dsp_timing else 'strict',
                audio_mode='coarse-packet-slots' if args.functional_dsp_audio else 'stopped-clock',
                event_hash=hashlib.sha256(args.events.read_bytes()).hexdigest()
                if args.events is not None else None)
        except (OSError, ValueError) as error:
            parser.error(str(error))
        args.dump = selected_path
        print(f'Selected newest compatible checkpoint: {args.dump}', file=sys.stderr)
    elif not args.dump.is_file():
        parser.error('input dump/checkpoint must be an existing file or directory')
    if args.expect_trace is not None and not args.expect_trace.is_file():
        parser.error('expected trace must be an existing file')
    if args.events is not None and not args.events.is_file():
        parser.error('event transcript must be an existing file')
    if not args.formats.is_file():
        parser.error('C6x format header is required (build dependencies per BUILD.md or use --formats)')
    cc = shutil.which('cc')
    if not cc:
        parser.error('C compiler required (install Xcode command line tools)')
    # Snapshot input so hashing and execution always describe the same bytes.
    data = data if selected_checkpoint is not None else args.dump.read_bytes()
    checkpoint = data.startswith(tuple(CHECKPOINT_MAGIC.values()))
    capture_manifest = None
    input_checkpoint = None
    checkpoint_origin = None
    checkpoint_manifest_sha256 = None
    checkpoint_gate_sha256 = None
    if checkpoint:
        try:
            input_checkpoint = selected_checkpoint or checkpoint_info(data)
            provenance = selected_provenance or checkpoint_provenance(
                args.dump, data, input_checkpoint)
        except (ValueError, json.JSONDecodeError) as error:
            parser.error(str(error))
        capture_manifest = provenance['capture_manifest']
        checkpoint_manifest_sha256 = provenance['manifest_sha256']
        checkpoint_origin = provenance['origin']
        checkpoint_gate_sha256 = provenance['gate_sha256']
    elif len(data) != 0x40000:
        parser.error('legacy dump must be exactly 256 KiB')
    event_data = args.events.read_bytes() if args.events is not None else None
    checkpoint_scheduler_mode = (input_checkpoint['dsp_scheduler_mode']
                                 if input_checkpoint else 'legacy')
    dsp_scheduler_mode = (capture_manifest.get('dsp_scheduler_mode',
                                               checkpoint_scheduler_mode)
                          if capture_manifest else checkpoint_scheduler_mode)
    if dsp_scheduler_mode not in {'legacy', 'deferred-v1'}:
        parser.error('checkpoint manifest has an unsupported DSP scheduler mode')
    if dsp_scheduler_mode != checkpoint_scheduler_mode:
        parser.error('checkpoint scheduler state disagrees with its manifest declaration')
    if event_data is not None:
        if not checkpoint:
            parser.error('event injection requires a connected checkpoint')
        if checkpoint_origin == 'diagnostic_connected_checkpoint':
            parser.error('diagnostic fault-only checkpoint has no connected event transcript; omit --events')
        transcript = capture_manifest.get('event_transcript')
        expected_event_hash = (transcript.get('sha256') if isinstance(transcript, dict)
                               else capture_manifest.get('event_transcript_sha256'))
        if not isinstance(expected_event_hash, str):
            parser.error('checkpoint manifest has no complete event-transcript provenance')
        if hashlib.sha256(event_data).hexdigest() != expected_event_hash:
            parser.error('event transcript does not match the checkpoint manifest')
    expected = args.expect_trace.read_bytes() if args.expect_trace is not None else None
    # Compile the exact source/header bytes whose hashes are recorded. A later
    # worktree edit must not make the manifest describe a different binary.
    # Headers are derived from SOURCES by suffix, so any header with no matching
    # .c has to be listed explicitly or the standalone build cannot find it.
    inputs = (SOURCES + [p.with_suffix('.h') for p in SOURCES[1:]] +
              [ROOT / 'emulator/qemu/cdj_c6747_spi_clock.h',
               ROOT / 'emulator/qemu/cdj_c674x_multicycle.h'])
    source_data = {p: p.read_bytes() for p in inputs}
    format_data = args.formats.read_bytes()
    analysis_data = {path: path.read_bytes() for path in ANALYSIS_SOURCES}
    with tempfile.TemporaryDirectory(prefix='cdj-dsp-replay-') as temp:
        binary = Path(temp) / 'replay'
        snapshot = Path(temp) / 'l2.bin'
        snapshot.write_bytes(data)
        event_snapshot = Path(temp) / 'events.jsonl'
        if event_data is not None:
            event_snapshot.write_bytes(event_data)
        for path, content in source_data.items():
            (Path(temp) / path.name).write_bytes(content)
        binary, build = build_native(cc, Path(temp),
            [Path(temp) / p.name for p in SOURCES],
            cache=None if args.no_build_cache else Path(os.environ.get(
                'CDJ_REPLAY_CACHE', str(ROOT / 'build/replay-cache'))),
            optimization='-O0' if args.build_profile == 'debug' else '-O2')
        args.output.mkdir(parents=True, exist_ok=False)
        external_event_assumption = (
            'ordered post-checkpoint MAIN/HPI events injected and gated against every connected DSP stop'
            if event_data is not None else
            'no later MAIN/HPI events injected; replay stops when an external event is required')
        approximations = [
            'EMIFB mirrors populated 32 MiB SDRAM through the C0000000-DFFFFFFF aperture; upper D-window decoding is inferred from MPU2 coverage and unused SDRAM address pins (SPRUH91D 5.2.2 and 19.2.6.10), not hardware-validated; MPU protection and geometry reconfiguration are not modeled',
            'EDMA ICR is write-only (SPRUH91D 16.4.2.6.5); read-zero is a firmware compatibility choice, not a hardware-validated read value',
            *(['two-cycle SPLOOPD functional run-ahead; not cycle-accurate']
               if args.functional_dsp_timing else []),
            *(['SPI1 WM8740 control transfers complete at commit; serial timing is not modeled']
               if args.functional_dsp_timing else []),
            # SPRUFE8B 7.7.3.1 resumes an interrupted loop by re-executing
            # its prolog from program memory, so this is how the hardware
            # works rather than a timing mode, and the caveat applies to every
            # run that interrupts a software loop, strict or breadth.
            'an interrupted SPLOOP resumes by rebuilding the loop buffer from program memory (SPRUFE8B 7.7.3.1); a loop body changed between the interrupt and the return is undetected once an ISR software loop has replaced the retained cross-check',
            # Timer64P0/1 advance one input clock per CPU cycle_tick, which is
            # one modelled VLIW issue.  cpu->cycles has no stall, memory
            # latency or cache model and no TI page relates it to Hz, so this
            # is a counter and an ORDER, never a rate: a run in which a timer
            # period expired says the register sequence of SPRUH91D chapter 28
            # is correct and says nothing whatever about how long it took, how
            # it relates to AUXCLK or cdj_c6747_timer_input_hz(), or what a
            # firmware delay loop would measure on hardware.
            'the reciprocal approximations RCPSP/RCPDP/RSQRSP/RSQRDP deliver a correct exponent and a mantissa within the 2^-8 the manual specifies, but their bits below the eighth mantissa position are not hardware-exact; firmware that refines the seed (the documented Newton-Raphson use) converges regardless, firmware that consumes it directly may diverge',
            'Timer64P counts one input clock per emulated CPU cycle (SPRUH91D chapter 28 register order, not rate); the step-to-tick ratio is unrelated to AUXCLK, so no elapsed-time, frequency or audio-rate conclusion may be drawn from a timer period expiring',
            *(['interrupt entry retires already-issued results with minimum empty cycles; exact interrupt pipeline latency is not modeled']
               if args.functional_dsp_timing else []),
            *(['coarse packet-driven McASP slots; not audio-rate or cycle-accurate']
               if args.functional_dsp_audio else []),
            *(['deferred-v1 replays captured host activation slices; this is scheduling '
               'provenance, not DSP clock, frequency, cycle-accuracy, or hardware evidence']
              if dsp_scheduler_mode == 'deferred-v1' else []),
        ]
        inherited_exploratory = (exploratory_ancestry(capture_manifest) or
                                 dsp_scheduler_mode != 'legacy')
        inherited_approximations = (capture_manifest.get('approximations', [])
                                   if inherited_exploratory else [])
        if inherited_exploratory:
            approximations = list(dict.fromkeys([
                *inherited_approximations, *approximations,
                'input checkpoint inherits exploratory state; switching execution modes does not validate prior state',
            ]))
        validation_eligible = not (inherited_exploratory or
                                  args.functional_dsp_timing or args.functional_dsp_audio or
                                  dsp_scheduler_mode != 'legacy')
        limits = dict(steps=args.steps, packets=args.packets, cycles=args.cycles,
                      packet_cycle_origin='input checkpoint counters',
                      boundary_semantics='checked between successful core steps; multicycle steps may cross a cycle ceiling')
        manifest = dict(build=build, trace_mode=args.trace_mode,
                        complete=False,
                        dump_sha256=hashlib.sha256(data).hexdigest(),
                        dump_path=str(args.dump.resolve()), steps=args.steps,
                        limits=limits, approximations=approximations,
                        dsp_timing_mode=('functional-runahead' if args.functional_dsp_timing else 'strict'),
                        dsp_audio_mode=('coarse-packet-slots' if args.functional_dsp_audio else 'stopped-clock'),
                        dsp_scheduler_mode=dsp_scheduler_mode,
                        dsp_scheduler_provenance={
                            'declared_input_mode': dsp_scheduler_mode,
                            'checkpoint_mode': checkpoint_scheduler_mode,
                            'scheduler_state_captured': bool(
                                checkpoint and input_checkpoint['schema'] >= 11),
                            'execution': ('connected event-transcript schedule replay'
                                          if event_data is not None else
                                          'standalone replay loop'),
                            'claim': ('host scheduling policy only; not a DSP clock, '
                                      'frequency, cycle-accuracy, or hardware claim'),
                        },
                        architectural_validation_eligible=validation_eligible,
                        inherited_exploratory_state=inherited_exploratory,
                        break_pc=args.break_pc, boot_phase=args.boot_phase,
                        observe_pcm=args.observe_pcm, connected_stops=args.connected_stops,
                        input_kind=checkpoint_origin if checkpoint else 'legacy_l2_dump',
                        input_checkpoint=input_checkpoint,
                        input_manifest_sha256=checkpoint_manifest_sha256,
                        input_gate_sha256=checkpoint_gate_sha256,
                        event_transcript_sha256=(hashlib.sha256(event_data).hexdigest()
                                                 if event_data is not None else None),
                        capture_source_sha256=(capture_manifest.get('source_sha256') or
                                               capture_manifest.get('capture_source_sha256'))
                                              if capture_manifest else None,
                        capture_firmware_sha256=(capture_manifest.get('firmware_sha256') or
                                                 capture_manifest.get('capture_firmware_sha256'))
                                                if capture_manifest else None,
                        boot_rom_executed=False,
                        pll_assumptions=['POR configuration at ROM handoff',
                                         'initial bypass; NXS OSCIN 16934400 Hz, active SYSCLK1 division',
                                         'catalog PLL reset/lock bounds applied to custom DSP; not measured lock',
                                         'early PLL enable latches and is flagged; analog acquisition not simulated',
                                         external_event_assumption,
                                         ('schema-1 input did not capture shared RAM; restored zero before its first observed use'
                                          if checkpoint and input_checkpoint['schema'] == 1 else
                                          '128 KiB shared RAM captured losslessly in schema-2+ checkpoints'),
                                         'EMIFB register readback and 32 MiB storage modeled; SDRAM command timing and arbitration omitted',
                                         f'MAIN-to-DSP GPIO boot phase fixed at {args.boot_phase}; other external GPIO inputs default low',
                                         'oscillator counter complete at handoff, not PLL lock',
                                         'legacy PLLCTL bit 4 writable latch; C6747 effect unverified',
                                         'divider GO completes after eight subsequent DSP cycles; not physical clock timing'],
                        sources={str(p.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
                                 for p, content in source_data.items()},
                        analysis_sources={
                            **{str(path.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
                               for path, content in analysis_data.items()},
                            str(args.formats.resolve()): hashlib.sha256(format_data).hexdigest(),
                        })
        # No manifest is written before execution: a manifest on disk is a
        # record of a finished run, and a crashed replay must leave none.
        command = [str(binary), str(snapshot), str(args.steps), str(args.packets),
                   str(args.cycles), str(args.break_pc), str(args.boot_phase),
                   str(args.output / 'final.cdjdsp')]
        if event_data is not None:
            command.append(str(event_snapshot))
        replay_env = os.environ.copy()
        replay_env.pop('CDJ_DSP_COMPACT_TRACE', None)
        if args.trace_mode == 'compact':
            replay_env['CDJ_DSP_COMPACT_TRACE'] = '1'
        replay_env.pop('CDJ_DSP_OBSERVE_PCM', None)
        replay_env.pop('CDJ_DSP_CONNECTED_STOPS', None)
        if args.connected_stops:
            replay_env['CDJ_DSP_CONNECTED_STOPS'] = str(args.connected_stops)
        if args.observe_pcm:
            replay_env['CDJ_DSP_OBSERVE_PCM'] = '1'
        if args.functional_dsp_timing:
            replay_env['CDJ_NXS_DSP_FUNCTIONAL_TIMING'] = '1'
        else:
            replay_env.pop('CDJ_NXS_DSP_FUNCTIONAL_TIMING', None)
        if args.functional_dsp_audio:
            replay_env['CDJ_NXS_DSP_FUNCTIONAL_AUDIO'] = '1'
        else:
            replay_env.pop('CDJ_NXS_DSP_FUNCTIONAL_AUDIO', None)
        if args.capture_dsp_tx:
            replay_env['CDJ_NXS_DSP_TX_CAPTURE'] = str(args.output / 'dsp-tx.jsonl')
        else:
            replay_env.pop('CDJ_NXS_DSP_TX_CAPTURE', None)
        with (args.output / 'trace.jsonl').open('w') as trace:
            result = subprocess.run(command, stdout=trace, env=replay_env)
        if result.returncode == 3:
            try:
                diagnostic = event_budget_diagnostic(args.output / 'trace.jsonl')
            except (OSError, ValueError, json.JSONDecodeError) as error:
                parser.error(f'invalid event-budget diagnostic: {error}')
            failure = dict(
                schema=1,
                outcome='event_budget_exhausted',
                diagnostic=diagnostic,
                limits=manifest['limits'],
                resumable_checkpoint=None,
                limitation=('execution stopped between connected transcript boundaries; '
                            'there is no matching external-event boundary provenance, so no '
                            'output checkpoint was written'),
            )
            failure_bytes = (json.dumps(failure, indent=2) + '\n').encode()
            (args.output / 'failure.json').write_bytes(failure_bytes)
            manifest.update(
                complete=True,
                outcome='event_budget_exhausted',
                architectural_validation_eligible=False,
                validation_incomplete=True,
                output_checkpoint=None,
                coverage=None,
                stop=None,
                budget_exhaustion=diagnostic,
                failure_artifact={
                    'file': 'failure.json',
                    'sha256': hashlib.sha256(failure_bytes).hexdigest(),
                },
            )
            write_manifest(args.output, manifest)
            if args.verify_repeat or expected is not None:
                gate = dict(
                    scope='bounded event replay did not reach a connected boundary',
                    passed=False,
                    outcome='event_budget_exhausted',
                    limits=manifest['limits'],
                    diagnostic=diagnostic,
                    repeat_not_run=True,
                )
                (args.output / 'gate.json').write_text(json.dumps(gate, indent=2) + '\n')
            print(json.dumps(diagnostic, separators=(',', ':')))
            print('Replay event budget exhausted before a connected boundary; '
                  'no resumable checkpoint was written', file=sys.stderr)
            raise SystemExit(1)
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command)
        if args.verify_repeat:
            repeat_command = command.copy()
            repeat_command[7] = str(args.output / 'repeat-final.cdjdsp')
            repeat_env = replay_env.copy()
            if args.capture_dsp_tx:
                repeat_env['CDJ_NXS_DSP_TX_CAPTURE'] = str(
                    args.output / 'repeat-dsp-tx.jsonl')
            with (args.output / 'repeat.jsonl').open('w') as trace:
                subprocess.run(repeat_command, stdout=trace, check=True, env=repeat_env)
    coverage_data = {}
    for trace_name, checkpoint_name, output_name in [
            ('trace.jsonl', 'final.cdjdsp', 'coverage.json'),
            *(([('repeat.jsonl', 'repeat-final.cdjdsp', 'repeat-coverage.json')]
               if args.verify_repeat else []))]:
        checkpoint_bytes = (args.output / checkpoint_name).read_bytes()
        trace_bytes = (args.output / trace_name).read_bytes()
        try:
            coverage = build_coverage(checkpoint_bytes, trace_bytes, format_data)
        except (ValueError, json.JSONDecodeError) as error:
            parser.error(f'coverage generation failed: {error}')
        coverage['architectural_validation_eligible'] = validation_eligible
        coverage['validation_eligible'] &= validation_eligible
        coverage['approximations'] = approximations
        coverage['sha256'] = {
            'checkpoint': hashlib.sha256(checkpoint_bytes).hexdigest(),
            'trace': hashlib.sha256(trace_bytes).hexdigest(),
            'formats': hashlib.sha256(format_data).hexdigest(),
            **{str(path.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
               for path, content in analysis_data.items()},
        }
        serialized = (json.dumps(coverage, indent=2) + '\n').encode()
        (args.output / output_name).write_bytes(serialized)
        coverage_data[output_name] = (coverage, serialized)
    manifest['coverage'] = {
        'file': 'coverage.json',
        'sha256': hashlib.sha256(coverage_data['coverage.json'][1]).hexdigest(),
        'counts': coverage_data['coverage.json'][0]['counts'],
        'validation_eligible': coverage_data['coverage.json'][0]['validation_eligible'],
    }
    primary_coverage = coverage_data['coverage.json'][0]
    manifest['progress'] = primary_coverage.get('progress', {})
    tx_capture = None
    repeat_tx_capture = None
    if args.capture_dsp_tx:
        try:
            tx_capture = tx_capture_metadata(args.output / 'dsp-tx.jsonl')
            if args.verify_repeat:
                repeat_tx_capture = tx_capture_metadata(
                    args.output / 'repeat-dsp-tx.jsonl')
        except (OSError, ValueError, json.JSONDecodeError) as error:
            parser.error(f'DSP transmit capture validation failed: {error}')
        manifest['dsp_tx_capture'] = tx_capture
    final_checkpoint_bytes = (args.output / 'final.cdjdsp').read_bytes()
    manifest['output_checkpoint'] = {
        'file': 'final.cdjdsp',
        **checkpoint_info(final_checkpoint_bytes),
    }
    stop = None
    with (args.output / 'trace.jsonl').open() as trace:
        for line in trace:
            event = json.loads(line)
            if event.get('event') == 'stop':
                stop = event
    if stop is None:
        parser.error('replay trace has no terminal stop record')
    manifest['stop'] = {key: stop.get(key) for key in
                        ('reason', 'fault', 'pc', 'fault_pc', 'fault_word',
                         'packets', 'cycles')}
    failure_bytes = None
    if stop.get('fault'):
        unsupported = {}
        for item in primary_coverage.get('unsupported', []):
            key = (item.get('word'), item.get('reason'))
            unsupported[key] = dict(
                word=item.get('word'), pc=item.get('pc'),
                reason=item.get('reason'), width=None,
                width_limitation='fault latch does not retain compact/full width')
        unsupported_encodings = list(unsupported.values())
        # A valid instruction can still terminate a packet for a resource or
        # timing conflict.  Keep that terminal encoding in the broader fault
        # set without misclassifying it as an unsupported opcode.
        faults = dict(unsupported)
        for item in primary_coverage['faults']:
            key = (item.get('word'), item.get('reason'))
            faults[key] = dict(
                word=item.get('word'), pc=item.get('pc'),
                reason=item.get('reason'), width=None,
                width_limitation='fault latch does not retain compact/full width')
        failure = dict(
            schema=1,
            outcome='fail_closed_fault',
            stop=manifest['stop'],
            distinct_fault_encodings=list(faults.values()),
            distinct_unsupported_encodings=unsupported_encodings,
            resumable_checkpoint=manifest['output_checkpoint'],
            progress=manifest['progress'],
            approximations=manifest['approximations'],
            limitation=('one fail-closed run normally exposes one terminal encoding; '
                        'width is reported only when the core records it'),
        )
        failure_bytes = (json.dumps(failure, indent=2) + '\n').encode()
        (args.output / 'failure.json').write_bytes(failure_bytes)
        manifest['failure_artifact'] = {
            'file': 'failure.json',
            'sha256': hashlib.sha256(failure_bytes).hexdigest(),
            'distinct_unsupported_encodings': len(unsupported_encodings),
        }
    manifest['complete'] = True
    write_manifest(args.output, manifest)
    print(json.dumps(stop, separators=(',', ':')))
    if args.verify_repeat or expected is not None:
        actual = (args.output / 'trace.jsonl').read_bytes()
        gate = dict(scope='trace equivalence within selected mode; not architectural correctness or boot',
                    trace_mode=args.trace_mode,
                    architectural_validation_eligible=validation_eligible,
                    limits=manifest['limits'],
                    approximations=manifest['approximations'],
                    progress=manifest['progress'],
                    stop=manifest['stop'],
                    trace_sha256=hashlib.sha256(actual).hexdigest(),
                    coverage_sha256=manifest['coverage']['sha256'],
                    coverage_counts=manifest['coverage']['counts'],
                    coverage_validation_eligible=manifest['coverage']['validation_eligible'],
                    passed=True)
        if failure_bytes is not None:
            gate['failure_artifact_sha256'] = hashlib.sha256(failure_bytes).hexdigest()
        if event_data is not None:
            verified_stops = sum(
                json.loads(line).get('event') == 'verified_connected_stop'
                for line in actual.decode().splitlines())
            gate['verified_connected_stops'] = verified_stops
            gate['passed'] &= verified_stops > 0
        if args.verify_repeat:
            repeated = (args.output / 'repeat.jsonl').read_bytes()
            gate['repeat_matches'] = actual == repeated
            gate['repeat_sha256'] = hashlib.sha256(repeated).hexdigest()
            gate['passed'] &= gate['repeat_matches']
            final = (args.output / 'final.cdjdsp').read_bytes()
            repeat_final = (args.output / 'repeat-final.cdjdsp').read_bytes()
            gate['final_checkpoint'] = checkpoint_info(final)
            gate['repeat_final_checkpoint'] = checkpoint_info(repeat_final)
            gate['final_state_and_memory_match'] = final == repeat_final
            gate['passed'] &= gate['final_state_and_memory_match']
            repeated_coverage = coverage_data['repeat-coverage.json'][1]
            gate['repeat_coverage_sha256'] = hashlib.sha256(repeated_coverage).hexdigest()
            gate['repeat_coverage_matches'] = coverage_data['coverage.json'][1] == repeated_coverage
            gate['passed'] &= gate['repeat_coverage_matches']
            if args.capture_dsp_tx:
                tx_bytes = (args.output / 'dsp-tx.jsonl').read_bytes()
                repeat_tx_bytes = (args.output / 'repeat-dsp-tx.jsonl').read_bytes()
                gate['dsp_tx_capture'] = tx_capture
                gate['repeat_dsp_tx_capture'] = repeat_tx_capture
                gate['dsp_tx_capture_matches'] = tx_bytes == repeat_tx_bytes
                gate['passed'] &= gate['dsp_tx_capture_matches']
        if expected is not None:
            gate['expected_path'] = str(args.expect_trace.resolve())
            gate['expected_sha256'] = hashlib.sha256(expected).hexdigest()
            gate['expected_matches'] = actual == expected
            gate['passed'] &= gate['expected_matches']
        (args.output / 'gate.json').write_text(json.dumps(gate, indent=2) + '\n')
        if not gate['passed']:
            print('Replay equivalence gate failed; inspect gate.json and saved traces', file=sys.stderr)
            raise SystemExit(1)


if __name__ == '__main__':
    main()
