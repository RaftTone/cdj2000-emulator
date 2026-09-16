"""Find cross-references to an address in the unpacked MAIN image.

    python -m tools.cdj_main.xrefs 0x042a033c

SH4 has no absolute call or absolute load: reaching 0x042a033c or 0x04cf222c
means `mov.l @(disp,PC),Rn` against a literal pool entry holding that value.
So every xref is a literal-pool hit plus the PC-relative load that reads it,
which is what this scans for.  That is the whole bottleneck NXS_SD_READINESS.md
names - QEMU's `xp /Ni` is linear and cannot answer "who calls this".

Reports the loading instruction, the register loaded, and - when the value is
code - the jsr/jmp/braf on that register that follows within a few insns.

These are candidates, not proof: the scan cannot tell code from data, so a data
word shaped like `0xDnnn` whose displacement happens to land on the pool is
reported too.  Disassemble what it names before believing it.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "firmware/nxs/main-unpacked.bin"
BASE = 0x04000000


def literal_offsets(blob: bytes, value: int) -> list[int]:
    needle = struct.pack("<I", value)
    out, start = [], 0
    while True:
        hit = blob.find(needle, start)
        if hit < 0:
            return out
        if hit % 4 == 0:                     # a pool entry is word-aligned
            out.append(hit)
        start = hit + 1


def loads_of(blob: bytes, pool: int) -> list[tuple[int, int]]:
    """`mov.l @(disp,PC),Rn` = 0xDnnn, target ((pc+4)&~3)+disp*4, disp<=255."""
    found = []
    for off in range(max(0, pool - 1024), pool, 2):
        word = struct.unpack_from("<H", blob, off)[0]
        if word >> 12 != 0xD:
            continue
        if (((off + 4) & ~3) + (word & 0xFF) * 4) == pool:
            found.append((off, (word >> 8) & 0xF))
    return found


def user_of(blob: bytes, off: int, reg: int, span: int = 24) -> str:
    """jsr @Rn = 0x4n0b, jmp @Rn = 0x4n2b, braf Rn = 0x0n23, bsrf = 0x0n03."""
    kinds = {0x400B: "jsr", 0x402B: "jmp", 0x0023: "braf", 0x0003: "bsrf"}
    for at in range(off + 2, off + 2 + span, 2):
        word = struct.unpack_from("<H", blob, at)[0]
        kind = kinds.get(word & 0xF0FF)
        if kind and (word >> 8) & 0xF == reg:
            return "%s @r%d at %#010x" % (kind, reg, BASE + at)
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("value", type=lambda s: int(s, 0))
    parser.add_argument("--image", type=Path, default=IMAGE)
    args = parser.parse_args()

    blob = args.image.read_bytes()
    pools = literal_offsets(blob, args.value)
    print("%#010x: %d literal-pool entries in %s"
          % (args.value, len(pools), args.image.name))
    for pool in pools:
        loads = loads_of(blob, pool)
        print("  pool %#010x  (%d loads)" % (BASE + pool, len(loads)))
        for off, reg in loads:
            print("    %#010x  mov.l @(pc),r%-2d %s"
                  % (BASE + off, reg, user_of(blob, off, reg)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
