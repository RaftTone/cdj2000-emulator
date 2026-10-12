# SPDX-License-Identifier: GPL-2.0-or-later
"""Build evidence-scoped C674x coverage from deterministic replay events.

Only source packets that completed a replay step are confirmed executable.
Dynamic transition or direct branch targets that were not completed are merely
probable code. Everything else in captured memory remains unclassified; this
tool never turns a broad format scan into a code-coverage claim.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path

from .inventory import decode_packet, read_format_specs, read_input


def normalize_pc(pc):
    """Map the C674x local L2 alias to the global address used in captures."""
    return pc + 0x11000000 if 0x00800000 <= pc < 0x00840000 else pc


def sign_extend(value, bits):
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def expanded_word(event):
    value = event['word']
    if event['compact']:
        header = event['header']
        value |= ((header >> 14) & 1) << 16
        value |= ((header >> 15) & 1) << 17
        value |= ((header >> 16) & 7) << 18
    return value


def matching_specs(event, specs):
    value = expanded_word(event)
    matches = [spec for spec in specs
               if spec['width'] == (16 if event['compact'] else 32) and
               value & spec['mask'] == spec['value']]
    specificity = max((spec['mask'].bit_count() for spec in matches), default=0)
    return sorted((spec for spec in matches if spec['mask'].bit_count() == specificity),
                  key=lambda spec: spec['name'])


def field_value(spec, name, value):
    field = spec['fields'].get(name)
    if field is None:
        return None
    pos, width = field
    return (value >> pos) & ((1 << width) - 1)


def instruction_unit(event):
    """Mirror the core's format-level unit classifier; zero stays unknown."""
    word = event['word']
    side = (word & 1) if event['compact'] else ((word >> 1) & 1)
    if not event['compact']:
        if word & 0x0c == 12:
            return 32
        if word & 0x0c == 4:
            return 16 << ((word >> 7) & 1)
        if word & 0x1c == 0x18:
            return 1 << side
        if word & 0x7c == 0x40 or word & 0xc3c == 0x830:
            return 16 << side
        if (word & 0x3c in (0x20, 0x28, 8) or word & 0x7c in (0x10, 0x50) or
                word & 0xc3c == 0xc30):
            return 4 << side
        if word & 0x7c == 0 or word & 0x83c == 0x30:
            return 64 << side
        return 0
    if (((word & 0x26 == 6) or word & 0x1c66 in (0x0866, 0x1866)) and
            (word >> 3) & 3 != 3):
        return 1 << (2 * ((word >> 3) & 3) + side)
    if (word & 6 == 4 or word & 0x087f == 0x0077 or
            word & 0x047e in (0x0036, 0x0436) or word & 0x1c7e == 0x0c76):
        return 16 << side
    if (word & 0x040e in (0, 0x400, 0x408) or
            word & 0x047e in (0x426, 0x26)):
        return 1 << side
    if word & 0x001e == 0x001e:
        return 64 << side
    if (word & 0x040e in (0xa, 0x40a) or word & 0x001e in (0x12, 2) or
            word & 0x047e in (0x62, 0x462, 0x2e, 0x42e) or
            word & 0x187e == 0x6e or word & 0x1c7e == 0x186e):
        return 4 << side
    return 0


UNIT_NAMES = {1: 'L1', 2: 'L2', 4: 'S1', 8: 'S2',
              16: 'D1', 32: 'D2', 64: 'M1', 128: 'M2'}

# Only the decoder's terminal rejections establish a missing encoding.
# Resource conflicts, unsupported operating modes, and peripheral failures
# can reject valid instructions and must remain separate backlog evidence.
UNSUPPORTED_ENCODING_FAULTS = frozenset({
    'instruction not implemented', 'compact instruction not implemented',
})


def architecture_family(format_name):
    if format_name.startswith('d_'):
        return 'memory_or_address'
    if format_name.startswith('m_'):
        return 'multiply'
    if format_name.startswith('l_'):
        return 'arithmetic_logic'
    if format_name.startswith('lsdmv'):
        return 'cross_unit_move'
    if format_name.startswith('lsd'):
        return 'compact_unit_selected'
    if format_name in ('nfu_dint', 'nfu_rint'):
        return 'interrupt_control'
    if format_name.startswith(('nfu_sp', 'nfu_usp', 'nfu_loop')):
        return 'software_loop'
    if format_name in ('nfu_nop_idle', 'nfu_unop'):
        return 'nop_idle'
    if (format_name.startswith(('s_branch', 's_call', 's_b_', 's_bdec', 's_bpos')) or
            format_name == 's_ext_branch_cond_imm' or
            format_name in ('s_sbs7', 's_sbu8', 's_scs10', 's_sbs7c', 's_sbu8c')):
        return 'control_flow'
    if format_name.startswith('s_'):
        return 'arithmetic_control'
    return 'unclassified'


