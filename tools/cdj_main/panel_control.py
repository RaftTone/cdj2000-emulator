"""Speak to the panel of a running CDJ-2000.

    python -m tools.cdj_main.panel_control press sd
    python -m tools.cdj_main.panel_control rotary 4 +8
    python -m tools.cdj_main.panel_control state

`CDJ_PANEL_KEYS` presses buttons on a schedule fixed before the machine boots,
at most sixteen of them.  This is the other kind of input: a line-oriented TCP
channel into `emulator/qemu/cdj2000_input.c`, which merges what it is told into
the panel payload just before the checksum is taken.  QEMU opens the channel
only when `CDJ_INPUT_PORT` names a port, so a run without that variable is a
control run in the strict sense -- nothing binds and nothing is merged.

Everything here is host-side and testable without an emulator: the frame
builder and its checksum are a model of the rule in `cdj2000_main.c`, and the
wire protocol is a pure string encoding.  `tests/test_panel_control.py` checks
both, and also checks this file against the C it talks to.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import os
import socket
import sys
import time
from typing import NamedTuple

# ---------------------------------------------------------------- geometry --
#
# The panel frame is 24 bytes: 22 payload bytes, a carry-folded sum, and the
# 0x8f marker that 0x28cdf8 insists on.
PANEL_FRAME_LEN = 24
PANEL_PAYLOAD_LEN = 22
PANEL_FRAME_MARK = 0x8F

# Payload bytes 2..13 as the decoder at 0x28e1d6 splits them: two 8-bit fields,
# then five 16-bit big-endian pairs, landing at 0x04fe2a20 + 0, +4, +8, +12,
# +16, +20, +24.
#
# **Field 7 is payload byte 14, and it is the encoder.**  The same decoder reads
# it (`mov.b @(14,r1)` at 0x28e26e), adds the sign-extended halfword at
# 0x04fe2af8 to it and stores the sum at 0x04fe2a44 -- and MAIN's own 66-arm
# panel simulator at 0x1010a4 devotes arms 64 and 65 to nothing but `+1` and
# `-1` on that halfword, which is what an encoder looks like and what no other
# arm does.  A sweep over fields 0..6 could therefore only ever have returned
# zeros: the encoder was outside the range the channel could reach.
#
# Bytes 15..17 carry further switch bits, spread into the status block by the
# same function.  They are buttons and are deliberately not here.
#
# Keep in step with the table of the same name in
# emulator/qemu/cdj2000_input.c -- a test compares the two.
ANALOG_FIELDS: list[tuple[int, int]] = [
    (2, 1), (3, 1), (4, 2), (6, 2), (8, 2), (10, 2), (12, 2), (14, 1),
]

# Which of them is the select encoder, named once instead of spelled 7 in three
# places.  Two independent readings agree: the decoder at 0x28e26e reads
# payload byte 14, adds the signed halfword at 0x04fe2af8 and stores the sum at
# 0x04fe2a44; and arms 64/65 of MAIN's own panel simulator at 0x1010a4 do
# nothing but +1 and -1 on that halfword, which no other arm does.
ENCODER_FIELD = 7

# **Field 6 carries two inputs, not one**, and the second has never been driven.
# Read out of the image rather than out of this file's prose (`sh_index fn
# 0x28e1ae`), the pair is tested *before* it is masked:
#
#     0028e1d8  mov.l 0028e260,r2      r2 = 0x8000
#     0028e21c  ... or r4,r5           r5 = byte12 << 8 | byte13
#     0028e230  tst   r2,r5
#     0028e232  bf    0028e26c
#     0028e234  bra   0028e26e
#     0028e236  and.b #251,@(r0,gbr)   r0 = 0x04fe2a3c, gbr = 0: clear bit 2
#     0028e26c  or.b  #4,@(r0,gbr)     the other arm: set bit 2
#     0028e27a  add   #-1,r4           r4 = 0x1ff
#     0028e27e  and   r4,r5
#     0028e288  mov.l r5,@(24,r10)     0x04fe2a38, the 0..511 position
#
# So bit 15 is a flag with a destination of its own and the position is nine
# bits wide.  `rotary` cannot reach the flag: it walks one count per panel
# exchange, so 0x8000 is 32 768 exchanges from rest, and every run in
# INPUT_MANIFEST.md drove this field with `rotary`.  `analog 6 <value>` is the
# only verb that can set it, and nothing has ever sent one.
ANALOG_TOUCH_FIELD = 6
ANALOG_TOUCH_MASK = 0x8000
ANALOG_POSITION_MASK = 0x01FF


class AnalogControl(NamedTuple):
    """What kind of control an analogue field is, so a window can offer one.

    A field is not a button and a spinbox naming a number is not a control: a
    jog wheel, a fader and a detented encoder want different widgets and have
    different ends.  `kind` is what to build, `low`/`high` are the ends, `step`
    is one detent of the +/- pair, and `evidence` is why -- never a guess from
    where a control sits on the front panel.
    """

    field: int
    label: str
    kind: str           # "encoder" | "position" | "level"
    low: int
    high: int
    step: int
    touch_mask: int     # 0, or the bit that carries a touch flag
    evidence: str


# The eight, each with the reading that fixes its shape.  Two sources and no
# third: the decoder at 0x28e1ae (what the firmware does with the bytes) and
# MAIN's own 66-arm panel simulator at 0x1010a4 (what the firmware thinks each
# control *is*, arm by arm).  Where neither names a span or a detent the entry
# says so rather than inventing one -- an unnamed field still gets a control,
# it just gets a plain one.
ANALOG_CONTROLS: list[AnalogControl] = [
    AnalogControl(0, "field 0 - byte 2", "level", 0, 255, 1, 0,
                  "8-bit level -> 0x04fe2a20 (0x28e1dc); simulator arms 51..56 "
                  "set fields 0..2 to 0 or 255, so the ends are measured and "
                  "the detent is not"),
    AnalogControl(1, "field 1 - byte 3", "level", 0, 255, 1, 0,
                  "8-bit level -> +4 (0x28e1e0); simulator arms 51..56 set it "
                  "to 0 or 255"),
    AnalogControl(2, "field 2 - bytes 4/5", "level", 0, 0xFFFF, 1, 0,
                  "16-bit BE -> +8 (0x28e1e6); simulator arms 51..56 set it to "
                  "0 or 255, which is a use rather than a range"),
    AnalogControl(3, "field 3 - bytes 6/7", "level", 0, 0xFFFF, 1, 0,
                  "16-bit BE -> +12 (0x28e1f2); no arm of the panel simulator "
                  "names it, so no span and no detent are claimed"),
    AnalogControl(4, "field 4 - bytes 8/9", "level", 0, 0xFFFF, 200, 0,
                  "16-bit BE -> +16 (0x28e200); simulator arms 59/60 move it "
                  "by +/-200 with field 5 pinned at 300"),
    AnalogControl(5, "field 5 - bytes 10/11", "level", 0, 0xFFFF, 1, 0,
                  "16-bit BE -> +20 (0x28e20e); arms 59/60 pin it at 300 while "
                  "they move field 4"),
    AnalogControl(6, "field 6 - bytes 12/13", "position", 0, 511, 10,
                  ANALOG_TOUCH_MASK,
                  "masked 0x1ff -> +24 at 0x28e288, and bit 15 -> the flag at "
                  "0x04fe2a3c bit 2 (0x28e236 / 0x28e26c); arms 62/63 step it "
                  "by +/-10 within [0, 390]"),
    AnalogControl(7, "field 7 - byte 14  SELECT encoder", "encoder", 0, 255, 1,
                  0,
                  "byte 14 + the halfword at 0x04fe2af8 -> 0x04fe2a44 "
                  "(0x28e26e); arms 64 and 65 do nothing but +1 and -1 on that "
                  "halfword, which no other arm does"),
]


# Where each analogue field lands in MAIN's status block -- the address a watch
# has to be pointed at to prove the field *arrived*, as opposed to proving the
# host sent it.
#
# **Read out of the same decoder as the table above**, one store instruction per
# row, so this is not a second opinion about the panel but the same one written
# as addresses.  0x28e1d6 splits payload bytes 2..13 into 0x04fe2a20 + 0, +4,
# +8, +12, +16, +20, +24, and 0x28e26e stores byte 14 plus the halfword at
# 0x04fe2af8 to 0x04fe2a44.
#
# **Two properties make this a usable arrival proof, and both are arithmetic
# rather than probability.**  First, every destination is *absolute*: it holds
# the field's value, so its change across a window equals the change the
# transcript asked for -- `rotary 4 200` moves 0x04fe2a30 by exactly +200, and
# `rotary 7 12` moves 0x04fe2a44 by exactly +12 (r166 read 16 = +4+4+4+4-4-4+4+4
# and r168 read 6 = +4+1+1 at the end of their runs, which is the same statement
# summed).  Second, all eight sit inside 0x04fe2a20..0x04fe2a47, i.e. **one 4 KiB
# page**, and CDJ_WATCH's cost is per page: watching all eight costs what
# watching one costs.
#
# 0x04fe2a44 is the interesting one, because field 7 is the select encoder and
# the encoder is the only analogue field that has ever moved the display.  The
# other seven are here so that a field which does move something can be proven
# to have arrived on its own address instead of being judged by the encoder's.
ANALOG_DESTINATION: dict[int, int] = {
    0: 0x04FE2A20,      # 0x28e1dc  byte 2
    1: 0x04FE2A24,      # 0x28e1e0  byte 3
    2: 0x04FE2A28,      # 0x28e1e6  bytes 4/5
    3: 0x04FE2A2C,      # 0x28e1f2  bytes 6/7
    4: 0x04FE2A30,      # 0x28e200  bytes 8/9
    5: 0x04FE2A34,      # 0x28e20e  bytes 10/11
    6: 0x04FE2A38,      # 0x28e288  bytes 12/13, masked 0x1ff
    7: 0x04FE2A44,      # 0x28e26e  byte 14 + W[0x04fe2af8] -- the encoder
}

# The encoder's destination, spelled once so no caller repeats the number.
ENCODER_COUNTER = ANALOG_DESTINATION[ENCODER_FIELD]

# How wide the *value* at each destination is, as a modulus -- and this is not
# the width of the store.
#
# **The measurement that made this necessary, r174.**  `rotary 7 -24` walked the
# encoder from 12 down to -12; the payload byte therefore went 0x0c -> 0xf4, and
# every one of the 20 writes reads `4-byte write` with the word holding
# `0x000000f4`.  A checker that subtracts the words gets **+232**, calls it a
# mismatch against the -24 the transcript sent, and refuses the row.  Every
# negative rotary window fails that way, silently and systematically -- which is
# the guard doing the opposite of its job.  `plan coverage`'s own
# `rotary 7 -24` was among them.
#
# **The wrap comes from the wire, not from the store.**  MAIN writes a whole
# 32-bit word at each destination (`4-byte write` in every archived stream), but
# what it writes is the payload field read back out, and the payload field is
# one or two bytes on the panel frame -- `cdj2000_input.c`'s own table:
#
#     { 2, 1 }, { 3, 1 }, { 4, 2 }, { 6, 2 }, { 8, 2 }, { 10, 2 }, { 12, 2 },
#     { 14, 1 }
#
# so fields 0, 1 and 7 wrap at 256 and the rest at 65 536.  **Field 6 is the
# exception and it is narrower than its wire**: 0x28e27a..0x28e288 masks the
# 16-bit pair with 0x1ff before storing it, so its destination wraps at 512.
# `(v2 & 0x1ff) - (v1 & 0x1ff) == (v2 - v1) mod 512`, which is why the mask is a
# modulus here and not a second rule.
#
# **And 0xf4 is not a sign extension.**  `frame_delta.signed32` was written on
# the reading that byte 14 arrives through `mov.b`, i.e. that -12 would land as
# `0xfffffff4`; r174's stream says `0x000000f4` in all 29 writes.  The byte is
# zero-extended, so the wrap is the only thing that recovers the sign, and
# signed32 stays only for a word that is genuinely negative.
ANALOG_MODULUS: dict[int, int] = {
    0: 1 << 8,          # payload byte 2
    1: 1 << 8,          # payload byte 3
    2: 1 << 16,         # bytes 4/5
    3: 1 << 16,         # bytes 6/7
    4: 1 << 16,         # bytes 8/9   -- r174: rotary 4 200 read +200 exactly
    5: 1 << 16,         # bytes 10/11
    6: ANALOG_POSITION_MASK + 1,    # bytes 12/13, masked to nine bits at 0x28e288
    7: 1 << 8,          # byte 14     -- r174: rotary 7 -24 read +232 = -24 mod 256
}


def analog_modulus(field: int) -> int | None:
    """The wrap of this field's destination value, or None for no such field."""
    return ANALOG_MODULUS.get(field)


