"""Read MAIN's caution store — the machine's own list of what is wrong.

The GUI's error banner is not the GUI's opinion.  Status word 20 of the 64-byte
record is a copy of ``[0x053560b0]``, and that word is written by exactly one
instruction in the whole image — ``0x2bfae6``, inside the caution-apply function
``0x2bf6d4`` — as ``(qualifier << 8) | code``.  Everything else about the caution
system is a store around it:

===========================  ============  =====================================
what                         address       shape
===========================  ============  =====================================
published A                  0x053560b0    ``(qualifier << 8) | code``
published B                  0x053560b8    second list, same shape
pending list A               0x053560c8    **5 records of 16 B, one per priority**
list A, as published         0x0535616c    same records, indexed by the winner
pending list B               0x05356118    5 records
list B, as published         0x053561cc
current priority A           0x05357ec4    which slot is on screen
current priority B           0x05357ec8
history ring A               0x05356298    100 records of 36 B
  newest index / next slot   0x05356290 / 0x05356294
history ring B               0x053570b0    100 records of 36 B
  newest index / next slot   0x053570a8 / 0x053570ac
===========================  ============  =====================================

The pending list is indexed **by priority**, which is the fact that matters:
``0x2bf6d4`` stores every caution in the slot for its priority but publishes only
the highest, so *one caution per priority survives and the rest sit underneath*.
That is why killing the DSP complaint (priority 4) revealed code 61 (priority 3):
61 had been pending all along.  It also means a slot can hide a peer — 61 and 64
share priority 3 — so the **history ring is the honest instrument**: ``0x2bf040``
appends every raise, deduplicating only against the immediately preceding one.

Two tables in MAIN's own image decode the result, and neither has to be guessed:

* ``0xA40A5288`` — ``{u32 code, u32 e_number}`` until code 100.  Code 2 is
  ``E-7010``, 61 is ``E-7020``, 66 is ``E-8308``.
* ``0xA40A5348`` — 20-byte attributes indexed by code:
  ``{code, qualifier, flag, priority, extra}``.

The GUI's own ``{code, id}`` table at ``0xb456e0`` plus its string index at
``0xb3d468`` supply the banner text, and agreeing with MAIN's E-numbers is a
cross-check rather than an assumption.

The device cautions do **not** come through ``Caution_Set``.  ``0x24fd20`` owns
an eight-entry device-status array and turns the first failed device into a code
(see ``DEVICE_CAUTION`` below), which is why no call site in the image passes 61,
2 or 14 as an immediate and why a breakpoint on ``Caution_Set`` never sees them.
The array is a plain word each, so ``--live`` alone already answers *which device
is broken* without stopping the machine at all.

    python -m tools.cdj_main.caution --live --seconds 90
    python -m tools.cdj_main.caution --live --trace          # who reported what
    python -m tools.cdj_main.caution --live --env CDJ_USB_ABSENT=1 --save absent.txt
    python -m tools.cdj_main.caution --words absent.txt

``--live`` runs MAIN **alone**, without the Blackfin board.  That is deliberate:
the trace stops the machine at every breakpoint hit, and a two-board run loses
its timing races when it is starved.  The price is that device 5, the GUI link,
reports failed -- it has no caution code, so it raises no banner, but it does
stop ``0x24fd20``'s scan before device 6, so a MAIN-only run cannot see the disc
drive's complaint.  Use ``boot_vm --caution`` for that.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
from tools.paths import FIRMWARE, QEMU, qemu_environment
MAIN_BIN = Path(os.environ.get("CDJ_MAIN_BIN", FIRMWARE / "main-unpacked.bin"))
GUI_ELF = Path(os.environ.get("CDJ_GUI_ELF", FIRMWARE / "gui-boot-memory.elf"))
TEMP = Path(os.environ.get("TEMP", "."))
PORT = int(os.environ.get("CDJ_CAUTION_PORT", "5990"))

# The unpacked application is linked at 0xA4000000 and file offset == address.
MAIN_BASE = 0xA4000000
E_NUMBER_TABLE = 0xA40A5288      # {u32 code, u32 e_number}, terminated by code 100
ATTRIBUTE_TABLE = 0xA40A5348     # {code, qualifier, flag, priority, extra}, 20 B
ATTRIBUTE_STRIDE = 20

# The GUI image's banner tables (Blackfin, little-endian, UTF-16LE strings).
GUI_CODE_TABLE = 0x00B456E0      # {u32 code, u32 string_id}, 84 entries
GUI_CODE_ENTRIES = 84
GUI_STRING_INDEX = 0x00B3D468    # 298 pointers, index = string id

PUBLISHED = {"A": 0x053560B0, "B": 0x053560B8}
PENDING = {"A": 0x053560C8, "B": 0x05356118}
PENDING_WINNER = {"A": 0x0535616C, "B": 0x053561CC}
CURRENT = {"A": 0x05357EC4, "B": 0x05357EC8}
HISTORY = {"A": 0x05356298, "B": 0x053570B0}
HISTORY_INDEX = {"A": 0x05356290, "B": 0x053570A8}
SLOTS = 5                        # priorities 0..4
SLOT_WORDS = 4                   # {code, sub, arg1, arg2}
HISTORY_DEPTH = 100
HISTORY_WORDS = 9                # 36 bytes

# Caution_Set(code, sub, ptr) and its wrappers, as P0 addresses -- which is how
# the firmware itself names them (the literal pools carry 0x4250f6c, not
# 0xa4250f6c), and therefore what the CPU's PC reads as.
CAUTION_SET = 0x04250E68
CAUTION_ENTRIES = {
    0x0424FD20: "DeviceStatus(device, state)",
    0x042CD630: "Caution_Post(code, sub)",
    0x04250E68: "Caution_Set(code, sub, ptr)",
    0x04250F6C: "Caution_Set(code, sub)",
    0x04250F90: "Caution_Set(code, sub, arg)",
    0x04250D48: "Caution_Clear(code, sub, ptr)",
    0x04250E44: "Caution_Clear(code, sub)",
}

# `0x24fd20` is where the device cautions are actually born, and it is *not*
# `Caution_Set`.  It keeps an eight-entry status array at 0x04c0875c, writes
# `array[device] = state` (1 = up, 2 = failed), scans for the first failure and
# turns the device index into a caution code before tail-calling the poster at
# `0x2cd630`.  That indirection is why no call site in the whole image passes 61,
# 2 or 14 as an immediate: those codes exist only inside this switch.
#
# Who reports which device is read off the call sites of 0x24fd20, with the
# device number the immediate in r4 at each one:
#
#   1  0x1c77dc  the DSP bring-up            -> 2   E-7010
#   2  0x261bac                              -> 62  E-7021 PHY CHIP
#   3  0x2dea9e  USBFD_TSK                   -> 61  E-7020 USB-B
#   4  0x28ceb8 / PnlCom_SndTASK             -> 63  E-7022
#   5  GuiCom_SndTASK, 0x2154f0              -> (none: the link raises no banner)
#   6  0x109be2, inside ATAPI_TSK            -> 1   E-7001 DISC DRIVE
#   7  0x28ceb8 / PnlCom_SndTASK             -> 68  E-7025 CDC
#
# The scan walks the array in pairs and stops at the first entry that has not
# been reported yet, so a device failing does not guarantee its code is the one
# published -- device 5 failing hides device 6 completely.
DEVICE_CAUTION = {1: 2, 2: 62, 3: 61, 4: 63, 6: 1, 7: 68}
DEVICE_STATUS_ARRAY = 0x04C0875C
DEVICE_SLOTS = 8
DEVICE_STATE = {0: "not reported", 1: "up", 2: "FAILED"}

# The raise side only, by default.  Breaking on the clear side as well costs the
# run everything: `0x2918ea` clears the same caution about 3 800 times a second,
# and a 90-second trace of that made 334 842 stops and left MAIN with 481 log
# lines where an untraced run has 77 041.  The set entries fire about five times
# a second and cost 7 %.
CAUTION_DEFAULT_TRACE = (0x0424FD20, 0x042CD630, 0x04250F6C, 0x04250F90)

# The firmware's own error log, which is a different instrument from the caution
# store: a caution is what the machine decided to *show*, an error-log record is
# what a single function decided to *report*, and there are 1 594 call sites.
#
# `0x1df204(fnc, code, prm)` appends `{tick, tid, fnc, u16 code, prm}` to a
# 32-record ring and, when the byte at 0x07d93f04 is set, also prints it as
# `err : fnc=%lXh - %u, prm=%lXh, tsk=%u, time=%lu`.  The running index sits at
# 0x0483eb10, which is exactly ring + 32*20 -- the two addresses confirm each
# other, so neither the depth nor the stride is a guess.
#
# `0x1df2ae` (critical) and `0x1df322` (warning) print the same shape and write
# no ring at all, so this one address is the whole log.
#
# `fnc` is not an address but a constant per call site, built as one literal, so
# a record can be tied back to the instruction that raised it by looking the
# value up in the literal pools -- see `errlog_sites`.
ERRLOG = 0x0483E890
ERRLOG_INDEX = 0x0483EB10
ERRLOG_DEPTH = 32
ERRLOG_WORDS = 5

# The filesystem layer's drive letters, which is a different question from "is
# the card mounted".  `0x2dfd84` is the wide-character open every database path
# goes through; it takes the *first character of the path*, folds it to lower
# case, and indexes a 12-byte record with it -- so `b:/PIONEER/rekordbox/
# export.pdb` succeeds or fails on what letter `b` happens to be, no matter what
# the block layer underneath is doing.
#
# The base is not written down anywhere as a constant.  `0x2dfd84` computes
# `0x05762fe8 + letter*12` and then reads two fields at -1164 and -1160, and
# `0x188cee` reads the same type word from `0x05762b60 + letter*12`; the two
# agree on a record at `0x05762b5c + letter*12`, `{u32 flags, u16 type}`.
#
# The type is what the open switches on: 2, 3, 4 and 5 each have an arm and
# everything else falls through to the default.  A letter nobody mounted reads 0.
VOLUME_RECORD = 0x05762B5C
VOLUME_STRIDE = 12
VOLUME_FIRST = "a"
VOLUME_LETTERS = 16
VOLUME_TYPE = {
    2: "type 2 (0x19711c)", 3: "type 3 (FAT, the arm with the 'b' special case)",
    4: "type 4 (0x2dfea4)", 5: "type 5 (0x2dff04)",
}

# Every region the decoder wants, as (address, words).  Kept in one place so a
# saved dump and a live read cover exactly the same ground.
def regions() -> list[tuple[int, int]]:
    spans = [(0x053560B0, 6), (0x05357EC4, 2), (DEVICE_STATUS_ARRAY, DEVICE_SLOTS)]
    for key in ("A", "B"):
        spans.append((PENDING[key], SLOTS * SLOT_WORDS))
        spans.append((PENDING_WINNER[key], SLOTS * SLOT_WORDS))
        spans.append((HISTORY_INDEX[key], 2))
        spans.append((HISTORY[key], HISTORY_DEPTH * HISTORY_WORDS))
    spans.append((ERRLOG, ERRLOG_DEPTH * ERRLOG_WORDS + 1))
    spans.append((VOLUME_RECORD + ord(VOLUME_FIRST) * VOLUME_STRIDE,
                  VOLUME_LETTERS * VOLUME_STRIDE // 4))
    return spans


# ---------------------------------------------------------------- static tables

@lru_cache(maxsize=1)
def image() -> bytes:
    return MAIN_BIN.read_bytes()


@lru_cache(maxsize=1)
def e_numbers() -> dict[int, int]:
    """code -> E-number, from MAIN's own table.  Terminated by code 100."""
    data, out, offset = image(), {}, E_NUMBER_TABLE - MAIN_BASE
    while offset + 8 <= len(data):
        code, number = struct.unpack_from("<2I", data, offset)
        if code == 100:
            break
        out[code] = number
        offset += 8
    return out


