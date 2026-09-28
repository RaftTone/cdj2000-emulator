"""Put one file into a FAT32 card image, or take one out.

    python -m tools.cdj_main.card_put IMAGE CARD_PATH LOCAL_FILE [--out COPY]
    python -m tools.cdj_main.card_put IMAGE CARD_PATH --get LOCAL_FILE
    python -m tools.cdj_main.card_put IMAGE [DIR] --ls

For synthetic test tracks: an ANLZ file with hot cues D..H, memory loops or
coloured loops, dropped in place of the one rekordbox wrote, e.g.

    ./cdj card_put card.img PIONEER/USBANLZ/P04A/0001519E/ANLZ0000.EXT local.EXT

Patching is --get, an edit, then a put. CARD_PATH components match a long or a
short name, case-insensitively. A file that does not exist yet is created with
long-name entries, as make_sd_image writes them, and so are missing
directories. The old clusters of a replaced file are freed.

The image is changed in place unless --out names a copy. Never run it on an
image a running machine has open: QEMU's SD card caches sectors.

Works on any FAT32 volume, not only make_sd_image's: the layout comes from the
boot sector, behind an MBR partition or at sector 0. FSInfo gets the real free
count, because an "unknown" one makes MAIN read the whole FAT at mount
(make_sd_image.Builder.finish).
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import array
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from .make_sd_image import fits_8_3, lfn_checksum, lfn_entries, short_name

END = 0x0FFFFFF8          # this and above: end of chain
MASK = 0x0FFFFFFF         # the top four bits of a FAT32 entry are reserved
ENTRY = 32


class Fat32:
    def __init__(self, path: Path):
        self.file = open(path, "r+b")
        sector0 = self.read(0, 512)
        self.base = 0
        if sector0[82:90] != b"FAT32   ":
            for slot in range(4):
                entry = sector0[446 + 16 * slot:462 + 16 * slot]
                if entry[4] in (0x0B, 0x0C):
                    self.base = struct.unpack_from("<I", entry, 8)[0] * 512
                    break
        boot = self.read(self.base, 512)
        if boot[82:90] != b"FAT32   " or boot[510:512] != b"\x55\xaa":
            raise SystemExit("%s: no FAT32 volume found" % path)
        (self.sector, spc, reserved, fats) = struct.unpack_from("<HBHB", boot, 11)
        total = struct.unpack_from("<I", boot, 32)[0]
        fat_sectors, _, _, self.root, fsinfo = struct.unpack_from("<IHHIH", boot, 36)
        self.cluster = self.sector * spc
        self.fat_offsets = [self.base + (reserved + i * fat_sectors) * self.sector
                            for i in range(fats)]
        self.data = self.base + (reserved + fats * fat_sectors) * self.sector
        self.clusters = (total - reserved - fats * fat_sectors) // spc
        self.fsinfo = self.base + fsinfo * self.sector
        self.fat = array.array("I")
        self.fat.frombytes(self.read(self.fat_offsets[0], (self.clusters + 2) * 4))
        if sys.byteorder != "little":
            self.fat.byteswap()
        self.hint = 2

    def close(self) -> None:
        self.file.close()

    def __enter__(self) -> "Fat32":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- raw access -------------------------------------------------------

    def read(self, offset: int, size: int) -> bytes:
        self.file.seek(offset)
        return self.file.read(size)

    def write(self, offset: int, data: bytes) -> None:
        self.file.seek(offset)
        self.file.write(data)

    def offset(self, cluster: int) -> int:
        return self.data + (cluster - 2) * self.cluster

    def next(self, cluster: int) -> int:
        return self.fat[cluster] & MASK

    def set(self, cluster: int, value: int) -> None:
        self.fat[cluster] = (self.fat[cluster] & ~MASK & 0xFFFFFFFF) | value

    def chain(self, first: int) -> list[int]:
        out = []
        cluster = first
        while 2 <= cluster < END:
            if cluster >= self.clusters + 2 or len(out) > self.clusters:
                raise SystemExit("broken cluster chain from %d" % first)
            out.append(cluster)
            cluster = self.next(cluster)
        return out

    def read_chain(self, first: int, size: int | None = None) -> bytes:
        data = b"".join(self.read(self.offset(c), self.cluster) for c in self.chain(first))
        return data if size is None else data[:size]

    def write_chain(self, chain: list[int], data: bytes) -> None:
        data = data.ljust(len(chain) * self.cluster, b"\0")
        for i, cluster in enumerate(chain):
            self.write(self.offset(cluster), data[i * self.cluster:(i + 1) * self.cluster])

    def alloc(self, count: int) -> list[int]:
        chain = []
        cluster = self.hint
        for _ in range(self.clusters):
            if len(chain) == count:
                break
            if self.next(cluster) == 0:
                chain.append(cluster)
            cluster = cluster + 1 if cluster + 1 < self.clusters + 2 else 2
        if len(chain) < count:
            raise SystemExit("the card is full")
        for a, b in zip(chain, chain[1:]):
            self.set(a, b)
        if chain:
            self.set(chain[-1], MASK)
            self.hint = chain[-1] + 1 if chain[-1] + 1 < self.clusters + 2 else 2
        return chain

    def free(self, first: int) -> None:
        for cluster in self.chain(first):
            self.set(cluster, 0)

    def flush(self) -> None:
        table = self.fat
        if sys.byteorder != "little":
            table = array.array("I", table)
            table.byteswap()
        blob = table.tobytes()
        for offset in self.fat_offsets:
            self.write(offset, blob)
        info = bytearray(self.read(self.fsinfo, 512))
        if info[0:4] == b"RRaA" and info[484:488] == b"rrAa":
            free = sum(1 for c in range(2, self.clusters + 2) if self.next(c) == 0)
            struct.pack_into("<II", info, 488, free, self.hint)
            self.write(self.fsinfo, bytes(info))
        self.file.flush()

    # -- directories ------------------------------------------------------

    def listing(self, first: int) -> tuple[list[int], bytearray, list[dict]]:
        """The directory's chain, its bytes, and one dict per live entry."""
        chain = self.chain(first)
        blob = bytearray(self.read_chain(first))
        entries, parts = [], []
        for slot in range(len(blob) // ENTRY):
            raw = blob[slot * ENTRY:(slot + 1) * ENTRY]
            if raw[0] == 0:
                break
            if raw[0] == 0xE5:
                parts = []
                continue
            if raw[11] == 0x0F:
                parts.append((slot, raw))
                continue
            short = bytes(raw[0:11])
            if short[0] == 0x05:
                short = b"\xe5" + short[1:]
            long_name = None
            if parts and all(p[13] == lfn_checksum(bytes(raw[0:11])) for _, p in parts):
                text = b"".join(p[1:11] + p[14:26] + p[28:32] for _, p in reversed(parts))
                long_name = text.decode("utf-16-le", "replace").split("\0")[0]
            stem, ext = short[:8].decode("latin-1").rstrip(), short[8:].decode("latin-1").rstrip()
            entries.append({
                "slot": slot, "first_slot": parts[0][0] if long_name else slot,
                "short": short, "name": long_name or (stem + "." + ext if ext else stem),
                "short_text": stem + "." + ext if ext else stem, "attr": raw[11],
                "cluster": struct.unpack_from("<H", raw, 20)[0] << 16
                | struct.unpack_from("<H", raw, 26)[0],
                "size": struct.unpack_from("<I", raw, 28)[0]})
            parts = []
        return chain, blob, entries

    @staticmethod
    def find(entries: list[dict], name: str) -> dict | None:
        want = name.upper()
        for entry in entries:
            if entry["short"][0:1] == b"." or entry["attr"] & 0x08:
                continue
            if entry["name"].upper() == want or entry["short_text"].upper() == want:
                return entry
        return None

    def add_entry(self, directory: int, name: str, attr: int, cluster: int,
                  size: int) -> None:
        chain, blob, entries = self.listing(directory)
        taken = {entry["short"] for entry in entries}
        short = short_name(name, taken)
        new = [] if fits_8_3(name) else lfn_entries(name, short)
        new.append(short + bytes([attr]) + b"\0" * 8
                   + struct.pack("<HHHH", cluster >> 16, 0, 0, cluster & 0xFFFF)
                   + struct.pack("<I", size))
        need = len(new)
        slots = len(blob) // ENTRY
        run, start, at_end = 0, None, False
        for slot in range(slots):
            first = blob[slot * ENTRY]
            if first == 0:
                # Everything from the end marker on is free.
                if slots - slot >= need:
                    start, at_end = slot, True
                break
            run = run + 1 if first == 0xE5 else 0
            if run == need:
                start = slot - need + 1
                break
        if start is None:
            # Past the end marker; grow the directory by whole zeroed clusters.
            start = next((s for s in range(slots) if blob[s * ENTRY] == 0), slots)
            grow = (start + need + 1) * ENTRY - len(blob)
            extra = self.alloc((grow + self.cluster - 1) // self.cluster)
            self.set(chain[-1], extra[0])
            chain += extra
            blob += b"\0" * (len(extra) * self.cluster)
            at_end = True
        for i, raw in enumerate(new):
            blob[(start + i) * ENTRY:(start + i + 1) * ENTRY] = raw
        end = (start + need) * ENTRY
        if at_end and end < len(blob):
            # Slots past the old end marker may hold stale bytes; keep the
            # directory ending right after the new entries.
            blob[end] = 0
        self.write_chain(chain, bytes(blob))

    def set_entry(self, directory: int, slot: int, cluster: int, size: int) -> None:
        chain, blob, _ = self.listing(directory)
        struct.pack_into("<H", blob, slot * ENTRY + 20, cluster >> 16)
        struct.pack_into("<H", blob, slot * ENTRY + 26, cluster & 0xFFFF)
        struct.pack_into("<I", blob, slot * ENTRY + 28, size)
        self.write_chain(chain, bytes(blob))

    def mkdir(self, parent: int, name: str) -> int:
        cluster = self.alloc(1)[0]
        dotdot = 0 if parent == self.root else parent
        dots = b"".join(
            short + bytes([0x10]) + b"\0" * 8
            + struct.pack("<HHHH", target >> 16, 0, 0, target & 0xFFFF) + b"\0" * 4
            for short, target in ((b".          ", cluster), (b"..         ", dotdot)))
        self.write_chain([cluster], dots)
        self.add_entry(parent, name, 0x10, cluster, 0)
        return cluster

    def walk(self, parts: list[str], create: bool = False) -> int:
        """The first cluster of the directory PARTS name."""
        cluster = self.root
        for part in parts:
            entry = self.find(self.listing(cluster)[2], part)
            if entry is None:
                if not create:
                    raise SystemExit("no directory %s on the card" % part)
                cluster = self.mkdir(cluster, part)
            elif not entry["attr"] & 0x10:
                raise SystemExit("%s is a file, not a directory" % part)
            else:
                cluster = entry["cluster"] or self.root
        return cluster

    # -- files ------------------------------------------------------------

    def get(self, path: str) -> bytes:
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        entry = self.find(self.listing(self.walk(parts[:-1]))[2], parts[-1])
        if entry is None or entry["attr"] & 0x10:
            raise SystemExit("no file %s on the card" % path)
        return self.read_chain(entry["cluster"], entry["size"]) if entry["cluster"] else b""

    def put(self, path: str, data: bytes) -> str:
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        directory = self.walk(parts[:-1], create=True)
        entry = self.find(self.listing(directory)[2], parts[-1])
        count = (len(data) + self.cluster - 1) // self.cluster
        if entry is not None:
            if entry["attr"] & 0x10:
                raise SystemExit("%s is a directory" % path)
            if entry["cluster"]:
                self.free(entry["cluster"])
        chain = self.alloc(count)
        self.write_chain(chain, data)
        first = chain[0] if chain else 0
        if entry is None:
            self.add_entry(directory, parts[-1], 0x20, first, len(data))
            verb = "created"
        else:
            self.set_entry(directory, entry["slot"], first, len(data))
            verb = "replaced (%d bytes before)" % entry["size"]
        self.flush()
        return verb


def copy_image(source: Path, target: Path) -> None:
    """A clone on APFS (cp -c), so a sparse 4 GiB card copy costs nothing
    until written; a plain copy elsewhere."""
    if sys.platform == "darwin":
        done = subprocess.run(["cp", "-c", str(source), str(target)],
                              capture_output=True)
        if done.returncode == 0:
            return
    shutil.copyfile(source, target)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("image", type=Path)
    parser.add_argument("card_path", nargs="?", default="")
    parser.add_argument("local", type=Path, nargs="?", help="the file to put")
    parser.add_argument("--out", type=Path, help="write a copy of the image instead")
    parser.add_argument("--get", type=Path, metavar="LOCAL", help="take the file out")
    parser.add_argument("--ls", action="store_true", help="list the directory")
    args = parser.parse_args(argv)

    image = args.image
    if args.out:
        if args.get or args.ls:
            parser.error("--out is for a put")
        copy_image(image, args.out)
        image = args.out
    with Fat32(image) as card:
        return run(card, image, args, parser)


def run(card: Fat32, image: Path, args, parser) -> int:
    if args.ls:
        parts = [p for p in args.card_path.replace("\\", "/").split("/") if p]
        for entry in card.listing(card.walk(parts))[2]:
            if entry["short"][0:1] == b".":
                continue
            kind = "<dir>" if entry["attr"] & 0x10 else "%d" % entry["size"]
            print("%12s  %-12s %s" % (kind, entry["short_text"], entry["name"]))
        return 0
    if not args.card_path:
        parser.error("CARD_PATH is required")
    if args.get:
        args.get.write_bytes(card.get(args.card_path))
        print("%s: %s -> %s" % (image, args.card_path, args.get))
        return 0
    if args.local is None:
        parser.error("give LOCAL_FILE to put, or --get / --ls")
    data = args.local.read_bytes()
    verb = card.put(args.card_path, data)
    print("%s: %s %s, %d bytes" % (image, args.card_path, verb, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
