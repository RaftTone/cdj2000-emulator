"""A transparent proxy on the MAIN/GUI link that can inject GUI requests.

The GUI simulator normally connects straight to the two TCP chardevs of the
emulated MAIN board (requests to port, records from port+2).  Run this
between them -- ``BFIN_MAIN_LINK=127.0.0.1:5990`` on the simulator, MAIN on its
usual 5980/5982 -- and every byte is forwarded both ways while, at the times
given, a valid 48-byte request of your own goes to MAIN as if the GUI had sent
it.  That is how a track was first loaded in the emulator (2026-09-03): the
NXS GUI's own browse keys never produced an "enter", so the browse and load
requests were injected instead::

    python -m tools.cdj_main.link_inject --inject 90:1:3:7:1:0 \\
        --inject 110:1:3:7:1:0 --inject 130:1:3:7:1:0 --inject 155:7:1:0:0

An injection is ``SECONDS:TYPE:CURSOR[:W3:W4:...]``: word 1 of the request is
``0x8000 | TYPE``, word 2 the cursor, words 3.. as given, the firmware CRC in
the last word.  Seconds count from the moment the GUI connected.

What MAIN's requests mean, as measured (``runs/nxs-swap/trackload-11/-12``):

* type 1 cursor 11, w3 = 7, w4 = 2 (the SD KIND), w5 = N -- the preview pane
  of row N of MAIN's current list;
* type 1 cursor 3, w3 = 7, w4 = 1, w5 = N -- ENTER row N: the answer (command
  0x13) becomes MAIN's current list, its header word 5 is the new level
  (categories 1, PLAYLIST root 2, a folder 3, a playlist's tracks 4);
* type 7 cursor 1, words 3/4 = a 32-bit index into MAIN's current list --
  the track load.  MAIN's loader (task 60) walks the list from that index and
  needs a track row; on any other row it logs an error-ring entry
  (``boot_vm --caution`` prints the ring) and loads nothing.

So from the library screen (current list = the categories, PLAYLIST first):
enter 0 (the playlist root), enter 0 (the first folder), enter 0 (its first
playlist: the track list), load 0 (its first track).

The request format and checksum are the GUI's own (``main_packet.py``);
an earlier stand-alone proxy injected the load alone.

``--nxs-prefix`` rewrites the other direction.  The NXS MAIN fills status
record words 1 and 2 with the beat display's bitfields (its builder at
0xa425ca0c; the NXS GUI's decoder 0x00d0e346 takes them apart into the
globals 0x6003c2..0x6003ca that the screen-0 orchestrator 0x00d2d80c hands
the widgets); MAIN 4.33 sends those words as 0 (its builder starts at word
3).  With the option every 64-byte record MAIN sends gets:

* word 1 bits 14..12 and 10..8: the beat in the bar, 1..4, computed here
  from the record's own time (words 7/8 length, 5/6 remaining, 150ths of a
  second) and word 10 (BPM) -- widget 0x26 (0x00d2cd68) lights it;
* word 1 bits 7..4: MODE, bits 3..0: STATE (0x00d2d580 shows one of two
  counter groups depending on whether STATE is 0 or equals MODE);
* word 1 bit 15 + word 2 high byte: a 9-bit bars.beat counter (0x00d2d106,
  0x1ff = blank); word 1 bit 11 + word 2 low byte: a second one (0x00d2d330).

The checksum in word 31 is redone (``firmware_crc`` over the first 62 bytes,
the same routine as the requests).  Records with a blank time (minutes 99 or
0xffff) or BPM 0 pass unchanged.

``--nxs-markers`` delivers the marker payloads the NXS GUI draws on its
waveforms and MAIN 4.33 never sends.  The NXS MAIN's producer 0xa425c3b8
builds command 0x21 as 448 halfwords: word 0 = 0x21, words 1/2 = the part
number (32-bit, from 1), words 3/4 = the total marker bytes (32-bit), words
5..7 = three fields of its analysis snapshot, word 8 = 0, then up to 219
four-byte records and the checksum in the last word; a continuation part
carries its number in words 1/2 and up to 222 records from word 3.  A record
is byte 0 = type, bytes 1, 3, 2 = the time in milliseconds as 24 bits (the
converter 0xa425bba4; the GUI's consumer 0x00d2c118 reads b1 << 16 | b3 << 8
| b2 and scales by 150/1000).  The GUI's wire handler 0x00d0fe00 stages the
parts at 0x01b40d08 and hands the whole to the consumer when the total is
reached.  The GUI asks for them: after a load it sends
request type 0x20 (cursor 0x20) for the detail waveform and type 0x21 for
the markers, and its completion dispatch 0x00d0f1a8 accepts a bulk answer
only while the request of that command is open (MAIN 4.33 answers those
requests with its last list, trackload-97).  The link is request/answer in lockstep:
every GUI request gets one record back, MAIN announces a payload in status
words 29 (count) and 30 (halfwords) of the record that answers the request,
and sends the payload as the answer to the GUI's next request (trackload-99:
swallowing the typed request without an answer stalled the GUI into
timeouts and it dropped the continuation).  So the proxy answers a typed
0x20/0x21 request itself: MAIN's last status record plus the announcement,
immediately followed by the part (the firmware arms the payload receive
from the announcement and takes the next frame of that length), and so on
for each further typed request until the command's parts are out; MAIN
sees none of those requests.  While a transfer runs, MAIN's own
announcements in its records are cleared (trackload-101: MAIN kept
re-announcing a stale 272-byte answer and the GUI fetched those); MAIN's
answers to the requests it does get pass through unchanged -- the GUI's
completion waits for them (trackload-119: dropped, the consumer never ran;
trackload-106/112: released in a burst afterwards, the time display froze)
and with BFIN_LINK_DEPTH=64 they no longer overwrite the parts.  The spec is a comma list of
``beat:BPM[:OFFSET_MS[:TYPE[:BAR_TYPE]]]`` (a grid over the track's length
from the record's words 7/8, TYPE on every beat, BAR_TYPE on every fourth,
defaults 1 and 2) and ``cue:MS:TYPE`` items, plus ``at:SECONDS`` for when to
start (default: as soon as the length is known).  Types are what the NXS
converter copies from its 16-byte source records; which value draws which
glyph is measured, not known -- give each cue a different one to find out.

``--nxs-waveform FILE`` sends the detail waveform the markers are drawn on:
command 0x20 (the NXS producer 0xa425bd60), word 0 = 0x20, words 1/2 = part
number, words 3/4 = the byte count, words 5/6 = two PWV3 descriptor fields,
then 0x370 bytes of PWV3 entries (one byte a column, 150 columns a second:
height in bits 4..0, colour in bits 7..5), continuations with 0x378 bytes
from word 3.  FILE holds the entries as rekordbox's ANLZ PWV3 tag carries
them (the tag's 24-byte header stripped).  The waveform goes first, the
markers after it, each part announced and fetched in turn.
"""

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import selectors
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from ..cdj_gui.main_packet import firmware_crc