def input_ids() -> list[str]:
    """Every input this board decodes, in the names everything else uses.

    One list, so the plan's window names, the manifest's table rows and the
    operator window's controls cannot count differently.  They already had --
    `plan keys` drove 38 of these 48 and nothing it printed said so.
    """
    return (["%d.%d" % pair for pair in BUTTON_BITS]
            + ["field%d" % index for index in range(len(ANALOG_FIELDS))])

# The decoded button bits, as (payload byte, bit), in the order the decoder
# reads them.
#
# **This was 22 bits and half a panel.**  The table was taken from 0x28e44a,
# which is where payload byte 18 begins -- but the decoder starts at 0x28e1ae,
# and before byte 18 it reads three more bytes as bit sources, spreading them
# into the same status block at 0x04fe29f4 + 72..87:
#
#     0x28e280   byte 15, bits 0 1 5 6 7            -> +75 bits 7 6 5 4 3, +79 bit 6
#     0x28e2fc   byte 16, bits 0..7                 -> +75 bits 2 1 0, +74 bits 4 1 2 6 7
#     0x28e39a   byte 17, bits 0 1 2                -> +86 bit 5, +74 bits 3 0, +72 bit 1
#
# Sixteen more inputs, and none of them had ever been driven -- which is exactly
# the space the missing display changes can be in, after r115 (all seven
# analogue fields) and r116 (all 22 old bits) each returned **zero**
# attributable changes.  Found from both ends on the same afternoon: A reading
# the decoder forwards from its entry, B reading MAIN's own panel simulator at
# 0x1010a4 backwards from its jump table.
#
# **It was 38 for a day, and 38 was two short.**  0x28e59a spreads payload 20.3
# into status 87.0 and 0x28e61e spreads 21.3 into 72.6; MAIN's own service-mode
# name table calls them MENU and MEMORY.  They were held out of this list on
# 2026-08-07 for one reason only -- `plan coverage` was in flight as B-016/r160
# and two more windows would have moved HEAD under a running measurement.  That
# run is finished, so the pair is in and the denominator is 40 + 8 = 48.
#
# Bits 15.2, 15.3, 15.4, 17.3..17.7, 18.5, 19.5, 20.6 and 20.7 are not decoded
# and are absent on purpose: they are not inputs on this board.
BUTTON_BITS: list[tuple[int, int]] = [
    (15, 0), (15, 1), (15, 5), (15, 6), (15, 7),
    (16, 0), (16, 1), (16, 2), (16, 3), (16, 4), (16, 5), (16, 6), (16, 7),
    (17, 0), (17, 1), (17, 2),
    (18, 0), (18, 1), (18, 2), (18, 3), (18, 4), (18, 6), (18, 7),
    (19, 0), (19, 1), (19, 2), (19, 3), (19, 4), (19, 6), (19, 7),
    (20, 0), (20, 1), (20, 2), (20, 3), (20, 4), (20, 5),
    (21, 0), (21, 1), (21, 2), (21, 3),
]

# The name MAIN itself prints for each bit, read out of its SERVICE MODE page.
#
# This is not a guess from where a label sits on the front panel -- that is the
# claim this project has been burned by, and it is still forbidden.  It is the
# firmware's own statement: the panel payload becomes status bits at 0x28e1ae,
# PnlCom_RcvTASK ships 44 of them to SRVMOD_MBX as message 10001 (0x2a1022),
# 0x2a09f2 matches the *changed* bits against the {mask, code} tables at
# 0x000a0e14 and 0x000a0d24, and 0x29f9b0 prints name[code] out of the pointer
# table at image 0x000a133c / runtime 0x07db345c.  Five hops, all static.
#
# **Every one of these 38 entries is checked against the image** by
# tests/test_panel_names_match_the_firmware.py, which rebuilds the table from
# main-unpacked.bin and compares it to this dict.  A host name that disagrees
# with the board is a failing test, not a surprise on a run -- which is the
# whole lesson of the SOURCE keys below.
#
# Two of the 40 decoded bits (15.6, 15.7) reach no {mask, code} entry and so
# have no name; they are absent rather than invented.
FIRMWARE_KEY_NAMES: dict[tuple[int, int], str] = {
    (15, 0): "LOCK",
    (15, 1): "REV",                 # active low -- 0x2a097c inverts bit 30
    (15, 5): "JOG TOUCH SW",
    (16, 0): "PLAY",
    (16, 1): "CUE",
    (16, 2): "RELOOP",
    (16, 3): "OUT",
    (16, 4): "IN",
    (16, 5): "HOT CUE A",
    (16, 6): "HOT CUE B",
    (16, 7): "HOT CUE C",
    (17, 0): "ENCODER PUSH",
    (17, 1): "4-BEAT LOOP",
    (17, 2): "SD OPEN",
    (18, 0): "REC MODE",
    (18, 1): "PREVIOUS |<<",
    (18, 2): "NEXT >>|",
    (18, 3): "REV <<",
    (18, 4): "FWD >>",
    (18, 6): "JOG MODE",
    (18, 7): "TEMPO RANGE",
    (19, 0): "LINK",
    (19, 1): "USB",
    (19, 2): "SD",
    (19, 3): "DISC",
    (19, 4): "TIME/ACUE",
    (19, 6): "MASTER TEMPO",
    (19, 7): "TEMPO RESET",
    (20, 0): "BROWSE",
    (20, 1): "TAG LIST",
    (20, 2): "INFORMATION",
    (20, 3): "MENU",
    (20, 4): "RETURN",
    (20, 5): "TAG TRACK",
    (21, 0): "< CALL",
    (21, 1): "CALL >",
    (21, 2): "DELETE",
    (21, 3): "MEMORY",
}

