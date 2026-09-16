"""The literal-pool cross-reference scan, on a synthetic image.

SH4 cannot name an absolute address in an instruction, so every reference is a
`mov.l @(disp,PC),Rn` against a pool entry.  That decode is the whole tool.
"""
import struct

from tools.cdj_main import xrefs


def image(pairs):
    blob = bytearray(b"\x00" * 0x40)
    for offset, halfword in pairs:
        struct.pack_into("<H", blob, offset, halfword)
    return bytes(blob)


def test_finds_the_load_and_the_call():
    # at 0x10: mov.l @(2,pc),r5 -> ((0x10+4)&~3)+8 = 0x1c ; at 0x12: jsr @r5
    blob = bytearray(image([(0x10, 0xD502), (0x12, 0x450B)]))
    struct.pack_into("<I", blob, 0x1C, 0x042A033C)
    blob = bytes(blob)

    assert xrefs.literal_offsets(blob, 0x042A033C) == [0x1C]
    assert xrefs.loads_of(blob, 0x1C) == [(0x10, 5)]
    assert "jsr @r5" in xrefs.user_of(blob, 0x10, 5)


def test_ignores_a_load_aimed_elsewhere():
    blob = bytearray(image([(0x10, 0xD503)]))       # displacement 3 -> 0x20
    struct.pack_into("<I", blob, 0x1C, 0x042A033C)
    assert xrefs.loads_of(bytes(blob), 0x1C) == []
