"""Build a FAT32 SD-card image from a directory tree.

QEMU's `sd-card` insists on a power-of-two image size, which rules out its own
vvfat driver (`fat:<dir>` synthesised a 504 MiB disk and was rejected), and this
tree has no `mkfs.fat`.  So build the filesystem directly.

    python -m tools.cdj_main.make_sd_image <source-dir> <image> [--size 512M]

The layout is what a card formatted by rekordbox looks like from the driver's
side: an MBR with one type-0x0C partition at LBA 2048, then FAT32 with 4 KiB
clusters.  MAIN reads sector 0 first (CMD17 with argument 0), so the MBR is not
optional.

Every name that is not a plain upper-case 8.3 name gets a long-name entry set
in front of its short entry (`lfn_entries`), with the short name made unique per
directory by a `~N` tail.  That is what a rekordbox stick looks like and what
MAIN needs: `PIONEER/rekordbox/` itself is nine characters, and the audio files
a rekordbox export references keep their long names.  (An earlier version of
this text claimed only 8.3 names were written; the code never did that.)
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import mmap
import struct
import sys
from pathlib import Path

SECTOR = 512
CLUSTER_SECTORS = 8
CLUSTER = SECTOR * CLUSTER_SECTORS
RESERVED = 32
PART_LBA = 2048


def parse_size(text: str) -> int:
    text = text.strip().upper()
    scale = 1
    if text.endswith("M"):
        scale, text = 1 << 20, text[:-1]
    elif text.endswith("G"):
        scale, text = 1 << 30, text[:-1]
    value = int(text) * scale
    if value & (value - 1):
        raise SystemExit("size must be a power of two: %s" % text)
    return value


BAD = ' +,;=[]"*?<>|:/\\'


def fits_8_3(name: str) -> bool:
    stem, _, ext = name.rpartition(".")
    if not stem:
        stem, ext = name, ""
    return (len(stem) <= 8 and len(ext) <= 3 and name == name.upper()
            and not any(c in name for c in BAD))


def short_name(name: str, taken: set[bytes]) -> bytes:
    """Return an 11-byte 8.3 field, tilde-mangled if the name does not fit.

    `PIONEER/rekordbox/` is the case that forces this: nine characters, so a
    real card carries it as a long name with `REKORD~1` behind it, and dropping
    it takes `export.pdb` with it.
    """
    stem, _, ext = name.rpartition(".")
    if not stem:
        stem, ext = name, ""
    # A short name is ASCII: `RÜFÜS` or a Cyrillic title keeps its real name
    # in the long-name entries only, and the 8.3 field gets `_` in its place,
    # as Windows does.
    stem = "".join(c if c.isascii() else "_" for c in stem.upper() if c not in BAD)
    ext = "".join(c if c.isascii() else "_" for c in ext.upper() if c not in BAD)[:3]
    # Compare against the upper-cased original: 8.3 is case-insensitive and is
    # stored upper-case, so `export.pdb` is a perfectly good short name.
    # Comparing against `name` itself only ever matched already-upper-case
    # names, which gave every lower-case one a `~1` tail it did not need --
    # and `EXPORT~1PDB` is not what a FAT layer that compares 8.3 names goes
    # looking for when it is asked for `export.pdb`.
    if len(stem) <= 8 and (stem + "." + ext if ext else stem) == name.upper():
        candidate = (stem.ljust(8) + ext.ljust(3)).encode("ascii")
        if candidate not in taken:
            taken.add(candidate)
            return candidate
    for n in range(1, 1000):
        suffix = "~%d" % n
        candidate = ((stem[:8 - len(suffix)] + suffix).ljust(8)
                     + ext.ljust(3)).encode("ascii")
        if candidate not in taken:
            taken.add(candidate)
            return candidate
    raise SystemExit("cannot make a unique short name for %s" % name)


def lfn_checksum(short: bytes) -> int:
    total = 0
    for byte in short:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def lfn_entries(name: str, short: bytes) -> list[bytes]:
    """The long-name entries that must precede the short one, last part first."""
    checksum = lfn_checksum(short)
    encoded = name.encode("utf-16-le") + b"\0\0"
    parts = [encoded[i:i + 26] for i in range(0, len(encoded), 26)]
    parts[-1] = parts[-1].ljust(26, b"\xff")
    out = []
    for index, part in enumerate(parts, start=1):
        order = index | (0x40 if index == len(parts) else 0)
        out.append(bytes([order]) + part[0:10] + bytes([0x0F, 0, checksum])
                   + part[10:22] + b"\0\0" + part[22:26])
    return list(reversed(out))


class Builder:
    def __init__(self, total_bytes: int, path: Path | None = None):
        self.total_sectors = total_bytes // SECTOR
        self.part_sectors = self.total_sectors - PART_LBA
        clusters = self.part_sectors // CLUSTER_SECTORS
        # Each FAT copy must hold one 32-bit entry per cluster.
        self.fat_sectors = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        self.data_start = RESERVED + 2 * self.fat_sectors
        self.max_cluster = (self.part_sectors - self.data_start) // CLUSTER_SECTORS
        if path is None:
            self.image = bytearray(total_bytes)
        else:
            # Written in place through a mapping of a sparse file: a 4 GiB
            # card held as a bytearray, and then copied by bytes() to write
            # it out, costs twice its size in memory.
            with open(path, "wb") as created:
                created.truncate(total_bytes)
            self._file = open(path, "r+b")
            self.image = mmap.mmap(self._file.fileno(), total_bytes)
        self.fat = [0] * (self.max_cluster + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
        self.next_cluster = 2

    def alloc(self, count: int) -> list[int]:
        if self.next_cluster + count > self.max_cluster + 2:
            raise SystemExit("image too small for the tree")
        chain = list(range(self.next_cluster, self.next_cluster + count))
        self.next_cluster += count
        for a, b in zip(chain, chain[1:]):
            self.fat[a] = b
        self.fat[chain[-1]] = 0x0FFFFFFF
        return chain

    def cluster_offset(self, cluster: int) -> int:
        sector = PART_LBA + self.data_start + (cluster - 2) * CLUSTER_SECTORS
        return sector * SECTOR

    def write_chain(self, chain: list[int], data: bytes) -> None:
        for i, cluster in enumerate(chain):
            piece = data[i * CLUSTER:(i + 1) * CLUSTER]
            start = self.cluster_offset(cluster)
            self.image[start:start + len(piece)] = piece

    @staticmethod
    def entry(name: bytes, attr: int, cluster: int, size: int) -> bytes:
        return (name + bytes([attr]) + b"\0" * 8
                + struct.pack("<HHHH", cluster >> 16, 0, 0, cluster & 0xFFFF)
                + struct.pack("<I", size))

    def add_dir(self, source: Path, cluster: int, parent: int,
                is_root: bool = False) -> None:
        """Fill the directory whose first cluster is CLUSTER from SOURCE.

        The FAT32 root has no "." or ".." entries; every other directory must
        have both, and ".." points at 0 when the parent is the root.
        """
        entries = [] if is_root else [
            self.entry(b".          ", 0x10, cluster, 0),
            self.entry(b"..         ", 0x10, parent, 0)]
        children: list[tuple[Path, int]] = []
        taken: set[bytes] = set()
        for child in sorted(source.iterdir()):
            name = short_name(child.name, taken)
            if not fits_8_3(child.name):
                entries.extend(lfn_entries(child.name, name))
            if child.is_dir():
                sub = self.alloc(1)[0]
                entries.append(self.entry(name, 0x10, sub, 0))
                children.append((child, sub))
            else:
                data = child.read_bytes()
                count = max(1, (len(data) + CLUSTER - 1) // CLUSTER)
                chain = self.alloc(count)
                self.write_chain(chain, data)
                entries.append(self.entry(name, 0x20, chain[0], len(data)))
        blob = b"".join(entries)

        # A directory is a cluster chain like any other file, and USBANLZ has
        # directories with more entries than one 4 KiB cluster holds.
        extra = (len(blob) + CLUSTER - 1) // CLUSTER - 1
        chain = [cluster]
        if extra > 0:
            tail = self.alloc(extra)
            self.fat[cluster] = tail[0]
            chain += tail
        self.write_chain(chain, blob.ljust(len(chain) * CLUSTER, b"\0"))

        for child, sub in children:
            self.add_dir(child, sub, 0 if is_root else cluster)

    def finish(self, label: str = "CDJ2000") -> None:
        # MBR: one FAT32-LBA partition.
        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack("<BBBBBBBBII", 0x80, 0, 1, 0, 0x0C,
                                   0xFE, 0xFF, 0xFF, PART_LBA,
                                   self.part_sectors)
        mbr[510:512] = b"\x55\xaa"
        self.image[0:SECTOR] = mbr

        boot = bytearray(SECTOR)
        boot[0:3] = b"\xeb\x58\x90"
        boot[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, CLUSTER_SECTORS,
                         RESERVED, 2, 0, 0, 0xF8, 0, 63, 255, PART_LBA,
                         self.part_sectors)
        struct.pack_into("<IHHIHH", boot, 36, self.fat_sectors, 0, 0, 2, 1, 6)
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into("<I", boot, 67, 0xCD720000)
        boot[71:82] = label.ljust(11).encode("ascii")
        boot[82:90] = b"FAT32   "
        boot[510:512] = b"\x55\xaa"
        base = PART_LBA * SECTOR
        self.image[base:base + SECTOR] = boot
        self.image[base + 6 * SECTOR:base + 7 * SECTOR] = boot

        # A free count of 0xFFFFFFFF means "unknown", and a driver that is
        # asked for free space then counts it by reading every FAT sector.
        # These images have a 1,023-sector FAT, and a CDJ traced mounting one
        # walks all of it before it touches a single data sector -- minutes of
        # emulated time, during which a SOURCE key gets one-row answers the GUI
        # abandons. The builder knows the real number, so it writes it.
        fsinfo = bytearray(SECTOR)
        fsinfo[0:4] = b"RRaA"
        fsinfo[484:488] = b"rrAa"
        free = self.max_cluster - (self.next_cluster - 2)
        struct.pack_into("<II", fsinfo, 488, free, self.next_cluster)
        fsinfo[510:512] = b"\x55\xaa"
        self.image[base + SECTOR:base + 2 * SECTOR] = fsinfo

        table = b"".join(struct.pack("<I", e) for e in self.fat)
        table += b"\0" * (self.fat_sectors * SECTOR - len(table))
        for copy in range(2):
            start = base + (RESERVED + copy * self.fat_sectors) * SECTOR
            self.image[start:start + len(table)] = table


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("--size", default="512M")
    args = parser.parse_args()

    builder = Builder(parse_size(args.size), args.image)
    root = builder.alloc(1)[0]
    if root != 2:
        raise SystemExit("root cluster must be 2")
    builder.add_dir(args.source, root, 0, is_root=True)
    builder.finish()
    builder.image.flush()

    print("%s: %d bytes, %d clusters used of %d"
          % (args.image, len(builder.image), builder.next_cluster - 2,
             builder.max_cluster))
    return 0


if __name__ == "__main__":
    sys.exit(main())
