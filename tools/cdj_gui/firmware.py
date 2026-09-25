# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import binascii
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


UPDATE_HEADER_SIZE = 32
UPDATE_TRAILER_SIZE = 4
BOOT_HEADER = struct.Struct("<IIH")
BLACKFIN_BF533_ENTRY_POINT = 0xFFA00000
BLACKFIN_BF531_ENTRY_POINT = 0xFFA08000
ELF_MACHINE_BLACKFIN = 106
ELF_PAYLOAD_ALIGNMENT = 0x1000
GUI_FLASH_SIZE = 0x200000
GUI_RESOURCE_FLASH_GAP = 0x10000

BFLAG_ZEROFILL = 0x0001
BFLAG_RESVECT = 0x0002
BFLAG_INIT = 0x0008
BFLAG_IGNORE = 0x0010
BFLAG_FINAL = 0x8000


class FirmwareFormatError(ValueError):
    """Raised when an updater or Blackfin boot stream is malformed."""


@dataclass(frozen=True)
class BootBlock:
    index: int
    header_offset: int
    payload_offset: int
    target: int
    count: int
    flags: int
    data: bytes

    @property
    def is_zero_fill(self) -> bool:
        return bool(self.flags & BFLAG_ZEROFILL)

    @property
    def is_init(self) -> bool:
        return bool(self.flags & BFLAG_INIT)

    @property
    def uses_bf533_reset_vector(self) -> bool:
        return bool(self.flags & BFLAG_RESVECT)

    @property
    def is_ignored(self) -> bool:
        return bool(self.flags & BFLAG_IGNORE)

    @property
    def is_final(self) -> bool:
        return bool(self.flags & BFLAG_FINAL)

    @property
    def loaded(self) -> bool:
        return not self.is_ignored

    def flag_names(self) -> list[str]:
        names: list[str] = []
        if self.is_zero_fill:
            names.append("zero_fill")
        if self.uses_bf533_reset_vector:
            names.append("bf533_reset_vector")
        if self.is_init:
            names.append("init")
        if self.is_ignored:
            names.append("ignore")
        if self.is_final:
            names.append("final")
        known = BFLAG_ZEROFILL | BFLAG_RESVECT | BFLAG_INIT | BFLAG_IGNORE | BFLAG_FINAL
        if self.flags & ~known:
            names.append(f"unknown_0x{self.flags & ~known:04x}")
        return names


@dataclass(frozen=True)
class MemorySpan:
    address: int
    data: bytes

    @property
    def end(self) -> int:
        return self.address + len(self.data)


@dataclass(frozen=True)
class GuiUpdate:
    path: Path
    raw: bytes
    header: bytes
    body: bytes
    trailer: bytes
    blocks: tuple[BootBlock, ...]
    boot_stream_end: int
    crc_calculated: int
    crc_stored: int

    @property
    def version_text(self) -> str:
        return self.header.decode("ascii", errors="replace").rstrip("\x00 ")

    @property
    def crc_valid(self) -> bool:
        return self.crc_calculated == self.crc_stored

    @property
    def resource_tail(self) -> bytes:
        return self.body[self.boot_stream_end :]

    @property
    def entry_point(self) -> int:
        if self.blocks[-1].uses_bf533_reset_vector:
            return BLACKFIN_BF533_ENTRY_POINT
        return BLACKFIN_BF531_ENTRY_POINT

    def memory_spans(self) -> tuple[MemorySpan, ...]:
        return blocks_memory_spans(self.blocks)


def blocks_memory_spans(blocks: Iterable[BootBlock]) -> tuple[MemorySpan, ...]:
    """What a boot stream leaves in memory, as contiguous spans."""
    memory: dict[int, int] = {}
    for block in blocks:
        if not block.loaded:
            continue
        payload = bytes(block.count) if block.is_zero_fill else block.data
        for offset, value in enumerate(payload):
            memory[block.target + offset] = value

    if not memory:
        return ()

    spans: list[MemorySpan] = []
    addresses = sorted(memory)
    start = addresses[0]
    previous = start
    values = bytearray([memory[start]])
    for address in addresses[1:]:
        if address != previous + 1:
            spans.append(MemorySpan(start, bytes(values)))
            start = address
            values = bytearray()
        values.append(memory[address])
        previous = address
    spans.append(MemorySpan(start, bytes(values)))
    return tuple(spans)


def crc16_xmodem(data: bytes) -> int:
    return binascii.crc_hqx(data, 0)


