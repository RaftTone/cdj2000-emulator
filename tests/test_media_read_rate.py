"""CDJ_SD_READ_BPS on the real QEMU board, with the guest CPU stopped.

qtest drives the SD host's registers the way the firmware's driver does
(CMD0, CMD8, ACMD41, CMD2, CMD3, CMD7, CMD16, then CMD17 / CMD18) against a
small card image, steps the virtual clock and watches INFO2's RXRDY: without a
rate a block arrives after the model's fixed 20 us, with one after size / rate,
and the bytes are the same either way.  No firmware is needed.  Build QEMU
with scripts/build-qemu-sh4.sh first, or set CDJ_QEMU to the binary to test.
"""

from __future__ import annotations

import os
from pathlib import Path
import select
import subprocess

import pytest

# The qtest pipe is polled with select(), which Windows only allows on sockets.
pytestmark = pytest.mark.skipif(
    os.name == "nt", reason="select() on pipes is not supported on Windows")

ROOT = Path(__file__).resolve().parents[1]
SDHI = 0xFFE40000
CMD, ARG0, ARG1, STOP, SECCNT, RSP, INFO1, INFO2, SIZE, DATA = (
    0x00, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x1C, 0x1E, 0x26, 0x30)
RXRDY, CMDTIMEOUT, DATAEND = 0x0100, 0x0040, 0x0004
R1, NO_RESPONSE, R2 = 0x0400, 0x0300, 0x0600
READ_DATA, MULTI = 0x1800, 0x2000
XFER_NS = 20000                     # SDHI_XFER_NS in cdj2000_main.c
CARD_BYTES = 1 << 20


def binary() -> Path:
    return Path(os.environ.get("CDJ_QEMU", ROOT / "build/qemu/build/qemu-system-sh4"))


def pattern(offset: int, length: int) -> bytes:
    return bytes(((i * 7) ^ (i >> 9)) & 0xFF for i in range(offset, offset + length))


@pytest.fixture
def board(tmp_path):
    """Start the board with the given extra environment; yields (qtest, stderr path)."""
    if not binary().is_file():
        pytest.skip("build the CDJ QEMU board or set CDJ_QEMU")
    procs = []

    def start(extra: dict[str, str]):
        bios = tmp_path / "dummy.bin"
        bios.write_bytes(bytes(16))
        card = tmp_path / "card.img"
        card.write_bytes(pattern(0, CARD_BYTES))
        env = {k: v for k, v in os.environ.items() if not k.startswith(("CDJ_", "BFIN_"))}
        env.update(extra)
        stderr = tmp_path / "qemu.log"
        proc = subprocess.Popen(
            [str(binary()), "-M", "cdj2000-main", "-bios", str(bios),
             "-display", "none", "-serial", "null", "-monitor", "none",
             "-drive", f"if=sd,format=raw,file={card}",
             "-accel", "qtest", "-qtest", "stdio", "-qtest-log", os.devnull],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr.open("w"),
            env=env, bufsize=0)
        procs.append(proc)

        def qtest(line: str) -> str:
            proc.stdin.write((line + "\n").encode())
            assert select.select([proc.stdout], [], [], 10)[0], "QEMU qtest timed out"
            response = proc.stdout.readline().decode().strip()
            assert response.startswith("OK"), response
            return response[2:].strip()

        return qtest, stderr

    yield start
    for proc in procs:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stdin.close()
        proc.stdout.close()


def readw(q, offset: int) -> int:
    return int(q(f"readw {SDHI + offset:#x}"), 0)


def writew(q, offset: int, value: int) -> None:
    q(f"writew {SDHI + offset:#x} {value:#x}")


def command(q, index: int, arg: int = 0, flags: int = R1) -> None:
    writew(q, ARG0, arg & 0xFFFF)
    writew(q, ARG1, arg >> 16)
    writew(q, CMD, flags | index)
    assert not readw(q, INFO2) & CMDTIMEOUT, f"CMD{index} got no answer"