REQUEST_BYTES = 48
RECORD_MAGIC = b"CDJL"
STATUS_RECORD_BYTES = 64


MARKER_PART_HALFWORDS = 448
MARKER_FIRST_RECORDS = 219
MARKER_NEXT_RECORDS = 222
WAVEFORM_FIRST_BYTES = 0x370
WAVEFORM_NEXT_BYTES = 0x378
POLL_WORDS = 24


def waveform_parts(entries: bytes, fields: tuple[int, int] = (1, 0)) -> list[bytes]:
    """Split PWV3 entries into command-0x20 parts of 448 halfwords, checksummed."""
    total = len(entries)
    parts: list[bytes] = []
    index = 0
    number = 1
    while True:
        if number == 1:
            take = entries[index:index + WAVEFORM_FIRST_BYTES]
            head = struct.pack("<7H", 0x20, 1, 0, total & 0xffff, total >> 16,
                               fields[0] & 0xffff, fields[1] & 0xffff)
        else:
            take = entries[index:index + WAVEFORM_NEXT_BYTES]
            head = struct.pack("<3H", 0x20, number & 0xffff, number >> 16)
        body = head + take
        body += bytes(MARKER_PART_HALFWORDS * 2 - 2 - len(body))
        parts.append(body + struct.pack("<H", firmware_crc(body)))
        index += len(take)
        number += 1
        if index >= total:
            return parts


def marker_record(kind: int, milliseconds: int) -> bytes:
    """One four-byte record: type, then the time's bits 23..16, 7..0, 15..8."""
    t = milliseconds & 0xffffff
    return bytes((kind & 0xff, (t >> 16) & 0xff, t & 0xff, (t >> 8) & 0xff))