def physical_flash_image(update: GuiUpdate) -> bytes:
    """Expand the compact updater body into the GUI's physical 2 MiB flash.

    What this returns puts the boot stream at flash 0 and a 64 KiB gap
    between it and the resource tail.  **The GUI's own updater does not do
    that** (run update-30, 2026-09-06, the 2000 firmware applying a
    C2KGUI.UPD it received over the link, the simulator's flash dumped
    afterwards): it programs the *whole body*, stream and tail contiguous,
    at flash 0x10000, and never erases or writes 0x0..0xffff -- the 64 KiB
    gap is in front of the stream, not behind it, and sector 0 holds
    something the update file does not carry (on the board presumably the
    first-stage loader the BF531 boot ROM parses; here whatever this image
    put there).  The resource tail lands at the same offset either way
    (0x10000 + stream length), which is why the simulator never noticed.
    Kept as it is because the simulator loads the ELF and reads only the
    tail from this image; a real flash dump would settle sector 0.
    """

    flash_body = (
        update.body[: update.boot_stream_end]
        + bytes([0xFF]) * GUI_RESOURCE_FLASH_GAP
        + update.body[update.boot_stream_end :]
    )
    if len(flash_body) > GUI_FLASH_SIZE:
        raise FirmwareFormatError(
            f"expanded GUI flash body is 0x{len(flash_body):x} bytes, "
            "larger than 2 MiB flash"
        )
    return flash_body + bytes([0xFF]) * (GUI_FLASH_SIZE - len(flash_body))


def parse_boot_stream(body: bytes) -> tuple[tuple[BootBlock, ...], int]:
    blocks: list[BootBlock] = []
    cursor = 0
    while True:
        if cursor + BOOT_HEADER.size > len(body):
            raise FirmwareFormatError("Blackfin boot stream has no complete final header")

        header_offset = cursor
        target, count, flags = BOOT_HEADER.unpack_from(body, cursor)
        cursor += BOOT_HEADER.size
        payload_offset = cursor

        consumes_payload = not (flags & BFLAG_ZEROFILL)
        if consumes_payload:
            end = cursor + count
            if end > len(body):
                raise FirmwareFormatError(
                    f"block {len(blocks)} payload ends at 0x{end:x}, outside GUI body"
                )
            data = body[cursor:end]
            cursor = end
        else:
            data = b""

        block = BootBlock(
            index=len(blocks),
            header_offset=header_offset,
            payload_offset=payload_offset,
            target=target,
            count=count,
            flags=flags,
            data=data,
        )
        blocks.append(block)
        if block.is_final:
            return tuple(blocks), cursor


def parse_gui_update(path: Path | str) -> GuiUpdate:
    path = Path(path)
    raw = path.read_bytes()
    minimum = UPDATE_HEADER_SIZE + UPDATE_TRAILER_SIZE + BOOT_HEADER.size
    if len(raw) < minimum:
        raise FirmwareFormatError(f"GUI updater is only {len(raw)} bytes")

    header = raw[:UPDATE_HEADER_SIZE]
    body = raw[UPDATE_HEADER_SIZE:-UPDATE_TRAILER_SIZE]
    trailer = raw[-UPDATE_TRAILER_SIZE:]
    blocks, boot_stream_end = parse_boot_stream(body)
    return GuiUpdate(
        path=path,
        raw=raw,
        header=header,
        body=body,
        trailer=trailer,
        blocks=blocks,
        boot_stream_end=boot_stream_end,
        crc_calculated=crc16_xmodem(raw[:-UPDATE_TRAILER_SIZE]),
        crc_stored=int.from_bytes(trailer[2:], "big"),
    )


