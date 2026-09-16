"""Optional stopped-CPU QEMU controller integration, NOT a firmware boot test.

CDJ_ETH_QEMU_TEST=1 python -m pytest -q tests/test_nxs_ethernet_qemu.py
Requires rebuilt QEMU, local MAIN BIOS, and permission for loopback sockets.
Uses only synthetic guest RAM descriptors/frames; firmware never executes.
"""
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x1EF00000


def test_nxs_ethernet_qemu(tmp_path):
    if os.environ.get("CDJ_ETH_QEMU_TEST") != "1":
        pytest.skip("opt-in QEMU/socket controller integration")
    qemu = Path(os.environ.get("CDJ_QEMU", ROOT / "build/qemu/build/qemu-system-sh4"))
    bios = ROOT / "firmware/nxs/main-firmware.bin"
    if not qemu.is_file() or not bios.is_file():
        pytest.skip("requires built SH4 QEMU and local MAIN BIOS")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = listener.getsockname()[1]
        log = (tmp_path / "qemu.log").open("w+")
        process = subprocess.Popen([
            # qtest accelerator never executes CPUs. Do not use -S: QEMU
            # suppresses network queue delivery while runstate is paused.
            str(qemu), "-M", "cdj2000nxs-main", "-accel", "qtest",
            "-bios", str(bios), "-display", "none", "-serial", "null",
            "-serial", "null", "-serial", "null", "-monitor", "none",
            "-qtest", "stdio", "-qtest-log", "/dev/null", "-nic",
            f"socket,model=cdj-nxs-ethernet,connect=127.0.0.1:{port}",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
            text=True, bufsize=1)
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(5)

                def command(text):
                    process.stdin.write(text + "\n")
                    process.stdin.flush()
                    assert select.select([process.stdout], [], [], 5)[0], text
                    response = process.stdout.readline().strip()
                    assert response.startswith("OK"), (text, response)
                    return response.split()[1:]

                def rd(address):
                    return int(command(f"readl {address:#x}")[0], 0)

                def wr(address, value):
                    command(f"writel {address:#x} {value:#x}")

                def write_mem(address, data):
                    command(f"write {address:#x} {len(data)} 0x{data.hex()}")

                def read_mem(address, size):
                    return bytes.fromhex(command(f"read {address:#x} {size}")[0][2:])

                # Synthetic clock characterization, not firmware setup. Repeat
                # from the same counter epoch; never derive rate from host time.
                clock_runs = []
                for _ in range(2):
                    wr(0xFFDC0004, 0)  # TSTR2: stop TMU3
                    wr(0xFFDC0008, 0xFFFFFFFF)  # TCOR3
                    wr(0xFFDC000C, 0xFFFFFFFF)  # TCNT3
                    wr(0xFFDC0010, 0)  # TCR3: Pphi/4, no IRQ
                    wr(0xFFDC0004, 1)
                    initial = rd(0xFFDC000C)
                    values = []
                    for delta in (1000000, 999000000, 318000000000):
                        command(f"clock_step {delta}")
                        values.append(rd(0xFFDC000C))
                    clock_runs.append(values)
                    assert (initial - values[0]) & 0xFFFFFFFF == 13500
                    assert (initial - values[1]) & 0xFFFFFFFF == 13500000
                    # 319 virtual seconds crosses the 32-bit downcounter wrap.
                    assert abs(((initial - values[2]) & 0xFFFFFFFF)
                               - ((319 * 13500000) & 0xFFFFFFFF)) <= 1
                assert clock_runs[0] == clock_runs[1]
                wr(0xFFDC0004, 0)

                def bit(drive, value):
                    v = (2 if drive else 0) | (4 if value else 0)
                    wr(BASE + 0x120, v)
                    wr(BASE + 0x120, v | 1)
                    return (rd(BASE + 0x120) >> 3) & 1

                def bits(value, count):
                    for i in reversed(range(count)):
                        bit(True, (value >> i) & 1)

                def mdio(reg, value=None):
                    bits(0xFFFFFFFF, 32)
                    bits(1, 2)
                    bits(2 if value is None else 1, 2)
                    bits(1, 5)
                    bits(reg, 5)
                    if value is None:
                        assert bit(False, 1) == 1
                        assert bit(False, 1) == 0
                        value = 0
                        for _ in range(16):
                            value = (value << 1) | bit(False, 1)
                    else:
                        bits(2, 2)
                        bits(value, 16)
                    bit(False, 1)
                    return value

                assert mdio(2) == 0x001C
                assert mdio(3) == 0xC816
                assert not mdio(1) & 4
                command("clock_step 100000000")
                mdio(1)  # latched-low BMSR must be read twice
                assert mdio(1) & 0x24 == 0x24
                assert mdio(5) == 0x4101
                # Reset/re-negotiate through actual Clause-22 PIR edges.
                mdio(0, 0x8000)
                assert mdio(0) & 0x8000
                command("clock_step 200000000")
                assert not mdio(0) & 0x8000
                mdio(1)
                assert mdio(1) & 4
                # Native SH little-endian numeric descriptor words.
                tx, rx, data, dest = 0x04010000, 0x04010100, 0x04011000, 0x04012000
                payload = bytes.fromhex("ffffffffffff0200000000010800") + b"test"
                write_mem(tx, struct.pack("<IIII", 0xF0000000, len(payload) << 16, data, 0))
                write_mem(data, payload)
                write_mem(rx, struct.pack("<IIII", 0xC0000000, 128 << 16, dest, 0))
                for offset, value in [(0, 0x40), (0x18, tx), (0x20, rx),
                                      (0x58, 3), (0x100, 0x62), (0x30, 0x240000)]:
                    wr(BASE + offset, value)
                assert rd(0xFEF00000 + 0x18) == tx  # P4/Area7 alias
                wr(BASE + 8, 1)

                def recv_exact(size):
                    result = b""
                    while len(result) < size:
                        chunk = peer.recv(size - len(result))
                        assert chunk
                        result += chunk
                    return result

                length = struct.unpack("!I", recv_exact(4))[0]
                assert recv_exact(length) == payload.ljust(60, b"\0")
                assert rd(tx) == 0x70000000
                assert rd(0x1FD400C0) == 0x10000
                assert rd(0x1FD400C4) == 0
                wr(0x1FD400D4, 0x10000)
                assert rd(0xFFD400D0) & 0x10000 == 0
                assert rd(0x1FD400C4) == 0x10000
                wr(BASE + 0x28, 0x200000)
                assert rd(0x1FD400C0) == 0
                wr(BASE + 0x10, 1)
                frame = payload.ljust(60, b"\x5a")
                peer.sendall(struct.pack("!I", len(frame)) + frame)
                deadline = time.monotonic() + 5
                while rd(rx) & 0x80000000 and time.monotonic() < deadline:
                    time.sleep(0.005)
                assert rd(rx) == 0x70000000
                assert rd(rx + 4) == (128 << 16) | len(frame)
                assert read_mem(dest, len(frame)) == frame
                assert rd(0x1FD400C4) == 0x10000
                wr(BASE, 1)
                assert rd(BASE + 0x18) == tx and rd(BASE + 0x20) == rx
                assert rd(BASE + 0x100) == 0 and rd(BASE + 0x28) == 0
                assert rd(0x1FD400C4) == 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            log.close()
            process.stdin.close()
            process.stdout.close()
