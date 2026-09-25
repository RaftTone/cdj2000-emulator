"""What the deck played, as a WAV file: the DSP model's stream and transport.

    python -m tools.cdj_main.deck_audio --stream STREAM.bin --transport TRANSPORT.log \
        --out deck.wav

The DSP holds the whole track in its own SDRAM: MAIN streams it over the host
window in 8192-byte blocks, each followed by a 24-byte tail, alternating the two
halves of the staging area (+0x81e0 / +0xbea0, tails at +0xa1e0 / +0xdea0).  For
an MP3 those blocks are the file's audio data in order, from the first 8 KiB
boundary after the ID3 tag (measured against the file on the card: block n of
record 1 is file offset 0x9a000 + 8192 n).  `CDJ_DSP_STREAM_DUMP` records them,
tagged with the record of the format command they belong to;
`CDJ_DSP_TRANSPORT_LOG` records where the position model says the deck is,
every 10 ms of guest time while it plays.

This joins the blocks of each record back into a stream, decodes it with ffmpeg
and plays it the way the log says -- silence while the deck stands, the
position's jumps at a CUE or a loop, the tempo word as the rate -- on the guest
time axis.  It is the PCM tap, not the DSP: no master tempo, no EQ or effects,
no DA710 code runs.  WAV (PCM) streams are not handled yet.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import subprocess
import wave
from pathlib import Path

import numpy as np

RATE = 44100
DATA_OFFSETS = (0x81E0, 0xBEA0)


def records(stream: Path) -> dict[int, bytes]:
    """The 8192-byte data blocks of each record, joined in arrival order."""
    data = stream.read_bytes()
    joined: dict[int, list[bytes]] = {}
    at = 0
    while at + 32 <= len(data) and data[at:at + 4] == b"CDJS":
        offset, length, record = (int.from_bytes(data[at + 12 + 4 * i:at + 16 + 4 * i], "little")
                                  for i in range(3))
        if offset in DATA_OFFSETS and length == 8192:
            joined.setdefault(record, []).append(data[at + 32:at + 32 + length])
        at += 32 + length
    return {record: b"".join(blocks) for record, blocks in joined.items()}


def decode(mp3: bytes) -> np.ndarray:
    """Stereo float32 at 44.1 kHz."""
    # Through stdin: Windows will not let ffmpeg open a temporary file that
    # is still open here.
    pcm = subprocess.run(["ffmpeg", "-v", "error", "-f", "mp3", "-i", "pipe:0", "-f", "s16le",
                          "-ac", "2", "-ar", str(RATE), "-"],
                         input=mp3, capture_output=True, check=True).stdout
    return np.frombuffer(pcm, dtype="<i2").reshape(-1, 2).astype(np.float32) / 32768.0


def transport(log: Path) -> list[tuple[int, int, int, int, int]]:
    rows = []
    for line in log.read_text().splitlines():
        t, pos, state, rate, record = line.split()
        rows.append((int(t), int(pos), int(state), int(rate, 16), int(record)))
    return rows


def render(rows, decoded: dict[int, np.ndarray]) -> np.ndarray:
    start = rows[0][0]
    total = int((rows[-1][0] - start) * RATE / 1e9)
    out = np.zeros((total, 2), dtype=np.float32)
    for (t0, pos, state, rate, record), (t1, *_rest) in zip(rows, rows[1:]):
        if state != 3 or record not in decoded:
            continue
        a = int((t0 - start) * RATE / 1e9)
        b = int((t1 - start) * RATE / 1e9)
        if b <= a:
            continue
        speed = (rate or 0x100000) / 0x100000
        source = pos * RATE / 1000.0 + np.arange(b - a) * speed
        pcm = decoded[record]
        index = np.clip(source.astype(np.int64), 0, len(pcm) - 1)
        out[a:b] = pcm[index]
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--stream", type=Path, required=True, help="CDJ_DSP_STREAM_DUMP file")
    parser.add_argument("--transport", type=Path, required=True, help="CDJ_DSP_TRANSPORT_LOG file")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--save-records", type=Path,
                        help="also write each record's joined stream here as rN.mp3")
    args = parser.parse_args(argv)

    rows = transport(args.transport)
    if len(rows) < 2:
        raise SystemExit("deck_audio: the transport log has nothing to play")
    streams = records(args.stream)
    played = {row[4] for row in rows if row[2] == 3}
    decoded = {}
    for record, data in streams.items():
        if args.save_records:
            args.save_records.mkdir(parents=True, exist_ok=True)
            (args.save_records / f"r{record}.mp3").write_bytes(data)
        if record in played:
            decoded[record] = decode(data)
            print(f"record {record}: {len(data)} bytes of stream, "
                  f"{len(decoded[record]) / RATE:.1f} s decoded")
    audio = render(rows, decoded)
    with wave.open(str(args.out), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes((np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes())
    playing = sum(1 for row in rows if row[2] == 3) * 0.01
    print(f"{args.out}: {len(audio) / RATE:.1f} s of guest time, about {playing:.1f} s playing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