@lru_cache(maxsize=1)
def priorities() -> dict[int, int]:
    """code -> priority, from the 20-byte attribute table."""
    data, out = image(), {}
    for code in range(0, 128):
        offset = ATTRIBUTE_TABLE - MAIN_BASE + code * ATTRIBUTE_STRIDE
        if offset + ATTRIBUTE_STRIDE > len(data):
            break
        stored, _, _, priority, _ = struct.unpack_from("<5I", data, offset)
        if stored != code:
            break
        out[code] = priority
    return out


@lru_cache(maxsize=1)
def banners() -> dict[int, str]:
    """code -> the text the GUI would put on screen.  Empty if the ELF is absent."""
    if not GUI_ELF.exists():
        return {}
    blob = GUI_ELF.read_bytes()
    (phoff,) = struct.unpack_from("<I", blob, 0x1C)
    entsize, count = struct.unpack_from("<HH", blob, 0x2A)
    segments = []
    for index in range(count):
        kind, offset, vaddr, _, filesz, _ = struct.unpack_from(
            "<6I", blob, phoff + index * entsize)
        if kind == 1:
            segments.append((vaddr, offset, filesz))

    def read(address: int, size: int) -> bytes | None:
        for vaddr, offset, filesz in segments:
            if vaddr <= address < vaddr + filesz:
                return blob[offset + address - vaddr:offset + address - vaddr + size]
        return None

    def word(address: int) -> int | None:
        raw = read(address, 4)
        return struct.unpack("<I", raw)[0] if raw else None

    def text(pointer: int, limit: int = 96) -> str:
        raw = read(pointer, limit * 2) or b""
        out = []
        for i in range(0, len(raw), 2):
            (unit,) = struct.unpack_from("<H", raw, i)
            if unit == 0:
                break
            out.append(chr(unit) if 32 <= unit < 0x2000 else " ")
        return "".join(out).strip()

    out = {}
    for index in range(GUI_CODE_ENTRIES):
        code = word(GUI_CODE_TABLE + 8 * index)
        string_id = word(GUI_CODE_TABLE + 8 * index + 4)
        if code is None or string_id is None or string_id >= 298:
            continue
        pointer = word(GUI_STRING_INDEX + 4 * string_id)
        if pointer:
            out[code] = text(pointer)
    return out


