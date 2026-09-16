"""Decode a BFIN_MAIN_LINK_DUMP: every record the link handed the GUI firmware.

The simulator appends each record it hands to the firmware's receive DMA as
``"SPRX"`` + little-endian 32-bit length + body (``dv-bfin_ppi.c``,
``bfin_sport_link_dump``). Status records are 64 bytes with command word 0.
Payloads can also be 64 bytes (notably short NXS browser lists), so length
alone cannot distinguish them. In cached-delivery mode the model repeats
status between MAIN transmissions; that count is not MAIN's send count
(``link-tx: sent`` in MAIN's -D log). Payloads are announced in status
halfwords 29 and 30 (count, length in halfwords) and fetched by firmware.

Printed, in record order:

* a line whenever one of the watched status halfwords changes -- 13 (protocol
  mode), 18 (source), 19, 20 (caution code), 26 (legacy media state), 29/30 (the
  announcement);
* every payload, identical consecutive ones collapsed with a repeat count.
  List answers (commands ``0x10..0x1f`` except ``0x19``, the layout of
  ``build_type1_list``) are decoded into their rows, player-state answers
  (``0x19``, ``build_player_state``) into their strings;
* a summary: records per length, the announcements (w29, w30) with the record
  range they stood in, and for each announced length whether a payload of
  that length ever went over.  An announcement that stands to the end of the
  dump with nothing delivered is the signature of a frame the link lost --
  the 896-byte track list of a playlist was found that way (2026-09-02).

The semantic labels above originate in CDJ-2000 analysis. In particular,
NXS halfword 26 is not a validated media-readiness indicator: the verified
native-load capture lists TESTTONE.WAV while it remains 0x1000. Report its
raw value, and use actual list replies to establish NXS browsing progress.
"""

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import collections
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

MAGIC = b"SPRX"
STATUS_LENGTH = 64
WATCHED_WORDS = (13, 18, 19, 20, 26, 29, 30)
PLAYER_STATE_COMMAND = 0x19
LIST_FIRST_ENTRY_WORD = 9
LIST_MAX_ENTRY_CHARS = 0x100


def iter_records(blob: bytes) -> Iterator[bytes]:
    """Yield the record bodies of a dump, resynchronising on the magic."""

    offset = 0
    while offset + 8 <= len(blob):
        if blob[offset:offset + 4] != MAGIC:
            offset += 1
            continue
        length = int.from_bytes(blob[offset + 4:offset + 8], "little")
        body = blob[offset + 8:offset + 8 + length]
        offset += 8 + length
        if len(body) == length:
            yield body


