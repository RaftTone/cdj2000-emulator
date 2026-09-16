# SPDX-License-Identifier: GPL-2.0-or-later
"""Does the captured firmware contain the instructions this core cannot execute?

Two questions are answered separately and never merged:

1. **executed** - the replay-confirmed instruction addresses of a semantic
   inventory (``runs/dsp-semantic-inventory-3.json``), plus every
   ``instruction not implemented`` fault recorded in ``runs/*/coverage.json``.
   Strong evidence: the emulator fetched the packet and decoded this word in it.
   Note that a confirmed address is a *source fetch* of its packet, not a
   predicate-body execution, and that the confirmed set is biased: a packet is
   only confirmed if it completed, so an unimplemented instruction can appear in
   this evidence class **only** as a recorded fault.
2. **present** - a fetch-packet-aligned position in a captured memory image whose
   disassembly names the instruction.  Weak evidence: 256 KB of L2 holds tables,
   sample buffers and strings, and a pattern match is not code.

False-positive controls applied to (2), all reported in the output:

* positions come from the same GNU ``tic6x`` disassembler the semantic inventory
  uses, driven over whole 32-byte fetch packets, so compact headers and 16-bit
  units follow the p-bit/header structure instead of a sweep at every offset;
* a fetch packet contributes nothing unless *every* word in it decodes - real
  code contains no undefined words;
* identical fetch packets are deduplicated across inputs, so a hit is counted
  once and carries the number of input images it appears in;
* each hit is labelled by whether its fetch packet is one the replay evidence
  demonstrably executed, is within ``--neighbourhood`` bytes of one, or neither;
* the same pipeline runs over two control blobs of the same total size - the
  firmware's own bytes shuffled, and uniform pseudo-random bytes - and those
  per-mnemonic hit counts are the reported noise floor.

Nothing here promotes a candidate to confirmed code.  ``verdict`` is
``reached-confirmed`` only on evidence class (1).

    python -m tools.cdj_dsp.reachability analysis/dsp/reachability.json \
        --disassembler /tmp/cdj-tic6x-disasm
"""

from __future__ import annotations

import argparse
import collections
import math
import glob
import hashlib
import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from .inventory import read_input
from .semantic_inventory import assembly_fields

ROOT = Path(__file__).resolve().parents[2]
FETCH_PACKET = 32
SCAN_BASE = 0x11800000
CONTROL_SEED = 0x6747
# 40-bit long-operand forms are a register-pair operand shape, not a row name.
LONG40_MNEMONICS = ('cmpeq', 'cmpgt', 'cmpgtu', 'cmplt', 'cmpltu', 'shl', 'shr', 'shru')
REGISTER_PAIR = re.compile(r'\b[ab][0-9]+:[ab]?[0-9]+\b')
# Almost certainly absent from a bare-metal audio decoder: Galois multiply belongs to
# disc/channel ECC, which this player does in dedicated silicon, and SWE/SWENR are the
# supervisor-entry mechanism of an OS this DSP does not run.  Their hit counts are the
# instruction-level calibration: whatever they score is noise by construction.
CALIBRATION_MNEMONICS = ('gmpy', 'gmpy4', 'xormpy', 'swe', 'swenr')


def portable(path):
    """Record repo-relative paths so the report reads the same in any checkout."""
    for candidate in (Path(path), Path(path).resolve()):
        try:
            return str(candidate.relative_to(ROOT))
        except ValueError:
            continue
    return str(path)


def base_mnemonic(row_name):
    """'MPY32 (32-bit result)' -> 'mpy32'; a manual row name is not an encoding."""
    return re.split(r'[\s(]', row_name.strip())[0].lower()