def describe(code: int) -> str:
    """One line for a caution code: E-number, priority and the banner text."""
    parts = []
    number = e_numbers().get(code)
    if number:
        # The stored word is hex-coded decimal: 0x7010 is E-7010, 0x8709 is
        # E-8709.  Printing it as a decimal integer gives E-28688.
        parts.append("E-%04x" % number)
    priority = priorities().get(code)
    if priority is not None:
        parts.append("prio %d" % priority)
    banner = banners().get(code)
    if banner:
        parts.append(banner)
    return "  ".join(parts) if parts else "(not in either table)"


# ------------------------------------------------------------------ the reading

# `xp` answers `053560c8: 0x00000000 0xffffffff ...` -- a bare address and
# 0x-prefixed cells -- and the telnet monitor echoes every keystroke with escape
# sequences around it, so the escapes have to go before anything is matched.
WORD_LINE = re.compile(r"^([0-9a-f]{8,16}):((?:\s+(?:0x)?[0-9a-f]{8})+)\s*$")


def parse_words(text: str) -> dict[int, int]:
    """Words out of QEMU monitor `xp` output, or a file holding the same."""
    words: dict[int, int] = {}
    for line in re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text).splitlines():
        match = WORD_LINE.match(line.strip())
        if not match:
            continue
        base = int(match.group(1), 16)
        for index, cell in enumerate(match.group(2).split()):
            words[base + 4 * index] = int(cell, 16)
    return words