# The four SOURCE keys, by the name a human types.
#
# **THIS TABLE WAS REVERSED UNTIL 2026-08-07, and it cost the project weeks.**
# `press sd` sent 19.1, which is the USB key, so MAIN reported USB in status
# word 18, so the GUI asked for a USB browse list, so the list came back empty
# while the card was SD.  Three independent readings agree on the order above,
# and none of them needed a new run:
#
#   * MAIN's own name table (FIRMWARE_KEY_NAMES): 19.0..19.3 = LINK USB SD DISC.
#   * r160's coverage plan pressed all four in turn and status word 18 stepped
#     1 / 2 / 3 / 4 with the bit index, i.e. w18 = bit index + 1.
#   * The GUI turns that byte straight into the request's word 4 with one writer
#     per link and no remap (0xb7e442 -> 0xb9b5c8 -> 0xb9952c -> 0xb7dbce), and
#     A-025 measured MAIN's KIND enumeration as 0 LINK, 1 USB, 2 SD, 3 DISC.
#
# 0x28ddc8 is still the reason these four are wired at all: it turns a rising
# edge of status 87.7-x into the one-hot source flag at 0x04c084d0 + n*4.  What
# it never said was which n is which medium.
#
# tools/cdj_main/boot_vm.py and tools/cdj_main/view_vm.py carry the same four
# masks; tests/test_panel_names_match_the_firmware.py reads all three and the
# firmware, so they cannot drift apart again.
BUTTON_NAMES: dict[str, tuple[int, int]] = {
    "link": (19, 0x01),
    "usb": (19, 0x02),
    "sd": (19, 0x04),
    "disc": (19, 0x08),
}

DEFAULT_PORT = int(os.environ.get("CDJ_INPUT_PORT", "5984"))


# ------------------------------------------------------------ frame model ---
def panel_checksum(payload: bytes) -> int:
    """Byte 22 of a panel frame: the sum of 0..21 with an end-around carry.

    Transcribed from cdj_panel_frame() in emulator/qemu/cdj2000_main.c, which in
    turn is what the validator at 0x28cdf8 checks::

        sum += frame[i]
        if sum & 0xff00:
            sum = (sum + 1) & 0xff

    The carry is folded *inside* the loop, not at the end, so it is not the same
    as a one's-complement sum over the whole payload.  An all-zero payload
    therefore checksums to zero, which is why an unconfigured panel reply still
    validates.
    """
    total = 0
    for byte in payload:
        total += byte
        if total & 0xFF00:
            total = (total + 1) & 0xFF
    return total & 0xFF


def panel_frame(payload: bytes) -> bytes:
    """The 24 bytes the panel would put on the wire for this payload."""
    if len(payload) > PANEL_PAYLOAD_LEN:
        raise ValueError(f"payload is {len(payload)} bytes, at most "
                         f"{PANEL_PAYLOAD_LEN}")
    body = bytes(payload).ljust(PANEL_PAYLOAD_LEN, b"\0")
    return body + bytes([panel_checksum(body), PANEL_FRAME_MARK])


def analog_bytes(field: int, value: int) -> bytes:
    """The payload bytes analogue `field` would carry for `value`.

    16-bit fields are big-endian, the way 0x28e1d6 reassembles the pair.
    """
    start, width = ANALOG_FIELDS[field]
    return (value & ((1 << (8 * width)) - 1)).to_bytes(width, "big")


def apply_analog(payload: bytearray, field: int, value: int) -> None:
    start, width = ANALOG_FIELDS[field]
    payload[start:start + width] = analog_bytes(field, value)


def button_mask(name: str) -> tuple[int, int]:
    """Resolve 'sd', '19.1' or '19:02' to a (payload byte, mask) pair.

    A suffix after '-' names a way of pressing the same bit -- '20.3-hold' is
    MENU held down, the deck's UTILITY key -- and is not part of the bit.
    """
    key = name.strip().lower().split("-")[0]
    if key in BUTTON_NAMES:
        return BUTTON_NAMES[key]
    separator = "." if "." in key else (":" if ":" in key else None)
    if separator is None:
        raise ValueError(f"unknown button {name!r}; use a name from "
                         f"{sorted(BUTTON_NAMES)} or 'BYTE.BIT'")
    head, _, tail = key.partition(separator)
    byte = int(head, 0)
    if separator == ".":
        bit = int(tail, 0)
        if not 0 <= bit <= 7:
            raise ValueError(f"bit {bit} is not 0..7")
        mask = 1 << bit
    else:
        mask = int(tail, 16)
    if not 0 <= byte < PANEL_PAYLOAD_LEN:
        raise ValueError(f"payload byte {byte} is not 0..{PANEL_PAYLOAD_LEN - 1}")
    if not 1 <= mask <= 0xFF:
        raise ValueError(f"mask {mask:#x} is not 01..ff")
    return byte, mask


# ------------------------------------------------------------- the hold time
#
# **How long a press is held down, and it is the difference between a plan that
# measures something and a plan that measures nothing.**
#
# MAIN's copy of a key bit is a **level**, sampled once per accepted panel
# frame, not an event: 0x28cfe2 reads bit 5 of B[0x04fe2a4a] and 0x28cff8
# writes [0x0489bd60] on every frame it accepts.  So a press MAIN saw is a
# *block* of 75-85 consecutive status records and a press it missed leaves
# nothing at all -- there is no partial arrival.  A-033 read status halfword 16
# straight out of three link dumps (`runs/hw16.py`) and got the first clean
# measurement of that stage:
#
#     hold      presses   landed in a status record
#     300 ms      24            0     (r171 x18, r172 x6)
#     800 ms       3            0     (r172)
#     2000 ms      1            1     (r172)
#     2500 ms     10            8     (r173)
#
# A 300 ms press simply falls between two samples.  The "9 % per press" this
# project has priced the point-3 mill on since r150 is therefore a property of
# **cdj2000_input.c's 300 ms default**, not of the firmware -- and `plan
# coverage` used that default, because nothing here ever set the hold field
# `press` has carried since B-005.
#
# **It costs no run time.**  The board's press queue is serial and one press
# occupies `hold + gap`; at 2800 + 300 that is 3.1 s inside a 25 s window.  See
# `press_period_s` for the arithmetic and `plan_timing_notes` for the line the
# plan prints.
#
# ---------------------------------------------------------------------------
# **2 800 ms, not 2 500, and not 5 000 -- A-035 turned this stage into an
# arithmetic and the answer is a ceiling, not a maximum.**
#
# A-035 pooled the three runs that ever pressed at 2 500 ms (r173 8/10, r175
# 11/20, r176 10/12 = 29 of 42 = 69 %) and, more usefully, explained it: MAIN
# copies the key level into the record it *builds next*, so a press lands iff a
# record is built while the key is down, i.e. `P ~ hold / MAIN's record
# cadence`.  Checked on r175, where both sides were counted: a 4.9 s cadence
# predicts 51 % and 55 % was measured.  The cadence fitted to all 42 presses is
# 3.6 s, so more hold really does buy more stage 1 -- up to 100 % at 3.6 s.
#
# **And yet the longest hold is the wrong choice, because the binding
# constraint is frame_delta's 10 s attribution limit and not stage 1.**  A mill
# window is a burst on one bit, the queue is serial, and the burst is
# `(10 - 0.8) / press_span`.  It falls from 2 to 1 the moment the span passes
# 4.6 s -- and that halves the mill window, which costs more than the extra
# stage 1 buys.  The shape of the sweep is:
#
#     hold     stage 1   burst (stretch 1.40 / 1.44)   mill runs   last day
#     2500 ms    69 %              2  /  2                 5        1:55:10
#     2800 ms    77 %              2  /  2                 4      **1:43:30**
#     2900 ms    80 %              2  /  1                 4 or 8   up to 2:30
#     3000 ms    83 %              1  /  1                 8        2:30:10
#     5000 ms   100 %              1  /  1                 6        2:06:50
#
# **2 800 is the last hold that keeps the burst at the WORST measured stretch.**
# `HOLD_STRETCH` is 1.4 nominal, but r173's own range is 1.28..1.44, and at 1.44
# the burst-2 ceiling is 2 894 ms.  2 900 sits on the wrong side of it: it looks
# like the better choice on the nominal stretch and costs four extra mill runs
# on the measured worst one.  That asymmetry is the whole argument -- one hold
# is 100 ms better on the mean and 2 800 s worse in the tail.
#
# The second ceiling, which does not bite here but is the one to watch if
# `PLAN_ATTRIBUTION_LIMIT` ever moves: the press occupies the FRONT of the 10 s
# window, and r173's only dispatch->change delay that ever scored was 5.0 s
# (the other was 11.6 s and was refused).  A span above 5.0 s would push that
# one outside the limit too, which is a hold of 3 172 ms at stretch 1.44.
PLAN_HOLD_MS = 2800
PLAN_HOLD_SOURCE = (
    "A-035: 29 of 42 landed at 2500 ms, cadence 3.6 s, so 2800 ms models 77 %; "
    "it is the largest hold that keeps a burst of 2 under frame_delta's 10 s "
    "limit at the worst measured HOLD_STRETCH (1.44 -> ceiling 2894 ms)")

# The lowest hold at which a press has **ever** been seen in a status record.
# Below it the measured record is 0 of 27 across two runs, so a plan that
# generates shorter presses is a plan that is guaranteed to measure nothing.
#
# This is a floor, not a target: `tests/test_press_hold.py` walks every plan
# this repository can generate -- `panel_control`'s four, coverage and source
# rescue, and the operator window's buttons -- and fails on
# any press below it, including one that carries no hold at all and would run on
# the board's default.  A default that comes back silently is exactly how r171
# spent eighteen presses on nothing.
HOLD_DELIVERY_FLOOR_MS = 2000
HOLD_DELIVERY_FLOOR_SOURCE = (
    "r171/r172: 0 of 27 presses below 2000 ms reached a status record; "
    "r172's single 2000 ms press did")

# The board's own two defaults, mirrored here because the queue arithmetic
# needs them and a plan must not have to open the C to know what a press costs.
# `cdj_input_hold_ns` and `cdj_input_gap_ns` in emulator/qemu/cdj2000_input.c;
# tests/test_input_channel.py compares the three numbers.
CHANNEL_HOLD_DEFAULT_MS = 300
CHANNEL_GAP_MS = 300