def select_card(q) -> None:
    """Idle -> transfer state, as 0x1ff518's callers take a card there."""
    command(q, 0, flags=NO_RESPONSE)
    command(q, 8, 0x1AA)
    command(q, 55)
    command(q, 41, 0x00FF8000)          # not an enquiry: powered up at once
    command(q, 2, flags=R2)
    command(q, 3)
    rca = readw(q, RSP + 2)
    command(q, 7, rca << 16)
    command(q, 16, 512)
    writew(q, SIZE, 512)


def arrival_ns(q, limit_ns: int, step_ns: int) -> int:
    """Step the clock until RXRDY; the guest time it took, in steps of step_ns."""
    waited = 0
    while not readw(q, INFO2) & RXRDY:
        assert waited <= limit_ns, "no block within %d ns" % limit_ns
        q(f"clock_step {step_ns}")
        waited += step_ns
    return waited


def drain(q) -> bytes:
    out = bytearray()
    for _ in range(256):
        out += readw(q, DATA).to_bytes(2, "little")
    return bytes(out)


@pytest.mark.parametrize("rate,block_ns", [
    (None, XFER_NS),                    # unset: the model's own timing
    (0, XFER_NS),                       # 0: the same
    (512_000_000, XFER_NS),             # faster than 20 us a block: no change
    (512_000, 1_000_000),               # 1 ms a block
    (2_000_000, 256_000),
])
def test_sd_block_arrives_after_size_over_rate(board, rate, block_ns):
    q, _ = board({} if rate is None else {"CDJ_SD_READ_BPS": str(rate)})
    select_card(q)
    step = 1000
    # A single block (CMD17, byte address on this small card).
    command(q, 17, 3 * 512, R1 | READ_DATA)
    assert arrival_ns(q, 10 * block_ns, step) == block_ns
    assert drain(q) == pattern(3 * 512, 512)
    # Three blocks with the automatic stop (CMD18): each after its own delay,
    # counted from the moment the one before was drained.
    writew(q, SECCNT, 3)
    writew(q, STOP, 0x100)
    command(q, 18, 8 * 512, R1 | READ_DATA | MULTI)
    for block in range(3):
        assert arrival_ns(q, 10 * block_ns, step) == block_ns
        assert drain(q) == pattern((8 + block) * 512, 512)
    assert readw(q, INFO1) & DATAEND


def test_read_log_names_commands_and_bursts(board):
    q, stderr = board({"CDJ_SD_READ_BPS": "512000", "CDJ_READ_LOG": "2"})
    select_card(q)
    command(q, 17, 5 * 512, R1 | READ_DATA)
    arrival_ns(q, 2_000_000, 1000)
    drain(q)
    q("clock_step 60000000")            # past the 50 ms that ends a burst
    text = stderr.read_text()
    assert "cdj2000-sd: reads limited to 512000 bytes per guest second" in text
    assert "cdj2000-sd: read CMD17 block 0xa00 x1 t=" in text
    burst = [line for line in text.splitlines() if "cdj2000-sd: reads t=" in line]
    assert len(burst) == 1, text
    assert "512 bytes, 1 commands, 0.001 s added by the rate" in burst[0]


@pytest.mark.parametrize("variable", ["CDJ_SD_READ_BPS", "CDJ_USB_READ_BPS"])
@pytest.mark.parametrize("value", ["2M", "-1", "1.5", " 7"])
def test_a_rate_that_is_not_a_number_stops_the_board(tmp_path, variable, value):
    if not binary().is_file():
        pytest.skip("build the CDJ QEMU board or set CDJ_QEMU")
    bios = tmp_path / "dummy.bin"
    bios.write_bytes(bytes(16))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CDJ_", "BFIN_"))}
    env[variable] = value
    done = subprocess.run(
        [str(binary()), "-M", "cdj2000-main", "-bios", str(bios), "-display", "none",
         "-serial", "null", "-monitor", "none", "-S", "-qtest", "null"],
        env=env, capture_output=True, text=True, timeout=20)
    assert done.returncode != 0
    assert f"{variable}: not a number of bytes per second" in done.stderr