def unimplemented_rows(probe):
    """Manual rows with at least one encoding the core rejects as unimplemented."""
    status = {name: key for key, names in probe['rows_by_status'].items() for name in names}
    rows = {}
    for name, row in probe['instructions'].items():
        words = sorted({int(c['word'], 16) for c in row['candidates']
                        if c['word'] and 'not implemented' in (c['detail'] or '')})
        if not words:
            continue
        mnemonic = base_mnemonic(name)
        # A mnemonic with accepted forms too cannot be resolved by mnemonic alone:
        # a disassembled `cmpgt` may be the implemented 32-bit form or the absent
        # 40-bit one.  LONG40_MNEMONICS are resolved instead by operand shape.
        shared = status.get(name) == 'some-forms-rejected' or mnemonic != name.lower()
        rows[name] = dict(mnemonic=mnemonic, probe_status=status.get(name),
                          rejected_encodings=len(words),
                          resolution=('operand-shape' if shared and mnemonic in LONG40_MNEMONICS
                                      else 'mnemonic-shared' if shared else 'row'),
                          # TI's unconditional extensions pin bits 31-28 to 0001.
                          nonconditional=all(word >> 28 == 1 for word in words))
    return rows


def requirement_groups(inventory):
    """Family groups named by the audit, with their own '(N rows)' self-check."""
    groups = {}
    for row in inventory['rows']:
        requirement = row['requirement']
        if not row['id'].startswith('ISA-') or ':' not in requirement:
            continue
        head, _, tail = requirement.rpartition(':')
        names = [word for word in re.findall(r'[A-Z][A-Z0-9]*', tail)]
        if not names:
            continue
        declared = re.search(r'\((\d+) rows\)', tail)
        groups[row['id']] = dict(requirement=head.strip(), mnemonics=names,
                                 parsed_rows=len(names),
                                 declared_rows=int(declared[1]) if declared else None,
                                 audit_implementation=row['implementation'])
    return groups


def confirmed_from_inventory(inventory):
    """Decoded confirmed-executed addresses, grouped by mnemonic."""
    rows = collections.defaultdict(lambda: dict(addresses=0, compact_addresses=0,
                                                source_fetch_observations=0,
                                                long40_addresses=0, images=set()))
    for item in inventory['instructions']:
        row = rows[item['mnemonic']]
        row['addresses'] += 1
        row['compact_addresses'] += bool(item['compact'])
        row['source_fetch_observations'] += item['source_fetches']
        if item['mnemonic'] in LONG40_MNEMONICS and REGISTER_PAIR.search(item['operands'] or ''):
            row['long40_addresses'] += 1
        row['images'].add(item['pc'] & ~(FETCH_PACKET - 1))
    return {name: dict(row, fetch_packets=len(row.pop('images'))) for name, row in rows.items()}


def recorded_unsupported(paths):
    """Faults the replay actually hit, which is the only confirmed-unimplemented class."""
    seen = collections.defaultdict(lambda: dict(runs=set(), schemas=set()))
    for path in paths:
        try:
            report = json.loads(Path(path).read_text())
        except (OSError, ValueError):
            continue
        for fault in report.get('unsupported', []):
            entry = seen[(fault['reason'], fault['pc'], fault.get('word'))]
            entry['runs'].add(str(path))
            entry['schemas'].add(report.get('schema'))
    return [dict(reason=reason, pc=pc, word=word, run_artifacts=len(entry['runs']),
                 coverage_schemas=sorted(s for s in entry['schemas'] if s is not None))
            for (reason, pc, word), entry in sorted(seen.items(), key=lambda item: item[0][1])]


def decode_words(words, disassembler, workdir):
    """Name a bare 32-bit encoding: one word per otherwise empty fetch packet."""
    order = sorted(words)
    blob = bytearray(FETCH_PACKET * len(order))
    for index, word in enumerate(order):
        blob[index * FETCH_PACKET:index * FETCH_PACKET + 4] = word.to_bytes(4, 'little')
    decoded = disassemble_blob(bytes(blob), disassembler, workdir, 'encodings')
    named = {}
    for index, word in enumerate(order):
        lines = decoded.get(index, [])
        text = lines[0][1] if lines and lines[0][0] == 0 else ''
        named[word] = assembly_fields(text)['mnemonic'] if text else None
    return named


def executed_fetch_packets(inventory, coverage_paths):
    """Fetch-packet bases the replay evidence confirms the firmware executed."""
    packets = {item['pc'] & ~(FETCH_PACKET - 1) for item in inventory['instructions']}
    for path in coverage_paths:
        try:
            report = json.loads(Path(path).read_text())
        except (OSError, ValueError):
            continue
        for item in report.get('confirmed_instructions', []):
            packets.add(item['pc'] & ~(FETCH_PACKET - 1))
    return packets