# What a nominal millisecond of hold actually costs on the wire.  r173 delivered
# 75-85 records per 2500 ms press at that run's 23.5 records/s, i.e. 3.2-3.6 s
# of record time for a 2.5 s hold: the level is copied faithfully and a little
# late, which is what a level does.  Used to price a burst *conservatively*
# rather than nominally -- the difference decides how many presses fit under the
# attribution limit.
HOLD_STRETCH = 1.4
HOLD_STRETCH_SOURCE = "r173: 75-85 records per 2500 ms hold at 23.5 records/s"


def press_period_s(hold_ms: int = PLAN_HOLD_MS,
                   gap_ms: int = CHANNEL_GAP_MS) -> float:
    """How long one queued press occupies the board, hold plus gap.

    **The queue is serial**, so this and not the transcript's spacing is what
    decides how long a burst takes: `cdj_input_run_press` starts the next entry
    only once the current one has been down for its hold and up for the gap.
    Twelve presses sent 0.7 s apart do not take 8.4 s, they take twelve times
    this.
    """
    return (hold_ms + gap_ms) / 1000.0


def press_span_s(count: int, hold_ms: int = PLAN_HOLD_MS,
                 stretch: float = HOLD_STRETCH) -> float:
    """The wall time `count` queued presses take, with the measured stretch.

    Nominal would be `count * press_period_s()`; r173 says the block MAIN
    actually sees is about `HOLD_STRETCH` times the nominal hold, and a burst
    priced nominally is a burst whose last press answers after the window that
    was supposed to catch it has closed.
    """
    return count * press_period_s(hold_ms) * stretch


def press_hold_of(command: str) -> int | None:
    """The hold a schedule command carries, or None when it carries none.

    None is the finding, not an absence: a press with no hold runs on the
    board's 300 ms default, which is the setting measured to deliver nothing.
    Raises for a command that is not a press, so a caller cannot silently pass
    the wrong verb and read the answer as "fine".
    """
    parts = command.split()
    if not parts or parts[0] != "press":
        raise ValueError("not a press: %r" % command)
    if len(parts) < 3:
        return None
    return int(parts[2], 0)


def short_presses(commands, floor: int = HOLD_DELIVERY_FLOOR_MS
                  ) -> list[tuple[str, int | None]]:
    """Every press in `commands` that would run under the delivery floor.

    Takes plain schedule commands (`press 19.2 2500`), so it works on anything
    that generates one -- a plan's entries, the operator window's control
    lines -- without each of them needing a shape of
    its own.  A non-press is skipped rather than refused: a caller is expected
    to hand over a whole schedule.
    """
    bad: list[tuple[str, int | None]] = []
    for command in commands:
        text = command.strip()
        if not text.split()[:1] == ["press"]:
            continue
        hold = press_hold_of(text)
        if hold is None or hold < floor:
            bad.append((text, hold))
    return bad


class PressTooShort(ValueError):
    """A plan whose presses cannot reach a status record.

    Raised at generation time, the same way `PlanTooLong` is, and for the same
    reason: r171 spent eighteen presses and 1 300 s discovering this after the
    fact.
    """


def check_press_holds(commands, floor: int = HOLD_DELIVERY_FLOOR_MS) -> None:
    bad = short_presses(commands, floor)
    if not bad:
        return
    raise PressTooShort(
        "%d press(es) hold below %d ms, and below that the measured delivery "
        "into a status record is 0 of 27 (%s).  MAIN samples the key bit once "
        "per panel frame, so a short press falls between two samples and "
        "leaves nothing at all -- this plan would measure the sampler, not the "
        "firmware.  First offender: %r (hold %s)."
        % (len(bad), floor, HOLD_DELIVERY_FLOOR_SOURCE, bad[0][0],
           "%d ms" % bad[0][1] if bad[0][1] is not None
           else "none, so the board's %d ms default" % CHANNEL_HOLD_DEFAULT_MS))


# --------------------------------------------------------- wire protocol ----
def encode(verb: str, *args: object) -> str:
    """One protocol line, terminated.

    The server splits on spaces and reads numbers with strtol, so masks go out
    as bare hex (no 0x, matching how CDJ_PANEL_KEYS and INPUT_MANIFEST.md spell
    them) and everything else as decimal.
    """
    parts = [verb]
    parts.extend(str(argument) for argument in args)
    line = " ".join(parts)
    if "\n" in line or "\r" in line:
        raise ValueError("a command is one line")
    return line + "\n"


def encode_press(byte: int, mask: int,
                 hold_ms: int | None = PLAN_HOLD_MS) -> str:
    """One down/up pulse, with the measured hold on it by default.

    **The default used to be None**, i.e. "let the board decide", and the board
    decides 300 ms -- the one hold measured to put 0 of 24 presses into a
    status record.  Every caller that did not think about the hold therefore
    sent a press that could not arrive, which is 38 of the 48 windows of `plan
    coverage` and every button in the operator window.  Pass `hold_ms=None`
    deliberately to get the board's default back; it is a control case, not a
    convenience.
    """
    if hold_ms is None:
        return encode("press", byte, "%02x" % mask)
    return encode("press", byte, "%02x" % mask, hold_ms)


def encode_hold(byte: int, mask: int, down: bool) -> str:
    return encode("down" if down else "up", byte, "%02x" % mask)


def encode_level(byte: int, mask: int, high: bool) -> str:
    """Set a persistent literal contact level, including active-low switches."""
    if not 0 <= byte < PANEL_PAYLOAD_LEN or not 1 <= mask <= 0xff:
        raise ValueError("level requires a payload byte and nonzero 8-bit mask")
    return encode("level", byte, "%02x" % mask, int(high))


def encode_analog(field: int, value: int) -> str:
    return encode("analog", field, value)


def encode_rotary(field: int, delta: int) -> str:
    return encode("rotary", field, delta)


class PanelControl:
    """A connection to the control channel of a running machine."""

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_PORT,
                 timeout: float = 5.0) -> None:
        self.address = (host, port)
        self.timeout = timeout
        self.socket: socket.socket | None = None
        self._pending = b""

    # The greeting is sent by the server the moment it accepts, so a connect
    # that returns without one is a port that is listening but not ours.
    def open(self) -> str:
        self.socket = socket.create_connection(self.address, self.timeout)
        self.socket.settimeout(self.timeout)
        return self.read_line()

    def open_when_ready(self, deadline: float, poll: float = 0.5) -> str:
        """Retry until the machine is listening, or `deadline` seconds pass.

        The channel is opened by the *first panel exchange*, not by QEMU
        starting, so there is a gap of several seconds after the process
        appears in which nothing is listening.  A session that gave up at once
        would look like a broken build.
        """
        started = time.monotonic()
        while True:
            try:
                return self.open()
            except OSError:
                if time.monotonic() - started >= deadline:
                    raise
                time.sleep(poll)

    def close(self) -> None:
        if self.socket is not None:
            try:
                self.socket.close()
            finally:
                self.socket = None

    def __enter__(self) -> "PanelControl":
        self.open()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def read_line(self) -> str:
        assert self.socket is not None, "not connected"
        while b"\n" not in self._pending:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise ConnectionError("the control channel closed")
            self._pending += chunk
        line, _, self._pending = self._pending.partition(b"\n")
        return line.decode("ascii", "replace").strip()

    def send(self, line: str) -> str:
        assert self.socket is not None, "not connected"
        self.socket.sendall(line.encode("ascii"))
        return self.read_line()

    def send_resilient(self, line: str, deadline: float = 20.0,
                       report=None) -> str:
        """Send, and if the channel has gone, reconnect and send again.

        r089 lost the channel between a command at t=20 and one at t=150, from
        inside the guest, and the run could not tell whether the key did nothing
        or never arrived.  The board now accepts a new connection at any time,
        so a scheduled command that finds a dead socket can simply take a new
        one rather than being silently lost -- which is the difference between a
        wasted run and a measurement.
        """
        try:
            return self.send(line)
        except (OSError, ConnectionError) as error:
            if report:
                report(f"# channel lost ({error}); reconnecting")
            self.close()
            self._pending = b""
            greeting = self.open_when_ready(deadline)
            if report:
                report(f"# reconnected: {greeting}")
            return self.send(line)

    def ping(self) -> str:
        return self.send(encode("ping"))

    def state(self) -> str:
        return self.send(encode("state"))

    def press(self, button: str, hold_ms: int | None = PLAN_HOLD_MS) -> str:
        byte, mask = button_mask(button)
        return self.send(encode_press(byte, mask, hold_ms))

    def hold(self, button: str, down: bool = True) -> str:
        byte, mask = button_mask(button)
        return self.send(encode_hold(byte, mask, down))

    def analog(self, field: int, value: int) -> str:
        return self.send(encode_analog(field, value))

    def rotary(self, field: int, delta: int) -> str:
        return self.send(encode_rotary(field, delta))

    def clear(self) -> str:
        return self.send(encode("clear"))


# --------------------------------------------------------------- sessions ---
def parse_schedule(entries: list[str]) -> list[tuple[float, str]]:
    """`12.5:press sd` -> (12.5, 'press sd'), sorted by time.

    Seconds are host wall clock counted from the moment the channel opened, so
    they line up with `boot_vm --seconds` and with the frame sampler's file
    names, which is what makes a before/after pair attributable.
    """
    schedule: list[tuple[float, str]] = []
    for entry in entries:
        head, separator, command = entry.partition(":")
        if not separator or not command.strip():
            raise ValueError(f"expected SECONDS:COMMAND, got {entry!r}")
        schedule.append((float(head), command.strip()))
    return sorted(schedule, key=lambda item: item[0])