def read_regions(monitor: socket.socket) -> dict[int, int]:
    """Ask the monitor for every region and collect the words."""
    for address, count in regions():
        monitor.sendall(("xp /%dwx %#x\n" % (count, address)).encode())
        time.sleep(0.15)
    text = ""
    try:
        while True:
            chunk = monitor.recv(1 << 16)
            if not chunk:
                break
            text += chunk.decode("ascii", "replace")
    except socket.timeout:
        pass
    return parse_words(text)


def dump_text(words: dict[int, int]) -> str:
    """Render the words back into the same form `xp` produced, four per line."""
    lines = []
    for address, count in regions():
        for start in range(0, count, 4):
            cells = [words.get(address + 4 * (start + i))
                     for i in range(min(4, count - start))]
            if any(cell is None for cell in cells):
                continue
            lines.append("%08x: %s" % (address + 4 * start,
                                       " ".join("%08x" % cell for cell in cells)))
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------- the report

def report(words: dict[int, int], out=sys.stdout) -> None:
    missing = [hex(a) for a, _ in regions() if a not in words]
    if missing:
        print("# regions missing from the dump: " + ", ".join(missing), file=out)

    if DEVICE_STATUS_ARRAY in words:
        print(file=out)
        print("== device status, 0x%08x ==" % DEVICE_STATUS_ARRAY, file=out)
        for device in range(DEVICE_SLOTS):
            state = words.get(DEVICE_STATUS_ARRAY + 4 * device)
            code = DEVICE_CAUTION.get(device)
            print("  device %d  %-12s %s"
                  % (device, DEVICE_STATE.get(state, "0x%x" % (state or 0)),
                     ("-> code %d  %s" % (code, describe(code))) if code
                     else "(no caution code)"), file=out)

    for key in ("A", "B"):
        published = words.get(PUBLISHED[key])
        current = words.get(CURRENT[key])
        print(file=out)
        print("== list %s ==" % key, file=out)
        if published is None:
            print("  (no data)", file=out)
            continue
        code, qualifier = published & 0xFF, (published >> 8) & 0xFFFFFF
        print("  published  0x%08x  ->  code %-3d  qualifier %-3d   %s"
              % (published, code, qualifier,
                 describe(code) if code else "nothing on screen"), file=out)
        print("  current priority %s" % ("-" if current is None else current), file=out)
        for slot in range(SLOTS):
            base = PENDING[key] + slot * SLOT_WORDS * 4
            cells = [words.get(base + 4 * i) for i in range(SLOT_WORDS)]
            if cells[0] is None:
                continue
            code, sub, arg1, arg2 = cells
            mark = "<-- published" if slot == current else ""
            if not code and sub in (0, 0xFFFFFFFF):
                print("  slot %d  empty %s" % (slot, mark), file=out)
                continue
            print("  slot %d  code %-3d  sub %-11s args %08x %08x  %s %s"
                  % (slot, code, signed(sub), arg1 or 0, arg2 or 0,
                     describe(code), mark), file=out)

    for key in ("A", "B"):
        entries = history(words, key)
        print(file=out)
        print("== history %s: %d record%s, oldest first =="
              % (key, len(entries), "" if len(entries) == 1 else "s"), file=out)
        for order, (slot, record) in enumerate(entries):
            code, sub, arg1, arg2 = record[0:4]
            print("  %2d  [%2d]  code %-3d  sub %-11s args %08x %08x  %s"
                  % (order, slot, code, signed(sub), arg1, arg2, describe(code)),
                  file=out)
        if not entries:
            print("  (nothing was ever raised)", file=out)

    codes = sorted({record[0] for key in ("A", "B")
                    for _, record in history(words, key) if record[0]})
    print(file=out)
    print("== every code this boot ==", file=out)
    for code in codes:
        print("  code %-3d  %s" % (code, describe(code)), file=out)
    if not codes:
        print("  (none)", file=out)

    volume_report(words, out=out)
    errlog_report(words, out=out)


def volume_report(words: dict[int, int], out=sys.stdout) -> None:
    """Which drive letters the filesystem layer has, and of what type."""
    first = VOLUME_RECORD + ord(VOLUME_FIRST) * VOLUME_STRIDE
    if first not in words:
        return
    print(file=out)
    print("== drive letters, 0x%08x ==" % VOLUME_RECORD, file=out)
    shown = 0
    for index in range(VOLUME_LETTERS):
        letter = chr(ord(VOLUME_FIRST) + index)
        base = VOLUME_RECORD + ord(letter) * VOLUME_STRIDE
        flags = words.get(base)
        packed = words.get(base + 4)
        if flags is None or packed is None:
            continue
        kind = packed & 0xFFFF
        if not kind and not flags:
            continue
        shown += 1
        print("  %s:  type %-3d %-46s flags %08x"
              % (letter, kind, VOLUME_TYPE.get(kind, "(no arm in 0x2dfd84)"),
                 flags), file=out)
    if not shown:
        print("  (no letter is mounted -- every record reads zero)", file=out)


