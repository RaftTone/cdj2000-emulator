# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate deterministic captures of genuine McASP XBUF transmit words."""
from collections import Counter
import hashlib
import json
from pathlib import Path


def tx_capture_metadata(path: Path) -> dict:
    counts = Counter()
    nonzero_counts = Counter()
    sequence = 0
    first_nonzero = None
    first_virtual_ns = last_virtual_ns = None
    virtual_time_records = 0
    clock_mode = None
    digest = hashlib.sha256()
    with path.open('rb') as raw:
        for raw_line in raw:
            digest.update(raw_line)
            sequence += 1
            if not raw_line.endswith(b'\n'):
                raise ValueError(f'truncated DSP transmit capture record {sequence}')
            event = json.loads(raw_line)
            required = {'sequence', 'instance', 'slot', 'serializer', 'word',
                        'xbuf_sequence', 'packets', 'cycles', 'source', 'clock'}
            fields = set(event)
            virtual_ns = event.get('virtual_ns')
            if (not required <= fields or not fields <= required | {'virtual_ns'} or
                    event['sequence'] != sequence or
                    event['instance'] not in (1, 2) or
                    not 0 <= event['slot'] <= 0x17f or
                    not 0 <= event['serializer'] < 16 or
                    not 0 <= event['word'] <= 0xffffffff or
                    event['xbuf_sequence'] <= 0 or event['packets'] < 0 or
                    event['cycles'] < 0 or event['source'] != 'genuine_xbuf' or
                    event['clock'] not in ('functional-coarse-packet-slot',
                                           'virtual-clock-batch')):
                raise ValueError(f'invalid DSP transmit capture record {sequence}')
            if clock_mode is None:
                clock_mode = event['clock']
            elif event['clock'] != clock_mode:
                raise ValueError('DSP transmit capture mixes clock modes')
            if 'virtual_ns' in event:
                if type(virtual_ns) is not int or virtual_ns < 0 or (
                        last_virtual_ns is not None and virtual_ns < last_virtual_ns):
                    raise ValueError(f'invalid DSP virtual timestamp at record {sequence}')
                if first_virtual_ns is None:
                    first_virtual_ns = virtual_ns
                last_virtual_ns = virtual_ns
                virtual_time_records += 1
            counts[f"mcasp{event['instance']}.serializer{event['serializer']}"] += 1
            if event['word']:
                nonzero_counts[f"mcasp{event['instance']}.serializer{event['serializer']}"] += 1
                if first_nonzero is None:
                    first_nonzero = dict(sequence=sequence,
                                         instance=event['instance'],
                                         slot=event['slot'],
                                         serializer=event['serializer'],
                                         word=event['word'])
    if virtual_time_records not in (0, sequence):
        raise ValueError('DSP transmit capture mixes timed and untimed records')
    return dict(schema=1, format='canonical JSONL genuine McASP XBUF slot words',
                file=path.name, sha256=digest.hexdigest(), records=sequence,
                counts=dict(sorted(counts.items())),
                nonzero_records=sum(nonzero_counts.values()),
                nonzero_counts=dict(sorted(nonzero_counts.items())),
                first_nonzero=first_nonzero,
                virtual_time_records=virtual_time_records,
                virtual_time_span_ns=(last_virtual_ns - first_virtual_ns
                                      if virtual_time_records else None),
                sample_encoding='unsigned 32-bit serializer word; not PCM interpretation',
                timing=('experimental virtual-time slot batches; not host audio timing'
                        if clock_mode == 'virtual-clock-batch' else
                        'functional coarse packet slots; not serializer-clock or audio timing'),
                synthesized_samples=False)