def words_of(body: bytes) -> tuple[int, ...]:
    return struct.unpack(f"<{len(body) // 2}H", body[: len(body) // 2 * 2])


def decode_list(words: tuple[int, ...]) -> tuple[dict[str, int], list[tuple[int, int, str]]]:
    """Split a type-1 list answer into its header and ``(attr, attr2, text)`` rows."""

    header = {
        "total": (words[1] << 16) | words[2],
        "cursor": (words[3] << 16) | words[4],
        "word5": words[5],
        "highlight": words[6],
        "flags": words[7],
        "entries": words[8],
    }
    rows = []
    offset = LIST_FIRST_ENTRY_WORD
    for _ in range(header["entries"]):
        if offset + 3 > len(words):
            break
        attr, attr2, length = words[offset], words[offset + 1], words[offset + 2]
        if length > LIST_MAX_ENTRY_CHARS or offset + 3 + length > len(words):
            break
        text = "".join(chr(word) for word in words[offset + 3:offset + 3 + length])
        rows.append((attr, attr2, text))
        offset += 3 + length
    return header, rows


def decode_player_state(words: tuple[int, ...]) -> tuple[list[int], list[str]]:
    """Return the nine head scalars and the seven strings of a command-0x19 answer."""

    scalars = [words[index] for index in range(3, 20, 2) if index < len(words)]
    strings = []
    offset = 21
    for index in range(7):
        if index:
            offset += 1                      # the length is the low half of a 32-bit field
        if offset >= len(words):
            break
        length = words[offset]
        if length > 0x20 or offset + 1 + length > len(words):
            break
        strings.append("".join(chr(word) for word in words[offset + 1:offset + 1 + length]))
        offset += 1 + length
    return scalars, strings


def printable(body: bytes, limit: int = 48) -> str:
    return "".join(chr(byte) if 32 <= byte < 127 else "." for byte in body[:limit])


@dataclass
class Summary:
    records: int = 0
    lengths: collections.Counter = field(default_factory=collections.Counter)
    delivered: collections.Counter = field(default_factory=collections.Counter)
    announcements: list[tuple[int, int, int, int]] = field(default_factory=list)  # w29, w30, first, last
    player_states: int = 0


def describe_payload(body: bytes, hex_words: int) -> list[str]:
    words = words_of(body)
    command = words[0] if words else 0
    lines = []
    if hex_words:
        lines.append("      " + " ".join("%04x" % word for word in words[:hex_words]))
    if 0x10 <= command <= 0x1F and command != PLAYER_STATE_COMMAND and len(words) > LIST_FIRST_ENTRY_WORD:
        header, rows = decode_list(words)
        lines.append("      list cursor %d: total %d, first %d, highlight %d, flags %#06x, %d entries"
                     % (command & 0xF, header["total"], header["cursor"], header["highlight"],
                        header["flags"], header["entries"]))
        for attr, attr2, text in rows:
            lines.append("        attr %#06x/%#06x  %r" % (attr, attr2, text))
    elif command == PLAYER_STATE_COMMAND:
        scalars, strings = decode_player_state(words)
        lines.append("      player state: scalars %s" % " ".join("%d" % value for value in scalars))
        for index, text in enumerate(strings, 1):
            lines.append("        string %d  %r" % (index, text))
    else:
        lines.append("      %s" % printable(body))
    return lines


def decode(blob: bytes, *, hex_words: int = 0, collapse: bool = True, limit: int = 0) -> tuple[list[str], Summary]:
    summary = Summary()
    lines: list[str] = []
    watched = None
    last_payload = None
    run = 0
    run_first = 0
    announcement = None
    announcement_first = 0

    def flush() -> None:
        nonlocal run, last_payload
        if last_payload is not None and run:
            lines.append("#%-6d payload %4d bytes x%d" % (run_first, len(last_payload), run))
            lines.extend(describe_payload(last_payload, hex_words))
        run = 0

    for index, body in enumerate(iter_records(blob), 1):
        summary.records += 1
        summary.lengths[len(body)] += 1
        if limit and index > limit:
            break
        words = words_of(body)
        if len(body) == STATUS_LENGTH and words[0] == 0:
            current = tuple(words[word] for word in WATCHED_WORDS)
            pair = (words[29], words[30])
            if pair != announcement:
                if announcement is not None:
                    summary.announcements.append((*announcement, announcement_first, index - 1))
                announcement, announcement_first = pair, index
            if current != watched:
                flush()
                last_payload = None
                lines.append("#%-6d status %s" % (index, " ".join(
                    "w%d=%s" % (word, ("%d" if word in (13, 29, 30) else "%04x") % value)
                    for word, value in zip(WATCHED_WORDS, current))))
                watched = current
            continue
        summary.delivered[len(body)] += 1
        if words and words[0] == PLAYER_STATE_COMMAND:
            summary.player_states += 1
        if collapse and body == last_payload:
            run += 1
            continue
        flush()
        last_payload, run, run_first = body, 1, index
    flush()
    if announcement is not None:
        summary.announcements.append((*announcement, announcement_first, summary.records))
    return lines, summary


def format_summary(summary: Summary) -> list[str]:
    lines = ["records: %d" % summary.records]
    lines.append("lengths: " + ", ".join(
        "%d B x%d" % (length, count) for length, count in sorted(summary.lengths.items())))
    lines.append("player-state answers (0x19): %d" % summary.player_states)
    lines.append("announcements (w29, w30 halfwords) by record range, and whether that length went over:")
    for w29, w30, first, last in summary.announcements:
        if not w29 and not w30:
            continue
        length = w30 * 2
        delivered = summary.delivered.get(length, 0)
        lines.append("  (%d, %d) = %4d bytes  records %d..%d  delivered x%d%s"
                     % (w29, w30, length, first, last, delivered,
                        "" if delivered else "  <-- never delivered"))
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", type=Path, help="the BFIN_MAIN_LINK_DUMP file")
    parser.add_argument("--hex", type=int, default=0, metavar="N",
                        help="also print the first N halfwords of every payload")
    parser.add_argument("--no-collapse", action="store_true",
                        help="print every payload, not one line per run of identical ones")
    parser.add_argument("--limit", type=int, default=0, metavar="N",
                        help="stop after N records (the summary still counts them all)")
    parser.add_argument("--summary", action="store_true", help="print only the summary")
    args = parser.parse_args()

    lines, summary = decode(args.dump.read_bytes(), hex_words=args.hex,
                            collapse=not args.no_collapse, limit=args.limit)
    if not args.summary:
        print("\n".join(lines))
        print()
    print("\n".join(format_summary(summary)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
