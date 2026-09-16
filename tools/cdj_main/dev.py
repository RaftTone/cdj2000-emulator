"""Small agent-facing control CLI for an active NXS run.

Examples::

    python -m tools.cdj_main.dev runs/demo status --json
    python -m tools.cdj_main.dev runs/demo press sd
    python -m tools.cdj_main.dev runs/demo rotary 2
    python -m tools.cdj_main.dev runs/demo qmp status
    python -m tools.cdj_main.dev runs/demo memory 0x1000 --length 64
    python -m tools.cdj_main.dev runs/demo stop
    python -m tools.cdj_main.dev runs/demo screenshot /tmp/demo.png
    python -m tools.cdj_main.dev runs/demo wait-browser TESTTONE.WAV --timeout 30
    python -m tools.cdj_main.dev runs/demo wait-duration --timeout 120
    python -m tools.cdj_main.dev runs/demo checkpoint --timeout 30

The command intentionally only exposes reversible, bounded operations.  It
resolves live endpoints from the run manifest/session and records every
attempt in ``actions.jsonl`` so an agent can explain what it did later.
"""

from __future__ import annotations

import argparse
import fcntl
from io import BytesIO
import json
import math
from pathlib import Path
import shutil
import sys
import time
from typing import Any

from PIL import Image

from tools.cdj_main import nxs_panel
from tools.cdj_main import panel_control
from tools.cdj_main.qmp import Qmp, QmpError
from tools.cdj_main.nxs_vm import checkpoint_metadata, read_frame
from tools.cdj_main.run_state import LinkObserver, observe, record_action, write_json


RUN_STATES = {"starting", "running", "stopping", "stopped", "failed"}


def _reply_ok(reply: str) -> bool:
    """Accept only the panel protocol's explicit success response."""
    if not isinstance(reply, str):
        return False
    tokens = reply.strip().split(None, 1)
    return bool(tokens) and tokens[0].casefold() == "ok"


def _json(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True, default=str))


def _manifest(run: Path) -> dict:
    try:
        value = json.loads((run / "run.json").read_text())
    except FileNotFoundError as error:
        raise ValueError(f"run manifest is missing: {run / 'run.json'}") from error
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read run manifest: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("run manifest must be a JSON object")
    return value


def _session(run: Path) -> dict:
    try:
        value = json.loads((run / "session.json").read_text())
    except FileNotFoundError:
        return {}
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read run session: {error}") from error
    return value if isinstance(value, dict) else {}


def _run(path: str | Path) -> Path:
    run = Path(path).resolve()
    if not run.is_dir():
        raise ValueError(f"run directory does not exist: {run}")
    return run


def _state(run: Path) -> str:
    return str(_session(run).get("state", "unknown"))


def _endpoint(run: Path, name: str) -> tuple[str, int] | Path:
    manifest = _manifest(run)
    endpoints = manifest.get("endpoints")
    if not isinstance(endpoints, dict):
        raise ValueError("run manifest has no endpoint metadata")
    if name == "panel":
        host = endpoints.get("panel_host", "127.0.0.1")
        port = endpoints.get("panel_port")
        if not isinstance(host, str) or not isinstance(port, int):
            raise ValueError("run manifest has no panel endpoint")
        return host, port
    if name == "gdb":
        host = endpoints.get("gdb_host", "127.0.0.1")
        port = endpoints.get("gdb_port")
        if not isinstance(host, str) or not isinstance(port, int):
            raise ValueError("run manifest has no GDB endpoint; start with --debug")
        return host, port
    if name == "qmp":
        qmp = endpoints.get("qmp")
        if not isinstance(qmp, str):
            raise ValueError("run manifest has no QMP endpoint; start with --debug")
        path = Path(qmp)
        return path if path.is_absolute() else run / path
    raise ValueError(f"unknown endpoint: {name}")


def _require_running(run: Path) -> None:
    state = _state(run)
    if state in {"stopped", "failed"}:
        raise ValueError(f"run is {state}; mutations are refused")
    if state not in {"starting", "running"}:
        raise ValueError(f"run state is {state!r}; mutations are refused")


