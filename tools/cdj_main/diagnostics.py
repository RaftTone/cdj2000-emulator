"""Capture a bounded, portable diagnostic snapshot of an existing run."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import time

from tools.cdj_main.run_state import LOGS, log_tail, observe
from tools.cdj_main.nxs_vm import read_frame

METADATA = ('run.json', 'session.json', 'result.json')
MAX_METADATA = 1024 * 1024
TAIL_BYTES = 64 * 1024


def capture(run: Path, output: Path) -> dict:
    """Create a new directory, retaining partial evidence if a source is absent.

    No debugger or panel connection is opened. Files can change during capture;
    the inventory records source sizes and times, not an atomic guest snapshot.
    Raw firmware, media, memory dumps and unbounded logs are not copied.
    """
    run, output = run.resolve(), output.resolve()
    if not run.is_dir():
        raise ValueError(f'run directory does not exist: {run}')
    output.mkdir(parents=True, exist_ok=False)
    started = time.time()
    files, errors = {}, {}

    def save(name: str, data: bytes) -> None:
        (output / name).write_bytes(data)
        files[name] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())

    def source(name: str) -> None:
        try:
            stat = (run / name).stat()
            inventory[name] = dict(bytes=stat.st_size, mtime_ns=stat.st_mtime_ns)
        except OSError as error:
            errors[name] = str(error)

    inventory = {}
    for name in (*METADATA, *LOGS, 'actions.jsonl', 'main-link.bin',
                 'gui-link-tx.bin', 'screen.ppm'):
        source(name)
    for name in METADATA:
        try:
            with (run / name).open('rb') as stream:
                data = stream.read(MAX_METADATA + 1)
            if len(data) > MAX_METADATA:
                raise ValueError('metadata exceeds 1 MiB; see original run')
            save(name, data)
        except (OSError, ValueError) as error:
            errors[name] = str(error)
    for name in (*LOGS, 'actions.jsonl'):
        if name in inventory:
            save(name + '.tail', log_tail(run / name, TAIL_BYTES).encode())
    try:
        save('status.json', (json.dumps(observe(run), indent=2) + '\n').encode())
    except Exception as error:
        errors['status.json'] = str(error)
    try:
        raw, frame = read_frame(run / 'screen.ppm')
        if raw is None:
            errors['screen.ppm'] = str(frame)
        else:
            save('screen.ppm', raw)
    except (OSError, ValueError) as error:
        errors['screen.ppm'] = str(error)
    manifest = dict(schema=1, run=str(run), output=str(output),
                    started_unix=started, finished_unix=time.time(),
                    scope='non-atomic bounded diagnostic capture; original run retains full evidence',
                    log_tail_bytes=TAIL_BYTES, source_inventory=inventory,
                    files=files, errors=errors)
    (output / 'capture.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return dict(ok=True, complete=not errors, output=str(output), errors=errors,
                files=list(files), manifest=str(output / 'capture.json'))