def architectural_state(family):
    return {
        'memory_or_address': 'registers, memory, and load/store pipeline',
        'multiply': 'registers and operation-dependent arithmetic status',
        'arithmetic_logic': 'registers and operation-dependent CSR/FP status',
        'arithmetic_control': 'registers, control registers, or branch pipeline by opcode',
        'cross_unit_move': 'register or control-register transfer',
        'compact_unit_selected': 'unit-selected registers or control state',
        'interrupt_control': 'interrupt enable/return state',
        'software_loop': 'loop buffer, ILC/RILC, and fetch schedule',
        'nop_idle': 'cycle count and idle state',
        'control_flow': 'PC and delayed-branch pipeline',
    }.get(family, 'unknown; format classification only')


def predicate_description(event, specs):
    value = expanded_word(event)
    names = {spec['name'] for spec in specs}
    if event['compact'] and names == {'lsdx1c'}:
        # SPRUFE8B Figure G-3: CC selects A0/!A0/B0/!B0, not the
        # destination's side or RS-selected register subset.
        cc = (event['word'] >> 14) & 3
        return f'{"zero" if cc & 1 else "nonzero"}:{"B" if cc & 2 else "A"}0'
    if event['compact'] and names & {'s_sbs7c', 's_sbu8c'}:
        bank = 'B' if event['word'] & 1 else 'A'
        return f'{"zero" if (event["word"] >> 4) & 1 else "nonzero"}:{bank}0'
    if event['compact'] and 'nfu_uspldr' in names:
        bank = 'B' if event['word'] & 1 else 'A'
        return f'{"zero" if (event["word"] >> 3) & 1 else "nonzero"}:{bank}0'
    spec = next((item for item in specs if 'creg' in item['fields']), None)
    if spec is None:
        # Explicitly audited formats only. Section 3.6 describes compact
        # operations as unconditional, but G-3 and conditional branch/loop
        # formats above are exceptions; do not generalize to unknown names.
        compact_unconditional = {
            'nfu_unop', 'd_dpp', 'd_dstk', 'lsdmvfr', 'lsdmvto', 'l_lx5',
            'l_l3i', 's_smvk8', 's_sx1b', 'l_l3_sat_0', 'l_l3_sat_1',
            'l_l2c', 's_scs10', 'nfu_uspma', 'nfu_uspmb', 's_sc5', 'l_lx3c',
            's_sx1', 'nfu_uspk', 'nfu_uspl', 'l_lx1', 's_sbs7', 'd_dx5',
            's_ssh5_sat_0', 's_ssh5_sat_1', 'd_dx1', 's_s3i',
            's_s3_sat_0', 's_s3_sat_1', 's_sx5', 's_s2ext', 'l_lx1c',
            'd_dx5p', 'd_dx2op', 's_s2sh',
        }
        compact_unconditional.update(
            f'd_{form}_dsz_{size}' for form in ('doff4', 'dinc', 'dind', 'ddec')
            for size in ('000', '001', '010', '011', '100', '101', '110', '111',
                         '01x', 'x11'))
        full_unconditional = {
            'nfu_nop_idle', 's_call_imm_nop', 'nfu_spkernel', 'nfu_spmask',
            's_ext_1_or_2_src_noncond', 'nfu_dint', 'nfu_rint',
        }
        known = compact_unconditional if event['compact'] else full_unconditional
        if names and names <= known:
            return 'unconditional'
        return 'unconditional_or_format_specific'
    creg = field_value(spec, 'creg', value)
    zero = field_value(spec, 'z', value)
    if creg == 0:
        return 'reserved' if zero else 'unconditional'
    if creg == 7:
        return 'reserved'
    registers = {1: 'B0', 2: 'B1', 3: 'B2', 4: 'A1', 5: 'A2', 6: 'A0'}
    return f'{"zero" if zero else "nonzero"}:{registers[creg]}'


