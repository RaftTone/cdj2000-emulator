# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory candidate C6x format families without executing or skipping code.

Reads GNU's external tic6x-insn-formats.h as data and scans either a raw L2
image or the L2/shared-RAM/SDRAM regions in a supported checkpoint. This is a format scan,
not a disassembler or proof of reachability, valid operands, or emulator
support. Explicit address ranges are required because firmware images also
contain data.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

BASE = 0x11800000
SHARED_RAM_BASE = 0x80000000
SHARED_RAM_SIZE = 0x20000
SDRAM_BASE = 0xc0000000
L1D_BASE = 0x00f00000
L1D_SIZE = 0x8000
CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0', 3: b'CDJDSP3\0',
                    4: b'CDJDSP4\0', 5: b'CDJDSP5\0', 6: b'CDJDSP6\0',
                    7: b'CDJDSP7\0', 8: b'CDJDSP8\0', 9: b'CDJDSP9\0',
                    10: b'CDJDSP10', 11: b'CDJDSP11', 12: b'CDJDSP12'}
SCHEDULER_STATE = struct.Struct('<QQIIBBBB')


def _checkpoint_scheduler_mode(data, header_size, state_size, schema):
    if schema < 11:
        return 'legacy'
    if state_size < 32:
        raise ValueError('schema-11 checkpoint scheduler state is incomplete')
    fields = SCHEDULER_STATE.unpack_from(data, header_size + state_size - 32)
    activation, slice_id, remaining, slice_steps, pending, rearm, mode, reserved = fields
    if reserved or pending > 1 or rearm > 1:
        raise ValueError('schema-11 checkpoint scheduler state is invalid')
    if mode == 0:
        valid = not any((activation, slice_id, remaining, slice_steps,
                         pending, rearm))
        name = 'legacy'
    elif mode == 1:
        valid = (slice_steps == 4096 and remaining <= 1000000 and
                 pending == bool(remaining) and (not rearm or pending) and
                 ((activation != 0) or
                  not any((slice_id, remaining, pending, rearm))) and
                 (slice_id != 0 or activation == 0 or remaining == 1000000) and
                 (remaining == 0 or (1000000 - remaining) % slice_steps == 0))
        name = 'deferred-v1'
    else:
        valid = False
        name = None
    if not valid:
        raise ValueError('schema-11 checkpoint scheduler state is invalid')
    return name


