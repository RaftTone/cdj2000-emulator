# SPDX-License-Identifier: GPL-2.0-or-later
"""Reconstruct McASP1 stereo PCM from a genuine XBUF capture.

The rate labels the WAV for listening. The current packet-based McASP driver
does not establish that the guest delivered samples at that rate.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import tempfile
import wave

from tools.cdj_dsp.tx_capture import tx_capture_metadata


def export_wav(capture: Path, output: Path, sample_rate: int) -> dict:
    if not 8000 <= sample_rate <= 192000:
        raise ValueError('sample rate must be 8000..192000 Hz')
    if capture.resolve() == output.resolve():
        raise ValueError('output must differ from capture')
    metadata = tx_capture_metadata(capture)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    records = filled_zeros = slots = 0
    first_sequence = previous_sequence = None
    try:
        with tempfile.NamedTemporaryFile(prefix=f'.{output.name}.', suffix='.tmp',
                                         dir=output.parent, delete=False) as raw:
            temporary = Path(raw.name)
        with wave.open(str(temporary), 'wb') as wav, capture.open() as source:
            wav.setnchannels(2)
            wav.setsampwidth(2)
            wav.setframerate(sample_rate)
            buffer = bytearray()
            for line in source:
                event = json.loads(line)
                if (event['instance'], event['serializer']) != (1, 0):
                    continue
                if event['slot'] not in (0, 1):
                    raise ValueError('McASP1 capture is not two-slot stereo')
                sequence = event['xbuf_sequence']
                if previous_sequence is None:
                    if event['slot'] != 0:
                        raise ValueError('capture begins between stereo frames')
                    first_sequence = sequence
                    gap = 0
                else:
                    gap = sequence - previous_sequence - 1
                    if gap < 0:
                        raise ValueError('McASP1 XBUF sequence is not increasing')
                    if gap > 1_000_000:
                        raise ValueError('McASP1 XBUF gap is too large to fill')
                if event['slot'] != (sequence - first_sequence) % 2:
                    raise ValueError('McASP1 slot and XBUF order disagree')
                buffer.extend(bytes(2 * gap))
                buffer.extend(struct.pack('<H', event['word'] >> 16))
                slots += gap + 1
                filled_zeros += gap
                records += 1
                previous_sequence = sequence
                if len(buffer) >= 65536:
                    wav.writeframesraw(buffer)
                    buffer.clear()
            if not records:
                raise ValueError('capture has no McASP1 serializer 0 records')
            if slots % 2:
                raise ValueError('capture ends between stereo frames')
            wav.writeframes(buffer)
        os.replace(temporary, output)
    except BaseException:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise
    return {'output': str(output), 'frames': slots // 2,
            'captured_words': records, 'zero_words_reconstructed': filled_zeros,
            'sample_rate_label_hz': sample_rate,
            'capture_sha256': metadata['sha256'],
            'timing': 'rate label only; packet-paced source has no audio timing proof'}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rate', type=int, required=True,
                        help='PCM sample-rate label in Hz; does not validate emulator timing')
    args = parser.parse_args(argv)
    try:
        result = export_wav(args.capture, args.output, args.rate)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.exit(2, f'tx-to-wav: {error}\n')
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
