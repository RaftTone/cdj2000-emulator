# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded, read-only observations shared by the deck and agent CLI.

Link records are evidence of delivered messages, not proof of GUI consumption
or audio playback. An unchanged framebuffer is not a CPU-liveness test.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import time

from tools.cdj_gui.decode_link_dump import decode_list, decode_player_state, words_of

LOGS = ('main-stderr.log', 'gui.log', 'main.log')
NORMAL_DSP_STOPS = frozenset({
    'phase budget exhausted',
    'deferred slice boundary',
    'hint host-event yield',
})


def transport_observation(status: dict | None) -> dict | None:
    """Decode native NXS counter fields; a single sample cannot prove motion."""
    words = (status or {}).get('words', [])
    if len(words) < 9:
        return None

    def frames(minutes: int, packed: int) -> int | None:
        seconds, fraction = divmod(packed, 256)
        if not (0 <= minutes < 99 and 0 <= seconds < 60 and 0 <= fraction < 150):
            return None
        return (minutes * 60 + seconds) * 150 + fraction

    remaining = frames(words[5], words[6])
    duration = frames(words[7], words[8])
    if remaining is None or duration is None or duration <= 0 or remaining > duration:
        return None
    return dict(record=status.get('record'), remaining_frames=remaining,
                duration_frames=duration, elapsed_frames=duration - remaining,
                play_requested=(bool(words[17] & 0x200) if len(words) > 17 else None),
                frames_per_second=150, remaining_seconds=remaining / 150,
                duration_seconds=duration / 150,
                meaning='native counter sample; compare fresh records to establish movement')