def marker_parts(records: list[bytes], fields: tuple[int, int, int] = (0, 0, 0)) -> list[bytes]:
    """Split records into command-0x21 parts of 448 halfwords, checksummed."""
    total = len(records) * 4
    parts: list[bytes] = []
    index = 0
    number = 1
    while True:
        if number == 1:
            take = records[index:index + MARKER_FIRST_RECORDS]
            head = struct.pack("<9H", 0x21, 1, 0, total & 0xffff, total >> 16,
                               fields[0] & 0xffff, fields[1] & 0xffff, fields[2] & 0xffff, 0)
        else:
            take = records[index:index + MARKER_NEXT_RECORDS]
            head = struct.pack("<3H", 0x21, number & 0xffff, number >> 16)
        body = head + b"".join(take)
        body += bytes(MARKER_PART_HALFWORDS * 2 - 2 - len(body))
        parts.append(body + struct.pack("<H", firmware_crc(body)))
        index += len(take)
        number += 1
        if index >= len(records):
            return parts


class MarkerFeed:
    """Announce and deliver command-0x21 marker payloads in MAIN's place."""

    def __init__(self, spec: str) -> None:
        self.beats: list[tuple[float, int, int, int]] = []     # bpm, offset ms, type, bar type
        self.cues: list[tuple[int, int]] = []                    # ms, type
        self.start = 0.0
        for item in spec.split(","):
            fields = item.split(":")
            if fields[0] == "beat":
                nums = [float(fields[1])] + [int(f, 0) for f in fields[2:]]
                bpm = nums[0]
                offset = int(nums[1]) if len(nums) > 1 else 0
                kind = int(nums[2]) if len(nums) > 2 else 1
                bar = int(nums[3]) if len(nums) > 3 else 2
                if bpm <= 0:
                    raise ValueError("--nxs-markers: BPM must be positive")
                self.beats.append((bpm, offset, kind, bar))
            elif fields[0] == "cue" and len(fields) == 3:
                self.cues.append((int(fields[1], 0), int(fields[2], 0)))
            elif fields[0] == "at" and len(fields) == 2:
                self.start = float(fields[1])
            else:
                raise ValueError(f"--nxs-markers: cannot read {item!r}")
        self.length_ms = 0
        self.parts: list[bytes] = []
        self.queues: dict[int, list[bytes]] = {}
        self.requested = 0                  # the bulk command the GUI has open, 0 = none
        self.requests = 0
        self.announced = False
        self.sent = 0
        self.done = False
        self.waveform: bytes = b""
        self.waveform_fields = None         # command 0x20 words 5/6: the track length as the
        self.last_status: bytes = b""       # MAIN's latest 64-byte record, prefix applied
        self.elapsed = 0.0
        self.pace = 0                       # typed requests seen during the transfer
        self.hidden = 0                     # MAIN announcements cleared during transfers

    def build(self, length_ms: int, length_words: tuple[int, int] = (0, 0)) -> None:
        """Build the parts for a track of LENGTH_MS; LENGTH_WORDS are the status
        record's words 7/8, which the detail waveform's first part must repeat
        in its words 5/6 -- the GUI's widget handler 0x00d2e8e8 sets the
        readiness word 0x00cd3694 the draw gate 0x00d2f418 waits for only when
        minutes, seconds and (within 2) frames match the record (trackload-121:
        both consumers ran, nothing drew, with 1/0 there)."""
        records: list[tuple[int, int, int]] = []
        for bpm, offset, kind, bar in self.beats:
            period = 60000.0 / bpm
            n = 0
            while True:
                t = offset + n * period
                if t > length_ms:
                    break
                records.append((int(t), bar if n % 4 == 0 else kind, 1))
                n += 1
        for ms, kind in self.cues:
            records.append((ms, kind, 0))
        records.sort()
        self.queues = {}
        if self.waveform:
            self.queues[0x20] = waveform_parts(self.waveform, self.waveform_fields or length_words)
        self.queues[0x21] = marker_parts([marker_record(k, t) for t, k, _ in records])
        self.parts = [p for q in self.queues.values() for p in q]
        self.length_ms = length_ms
        print(f"proxy: nxs-markers built {len(records)} records"
              + (f" after {len(self.waveform)} waveform bytes" if self.waveform else "")
              + f" in {len(self.parts)} parts for {length_ms} ms", flush=True)

    def answer(self, request: bytes) -> bytes | None:
        """The frame to send the GUI for REQUEST instead of forwarding it, or None.

        A typed 0x20/0x21 request while that command's parts are pending is
        answered with the last status record announcing a part; the request
        after an announcement (typed or the all-zero poll) gets the part."""
        if len(request) != REQUEST_BYTES or self.elapsed < self.start:
            return None
        kind, cursor = struct.unpack_from("<HH", request, 2)
        typed = kind & 0x8000 and kind & 0x7fff in (0x20, 0x21)
        if not typed:
            return None
        command = kind & 0x7fff
        # The GUI repeats a typed request faster than it takes parts
        # (trackload-103: the firmware saw parts 4, 5, 8, 11, ...), so only
        # every second request of a running transfer gets a part; the other
        # goes to MAIN, whose status answer paces the GUI's next request.
        if self.requested == command and self.queues.get(command):
            self.pace += 1
            if self.pace % 2:
                return None
        if not self.parts:
            words = list(struct.unpack("<32H", self.last_status)) if self.last_status else None
            if words is None:
                return None
            minutes, second = words[7], words[8]
            if minutes >= 99 or minutes == 0xffff:
                return None
            length = minutes * 60000 + (second >> 8) * 1000 + (second & 0xff) * 1000 // 150
            if length <= 0:
                return None
            self.build(length, (minutes, second))
        if not self.queues.get(command) or not self.last_status:
            return None
        if self.requested != command:
            self.requests += 1
            print(f"proxy: nxs-markers: GUI asks for command 0x{command:x} "
                  f"({len(self.queues[command])} parts)", flush=True)
        self.requested = command
        words = list(struct.unpack("<32H", self.last_status))
        words[29] = 1
        words[30] = MARKER_PART_HALFWORDS
        body = struct.pack("<31H", *words[:31])
        record = body + struct.pack("<H", firmware_crc(body))
        queue = self.queues[command]
        part = queue.pop(0)
        self.sent += 1
        print(f"proxy: nxs-markers sent command 0x{command:x} part ({len(queue)} left)", flush=True)
        if not queue:
            self.done = all(not q for q in self.queues.values())
            self.requested = 0
        return (RECORD_MAGIC + struct.pack("<I", len(record)) + record
                + RECORD_MAGIC + struct.pack("<I", len(part)) + part)

    def transferring(self) -> bool:
        return bool(self.requested) and bool(self.queues.get(self.requested))

    def on_record(self, words: list[int], elapsed: float) -> bool:
        """Called with MAIN's status words (after the prefix): remember the record
        as the template for the proxy's own answers; while a transfer runs, clear
        MAIN's own announcement so the GUI fetches nothing else."""
        self.elapsed = elapsed
        changed = False
        if self.transferring() and words[29]:
            words[29] = 0
            words[30] = 0
            self.hidden += 1
            changed = True
        body = struct.pack("<31H", *words[:31])
        self.last_status = body + struct.pack("<H", firmware_crc(body))
        return changed