def _action(run: Path, command: str, **fields: Any) -> None:
    record_action(run, dict(command=command, **fields))


def _panel(run: Path) -> panel_control.PanelControl:
    host, port = _endpoint(run, "panel")
    connection = panel_control.PanelControl(host, port, timeout=2.0)
    try:
        connection.open()
    except OSError:
        connection.close()
        raise ValueError(f"panel channel is unavailable at {host}:{port}")
    return connection


def status(run: Path, as_json: bool) -> int:
    try:
        snapshot = observe(run)
    except Exception as error:
        _action(run, "status", outcome="error", error=str(error))
        raise
    _action(run, "status", outcome="observed", json=as_json,
            records=snapshot.get("link", {}).get("records", 0))
    if as_json:
        _json(snapshot)
    else:
        print(snapshot.get("progress", "No status available."))
        frame = snapshot.get("frame", {})
        if frame:
            print("frame: " + str(frame.get("status", "unknown")))
        if snapshot.get("recent_fault_lines"):
            print(f"faults: {len(snapshot['recent_fault_lines'])} recent log lines")
        for step in snapshot.get("next_steps", []):
            print(f"next: {step}")
    return 0


def press(run: Path, button: str, hold_ms: int | None) -> int:
    _require_running(run)
    if button.strip().lower() in {"rev", "reverse"}:
        raise ValueError("REV is an active-low switch; use 'switch rev on' or 'switch rev off'")
    # Agent controls default to a human-like short pulse.  The long, measured
    # plan hold remains available explicitly for sampling-sensitive tests.
    duration = 100 if hold_ms is None else hold_ms
    if not 0 <= duration <= 60000:
        raise ValueError("hold-ms must be 0..60000")
    byte, mask = nxs_panel.button_mask(button)
    wire = panel_control.encode_press(byte, mask, duration)
    _action(run, "press", button=button, byte=byte, mask=mask,
            hold_ms=duration, wire=wire.strip(), outcome="attempted")
    connection = _panel(run)
    try:
        reply = connection.send(wire)
    except Exception as error:
        _action(run, "press", button=button, outcome="error", error=str(error))
        raise
    finally:
        connection.close()
    ok = _reply_ok(reply)
    _action(run, "press", button=button, outcome="replied", reply=reply, ok=ok)
    _json({"ok": ok, "button": button,
           "hold_ms": duration, "reply": reply})
    return 0 if ok else 1


def switch(run: Path, name: str, active: bool) -> int:
    """Set a physical switch by its logical state, rather than pulsing a key."""
    _require_running(run)
    if name.lower() not in {"rev", "reverse"}:
        raise ValueError("supported switch: rev")
    byte, mask = nxs_panel.button_mask("rev")
    high = nxs_panel.contact_level(byte, mask, active)
    wire = panel_control.encode_level(byte, mask, high)
    _action(run, "switch", switch="rev", active=active, high=high,
            wire=wire.strip(), outcome="attempted")
    connection = _panel(run)
    try:
        reply = connection.send(wire)
    finally:
        connection.close()
    ok = _reply_ok(reply)
    _action(run, "switch", switch="rev", active=active, high=high,
            reply=reply, ok=ok, outcome="replied")
    _json(dict(ok=ok, switch="rev", active=active, high=high, reply=reply))
    return 0 if ok else 1


def rotary(run: Path, delta: int) -> int:
    _require_running(run)
    field = panel_control.ENCODER_FIELD
    wire = panel_control.encode_rotary(field, delta)
    _action(run, "rotary", field=field, delta=delta, wire=wire.strip(), outcome="attempted")
    connection = _panel(run)
    try:
        reply = connection.send(wire)
    except Exception as error:
        _action(run, "rotary", field=field, delta=delta, outcome="error", error=str(error))
        raise
    finally:
        connection.close()
    ok = _reply_ok(reply)
    _action(run, "rotary", field=field, delta=delta, outcome="replied", reply=reply, ok=ok)
    _json({"ok": ok, "field": field,
           "delta": delta, "reply": reply})
    return 0 if ok else 1