def errlog(words: dict[int, int]) -> list[tuple[int, list[int]]]:
    """The error ring in write order, oldest first, as (slot, record) pairs.

    The index at ``ERRLOG_INDEX`` is a running count, not a slot, so it says by
    itself whether the ring has wrapped and where the oldest record is: below
    the depth nothing has been overwritten yet, above it the next slot to be
    written is the oldest one still there.
    """
    total = words.get(ERRLOG_INDEX)
    if total is None:
        return []
    order = (list(range(total % ERRLOG_DEPTH, ERRLOG_DEPTH))
             + list(range(0, total % ERRLOG_DEPTH)) if total > ERRLOG_DEPTH
             else list(range(0, total)))
    out = []
    for slot in order:
        base = ERRLOG + slot * ERRLOG_WORDS * 4
        record = [words.get(base + 4 * i) for i in range(ERRLOG_WORDS)]
        if any(cell is None for cell in record) or not any(record):
            continue
        out.append((slot, record))
    return out


@lru_cache(maxsize=None)
def errlog_sites(fnc: int) -> str:
    """Where a `fnc` constant is built, as `instruction in function`.

    The value reaches the log as one literal-pool word, so the pool is the whole
    reference: find the word, find what loads it.  Needs the disassembly index,
    which is a minute's work the first time and absent on a fresh checkout, so a
    failure here degrades to an empty column rather than to no report.
    """
    try:
        from . import sh_index
    except ImportError:                                   # pragma: no cover
        return ""
    try:
        data = sh_index.image()
        sites = []
        needle = struct.pack("<I", fnc)
        position = data.find(needle)
        while position >= 0 and len(sites) < 4:
            if position % 4 == 0:
                sites.extend(sh_index.loaders(position))
            position = data.find(needle, position + 1)
        return ", ".join("%06x in %s" % (site, sh_index.label(sh_index.function_of(site)))
                         for site in sorted(set(sites))[:3])
    except Exception:                                     # pragma: no cover
        return ""


def errlog_report(words: dict[int, int], out=sys.stdout) -> None:
    """`0x1df204`'s ring: what each function reported, oldest first."""
    if ERRLOG_INDEX not in words:
        return
    entries = errlog(words)
    total = words.get(ERRLOG_INDEX, 0)
    print(file=out)
    print("== error log, 0x%08x: %d of %d record%s, oldest first =="
          % (ERRLOG, len(entries), total, "" if total == 1 else "s"), file=out)
    for order, (slot, record) in enumerate(entries):
        tick, tid, fnc, packed, prm = record
        print("  %2d  [%2d]  time %-10u tsk %-4s fnc %08x - %-5d prm %08x  %s"
              % (order, slot, tick, signed(tid), fnc, packed & 0xFFFF, prm,
                 errlog_sites(fnc)), file=out)
    if not entries:
        print("  (nothing was ever reported)", file=out)


def signed(value: int | None) -> str:
    if value is None:
        return "-"
    return str(value - (1 << 32) if value >= 1 << 31 else value)


def history(words: dict[int, int], key: str) -> list[tuple[int, list[int]]]:
    """The ring in write order, oldest first, as (slot, record) pairs.

    ``0x2bf040`` keeps two words: the slot the newest record went into and the
    slot the next one will use, the second wrapping at 100.  Before the ring has
    wrapped those are ``n`` and ``n+1``; afterwards the next slot is also the
    oldest record, which is what makes the order recoverable.
    """
    newest = words.get(HISTORY_INDEX[key])
    nxt = words.get(HISTORY_INDEX[key] + 4)
    if newest is None or nxt is None:
        return []
    wrapped = nxt <= newest and newest != 0 or nxt == 0 and newest == HISTORY_DEPTH - 1
    order = (list(range(nxt, HISTORY_DEPTH)) + list(range(0, nxt)) if wrapped
             else list(range(0, nxt)))
    out = []
    for slot in order:
        base = HISTORY[key] + slot * HISTORY_WORDS * 4
        record = [words.get(base + 4 * i) for i in range(HISTORY_WORDS)]
        if any(cell is None for cell in record):
            continue
        if not any(record):
            continue
        out.append((slot, record))
    return out


# --------------------------------------------------------------- the gdb client

def unpack_rsp(body: bytes) -> bytes:
    """Undo the stub's run-length and escape encoding."""
    out = bytearray()
    index = 0
    while index < len(body):
        char = body[index]
        if char == 0x2A and out and index + 1 < len(body):      # '*'
            out.extend(out[-1:] * (body[index + 1] - 29))
            index += 2
        elif char == 0x7D and index + 1 < len(body):            # '}'
            out.append(body[index + 1] ^ 0x20)
            index += 2
        else:
            out.append(char)
            index += 1
    return bytes(out)