def resolve(command: str) -> str:
    """Turn one schedule line into a protocol line.

    `press sd` and `press 18.1` are resolved here rather than on the board, so
    the board never has to know a name and the names stay in one place.
    """
    parts = command.split()
    verb = parts[0]
    if verb in ("press", "down", "up") and len(parts) >= 2:
        byte, mask = button_mask(parts[1])
        if verb == "press":
            hold = int(parts[2]) if len(parts) > 2 else None
            return encode_press(byte, mask, hold)
        return encode_hold(byte, mask, verb == "down")
    if verb in ("rotary", "analog") and len(parts) == 3:
        return encode(verb, int(parts[1], 0), int(parts[2], 0))
    return encode(*parts)


# The transcript exists so that the offset between the session's clock and the
# run's own can be *measured* rather than assumed.  A session's seconds count
# from the moment it connects, and the frame sampler's file names count from the
# moment boot_vm started -- in r091 those two differed by about 45 s, which at 25
# s spacing slides every window nearly two positions.
#
# Absolute epochs are what make the two comparable: the frames carry theirs in
# their modification times, so `frame_delta windows --align <transcript>` can
# work the shift out from artefacts alone, with no number typed in by anybody.
TRANSCRIPT_HEADER = "# panel_control session transcript v1"


def write_transcript(path: str, connect_epoch: float,
                     log: list[tuple[float, float, str, str]]) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(TRANSCRIPT_HEADER + "\n")
        handle.write("# connect_epoch %.6f\n" % connect_epoch)
        handle.write("# epoch\telapsed\tcommand\treply\n")
        for epoch, elapsed, command, reply in log:
            handle.write("%.6f\t%.3f\t%s\t%s\n" % (epoch, elapsed, command,
                                                   reply))


def read_transcript(path: str) -> tuple[float, list[tuple[float, float, str, str]]]:
    """(connect epoch, entries) from a transcript written by `session`."""
    connect_epoch = None
    entries: list[tuple[float, float, str, str]] = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if line.startswith("# connect_epoch "):
                connect_epoch = float(line.split()[-1])
                continue
            if line.startswith("#") or not line.strip():
                continue
            epoch, elapsed, command, reply = line.split("\t", 3)
            entries.append((float(epoch), float(elapsed), command, reply))
    if connect_epoch is None:
        raise ValueError(f"{path} has no connect_epoch line; it is not a "
                         f"panel_control transcript")
    return connect_epoch, entries


def run_session(panel: PanelControl, schedule: list[tuple[float, str]],
                report=print, transcript: str | None = None) -> list[tuple[float, str, str]]:
    """Send each command at its time and record when it actually went.

    **The clock starts when the channel opens, not when the machine boots.**
    The board opens the channel on its first panel exchange, so a session
    started before QEMU spends its first seconds waiting and everything after
    is relative to the end of that wait.  In r088 a command at 300 s therefore
    fell past the end of a 340-second run and was simply never sent -- which
    read like a key that did nothing.  The elapsed time printed on each line is
    the real one, and that is what belongs in a report.
    """
    started = time.monotonic()
    connect_epoch = time.time()
    log: list[tuple[float, str, str]] = []
    detailed: list[tuple[float, float, str, str]] = []
    try:
        for when, command in schedule:
            remaining = when - (time.monotonic() - started)
            if remaining > 0:
                time.sleep(remaining)
            elapsed = time.monotonic() - started
            reply = panel.send_resilient(resolve(command), report=report)
            log.append((elapsed, command, reply))
            detailed.append((time.time(), elapsed, command, reply))
            report("t%08.3f  %-24s %s" % (elapsed, command, reply))
    finally:
        # Written even if the run is cut short, because a partial transcript
        # still says which windows were actually driven -- and a window that
        # was never driven is the one thing an evaluation must not score.
        if transcript:
            write_transcript(transcript, connect_epoch, detailed)
            report("# transcript -> %s" % transcript)
    return log


# ------------------------------------------------------- the canonical plan --
#
# The coverage target is a versioned input manifest listing **every** CDJ key and
# the rotary (left, right, press), each with whether it was driven and what
# happened, and at least six display changes proven.  What exists today is
# stitched together from several runs under the 16-entry ceiling of
# CDJ_PANEL_KEYS -- a ceiling the control channel does not have.
#
# So the plan is generated, not written down twice.  The same object emits the
# session that drives the machine and the frame_delta invocation that judges it,
# which is the only way the two cannot drift apart.

# 25 s, not the 5-15 s of r024/r026.  Measured reason: the frame sampler writes
# a file only when the frame changed, so a quiet stretch leaves a hole, and in
# r076 four keys shared one before/after pair across a 50-second one.  A window
# has to be wider than any plausible hole and still under frame_delta's
# ten-second attribution limit at its own ends.
PLAN_SPACING = 25.0

# When the first input goes in, and it has moved twice for two different
# reasons.
#
# **t150 -> t210: the screen was still repainting.**  Reaching the browse phase
# (~115 s, r079) is not the same as standing still afterwards.  r113 repainted
# until t194.8 and its inputs at t151 and t176 measured the repainting.
#
# **t210 -> t300: before t150 no press has ever reached the key dispatcher.**
# Four runs with BFIN_STEP_TRACE, nine to eleven presses each, and the five
# presses that produced a dispatch are at t298.4, t373.9, t398.6, t423.5 and
# t448.3 -- every one in the second half.  t210 sits in a stretch where nothing
# has ever worked, so a plan starting there spends its first four windows on
# presses that will not arrive.
#
# **Five successes are not a model.**  Nobody knows yet why an early press is
# lost, so this is a warning turned into a default, not a law: pass --start if a
# run's own trace says the dispatcher was awake earlier.  What makes the wrong
# value survivable is that frame_delta --trace now refuses an undispatched row
# instead of scoring it.
PLAN_START = 300.0
PLAN_START_SOURCE = "r131-r134, no dispatch before t150; earliest success t298.4"

# How long after the last input the run must still be recording, so the last
# window has a frame at least `settle` seconds on the far side of it.
PLAN_TAIL = PLAN_SPACING

# The channel opens on the board's first panel exchange, not when QEMU starts,
# and a session's seconds count from the moment it connects.  r088 lost its
# command at 300 s off the end of a 340-second run for exactly this reason, so
# the allowance is part of the arithmetic rather than a footnote.
#
# **45, and the number is measured.**  In r091 the channel came up after the
# 40-second mark and the session clock ran about 45 s behind the run's own.  The
# 25 s that stood here before was an estimate, and it was wrong in the direction
# that costs a run: at 25 s spacing a 45-second error slides every window nearly
# two positions, so each key would be attributed to its neighbour -- consistently,
# and therefore invisibly.  A table that is uniformly wrong looks exactly like a
# table that is right.
#
# Do not tune this by eye.  It is the fallback for when the offset has not been
# measured on the run being evaluated; `session --transcript` plus
# `frame_delta windows --align` measure it per run and need no assumption at all.
PLAN_CONNECT_ALLOWANCE = 45.0
PLAN_CONNECT_ALLOWANCE_SOURCE = "r091"

# The mask has to come from a control run of the **same length and the same
# switches** as the run being judged, because it says which pixels move by
# themselves and that depends on what is on the screen.
#
# This default is not a preference.  `runs/anim-mask.bin` is r048's, from a
# world with an empty browse pane and the spinning "Wait" platter, and it stood
# here until r093 pasted this line verbatim and lost its answer: all four
# measurable windows landed on the platter, which that mask does not cover.  A
# mask from the wrong world erases exactly the fields that carry the evidence.
#
# r095 is the control run for the world these plans now drive: same 745 s, same
# BFIN_REQUEST_KIND=2, channel open and silent.  91.3 % evidence surface,
# measured noise floor 0.
#
# **That default has now expired, and nothing has replaced it.**  Two runs said
# so from opposite directions:
#
#   r095's screen stopped at t236 because the drawing task was stuck in the
#   fault loop r098 diagnosed, so most of its 91.3 % was "still" only in the
#   sense that nothing was drawing.  A mask from it lets real animation through
#   as a delta -- wrong in the direction that *invents* evidence.
#
#   r112, the control run for the world these plans drive now, has no steady
#   phase to fit at all: it stopped changing at t121.3, so `frame_delta mask
#   --from 150` finds zero frames.  There is no mask to build from it, and its
#   stillness says nothing about the churn a *keyed* run goes through (r113
#   churned to t194.8).
#
# So the plan names no mask.  What replaced it is the control the run carries
# with it: `frame_delta windows` takes a window in each input-free gap of the
# same run and refuses a row that does not beat the worst of them.  That control
# is in the right world by construction, which no stored mask can promise.  Pass
# --mask if a control run genuinely in this state ever exists.
PLAN_MASK = None
PLAN_MASK_NOTE = ("no stored mask.  r093 lost its answer to a mask from another "
                  "world and r113 invented two rows with no mask at all; the "
                  "self-baseline frame_delta takes from this run's own "
                  "input-free gaps is the control that cannot be from the wrong "
                  "world.  Pass --mask only for a control run in this state.")

