"""Pack a (modified) MAIN application back into a bootable flash image.

    python -m tools.cdj_main.repack_main APP.bin OUT-FLASH.bin \
        [--base firmware/main-firmware.bin] [--upd OUT.UPD --version 4.34]

The inverse of `tools.cdj_gui.main_unpack` for the application region: that
tool cuts `main-unpacked.bin` out of the flash image, this one puts an edited
copy back.  The region at 0x40000 is a little-endian 32-bit packed length, the
LZSS stream the boot ROM's second stage decodes (4 KiB ring of spaces, write
position 0xfee, 3..18-byte matches, flag bit 1 = literal), and a 16-bit
little-endian sum over the length word and the stream.

The compressor is greedy longest-match, like the Okumura encoder the original
was made with, but it is not byte-identical to Pioneer's output and does not
need to be: the image is checked by decoding it again with main_unpack's
decoder before anything is written, and the real proof is the boot ROM
unpacking it in the emulator.

Everything else in the base image -- boot ROM, loader at 0x10000 -- is kept.
The bytes between the new stream's end and the old one's are erased to 0xff,
so a shorter application leaves no stale tail behind.  `--upd` also writes the
update file, with `--end` taken from the new packed end rather than the stock
0x287d60.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from tools.cdj_gui.main_unpack import LZSS_LOOKAHEAD, LZSS_WINDOW_SIZE, decompress_lzss, unpack_region
from tools.cdj_main import make_upd
from tools.paths import FIRMWARE

APP_REGION = 0x40000
# The updater erases and programs 0x40000..0x3dffff; the packed application
# has to fit there with its length word and checksum.
APP_REGION_END = 0x3E0000
MIN_MATCH = 3
MAX_MATCH = LZSS_LOOKAHEAD
# One short of the ring: a match at distance 4096 reads the slot the decoder is
# about to overwrite, which works with this decoder but is not worth relying on.
MAX_DISTANCE = LZSS_WINDOW_SIZE - 1
START = LZSS_WINDOW_SIZE - LZSS_LOOKAHEAD
CHAIN_LIMIT = 256


def compress_lzss(data: bytes) -> bytes:
    """Encode DATA so that main_unpack.decompress_lzss returns it unchanged."""
    # The ring starts full of spaces, so the stream behaves as if DATA were
    # preceded by a whole window of them: matches may reach into that prefix.
    pad = LZSS_WINDOW_SIZE
    text = b" " * pad + data
    end = len(text)
    head: dict[bytes, int] = {}
    prev = [0] * end

    def insert(position: int) -> None:
        key = text[position:position + MIN_MATCH]
        prev[position] = head.get(key, -1)
        head[key] = position

    for position in range(pad - MAX_DISTANCE, pad):
        insert(position)

    out = bytearray()
    flag_at = -1
    flag_bit = 8
    position = pad
    while position < end:
        if flag_bit == 8:
            flag_at = len(out)
            out.append(0)
            flag_bit = 0
        best_length = 0
        best_from = 0
        limit = min(MAX_MATCH, end - position)
        if limit >= MIN_MATCH:
            candidate = head.get(text[position:position + MIN_MATCH], -1)
            chain = CHAIN_LIMIT
            floor = position - MAX_DISTANCE
            while candidate >= floor and chain:
                chain -= 1
                if text[candidate + best_length] == text[position + best_length]:
                    length = MIN_MATCH
                    while length < limit and text[candidate + length] == text[position + length]:
                        length += 1
                    if length > best_length:
                        best_length, best_from = length, candidate
                        if length == limit:
                            break
                candidate = prev[candidate]
        if best_length >= MIN_MATCH:
            ring = (START + best_from - pad) & (LZSS_WINDOW_SIZE - 1)
            out.append(ring & 0xFF)
            out.append(((ring >> 4) & 0xF0) | (best_length - MIN_MATCH))
            step = best_length
        else:
            out[flag_at] |= 1 << flag_bit
            out.append(text[position])
            step = 1
        flag_bit += 1
        for inserted in range(position, min(position + step, end - MIN_MATCH + 1)):
            insert(inserted)
        position += step
    return bytes(out)


def repack(base: bytes, app: bytes) -> tuple[bytes, int]:
    """(new flash image, end of the packed region) with APP at 0x40000."""
    old = unpack_region(base, APP_REGION)
    old_end = APP_REGION + 4 + len(old.packed) + 2
    packed = compress_lzss(app)
    region = len(packed).to_bytes(4, "little") + packed
    region += (sum(region) & 0xFFFF).to_bytes(2, "little")
    new_end = APP_REGION + len(region)
    if new_end > APP_REGION_END:
        raise ValueError(f"packed application ends at 0x{new_end:x}, past the "
                         f"application area's 0x{APP_REGION_END:x}")
    image = bytearray(base)
    if len(image) < new_end:
        image.extend(b"\xff" * (new_end - len(image)))
    image[APP_REGION:new_end] = region
    if old_end > new_end:
        image[new_end:old_end] = b"\xff" * (old_end - new_end)

    check = unpack_region(bytes(image), APP_REGION)
    if not check.checksum_valid or check.unpacked != app:
        raise AssertionError("the packed region does not decode back to the application")
    if decompress_lzss(check.packed) != app:
        raise AssertionError("round trip through main_unpack failed")
    return bytes(image), new_end


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("app", type=Path, help="application image, e.g. an edited main-unpacked.bin")
    parser.add_argument("out", type=Path, help="flash image to write (boot it with --firmware)")
    parser.add_argument("--base", type=Path, default=FIRMWARE / "main-firmware.bin",
                        help="flash image that supplies the boot ROM and loader "
                             "(default firmware/main-firmware.bin)")
    parser.add_argument("--upd", type=Path, help="also write a C2KMAIN.UPD for the updater")
    parser.add_argument("--version", default="4.34",
                        help="header version for --upd; the updater only takes a newer one")
    args = parser.parse_args(argv)

    app = args.app.read_bytes()
    image, end = repack(args.base.read_bytes(), app)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)
    print(f"{args.out}: application 0x{len(app):x} -> packed region ends at 0x{end:x}, "
          f"sha256 {hashlib.sha256(image).hexdigest()}")
    if args.upd:
        upd_end = -(-end // make_upd.RECORD_BYTES) * make_upd.RECORD_BYTES
        data = make_upd.build(image, args.version, max(upd_end, make_upd.DEFAULT_END))
        args.upd.parent.mkdir(parents=True, exist_ok=True)
        args.upd.write_bytes(data)
        print(f"{args.upd}: version {args.version}, {len(data)} bytes, "
              f"crc {data[-2:][::-1].hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
