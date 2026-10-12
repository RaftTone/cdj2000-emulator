"""Summarize a completed or interrupted NXS run for humans and agents.

The launcher intentionally keeps raw evidence in separate files.  That is
good for reproducibility, but makes a quick answer to "what happened?"
surprisingly expensive: an agent has to open ``run.json``, inspect the frame
manifest, decode the link dump, and grep two logs.  This module performs those
read-only operations and emits one stable JSON document (or a short text
summary) suitable for a follow-up prompt.

    python -m tools.cdj_main.run_report runs/my-run
    python -m tools.cdj_main.run_report runs/my-run --json

The report is evidence-oriented.  It reports observations such as a visible
``TESTTONE.WAV`` list entry, a command-5 response, or a successful fresh
playhead check.  Counter movement is kept separate from audio output because
the emulator does not capture an audible signal.
"""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path

from tools.cdj_gui.decode_link_dump import decode_list, words_of
from tools.cdj_main.run_state import is_fault_line, recent_actions


def _records(stream):
    """Stream bounded SPRX records; tolerate garbage and a final partial write."""
    pending = b''
    while chunk := stream.read(65536):
        pending += chunk
        cursor = 0
        while cursor + 8 <= len(pending):
            if pending[cursor:cursor + 4] != b'SPRX':
                cursor += 1
                continue
            size = int.from_bytes(pending[cursor + 4:cursor + 8], 'little')
            if size < 2 or size > 65536 or size % 2:
                cursor += 1
                continue
            if cursor + 8 + size > len(pending):
                break
            yield pending[cursor + 8:cursor + 8 + size]
            cursor += 8 + size
        pending = pending[cursor:]


def _read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text())
    except FileNotFoundError:
        return {}
    except (OSError, json.JSONDecodeError) as error:
        return {"_error": str(error)}
    return value if isinstance(value, dict) else {"_error": "expected an object"}


def _native_counter_playback_proven(run: Path) -> bool:
    """Return whether this run recorded a successful fresh playhead check."""
    try:
        stream = (run / "actions.jsonl").open(encoding="utf-8")
    except (FileNotFoundError, OSError):
        return False
    try:
        for line in stream:
            try:
                action = json.loads(line)
            except json.JSONDecodeError:
                continue
            if (isinstance(action, dict)
                    and action.get("command") == "wait-playback"
                    and action.get("outcome") == "advanced"
                    and action.get("ok") is True):
                return True
    finally:
        stream.close()
    return False


def _link_evidence(path: Path) -> dict:
    """Extract compact media evidence while preserving raw-record provenance."""
    result = dict(status="missing", records=0, lengths={}, commands={},
                  statuses=[], entries=[], load_responses=0,
                  no_card_entries=0, wav_entries=[])
    try:
        stream = path.open('rb')
    except FileNotFoundError:
        return result
    except OSError as error:
        result.update(status="unreadable", error=str(error))
        return result

    lengths = collections.Counter()
    commands = collections.Counter()
    statuses = collections.deque(maxlen=100)
    entries = collections.deque(maxlen=100)
    status_count = entry_count = no_card_count = 0
    wav_entries = set()
    load_responses = 0
    try:
        for index, body in enumerate(_records(stream), 1):
            lengths[len(body)] += 1
            words = words_of(body)
            command = words[0] if words else None
            if command is not None:
                commands[f"0x{command:04x}"] += 1
            if len(body) == 64 and command == 0:
                status_count += 1
                # These are the NXS fields that are useful during a media bring-up;
                # keep their raw values and avoid applying the legacy readiness map.
                watched = (13, 18, 19, 20, 26, 29, 30)
                statuses.append({"record": index,
                                 "words": {str(word): words[word] for word in watched
                                           if word < len(words)}})
            if command is not None and 0x10 <= command <= 0x1F and command != 0x19:
                try:
                    header, rows = decode_list(words)
                except (IndexError, ValueError):
                    continue
                for attr, attr2, text in rows:
                    entry_count += 1
                    no_card_count += text.upper() == 'NO CARD'
                    if text.upper().endswith(('.WAV', '.MP3', '.AIFF')):
                        if len(wav_entries) < 100:
                            wav_entries.add(text)
                    entries.append({"record": index, "command": command,
                                    "text": text, "attr": attr, "attr2": attr2,
                                    "cursor": header["cursor"]})
            if command == 5:
                load_responses += 1
    except OSError as error:
        result.update(status='unreadable', error=str(error))
        return result
    finally:
        stream.close()

    result.update(status="captured", records=sum(lengths.values()),
                  lengths={str(size): count for size, count in sorted(lengths.items())},
                  commands=dict(sorted(commands.items())), statuses=list(statuses),
                  status_count=status_count, entry_count=entry_count,
                  entries=list(entries), load_responses=load_responses,
                  no_card_entries=no_card_count, wav_entries=sorted(wav_entries))
    return result


