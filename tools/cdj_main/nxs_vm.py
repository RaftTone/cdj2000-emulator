# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the experimental NXS MAIN and GUI profiles over a direct serial link.

No proxy or generated status packets. DSP uses UHPI transport and a partial C674x interpreter.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
import math
import os
import shlex
import socket
from pathlib import Path
import struct
import subprocess
import sys
import time

from tools.cdj_dsp.tx_capture import tx_capture_metadata
from tools.cdj_main.run_state import write_json
from tools.cdj_main.nxs_panel import neutral_frame
from tools.cdj_main.qmp import connect_chardev
from tools.paths import BFIN_SIM, QEMU, qemu_environment

ROOT = Path(__file__).resolve().parents[2]
# MinGW QEMU has no unix chardev; POSIX keeps the PR's AF_UNIX sockets.
UNIX_CONTROL = os.name != 'nt'

CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0', 3: b'CDJDSP3\0',
                    4: b'CDJDSP4\0', 5: b'CDJDSP5\0', 6: b'CDJDSP6\0',
                    7: b'CDJDSP7\0', 8: b'CDJDSP8\0', 9: b'CDJDSP9\0',
                    10: b'CDJDSP10', 11: b'CDJDSP11', 12: b'CDJDSP12'}
SCHEDULER_STATE = struct.Struct('<QQIIBBBB')
SHARED_RAM_SIZE = 0x20000
MAX_FRAME_BYTES = 16 * 1024 * 1024
SYNC_PROFILE_COMMANDS = ('info sync-profile -n 30', 'info sync-profile -m -n 30')


def existing_file(*candidates: Path) -> Path | None:
    for path in candidates:
        if path.is_file():
            return path
    return None


def resolve_simulator() -> Path:
    """Prefer the platform binary, but accept either spelling for tests and Mac."""
    return existing_file(
        ROOT / ('bin/cdj-run' + ('.exe' if os.name == 'nt' else '')),
        ROOT / 'bin/cdj-run',
        ROOT / 'bin/cdj-run.exe',
        BFIN_SIM,
    ) or ROOT / ('bin/cdj-run' + ('.exe' if os.name == 'nt' else ''))


def resolve_qemu(explicit: Path) -> Path:
    """Keep the in-tree Mac default when that file exists; honor CDJ_QEMU otherwise."""
    default = ROOT / 'build/qemu/build/qemu-system-sh4'
    if explicit != default:
        return existing_file(explicit, Path(str(explicit) + '.exe')) or explicit
    found = existing_file(default, Path(str(default) + '.exe'))
    if found:
        return found
    env = os.environ.get('CDJ_QEMU')
    if env:
        path = Path(env)
        return existing_file(path, Path(str(path) + '.exe')) or path
    if Path(QEMU).is_file():
        return Path(QEMU)
    return default


def connect_monitor(endpoint, timeout: float = 3) -> socket.socket:
    """HMP: Unix socket on POSIX, TCP on Windows."""
    return connect_chardev(endpoint, timeout)


def capture_sync_profile(run: Path, process, *, endpoint=None) -> dict:
    """Read-only HMP observations before QEMU teardown; failure is evidence too."""
    report = dict(commands=list(SYNC_PROFILE_COMMANDS), status='unavailable')
    if process is None or process.poll() is not None:
        report['error'] = 'QEMU is not running at collection time'
        return report
    if endpoint is None:
        endpoint = run / 'qemu-monitor.sock'
    output = bytearray()
    try:
        with connect_monitor(endpoint) as monitor:

            def prompt():
                deadline = time.monotonic() + 3
                response = bytearray()
                while not response.endswith(b'(qemu) '):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('QEMU monitor response timed out')
                    monitor.settimeout(remaining)
                    chunk = monitor.recv(4096)
                    if not chunk:
                        raise OSError('QEMU monitor closed before prompt')
                    response.extend(chunk)
                    output.extend(chunk)
                    if len(output) > 1024 * 1024:
                        raise OSError('QEMU monitor output exceeds diagnostic bound')

            prompt()
            for command in SYNC_PROFILE_COMMANDS:
                monitor.sendall(command.encode('ascii') + b'\n')
                prompt()
        report['status'] = 'captured'
    except (OSError, ValueError) as error:
        report['error'] = str(error)
    if output:
        path = run / 'qemu-sync-profile.txt'
        try:
            path.write_bytes(output)
            report.update(file=path.name, sha256=hashlib.sha256(output).hexdigest())
        except OSError as error:
            report.update(status='unavailable', error=str(error))
    return report


def fnv1a(data) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def checkpoint_scheduler_mode(raw, header_size, state_size, schema) -> str:
    if schema < 11:
        return 'legacy'
    if state_size < 32:
        raise RuntimeError('schema-11 DSP scheduler state is incomplete')
    fields = SCHEDULER_STATE.unpack_from(raw, header_size + state_size - 32)
    activation, slice_id, remaining, slice_steps, pending, rearm, mode, reserved = fields
    if reserved or pending > 1 or rearm > 1:
        raise RuntimeError('schema-11 DSP scheduler state is invalid')
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
        raise RuntimeError('schema-11 DSP scheduler state is invalid')
    return name


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


# The NXS source contacts are shifted from the original CDJ-2000.  Keep this
# map limited to the source selector keys documented by --source-key; other
# panel buttons are valid contacts, but are not media-source shortcuts.
from tools.cdj_main.nxs_panel import BUTTON_NAMES as _NXS_BUTTON_NAMES

NXS_SOURCE_KEYS = {
    name: _NXS_BUTTON_NAMES[name]
    for name in ('sd', 'usb', 'link', 'disc', 'rekordbox')
}


def input_metadata(path: Path) -> dict:
    """Hash the resolved artifact before launch, rejecting concurrent rewrites."""
    path = path.resolve()
    before = path.stat()
    digest = sha256(path)
    after = path.stat()
    if file_signature(before) != file_signature(after):
        raise RuntimeError(f'input changed while hashing: {path}')
    return dict(path=str(path), size=after.st_size, sha256=digest)


def file_signature(stat) -> tuple:
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)


def source_schedule(at: float, contact: tuple[int, int], retries: int = 0,
                    interval: float = 120.0) -> str:
    """Encode an insertion-time source press and optional readiness retries."""
    if retries < 0 or retries > 15:
        raise ValueError('source retries must be 0..15')
    if not math.isfinite(interval) or interval <= 0:
        raise ValueError('source retry interval must be finite and positive')
    byte, mask = contact
    return ';'.join('%g:%d:%02x' % (at + index * interval, byte, mask)
                    for index in range(retries + 1))


def launch_ports(base: int, debug: bool) -> tuple[int, ...]:
    """Return every localhost port this launcher will bind before launch."""
    ports = [base, base + 2, base + 4]
    if debug:
        ports.append(base + 3)
        if not UNIX_CONTROL:
            ports.append(base + 1)
    if not UNIX_CONTROL:
        ports.append(base + 5)
    return tuple(ports)