def qmp(run: Path, operation: str) -> int:
    # QMP is a live endpoint.  Once the launcher has closed a session, a stale
    # socket path can point at a later run, so never reconnect based only on
    # the manifest.  File-backed status and screenshots remain usable after a
    # run stops.
    _require_running(run)
    endpoint = _endpoint(run, "qmp")
    command = ("stop" if operation == "pause" else
               "cont" if operation == "resume" else
               "human-monitor-command" if operation == "registers" else
               "query-status")
    try:
        with Qmp(endpoint) as client:
            result = client.command(command, {"command-line": "info registers"}
                                    if operation == "registers" else None)
            if operation == "status":
                # QMP status says whether the virtual CPU is running.  HMP's
                # read-only register dump gives the agent the PC and general
                # registers without opening a second debugger connection.
                registers = client.command(
                    "human-monitor-command", {"command-line": "info registers"})
                result = {"status": result, "registers": registers}
    except (OSError, TimeoutError, ConnectionError, QmpError) as error:
        _action(run, "qmp", operation=operation, outcome="error", error=str(error))
        raise ValueError(f"QMP {operation} failed: {error}") from error
    _action(run, "qmp", operation=operation, qmp_command=command,
            outcome="replied", result=result)
    _json({"ok": True, "operation": operation, "result": result})
    return 0


def _physical_address(value: str | int) -> int:
    try:
        address = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as error:
        raise ValueError("address must be a numeric 32-bit physical address") from error
    if not 0 <= address <= 0xFFFFFFFF:
        raise ValueError("address must be a numeric 32-bit physical address")
    return address


def memory(run: Path, address: str | int, length: int = 64) -> int:
    """Read physical memory through QEMU's read-only HMP ``xp`` command.

    HMP returns formatted text rather than a stable binary schema.  The raw
    response is therefore preserved in JSON for an agent to parse or retain
    alongside the command that produced it.  Reads while the guest is
    running are inherently non-atomic.
    """
    _require_running(run)
    physical = _physical_address(address)
    if not isinstance(length, int) or isinstance(length, bool) or not 1 <= length <= 4096:
        raise ValueError("length must be 1..4096 bytes")
    command_line = f"xp /{length}bx 0x{physical:x}"
    endpoint = _endpoint(run, "qmp")
    try:
        with Qmp(endpoint) as client:
            raw = client.command("human-monitor-command", {"command-line": command_line})
    except (OSError, TimeoutError, ConnectionError, QmpError) as error:
        _action(run, "memory", address=physical, length=length,
                command_line=command_line, outcome="error", error=str(error))
        raise ValueError(f"QMP memory read failed: {error}") from error
    _action(run, "memory", address=physical, length=length,
            command_line=command_line, outcome="replied", raw=raw)
    _json({"ok": True, "address": physical, "length": length,
           "command": command_line, "raw": raw})
    return 0


def stop(run: Path) -> int:
    """Request graceful launcher cleanup without pretending it is complete."""
    _require_running(run)
    request = run / "stop-request.json"
    write_json(request, {"requested": True, "requested_unix": time.time()})
    _action(run, "stop", outcome="requested", requested=True,
            request_file=str(request))
    _json({"ok": True, "requested": True, "file": str(request)})
    return 0


def screenshot(run: Path, output: Path) -> int:
    raw_path = run / "screen.ppm"
    try:
        raw, metadata = read_frame(raw_path)
        if raw is None:
            raise ValueError(metadata.get("status", "invalid frame"))
        if metadata.get("width") != 480 or metadata.get("height") not in (234, 255):
            raise ValueError("expected a complete 480x234 or 480x255 framebuffer")
        output = output.resolve()
        with Image.open(BytesIO(raw)) as image:
            frame = image.convert("RGB")
            if frame.height == 255:
                frame = frame.crop((0, 0, 480, 234))
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("xb") as stream:
                frame.save(stream, format="PNG")
    except (OSError, ValueError) as error:
        _action(run, "screenshot", output=str(output), outcome="error", error=str(error))
        raise ValueError(f"cannot capture a complete screen frame: {error}") from error
    _action(run, "screenshot", output=str(output), outcome="saved",
            width=frame.width, height=frame.height,
            source_mtime_ns=metadata['source_mtime_ns'])
    _json({"ok": True, "file": str(output), "width": frame.width,
           "height": frame.height,
           "source_mtime_ns": metadata['source_mtime_ns'],
           "source_age_seconds": max(0, time.time() - metadata['source_mtime_ns'] / 1e9)})
    return 0