def direct_target(event, format_names):
    word, pc, header = event['word'], event['pc'], event['header']
    if not event['compact'] and word & 0x7c == 0x10:
        return (pc & ~31) + sign_extend((word >> 7) & 0x1fffff, 21) * 4
    if not event['compact'] and word & 0x1ffc == 0x120:
        scale = 2 if header else 4
        return (pc & ~31) + sign_extend((word >> 16) & 0xfff, 12) * scale
    if (event['compact'] and header & 0x8000 and
            (word & 0x3e == 0x0a or word & 0x2e == 0x2a)):
        displacement = ((word >> 6) & 0xff) if word & 0xc000 == 0xc000 else \
            sign_extend((word >> 6) & 0x7f, 7)
        return (pc & ~31) + displacement * 2
    return None


def observed_predicate_outcomes(description, states):
    """Source-fetch observations only; buffered issue and SPMASK are separate."""
    if states is None:
        return None  # Legacy traces did not record predicate state.
    if not isinstance(states, int) or not 0 <= states < (1 << 64):
        raise ValueError('invalid source predicate states')
    if description == 'unconditional':
        return [True] if states else []
    parts = description.split(':')
    if len(parts) != 2 or parts[0] not in ('zero', 'nonzero'):
        return None
    bit = ['B0', 'B1', 'B2', 'A1', 'A2', 'A0'].index(parts[1])
    return sorted({bool(pattern & (1 << bit)) ^ (parts[0] == 'zero')
                   for pattern in range(64) if states & (1 << pattern)})


def branch_delay_slots(event, family):
    if family != 'control_flow':
        return None
    return 5


def _json_key(values):
    return json.dumps(values, sort_keys=True, separators=(',', ':'))