def _fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def read_input(data):
    """Return address-keyed memory images from raw L2 or a supported checkpoint."""
    if len(data) == 0x40000 and not data.startswith(
            tuple(CHECKPOINT_MAGIC.values())):
        return {BASE: data}, dict(kind='raw_l2', schema=None)
    if len(data) < CHECKPOINT_HEADER.size:
        raise ValueError('input must be 256 KiB of L2 or a complete checkpoint')
    fields = CHECKPOINT_HEADER.unpack_from(data)
    magic, schema, endian, header_size, state_size = fields[:5]
    component_sizes = fields[5:14]
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or not state_size or
            not all(component_sizes) or l2_size != 0x40000 or
            sdram_size != 0x2000000 or page_size != 4096 or
            page_count != sdram_size // page_size or
            present_pages > page_count or len(data) != header_size + payload_size):
        raise ValueError('checkpoint structure is incompatible or incomplete')
    payload = memoryview(data)[header_size:]
    if _fnv1a(payload) != payload_checksum:
        raise ValueError('checkpoint payload checksum does not match')
    l2_start = state_size
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = l2_start + l2_size
    l1d_size = L1D_SIZE if schema >= 12 else 0
    l1d_start = shared_start + shared_size
    bitmap_size = (page_count + 7) // 8
    bitmap_start = l1d_start + l1d_size
    pages_start = bitmap_start + bitmap_size
    expected_size = pages_start + present_pages * page_size
    if len(payload) != expected_size:
        raise ValueError('checkpoint sparse memory layout is incomplete')
    if shared_size:
        l2 = bytes(payload[l2_start:shared_start])
        shared_ram = bytes(payload[shared_start:bitmap_start])
    else:
        l2 = bytes(payload[l2_start:bitmap_start])
        shared_ram = None
    bitmap = payload[bitmap_start:pages_start]
    sdram = bytearray(sdram_size)
    offset = pages_start
    count = 0
    for page in range(page_count):
        if bitmap[page // 8] & (1 << (page % 8)):
            begin = page * page_size
            sdram[begin:begin + page_size] = payload[offset:offset + page_size]
            offset += page_size
            count += 1
    if count != present_pages or offset != len(payload):
        raise ValueError('checkpoint sparse page count does not match')
    memories = {BASE: l2, SDRAM_BASE: sdram}
    if shared_ram is not None: memories[SHARED_RAM_BASE] = shared_ram
    if l1d_size: memories[L1D_BASE] = bytes(payload[l1d_start:bitmap_start])
    return memories, dict(
        kind='checkpoint', schema=schema, state_size=state_size,
        component_sizes=list(component_sizes),
        scheduler_state_captured=schema >= 11,
        dsp_scheduler_mode=_checkpoint_scheduler_mode(
            data, header_size, state_size, schema),
        shared_ram_captured=shared_ram is not None,
        l1d_captured=bool(l1d_size),
        present_sdram_pages=present_pages)


def expression(text):
    """Evaluate only the numeric OR and header macros used by GNU's table."""
    value = 0
    for term in text.split('|'):
        term = term.strip()
        if re.fullmatch(r'0x[0-9a-fA-F]+|[0-9]+', term):
            value |= int(term, 0)
            continue
        match = re.fullmatch(r'(SAT|BR|DSZ)\((0x[0-9a-fA-F]+|[0-9]+)\)', term)
        if not match:
            raise ValueError(f'unsupported format expression: {text}')
        # Private scan bits, deliberately independent of GNU's pseudo-bit ABI.
        shift, mask = {'SAT': (16, 1), 'BR': (17, 1), 'DSZ': (18, 7)}[match[1]]
        value |= (int(match[2], 0) & mask) << shift
    return value


def _split_arguments(text):
    """Split one macro invocation without treating nested commas as separators."""
    fields, start, depth = [], 0, 0
    for index, char in enumerate(text):
        if char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth < 0:
                raise ValueError('unbalanced format expression')
        elif char == ',' and depth == 0:
            fields.append(text[start:index].strip())
            start = index + 1
    if depth:
        raise ValueError('unbalanced format expression')
    fields.append(text[start:].strip())
    return fields


def _format_blocks(text):
    """Yield complete FMT argument lists from the GNU table."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    position = 0
    while True:
        match = re.search(r'\bFMT\s*\(', text[position:])
        if not match:
            return
        opening = position + match.end() - 1
        depth = 1
        cursor = opening + 1
        while cursor < len(text) and depth:
            if text[cursor] == '(':
                depth += 1
            elif text[cursor] == ')':
                depth -= 1
            cursor += 1
        if depth:
            raise ValueError('unterminated GNU C6x format')
        arguments = _split_arguments(text[opening + 1:cursor - 1])
        if len(arguments) != 5:
            raise ValueError(f'GNU C6x format has {len(arguments)} arguments, expected 5')
        yield arguments
        position = cursor


def read_format_specs(text):
    """Parse format masks and simple field positions used by coverage reports."""
    specs = []
    for name, width, value, mask, field_expression in _format_blocks(text):
        if not re.fullmatch(r'\w+', name) or width not in ('16', '32'):
            raise ValueError('invalid GNU C6x format name or width')
        fields = {}
        if 'CFLDS' in field_expression:
            fields.update(p=(0, 1), creg=(29, 3), z=(28, 1))
        elif 'NFLDS' in field_expression:
            fields['p'] = (0, 1)
        elif 'SFLDS' in field_expression:
            fields['s'] = (0, 1)
        for field_name, pos, field_width in re.findall(
                r'\bFLD\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)',
                field_expression):
            fields[field_name] = (int(pos), int(field_width))
        composites = sorted(set(re.findall(r'\bCOMPFLD\(\s*(\w+)', field_expression)))
        specs.append(dict(name=name, width=int(width), value=expression(value),
                          mask=expression(mask), fields=fields,
                          composite_fields=composites))
    if not specs:
        raise ValueError('no GNU C6x formats found')
    return specs


def read_formats(text):
    """Compatibility tuple view used by the discovery scanner."""
    return [(spec['name'], spec['width'], spec['value'], spec['mask'])
            for spec in read_format_specs(text)]


def _format_values(item):
    if isinstance(item, dict):
        return item['name'], item['width'], item['value'], item['mask']
    return item


def decode_one(data, base, pc, formats):
    """Decode the fetch width/layout and most-specific GNU format at one PC."""
    offset = pc - base
    if offset < 0 or offset + 2 > len(data):
        raise ValueError(f'instruction address is outside memory at {pc:#x}')
    header_offset = (offset & ~31) + 28
    if header_offset + 4 > len(data):
        raise ValueError(f'instruction fetch packet is incomplete at {pc:#x}')
    header = struct.unpack_from('<I', data, header_offset)[0]
    mixed = header >> 28 == 14
    packet_offset = offset & 31
    if mixed and packet_offset == 28:
        raise ValueError(f'instruction points at compact header at {pc:#x}')
    if mixed and packet_offset == 30:
        raise ValueError(f'instruction points into compact header at {pc:#x}')
    slot = packet_offset // 4
    compact = bool(mixed and (header >> (21 + slot)) & 1)
    width = 16 if compact else 32
    size = width // 8
    if pc % size or offset + size > len(data):
        raise ValueError(f'instruction is unaligned or incomplete at {pc:#x}')
    word = int.from_bytes(data[offset:offset + size], 'little')
    expanded = word
    if compact:
        expanded |= ((header >> 14) & 1) << 16
        expanded |= ((header >> 15) & 1) << 17
        expanded |= ((header >> 16) & 7) << 18
    matches = [(name, mask.bit_count()) for item in formats
               for name, bits, value, mask in [_format_values(item)]
               if bits == width and expanded & mask == value]
    specificity = max((bits for _, bits in matches), default=0)
    families = sorted(name for name, bits in matches if bits == specificity)
    next_pc = pc + size
    if mixed and (next_pc & 31) == 28:
        next_pc += 4
    return dict(pc=pc, word=word, expanded=expanded, width=width,
                compact=compact, header=header if mixed else 0,
                parallel=bool((header >> (packet_offset // 2)) & 1)
                if compact else bool(word & 1),
                next_pc=next_pc, families=families or ['unclassified'])


def decode_packet(memories, pc, formats):
    """Decode one execute packet starting at a dynamically confirmed source PC."""
    rows = []
    while True:
        matches = [(base, image) for base, image in memories.items()
                   if base <= pc < base + len(image)]
        if len(matches) != 1:
            raise ValueError(f'instruction address is not in exactly one memory at {pc:#x}')
        base, image = matches[0]
        row = decode_one(image, base, pc, formats)
        rows.append(row)
        if not row['parallel']:
            return rows
        if len(rows) == 8:
            raise ValueError(f'execute packet exceeds eight instructions at {rows[0]["pc"]:#x}')
        pc = row['next_pc']


def scan(data, base, start, end, formats):
    pc = start
    while pc < end:
        offset = pc - base
        header = struct.unpack_from('<I', data, (offset & ~31) + 28)[0]
        mixed = header >> 28 == 14
        slot = (offset & 31) // 4
        if mixed and slot == 7:
            pc = (pc & ~31) + 32
            continue
        row = decode_one(data, base, pc, formats)
        if row['next_pc'] > end and pc + row['width'] // 8 > end:
            raise ValueError(f'range splits an instruction at {pc:#x}')
        yield {key: row[key] for key in
               ('pc', 'word', 'width', 'header', 'parallel', 'families')}
        pc = row['next_pc']


def build_report(data, ranges, formats, trace):
    memories = {BASE: data} if isinstance(data, (bytes, bytearray, memoryview)) else data
    rows = []
    for start, end in ranges:
        matches = [(base, image) for base, image in memories.items()
                   if base <= start < end <= base + len(image)]
        if len(matches) != 1 or start % 2 or end % 2:
            raise ValueError('ranges must be aligned halfwords inside one available memory region')
        base, image = matches[0]
        rows.extend(scan(image, base, start, end, formats))
    rows = list({row['pc']: row for row in rows}.values())
    observed = Counter()
    fault = None
    for event in trace:
        if event.get('event') == 'step':
            pc = event['pc']
            if 0x00800000 <= pc < 0x00840000:
                pc += 0x11000000
            observed[pc] += 1
        if event.get('event') == 'stop' and event.get('reason') == 'fault':
            fault = event
    groups = {}
    for row in rows:
        row['trace_pc_visits'] = observed[row['pc']]
        for name in row['families']:
            group = groups.setdefault(name, dict(candidates=0, trace_pc_visits=0,
                                                 examples=[], encodings=set()))
            group['candidates'] += 1
            group['trace_pc_visits'] += row['trace_pc_visits']
            group['encodings'].add(row['word'])
            if len(group['examples']) < 8:
                group['examples'].append(row['pc'])
    for group in groups.values():
        group['unique_encodings'] = len(group.pop('encodings'))
    return dict(schema=1, discovery_only=True, execution_performed=False,
                caveat='Candidate formats may be data or invalid instructions. Trace PC visits '
                       'include idle/loading cycles and do not prove instruction execution. '
                       'Family matches do not establish emulator support.',
                ranges=ranges, candidate_count=len(rows), families=groups,
                observed_fault=fault, instructions=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path,
                        help='256 KiB L2 dump or supported DSP checkpoint')
    parser.add_argument('output', type=Path, help='new JSON report')
    parser.add_argument('--formats', type=Path, required=True,
                        help='GNU include/opcode/tic6x-insn-formats.h')
    parser.add_argument('--range', dest='ranges', action='append', required=True,
                        help='global L2 or SDRAM START:END (exclusive); repeatable')
    parser.add_argument('--trace', type=Path, help='optional deterministic replay JSONL')
    args = parser.parse_args()
    try:
        data = args.dump.read_bytes()
        memories, input_info = read_input(data)
        ranges = [tuple(int(x, 0) for x in item.split(':')) for item in args.ranges]
        if any(len(r) != 2 for r in ranges):
            raise ValueError('ranges must be START:END pairs')
        source = args.formats.read_bytes()
        trace_data = args.trace.read_bytes() if args.trace else b''
        trace = [json.loads(line) for line in trace_data.splitlines()]
        report = build_report(memories, ranges, read_formats(source.decode()), trace)
        report['input'] = input_info
        report['sha256'] = {name: hashlib.sha256(content).hexdigest() for name, content in
                            [('dump', data), ('formats', source), ('trace', trace_data),
                             ('inventory.py', Path(__file__).read_bytes())]}
        with args.output.open('x') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"{report['candidate_count']} candidates in {len(report['families'])} format families")
    for name, group in sorted(report['families'].items(),
                              key=lambda pair: (-pair[1]['candidates'], pair[0])):
        print(f"{group['candidates']:5} {name:28} {group['unique_encodings']:5} encodings")


if __name__ == '__main__':
    main()