def occupied_local_ports(base: int, debug: bool) -> list[int]:
    """Probe launcher-owned TCP ports without contacting other sessions."""
    occupied = []
    for port in launch_ports(base, debug):
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            probe.settimeout(0.25)
            probe.bind(('127.0.0.1', port))
        except PermissionError as error:
            raise PermissionError(
                f'permission denied while probing localhost port {port}') from error
        except OSError:
            occupied.append(port)
        finally:
            probe.close()
    return occupied


def automatic_run_path() -> Path:
    """Choose a readable timestamped run directory without overwriting one."""
    stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime())
    base = ROOT / 'runs' / f'nxs-{stamp}'
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = ROOT / 'runs' / f'nxs-{stamp}-{suffix}'
        suffix += 1
    return candidate


def run_command_path(run: Path) -> str:
    """Render a run path that can be pasted from the repository root."""
    try:
        return run.relative_to(ROOT).as_posix()
    except ValueError:
        return Path(run).as_posix()


def print_agent_commands(run: Path, port: int, debug: bool, qmp_endpoint=None) -> None:
    """Print bounded, copyable follow-up commands for humans and agents."""
    name = shlex.quote(run_command_path(run))
    print(f'run: {name}', flush=True)
    print(f'status: python -m tools.cdj_main.run_state {name}', flush=True)
    print(f'report: python -m tools.cdj_main.run_report {name}', flush=True)
    print(f'agent: python -m tools.cdj_main.dev {name} status --json', flush=True)
    print(f'agent press: python -m tools.cdj_main.dev {name} press sd --hold-ms 100', flush=True)
    print(f'panel: python -m tools.cdj_main.panel_control --port {port + 4} state',
          flush=True)
    if debug:
        print(f'agent qmp: python -m tools.cdj_main.dev {name} qmp status', flush=True)
        print(f'gdb: target remote 127.0.0.1:{port + 3}', flush=True)
        print(f'qmp: {shlex.quote(str(qmp_endpoint or run / "qmp.sock"))}', flush=True)


def write_gui_board_override(template: Path, flash: Path, output: Path) -> Path:
    """Write a run-local NXS board file pointing at the selected GUI flash."""
    lines = template.read_text().splitlines(keepends=True)
    marker = '/core/bfin_ebiu_amc/cfi@0/file '
    matches = [index for index, line in enumerate(lines)
               if line.lstrip().startswith(marker)]
    if len(matches) != 1:
        raise ValueError(f'GUI board template must contain one CFI flash file line: {template}')
    newline = '\n' if lines[matches[0]].endswith('\n') else ''
    lines[matches[0]] = marker + json.dumps(flash.resolve().as_posix()) + newline
    output.write_text(''.join(lines))
    return output


def complete_ppm_dimensions(raw: bytes) -> tuple[int, int] | None:
    """Validate one complete P6/255 frame, the simulator's output format.

    Consume only the header delimiter after maxval: raster bytes may themselves
    be whitespace or '#'. Reject concatenated, truncated and oversized images.
    """
    if not raw.startswith(b'P6') or len(raw) > MAX_FRAME_BYTES:
        return None
    cursor = 0
    tokens = []
    for _ in range(4):
        while cursor < len(raw):
            if raw[cursor] in b' \t\r\n\v\f':
                cursor += 1
            elif raw[cursor] == ord('#'):
                end = raw.find(b'\n', cursor)
                if end < 0:
                    return None
                cursor = end + 1
            else:
                break
        start = cursor
        while cursor < len(raw) and raw[cursor] not in b' \t\r\n\v\f#':
            cursor += 1
        tokens.append(raw[start:cursor])
    if tokens[0] != b'P6' or any(not token.isdigit() for token in tokens[1:]):
        return None
    try:
        width, height, maximum = map(int, tokens[1:])
    except ValueError:
        return None
    if width <= 0 or height <= 0 or maximum != 255 or cursor == len(raw):
        return None
    if raw[cursor] not in b' \t\r\n\v\f':
        return None
    cursor += 2 if raw[cursor:cursor + 2] == b'\r\n' else 1
    return (width, height) if len(raw) - cursor == width * height * 3 else None


def read_frame(path: Path) -> tuple[bytes | None, dict]:
    """Read an immutable published frame, or reject an incomplete/in-place write."""
    try:
        with path.open('rb') as stream:
            before = os.fstat(stream.fileno())
            raw = stream.read(MAX_FRAME_BYTES + 1)
            after = os.fstat(stream.fileno())
    except FileNotFoundError:
        return None, dict(status='missing')
    except OSError as error:
        return None, dict(status='unreadable', error=str(error))
    if file_signature(before) != file_signature(after):
        return None, dict(status='changed_during_read')
    dimensions = complete_ppm_dimensions(raw)
    if dimensions is None:
        return None, dict(status='invalid_or_incomplete_ppm')
    return raw, dict(status='captured', width=dimensions[0], height=dimensions[1],
                     size=len(raw), source_mtime_ns=after.st_mtime_ns)


class FrameSnapshots:
    """Periodic observations, not a renderer-liveness or boot-success oracle."""
    def __init__(self, run: Path, interval: float, started: float):
        self.run, self.interval, self.started = run, interval, started
        self.next_elapsed = 0.0
        self.observations = []
        self.previous_sha256 = None

    def manifest(self) -> dict:
        return dict(interval_seconds=self.interval, clock='monotonic_since_gui_launch',
                    meaning='frame observations only; unchanged images do not prove renderer liveness or boot',
                    observations=self.observations)

    def poll(self, now: float) -> None:
        elapsed = now - self.started
        if elapsed < self.next_elapsed:
            return
        # Skip missed intervals: do not fabricate historical observations.
        self.next_elapsed = (math.floor(elapsed / self.interval) + 1) * self.interval
        raw, metadata = read_frame(self.run / 'screen.ppm')
        record = dict(elapsed_seconds=elapsed, **metadata)
        directory = self.run / 'frames'
        directory.mkdir(exist_ok=True)
        if raw is not None:
            digest = hashlib.sha256(raw).hexdigest()
            filename = f'{len(self.observations) + 1:06d}.ppm'
            temporary_frame = directory / f'{filename}.tmp'
            temporary_frame.write_bytes(raw)
            temporary_frame.replace(directory / filename)
            record.update(file=f'frames/{filename}', sha256=digest,
                          same_as_previous_capture=(digest == self.previous_sha256))
            self.previous_sha256 = digest
        self.observations.append(record)
        temporary = directory / 'manifest.json.tmp'
        temporary.write_text(json.dumps(self.manifest(), indent=2) + '\n')
        temporary.replace(directory / 'manifest.json')