def build_coverage(checkpoint_data, trace_data, format_data):
    memories, input_info = read_input(checkpoint_data)
    specs = read_format_specs(format_data.decode())
    events = [json.loads(line) for line in trace_data.splitlines() if line.strip()]
    summaries = [event for event in events if event.get('event') == 'coverage_summary']
    if len(summaries) != 1:
        raise ValueError('trace must contain exactly one coverage_summary event')
    summary = summaries[0]
    if summary.get('overflow'):
        raise ValueError('replay coverage table overflowed; report would be incomplete')

    pc_events = {}
    for event in events:
        if event.get('event') != 'coverage_pc':
            continue
        event = dict(event)
        event['pc'] = normalize_pc(event['pc'])
        if event['pc'] in pc_events:
            raise ValueError(f'duplicate coverage_pc event at {event["pc"]:#x}')
        if event.get('encoding_changed'):
            raise ValueError(f'instruction encoding changed during replay at {event["pc"]:#x}')
        pc_events[event['pc']] = event

    instructions_by_source = defaultdict(list)
    for event in events:
        if event.get('event') != 'coverage_instruction':
            continue
        event = dict(event)
        for key in ('source_pc', 'packet_next_pc', 'pc'):
            event[key] = normalize_pc(event[key])
        instructions_by_source[event['source_pc']].append(event)
    for source_pc, instructions in instructions_by_source.items():
        instructions.sort(key=lambda item: item['index'])
        if [item['index'] for item in instructions] != list(range(len(instructions))):
            raise ValueError(f'coverage packet indices are incomplete at {source_pc:#x}')
        if any(item['source_pc'] != source_pc for item in instructions):
            raise ValueError(f'coverage packet source mismatch at {source_pc:#x}')

    confirmed_sources = {
        pc for pc, event in pc_events.items()
        if event.get('direct_fetches', 0) + event.get('loop_fetches', 0) > 0
    }
    if confirmed_sources != set(instructions_by_source):
        raise ValueError('source-fetch PCs and captured instruction packets do not match')

    dynamic_edges = []
    probable = set()
    for event in events:
        if event.get('event') != 'coverage_edge':
            continue
        source, target = normalize_pc(event['from']), normalize_pc(event['to'])
        dynamic_edges.append(dict(source=source, target=target, count=event['count'],
                                  kind='observed_source_transition'))
        if target not in confirmed_sources:
            probable.add(target)

    rows, direct_targets, loop_sources = [], [], set()
    for source_pc in sorted(confirmed_sources):
        pc_event = pc_events[source_pc]
        fetches = pc_event.get('direct_fetches', 0) + pc_event.get('loop_fetches', 0)
        packet = instructions_by_source[source_pc]
        for event in packet:
            matched = matching_specs(event, specs)
            names = [spec['name'] for spec in matched] or ['unclassified']
            primary = names[0]
            family = architecture_family(primary)
            unit = UNIT_NAMES.get(instruction_unit(event), 'none_or_unknown')
            side = int(unit[-1]) if unit in UNIT_NAMES.values() else None
            value = expanded_word(event)
            x_values = {field_value(spec, 'x', value) for spec in matched
                        if 'x' in spec['fields']}
            cross = next(iter(x_values)) if len(x_values) == 1 else None
            predicate = predicate_description(event, matched)
            target = direct_target(event, names)
            delay = branch_delay_slots(event, family)
            row = dict(classification='confirmed_executable',
                       emulator_status='completed_without_unsupported_fault',
                       source_pc=source_pc,
                       pc=event['pc'], word=event['word'], header=event['header'],
                       width=16 if event['compact'] else 32,
                       compact=event['compact'], parallel=event['parallel'],
                       packet_next_pc=event['packet_next_pc'],
                       source_fetches=fetches,
                       direct_fetches=pc_event.get('direct_fetches', 0),
                       loop_fetches=pc_event.get('loop_fetches', 0),
                       formats=names, instruction_family=family,
                       functional_unit=unit, side=side, cross_path=cross,
                       predication=predicate, delay_slots=delay,
                       source_predicate_outcomes=observed_predicate_outcomes(
                           predicate, pc_event.get('source_predicate_states')),
                       architectural_state=architectural_state(family),
                       direct_target=target)
            rows.append(row)
            if family == 'software_loop':
                loop_sources.add(source_pc)
            if target is not None:
                target = normalize_pc(target & 0xffffffff)
                direct_targets.append(dict(source=event['pc'], target=target,
                                           format=primary,
                                           kind='call' if primary.startswith('s_call') else 'branch'))
                if target not in confirmed_sources:
                    probable.add(target)

    stops = [event for event in events if event.get('event') == 'stop']
    faults, unsupported = [], []
    for event in stops:
        if event.get('fault'):
            fault_pc = normalize_pc(event.get('fault_pc') or event.get('pc'))
            fault = dict(pc=fault_pc, word=event.get('fault_word'),
                         reason=event['fault'])
            faults.append(fault)
            if event['fault'] in UNSUPPORTED_ENCODING_FAULTS:
                unsupported.append(fault)
            probable.add(fault_pc)
        elif event.get('reason') == 'breakpoint':
            probable.add(normalize_pc(event['pc']))

    probable_rows = []
    tuples = [(spec['name'], spec['width'], spec['value'], spec['mask']) for spec in specs]
    for pc in sorted(probable - confirmed_sources):
        try:
            packet = decode_packet(memories, pc, tuples)
            probable_rows.append(dict(classification='probable_code', pc=pc,
                                      packet=[dict(pc=row['pc'], word=row['word'],
                                                   width=row['width'], compact=row['compact'],
                                                   parallel=row['parallel'],
                                                   formats=row['families'])
                                              for row in packet],
                                      basis='uncompleted dynamic or direct control-flow target',
                                      limitation='The connected transcript did not complete this source packet; format matches do not prove opcode support or semantics.'))
        except ValueError as error:
            probable_rows.append(dict(classification='probable_code', pc=pc,
                                      decode_error=str(error),
                                      basis='uncompleted dynamic or direct control-flow target'))

    grouped = {}
    encoding_sets = defaultdict(set)
    for row in rows:
        dimensions = dict(instruction_family=row['instruction_family'],
                          encodings=row['formats'], functional_unit=row['functional_unit'],
                          side=row['side'], compact=row['compact'], cross_path=row['cross_path'],
                          predication=row['predication'], delay_slots=row['delay_slots'],
                          architectural_state=row['architectural_state'])
        key = _json_key(dimensions)
        group = grouped.setdefault(key, {**dimensions, 'unique_instruction_addresses': 0,
                                         'packet_fetch_observations': 0,
                                         'examples': []})
        group['unique_instruction_addresses'] += 1
        group['packet_fetch_observations'] += row['source_fetches']
        encoding_sets[key].add((row['width'], row['word'], row['header']))
        if len(group['examples']) < 8:
            group['examples'].append(row['pc'])
    groups = []
    for key, group in grouped.items():
        group['unique_instruction_encodings'] = len(encoding_sets[key])
        groups.append(group)
    groups.sort(key=lambda group: (-group['packet_fetch_observations'],
                                   group['instruction_family'], group['functional_unit']))

    distinct_words = {(row['width'], row['word'], row['header']) for row in rows}
    self_edges = [edge for edge in dynamic_edges if edge['source'] == edge['target']]
    return dict(
        schema=1,
        evidence_scope='completed deterministic replay source packets only',
        source_predicate_audit=dict(
            true_observed_addresses=sum(True in (row['source_predicate_outcomes'] or [])
                                        for row in rows),
            false_only_addresses=sum(row['source_predicate_outcomes'] == [False]
                                     for row in rows),
            unavailable_addresses=sum(row['source_predicate_outcomes'] is None
                                      for row in rows),
            no_observations_addresses=sum(row['source_predicate_outcomes'] == []
                                          for row in rows)),
        validation_eligible=not faults,
        caveats=[
            'A completed packet proves that this emulator accepted that observed encoding; it does not prove architectural correctness or that a predicate body was true.',
            'source_predicate_outcomes samples predicate registers before successful source fetch steps; it does not establish buffered instruction execution or account for SPMASK suppression. Null means unavailable or format-specific.',
            'Software-loop scheduler_cycles count issue cycles at a parked fetch PC and are not instruction-issue frequencies; only direct_fetches and loop_fetches establish source-packet observations.',
            'Observed source transitions span delayed branches, loop scheduling, and external event resumes; they are dynamic continuity evidence, not attributed branch edges.',
            'Unvisited captured memory remains unclassified and may be code or data.',
        ],
        input=input_info,
        trace_summary=summary,
        progress={key: summary.get(key) for key in
                  ('initial_packets', 'final_packets', 'packet_delta',
                   'initial_cycles', 'final_cycles', 'cycle_delta')
                  if key in summary},
        counts=dict(confirmed_source_packets=len(confirmed_sources),
                    confirmed_instruction_addresses=len(rows),
                    confirmed_distinct_encodings=len(distinct_words),
                    probable_code_addresses=len(probable_rows),
                    unsupported_faults=len(unsupported),
                    execution_faults=len(faults),
                    dynamic_edges=len(dynamic_edges), self_edges=len(self_edges),
                    grouped_rows=len(groups)),
        entry_points=[normalize_pc(summary['first_pc'])] if confirmed_sources else [],
        confirmed_source_packets=[pc_events[pc] for pc in sorted(confirmed_sources)],
        confirmed_instructions=rows,
        probable_code=probable_rows,
        unsupported=unsupported,
        faults=faults,
        control_flow=dict(observed_source_transitions=dynamic_edges,
                          direct_targets=direct_targets,
                          software_loop_source_packets=sorted(loop_sources),
                          self_transitions=self_edges),
        groups=groups,
        unclassified_memory_policy='All captured addresses not listed as confirmed or probable remain unclassified; no format scan is treated as code proof.',
    )