class StatusPrefix:
    """Rewrite words 1 and 2 of MAIN's status records with NXS beat bitfields."""

    def __init__(self, mode: int = 1, state: int = 0, counter: int = 0x1ff,
                 counter2: int = 0x1ff, seconds: float = 0.0) -> None:
        self.mode = mode & 0xf
        self.state = state & 0xf
        self.counter = counter & 0x1ff
        self.counter2 = counter2 & 0x1ff
        self.seconds = seconds
        self.pending = b""
        self.rewritten = 0
        self.last_beat = 0
        self.schedule: list[StatusPrefix] = []
        self.beat = True                    # False: leave words 1/2 alone
        self.markers: MarkerFeed | None = None
        self.elapsed = 0.0
        self.patches: list[tuple[int, int, int]] = []       # (index, value, mask)
        self.patch_schedule: list[tuple[float, int, int, int]] = []

    @staticmethod
    def parse_word(spec: str) -> tuple[float, int, int, int]:
        """``IDX=VALUE[/MASK][@SECONDS]`` -> (seconds, index, value, mask)."""
        spec, _, at = spec.partition("@")
        index, _, rest = spec.partition("=")
        value, _, mask = rest.partition("/")
        if not index or not value:
            raise ValueError(f"--status-word: expected IDX=VALUE[/MASK][@SECONDS], got {spec!r}")
        index = int(index, 0)
        if not 1 <= index <= 30:
            raise ValueError(f"--status-word: index {index} outside 1..30")
        return (float(at) if at else 0.0, index, int(value, 0) & 0xffff,
                int(mask, 0) & 0xffff if mask else 0xffff)

    def add_words(self, specs: list[str]) -> None:
        for spec in specs:
            self.patch_schedule.append(self.parse_word(spec))
        self.patch_schedule.sort()

    @classmethod
    def parse(cls, spec: str) -> "StatusPrefix":
        """``beat[:MODE[:STATE[:COUNTER[:COUNTER2]]]][@SECONDS]`` -- numbers in
        any base; SECONDS (from the GUI connecting) says when this variant
        starts, for a list of them."""
        spec, _, at = spec.partition("@")
        fields = spec.split(":")
        if fields[0] != "beat":
            raise ValueError(f"--nxs-prefix: unknown kind {fields[0]!r}, expected 'beat'")
        values = [int(field, 0) for field in fields[1:]]
        return cls(*values, seconds=float(at) if at else 0.0)

    @classmethod
    def parse_all(cls, specs: list[str]) -> "StatusPrefix":
        """The first variant, carrying the later ones in its schedule."""
        variants = sorted((cls.parse(spec) for spec in specs), key=lambda p: p.seconds)
        first = variants[0]
        first.schedule = variants[1:]
        return first

    def advance(self, elapsed: float) -> None:
        """Switch to the next scheduled variant once its time has come."""
        self.elapsed = elapsed
        while self.patch_schedule and self.patch_schedule[0][0] <= elapsed:
            _, index, value, mask = self.patch_schedule.pop(0)
            self.patches = [p for p in self.patches if p[0] != index] + [(index, value, mask)]
            print(f"proxy: status word {index} := 0x{value:04x} under mask 0x{mask:04x} at t{elapsed:.1f}",
                  flush=True)
        while self.schedule and self.schedule[0].seconds <= elapsed:
            nxt = self.schedule.pop(0)
            self.mode, self.state = nxt.mode, nxt.state
            self.counter, self.counter2 = nxt.counter, nxt.counter2
            print(f"proxy: nxs-prefix now mode={self.mode} state={self.state} "
                  f"counter=0x{self.counter:x} counter2=0x{self.counter2:x} at t{elapsed:.1f}",
                  flush=True)

    @staticmethod
    def time_150ths(minutes: int, second_word: int) -> int | None:
        if minutes >= 99 or minutes == 0xffff:
            return None
        return minutes * 9000 + (second_word >> 8) * 150 + (second_word & 0xff)

    def rewrite(self, record: bytes) -> bytes:
        words = list(struct.unpack("<32H", record))
        changed = False
        if self.beat:
            remaining = self.time_150ths(words[5], words[6])
            length = self.time_150ths(words[7], words[8])
            bpm = words[10]
            if remaining is not None and length is not None and bpm:
                elapsed = max(0, length - remaining)
                if self.markers is not None and self.markers.beats:
                    # the same grid the markers were built from: the beat in the
                    # bar changes exactly at the grid's beat times, so the
                    # phase meter and the waveform's beat marks agree
                    grid_bpm, offset = self.markers.beats[0][:2]
                    ms = elapsed * 1000 // 150 - offset
                    beat = (int(ms * grid_bpm / 60000) % 4 + 1) if ms >= 0 else 1
                else:
                    beat = int(elapsed * bpm / (60 * 150)) % 4 + 1
                self.last_beat = beat
                words[1] = ((self.counter >> 8) << 15) | (beat << 12) | ((self.counter2 >> 8) << 11) \
                    | (beat << 8) | (self.mode << 4) | self.state
                words[2] = ((self.counter & 0xff) << 8) | (self.counter2 & 0xff)
                changed = True
        for index, value, mask in self.patches:
            words[index] = (words[index] & ~mask & 0xffff) | (value & mask)
            changed = True
        if self.markers is not None and self.markers.on_record(words, self.elapsed):
            changed = True
        if not changed:
            return record
        body = struct.pack("<31H", *words[:31])
        self.rewritten += 1
        return body + struct.pack("<H", firmware_crc(body))

    def feed(self, data: bytes) -> bytes:
        """Return DATA with every complete 64-byte record rewritten; frames are
        ``CDJL`` + little-endian length + body, and a partial frame waits."""
        buffer = self.pending + data
        out = bytearray()
        while buffer:
            if not buffer.startswith(RECORD_MAGIC):
                # not at a frame boundary: pass a byte through and resync
                out.append(buffer[0])
                buffer = buffer[1:]
                continue
            if len(buffer) < 8:
                break
            length = struct.unpack_from("<I", buffer, 4)[0]
            if len(buffer) < 8 + length:
                break
            body = buffer[8:8 + length]
            if length == STATUS_RECORD_BYTES:
                body = self.rewrite(bytes(body))
            out += buffer[:8] + body
            buffer = buffer[8 + length:]
        self.pending = bytes(buffer)
        return bytes(out)