def checkpoint_metadata(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) < CHECKPOINT_HEADER.size:
        raise RuntimeError(f'incomplete DSP checkpoint: {path}')
    fields = CHECKPOINT_HEADER.unpack_from(raw)
    magic, schema, endian, header_size, state_size = fields[:5]
    component_sizes = list(fields[5:14])
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or
            len(raw) != header_size + payload_size or
            not state_size or not all(component_sizes) or
            l2_size != 0x40000 or sdram_size != 0x2000000 or
            page_size != 4096 or page_count != sdram_size // page_size or
            present_pages > page_count or
            fnv1a(memoryview(raw)[header_size:]) != payload_checksum):
        raise RuntimeError(f'incompatible or incomplete DSP checkpoint: {path}')
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = header_size + state_size + l2_size
    bitmap_size = (page_count + 7) // 8
    l1d_size = 0x8000 if schema >= 12 else 0
    expected_payload = (state_size + l2_size + shared_size + l1d_size + bitmap_size +
                        present_pages * page_size)
    if payload_size != expected_payload:
        raise RuntimeError(f'incompatible or incomplete DSP checkpoint: {path}')
    l1d_start = header_size + state_size + l2_size + shared_size
    bitmap_start = l1d_start + l1d_size
    bitmap = raw[bitmap_start:bitmap_start + bitmap_size]
    if sum(byte.bit_count() for byte in bitmap) != present_pages:
        raise RuntimeError(f'incompatible or incomplete DSP checkpoint: {path}')
    shared = raw[shared_start:shared_start + shared_size]
    return dict(file=path.name, sha256=hashlib.sha256(raw).hexdigest(),
                size=len(raw), schema=schema, endian='little', state_size=state_size,
                component_sizes=component_sizes, l2_size=l2_size,
                scheduler_state_captured=schema >= 11,
                dsp_scheduler_mode=checkpoint_scheduler_mode(
                    raw, header_size, state_size, schema),
                shared_ram_size=shared_size,
                shared_ram_sha256=(hashlib.sha256(shared).hexdigest()
                                   if shared_size else None),
                l1d_size=l1d_size,
                l1d_sha256=(hashlib.sha256(
                    raw[l1d_start:l1d_start + l1d_size]).hexdigest()
                    if l1d_size else None),
                sdram_size=sdram_size, page_size=page_size,
                page_count=page_count, present_pages=present_pages,
                payload_checksum=f'{payload_checksum:016x}')


def dsp_source_hashes() -> dict[str, str]:
    sources = sorted((ROOT / 'emulator/qemu').glob('cdj_c674*.c')) + \
              sorted((ROOT / 'emulator/qemu').glob('cdj_c674*.h')) + \
              [ROOT / 'emulator/qemu' / name for name in (
                  'cdj_dsp_checkpoint.c', 'cdj_dsp_checkpoint.h',
                  'cdj_dsp_scheduler.c', 'cdj_dsp_scheduler.h', 'cdj2000_nxs_hpi.c')]
    return {str(path.relative_to(ROOT)): sha256(path) for path in sources if path.is_file()}