class Stub:
    """The smallest gdb remote-serial client that can hold a breakpoint.

    Connecting stops the machine -- that is QEMU's behaviour, not a choice here
    -- so every user of this class has to `resume()` before the guest runs again.
    """

    def __init__(self, port: int, timeout: float = 5.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = b""

    def send(self, body: str) -> None:
        packet = "$%s#%02x" % (body, sum(body.encode()) & 0xFF)
        self.sock.sendall(packet.encode())

    def packet(self, timeout: float | None = None) -> str:
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            start = self.buffer.find(b"$")
            end = self.buffer.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(self.buffer) >= end + 3:
                body = self.buffer[start + 1:end]
                self.buffer = self.buffer[end + 3:]
                self.sock.sendall(b"+")
                return unpack_rsp(body).decode("ascii", "replace")
            if deadline is not None:
                self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise EOFError("the stub closed the connection")
            self.buffer += chunk

    def command(self, body: str, timeout: float | None = 5.0) -> str:
        self.send(body)
        return self.packet(timeout)

    def registers(self) -> list[int]:
        raw = bytes.fromhex(re.sub(r"[^0-9a-fA-F]", "", self.command("g")))
        return [int.from_bytes(raw[i:i + 4], "little")
                for i in range(0, len(raw) - 3, 4)]

    def close(self) -> None:
        try:
            self.send("D")
            time.sleep(0.05)
        except OSError:
            pass
        self.sock.close()


# QEMU's SH-4 gdb register order: r0..r15, pc, pr, gbr, vbr, ...
REG_PC, REG_PR = 16, 17


def trace_thread(port: int, addresses: list[int], budget: int,
                 stop: threading.Event,
                 hits: dict[tuple[int, ...], list],
                 cap: int = 0,
                 watches: list[tuple[int, int]] | None = None) -> None:
    """Hold breakpoints on the caution entry points and count what arrives.

    `watches` are (address, length) pairs for gdb write watchpoints (`Z2`):
    the stop reports the pc of the storing instruction with the same register
    key as a breakpoint hit, so a hit list answers "who writes this word" --
    the question a literal search cannot when the address is base+offset
    (trackload-60/61: the status record's time fields).  A watchpoint stop
    needs no step-over; QEMU resumes past the access on its own.

    Both wrappers are worth breaking on, not only `Caution_Set` itself: a hit on
    `0x250e68` reports `pr` inside the wrapper that called it, which says
    nothing.  The wrapper's own hit carries the caller the question is about.

    Repeats are counted rather than listed.  One caution can be re-raised
    hundreds of times in a minute, and a transcript of that hides the one line
    that matters.

    **Four argument registers, not two.**  SuperH passes r4..r7, and the
    questions that need a trace at all are usually about the third: the DB
    client's dispatch reads its message code into r6, so a two-register key
    turns "which responses arrive" into "some responses arrive".  `g` already
    carries the whole file, so reporting r6 and r7 costs nothing per hit.

    **And a hard per-site cap, separate from `budget`.**  `budget` only counts
    hits that said nothing new, so a site whose arguments differ every time --
    a pool-allocated message pointer, say -- is never dropped: `r072` took
    231 173 stops on one breakpoint, which is not a measurement of anything but
    of the guest being held.  `cap` is the absolute number of stops a single
    address may cost before its breakpoint is removed, whatever it reports.

    **And the guest is stepped over the breakpoint before it is resumed.**
    Without that, `c` returns to the same instruction, so every stop reports the
    same registers and the guest makes no progress -- see the comment at the
    resume below for the r151 measurement that proves it.  Every trace taken
    before 2026-08-07 therefore says only "this address was reached", never how
    often and never with a second argument set.
    """
    # QEMU opens the stub a moment after it starts; keep knocking for a while.
    deadline = time.monotonic() + 15.0
    while True:
        try:
            stub = Stub(port, timeout=10.0)
            break
        except OSError as error:
            if time.monotonic() > deadline or stop.is_set():
                print("# trace: cannot reach the stub: %s" % error)
                return
            time.sleep(0.2)
    placed = []
    watched = []
    started = time.monotonic()
    try:
        for address in addresses:
            if stub.command("Z0,%x,2" % address) == "OK":
                placed.append(address)
            else:
                print("# trace: the stub refused a breakpoint at 0x%08x" % address)
        watched = []
        for address, length in watches or []:
            if stub.command("Z2,%x,%x" % (address, length)) == "OK":
                watched.append((address, length))
            else:
                print("# trace: the stub refused a watchpoint at 0x%08x" % address)
        if not placed and not watched:
            return
        print("# trace: breakpoints at "
              + ", ".join("0x%08x" % address for address in placed)
              + (" watchpoints at " + ", ".join("0x%08x:%d" % w for w in watched)
                 if watched else ""))
        spent = {address: 0 for address in placed}
        total = {address: 0 for address in placed}
        stub.send("c")
        while not stop.is_set():
            try:
                signal = stub.packet(timeout=1.0)
            except socket.timeout:
                continue
            except (OSError, EOFError):
                return
            if not signal.startswith(("T", "S")):
                continue
            regs = stub.registers()
            if len(regs) > REG_PR:
                pc = regs[REG_PC]
                key = (pc, regs[4], regs[5], regs[6], regs[7], regs[REG_PR])
                # CDJ_TRACE_PEEK=<offset>: also the word at r4 + offset -- the
                # field of the object a method was handed, which the argument
                # registers alone never show.
                peek = os.environ.get("CDJ_TRACE_PEEK")
                if peek:
                    reply = stub.command("m%x,4" % ((regs[4] + int(peek, 0)) & 0xFFFFFFFF))
                    word = (int.from_bytes(bytes.fromhex(reply), "little")
                            if len(reply) == 8 and not reply.startswith("E") else -1)
                    key = key + (word,)
                record = hits.get(key)
                if record is None:
                    # [count, first seen, last seen], both in seconds since the
                    # tracer attached -- an order of arrival is worth as much as
                    # the count when two sites are being compared.
                    elapsed = time.monotonic() - started
                    record = hits[key] = [0, elapsed, elapsed]
                record[0] += 1
                record[2] = time.monotonic() - started
                count = record[0]
                total[pc] = total.get(pc, 0) + 1
                # A site that fires without end is not worth the whole run, but a
                # plain hit cap drops the breakpoint while it still has something
                # to say -- 20 000 repeats of "device 1 is up" hid the one call
                # that reported device 3 failed.  So count only hits that told us
                # nothing new, and reset on every first-time argument set.
                spent[pc] = 0 if count == 1 else spent.get(pc, 0) + 1
                drop = None
                if budget and spent[pc] == budget:
                    drop = "said nothing new in %d hits" % budget
                elif cap and total[pc] == cap:
                    drop = "reached the hard cap of %d stops" % cap
                if drop:
                    stub.command("z0,%x,2" % pc, timeout=2.0)
                    if pc in placed:
                        placed.remove(pc)
                    print("# trace: 0x%08x %s, breakpoint removed at t%.1f"
                          % (pc, drop, time.monotonic() - started))
                elif pc in placed:
                    # STEP OVER THE BREAKPOINT BEFORE RESUMING.  A plain `c`
                    # from a PC that still carries a BP_GDB breakpoint comes
                    # straight back to the same instruction, so the guest never
                    # advances and every stop reports the identical register
                    # file.  That is not a theory: r151 took 400 stops at
                    # 0x0413ca94 in 0.3 s, all with the same message pointer
                    # 0x076bac70 and the same code 0x3ef, and 0x0414ea38 --
                    # which is called *later in the handling of that same
                    # message* -- did not fire once until 0x0413ca94's
                    # breakpoint had been dropped, then took its own 400 with
                    # the same pointer.  Strictly sequential, never interleaved:
                    # one message reported eight hundred times.  r045 shows the
                    # same shape at the budget of the day (13 and 13, identical
                    # registers) and r072's 231 173 stops are the extreme of it.
                    # So `--trace-cap` was treating the symptom; this is the
                    # cause, and until now a trace could report "this address
                    # was reached" and nothing else -- never a count, never a
                    # second argument set.
                    #
                    # Removing the breakpoint, single-stepping and re-inserting
                    # is the standard remedy.  The step's own stop packet is
                    # consumed here so the loop above does not count it as a
                    # hit; if the step lands on another traced address that one
                    # hit is lost, which is the cheap end of the trade.
                    stub.command("z0,%x,2" % pc, timeout=2.0)
                    try:
                        stub.send("s")
                        stub.packet(timeout=5.0)
                    except (OSError, EOFError, socket.timeout):
                        return
                    stub.command("Z0,%x,2" % pc, timeout=2.0)
            stub.send("c")
    finally:
        for address in placed:
            try:
                stub.command("z0,%x,2" % address, timeout=2.0)
            except (OSError, EOFError, socket.timeout):
                pass
        for address, length in watched:
            try:
                stub.command("z2,%x,%x" % (address, length), timeout=2.0)
            except (OSError, EOFError, socket.timeout):
                pass
        stub.close()


# ------------------------------------------------------------------- the runner

def run_main_only(seconds: float, extra_env: dict[str, str], sd: str | None,
                  trace: list[int] | None, budget: int = 4000,
                  stderr: str | None = None) -> dict[int, int]:
    """Boot MAIN alone, let it run, then read the caution store back.

    `stderr` names a file to keep the board's stderr in.  Without it the stream
    is discarded, and every `CDJ_*_TRACE` line goes with it -- which is silent,
    because the run itself looks perfectly normal.
    """
    env = qemu_environment()
    env.setdefault("CDJ_TMU_FREQ", "54000000")
    env.setdefault("CDJ_SD_INSERT", "25")
    if sd:
        env.setdefault("CDJ_PANEL_FRAME",
                       "00000000000000000000000000000000000400000000")
    env.update(extra_env)

    log = TEMP / "caution-main.log"
    if log.exists():
        log.unlink()
    print("# MAIN alone: %s -M cdj2000-main, %g s" % (QEMU.name, seconds))
    if extra_env:
        print("# env: " + ", ".join("%s=%s" % item for item in extra_env.items()))
    stderr_file = open(stderr, "wb") if stderr else subprocess.DEVNULL
    if stderr:
        print("# board stderr -> %s" % stderr)
    board = subprocess.Popen(
        [
            str(QEMU), "-M", "cdj2000-main", "-bios", str(FIRMWARE / "main-firmware.bin"),
            "-display", "none", "-no-reboot", "-d", "unimp", "-D", str(log),
            "-serial", "null", "-serial", "null",
            "-serial", (f"file:{TEMP / 'caution-console.txt'}"
                        if env.get("CDJ_DEBUG_CONSOLE") else "null"),
            "-monitor", f"telnet:127.0.0.1:{PORT},server,nowait",
            "-gdb", f"tcp:127.0.0.1:{PORT + 1}",
            # Frozen at reset while tracing, because caution 61 is raised inside
            # the first second: a tracer that attaches to a running machine has
            # already missed it.  The tracer's own `c` starts the guest.
            *(["-S"] if trace else []),
            *(["-drive", f"if=sd,format=raw,file={sd}"] if sd else []),
        ],
        env=env, stdout=subprocess.DEVNULL, stderr=stderr_file,
    )
    if stderr:
        stderr_file.close()

    stop = threading.Event()
    hits: dict[tuple[int, ...], list] = {}
    tracer = None
    monitor = None
    try:
        if trace:
            time.sleep(0.5)
            tracer = threading.Thread(target=trace_thread,
                                      args=(PORT + 1, trace, budget, stop, hits),
                                      daemon=True)
            tracer.start()
        time.sleep(3)
        monitor = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        monitor.settimeout(1.0)
        time.sleep(max(0.0, seconds - 3))
        words = read_regions(monitor)
        monitor.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        stop.set()
        if tracer:
            tracer.join(timeout=5)
        if monitor:
            monitor.close()
        try:
            board.wait(timeout=20)
        except subprocess.TimeoutExpired:
            board.kill()

    if hits:
        print("\n# caution calls, first seen first, repeats counted")
        for (entry, first, second, _r6, _r7, caller, *_peek), record in hits.items():
            count = record[0]
            if entry == 0x0424FD20:
                code = DEVICE_CAUTION.get(first)
                meaning = "device %d %s%s" % (
                    first, DEVICE_STATE.get(second, "state %d" % second),
                    "  -> code %d  %s" % (code, describe(code))
                    if code and second == 2 else "")
            else:
                meaning = describe(first & 0xFF)
            print("  %-28s %-4d %-11s from 0x%08x  x%-5d %s"
                  % (CAUTION_ENTRIES.get(entry, "0x%08x" % entry), first & 0xFF,
                     signed(second), caller, count, meaning))
    elif trace:
        print("\n# no breakpoint was ever hit")
    if log.exists():
        text = log.read_text(errors="replace")
        if text.strip():
            print("# %d unimplemented-access lines in %s" % (len(text.splitlines()), log))
    return words


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--live", action="store_true",
                        help="boot MAIN alone and read the store out of it")
    parser.add_argument("--seconds", type=float, default=90.0,
                        help="wall-clock budget for a live run")
    parser.add_argument("--sd", default=os.environ.get("CDJ_SD_IMAGE"),
                        help="FAT32 card image to attach")
    parser.add_argument("--trace", nargs="?", default=None, metavar="ADDR[,ADDR]",
                        const=",".join("%#x" % entry
                                       for entry in CAUTION_DEFAULT_TRACE),
                        help="hold gdb breakpoints here and report r4/r5/pr on "
                             "every hit; defaults to the three caution *set* "
                             "entries, so a wrapper hit names the real caller")
    parser.add_argument("--trace-max", type=int, default=4000, metavar="N",
                        help="drop a breakpoint after it has fired this often, "
                             "0 to keep every one.  The set entries fire about "
                             "five times a second and cost the run 7 %%; the "
                             "cap is there for the clear side, which does not")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="extra environment for QEMU, e.g. CDJ_USB_ABSENT=1")
    parser.add_argument("--stderr", metavar="FILE",
                        help="keep the board's stderr here; without it every "
                             "CDJ_*_TRACE line is discarded, silently")
    parser.add_argument("--save", metavar="FILE",
                        help="write the raw words next to the report")
    parser.add_argument("--words", metavar="FILE",
                        help="decode a saved dump instead of running anything")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if args.words:
        words = parse_words(Path(args.words).read_text(errors="replace"))
    elif args.live:
        extra = dict(item.split("=", 1) for item in args.env)
        entries = [int(item, 0) for item in args.trace.split(",")] if args.trace else []
        words = run_main_only(args.seconds, extra, args.sd, entries,
                              args.trace_max, args.stderr)
    else:
        parser.error("give --live or --words FILE")
        return 2

    if args.save:
        Path(args.save).write_text(dump_text(words))
        print("\n# %d words saved to %s" % (len(words), args.save))
    report(words)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
