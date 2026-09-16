"""Breakpoints and watchpoints on MAIN, over QEMU's gdbstub.

    python -m tools.cdj_main.gdbprobe --sd runs/wav-test/test-track.img \
        --watch 0x04cf2994 --break 0x04238b64 --seconds 120

There is no gdb on this host and `xp /Ni` is a linear disassembler, so the two
questions this investigation keeps hitting - "who writes this word" and "does
this code ever run" - had no answer.  QEMU's own gdbstub answers both, and the
remote protocol needed for it is small enough to speak directly.

Each stop is reported with the elapsed time, the stop kind, PC and PR.  NOTE:
at a watchpoint stop the registers are the state AFTER the access, so PC is the
instruction after the store and r4 is not the call's argument - read the watched
word instead of trusting a register.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QEMU = ROOT / "build/qemu/build/qemu-system-sh4"
FIRMWARE = ROOT / "firmware/nxs"

# QEMU's sh4 gdbstub order: r0-r15, pc, pr, gbr, vbr, mach, macl, sr, ...
REG_PC, REG_PR = 16, 17


class Rsp:
    """The few remote-protocol packets this needs, with acks left on."""

    def __init__(self, port: int, timeout: float = 10.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def send(self, body: str) -> None:
        csum = sum(body.encode()) & 0xFF
        self.sock.sendall(b"$" + body.encode() + b"#" + b"%02x" % csum)

    def packet(self, timeout: float | None = None) -> str:
        """Next '$...#xx', skipping acks.  Returns '' on timeout."""
        deadline = time.time() + (timeout if timeout is not None else 10.0)
        while True:
            start = self.buf.find(b"$")
            end = self.buf.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buf) >= end + 3:
                body = self.buf[start + 1:end].decode("latin1")
                self.buf = self.buf[end + 3:]
                self.sock.sendall(b"+")
                return body
            if time.time() > deadline:
                return ""
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(1 << 16)
            except socket.timeout:
                continue
            if not chunk:
                return ""
            self.buf += chunk

    def cmd(self, body: str, timeout: float = 10.0) -> str:
        self.send(body)
        return self.packet(timeout)

    def regs(self) -> list[int]:
        raw = self.cmd("g")
        return [int.from_bytes(bytes.fromhex(raw[i:i + 8]), "little")
                for i in range(0, len(raw) - 7, 8)]

    def read(self, addr: int, length: int) -> bytes:
        reply = self.cmd("m%x,%x" % (addr, length))
        return b"" if reply.startswith("E") else bytes.fromhex(reply)

    def word(self, addr: int) -> int | None:
        raw = self.read(addr, 4)
        return int.from_bytes(raw, "little") if len(raw) == 4 else None

    def point(self, kind: int, addr: int, length: int, on: bool = True) -> str:
        return self.cmd("%s%d,%x,%x" % ("Z" if on else "z", kind, addr, length))

    def resume(self) -> None:
        self.send("c")

    def interrupt(self) -> None:
        self.sock.sendall(b"\x03")


def start(sd: Path, port: int, gdbport: int, env: dict, log: Path,
          frozen: bool = False):
    """`frozen` adds -S, so the machine waits for the first `c`.

    Some flags are set within the first seconds of boot: attaching after them
    sees only the settled value, so catching the write needs the CPU held
    before its first instruction.
    """
    return subprocess.Popen(
        [str(QEMU), "-M", "cdj2000nxs-main"] + (["-S"] if frozen else []) + [
         "-bios", str(FIRMWARE / "main-firmware.bin"),
         "-display", "none", "-no-reboot", "-d", "unimp", "-D", str(log),
         "-serial", "null", "-serial", "null", "-serial", "null",
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
         "-gdb", "tcp::%d" % gdbport,
         "-drive", "if=sd,format=raw,snapshot=on,file=%s" % sd.resolve()],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main() -> int:
    import os

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sd", type=Path, required=True)
    parser.add_argument("--attach-at", type=float, default=40.0,
                        help="seconds of free-running boot before attaching; "
                             "0 holds the CPU with -S so nothing runs first")
    parser.add_argument("--seconds", type=float, default=120.0)
    parser.add_argument("--break", dest="breaks", action="append", default=[],
                        type=lambda s: int(s, 0))
    parser.add_argument("--watch", action="append", default=[],
                        type=lambda s: int(s, 0), help="write watchpoint, 4 bytes")
    parser.add_argument("--sample", action="append", default=[],
                        type=lambda s: int(s, 0),
                        help="word to read out at every stop")
    parser.add_argument("--max-stops", type=int, default=200)
    parser.add_argument("--port", type=int, default=55711)
    parser.add_argument("--gdbport", type=int, default=55712)
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    args = parser.parse_args()

    env = dict(os.environ)
    for item in args.env:
        name, _, value = item.partition("=")
        env[name] = value

    frozen = args.attach_at <= 0
    board = start(args.sd, args.port, args.gdbport, env,
                  Path("/tmp/gdbprobe-main.log"), frozen=frozen)
    started = time.time()
    rsp = None
    try:
        time.sleep(max(args.attach_at, 2.0))
        rsp = Rsp(args.gdbport)
        rsp.cmd("?")                      # attaching stops the machine
        print("attached at t=%.1f" % (time.time() - started))
        for addr in args.breaks:
            print("  break %#010x -> %r" % (addr, rsp.point(0, addr, 2)))
        for addr in args.watch:
            print("  watch %#010x -> %r" % (addr, rsp.point(2, addr, 4)))
        for addr in args.sample:
            print("  sample %#010x = %s" % (addr, hex(rsp.word(addr) or 0)))

        stops = 0
        rsp.resume()
        while time.time() - started < args.seconds and stops < args.max_stops:
            reply = rsp.packet(timeout=1.0)
            if not reply:
                continue
            stops += 1
            regs = rsp.regs()
            extra = "".join("  %#010x=%s" % (a, hex(rsp.word(a) or 0))
                            for a in args.sample)
            print("t=%6.1f  %-28s pc=%#010x pr=%#010x r4=%#x r5=%#x r14=%#x%s"
                  % (time.time() - started, reply[:28],
                     regs[REG_PC], regs[REG_PR], regs[4], regs[5], regs[14],
                     extra), flush=True)
            rsp.resume()
        print("stops=%d" % stops)
    finally:
        if rsp:
            try:
                rsp.sock.close()
            except OSError:
                pass
        try:
            board.terminate()
            board.wait(timeout=20)
        except subprocess.TimeoutExpired:
            board.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