def _checkpoint_request_path(run: Path) -> Path:
    manifest = _manifest(run)
    environment = manifest.get("main_environment")
    request = environment.get("CDJ_NXS_DSP_CHECKPOINT_REQUEST") \
        if isinstance(environment, dict) else None
    if not isinstance(request, str) or not request:
        raise ValueError("run manifest has no DSP checkpoint request endpoint")
    path = Path(request)
    if not path.is_absolute():
        raise ValueError("DSP checkpoint request endpoint must be absolute")
    if path != run.resolve() / "dsp-checkpoint-request.json":
        raise ValueError("DSP checkpoint request endpoint does not belong to this run")
    return path


def _checkpoint_modes(manifest: dict) -> dict[str, str]:
    environment = manifest.get("main_environment")
    environment = environment if isinstance(environment, dict) else {}
    return {
        "dsp_timing_mode": (
            "functional-runahead"
            if environment.get("CDJ_NXS_DSP_FUNCTIONAL_TIMING") == "1"
            else "strict"),
        "dsp_audio_mode": (
            "coarse-packet-slots"
            if environment.get("CDJ_NXS_DSP_FUNCTIONAL_AUDIO") == "1"
            else "stopped-clock"),
        "dsp_scheduler_mode": str(
            environment.get("CDJ_NXS_DSP_SCHEDULER", "legacy")),
    }


def _copy_checkpoint_sidecar(run: Path, source: Path, metadata: dict) -> tuple[Path, Path]:
    """Copy a validated live checkpoint into immutable per-request provenance."""
    basename = source.name
    sidecar = run / "debug-checkpoints" / basename
    sidecar.mkdir(parents=True, exist_ok=False)
    target = sidecar / basename
    try:
        with source.open("rb") as input_stream, target.open("xb") as output_stream:
            shutil.copyfileobj(input_stream, output_stream, length=1024 * 1024)
            output_stream.flush()
        manifest = _manifest(run)
        capture = {
            "schema": 1,
            "origin": "diagnostic_connected_checkpoint",
            "complete": False,
            "architectural_validation_eligible": False,
            "dsp_capture_enabled": True,
            "dsp_capture_mode": "fault-only",
            "dsp_event_capture_enabled": False,
            "dsp_checkpoint_policy": "fault",
            "checkpoint_capture_complete": True,
            **_checkpoint_modes(manifest),
            "checkpoints": [{
                "file": basename,
                "sha256": metadata["sha256"],
                "size": metadata["size"],
            }],
            "checkpoint_metadata": metadata,
            "captured_run": str(run),
            "captured_input_artifacts": manifest.get("input_artifacts", {}),
            "capture_source_note": (
                "run.json identifies the captured executable and input hashes; "
                "it does not identify the currently edited source tree."),
        }
        manifest_path = sidecar / "manifest.json"
        write_json(manifest_path, capture)
    except Exception:
        # A failed copy must not leave a path that could be mistaken for a
        # ready diagnostic snapshot.  The request itself remains board-owned.
        try:
            shutil.rmtree(sidecar)
        except OSError:
            pass
        raise
    return target, manifest_path