@dataclass
class DropSpec:
    """Drop the ``nth`` (1-based) 896-byte MAIN->GUI part whose command halfword
    equals ``cmd``, once -- to exercise WP4 row 6 (recovery after a lost part)."""
    cmd: int
    nth: int
    seen: int = 0
    dropped: bool = False


class DropFilter:
    """Reassemble CDJL frames on the MAIN->GUI stream and drop matching parts.

    Test-harness fault injection only: it discards a stream part so the GUI's
    typed re-poll and MAIN's stock resend path can be observed.  Nothing else on
    the link is altered; 64-byte status records always pass through.
    """

    PART_BYTES = 896

    def __init__(self, specs: list[DropSpec]):
        self.specs = specs
        self.pending = b""

    @classmethod
    def parse(cls, raw: list[str]) -> "DropFilter":
        specs = []
        for spec in raw:
            cmd_s, _, nth_s = spec.partition(":")
            if not nth_s:
                raise ValueError(f"--drop-part wants CMD:N, got {spec!r}")
            specs.append(DropSpec(cmd=int(cmd_s, 0), nth=int(nth_s, 0)))
        return cls(specs)

    def feed(self, data: bytes) -> bytes:
        buffer = self.pending + data
        out = bytearray()
        while buffer:
            if not buffer.startswith(RECORD_MAGIC):
                # a partial magic at the tail: wait for the rest, don't resync
                if len(buffer) < len(RECORD_MAGIC) and RECORD_MAGIC.startswith(buffer):
                    break
                out.append(buffer[0])
                buffer = buffer[1:]
                continue
            if len(buffer) < 8:
                break
            length = struct.unpack_from("<I", buffer, 4)[0]
            if len(buffer) < 8 + length:
                break
            body = buffer[8:8 + length]
            frame = buffer[:8 + length]
            buffer = buffer[8 + length:]
            drop = False
            if length == self.PART_BYTES and len(body) >= 2:
                cmd = struct.unpack_from("<H", body, 0)[0]
                for s in self.specs:
                    if s.cmd == cmd:
                        s.seen += 1
                        hit = (s.seen == s.nth and not s.dropped)
                        print("proxy: part cmd=0x%02x len=%d n=%d%s"
                              % (cmd, length, s.seen, " -> DROP" if hit else ""),
                              flush=True)
                        if hit:
                            s.dropped = True
                            drop = True
                        break
            if not drop:
                out += frame
        self.pending = buffer
        return bytes(out)