def build_report(run: Path) -> dict:
    run = run.resolve()
    manifest = _read_json(run / "run.json")
    result = _read_json(run / "result.json")
    frames = _read_json(run / "frames/manifest.json")
    link = _link_evidence(run / "main-link.bin")
    checkpoint_path = run / "dsp-checkpoints/manifest.json"
    checkpoint_manifest = _read_json(checkpoint_path)
    native_counter_playback = _native_counter_playback_proven(run)

    faults = collections.deque(maxlen=100)
    fault_count = 0
    for name in ("main-stderr.log", "main.log", "gui.log"):
        path = run / name
        try:
            with path.open(errors='replace') as stream:
                for line in stream:
                    if is_fault_line(line):
                        fault_count += 1
                        faults.append({'file': name, 'line': line.rstrip()[:500]})
        except FileNotFoundError:
            continue
        except OSError as error:
            fault_count += 1
            faults.append({"file": name, "error": str(error)})
            continue

    latest_frame = None
    observations = frames.get("observations", []) if isinstance(frames, dict) else []
    if observations:
        latest_frame = observations[-1]
    return {
        "run": str(run),
        "profile": manifest.get("profile"),
        "result": result,
        "session": _read_json(run / 'session.json'),
        "endpoints": manifest.get('endpoints', {}),
        "recent_actions": recent_actions(run),
        "media": {
            "configured": manifest.get("media", {}).get("images", {}),
            "entries_seen": link["entries"],
            "entry_count": link.get('entry_count', 0),
            "entry_scope": 'latest 100 rows; up to 100 distinct track names; counters cover full log',
            "wav_entries": link["wav_entries"],
            "no_card_entries": link["no_card_entries"],
            "load_responses": link["load_responses"],
        },
        "link": {key: link[key] for key in
                 ("status", "records", "lengths", "commands", "statuses")},
        "link_status_count": link.get("status_count", len(link["statuses"])),
        "frame": {"latest": latest_frame,
                  "observations": len(observations)},
        "dsp_capture": {
            "dsp_capture_enabled": manifest.get("dsp_capture_enabled"),
            "dsp_capture_mode": manifest.get("dsp_capture_mode"),
            "dsp_event_capture_enabled": manifest.get("dsp_event_capture_enabled"),
            "dsp_checkpoint_policy": manifest.get("dsp_checkpoint_policy"),
            "checkpoint_manifest_exists": checkpoint_path.is_file(),
            "checkpoint_manifest_complete": (
                checkpoint_manifest.get("complete")
                if checkpoint_path.is_file() and isinstance(checkpoint_manifest, dict)
                else None),
            "checkpoint_architectural_validation_eligible": (
                checkpoint_manifest.get("architectural_validation_eligible")
                if checkpoint_path.is_file() and isinstance(checkpoint_manifest, dict)
                else None),
        },
        "faults": list(faults),
        "fault_count": fault_count,
        "artifacts": {name: (run / name).exists() for name in
                       ("main-link.bin", "main-stderr.log", "main.log",
                        "screen.ppm", "frames/manifest.json",
                        "dsp-checkpoints/manifest.json", "dsp-events.jsonl",
                        "dsp-render.wav", "dsp-audio.wav")},
        "interpretation": {
            "track_list_observed": bool(link["wav_entries"]),
            "native_load_response_observed": bool(link["load_responses"]),
            "native_counter_playback_proven": native_counter_playback,
            "audio_playback_proven": False,
        },
    }


def _text(report: dict) -> str:
    media = report["media"]
    link = report["link"]
    result = report["result"]
    lines = [f"run: {report['run']}",
             f"profile: {report.get('profile') or 'unknown'}",
             f"link: {link['status']}, {link['records']} records",
             f"media entries: {media['entry_count']} ({len(media['entries_seen'])} retained; "
             f"{', '.join(media['wav_entries']) or 'none'})",
             f"NO CARD entries: {media['no_card_entries']}; load responses: {media['load_responses']}",
             f"native counter playback proven: "
             f"{report['interpretation']['native_counter_playback_proven']}",
             f"audio playback proven: {report['interpretation']['audio_playback_proven']}"]
    capture = report["dsp_capture"]
    lines.append("DSP capture: " + json.dumps(capture, sort_keys=True))
    if result:
        lines.append("result: " + json.dumps(result, sort_keys=True))
    if report["faults"]:
        lines.append(f"diagnostic lines: {report['fault_count']} (showing up to {len(report['faults'])})")
        lines.extend("  " + item.get("line", item.get("error", ""))
                     for item in report["faults"][:8])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json",
                        help="emit the complete machine-readable report")
    args = parser.parse_args(argv)
    if not args.run.is_dir():
        parser.error(f"run directory does not exist: {args.run}")
    report = build_report(args.run)
    print(json.dumps(report, indent=2, sort_keys=True) if args.as_json
          else _text(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