def checkpoint(run: Path, timeout: float = 30, poll: float = 0.25) -> int:
    """Request and collect one DSP checkpoint without touching live manifests."""
    # Hold a process lock through collection: the board removes the request
    # before the client copies its snapshot, so request existence alone cannot
    # keep a second client from replacing the completion. Keep the lock inode
    # after release; unlinking it would permit competing locks on two inodes.
    with (run / "dsp-checkpoint-client.lock").open("a") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("another DSP checkpoint client is collecting a snapshot") from error
        try:
            return _checkpoint_locked(run, timeout, poll)
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def _checkpoint_locked(run: Path, timeout: float, poll: float) -> int:
    _require_running(run)
    if not math.isfinite(timeout) or timeout <= 0 or timeout > 300:
        raise ValueError("timeout must be >0 and <=300 seconds")
    if not math.isfinite(poll) or poll <= 0 or poll > 60:
        raise ValueError("poll must be >0 and <=60 seconds")
    request = _checkpoint_request_path(run)
    done = Path(str(request) + ".done")
    if request.exists():
        raise ValueError(f"DSP checkpoint request is already pending: {request}")
    try:
        done.unlink()
    except FileNotFoundError:
        pass
    except OSError as error:
        raise ValueError(f"cannot clear previous checkpoint completion: {error}") from error

    payload = {"requested": True, "requested_unix": time.time(), "reason": "debug request"}
    try:
        with request.open("x", encoding="utf-8") as stream:
            json.dump(payload, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
    except FileExistsError as error:
        raise ValueError(f"DSP checkpoint request is already pending: {request}") from error
    _action(run, "checkpoint", outcome="requested", request_file=str(request),
            done_file=str(done), timeout=timeout)
    deadline = time.monotonic() + timeout
    while True:
        state = _state(run)
        if state in {"stopped", "failed"}:
            raise ValueError(
                f"run is {state} while DSP checkpoint request remains pending; "
                f"inspect {request} and {done}")
        if done.is_file():
            try:
                response = json.loads(done.read_text())
            except (OSError, json.JSONDecodeError) as error:
                raise ValueError(f"invalid DSP checkpoint completion: {error}") from error
            if not isinstance(response, dict):
                raise ValueError("DSP checkpoint completion must be a JSON object")
            if response.get("ok") is not True:
                error = response.get("error", "board rejected checkpoint request")
                _action(run, "checkpoint", outcome="error", error=str(error),
                        response=response)
                _json({"ok": False, "requested": True, "response": response,
                       "request_file": str(request), "done_file": str(done)})
                return 2
            filename = response.get("file")
            if not isinstance(filename, str) or not filename or Path(filename).name != filename:
                raise ValueError("DSP checkpoint completion has an invalid file name")
            source = run / "dsp-checkpoints" / filename
            try:
                metadata = checkpoint_metadata(source)
                target, manifest_path = _copy_checkpoint_sidecar(run, source, metadata)
            except (OSError, RuntimeError, ValueError, KeyError) as error:
                raise ValueError(f"DSP checkpoint failed validation: {error}") from error
            result = {
                "ok": True,
                "requested": True,
                "ready": True,
                "snapshot": str(target),
                "file": str(target),
                "manifest": str(manifest_path),
                "provenance": "diagnostic_connected_checkpoint",
                "checkpoint": metadata,
                "meaning": "validated debug snapshot; architectural validation and connected event replay remain disabled",
            }
            _action(run, "checkpoint", outcome="ready", file=str(target),
                    manifest=str(manifest_path), sha256=metadata["sha256"])
            _json(result)
            return 0
        if time.monotonic() >= deadline:
            result = {"ok": False, "requested": True, "ready": False,
                      "pending": request.exists(), "request_file": str(request),
                      "done_file": str(done), "timeout": timeout}
            _action(run, "checkpoint", outcome="timeout", pending=request.exists(),
                    request_file=str(request), done_file=str(done), timeout=timeout)
            _json(result)
            return 1
        time.sleep(min(poll, max(0, deadline - time.monotonic())))


def _validate_after_record(after_record: int) -> None:
    if (isinstance(after_record, bool) or not isinstance(after_record, int) or
            after_record < 0):
        raise ValueError("after-record must be an integer >=0")


def wait_browser(run: Path, text: str, timeout: float, poll: float,
                 after_record: int = 0) -> int:
    if not text.strip():
        raise ValueError("browser text must not be empty")
    _validate_after_record(after_record)
    if not math.isfinite(timeout) or timeout <= 0 or timeout > 3600:
        raise ValueError("timeout must be >0 and <=3600 seconds")
    if not math.isfinite(poll) or poll <= 0 or poll > 60:
        raise ValueError("poll must be >0 and <=60 seconds")
    observer = LinkObserver()
    deadline = time.monotonic() + timeout
    needle = text.casefold()
    while True:
        state = _state(run)
        if state in {"stopped", "failed"}:
            raise ValueError(f"run is {state} before browser text {text!r} was observed")
        snapshot = observer.poll(run / "main-link.bin")
        browser = snapshot.get("browser") or {}
        rows = [str(row.get("text", "")) for row in browser.get("rows", [])]
        if (snapshot.get("caught_up") and browser.get("complete") and
                browser.get("record", 0) > after_record and
                any(needle in row.casefold() for row in rows)):
            result = {"ok": True, "text": text, "rows": rows,
                      "records": snapshot.get("records", 0),
                      "matched_record": browser.get("record"),
                      "after_record": after_record}
            _action(run, "wait-browser", text=text, outcome="matched", rows=rows,
                    matched_record=browser.get("record"), after_record=after_record)
            _json(result)
            return 0
        if time.monotonic() >= deadline:
            result = {"ok": False, "text": text, "rows": rows,
                      "records": snapshot.get("records", 0), "timeout": timeout,
                      "after_record": after_record}
            _action(run, "wait-browser", text=text, outcome="timeout", rows=rows,
                    after_record=after_record)
            _json(result)
            return 1
        time.sleep(min(poll, max(0, deadline - time.monotonic())))


def _duration_observation(snapshot: dict) -> tuple[dict, list[int], int] | None:
    """Return a complete, nonzero command-5 duration observation.

    Command-5 words 3 and 4 are the NXS track duration field.  As with the
    other NXS 32-bit link fields, the high halfword comes first (the local
    decoder uses ``(words[3] << 16) | words[4]``).  Keep the source words as
    evidence and expose the decoded integer without assigning units that the
    firmware protocol does not document here.
    """
    if not snapshot.get("caught_up") or snapshot.get("pending_bytes", 0):
        return None
    payload = snapshot.get("duration_payload")
    if not isinstance(payload, dict) or payload.get("command") != 5:
        return None
    raw_words = payload.get("words")
    if not isinstance(raw_words, list) or len(raw_words) < 5:
        return None
    try:
        words = [int(word) for word in raw_words]
    except (TypeError, ValueError):
        return None
    if any(not 0 <= word <= 0xFFFF for word in words):
        return None
    duration_words = words[3:5]
    if not any(duration_words):
        return None
    duration_field = (duration_words[0] << 16) | duration_words[1]
    return payload, duration_words, duration_field


def wait_duration(run: Path, timeout: float, poll: float,
                  after_record: int = 0) -> int:
    """Wait for a complete command-5 track duration reply from the link."""
    if not math.isfinite(timeout) or timeout <= 0 or timeout > 3600:
        raise ValueError("timeout must be >0 and <=3600 seconds")
    if not math.isfinite(poll) or poll <= 0 or poll > 60:
        raise ValueError("poll must be >0 and <=60 seconds")
    _validate_after_record(after_record)
    observer = LinkObserver()
    deadline = time.monotonic() + timeout
    snapshot = {"records": 0, "duration_payload": None, "caught_up": False}
    while True:
        state = _state(run)
        if state in {"stopped", "failed"}:
            raise ValueError(f"run is {state} before a complete track duration was observed")
        snapshot = observer.poll(run / "main-link.bin")
        observation = _duration_observation(snapshot)
        if (observation is not None and
                int(observation[0].get("record", 0)) > after_record):
            payload, duration_words, duration_field = observation
            result = {
                "ok": True,
                "observed": True,
                "duration_payload": payload,
                "duration_words": duration_words,
                "duration_field": duration_field,
                "records": snapshot.get("records", 0),
                "matched_record": payload.get("record"),
                "after_record": after_record,
                "playback_proven": False,
                "meaning": "complete command-5 duration observed; this does not prove playback",
            }
            _action(run, "wait-duration", outcome="matched",
                    duration_words=duration_words, duration_field=duration_field,
                    records=snapshot.get("records", 0),
                    matched_record=payload.get("record"), after_record=after_record)
            _json(result)
            return 0
        if time.monotonic() >= deadline:
            result = {
                "ok": False,
                "observed": False,
                "records": snapshot.get("records", 0),
                "duration_payload": snapshot.get("duration_payload"),
                "caught_up": snapshot.get("caught_up", False),
                "pending_bytes": snapshot.get("pending_bytes", 0),
                "timeout": timeout,
                "after_record": after_record,
            }
            _action(run, "wait-duration", outcome="timeout",
                    records=snapshot.get("records", 0),
                    caught_up=snapshot.get("caught_up", False),
                    pending_bytes=snapshot.get("pending_bytes", 0),
                    after_record=after_record)
            _json(result)
            return 1
        time.sleep(min(poll, max(0, deadline - time.monotonic())))


def wait_playback(run: Path, timeout: float = 120, poll: float = 0.25,
                  min_frames: int = 150) -> int:
    """Observe fresh counter decreases after catching up with the live link.

    Require two decreases plus a minimum total advance at one duration.
    Repeated records, historic playback and a track duration alone cannot pass.
    A seek or track change resets the observation sequence. This does not test
    audible output or distinguish automatic playback from repeated jog input.
    """
    if not math.isfinite(timeout) or not 0 < timeout <= 3600:
        raise ValueError("timeout must be >0 and <=3600 seconds")
    if not math.isfinite(poll) or not 0 < poll <= 60:
        raise ValueError("poll must be >0 and <=60 seconds")
    if isinstance(min_frames, bool) or not isinstance(min_frames, int) or min_frames <= 0:
        raise ValueError("min-frames must be a positive integer")
    _require_running(run)
    observer = LinkObserver()
    deadline = time.monotonic() + timeout
    previous = None
    previous_record = None
    first = None
    decreases = 0
    samples = 0
    while True:
        _require_running(run)
        snapshot = observer.poll(run / "main-link.bin")
        row = snapshot.get("transport") if snapshot.get("caught_up") else None
        record = row.get("record") if row else None
        if isinstance(record, int) and (previous_record is None or record > previous_record):
            previous_record = record
            samples += 1
            if (previous is None or
                    row["duration_frames"] != previous["duration_frames"] or
                    row["remaining_frames"] > previous["remaining_frames"]):
                first = row
                decreases = 0
            elif row["remaining_frames"] < previous["remaining_frames"]:
                decreases += 1
            previous = row
            advanced = first['remaining_frames'] - row['remaining_frames']
            if decreases >= 2 and advanced >= min_frames:
                result = dict(ok=True, counter_advanced=True, fresh_samples=samples,
                              decreases=decreases, first=first, latest=row,
                              advanced_frames=advanced, min_frames=min_frames,
                              audio_verified=False,
                              meaning="fresh native counter decreases observed; audio output was not tested")
                _action(run, "wait-playback", outcome="advanced", **result)
                _json(result)
                return 0
        elif snapshot.get("caught_up") and row is None:
            previous = first = None
            decreases = 0
        if time.monotonic() >= deadline:
            result = dict(ok=False, counter_advanced=False, timeout=timeout,
                          fresh_samples=samples, decreases=decreases, first=first,
                          min_frames=min_frames,
                          advanced_frames=(first['remaining_frames'] - previous['remaining_frames']
                                           if first and previous else 0),
                          latest=previous, audio_verified=False,
                          meaning="insufficient fresh counter movement before timeout")
            _action(run, "wait-playback", outcome="timeout", **result)
            _json(result)
            return 1
        time.sleep(min(poll, max(0, deadline - time.monotonic())))


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    state = sub.add_parser("status")
    state.add_argument("--json", action="store_true", dest="as_json")
    button = sub.add_parser("press")
    button.add_argument("button")
    button.add_argument("--hold-ms", type=int, default=None,
                        help="guest virtual milliseconds; default 100 (host response time is separate)")
    switch_parser = sub.add_parser("switch", help="set a physical switch by its logical state")
    switch_parser.add_argument("name", choices=("rev", "reverse"))
    switch_parser.add_argument("state", choices=("on", "off"))
    knob = sub.add_parser("rotary")
    knob.add_argument("delta", type=int)
    qmp_parser = sub.add_parser("qmp")
    qmp_parser.add_argument("operation", choices=("pause", "resume", "status", "registers"))
    memory_parser = sub.add_parser("memory")
    memory_parser.add_argument("address")
    memory_parser.add_argument("--length", type=int, default=64)
    sub.add_parser("stop")
    diagnostic = sub.add_parser("diagnose", help="capture bounded status, log tails and framebuffer for handoff")
    diagnostic.add_argument("output", type=Path, help="new diagnostic directory (must not exist)")
    media = sub.add_parser("wait-media", help="wait for NXS card presence, readiness and mount latch (--debug required)")
    media.add_argument("--timeout", type=float, default=120, help="wall-clock seconds, 0 for one sample")
    media.add_argument("--poll", type=float, default=1, help="wall-clock seconds between samples")
    shot = sub.add_parser("screenshot")
    shot.add_argument("output", type=Path)
    waiting = sub.add_parser("wait-browser")
    waiting.add_argument("text")
    waiting.add_argument("--timeout", type=float, default=30)
    waiting.add_argument("--poll", type=float, default=0.25)
    waiting.add_argument("--after-record", type=int, default=0,
                         help="require a matching reply with a greater record number")
    duration = sub.add_parser("wait-duration")
    duration.add_argument("--timeout", type=float, default=120)
    duration.add_argument("--poll", type=float, default=0.25)
    duration.add_argument("--after-record", type=int, default=0,
                          help="require a duration reply with a greater record number")
    playback = sub.add_parser("wait-playback", help="wait for fresh native counter movement")
    playback.add_argument("--timeout", type=float, default=120)
    playback.add_argument("--poll", type=float, default=0.25)
    playback.add_argument("--min-frames", type=int, default=150,
                          help="minimum counter advance in 1/150-second frames (default: 150)")
    checkpoint_parser = sub.add_parser(
        "checkpoint",
        help="request a validated diagnostic DSP snapshot at a safe boundary",
    )
    checkpoint_parser.add_argument("--timeout", type=float, default=30,
                                   help="seconds to wait for the board (default: 30, max: 300)")
    checkpoint_parser.add_argument("--poll", type=float, default=0.25,
                                   help="seconds between completion checks (default: 0.25)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        run = _run(args.run)
        if args.command == "status":
            return status(run, args.as_json)
        if args.command == "diagnose":
            from tools.cdj_main.diagnostics import capture
            result = capture(run, args.output)
            _action(run, "diagnose", outcome="captured", **result)
            _json(result)
            return 0
        if args.command == "wait-media":
            from tools.cdj_main.media_readiness import observe_run
            if _state(run) not in {"starting", "running"}:
                raise ValueError("wait-media needs an active run; use diagnose or run_report for saved evidence")
            result = observe_run(run, timeout=args.timeout, poll=args.poll)
            _action(run, "wait-media", outcome="matched" if result["ok"] else "timeout", **result)
            _json(result)
            return 0 if result["ok"] else 1
        if args.command == "press":
            return press(run, args.button, args.hold_ms)
        if args.command == "switch":
            return switch(run, args.name, args.state == "on")
        if args.command == "rotary":
            return rotary(run, args.delta)
        if args.command == "qmp":
            return qmp(run, args.operation)
        if args.command == "memory":
            return memory(run, args.address, args.length)
        if args.command == "stop":
            return stop(run)
        if args.command == "screenshot":
            return screenshot(run, args.output)
        if args.command == "wait-duration":
            return wait_duration(run, args.timeout, args.poll, args.after_record)
        if args.command == "wait-playback":
            return wait_playback(run, args.timeout, args.poll, args.min_frames)
        if args.command == "checkpoint":
            return checkpoint(run, args.timeout, args.poll)
        return wait_browser(run, args.text, args.timeout, args.poll, args.after_record)
    except (ValueError, OSError, ConnectionError, QmpError) as error:
        try:
            if "run" in locals():
                _action(run, args.command, outcome="error", error=str(error))
        except OSError:
            pass
        print(json.dumps({"ok": False, "error": str(error)}, sort_keys=True),
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
