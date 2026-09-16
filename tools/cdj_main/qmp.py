# SPDX-License-Identifier: GPL-2.0-or-later
"""Small synchronous QMP client with deadlines and event/reply separation."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import socket
import time


class QmpError(RuntimeError):
    pass


class Qmp:
    def __init__(self, path: Path, timeout: float = 3):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError('QMP timeout must be finite and positive')
        self.timeout, self.sequence, self.pending = timeout, 0, b''
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.socket.settimeout(timeout)
            # Prefer the shorter spelling for macOS's sockaddr_un limit.
            names = (str(path.resolve()), os.path.relpath(path))
            self.socket.connect(min(names, key=lambda s: len(os.fsencode(s))))
            greeting = self.receive(time.monotonic() + timeout)
            if 'QMP' not in greeting:
                raise QmpError('endpoint did not send a QMP greeting')
            self.command('qmp_capabilities')
        except BaseException:
            self.socket.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.socket.close()

    def receive(self, deadline: float) -> dict:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('QMP response deadline expired')
            if b'\n' in self.pending:
                line, _, self.pending = self.pending.partition(b'\n')
                try:
                    reply = json.loads(line)
                except ValueError as error:
                    raise QmpError('invalid QMP JSON response') from error
                if not isinstance(reply, dict):
                    raise QmpError('invalid QMP response object')
                return reply
            if len(self.pending) > 1024 * 1024:
                raise QmpError('QMP response exceeds 1 MiB')
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError('QMP connection closed')
            self.pending += chunk

    def command(self, command: str, arguments: dict | None = None):
        self.sequence += 1
        request = dict(execute=command, id=self.sequence)
        if arguments is not None:
            request['arguments'] = arguments
        deadline = time.monotonic() + self.timeout
        self.socket.settimeout(self.timeout)
        self.socket.sendall(json.dumps(request).encode() + b'\n')
        while True:
            reply = self.receive(deadline)
            if 'event' in reply:
                continue
            if reply.get('id') != self.sequence:
                raise QmpError('QMP response ID does not match request')
            if 'error' in reply:
                raise QmpError(f'{command}: {reply["error"]}')
            if 'return' not in reply:
                raise QmpError('QMP reply has no result')
            return reply['return']