def write_json(path: Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {'error': 'expected JSON object'}
    except (OSError, ValueError) as error:
        return {'error': str(error)}


def log_tail(path: Path, limit: int = 16384) -> str:
    try:
        with path.open('rb') as stream:
            stream.seek(0, 2)
            start = max(0, stream.tell() - limit)
            stream.seek(start)
            raw = stream.read(limit)
        if start:
            raw = raw.partition(b'\n')[2]
        return raw.decode('utf-8', 'replace')
    except FileNotFoundError:
        return ''
    except OSError as error:
        return f'[unable to read {path.name}: {error}]'


def record_action(run: Path, action: dict) -> None:
    """One append per entry so CLI and viewer can share a transcript."""
    raw = (json.dumps(dict(unix=time.time(), **action)) + '\n').encode()
    fd = os.open(run / 'actions.jsonl', os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        if os.write(fd, raw) != len(raw):
            raise OSError('incomplete action transcript write')
    finally:
        os.close(fd)


def recent_actions(run: Path) -> list[dict]:
    """Read a bounded transcript tail, tolerating an append still in flight."""
    entries = []
    for line in log_tail(run / 'actions.jsonl').splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if isinstance(entry, dict):
            entries.append(entry)
    return entries[-20:]


class LinkObserver:
    """Incremental SPRX reader: retains partial records and bounds each poll."""
    def __init__(self):
        self.identity = None
        self.offset = 0
        self.pending = b''
        self.records = 0
        self.latest_status = None
        self.latest_list = None
        self.player = None
        self.duration = None
        self.skipped_bytes = 0
        self.skipped_prefix_bytes = 0
        self.malformed_records = 0
        self.record_scope = 'incremental full file; record numbers are cumulative'

    def _consume(self, blob: bytes) -> None:
        """Decode as many complete records as are present in ``blob``."""
        cursor = 0
        while cursor + 8 <= len(blob):
            if blob[cursor:cursor + 4] != b'SPRX':
                cursor += 1
                self.skipped_bytes += 1
                continue
            size = int.from_bytes(blob[cursor + 4:cursor + 8], 'little')
            if size < 2 or size > 65536 or size % 2:
                cursor += 1
                self.skipped_bytes += 1
                continue
            if cursor + 8 + size > len(blob):
                break
            words = words_of(blob[cursor + 8:cursor + 8 + size])
            cursor += 8 + size
            self.records += 1
            entry = {'record': self.records, 'command': words[0]}
            if size == 64 and words[0] == 0:
                self.latest_status = dict(entry, words=list(words))
            elif 0x10 <= words[0] <= 0x1f and words[0] != 0x19:
                try:
                    header, rows = decode_list(words)
                except (IndexError, ValueError):
                    self.malformed_records += 1
                    continue
                if len(rows) != header['entries']:
                    self.malformed_records += 1
                self.latest_list = dict(entry, header=header,
                    rows=[{'attr': a, 'attr2': b, 'text': t} for a, b, t in rows],
                    complete=len(rows) == header['entries'])
            elif words[0] == 0x19:
                try:
                    scalars, strings = decode_player_state(words)
                except (IndexError, ValueError):
                    self.malformed_records += 1
                    continue
                self.player = dict(entry, scalars=scalars, strings=strings)
            elif words[0] == 5:
                self.duration = dict(entry, words=list(words))
        self.pending = blob[cursor:]

    def _snapshot(self, size: int, bytes_read: int, caught_up: bool) -> dict:
        return dict(status='observed', records=self.records,
                    bytes_read=bytes_read, bytes_available=size,
                    caught_up=caught_up, pending_bytes=len(self.pending),
                    skipped_bytes=self.skipped_bytes,
                    skipped_prefix_bytes=self.skipped_prefix_bytes,
                    malformed_records=self.malformed_records,
                    record_scope=self.record_scope,
                    latest_status=self.latest_status, browser=self.latest_list,
                    transport=transport_observation(self.latest_status),
                    player=self.player, duration_payload=self.duration,
                    meaning='delivered link messages; not load-completion or audio proof')

    def poll(self, path: Path, budget: int = 1024 * 1024) -> dict:
        try:
            with path.open('rb') as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if self.identity != identity or stat.st_size < self.offset:
                    self.__init__()
                    self.identity = identity
                stream.seek(self.offset)
                raw = stream.read(budget)
                self.offset += len(raw)
        except FileNotFoundError:
            return {'status': 'missing', 'records': 0}
        except OSError as error:
            return {'status': 'unreadable', 'records': self.records, 'error': str(error)}
        blob = self.pending + raw
        self._consume(blob)
        return self._snapshot(stat.st_size, self.offset,
                              self.offset >= stat.st_size)

    def poll_latest_tail(self, path: Path, budget: int = 1024 * 1024) -> dict:
        """Observe only the newest bounded tail of a link dump.

        This is for one-shot snapshots where replaying the entire history would
        exceed the observer budget.  Record numbers are deliberately local to
        this tail; callers needing cumulative numbers should retain an observer
        and use :meth:`poll` instead.
        """
        try:
            with path.open('rb') as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if stat.st_size <= budget:
                    return self.poll(path, budget)
                start = stat.st_size - budget
                stream.seek(start)
                raw = stream.read(budget)
        except FileNotFoundError:
            return {'status': 'missing', 'records': 0}
        except OSError as error:
            return {'status': 'unreadable', 'records': self.records, 'error': str(error)}

        self.__init__()
        self.identity = identity
        first = raw.find(b'SPRX')
        if first < 0:
            first = len(raw)
        self.skipped_prefix_bytes = start + first
        self.record_scope = ('latest %d-byte tail; record numbers are local to tail'
                             % budget)
        self.offset = stat.st_size
        self._consume(raw[first:])
        return self._snapshot(stat.st_size, len(raw), True)


def progress_text(link: dict) -> str:
    if not link.get('caught_up', True):
        return 'Reading earlier link observations…'
    transport = link.get('transport')
    if transport:
        state = 'playing' if transport.get('play_requested') else 'paused/stopped'
        return ('Native transport %s: %.3f s remaining / %.3f s duration.' %
                (state, transport['remaining_seconds'],
                 transport['duration_seconds']))
    browser = link.get('browser')
    if not browser:
        if link.get('skipped_prefix_bytes'):
            return 'No browser reply in the recent link window; inspect the screen or full run report.'
        return 'Waiting for a browser reply from firmware.'
    rows = [row['text'] for row in browser['rows'] if row['text'].strip()]
    if 'NO CARD' in rows:
        return 'Firmware reports NO CARD. Media may still be initializing; reselect SD after it mounts.'
    return 'Latest browser reply: ' + (' · '.join(rows)[:180] or '(empty)')


def next_steps(link: dict, session: dict | None = None,
               faults: list[dict] | None = None) -> list[str]:
    """Suggest bounded follow-up observations without treating absence as proof."""
    session = session or {}
    faults = faults or []
    state = session.get('state')
    steps: list[str] = []
    if faults:
        steps.append('Capture evidence: python -m tools.cdj_main.dev <run-dir> diagnose <new-output-dir>')
    if state in {'stopped', 'failed'}:
        steps.append('Review completed evidence: python -m tools.cdj_main.run_report <run-dir> --json')
        return steps
    browser = link.get('browser')
    no_card = any(str(row.get('text', '')).strip().upper() == 'NO CARD'
                  for row in (browser or {}).get('rows', []))
    if not browser or no_card:
        steps.append('Inspect the mount gate (--debug required): python -m tools.cdj_main.dev <run-dir> wait-media --timeout 30')
        steps.append('Wait for a known browser row: python -m tools.cdj_main.dev <run-dir> wait-browser TESTTONE.WAV --timeout 30')
        if not browser:
            steps.append('Latest-tail absence is inconclusive; use run_report --json for full-file evidence.')
    transport = link.get('transport')
    if transport:
        steps.append('Verify fresh counter movement: python -m tools.cdj_main.dev <run-dir> wait-playback --timeout 120')
    elif browser and not no_card:
        steps.append('Wait for a native duration sample: python -m tools.cdj_main.dev <run-dir> wait-duration --timeout 120')
    if not steps:
        steps.append('Capture bounded evidence: python -m tools.cdj_main.dev <run-dir> diagnose <new-output-dir>')
    return steps


def is_fault_line(line: str) -> bool:
    """Recognize emulator faults while excluding normal DSP yield markers."""
    if re.search(r'\b(fatal|double fault|unsupported opcode|DSP fault|'
                 r'failed to bind|could not open|operation not permitted|'
                 r'address already in use)\b', line, re.I):
        return True
    match = re.search(r'\bstop=([^\r\n]+)', line, re.I)
    if not match:
        return False
    reason = re.split(r'\s+(?:B15|B14|B3|pc|word|cycles|packets)=',
                      match.group(1), maxsplit=1, flags=re.I)[0].strip()
    return reason.casefold() not in NORMAL_DSP_STOPS


def observe(run: Path, observer: LinkObserver | None = None) -> dict:
    from tools.cdj_main.nxs_vm import read_frame
    incremental = observer is not None
    observer = observer or LinkObserver()
    link = (observer.poll(run / 'main-link.bin') if incremental else
            observer.poll_latest_tail(run / 'main-link.bin'))
    raw, frame = read_frame(run / 'screen.ppm')
    if raw is not None:
        frame.update(sha256=hashlib.sha256(raw).hexdigest(),
                     age_seconds=max(0, time.time() - frame['source_mtime_ns'] / 1e9))
    tails = {name: log_tail(run / name) for name in LOGS}
    stats = [line for line in tails['gui.log'].splitlines() if line.startswith('STATS ')]
    faults = [dict(file=name, line=line[:1000]) for name, tail in tails.items()
              for line in tail.splitlines() if is_fault_line(line)]
    manifest = read_json(run / 'run.json')
    session = read_json(run / 'session.json')
    result = read_json(run / 'result.json') if (run / 'result.json').exists() else None
    progress = progress_text(link)
    state = session.get('state')
    if state == 'failed' or (result and result.get('error')):
        error = (result or {}).get('error') or (result or {}).get('finalization_error')
        progress = 'Run failed: ' + (error or 'inspect result.json and diagnostic logs.')
    elif state == 'stopped':
        progress = 'Run stopped. ' + progress
    elif state == 'stopping':
        progress = 'Stopping run; finalizing artifacts.'
    elif 'error' in session:
        progress = 'Session unavailable: ' + session['error']
    elif faults:
        progress = 'Emulator fault reported: ' + faults[-1]['line'][:200]
    return dict(schema=1, run=str(run.resolve()), observed_unix=time.time(),
                session=session, result=result,
                endpoints=manifest.get('endpoints', {}),
                debug=manifest.get('debug', {}), media=manifest.get('media', {}),
                inputs=manifest.get('input_artifacts', {}),
                manifest_error=manifest.get('error'),
                frame=frame, link=link, progress=progress,
                gui_stats=stats[-1] if stats else None,
                log_tails={name: tail[-2048:] for name, tail in tails.items()},
                recent_actions=recent_actions(run),
                recent_fault_lines=faults[-20:],
                next_steps=next_steps(link, session, faults),
                fault_scope='bounded log tails only; absence is not proof of no fault')


def main(argv=None) -> int:
    """Print one bounded snapshot, making the live observer agent-callable."""
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--json', action='store_true', dest='as_json',
                        help='emit the complete machine-readable snapshot')
    args = parser.parse_args(argv)
    if not args.run.is_dir():
        parser.error(f'run directory does not exist: {args.run}')
    snapshot = observe(args.run)
    print(json.dumps(snapshot, indent=2, sort_keys=True) if args.as_json
          else snapshot['progress'])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
