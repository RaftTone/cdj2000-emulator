"""CDJ_SDRAM_FILL: what the SDRAM holds at power-on, read back through qtest.

No proprietary firmware is needed (a 16-byte dummy image, the CPU stopped).
Build QEMU with scripts/build-qemu-sh4.sh first, or set CDJ_QEMU to the binary.
"""

from __future__ import annotations

import os
from pathlib import Path
import select
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SDRAM = 0x04000000
SIZE = 64 * 1024 * 1024
PROBES = [SDRAM, SDRAM + 0x100000, SDRAM + SIZE - 16]
LEN = 16


def power_on(tmp_path, fill, machine="cdj2000-main"):
    """Start the board with CDJ_SDRAM_FILL=fill; return (bytes at the probes, stderr)."""
    binary = Path(os.environ.get("CDJ_QEMU", ROOT / "build/qemu/build/qemu-system-sh4"))
    if not binary.is_file():
        pytest.skip("build the CDJ QEMU board or set CDJ_QEMU")
    bios = tmp_path / "dummy.bin"
    bios.write_bytes(bytes(16))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CDJ_", "BFIN_"))}
    if fill is not None:
        env["CDJ_SDRAM_FILL"] = fill
    log_path = tmp_path / "qemu.log"
    with log_path.open("w") as log:
        proc = subprocess.Popen(
            [str(binary), "-M", machine, "-bios", str(bios),
             "-display", "none", "-serial", "null", "-monitor", "none",
             "-S", "-qtest", "stdio", "-qtest-log", os.devnull],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=env,
            bufsize=0,
        )
        try:
            seen = []
            for address in PROBES:
                proc.stdin.write(f"read {address:#x} {LEN:#x}\n".encode())
                assert select.select([proc.stdout], [], [], 10)[0], "QEMU qtest timed out"
                response = proc.stdout.readline().decode().strip()
                assert response.startswith("OK"), response
                seen.append(bytes.fromhex(response[2:].strip()[2:]))
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()
    return seen, log_path.read_text()


@pytest.mark.parametrize("machine", ["cdj2000-main", "cdj2000nxs-main"])
@pytest.mark.parametrize("fill,byte", [(None, 0x00), ("00", 0x00), ("ff", 0xFF), ("a5", 0xA5)])
def test_constant_fill(tmp_path, fill, byte, machine):
    seen, _ = power_on(tmp_path, fill, machine)
    assert seen == [bytes([byte]) * LEN] * len(PROBES)


@pytest.mark.parametrize("machine", ["cdj2000-main", "cdj2000nxs-main"])
def test_random_fill_is_repeatable_per_seed(tmp_path, machine):
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    (tmp_path / "c").mkdir()
    first, _ = power_on(tmp_path / "a", "random:7", machine)
    again, _ = power_on(tmp_path / "b", "random:7", machine)
    other, _ = power_on(tmp_path / "c", "random:8", machine)
    assert first == again
    assert first != other
    for chunk in first + other:
        assert chunk != bytes(LEN)
    assert len(set(first)) == len(first)     # the three probes do not repeat each other


def test_unknown_spec_leaves_zero_and_warns(tmp_path):
    seen, log = power_on(tmp_path, "zz")
    assert seen == [bytes(LEN)] * len(PROBES)
    assert "CDJ_SDRAM_FILL=zz" in log