def write_coverage(checkpoint, trace, formats, output):
    checkpoint_data, trace_data, format_data = (checkpoint.read_bytes(), trace.read_bytes(),
                                                 formats.read_bytes())
    report = build_coverage(checkpoint_data, trace_data, format_data)
    # A trace/checkpoint pair does not establish execution-mode provenance.
    # Only the replay orchestrator evaluates inherited approximations and
    # connected/repeat gates. Standalone reanalysis must not launder an
    # exploratory checkpoint into an architectural validation result.
    report['architectural_validation_eligible'] = False
    report['validation_eligible'] = False
    report['validation_provenance'] = 'not evaluated by standalone coverage analysis; use replay gates'
    report['sha256'] = {
        'checkpoint': hashlib.sha256(checkpoint_data).hexdigest(),
        'trace': hashlib.sha256(trace_data).hexdigest(),
        'formats': hashlib.sha256(format_data).hexdigest(),
        'coverage.py': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    with output.open('x') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint', type=Path, help='final deterministic checkpoint')
    parser.add_argument('trace', type=Path, help='replay JSONL with coverage events')
    parser.add_argument('output', type=Path, help='new JSON coverage report')
    parser.add_argument('--formats', type=Path, required=True,
                        help='GNU include/opcode/tic6x-insn-formats.h')
    args = parser.parse_args()
    try:
        report = write_coverage(args.checkpoint, args.trace, args.formats, args.output)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    counts = report['counts']
    print(f"{counts['confirmed_source_packets']} confirmed packets, "
          f"{counts['confirmed_instruction_addresses']} instruction addresses, "
          f"{counts['unsupported_faults']} unsupported faults")


if __name__ == '__main__':
    main()
