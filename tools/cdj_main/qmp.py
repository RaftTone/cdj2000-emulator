# SPDX-License-Identifier: GPL-2.0-or-later
"""Small synchronous QMP client with deadlines and event/reply separation."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import socket
import time

QmpEndpoint = Path | tuple[str, int] | str


class QmpError(RuntimeError):
    pass


def is_tcp_endpoint(endpoint: object) -> bool:
    """True for (host, port) or a host:port string with no path separators."""
    if isinstance(endpoint, tuple) and len(endpoint) == 2:
        host, port = endpoint
        return isinstance(host, str) and isinstance(port, int)
    if not isinstance(endpoint, str):
        return False
    if '/' in endpoint or '\\' in endpoint:
        return False
    host, separator, port = endpoint.rpartition(':')
    return bool(separator) and bool(host) and port.isdigit()


def parse_endpoint(endpoint: QmpEndpoint, *, relative_to: Path | None = None) -> QmpEndpoint:
    """Return a Path for a Unix socket or (host, port) for TCP."""
    if isinstance(endpoint, tuple):
        return endpoint
    if is_tcp_endpoint(endpoint):
        host, _, port = str(endpoint).rpartition(':')
        return host, int(port)
    path = Path(endpoint)
    if relative_to is not None and not path.is_absolute():
        path = relative_to / path
    return path


def connect_chardev(endpoint: QmpEndpoint, timeout: float = 3) -> socket.socket:
    """Connect to a QEMU unix or TCP chardev. Caller owns the socket."""
    parsed = parse_endpoint(endpoint)
    if isinstance(parsed, tuple):
        sock = socket.create_connection(parsed, timeout=timeout)
        sock.settimeout(timeout)
        return sock
    family = getattr(socket, 'AF_UNIX', socket.AF_INET)
    sock = socket.socket(family, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    names = (str(parsed.resolve()), os.path.relpath(parsed))
    sock.connect(min(names, key=lambda s: len(os.fsencode(s))))
    return sock


class Qmp:
    def __init__(self, path: QmpEndpoint, timeout: float = 3):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError('QMP timeout must be finite and positive')
        self.timeout, self.sequence, self.pending = timeout, 0, b''
        self.socket = None
        endpoint = parse_endpoint(path)
        try:
            if isinstance(endpoint, tuple):
                self.socket = socket.create_connection(endpoint, timeout=timeout)
            else:
                family = getattr(socket, 'AF_UNIX', socket.AF_INET)
                self.socket = socket.socket(family, socket.SOCK_STREAM)
                self.socket.settimeout(timeout)
                # Prefer the shorter spelling for macOS's sockaddr_un limit.
                names = (str(endpoint.resolve()), os.path.relpath(endpoint))
                self.socket.connect(min(names, key=lambda s: len(os.fsencode(s))))
            self.socket.settimeout(timeout)
            greeting = self.receive(time.monotonic() + timeout)
            if 'QMP' not in greeting:
                raise QmpError('endpoint did not send a QMP greeting')
            self.command('qmp_capabilities')
        except BaseException:
            if self.socket is not None:
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