# How long the display took to answer a key, measured rather than assumed.
#
# r096, bit 18.3: the screen had stood still for 18.3 s, the press landed at
# t227.0, and the repaint -- the TRACK digit going 0 to 1 -- is at t234.8.  The
# stillness either side is what makes it a response time and not an interval
# between two repaints: index.tsv has eleven `same` ticks before it.
#
# How long the display takes to answer a key that actually arrived.
#
# **It is 0.7-0.8 s, and every earlier figure here was measured on a row that
# was not an answer.**  7.8 s came from r096's 18.3 and 9.5 s from r113's 18.7;
# neither run had a dispatcher trace, and r116 later drove 18.7 again and got a
# proven no-op, so those two numbers describe repaints nobody can attribute to a
# key.  The two library switches that *are* attributed -- r133's 19.1-sd2 and
# r134's 19.3-i, each with a `key dispatcher` line beside it -- answered in
# **0.7 s and 0.8 s**.
#
# The decision rule this file has carried since B-008 said a median above 7.8 s
# would mean the constant was wrong.  It went the other way and by more: the
# constant was an order of magnitude too high, which is the direction that
# quietly loses rows, because --settle is derived from it.
PLAN_RESPONSE = 0.8
PLAN_RESPONSE_SOURCE = "r134 19.3-i (0.8 s); r133 19.1-sd2 gave 0.7 s"

# frame_delta.MAX_WINDOW_SECONDS, repeated rather than imported so that driving
# the machine does not pull in numpy and PIL.  tests/test_panel_control.py
# asserts the two agree.
PLAN_ATTRIBUTION_LIMIT = 10.0

# `--settle` is a floor on how late the second frame may be taken.  It must stay
# **below** the fastest answer the display gives, and that is not a preference:
# the sampler writes nothing while the screen stands, so a settle longer than
# the answer skips the answering frame and takes the next input's repaint
# instead -- a real number over a two-input window.  Measured on a synthetic
# run: a 5-second answer read with --settle 6 reports 24 bytes over 30 s where
# the truth is 12 bytes over 5 s.
#
# The two errors are therefore not symmetric.  Too high loses the row and can
# replace it with a plausible wrong one; too low can at worst catch a frame
# mid-redraw, which the index makes visible as consecutive `new` ticks and which
# costs nothing but a second look.
#
# **So the margin goes all the way down: 0.**  A settle of 1 s was already above
# the whole measured answer time (0.7-0.8 s), and r133 shows what that costs on
# a real row -- the same input read two ways:
#
#     --settle 3   19.1-sd2   t0208.9 -> t0407.3   30.5 s, unattributable
#     --settle 0   19.1-sd2   t0208.9 -> t0377.5    0.7 s, a result
#
# What used to justify a floor was the fear of catching a frame mid-redraw.  The
# index shows that as consecutive `new` ticks, and placing a change relative to
# its input is now the index's job (CHANGE NOT PROVEN AFTER THE INPUT) rather
# than the settle's.  So the settle has no work left to do.
PLAN_SETTLE = 0.0


class PlanTooLong(ValueError):
    """The last input would fall past the end of the run.

    Raised at generation time on purpose.  r088 died because a schedule was
    accepted that could not fit, and nothing said so until the frames came back
    with a window that was never driven.
    """


# A command shortly after connecting, before the long wait for the browse phase.
#
# r094 lost every command of a `keys` run: the first was at t=150, so the
# channel sat idle from the moment it opened, and by t=150 it was gone.  r091,
# which survived, had a command at t=20 and went quiet *afterwards*.  A `ping`
# at t=5 is the difference between those two shapes, and it costs nothing: it
# touches no payload byte, so it cannot disturb a measurement, and it is not a
# window -- there is nothing to attribute.
#
# If an idle channel is what dies, this both tells us so and works around it.
# If it is not, the run still says whether the channel was ever alive, which
# r094 could not distinguish from a channel that died later.
PLAN_PROBE_AT = 5.0
PLAN_PROBE = "ping"


def plan_entries(name: str, start: float = PLAN_START,
                 spacing: float = PLAN_SPACING,
                 field: int | None = None,
                 probe: bool = True,
                 hold_ms: int = PLAN_HOLD_MS
                 ) -> list[tuple[float, str, str | None]]:
    """(seconds, command, window name) for one named plan.

    A window name of None is an entry that is driven but not scored -- the
    early probe.  `plan_windows` is what the evaluation is built from.

    **Every press carries its hold explicitly.**  It used to carry none, so
    every plan this file has ever printed ran on the board's 300 ms default --
    the one setting measured to put 0 of 24 presses into a status record.  See
    `PLAN_HOLD_MS`.  Passing a hold below `HOLD_DELIVERY_FLOOR_MS` raises here
    rather than at the end of a 1 520-second run.
    """
    check_press_holds(["press 0.0 %d" % hold_ms])
    if name == "keys":
        items = [("press %d.%d %d" % (byte, bit, hold_ms),
                  "%d.%d" % (byte, bit))
                 for byte, bit in BUTTON_BITS]
    elif name == "coverage":
        # Every input on the board in one run, which is a provenance decision
        # rather than a convenience.
        #
        # `keys` drives 40 of the 48 inputs INPUT_MANIFEST.md enumerates.  The
        # eight analogue fields are in `rotary-sweep` and the encoder's second
        # direction is in `rotary --field N`, so covering the board meant three
        # runs -- and the provenance rule says a coverage run counts
        # only on the same HEAD and the same QEMU binary as the others.  Three
        # runs can satisfy that only if nothing is rebuilt between them, which
        # is precisely the condition the last day cannot assume: r026's numbers
        # are unusable today for exactly this reason.
        #
        # Concatenating them also costs less wall clock than splitting: three
        # runs pay the t300 wait and the 45 s connect allowance three times
        # (1295 + 545 + 395 = 2235 s) where one pays it once.
        items = [("press %d.%d %d" % (byte, bit, hold_ms),
                  "%d.%d" % (byte, bit))
                 for byte, bit in BUTTON_BITS]
        items += [("rotary %d 12" % index, "field%d" % index)
                  for index in range(len(ANALOG_FIELDS))]
        # The encoder is field 7 (payload byte 14), and left is not a separate
        # control -- it is the same field walked the other way.  Twice as far,
        # so it passes back through the starting value and a stuck reading is
        # distinguishable from a genuine return.  It sits directly after its
        # own right-hand row so the pair can be read together.
        items += [("rotary %d -24" % ENCODER_FIELD, "rotary-left")]
    elif name == "rotary-sweep":
        # One ramp per analogue field, to find which is the select encoder.
        items = [("rotary %d 12" % index, "field%d" % index)
                 for index in range(len(ANALOG_FIELDS))]
    elif name == "rotary":
        if field is None:
            raise ValueError("the rotary plan needs --field, which "
                             "'rotary-sweep' is there to identify")
        items = [
            ("rotary %d 12" % field, "rotary-right"),
            # Twice as far, so it passes back through the starting value and
            # a stuck reading is distinguishable from a genuine return.
            ("rotary %d -24" % field, "rotary-left"),
        ]
    else:
        raise ValueError(f"unknown plan {name!r}")
    entries: list[tuple[float, str, str | None]] = []
    if probe:
        entries.append((PLAN_PROBE_AT, PLAN_PROBE, None))
    entries += [(start + index * spacing, command, window)
                for index, (command, window) in enumerate(items)]
    return entries


def plan_windows(entries: list[tuple[float, str, str | None]]
                 ) -> list[tuple[float, str]]:
    """Only the entries an evaluation should score."""
    return [(when, window) for when, _, window in entries if window]


def plan_seconds(entries: list[tuple[float, str, str]],
                 tail: float = PLAN_TAIL,
                 allowance: float = PLAN_CONNECT_ALLOWANCE) -> float:
    """The shortest run this plan fits inside, connection allowance included."""
    if not entries:
        return 0.0
    return entries[-1][0] + tail + allowance