def collect_packets(paths):
    """Deduplicate non-zero fetch packets across every region of every input."""
    packets = collections.defaultdict(lambda: dict(inputs=set(), addresses=set(), bases=set()))
    regions = collections.Counter()
    skipped = []
    blank = bytes(FETCH_PACKET)
    for path in paths:
        try:
            memories, _ = read_input(Path(path).read_bytes())
        except (OSError, ValueError) as error:
            skipped.append(dict(path=str(path), error=str(error)))
            continue
        for base, memory in memories.items():
            regions[f'{base:#010x}'] += 1
            view = memoryview(memory)
            for page in range(0, len(memory), 4096):
                chunk = view[page:page + 4096]
                if not any(chunk):
                    continue
                for offset in range(0, len(chunk), FETCH_PACKET):
                    data = bytes(chunk[offset:offset + FETCH_PACKET])
                    if data == blank:
                        continue
                    entry = packets[data]
                    entry['inputs'].add(str(path))
                    entry['addresses'].add(base + page + offset)
                    entry['bases'].add(f'{base:#010x}')
    return packets, dict(regions), skipped


def disassemble_blob(blob, disassembler, workdir, name):
    """Disassemble packed fetch packets; 32-byte alignment keeps compact headers valid."""
    image = Path(workdir) / f'{name}.bin'
    image.write_bytes(blob)
    result = subprocess.run([str(disassembler), str(image), hex(SCAN_BASE),
                             hex(SCAN_BASE), hex(SCAN_BASE + len(blob))],
                            text=True, capture_output=True, check=True, timeout=600)
    decoded = collections.defaultdict(list)
    for line in result.stdout.splitlines():
        match = re.fullmatch(r'0x([0-9a-fA-F]+):\t(.*)', line)
        if not match:
            raise ValueError(f'malformed disassembler output: {line!r}')
        address, text = int(match[1], 16) - SCAN_BASE, match[2]
        decoded[address // FETCH_PACKET].append((address % FETCH_PACKET, text))
    return decoded


def packet_instructions(lines):
    """(offset, width, mnemonic, operands) per instruction, or None if any word is undefined."""
    rows, undefined = [], False
    for index, (offset, text) in enumerate(lines):
        end = lines[index + 1][0] if index + 1 < len(lines) else FETCH_PACKET
        if text.startswith('<fetch packet header'):
            continue
        if text.startswith('<undefined') or text.startswith('.word'):
            undefined = True
            continue
        fields = assembly_fields(text)
        if not fields['mnemonic']:
            undefined = True
            continue
        rows.append((offset, end - offset, fields['mnemonic'], fields['operands'] or ''))
    return None if undefined else rows


def neighbourhood_packets(executed, neighbourhood):
    """Fetch-packet bases within `neighbourhood` bytes of a demonstrably executed one."""
    if not neighbourhood:
        return set()
    span = range(-(neighbourhood // FETCH_PACKET), neighbourhood // FETCH_PACKET + 1)
    return {packet + step * FETCH_PACKET for packet in executed for step in span}


def scan(packets, disassembler, workdir, name, executed, near):
    """Per-mnemonic static candidate counts from fully decoding fetch packets only."""
    order = list(packets)
    decoded = disassemble_blob(b''.join(order), disassembler, workdir, name)
    hits = collections.defaultdict(lambda: dict(
        candidate_positions=0, fetch_packets=0, images=0,
        in_executed_fetch_packet=0, in_executed_neighbourhood=0, elsewhere=0,
        compact_positions=0, long40_positions=0, regions=set()))
    compact_words = collections.defaultdict(lambda: dict(positions=0, images=0,
                                                         in_executed_fetch_packet=0))
    totals = dict(unique_fetch_packets=len(order), fully_decoding_fetch_packets=0,
                  decoded_instruction_positions=0)
    for index, data in enumerate(order):
        rows = packet_instructions(decoded.get(index, []))
        if rows is None:
            continue
        totals['fully_decoding_fetch_packets'] += 1
        totals['decoded_instruction_positions'] += len(rows)
        info = packets[data]
        images = len(info['inputs'])
        hit_executed = not executed.isdisjoint(info['addresses'])
        hit_near = not hit_executed and not near.isdisjoint(info['addresses'])
        seen = set()
        for offset, width, mnemonic, operands in rows:
            row = hits[mnemonic]
            row['candidate_positions'] += 1
            row['images'] = max(row['images'], images)
            row['regions'] |= info['bases']
            row['compact_positions'] += width == 2
            if mnemonic in LONG40_MNEMONICS and REGISTER_PAIR.search(operands):
                row['long40_positions'] += 1
            if hit_executed:
                row['in_executed_fetch_packet'] += 1
            elif hit_near:
                row['in_executed_neighbourhood'] += 1
            else:
                row['elsewhere'] += 1
            if mnemonic not in seen:
                seen.add(mnemonic)
                row['fetch_packets'] += 1
            if width == 2:
                entry = compact_words[int.from_bytes(data[offset:offset + 2], 'little')]
                entry['positions'] += 1
                entry['images'] = max(entry['images'], images)
                entry['in_executed_fetch_packet'] += hit_executed
    for row in hits.values():
        row['regions'] = sorted(row['regions'])
    return dict(hits), dict(compact_words), totals


def control_blobs(packets):
    """Same byte count as the real scan: one shuffled, one uniform pseudo-random."""
    raw = bytearray(b''.join(packets))
    random.Random(CONTROL_SEED).shuffle(raw)
    return {'shuffled-firmware-bytes': bytes(raw),
            'uniform-pseudo-random': random.Random(CONTROL_SEED + 1).randbytes(len(raw))}


def compact_sweep():
    """Reuse the audit sweep to learn which 16-bit words the core rejects."""
    from . import audit_sweeps
    with tempfile.TemporaryDirectory(prefix='cdj-reachability-') as tmp:
        text = audit_sweeps.run(audit_sweeps.build(Path(tmp)), 'compact')
    words, reasons = set(), collections.Counter()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == 'accept':
            continue
        if len(parts) >= 3:
            reason = ' '.join(parts[2:])
            reasons[reason] += 1
            if reason == 'compact instruction not implemented':
                words.add(int(parts[0], 16))
    return words, dict(reasons)


def noise_threshold(noise):
    """Exposure-scaled floor plus three standard deviations.

    The floor is a count of spurious decodes, so its sampling noise is
    Poisson-ish: near a floor of 9 the standard deviation is about 3 and a
    one-count margin says nothing.  Requiring ~3 sigma keeps a family from being
    credited with firmware presence on a difference the control draw itself could
    have produced.
    """
    return noise + 3.0 * math.sqrt(max(noise, 1.0))


def verdict(confirmed, scanned, candidates, noise, executed_hits):
    if confirmed:
        return 'reached-confirmed'
    if not scanned:
        return 'not-scanned'
    if executed_hits:
        return 'reached-candidate-only'
    if not candidates or candidates <= noise_threshold(noise):
        return 'not-found'
    return 'reached-candidate-only'


def build_report(args):
    probe = json.loads(args.probe.read_text())
    coverage_inventory = json.loads(args.coverage_inventory.read_text())
    semantic = json.loads(args.semantic_inventory.read_text())
    coverage_paths = sorted(set(sum((glob.glob(str(ROOT / pattern))
                                     for pattern in args.coverage), [])))
    image_paths = sorted(set(sum((glob.glob(str(ROOT / pattern))
                                 for pattern in args.image), [])))

    rows = unimplemented_rows(probe)
    groups = requirement_groups(coverage_inventory)
    confirmed = confirmed_from_inventory(semantic)
    faults = recorded_unsupported(coverage_paths)
    executed = executed_fetch_packets(semantic, coverage_paths)

    scan_result = dict(state='not-scanned',
                       reason='no --disassembler given; static presence was not measured')
    hits, compact_hits, noise = {}, {}, {}
    control_compact_words, long40_noise = {}, 0
    if args.disassembler and image_paths:
        packets, regions, skipped = collect_packets(image_paths)
        near = neighbourhood_packets(executed, args.neighbourhood)
        scanned_addresses = set().union(*(entry['addresses'] for entry in packets.values()))
        with tempfile.TemporaryDirectory(prefix='cdj-reachability-scan-') as tmp:
            hits, compact_hits, totals = scan(packets, args.disassembler, tmp, 'firmware',
                                              executed, near)
            controls = {}
            for name, blob in control_blobs(packets).items():
                control = {blob[at:at + FETCH_PACKET]: dict(inputs={name}, addresses={at},
                                                            bases={name})
                           for at in range(0, len(blob), FETCH_PACKET)}
                control_hits, control_compact, control_totals = scan(
                    control, args.disassembler, tmp, name, set(), set())
                controls[name] = dict(
                    totals=control_totals,
                    distinct_compact_words=len(control_compact),
                    long40_positions=sum(row['long40_positions']
                                         for row in control_hits.values()),
                    candidate_positions={mnemonic: row['candidate_positions']
                                         for mnemonic, row in sorted(control_hits.items())})
                control_compact_words[name] = set(control_compact)
                long40_noise = max(long40_noise, controls[name]['long40_positions'])
                # Rate, not count: scale each control's per-mnemonic hits up to the
                # firmware's decoded-position exposure before comparing.  A raw
                # count comparison is biased toward crediting the firmware by the
                # ratio of decoded positions, which here is about 11x.
                control_positions = control_totals['decoded_instruction_positions'] or 1
                exposure = (totals['decoded_instruction_positions'] / control_positions)
                for mnemonic, row in control_hits.items():
                    scaled = row['candidate_positions'] * exposure
                    noise[mnemonic] = max(noise.get(mnemonic, 0.0), scaled)
        scan_result = dict(
            state='scanned', images=len(image_paths), unreadable_inputs=skipped,
            regions_by_base=regions, totals=totals, neighbourhood_bytes=args.neighbourhood,
            executed_cross_check=dict(
                executed_fetch_packets=len(executed),
                present_in_scanned_images=len(executed & scanned_addresses),
                note='The cross-check can only speak for executed fetch packets the '
                     'captured images actually contain; the rest of the replayed code '
                     'lives in SDRAM pages no checkpoint holds.'),
            noise_floor=dict(
                definition='per-mnemonic control hits SCALED to the firmware decoded-position '
                           'exposure, max over the control blobs, then compared with a '
                           'three-sigma margin.  Raw counts are not comparable: the control '
                           'blobs decode far fewer positions than the firmware, which biases '
                           'a raw comparison toward crediting the firmware.',
                controls=controls),
            calibration=dict(
                rationale='Instruction-level calibration: these are almost certainly absent '
                          'from a bare-metal audio decoder (Galois multiply is channel ECC '
                          'done in dedicated silicon; SWE/SWENR are OS supervisor entry). '
                          'Whatever they score in the firmware is this method spurious-hit '
                          'rate for a narrow opcode, alongside their control-blob counts.',
                mnemonics={mnemonic: dict(
                    firmware_candidate_positions=hits.get(mnemonic, {}).get(
                        'candidate_positions', 0),
                    firmware_in_executed_fetch_packet=hits.get(mnemonic, {}).get(
                        'in_executed_fetch_packet', 0),
                    control_candidate_positions=noise.get(mnemonic, 0))
                    for mnemonic in CALIBRATION_MNEMONICS}))

    # A faulting packet is never confirmed, so a recorded not-implemented fault is the
    # only way an unimplemented instruction can carry confirmed-executed evidence.
    fault_mnemonics = collections.Counter()
    if args.disassembler:
        words = {fault['word'] for fault in faults if fault['word']}
        with tempfile.TemporaryDirectory(prefix='cdj-reachability-faults-') as tmp:
            named = decode_words(words, args.disassembler, tmp) if words else {}
        for fault in faults:
            fault['decoded_mnemonic'] = named.get(fault['word'])
            # 'reserved predicate' over bits 31-28 = 0001 is the pre-d7937e7 diagnostic
            # for an unimplemented nonconditional extension (audit section 5.1).
            fault['unimplemented_instruction_evidence'] = bool(
                'instruction not implemented' in fault['reason'] or
                (fault['reason'] == 'reserved predicate' and (fault['word'] or 0) >> 28 == 1))
            if fault['decoded_mnemonic'] and fault['unimplemented_instruction_evidence']:
                fault_mnemonics[fault['decoded_mnemonic']] += 1

    compact = dict(state='not-scanned', reason='compact sweep disabled')
    if args.compact_sweep:
        try:
            unimplemented_words, reasons = compact_sweep()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            compact = dict(state='not-scanned', reason=str(error))
        else:
            confirmed_words = {item['word'] for item in semantic['instructions']
                               if item['compact']} & unimplemented_words
            present = {word: row for word, row in compact_hits.items()
                       if word in unimplemented_words}
            in_executed = sum(1 for row in present.values() if row['in_executed_fetch_packet'])
            floor = max((len(words & unimplemented_words)
                         for words in control_compact_words.values()), default=0)
            compact = dict(
                state='scanned' if compact_hits else 'sweep-only',
                unimplemented_words=len(unimplemented_words),
                sweep_rejection_reasons=reasons,
                confirmed_executed_words=sorted(confirmed_words),
                confirmed_executed_word_count=len(confirmed_words),
                confirmed_executed_caveat=(
                    'RESOLVED, and not in this measurement\'s favour. All of these words '
                    'disassemble to the compact software-loop family - sploop, sploopd and '
                    'spkernel - which this core IMPLEMENTS and validates in '
                    'tests/cstub/c674x-spkernel-fields.c; 0xdc66 is the very word '
                    'DSP_BOOT_MILESTONE_AUDIT.md analyses. The compact sweep refuses them '
                    'only because a one-instruction probe packet has no active software loop '
                    'around them, which is the same reason tools/cdj_dsp/isa_probe.py '
                    'excludes that family from the 32-bit sweep. So these are NOT evidence '
                    'of an executed unimplemented instruction, and the verdict below is an '
                    'artifact of the sweep rather than a finding. See '
                    'analysis/dsp/audit_sweeps.json compact.not_implemented_breakdown: of '
                    'the 6,944 raw refusals, 6,616 are encodings the architecture does not '
                    'define, 96 are this loop family, 128 are a deliberate fail-closed '
                    'decision, 72 are s = 0 twins of forms whose SPRUFE8B figure hardwires '
                    's = 1, and 32 are the genuine gap (Figure H-6 predicated SPLOOPD, the '
                    'SPLOOP reload capability).'),
                confirmed_executed_are_loop_family=True,
                distinct_compact_words_seen=len(compact_hits),
                static_candidate_words=len(present),
                static_candidate_words_in_executed_fetch_packets=in_executed,
                static_candidate_positions=sum(row['positions'] for row in present.values()),
                noise_floor=floor,
                noise_floor_definition='distinct unimplemented compact words a control blob '
                                       'of the same size yields through the identical pipeline',
                # Pass 0 for the confirmed count: every confirmed word is loop
                # family (see the caveat), so crediting them would report an
                # implemented instruction as an executed unimplemented one.
                verdict=verdict(0, compact_hits != {}, len(present),
                                floor, in_executed),
                note='A 16-bit word present at a compact position is presence, not '
                     'execution; the core rejects all of these words today.')

    report_rows = {}
    scanned = scan_result['state'] == 'scanned'
    for name, row in sorted(rows.items()):
        mnemonic = row['mnemonic']
        executed_row = confirmed.get(mnemonic) or {}
        hit = hits.get(mnemonic)
        faulted = fault_mnemonics.get(mnemonic, 0)
        if row['resolution'] == 'operand-shape':
            executed_count = executed_row.get('long40_addresses', 0)
            candidates = hit['long40_positions'] if hit else 0
            floor = long40_noise
            executed_hits = 0
        else:
            # A row whose mnemonic is also produced by an IMPLEMENTED instruction
            # cannot claim that mnemonic's executed addresses: "B NRP" was
            # inheriting all 101 addresses of plain "b".  Only its own recorded
            # faults count as its confirmed-executed evidence.
            executed_count = (faulted if row['resolution'] == 'mnemonic-shared'
                              else executed_row.get('addresses', 0) + faulted)
            candidates = hit['candidate_positions'] if hit else 0
            floor = noise.get(mnemonic, 0)
            executed_hits = hit['in_executed_fetch_packet'] if hit else 0
        if row['resolution'] == 'mnemonic-shared':
            row_verdict = 'not-scanned'
        else:
            row_verdict = verdict(executed_count, scanned, candidates, floor, executed_hits)
        report_rows[name] = dict(
            row,
            confirmed_executed=executed_count,
            confirmed_executed_mnemonic_total=executed_row.get('addresses', 0),
            recorded_not_implemented_faults=faulted,
            confirmed_executed_note={
                'row': 'decoded confirmed addresses for this mnemonic; 0 is expected for an '
                       'unimplemented row because a faulting packet is never confirmed - see '
                       'recorded_unsupported_faults',
                'operand-shape': 'confirmed addresses of this mnemonic carrying a '
                                 'register-pair operand, i.e. the 40-bit long form only',
                'mnemonic-shared': 'this row shares its mnemonic with forms the core does '
                                   'implement, so neither disassembly nor the confirmed set '
                                   'can attribute evidence to the unimplemented form; '
                                   'confirmed_executed_mnemonic_total is the whole mnemonic',
            }[row['resolution']],
            static_candidates=candidates,
            static_candidate_detail=hit,
            noise_floor=floor,
            verdict=row_verdict)

    report_groups = {}
    for group_id, group in sorted(groups.items()):
        members = sorted(name for name in rows
                         if rows[name]['mnemonic'].upper() in group['mnemonics'])
        # Mnemonic-shared members would contribute their implemented forms' hits.
        counted = [name for name in members if rows[name]['resolution'] != 'mnemonic-shared']
        aggregate = dict(
            confirmed_executed=sum(report_rows[name]['confirmed_executed'] for name in counted),
            static_candidates=sum(report_rows[name]['static_candidates'] for name in counted),
            noise_floor=sum(report_rows[name]['noise_floor'] for name in counted),
            in_executed_fetch_packet=sum(
                (report_rows[name]['static_candidate_detail'] or {}).get(
                    'in_executed_fetch_packet', 0) for name in counted
                if report_rows[name]['resolution'] == 'row'))
        report_groups[group_id] = dict(
            group, unimplemented_members=members, unimplemented_member_count=len(members),
            unresolvable_members=sorted(set(members) - set(counted)), **aggregate,
            verdict=verdict(aggregate['confirmed_executed'],
                            scan_result['state'] == 'scanned',
                            aggregate['static_candidates'], aggregate['noise_floor'],
                            aggregate['in_executed_fetch_packet']))

    nonconditional = sorted(name for name, row in rows.items() if row['nonconditional'])
    long40_confirmed = sum(row['long40_addresses'] for row in confirmed.values())
    long40_candidates = sum(row['long40_positions'] for row in hits.values())
    forms = dict(
        nonconditional_extensions=dict(
            derivation='unimplemented rows whose every rejected probe encoding carries '
                       "TI's unconditional bits 31-28 = 0001 (audit section 5.1)",
            rows=nonconditional, row_count=len(nonconditional),
            confirmed_executed=sum(report_rows[name]['confirmed_executed']
                                   for name in nonconditional),
            static_candidates=sum(report_rows[name]['static_candidates']
                                  for name in nonconditional),
            verdict=verdict(sum(report_rows[name]['confirmed_executed']
                                for name in nonconditional),
                            scan_result['state'] == 'scanned',
                            sum(report_rows[name]['static_candidates']
                                for name in nonconditional),
                            sum(report_rows[name]['noise_floor'] for name in nonconditional),
                            sum((report_rows[name]['static_candidate_detail'] or {}).get(
                                'in_executed_fetch_packet', 0) for name in nonconditional))),
        long40_compare_and_shift=dict(
            derivation='register-pair src operand on ' + '/'.join(LONG40_MNEMONICS) +
                       ' - an operand shape, not a Table A-1 row',
            confirmed_executed=long40_confirmed,
            static_candidates=long40_candidates,
            noise_floor=long40_noise,
            verdict=verdict(long40_confirmed, scan_result['state'] == 'scanned',
                            long40_candidates, long40_noise, 0)))

    return dict(
        schema=1,
        validation_eligible=False,
        scope='Reachability only: which unimplemented instructions the captured '
              'firmware executed, and which merely appear in captured bytes.',
        caveats=[
            'confirmed_executed and static_candidates are different evidence classes '
            'and must never be added together.',
            'A confirmed address is a source fetch of its execute packet, not a '
            'predicate-body execution.',
            'The confirmed set is biased against unimplemented instructions: a packet '
            'that faults is never confirmed, so recorded_unsupported_faults is the only '
            'confirmed-executed evidence an unimplemented instruction can have.',
            'static_candidates is byte-pattern presence in captured memory. Probable '
            'code and unclassified memory are not promoted to confirmed code.',
            'Captured images cover L2, shared RAM and the SDRAM pages a checkpoint '
            'happened to hold; code in uncaptured SDRAM is invisible to the scan, so '
            'not-found means not found here, not absent from the firmware.',
            'Disassembly decides what a word names; the core decides what it executes. '
            'The two disagree where a format is decoded but unimplemented.',
        ],
        inputs=dict(
            probe=portable(args.probe), coverage_inventory=portable(args.coverage_inventory),
            semantic_inventory=portable(args.semantic_inventory),
            semantic_inventory_sha256=hashlib.sha256(
                args.semantic_inventory.read_bytes()).hexdigest(),
            analyzer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            coverage_artifacts=len(coverage_paths), image_inputs=len(image_paths),
            image_patterns=[portable(pattern) for pattern in args.image],
            disassembler_sha256=(hashlib.sha256(args.disassembler.read_bytes()).hexdigest()
                                 if args.disassembler else None)),
        counts=dict(
            unimplemented_rows=len(rows),
            confirmed_executed_mnemonics=len(confirmed),
            confirmed_executed_addresses=len(semantic['instructions']),
            executed_fetch_packets=len(executed),
            rows_reached_confirmed=sum(row['verdict'] == 'reached-confirmed'
                                       for row in report_rows.values()),
            rows_reached_candidate_only=sum(row['verdict'] == 'reached-candidate-only'
                                            for row in report_rows.values()),
            rows_not_found=sum(row['verdict'] == 'not-found'
                               for row in report_rows.values()),
            rows_not_scanned=sum(row['verdict'] == 'not-scanned'
                                 for row in report_rows.values())),
        static_scan=scan_result,
        recorded_unsupported_faults=faults,
        recorded_unsupported_fault_note=(
            'The only confirmed-executed evidence an unimplemented instruction can carry. '
            'decoded_mnemonic names the encoding; check it against '
            'confirmed_executed_by_mnemonic, because a fault recorded by an older coverage '
            'schema may name an instruction implemented since.'),
        recorded_fault_mnemonics=dict(sorted(fault_mnemonics.items())),
        confirmed_executed_by_mnemonic=confirmed,
        compact_encoding_space=compact,
        rows=report_rows,
        groups=report_groups,
        encoding_forms=forms)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('output', type=Path)
    parser.add_argument('--probe', type=Path, default=ROOT / 'analysis/dsp/isa_probe.json')
    parser.add_argument('--coverage-inventory', type=Path,
                        default=ROOT / 'analysis/dsp/coverage_inventory.json')
    parser.add_argument('--semantic-inventory', type=Path,
                        default=ROOT / 'runs/dsp-semantic-inventory-3.json')
    parser.add_argument('--coverage', action='append',
                        default=None, help='glob of coverage.json artifacts, repeatable')
    parser.add_argument('--image', action='append', default=None,
                        help='glob of raw L2 images or checkpoints, repeatable')
    parser.add_argument('--disassembler', type=Path,
                        help='built tools/cdj_dsp/tic6x_disasm.c frontend; '
                             'without it no static scan is attempted')
    parser.add_argument('--neighbourhood', type=int, default=128,
                        help='bytes either side of an executed fetch packet (0 disables)')
    parser.add_argument('--no-compact-sweep', dest='compact_sweep', action='store_false')
    args = parser.parse_args(argv)
    if args.coverage is None:
        args.coverage = ['runs/*/coverage.json', 'runs/*/repeat-coverage.json']
    if args.image is None:
        args.image = ['runs/*/dsp-l2.bin', 'runs/*/final.cdjdsp']
    try:
        report = build_report(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps(report['counts'], indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
