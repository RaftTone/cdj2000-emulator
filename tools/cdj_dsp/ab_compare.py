# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare two `replay.py --ab` outputs bit for bit.

mcasp1.bin / mcasp2.bin are every captured McASP XBUF word (u32 LE, slot
order); host.bin is what MAIN observed over HPI (16-byte records: kind 1 a
data read, kind 2 a DSP HPIC write; sequence, address, value).  Exit 0 when
all three are identical, 1 on any difference, 2 on unusable input.
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

AUDIO_FILES = ('mcasp1.bin', 'mcasp2.bin')
HOST_FILE = 'host.bin'
SLOT_RATE = 44100 * 2   # stereo slots per second at the configured 44.1 kHz


def _words_per_slot(directory: Path, name: str) -> int:
    """Active serializers on that port, from the replay manifest (default 1)."""
    try:
        counts = json.loads((directory / 'manifest.json').read_text())[
            'ab_words_per_serializer'][name.removesuffix('.bin')]
        return max(1, len(counts))
    except (OSError, ValueError, KeyError, TypeError):
        return 1


def compare_arrays(a: np.ndarray, b: np.ndarray) -> dict:
    common = min(len(a), len(b))
    mask = a[:common] != b[:common]
    diff = np.flatnonzero(mask.any(axis=1) if mask.ndim > 1 else mask)
    return dict(a_items=len(a), b_items=len(b), common=common,
                differing_items=int(len(diff)) + abs(len(a) - len(b)),
                first=int(diff[0]) if len(diff) else (common if len(a) != len(b) else None))


def compare(a_dir: Path, b_dir: Path) -> dict:
    result = dict(identical=True, files={})
    for name in AUDIO_FILES:
        a = np.fromfile(a_dir / name, dtype='<u4')
        b = np.fromfile(b_dir / name, dtype='<u4')
        item = compare_arrays(a, b)
        item['nonzero_words_a'] = int(np.count_nonzero(a))
        per_slot = _words_per_slot(a_dir, name)
        item['seconds_a'] = round(len(a) / per_slot / SLOT_RATE, 3)
        if item['first'] is not None:
            index = item['first']
            item['first_divergence'] = dict(
                word=index, slot=index // per_slot,
                seconds=round(index / per_slot / SLOT_RATE, 6),
                a=int(a[index]) if index < len(a) else None,
                b=int(b[index]) if index < len(b) else None)
            result['identical'] = False
        result['files'][name] = item
    a = np.fromfile(a_dir / HOST_FILE, dtype='<u4').reshape(-1, 4)
    b = np.fromfile(b_dir / HOST_FILE, dtype='<u4').reshape(-1, 4)
    item = compare_arrays(a, b)
    item['reads_a'] = int(np.count_nonzero(a[:, 0] == 1))
    item['hpic_writes_a'] = int(np.count_nonzero(a[:, 0] == 2))
    if item['first'] is not None:
        index = item['first']
        record = lambda r, i: (dict(zip(('kind', 'sequence', 'address', 'value'),
                                        map(int, r[i]))) if i < len(r) else None)
        item['first_divergence'] = dict(record=index, a=record(a, index), b=record(b, index))
        result['identical'] = False
    result['files'][HOST_FILE] = item
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('a', type=Path)
    parser.add_argument('b', type=Path)
    args = parser.parse_args(argv)
    for directory in (args.a, args.b):
        missing = [n for n in (*AUDIO_FILES, HOST_FILE) if not (directory / n).is_file()]
        if missing:
            print(f'{directory}: missing {missing} (not a replay.py --ab output)', file=sys.stderr)
            return 2
    try:
        result = compare(args.a, args.b)
    except ValueError as error:   # host.bin not a whole number of records
        print(f'unusable capture: {error}', file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result['identical'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
