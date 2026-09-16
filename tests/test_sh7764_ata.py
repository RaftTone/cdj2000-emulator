"""Exercise the SH4 ATA task file with an optional attached CD backend."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ATA_COMMAND = 0xFFF0001C


def _qtest(binary: Path, bios: Path, image: Path) -> tuple[list[str], str]:
    process = subprocess.Popen(
        [
            str(binary),
            "-M",
            "cdj2000nxs-main",
            "-bios",
            str(bios),
            "-drive",
            f"if=ide,media=cdrom,bus=0,unit=0,file={image},format=raw",
            "-display",
            "none",
            "-monitor",
            "none",
            "-S",
            "-qtest",
            "stdio",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    commands = [f"writew {ATA_COMMAND:#x} 0xa1", f"readw {ATA_COMMAND:#x}"]
    try:
        assert process.stdin is not None
        assert process.stdout is not None
        for command in commands:
            process.stdin.write(command + "\n")
            process.stdin.flush()
        replies = [process.stdout.readline().strip() for _ in commands]
    finally:
        process.terminate()
        process.wait(timeout=5)
    return replies, process.stderr.read()


def test_ata_attaches_cd_backend_and_answers_identify(tmp_path: Path) -> None:
    """A supplied image reaches ide-cd and the ATAPI identify command runs."""

    binary = Path(
        os.environ.get("CDJ_TEST_QEMU", ROOT / "build/qemu/build/qemu-system-sh4")
    )
    if not binary.is_file():
        import pytest

        pytest.skip("build the custom QEMU board or set CDJ_TEST_QEMU")

    bios = tmp_path / "bios.bin"
    image = tmp_path / "disc.iso"
    bios.write_bytes(bytes(16))
    image.write_bytes(bytes(2048 * 4))

    replies, stderr = _qtest(binary, bios, image)
    assert replies[0] == "OK"
    # DRDY + DSC + DRQ: the attached ide-cd accepted IDENTIFY PACKET DEVICE.
    assert replies[1].startswith("OK 0x")
    assert int(replies[1].split()[1], 0) & 0x58 == 0x58
    assert "attached IDE CD backend" in stderr
    assert "drive present with attached medium" in stderr