def finalize_dsp_artifacts(run: Path, firmware: Path, functional_dsp_timing: bool,
                           functional_dsp_audio: bool,
                           capture_dsp_tx: bool,
                           dsp_scheduler_mode: str,
                           main_firmware: Path | None = None,
                           *, dsp_checkpoint_policy: str = 'all',
                           source_sha256_at_launch: dict[str, str] | None = None) -> None:
    if dsp_checkpoint_policy not in {'all', 'fault'}:
        raise ValueError('DSP checkpoint policy must be all or fault')
    checkpoint_dir = run / 'dsp-checkpoints'
    checkpoints = [checkpoint_metadata(path) for path in sorted(checkpoint_dir.glob('*.cdjdsp'))]
    # Every checkpoint has been structurally and checksum validated above.  In
    # lightweight mode this is still useful diagnostic evidence even though
    # the connected event transcript is intentionally absent.
    checkpoint_capture_complete = bool(checkpoints)
    events = run / 'dsp-events.jsonl'
    event_counts = Counter()
    last_sequence = 0
    boot_phases = []
    if events.is_file():
        with events.open() as stream:
            for line in stream:
                event = json.loads(line)
                if event['sequence'] != last_sequence + 1:
                    raise RuntimeError('DSP event transcript sequence is incomplete')
                last_sequence = event['sequence']
                event_counts[event['event']] += 1
                if event['event'] == 'boot_phase':
                    boot_phases.append(event['value'])
    tx_path = run / 'dsp-tx.jsonl'
    if capture_dsp_tx and not tx_path.is_file():
        raise RuntimeError('requested DSP transmit capture is missing')
    tx_capture = tx_capture_metadata(tx_path) if capture_dsp_tx else None
    source_at_exit = dsp_source_hashes()
    source_hashes = source_at_exit if source_sha256_at_launch is None else source_sha256_at_launch
    source_changed = source_at_exit != source_hashes
    # Eligibility is a claim about a capture that produced checkpoints and a
    # transcript, so it cannot outrank `complete`.  Three recorded capture
    # manifests still claim it while complete is false: nxs-browse-blocker-
    # control-1, nxs-dsp-batch-strict-2 and optimization-11-profile.
    capture_complete = bool(checkpoint_capture_complete and events.is_file() and
                            (not capture_dsp_tx or tx_capture is not None))
    manifest = dict(schema=11, format=('ABI-bound native state including C6747 INTC/Timer64P/SPI/cache/McASP TX, EDMA, SYSCFG priority, WM8740 control, timed SPI1 transfer and declared DSP activation-scheduler state, '
                                     'L2 and shared RAM plus sparse zero-default SDRAM pages'),
        dsp_timing_mode=('functional-runahead' if functional_dsp_timing else 'strict'),
        dsp_audio_mode=('coarse-packet-slots' if functional_dsp_audio else 'stopped-clock'),
        dsp_scheduler_mode=dsp_scheduler_mode,
        dsp_checkpoint_policy=dsp_checkpoint_policy,
        checkpoint_capture_complete=checkpoint_capture_complete,
        architectural_validation_eligible=capture_complete and not source_changed and not (
            functional_dsp_timing or functional_dsp_audio or
            dsp_scheduler_mode != 'legacy'),
        byte_order=sys.byteorder,
        complete=capture_complete,
        checkpoints=checkpoints, latest=checkpoints[-1]['file'] if checkpoints else None,
        event_transcript=dict(file=events.name, sha256=sha256(events) if events.is_file() else None,
                              events=last_sequence, counts=dict(sorted(event_counts.items())),
                              boot_phases=boot_phases),
        dsp_tx_capture=tx_capture,
        firmware_sha256={
            'main-firmware.bin': sha256(main_firmware or firmware / 'main-firmware.bin'),
            'gui-boot-memory.elf': sha256(firmware / 'gui-boot-memory.elf'),
            'gui-flash-image.bin': sha256(firmware / 'gui-flash-image.bin')},
        main_firmware_path=str((main_firmware or firmware / 'main-firmware.bin').resolve()),
        source_sha256=source_hashes,
        source_sha256_observed_at_exit=source_at_exit,
        sources_changed_during_run=source_changed,
        source_provenance_note=('Source hashes observed at launch and exit; these do not prove '
                                'which sources built the executable. run.json identifies the binary.'
                                if source_sha256_at_launch is not None else
                                'Source hashes observed only at finalization; run.json identifies the binary.'),
        approximations=[
            'DSP boot ROM is not executed; its documented HPI-ready handoff is modeled',
            'checkpoint schema 11 is ABI-bound and rejects structure-size or endianness changes',
            '128 KiB C6747 shared RAM is captured losslessly',
            'sparse SDRAM pages are lossless because omitted pages restore as zero',
            'SDRAM command timing, arbitration and retention are not modeled',
            'EMIFB mirrors populated 32 MiB SDRAM through the C0000000-DFFFFFFF aperture; upper D-window decoding is inferred from MPU2 coverage and unused SDRAM address pins (SPRUH91D 5.2.2 and 19.2.6.10), not hardware-validated; MPU protection and geometry reconfiguration are not modeled',
            'EDMA ICR is write-only (SPRUH91D 16.4.2.6.5); read-zero is a firmware compatibility choice, not a hardware-validated read value',
            'PSC transition ticks and PLL divider GO latency remain deterministic approximations',
            'physical HPI pins, FIFO/HRDY timing and DSP interrupt delivery are not modeled',
            'the reciprocal approximations RCPSP/RCPDP/RSQRSP/RSQRDP deliver a correct exponent and a mantissa within the 2^-8 the manual specifies, but their bits below the eighth mantissa position are not hardware-exact; firmware that refines the seed (the documented Newton-Raphson use) converges regardless, firmware that consumes it directly may diverge',
            # Unconditional: the Timer64P counter advances from the CPU's
            # cycle_tick in every mode, strict and functional alike, so this
            # board asserts the same step-to-tick fiction the replay manifest
            # declares. SPRUH91D 28.1.5.2.1 binds the count unit to the
            # PLL-derived internal clock, so substituting an emulated issue
            # cycle diverges from a documented relation rather than filling a
            # gap the manual leaves open - which is why it is declared here and
            # not only in tools/cdj_dsp/replay.py.
            'Timer64P counts one input clock per emulated CPU cycle (SPRUH91D chapter 28 register order, not rate); the step-to-tick ratio is unrelated to AUXCLK, so no elapsed-time, frequency or audio-rate conclusion may be drawn from a timer period expiring',
            *(['functional run-ahead adds two SPLOOPD epilog cycles; not cycle-validation evidence']
              if functional_dsp_timing else []),
            *(['functional run-ahead collapses each evidence-backed SPI1/WM8740 transfer to its committing write; not SPI timing evidence']
              if functional_dsp_timing else []),
            # SPRUFE8B 7.7.3.1 resumes an interrupted loop by re-executing
            # its prolog from program memory, so this is not a timing mode and
            # the caveat applies in strict timing too.
            'an interrupted SPLOOP resumes by rebuilding the loop buffer from program memory (SPRUFE8B 7.7.3.1); a loop body changed between the interrupt and the return is undetected once an ISR software loop has replaced the retained cross-check',
            *(['interrupt entry retires already-issued results with minimum empty cycles; exact interrupt pipeline latency is not modeled']
              if functional_dsp_timing else []),
            *(['functional McASP scheduling advances one slot every 1024 executed DSP packets; '
               'not serializer-clock, sample-rate, or audio-output evidence']
              if functional_dsp_audio else []),
            *(['deferred-v1 divides each bounded DSP activation into 4096-step QEMU timer slices; '
               'this host scheduling approximation is not a DSP timing fix, frequency model, or hardware proof']
              if dsp_scheduler_mode == 'deferred-v1' else [])])
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    (checkpoint_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def run_succeeded(result: dict) -> bool:
    """A stop request must not hide a concurrent board or finalization failure."""
    return bool(not result.get('error') and not result.get('finalization_error')
                and not result.get('timed_out')
                and result.get('main_exit_before_teardown') is None
                and result.get('gui_exit') in (None, 0)
                and (result.get('stop_requested') or result.get('viewer_closed')
                     or (result.get('gui_exit') == 0 and result.get('frame_exists'))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', nargs='?', type=Path,
                        help='new run directory, relative to repository; defaults to a timestamped path under runs/')
    parser.add_argument('--timestamp-run', action='store_true',
                        help='force a timestamped run directory (useful when a positional name is not desired)')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--frame-interval', type=float, default=0,
                        help='save complete framebuffer observations every N seconds (0 disables)')
    parser.add_argument('--qemu-sync-profile', action='store_true',
                        help='profile QEMU lock waits; observer overhead changes host timing')
    parser.add_argument('--ui', action='store_true',
                        help='open the interactive deck; closing it stops this run')
    parser.add_argument('--debug', action='store_true',
                        help='enable run-local QMP and localhost GDB on --port + 3')
    parser.add_argument('--debug-paused', action='store_true',
                        help='with --debug, hold MAIN at reset until debugger/resume; GUI time still runs')
    parser.add_argument('--cosim', action='store_true',
                        help='both boards in one guest time (emulator/qemu/cdj2000_cosim.c): '
                             'MAIN under -icount and started with the GUI, the GUI on its '
                             'virtual time base, the link on --port + 5 with a fixed latency; '
                             'needs a machine with the CDJ-2000 link (cdj2000-main)')
    parser.add_argument('--cosim-quantum-us', type=int, default=100,
                        help='--cosim link latency and lookahead in microseconds (100)')
    parser.add_argument('--cosim-shift', type=int, default=2,
                        help='--cosim: MAIN runs 2^N ns per instruction (-icount shift=N)')
    parser.add_argument('--panel-rev2', action='store_true',
                        help='the GUI board reads PF3 = 1, the late "/2" panel revision '
                             '(BFIN_GPIO_STRAP=0x8:0x8)')
    parser.add_argument('--lightweight', action='store_true',
                        help='capture DSP checkpoints only on faults; omit the event transcript')
    parser.add_argument('--sd', type=Path,
                        help='raw FAT32 SD image; writes go to a temporary overlay')
    parser.add_argument('--test-track', action='store_true',
                        help='create a disposable FAT32 TESTTONE.WAV fixture inside the run and attach it as SD')
    parser.add_argument('--usb', type=Path,
                        help='raw FAT32 USB image; writes go to a temporary overlay')
    parser.add_argument('--gui-firmware', type=Path,
                        help='directory containing development gui-boot-memory.elf and gui-flash-image.bin')
    parser.add_argument('--trace-media', action='store_true',
                        help='log SD/USB host activity for media diagnosis (changes host timing)')
    parser.add_argument('--fresh-link', action='store_true',
                        help='diagnostic: deliver each real MAIN frame once, without cached repeats')
    parser.add_argument('--trace-link-tx', action='store_true',
                        help='record actual GUI SPORT transmit frames for loss/queue diagnosis')
    parser.add_argument('--sd-insert-seconds', type=int,
                        help='SD insertion time after reset in virtual seconds (0 keeps slot empty)')
    parser.add_argument('--source-key', default=None,
                        help="SOURCE key to press on the panel schedule: 'sd', "
                             "'usb', 'link', 'disc', 'rekordbox', 'none', or "
                             "a raw BYTE:MASK such as 19:08. "
                             "Defaults to 'sd' when --sd or --test-track is given")
    parser.add_argument('--source-key-at', type=float,
                        help='virtual seconds at which to press it; defaults to '
                             'two seconds after insertion. This schedule does '
                             'not wait for NXS media-manager readiness')
    parser.add_argument('--source-key-retries', type=int, default=0,
                        help='repeat the source press this many times while '
                             'media manager settles (0 keeps one press)')
    parser.add_argument('--source-key-retry-interval', type=float, default=120,
                        help='virtual seconds between source retries')
    parser.add_argument('--gui-link', metavar='HOST:PORT',
                        help='point the GUI at this address instead of MAIN. Use '
                             'it to put tools.cdj_main.link_inject between the '
                             'boards for transport diagnostics. Native NXS '
                             'ENTER and LOAD are verified through panel input '
                             '(NXS_LINK_LOADING.md). MAIN still listens on --port')
    parser.add_argument('--browse-aids', action='store_true',
                        help="opt into the legacy CDJ-2000 board-side aids "
                             "described in RUNNING.md (not a verified NXS "
                             "browse fix): the status repeat is rewritten into the "
                             "fresh shape and MAIN's browse replies are "
                             "re-stamped with the cursor being answered. This "
                             "changes link bytes, so a run using it is media "
                             "evidence and not link-fidelity evidence")
    parser.add_argument('--panel-hold-ms', type=int, default=3300,
                        help='how long each scheduled key stays down. MAIN builds '
                             'a status record every 3.05 s when nothing else '
                             'changes and a press only lands if one falls inside '
                             'it, so the default is deliberately longer than that')
    parser.add_argument('--port', type=int, default=5980)
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/qemu/build/qemu-system-sh4')
    parser.add_argument('--main-firmware', type=Path,
                        help='isolated address-zero MAIN flash image; leaves stock firmware untouched')
    parser.add_argument('--trace-bus', action='store_true',
                        help='log unmodeled external-bus accesses; does not implement the missing devices')
    parser.add_argument('--ethernet-peer-port', type=int,
                        help='connect modeled Ethernet to a framed test peer on 127.0.0.1 only')
    parser.add_argument('--functional-dsp-timing', action='store_true',
                        help='run past the unresolved SPLOOPD epilog with a labeled two-cycle approximation')
    parser.add_argument('--functional-dsp-audio', action='store_true',
                        help='schedule coarse McASP TX slots to exercise genuine firmware DMA/ISR flow')
    parser.add_argument('--capture-dsp-tx', action='store_true',
                        help='capture genuine XBUF words consumed by coarse McASP slot progression')
    parser.add_argument('--deferred-dsp-scheduling', action='store_true',
                        help='opt into diagnostic 4096-step deferred DSP scheduling (not timing evidence)')
    args = parser.parse_args()
    if args.sd_insert_seconds is not None and (not (args.sd or args.test_track) or
                                             not 0 <= args.sd_insert_seconds <= 86400):
        parser.error('--sd-insert-seconds requires --sd or --test-track and a value from 0 to 86400')
    if args.test_track and args.sd:
        parser.error('--test-track cannot be combined with --sd')
    if args.timestamp_run and args.run is not None:
        parser.error('--timestamp-run cannot be combined with a positional run directory')
    if args.capture_dsp_tx and not args.functional_dsp_audio:
        parser.error('--capture-dsp-tx requires --functional-dsp-audio')
    if args.gui_link is not None:
        host, _, port = args.gui_link.partition(':')
        if not host or not port.isdigit() or not 1024 <= int(port) <= 65535:
            parser.error('--gui-link must be HOST:PORT with port 1024..65535')
    if not 0 <= args.panel_hold_ms <= 60000:
        parser.error('--panel-hold-ms must be 0..60000')
    if args.source_key_at is not None and not 0 <= args.source_key_at <= 86400:
        parser.error('--source-key-at must be 0..86400 virtual seconds')
    if not 0 <= args.source_key_retries <= 15:
        parser.error('--source-key-retries must be 0..15')
    if (not math.isfinite(args.source_key_retry_interval) or
            args.source_key_retry_interval <= 0):
        parser.error('--source-key-retry-interval must be finite and positive')
    if args.ethernet_peer_port is not None and not 1024 <= args.ethernet_peer_port <= 65535:
        parser.error('--ethernet-peer-port must be 1024..65535')
    if not math.isfinite(args.seconds) or args.seconds <= 0 or not 1024 <= args.port <= 65531:
        parser.error('positive duration and port 1024..65531 required')
    if args.debug_paused and not args.debug:
        parser.error('--debug-paused requires --debug')
    if not math.isfinite(args.frame_interval) or args.frame_interval < 0:
        parser.error('--frame-interval must be finite and nonnegative')
    run = automatic_run_path() if args.timestamp_run or args.run is None else (ROOT / args.run).resolve()
    if run.exists():
        parser.error(f'run directory already exists: {run}')
    if args.cosim and args.qemu_sync_profile and not UNIX_CONTROL:
        parser.error('--cosim takes --port + 5, which --qemu-sync-profile uses for the monitor here')
    try:
        occupied = occupied_local_ports(args.port, args.debug)
        if args.cosim:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                try:
                    probe.bind(('127.0.0.1', args.port + 5))
                except OSError:
                    occupied.append(args.port + 5)
    except PermissionError as error:
        parser.error(f'localhost port preflight unavailable: {error}; grant socket probe permission or choose a permitted environment')
    except OSError as error:
        parser.error(f'cannot probe localhost ports: {error}')
    if occupied:
        ports = ', '.join(str(port) for port in occupied)
        parser.error(f'localhost port(s) already in use: {ports}; choose another --port')
    qmp_path = os.path.relpath(run / 'qmp.sock', ROOT)
    if UNIX_CONTROL and args.debug and (',' in qmp_path or len(os.fsencode(qmp_path)) >= 104):
        parser.error('debugging requires a shorter run path without commas')
    monitor_path = os.path.relpath(run / 'qemu-monitor.sock', ROOT)
    if UNIX_CONTROL and args.qemu_sync_profile and (',' in monitor_path or
                                   len(os.fsencode(monitor_path)) >= 104):
        parser.error('sync profiling requires a shorter run path without commas')
    qmp_endpoint = (qmp_path if UNIX_CONTROL else f'127.0.0.1:{args.port + 1}')
    monitor_endpoint = (monitor_path if UNIX_CONTROL else f'127.0.0.1:{args.port + 5}')
    firmware = ROOT / 'firmware/nxs'
    main_firmware = (args.main_firmware or firmware / 'main-firmware.bin').resolve()
    gui_firmware = (args.gui_firmware or firmware).resolve()
    if args.gui_firmware and not gui_firmware.is_dir():
        parser.error(f'GUI firmware directory does not exist: {gui_firmware}')
    args.qemu = resolve_qemu(args.qemu)
    simulator = resolve_simulator()
    for path in (args.qemu, simulator, main_firmware,
                 gui_firmware / 'gui-boot-memory.elf',
                 gui_firmware / 'gui-flash-image.bin'):
        if not path.is_file(): parser.error(f'missing input: {path}')
    inputs = dict(qemu=args.qemu, simulator=simulator,
                  main_firmware=main_firmware,
                  gui_boot=gui_firmware / 'gui-boot-memory.elf',
                  gui_flash=gui_firmware / 'gui-flash-image.bin')
    from tools.cdj_main.test_media import create as create_test_media, media_drives
    test_track_manifest = None
    try:
        # Validate an explicitly supplied USB before creating the run.  The
        # generated fixture is created below, inside the new run directory.
        if args.test_track:
            media_drives(None, args.usb)
            run.mkdir(parents=True, exist_ok=False)
            test_track_directory = run / 'test-media'
            test_track_manifest = create_test_media(test_track_directory)
            sd_image = test_track_directory / test_track_manifest['image']
        else:
            sd_image = args.sd
        media_command, media_inputs = media_drives(sd_image, args.usb)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if not run.exists():
        try:
            run.mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            parser.error(f'run directory already exists: {run}')
    gui_board = ROOT / 'emulator/cdj2000-gui-nxs.hw'
    if args.gui_firmware:
        gui_board = run / 'gui-board.hw'
        try:
            write_gui_board_override(
                ROOT / 'emulator/cdj2000-gui-nxs.hw',
                gui_firmware / 'gui-flash-image.bin', gui_board)
        except (OSError, ValueError) as error:
            parser.error(f'cannot create GUI board override: {error}')
    # The stock board is a repository input already represented by the
    # command line.  Hash the generated board only when an override creates a
    # new run-local artifact.
    if args.gui_firmware:
        inputs['gui_board'] = gui_board
    inputs.update(media_inputs)
    input_artifacts = {name: input_metadata(path) for name, path in inputs.items()}
    main_command = [str(args.qemu.resolve()), '-M', 'cdj2000nxs-main', '-bios', str(main_firmware),
        '-display', 'none', '-no-reboot', '-d', 'unimp,guest_errors', '-D', str(run / 'main.log'),
        '-serial', f'tcp:127.0.0.1:{args.port},server,nowait',
        '-serial', f'tcp:127.0.0.1:{args.port + 2},server,nowait', '-serial', 'null']
    main_command += media_command
    if args.debug:
        if UNIX_CONTROL:
            main_command += ['-qmp', f'unix:{qmp_path},server=on,wait=off',
                             '-gdb', f'tcp:127.0.0.1:{args.port + 3}']
        else:
            main_command += ['-qmp', f'tcp:{qmp_endpoint},server=on,wait=off',
                             '-gdb', f'tcp:127.0.0.1:{args.port + 3}']
        if args.debug_paused and not args.cosim:
            main_command += ['-S']
    if args.cosim:
        # MAIN's time is its instruction count, and it waits paused until the
        # GUI connects, so both boards start at guest time 0.
        main_command += ['-icount', f'shift={args.cosim_shift},sleep=off', '-S']
    if args.ethernet_peer_port is None:
        main_command += ['-nic', 'none']
    else:
        # No bridge, physical interface, DNS or arbitrary host selection.
        # Guest services (including modified firmware's debug console) stay
        # on this raw-Ethernet test connection, not the host's real network.
        main_command += ['-nic', 'socket,model=cdj-nxs-ethernet,id=nxsnet,'
                         f'connect=127.0.0.1:{args.ethernet_peer_port}']
    if args.qemu_sync_profile:
        if UNIX_CONTROL:
            main_command += ['-enable-sync-profile', '-monitor',
                             f'unix:{monitor_path},server=on,wait=off']
        else:
            main_command += ['-enable-sync-profile', '-monitor',
                             f'telnet:{monitor_endpoint},server,nowait']
    gui_command = [str(simulator), '--model', 'bf531', '--environment', 'operating', '--memory-region', '0,64M',
        '--hw-board-file', str(gui_board) if args.gui_firmware else 'emulator/cdj2000-gui-nxs.hw',
        str(gui_firmware / 'gui-boot-memory.elf')]
    overrides = dict(BFIN_PARALLEL_WRITEBACK='1', BFIN_GUI_COLOR='rgb555le',
        BFIN_GUI_OUTPUT=str(run / 'screen.ppm'),
        BFIN_MAIN_LINK=args.gui_link or f'127.0.0.1:{args.port}',
        BFIN_MAIN_LINK_DUMP=str(run / 'main-link.bin'), BFIN_GPIO5_READY_TOGGLE='1',
        BFIN_STATS='5', BFIN_EXCEPTION_TRACE='1', BFIN_EXIT_AFTER_WALL=str(args.seconds))
    if args.fresh_link or args.cosim:
        # --cosim: a wire delivers what MAIN sent, once (see boot_vm --cosim).
        overrides['BFIN_LINK_FRESH_ONLY'] = '1'
    if args.panel_rev2:
        overrides['BFIN_GPIO_STRAP'] = '0x8:0x8'
    if args.cosim:
        overrides['BFIN_COSIM'] = f'127.0.0.1:{args.port + 5}'
        overrides['BFIN_COSIM_QUANTUM_US'] = str(args.cosim_quantum_us)
    if args.trace_link_tx:
        overrides['BFIN_SPORT_TX_OUTPUT'] = str(run / 'gui-link-tx.bin')
    # Do not inherit replay/proxy data or a firmware shortcut from the shell.
    gui_env = {k:v for k,v in os.environ.items() if not k.startswith('BFIN_')}
    gui_env.update(overrides)
    main_env = {k:v for k,v in os.environ.items() if not k.startswith('CDJ_')}
    main_env['CDJ_INPUT_PORT'] = str(args.port + 4)
    if args.cosim:
        main_env['CDJ_COSIM'] = str(args.port + 5)
        main_env['CDJ_COSIM_QUANTUM_US'] = str(args.cosim_quantum_us)
    main_env['CDJ_PANEL_FRAME'] = neutral_frame().hex()
    main_env['CDJ_NXS_SD_LID'] = 'closed'
    if args.trace_bus:
        main_env['CDJ_BUS_TRACE'] = '1'
    if args.sd_insert_seconds is not None:
        main_env['CDJ_SD_INSERT'] = str(args.sd_insert_seconds)
    # Preserve the existing insertion-relative key schedule. It does not wait
    # for NXS media-manager readiness: the filesystem can be mounted while
    # the browser still answers NO CARD. See NXS_BROWSE_BLOCKER.md. Explicit
    # options are necessary because inherited CDJ_ variables are sanitized.
    source_key = args.source_key or ('sd' if (args.sd or args.test_track) else 'none')
    if source_key != 'none':
        contact = NXS_SOURCE_KEYS.get(source_key)
        if contact is None:
            byte, _, mask = source_key.partition(':')
            try:
                contact = (int(byte, 10), int(mask, 16))
            except ValueError:
                contact = None
            if contact is None or not (0 <= contact[0] <= 21) or not (1 <= contact[1] <= 255):
                parser.error("--source-key must be sd, usb, link, disc, "
                             "rekordbox, none or BYTE:MASK")
        insert_at = args.sd_insert_seconds if args.sd_insert_seconds is not None else 20
        at = args.source_key_at if args.source_key_at is not None else insert_at + 2.0
        main_env['CDJ_PANEL_KEYS'] = source_schedule(
            at, contact, args.source_key_retries,
            args.source_key_retry_interval)
        main_env['CDJ_PANEL_HOLD_MS'] = str(args.panel_hold_ms)
    if args.trace_media:
        main_env['CDJ_SDHI_TRACE'] = '1'
        main_env['CDJ_USBH_TRACE'] = '1'
    main_env['CDJ_NXS_HPI_DUMP'] = str(run / 'dsp-l2.bin')
    main_env['CDJ_NXS_DSP_CHECKPOINT_DIR'] = str(run / 'dsp-checkpoints')
    main_env['CDJ_NXS_DSP_CHECKPOINT_POLICY'] = 'fault' if args.lightweight else 'all'
    main_env['CDJ_NXS_DSP_CHECKPOINT_REQUEST'] = str(run / 'dsp-checkpoint-request.json')
    if not args.lightweight:
        main_env['CDJ_NXS_DSP_EVENTS'] = str(run / 'dsp-events.jsonl')
    else:
        main_env.pop('CDJ_NXS_DSP_EVENTS', None)
    dsp_scheduler_mode = ('deferred-v1' if args.deferred_dsp_scheduling else
                          'legacy')
    # Always override any inherited policy. Deferred scheduling changes the
    # connected host/DSP interleaving and must be an explicit run option.
    main_env['CDJ_NXS_DSP_SCHEDULER'] = dsp_scheduler_mode
    if args.functional_dsp_timing:
        main_env['CDJ_NXS_DSP_FUNCTIONAL_TIMING'] = '1'
    if args.functional_dsp_audio:
        main_env['CDJ_NXS_DSP_FUNCTIONAL_AUDIO'] = '1'
    if args.capture_dsp_tx:
        main_env['CDJ_NXS_DSP_TX_CAPTURE'] = str(run / 'dsp-tx.jsonl')
    # Genuine NXS validation must transport the firmware's bytes unchanged, so
    # both board-side aids stay off by default. They are not optional for one
    # job, though: RUNNING.md measures the card's library reaching the screen in
    # 0.6-2.6 s of the SOURCE key with them and the key being lost more often
    # than not without them, because MAIN's status records collapse and the
    # GUI's browse loop for the empty boot source never ends. Hardcoding them
    # off left that path unreachable from this launcher.
    main_env['CDJ_REQ_STATUS_FRESH'] = '1' if args.browse_aids else '0'
    main_env['CDJ_LINK_LINK_ROWS'] = 'match' if args.browse_aids else 'off'
    run_manifest = dict(main=main_command, gui=gui_command,
        dsp_source_sha256_at_launch=dsp_source_hashes(),
        endpoints=dict(panel_host='127.0.0.1', panel_port=args.port + 4,
                       qmp=qmp_endpoint if args.debug else None,
                       gdb_host='127.0.0.1' if args.debug else None,
                       gdb_port=args.port + 3 if args.debug else None),
        debug=dict(enabled=args.debug, main_starts_paused=args.debug_paused,
                   pause_scope='MAIN only; GUI and host deadlines continue'),
        # Fault-only mode still captures diagnostic checkpoints when a DSP
        # fault occurs; the separate event transcript is what lightweight
        # mode omits.  Keep the compatibility flag true and make both modes
        # explicit for agents consuming run.json.
        dsp_capture_enabled=True,
        dsp_capture_mode=('fault-only' if args.lightweight else 'full'),
        dsp_event_capture_enabled=not args.lightweight,
        dsp_checkpoint_policy='fault' if args.lightweight else 'all',
        gui_environment=overrides, main_environment={k:v for k,v in main_env.items() if k.startswith('CDJ_')},
        dsp='NXS UHPI plus partial C674x interpreter; incomplete ISA, ROM handoff abstraction', profile='experimental NXS',
        iic_model=dict(endpoint='Apple 2.0C identity registers 0/1 only; no cryptographic authentication',
                       timing='event-level nine-SCL-period bytes and one-period STOP; Pck 53.950MHz',
                       limitations='START/STOP and pin timing approximate; 53.930MHz board reference discrepancy; '
                                   'no IRQ, arbitration, double buffering, repeated START or certificates'),
        input_artifacts=input_artifacts, frame_interval_seconds=args.frame_interval,
        firmware=dict(main=str(main_firmware), gui=str(gui_firmware),
                      gui_board_file=str(gui_board),
                      gui_board_mode=('run-local override' if args.gui_firmware else 'stock'),
                      test_track=test_track_manifest),
        link_delivery='fresh-only diagnostic' if args.fresh_link else 'legacy cached repeats',
        gui_link_target=args.gui_link or f'127.0.0.1:{args.port} (MAIN directly)',
        link_fidelity=('board-side browse aids ENABLED: status repeats rewritten '
                       'fresh and browse replies re-stamped; media evidence only, '
                       'not link-fidelity evidence'
                       if args.browse_aids else
                       'firmware link bytes transported unchanged'),
        media=dict(images={name: str(path) for name, path in media_inputs.items()},
                   test_track=test_track_manifest,
                   sd_lid_initial='closed; persistent physical panel contact 17/04',
                   panel_key_schedule=main_env.get('CDJ_PANEL_KEYS'),
                   panel_key_retries=args.source_key_retries,
                   panel_key_retry_interval_seconds=args.source_key_retry_interval,
                   panel_key_hold_ms=main_env.get('CDJ_PANEL_HOLD_MS'),
                   writes='temporary QEMU snapshot overlays; discarded at exit',
                   firmware_load_verified=False, audio_verified=False),
        dsp_scheduler_mode=dsp_scheduler_mode,
        dsp_sdram=dict(physical_bytes=0x02000000,
            aperture='0xc0000000-0xdfffffff',
            addressing='physical 32 MiB mirror',
            hardware_validated=False,
            limitation='D-window decode inferred from MPU2 coverage and SDRAM pin mapping; MPU protection and geometry reconfiguration unmodeled'),
        qemu_sync_profile=dict(enabled=args.qemu_sync_profile,
            commands=list(SYNC_PROFILE_COMMANDS) if args.qemu_sync_profile else [],
            monitor=monitor_endpoint if args.qemu_sync_profile else None,
            observer_overhead='Lock profiling and monitor collection add host overhead; '
                              'timings are diagnostic observations, not uninstrumented performance'),
        architectural_validation_eligible=not (
            args.lightweight or args.functional_dsp_timing or
            args.functional_dsp_audio or args.deferred_dsp_scheduling),
        scheduling_provenance=(
            'deferred-v1 is an explicit 4096-step QEMU timer-slice host scheduling approximation; '
            'it is not a DSP timing fix, frequency model, or hardware proof'
            if args.deferred_dsp_scheduling else
            'legacy synchronous bounded DSP activation'))
    run_manifest['ethernet'] = dict(
        controller='SH7764 EtherC/E-DMAC', phy='RTL8201FL-VB-CG',
        peer=(f'127.0.0.1:{args.ethernet_peer_port}' if args.ethernet_peer_port else None),
        mode='isolated framed Ethernet' if args.ethernet_peer_port else 'disconnected',
        hardware_timing_validated=False,
        approximations=['atomic descriptor DMA; unified coherent RAM; no bus arbitration or wire timing',
                        'PHY negotiation uses an explicit virtual peer and modeled delay, not analog signaling',
                        'PHY MACR write-only register 13 reads zero for firmware RMW; hardware readback unverified',
                        'PHY external reset GPIO and LED activity pulses are not modeled',
                        'only EtherC interrupt masking is modeled in INT2MSKR1',
                        'network and MAIN state are not included in DSP-only checkpoints'])
    if args.frame_interval:
        run_manifest['frame_snapshots_manifest'] = 'frames/manifest.json'
    write_json(run / 'run.json', run_manifest)
    session = dict(schema=1, state='starting', started_unix=time.time(),
                   launcher_pid=os.getpid(), processes={})
    write_json(run / 'session.json', session)
    processes = []
    result = {}
    stop_requested = False
    viewer = None
    snapshots = None
    main_process = None
    with (run / 'main-stderr.log').open('w') as mainlog, (run / 'gui.log').open('w') as guilog:
        try:
            main_process = subprocess.Popen(main_command, cwd=ROOT, env=qemu_environment(main_env), stdin=subprocess.DEVNULL, stdout=mainlog, stderr=mainlog)
            processes.append(main_process)
            session['processes']['main'] = main_process.pid
            write_json(run / 'session.json', session)
            time.sleep(1)
            if main_process.poll() is not None: raise RuntimeError('MAIN exited; see main-stderr.log')
            gui = subprocess.Popen(gui_command, cwd=ROOT, env=gui_env, stdin=subprocess.DEVNULL, stdout=guilog, stderr=guilog)
            processes.append(gui)
            session['processes']['gui'] = gui.pid
            if args.frame_interval:
                snapshots = FrameSnapshots(run, args.frame_interval, time.monotonic())
            if args.ui:
                viewer = subprocess.Popen([sys.executable, '-m', 'tools.cdj_gui.view_ui',
                    '--attach', '--device-name', 'CDJ-2000NXS', '--nxs-panel',
                    '--run', str(run),
                    '--output', str(run / 'screen.ppm'),
                    '--control-port', str(args.port + 4)], cwd=ROOT)
                processes.append(viewer)
                session['processes']['viewer'] = viewer.pid
            session['state'] = 'running'
            write_json(run / 'session.json', session)
            print(f'MAIN {main_process.pid}, GUI {gui.pid}; logs: {run}', flush=True)
            print_agent_commands(run, args.port, args.debug,
                                  qmp_endpoint if args.debug else None)
            deadline = time.monotonic() + args.seconds + 5
            while gui.poll() is None and time.monotonic() < deadline:
                if (run / 'stop-request.json').is_file():
                    stop_requested = True
                    break
                if snapshots is not None:
                    snapshots.poll(time.monotonic())
                if main_process.poll() is not None: raise RuntimeError('MAIN exited during run')
                if viewer is not None and viewer.poll() is not None:
                    if viewer.returncode != 0: raise RuntimeError('Deck viewer exited with an error')
                    break
                time.sleep(.1)
            if (run / 'stop-request.json').is_file():
                stop_requested = True
            closed = viewer is not None and viewer.poll() == 0
            result = dict(gui_exit=gui.poll(), viewer_closed=closed,
                          main_exit_before_teardown=main_process.poll(),
                          stop_requested=stop_requested,
                          timed_out=gui.poll() is None and not closed and
                          not stop_requested,
                          frame_exists=(run / 'screen.ppm').exists())
            print(json.dumps(result), flush=True)
        except (Exception, KeyboardInterrupt) as error:
            result.update(error=str(error) or type(error).__name__,
                          main_exit_before_teardown=main_process.poll() if main_process else None)
            print(f'nxs_vm: {result["error"]}; diagnostics: {run}', file=sys.stderr)
        finally:
            session['state'] = 'stopping'
            write_json(run / 'session.json', session)
            if args.qemu_sync_profile:
                run_manifest['qemu_sync_profile']['collection'] = capture_sync_profile(
                    run, main_process, endpoint=monitor_endpoint)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            if snapshots is not None:
                run_manifest['frame_snapshots'] = snapshots.manifest()
            final_inputs = {}
            for name, path in inputs.items():
                try:
                    final_inputs[name] = input_metadata(path)
                except (OSError, RuntimeError) as error:
                    final_inputs[name] = dict(error=str(error))
            run_manifest['input_artifacts_after_run'] = final_inputs
            run_manifest['inputs_differ_at_exit'] = [name for name in inputs
                if input_artifacts[name] != final_inputs[name]]
            run_manifest['input_provenance_notes'] = ('Hashes observed before launch and after exit; '
                                                       'not continuous file-mutation monitoring' +
                                                       ('. GUI board is a run-local copy with its CFI file '
                                                        'bound to the selected GUI flash image.'
                                                        if args.gui_firmware else ''))
            write_json(run / 'run.json', run_manifest)
            try:
                if not args.lightweight or any((run / 'dsp-checkpoints').glob('*.cdjdsp')):
                    finalize_dsp_artifacts(run, gui_firmware, args.functional_dsp_timing,
                                           args.functional_dsp_audio, args.capture_dsp_tx,
                                           dsp_scheduler_mode, main_firmware,
                                           dsp_checkpoint_policy=('fault' if args.lightweight else 'all'),
                                           source_sha256_at_launch=run_manifest['dsp_source_sha256_at_launch'])
            except (OSError, ValueError, RuntimeError) as error:
                result['finalization_error'] = str(error)
            write_json(run / 'result.json', result)
            session.update(state='stopped' if run_succeeded(result) else 'failed',
                           ended_unix=time.time(), result='result.json')
            write_json(run / 'session.json', session)
    return 0 if run_succeeded(result) else 1


if __name__ == '__main__':
    raise SystemExit(main())