def build_request(request_type: int, cursor: int, words: tuple[int, ...] = ()) -> bytes:
    """A 48-byte GUI request: word 1 = 0x8000 | type, word 2 = cursor, words 3.. as given."""

    if not 0 <= request_type <= 0x3FFF:
        raise ValueError("request type must fit in 14 bits")
    if not 0 <= cursor <= 0xFFFF:
        raise ValueError("cursor must fit in 16 bits")
    if len(words) > REQUEST_BYTES // 2 - 4:
        raise ValueError("too many words for a 48-byte request")
    packet = bytearray(REQUEST_BYTES)
    struct.pack_into("<3H", packet, 0, 0, request_type | 0x8000, cursor)
    for index, word in enumerate(words, 3):
        struct.pack_into("<H", packet, index * 2, word & 0xFFFF)
    struct.pack_into("<H", packet, REQUEST_BYTES - 2, firmware_crc(packet[:-2]))
    return bytes(packet)


@dataclass(frozen=True, order=True)
class Injection:
    seconds: float
    request_type: int
    cursor: int
    words: tuple[int, ...]


def parse_injection(spec: str) -> Injection:
    parts = spec.split(":")
    if len(parts) < 3:
        raise ValueError("an injection is SECONDS:TYPE:CURSOR[:W3:W4:...]")
    result = Injection(float(parts[0]), int(parts[1], 0), int(parts[2], 0),
                       tuple(int(word, 0) for word in parts[3:]))
    if result.seconds < 0:
        raise ValueError("injection time cannot be negative")
    build_request(result.request_type, result.cursor, result.words)   # validates
    return result