def check_plan(entries: list[tuple[float, str, str]], seconds: float,
               tail: float = PLAN_TAIL,
               allowance: float = PLAN_CONNECT_ALLOWANCE) -> None:
    needed = plan_seconds(entries, tail, allowance)
    if needed > seconds:
        raise PlanTooLong(
            "the last input is at t%g and the run is %g s; with %g s of tail "
            "and %g s allowance for the channel opening late this needs "
            "--seconds %d.  Do not shrink the spacing to make it fit -- that is "
            "what left four rows of INPUT_MANIFEST.md unattributed."
            % (entries[-1][0], seconds, tail, allowance, -(-needed // 1)))


def split_plan(entries: list[tuple[float, str, str | None]], parts: int,
               start: float = PLAN_START, spacing: float = PLAN_SPACING
               ) -> list[list[tuple[float, str, str | None]]]:
    """Cut a plan into `parts` runs, each restarting its own clock.

    Each part is a separate run, so each gets its **own** early probe -- one
    carried over from the first part would sit at t=5 of a machine that had not
    been started yet.  The probe is not an input and is not counted when
    deciding how to divide the work.
    """
    probes = [entry for entry in entries if entry[2] is None]
    inputs = [entry for entry in entries if entry[2] is not None]
    size = -(-len(inputs) // parts)
    chunks = [inputs[index:index + size]
              for index in range(0, len(inputs), size)]
    return [list(probes)
            + [(start + position * spacing, command, window)
               for position, (_, command, window) in enumerate(chunk)]
            for chunk in chunks]


# Where the session writes its transcript and where the evaluation reads it.
#
# **One string, used by both printed lines.**  The two commands are meant to be
# copied verbatim, so anything that has to agree between them has to come from
# here rather than from the reader remembering to add it.  A session without
# --transcript leaves no anchor, and an evaluation without --align then has
# nothing to measure the 45-second clock offset from -- which is the error the
# whole alignment machinery exists to prevent.  Leaving those two flags off the
# printed lines put that error back one copy-paste away.
PLAN_TRANSCRIPT = "runs/r0NN/session.txt"


def plan_transcript(base: str, label: str) -> str:
    """A per-run transcript path, so a split plan does not overwrite its own."""
    head, dot, extension = base.rpartition(".")
    if not dot or "/" in extension or "\\" in extension:
        return "%s-%s" % (base, label)
    return "%s-%s.%s" % (head, label, extension)


def session_command(entries: list[tuple[float, str, str]], port: int,
                    wait: float = 120.0,
                    transcript: str = PLAN_TRANSCRIPT) -> str:
    parts = ["python -m tools.cdj_main.panel_control --port %d session "
             "--wait %g" % (port, wait)]
    parts += ['%g:"%s"' % (when, command) for when, command, _ in entries]
    parts.append("--linger %g" % PLAN_TAIL)
    parts.append("--transcript %s" % transcript)
    return " ".join(parts)


# Where the run writes BFIN_STEP_TRACE's output and where the evaluation reads
# it.  Same rule as PLAN_TRANSCRIPT: one string, so the two printed lines cannot
# name different files.
PLAN_TRACE = "runs/r0NN/step-trace.txt"

# And where the run's CDJ_WATCH stream lands.  Same rule again: one string.
#
# It is MAIN's stderr, because that is where the watch writes; `boot_vm
# --stderr` is what puts it on disk.  Without it the analogue rows of a plan --
# eight fields and the encoder's other direction, nine of the 49 windows -- can
# only be written down as NOT MEASURED, which is what happened to every one of
# r160's.
PLAN_COUNTER = "runs/r0NN/watch.txt"


def frame_delta_command(entries: list[tuple[float, str, str]], frames: str,
                        mask: str | None, look: str,
                        transcript: str = PLAN_TRANSCRIPT,
                        settle: float = PLAN_SETTLE,
                        trace: str = PLAN_TRACE,
                        counter: str | None = PLAN_COUNTER) -> str:
    # No --mask unless one was named.  A stored mask is only evidence if some
    # control run reached the state being judged, and none has: see PLAN_MASK.
    # frame_delta's self-baseline is the control that comes with the run.
    #
    # --trace is on the line unconditionally, because it is the one check the
    # picture cannot stand in for: r134's 172 807-byte row passed every pixel
    # guard and was the boot animation, inherited by a press that landed at its
    # end.  An evaluation without it cannot tell "the key did nothing" from
    # "the key never arrived".
    #
    # --counter is the analogue half of the same argument.  A rotary never
    # reaches the key dispatcher, so --trace can only ever say `NO KEY
    # DISPATCHED` about one -- a sentence about a path it does not take.  Its
    # arrival is the destination word in MAIN's status block moving by exactly
    # the amount sent, and that is only on disk if the run carried CDJ_WATCH.
    parts = ["python -m tools.cdj_main.frame_delta windows %s%s "
             "--look %s --align %s --trace %s%s --settle %g"
             % (frames, " --mask %s" % mask if mask else "",
                look, transcript, trace,
                " --counter %s" % counter if counter else "", settle)]
    # The probe is driven but not scored: it moves no payload byte, so there is
    # nothing to attribute and a window for it would only invite a reading.
    parts += ["%g:%s" % (when, window) for when, window in plan_windows(entries)]
    return " ".join(parts)


def plan_timing_notes(spacing: float = PLAN_SPACING,
                      settle: float = PLAN_SETTLE,
                      response: float = PLAN_RESPONSE,
                      hold_ms: int = PLAN_HOLD_MS,
                      presses_per_window: int = 1) -> list[str]:
    """What the measured answer time does to the plan, and what it costs.

    The short version, so it is not rediscovered on a run: it changes
    `--settle` and it does **not** change the run length.  The binding
    constraint is the attribution limit, not the spacing -- an answer must land
    inside 10 s of its press, and 25 s of spacing already keeps the next press
    15 s clear of that window.  Widening the spacing buys nothing an answer
    slower than 10 s could use.
    """
    headroom = PLAN_ATTRIBUTION_LIMIT - response
    span = press_span_s(presses_per_window, hold_ms)
    lines = [
        "hold             %d ms per press, and it is the first of THREE stages "
        "that lose presses.  press -> status record is governed by the hold "
        "against MAIN's record cadence (%s): MAIN copies the key bit once per "
        "accepted panel frame, so a press it saw is a block of about %d "
        "records and one it missed leaves nothing.  status record -> key "
        "dispatcher is the delivery and is NOT helped by this (A-035: 3 of 29 "
        "over r173+r175+r176, and MAIN's 224-byte answers are refused in the "
        "same place).  dispatch -> a scored window is frame_delta's guards.  A "
        "row that scores nothing does not say which stage lost it"
        % (hold_ms, PLAN_HOLD_SOURCE,
           round(83 * hold_ms / 2500.0)),
        "press cost       %g s per press on the board (hold %d ms + gap %d ms, "
        "and the queue is SERIAL), x%.1f as delivered (%s) = %.1f s for the %d "
        "press(es) of a window.  That is %.0f %% of the %g s spacing, so the "
        "hold costs this plan no run time at all -- plan_seconds is start + "
        "spacing x inputs and does not contain it"
        % (press_period_s(hold_ms), hold_ms, CHANNEL_GAP_MS, HOLD_STRETCH,
           HOLD_STRETCH_SOURCE, span, presses_per_window,
           100.0 * span / spacing, spacing),
        "answer time      %.1f s, measured on a row with a key dispatcher line "
        "beside it (%s).  --settle %g stays below it -- a settle longer than "
        "the answer skips the answering frame and takes the next input's "
        "repaint, which cost r133's best row 30 s of window"
        % (response, PLAN_RESPONSE_SOURCE, settle),
        "key dispatch     every evaluation carries --trace.  A window without a "
        "`key dispatcher` line is not a result: r134's 172 807-byte row passed "
        "the mask, the baseline and both timing guards and was the boot "
        "animation's repaint, inherited by a press at its end",
        "analogue arrival every evaluation also carries --counter, and the RUN "
        "has to write the file it names.  A rotary cannot reach the key "
        "dispatcher, so --trace can only ever say NO KEY DISPATCHED about one "
        "-- which is what happened to all nine analogue windows of r160.  Its "
        "arrival is the field's destination word moving by exactly the amount "
        "sent, so the run needs BOTH of:\n"
        "                     CDJ_WATCH=%s\n"
        "                     boot_vm ... --stderr %s\n"
        "                 Without them the %d analogue windows of this plan are "
        "ANALOG NOT MEASURED -- refused, but with their own reason, and the "
        "press rows are unaffected"
        % (",".join("%#x" % address
                    for _f, address in sorted(ANALOG_DESTINATION.items())),
           PLAN_COUNTER, len(ANALOG_FIELDS) + 1),
        "first input      t%g, past the churn (%s).  Reaching the browse phase "
        "is not standing still: a window inside the churn measures the churn, "
        "and frame_delta refuses it rather than scoring it"
        % (PLAN_START, PLAN_START_SOURCE),
        "run length       set by the first input and the spacing.  The binding "
        "limit on the *spacing* is attribution (%g s), not the gap: at %g s "
        "apart the next press is %g s clear of this window"
        % (PLAN_ATTRIBUTION_LIMIT, spacing, spacing - PLAN_ATTRIBUTION_LIMIT),
    ]
    if span + response > PLAN_ATTRIBUTION_LIMIT:
        lines.append(
            "  WARNING: %d press(es) at %d ms take %.1f s to drain and the "
            "display answers %.1f s later, which is past the %g s attribution "
            "limit.  The last press of the burst would be judged in the NEXT "
            "window -- shorten the burst or the hold, do not widen the limit."
            % (presses_per_window, hold_ms, span, response,
               PLAN_ATTRIBUTION_LIMIT))
    if span > spacing:
        lines.append(
            "  WARNING: the burst takes %.1f s and the windows are %g s apart, "
            "so presses queued for one window are still going down during the "
            "next one.  Every window after the first would be contaminated."
            % (span, spacing))
    if headroom < response / 2:
        lines.append(
            "  WARNING: only %.1f s of headroom under the %g s attribution "
            "limit, and the limit is not a tuning knob.  If a third answer "
            "crosses it, the limit and the spacing move together or not at all "
            "-- raising the limit alone widens every window and is exactly how "
            "a row gets a number it did not earn.  r096's 18.7 at 15.8 s is "
            "what running out looks like."
            % (headroom, PLAN_ATTRIBUTION_LIMIT))
    return lines


def plan_coverage(name: str = "keys") -> list[str]:
    """What full coverage asks for, and whether this plan drives all of it.

    The last line is the one that matters and it used to be missing: `keys`
    covers 40 of the 48 inputs INPUT_MANIFEST.md enumerates, so a run of it
    alone leaves the eight analogue fields and the encoder's left-hand
    direction without a row on this binary.
    """
    whole = name == "coverage"
    lines = [
        "%d button bits            -> %s" % (len(BUTTON_BITS),
                                             "in this plan" if name in
                                             ("keys", "coverage")
                                             else "plan 'keys'"),
        "%d analogue fields         -> %s" % (len(ANALOG_FIELDS),
                                              "in this plan" if name in
                                              ("rotary-sweep", "coverage")
                                              else "plan 'rotary-sweep'"),
        "rotary left               -> %s"
        % ("in this plan (field %d, the other direction)" % ENCODER_FIELD
           if whole else "plan 'rotary --field %d'" % ENCODER_FIELD),
        "rotary press              -> one of the button bits.  Which one is a "
        "result of these runs, not an input to them.",
    ]
    if not whole:
        lines.append(
            "INCOMPLETE: full coverage asks for every key AND the "
            "rotary, each with a result.  This plan drives %d of the %d "
            "inputs on the board.  `plan coverage` drives all of them in one "
            "run, which is also the only way they share one HEAD and one "
            "binary." % (len(plan_windows(plan_entries(name, field=ENCODER_FIELD)))
                         if name != "rotary" else 2,
                         len(BUTTON_BITS) + len(ANALOG_FIELDS)))
    return lines


# ------------------------------------------------------------------- CLI ----
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--host", default="127.0.0.1")
    sub = parser.add_subparsers(dest="command", required=True)

    press = sub.add_parser("press", help="one down/up pulse")
    press.add_argument("button", help="sd, usb, disc, link, or BYTE.BIT")
    press.add_argument("--hold-ms", type=int, default=PLAN_HOLD_MS,
                       help="milliseconds to hold the bit down; default %d "
                            "(%s).  The board's own default is %d ms, at which "
                            "0 of 24 measured presses reached a status record"
                            % (PLAN_HOLD_MS, PLAN_HOLD_SOURCE,
                               CHANNEL_HOLD_DEFAULT_MS))
    press.add_argument("--repeat", type=int, default=1)
    press.add_argument("--gap", type=float, default=0.0,
                       help="host-side seconds between repeats")

    for verb in ("down", "up"):
        held = sub.add_parser(verb, help=f"{verb} a bit, without the pulse")
        held.add_argument("button")

    rotary = sub.add_parser("rotary", help="move an analogue field by a delta")
    rotary.add_argument("field", type=int, choices=range(len(ANALOG_FIELDS)))
    rotary.add_argument("delta", type=int)

    analog = sub.add_parser("analog", help="set an analogue field outright")
    analog.add_argument("field", type=int, choices=range(len(ANALOG_FIELDS)))
    analog.add_argument("value", type=lambda text: int(text, 0))

    step = sub.add_parser("step", help="rotary steps per panel frame")
    step.add_argument("count", type=int)

    sub.add_parser("state", help="what the channel is driving right now")
    sub.add_parser("ping")
    sub.add_parser("clear", help="release everything")

    session = sub.add_parser(
        "session",
        help="run a timed script beside a boot_vm run, e.g. "
             "session --wait 90 20:'press sd' 60:'rotary 4 12'",
        description="Run a timed script beside a boot_vm run.\n\n"
                    "SECONDS COUNT FROM THE MOMENT THE CHANNEL OPENS, NOT FROM "
                    "BOOT. The board opens it on its first panel exchange, so a "
                    "session started before QEMU waits first and everything is "
                    "relative to the end of that wait. Budget for it: in r088 a "
                    "command at 300 s fell past the end of a 340-second run and "
                    "was never sent, which reads exactly like a key that does "
                    "nothing. Keep the last command well inside --seconds, and "
                    "read the elapsed times this prints rather than assuming "
                    "the schedule.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    session.add_argument("entry", nargs="+", metavar="SECONDS:COMMAND")
    session.add_argument("--wait", type=float, default=90.0,
                         help="seconds to keep retrying the connection; the "
                              "channel opens on the first panel exchange, not "
                              "when QEMU starts")
    session.add_argument("--linger", type=float, default=0.0,
                         help="hold the connection open this long afterwards, "
                              "so the last press is not cut short")
    session.add_argument("--transcript", default=None, metavar="FILE",
                         help="record every command with an absolute epoch, so "
                              "the offset between this clock and the frame "
                              "sampler's can be measured afterwards rather "
                              "than assumed.  frame_delta windows --align reads "
                              "it.  Write it into the run's evidence directory")

    plan = sub.add_parser(
        "plan",
        help="the canonical input plan for full coverage, and the two "
             "commands that drive and judge it")
    plan.add_argument("name",
                      choices=("coverage", "keys", "rotary-sweep", "rotary"),
                      help="'coverage' is every input on the board in one run "
                           "-- the only shape in which the whole manifest "
                           "table shares one HEAD and one binary.  The other "
                           "three are its parts and stay for diagnosis")
    plan.add_argument("--field", type=int, default=None,
                      help="the analogue field 'rotary-sweep' identified")
    plan.add_argument("--start", type=float, default=PLAN_START)
    plan.add_argument("--spacing", type=float, default=PLAN_SPACING)
    plan.add_argument("--parts", type=int, default=1,
                      help="cut it into this many runs, each with its own "
                           "clock, instead of shrinking the spacing")
    plan.add_argument("--seconds", type=float, default=None,
                      help="refuse to emit a plan that does not fit this run "
                           "budget")
    plan.add_argument("--frames", default="<scratch>/frames")
    plan.add_argument("--mask", default=PLAN_MASK, help=PLAN_MASK_NOTE)
    plan.add_argument("--look", default="<scratch>/look")
    plan.add_argument("--trace", default=PLAN_TRACE,
                      help="where the run writes BFIN_STEP_TRACE's output.  The "
                           "evaluation needs it to tell a key that did nothing "
                           "from a key that never reached the dispatcher, and "
                           "r134 shows a 172 807-byte row that every other "
                           "check passed and the trace refused")
    plan.add_argument("--settle", type=float, default=PLAN_SETTLE,
                      help="printed on the evaluation line.  Must stay "
                           "below the display's answer time (%g s, %s); "
                           "a longer settle skips the answering frame"
                           % (PLAN_RESPONSE, PLAN_RESPONSE_SOURCE))
    plan.add_argument("--transcript", default=PLAN_TRANSCRIPT,
                      help="where the session writes its transcript and the "
                           "evaluation reads it; both printed lines take it "
                           "from here, so they cannot disagree")
    plan.add_argument("--hold-ms", type=int, default=PLAN_HOLD_MS,
                      help="how long each press is held down.  Default %d ms "
                           "(%s); below %d ms the measured delivery into a "
                           "status record is 0 of 27 and this refuses to emit "
                           "the plan"
                           % (PLAN_HOLD_MS, PLAN_HOLD_SOURCE,
                              HOLD_DELIVERY_FLOOR_MS))

    args = parser.parse_args(argv)
    if args.command == "plan":
        try:
            entries = plan_entries(args.name, args.start, args.spacing,
                                   args.field, hold_ms=args.hold_ms)
            chunks = (split_plan(entries, args.parts, args.start, args.spacing)
                      if args.parts > 1 else [entries])
            for chunk in chunks:
                check_plan(chunk, args.seconds if args.seconds
                           else plan_seconds(chunk))
                check_press_holds(command for _at, command, _w in chunk)
        except (ValueError, PlanTooLong, PressTooShort) as error:
            print(f"panel_control: {error}", file=sys.stderr)
            return 1
        print("# plan '%s': %d inputs, %g s apart, first at t%g; "
              "probe '%s' at t%g"
              % (args.name, len(plan_windows(entries)), args.spacing,
                 args.start, PLAN_PROBE, PLAN_PROBE_AT))
        print("# what full coverage asks for:")
        for line in plan_coverage(args.name):
            print("#   " + line)
        print("# mask %s -- %s" % (args.mask or "none", PLAN_MASK_NOTE))
        for line in plan_timing_notes(args.spacing, args.settle,
                                      hold_ms=args.hold_ms):
            print("# " + line)
        for index, chunk in enumerate(chunks, start=1):
            label = ("%s-%d" % (args.name, index)) if len(chunks) > 1 else args.name
            # Each run needs its own transcript: a shared path would let the
            # second run overwrite the first one's anchor, and the first
            # evaluation would then align against the wrong clock.
            transcript = (args.transcript if len(chunks) == 1
                          else plan_transcript(args.transcript, label))
            print("\n## %s -- %d inputs, run needs --seconds %d"
                  % (label, len(plan_windows(chunk)),
                     -(-plan_seconds(chunk) // 1)))
            print(session_command(chunk, args.port, transcript=transcript))
            print()
            print(frame_delta_command(chunk, args.frames, args.mask, args.look,
                                      transcript=transcript,
                                      settle=args.settle, trace=args.trace))
        return 0

    if args.command == "session":
        try:
            schedule = parse_schedule(args.entry)
            for _, command in schedule:
                resolve(command)          # fail before the machine is touched
        except ValueError as error:
            print(f"panel_control: {error}", file=sys.stderr)
            return 1
        panel = PanelControl(args.host, args.port)
        try:
            print("# " + panel.open_when_ready(args.wait))
            run_session(panel, schedule, transcript=args.transcript)
            if args.linger:
                time.sleep(args.linger)
        except (OSError, ValueError) as error:
            print(f"panel_control: {error}", file=sys.stderr)
            return 1
        finally:
            panel.close()
        return 0

    try:
        with PanelControl(args.host, args.port) as panel:
            if args.command == "press":
                for index in range(max(1, args.repeat)):
                    if index and args.gap:
                        time.sleep(args.gap)
                    print(panel.press(args.button, args.hold_ms))
            elif args.command in ("down", "up"):
                print(panel.hold(args.button, args.command == "down"))
            elif args.command == "rotary":
                print(panel.rotary(args.field, args.delta))
            elif args.command == "analog":
                print(panel.analog(args.field, args.value))
            elif args.command == "step":
                print(panel.send(encode("step", args.count)))
            elif args.command == "state":
                print(panel.state())
            elif args.command == "ping":
                print(panel.ping())
            elif args.command == "clear":
                print(panel.clear())
    except (OSError, ValueError) as error:
        print(f"panel_control: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