def build_blackfin_elf(spans: Iterable[MemorySpan], entry: int = BLACKFIN_BF531_ENTRY_POINT) -> bytes:
    spans = tuple(spans)
    elf_header_size = 52
    program_header_size = 32
    section_header_size = 40
    program_headers_end = elf_header_size + program_header_size * len(spans)
    cursor = (program_headers_end + ELF_PAYLOAD_ALIGNMENT - 1) & ~(ELF_PAYLOAD_ALIGNMENT - 1)

    program_headers = bytearray()
    image = bytearray(cursor)
    section_records: list[tuple[int, MemorySpan]] = []
    for span in spans:
        offset = cursor
        program_headers += struct.pack(
            "<IIIIIIII",
            1,  # PT_LOAD
            offset,
            span.address,
            span.address,
            len(span.data),
            len(span.data),
            7,  # PF_R | PF_W | PF_X: firmware regions are mixed code/data
            4,
        )
        image += span.data
        section_records.append((offset, span))
        cursor += len(span.data)
        # Keep file-backed regions on separate pages.  Besides making the
        # image easier to inspect, this avoids stale sequential-read state in
        # the MinGW build of BFD used by GNU sim when adjacent sections are
        # several megabytes long.
        padding = (-cursor) & (ELF_PAYLOAD_ALIGNMENT - 1)
        image += bytes(padding)
        cursor += padding

    section_names = bytearray(b"\x00")
    section_name_offsets: list[int] = []
    for index in range(len(spans)):
        section_name_offsets.append(len(section_names))
        section_names += f".load{index}".encode("ascii") + b"\x00"
    shstrtab_name_offset = len(section_names)
    section_names += b".shstrtab\x00"
    shstrtab_offset = cursor
    image += section_names
    cursor += len(section_names)
    section_header_offset = (cursor + 3) & ~3
    image += bytes(section_header_offset - cursor)

    section_headers = bytearray(section_header_size)  # Mandatory null section.
    for name_offset, (file_offset, span) in zip(section_name_offsets, section_records):
        section_headers += struct.pack(
            "<IIIIIIIIII",
            name_offset,
            1,  # SHT_PROGBITS
            7,  # SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR
            span.address,
            file_offset,
            len(span.data),
            0,
            0,
            4,
            0,
        )
    section_headers += struct.pack(
        "<IIIIIIIIII",
        shstrtab_name_offset,
        3,  # SHT_STRTAB
        0,
        0,
        shstrtab_offset,
        len(section_names),
        0,
        0,
        1,
        0,
    )
    image += section_headers

    ident = b"\x7fELF" + bytes([1, 1, 1, 0, 0]) + bytes(7)
    header = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2,  # ET_EXEC
        ELF_MACHINE_BLACKFIN,
        1,
        entry,
        elf_header_size,
        section_header_offset,
        0,
        elf_header_size,
        program_header_size,
        len(spans),
        section_header_size,
        len(spans) + 2,
        len(spans) + 1,
    )
    image[:elf_header_size] = header
    image[elf_header_size:program_headers_end] = program_headers
    return bytes(image)


def manifest(update: GuiUpdate) -> dict[str, object]:
    spans = update.memory_spans()
    first = update.blocks[0]
    dxe_length = int.from_bytes(first.data, "little") if first.is_ignored and first.count == 4 else None
    return {
        "source": str(update.path),
        "version": update.version_text,
        "update_size": len(update.raw),
        "header_size": len(update.header),
        "body_size": len(update.body),
        "physical_flash": {
            "size": GUI_FLASH_SIZE,
            "resource_gap_offset": f"0x{update.boot_stream_end:x}",
            "resource_gap_size": GUI_RESOURCE_FLASH_GAP,
            "resource_tail_flash_offset": (
                f"0x{update.boot_stream_end + GUI_RESOURCE_FLASH_GAP:x}"
            ),
        },
        "trailer_hex": update.trailer.hex(),
        "crc16_xmodem": {
            "calculated": f"0x{update.crc_calculated:04x}",
            "stored": f"0x{update.crc_stored:04x}",
            "valid": update.crc_valid,
        },
        "boot": {
            "entry_point": f"0x{update.entry_point:08x}",
            "block_count": len(update.blocks),
            "dxe_length": dxe_length,
            "stream_end_body_offset": f"0x{update.boot_stream_end:x}",
            "stream_end_update_offset": f"0x{UPDATE_HEADER_SIZE + update.boot_stream_end:x}",
            "resource_tail_size": len(update.resource_tail),
        },
        "memory_spans": [
            {
                "start": f"0x{span.address:08x}",
                "end_exclusive": f"0x{span.end:08x}",
                "size": len(span.data),
            }
            for span in spans
        ],
        "blocks": [
            {
                "index": block.index,
                "header_offset": f"0x{block.header_offset:x}",
                "payload_offset": f"0x{block.payload_offset:x}",
                "target": f"0x{block.target:08x}",
                "count": block.count,
                "flags": f"0x{block.flags:04x}",
                "flag_names": block.flag_names(),
            }
            for block in update.blocks
        ],
    }


def write_outputs(update: GuiUpdate, output_dir: Path | str) -> None:
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    spans = update.memory_spans()
    (output_dir / "gui-flash-body.bin").write_bytes(update.body)
    # C2KGUI.UPD is a compact update image rather than a byte-for-byte flash
    # dump.  It omits one 64 KiB sector between the Blackfin boot stream and
    # the resource tail.  The stock firmware's hard-coded resource addresses
    # are consequently 0x10000 above their offsets in the update body.
    (output_dir / "gui-flash-image.bin").write_bytes(physical_flash_image(update))
    (output_dir / "gui-resource-tail.bin").write_bytes(update.resource_tail)
    (output_dir / "gui-boot-memory.elf").write_bytes(build_blackfin_elf(spans, update.entry_point))
    (output_dir / "gui-memory-map.json").write_text(
        json.dumps(manifest(update), indent=2) + "\n", encoding="utf-8"
    )
    spans_dir = output_dir / "spans"
    spans_dir.mkdir(exist_ok=True)
    for span in spans:
        (spans_dir / f"{span.address:08x}-{span.end:08x}.bin").write_bytes(span.data)