def connect_retry(host: str, port: int, deadline: float) -> socket.socket:
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            stream = socket.create_connection((host, port), timeout=1.0)
            stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return stream
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise TimeoutError(f"could not connect to {host}:{port}: {last_error}")


def listener(host: str, port: int) -> socket.socket:
    stream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    stream.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    stream.bind((host, port))
    stream.listen(1)
    return stream


def accept_until(stream: socket.socket, deadline: float) -> socket.socket:
    stream.settimeout(max(0.1, deadline - time.monotonic()))
    client, _ = stream.accept()
    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return client


def run_proxy(listen_host: str, listen_port: int, main_host: str, main_port: int,
              injections: list[Injection], timeout: float,
              request_dump: Path | None = None,
              prefix: StatusPrefix | None = None,
              drop: "DropFilter | None" = None) -> int:
    request_listener = listener(listen_host, listen_port)
    record_listener = listener(listen_host, listen_port + 2)
    streams: list[socket.socket] = [request_listener, record_listener]
    dump = request_dump.open("wb") if request_dump else None
    deadline = time.monotonic() + timeout
    try:
        main_request = connect_retry(main_host, main_port, deadline)
        main_record = connect_retry(main_host, main_port + 2, deadline)
        streams += [main_request, main_record]
        print(f"proxy: MAIN connected at {main_host}:{main_port}/{main_port + 2}", flush=True)
        gui_request = accept_until(request_listener, deadline)
        gui_record = accept_until(record_listener, deadline)
        streams += [gui_request, gui_record]
        print(f"proxy: GUI connected at {listen_host}:{listen_port}/{listen_port + 2}", flush=True)

        pairs = {
            gui_request: (main_request, "gui-request"),
            main_request: (gui_request, "main-request"),
            gui_record: (main_record, "gui-record"),
            main_record: (gui_record, "main-record"),
        }
        selector = selectors.DefaultSelector()
        for source in pairs:
            source.setblocking(False)
            selector.register(source, selectors.EVENT_READ)

        link_started = time.monotonic()
        pending = sorted(injections)
        counts = {name: 0 for _, name in pairs.values()}
        while time.monotonic() < deadline:
            elapsed = time.monotonic() - link_started
            while pending and pending[0].seconds <= elapsed:
                injection = pending.pop(0)
                packet = build_request(injection.request_type, injection.cursor, injection.words)
                main_request.sendall(packet)
                if dump:
                    dump.write(packet)
                    dump.flush()
                print("proxy: injected type=%d cursor=%d words=%s crc=%04x at t%.3f"
                      % (injection.request_type, injection.cursor,
                         " ".join("%04x" % word for word in injection.words),
                         struct.unpack_from("<H", packet, REQUEST_BYTES - 2)[0], elapsed),
                      flush=True)
            wait = 0.2
            if pending:
                wait = min(wait, max(0.0, pending[0].seconds - elapsed))
            for key, _ in selector.select(wait):
                source = key.fileobj
                target, name = pairs[source]
                try:
                    data = source.recv(1 << 16)
                except BlockingIOError:
                    continue
                except ConnectionResetError:
                    # QEMU closes its chardevs with RST when boot_vm tears the
                    # machine down; the capture is complete by then.
                    print(f"proxy: {name} reset during teardown", flush=True)
                    return 0
                if not data:
                    print(f"proxy: {name} closed", flush=True)
                    return 0
                if prefix is not None and name == "main-record":
                    prefix.advance(elapsed)
                    data = prefix.feed(data)
                    if not data:
                        continue
                if drop is not None and name == "main-record":
                    data = drop.feed(data)
                    if not data:
                        continue
                if prefix is not None and prefix.markers is not None and name == "gui-request":
                    kept = bytearray()
                    for offset in range(0, len(data) - len(data) % REQUEST_BYTES, REQUEST_BYTES):
                        request = data[offset:offset + REQUEST_BYTES]
                        frame = prefix.markers.answer(request)
                        if frame is None:
                            kept += request
                        else:
                            gui_record.sendall(frame)
                    kept += data[len(data) - len(data) % REQUEST_BYTES:]
                    data = bytes(kept)
                    if not data:
                        continue
                try:
                    target.sendall(data)
                except ConnectionResetError:
                    print(f"proxy: {name} target reset during teardown", flush=True)
                    return 0
                counts[name] += len(data)
                if dump and name == "gui-request":
                    dump.write(data)
                    dump.flush()
        print("proxy: timeout; " + ", ".join(f"{name}={count}" for name, count in sorted(counts.items())),
              flush=True)
        return 0
    finally:
        if prefix is not None:
            print(f"proxy: nxs-prefix rewrote {prefix.rewritten} status records, last beat {prefix.last_beat}",
                  flush=True)
            if prefix.markers is not None:
                print(f"proxy: nxs-markers sent {prefix.markers.sent} of {len(prefix.markers.parts)} parts, "
                      f"hid {prefix.markers.hidden} MAIN announcements", flush=True)
        if dump:
            dump.close()
        for stream in reversed(streams):
            try:
                stream.close()
            except OSError:
                pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     epilog="See the module docstring for the request vocabulary.")
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, default=5990,
                        help="where the GUI connects (records on +2); default 5990")
    parser.add_argument("--main-host", default="127.0.0.1")
    parser.add_argument("--main-port", type=int, default=5980,
                        help="MAIN's request chardev (records on +2); default 5980")
    parser.add_argument("--inject", action="append", default=[], metavar="SECONDS:TYPE:CURSOR[:W3..]",
                        help="a request to send MAIN at SECONDS after the GUI connected; repeatable")
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--request-dump", type=Path,
                        help="append every GUI request and every injection, 48 bytes each")
    parser.add_argument("--nxs-waveform", metavar="FILE[:W5[:W6]]",
                        help="PWV3 entries to send as the command-0x20 detail waveform before "
                             "the markers (needs --nxs-markers, even an empty 'at:0'); W5/W6 "
                             "are the two descriptor words of the first part (default: the "
                             "record's length words 7/8, which the GUI's readiness check wants)")
    parser.add_argument("--nxs-markers", metavar="SPEC",
                        help="deliver command-0x21 marker payloads: beat:BPM[:OFFSET_MS[:TYPE"
                             "[:BAR_TYPE]]],cue:MS:TYPE,...,at:SECONDS (see the module docstring)")
    parser.add_argument("--status-word", action="append", default=[],
                        metavar="IDX=VALUE[/MASK][@SECONDS]",
                        help="patch halfword IDX (1..30) of every status record MAIN sends, "
                             "bits under MASK (default all), checksum redone; repeatable, "
                             "@SECONDS schedules it")
    parser.add_argument("--nxs-prefix", action="append", default=[],
                        metavar="beat[:MODE[:STATE[:COUNTER[:COUNTER2]]]][@SECONDS]",
                        help="write the NXS beat bitfields into status record words 1 and 2 "
                             "(see the module docstring); repeatable with @SECONDS for a "
                             "sequence of variants")
    parser.add_argument("--drop-part", action="append", default=[], metavar="CMD:N",
                        help="drop the N-th (1-based) 896-byte MAIN->GUI part whose command "
                             "halfword is CMD (e.g. 0x20:2), once -- WP4 row 6 fault injection; "
                             "repeatable")
    args = parser.parse_args(argv)
    try:
        injections = [parse_injection(spec) for spec in args.inject]
        prefix = StatusPrefix.parse_all(args.nxs_prefix) if args.nxs_prefix else None
        if args.status_word:
            if prefix is None:
                prefix = StatusPrefix()
                prefix.beat = False
            prefix.add_words(args.status_word)
        if args.nxs_markers:
            if prefix is None:
                prefix = StatusPrefix()
                prefix.beat = False
            prefix.markers = MarkerFeed(args.nxs_markers)
            if args.nxs_waveform:
                spec = args.nxs_waveform.split(":")
                prefix.markers.waveform = Path(spec[0]).read_bytes()
                if len(spec) > 1:
                    prefix.markers.waveform_fields = (int(spec[1], 0),
                                                      int(spec[2], 0) if len(spec) > 2 else 0)
        drop = DropFilter.parse(args.drop_part) if args.drop_part else None
        return run_proxy(args.listen_host, args.listen_port, args.main_host, args.main_port,
                         injections, args.timeout, args.request_dump, prefix, drop)
    except (OSError, TimeoutError, ValueError) as exc:
        print(f"link_inject: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
