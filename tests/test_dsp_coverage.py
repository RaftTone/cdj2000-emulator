"""Dynamic coverage must distinguish completed packets from probable code."""
import json
import struct
import pytest

from tools.cdj_dsp.coverage import (build_coverage, observed_predicate_outcomes,
                                    predicate_description, write_coverage)


@pytest.mark.parametrize('cc,expected', list(enumerate(
    ['nonzero:A0', 'zero:A0', 'nonzero:B0', 'zero:B0'])))
def test_compact_mvk_predicate_is_independent_of_side_and_rs(cc, expected):
    for side in (0, 1):
        for rs in (0, 1 << 19):
            event = dict(word=(cc << 14) | 0x0866 | side, compact=True, header=rs)
            assert predicate_description(event, [dict(name='lsdx1c', fields={})]) == expected


def test_only_audited_unconditional_formats_are_classified():
    for compact, names in ((True, ['l_l2c', 'd_doff4_dsz_01x', 'd_doff4_dsz_x11']),
                           (False, ['nfu_nop_idle', 'nfu_spkernel'])):
        event = dict(word=0, compact=compact, header=0)
        specs = [dict(name=name, fields={}) for name in names]
        assert predicate_description(event, specs) == 'unconditional'
        assert predicate_description(event, []) == 'unconditional_or_format_specific'
        assert predicate_description(event, specs + [dict(name='unknown', fields={})]) == \
            'unconditional_or_format_specific'


@pytest.mark.parametrize('bit,register', list(enumerate(['B0', 'B1', 'B2', 'A1', 'A2', 'A0'])))
def test_source_predicate_observations(bit, register):
    nonzero = 1 << (1 << bit)
    assert observed_predicate_outcomes('nonzero:' + register, nonzero) == [True]
    assert observed_predicate_outcomes('zero:' + register, nonzero) == [False]
    assert observed_predicate_outcomes('nonzero:' + register, 1) == [False]
    assert observed_predicate_outcomes('zero:' + register, nonzero | 1) == [False, True]


def test_source_predicate_missing_and_invalid_observations():
    assert observed_predicate_outcomes('unconditional', None) is None
    assert observed_predicate_outcomes('unconditional', 0) == []
    assert observed_predicate_outcomes('unconditional', 1) == [True]
    assert observed_predicate_outcomes('unconditional_or_format_specific', 1) is None
    for mask in (-1, 1 << 64, '1'):
        with pytest.raises(ValueError):
            observed_predicate_outcomes('unconditional', mask)
from tools.cdj_dsp.inventory import CHECKPOINT_HEADER, SHARED_RAM_SIZE, _fnv1a


FORMATS = b'''
FMT(s_mvk, 32, 0x28, 0x3c,
    CFLDS4(FLD(s, 1, 1), FLD(h, 6, 1), FLD(cst, 7, 16), FLD(dst, 23, 5)))
FMT(s_ext_branch_cond_imm, 32, 0x10, 0x7c,
    CFLDS2(FLD(s, 1, 1), FLD(cst, 7, 21)))
FMT(s_call_imm_nop, 32, 0x10, 0xe000007c,
    NFLDS3(FLD(s, 1, 1), FLD(cst, 7, 21), FLD(z, 28, 1)))
'''


def checkpoint(words=()):
    state = bytes(16)
    l2 = bytearray(0x40000)
    for offset, word in words:
        struct.pack_into('<I', l2, offset, word)
    payload = state + l2 + bytes(SHARED_RAM_SIZE) + bytes(1024)
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP5\0', 5, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    return header + payload


def trace(*events):
    return b''.join((json.dumps(event) + '\n').encode() for event in events)


def test_confirmed_packets_groups_edges_and_loop_scheduler_caveat():
    mvk = (3 << 23) | (123 << 7) | 0x28
    branch = 0x10 | (1 << 7)  # Fetch-packet base + 4: self target at 0x11800024.
    data = trace(
        {'event': 'coverage_summary', 'first_pc': 0x00800020,
         'last_pc': 0x11800024, 'unique_pcs': 2, 'unique_edges': 2,
         'unique_source_pcs': 2, 'source_fetches': 4,
         'scheduler_cycles': 3, 'idle_cycles': 0, 'overflow': False},
        {'event': 'coverage_pc', 'pc': 0x00800020, 'direct_fetches': 1,
         'loop_fetches': 0, 'scheduler_cycles': 0, 'idle_cycles': 0,
         'encoding_changed': False},
        {'event': 'coverage_instruction', 'source_pc': 0x00800020,
         'packet_next_pc': 0x00800024, 'index': 0, 'pc': 0x00800020,
         'word': mvk, 'header': 0, 'compact': False, 'parallel': False},
        {'event': 'coverage_pc', 'pc': 0x11800024, 'direct_fetches': 1,
         'loop_fetches': 2, 'scheduler_cycles': 3, 'idle_cycles': 0,
         'encoding_changed': False},
        {'event': 'coverage_instruction', 'source_pc': 0x11800024,
         'packet_next_pc': 0x11800028, 'index': 0, 'pc': 0x11800024,
         'word': branch, 'header': 0, 'compact': False, 'parallel': False},
        {'event': 'coverage_edge', 'from': 0x00800020, 'to': 0x11800024, 'count': 1},
        {'event': 'coverage_edge', 'from': 0x11800024, 'to': 0x11800024, 'count': 2},
        {'event': 'stop', 'reason': 'step_limit', 'fault': '', 'pc': 0x11800024,
         'fault_pc': 0, 'fault_word': 0},
    )
    report = build_coverage(checkpoint([(0x20, mvk), (0x24, branch)]), data, FORMATS)
    assert report['validation_eligible']
    assert report['entry_points'] == [0x11800020]
    assert report['counts']['confirmed_source_packets'] == 2
    assert report['counts']['confirmed_instruction_addresses'] == 2
    assert report['counts']['self_edges'] == 1
    assert report['probable_code'] == [] and report['unsupported'] == []
    branch_row = next(row for row in report['confirmed_instructions']
                      if row['pc'] == 0x11800024)
    assert branch_row['instruction_family'] == 'control_flow'
    assert branch_row['direct_target'] == 0x11800024
    assert branch_row['delay_slots'] == 5
    assert branch_row['source_fetches'] == 3


def test_standalone_coverage_cannot_claim_execution_provenance(tmp_path):
    checkpoint_path = tmp_path / 'final.cdjdsp'
    trace_path = tmp_path / 'trace.jsonl'
    formats_path = tmp_path / 'formats.h'
    output = tmp_path / 'coverage.json'
    checkpoint_path.write_bytes(checkpoint())
    trace_path.write_bytes(trace(
        dict(event='coverage_summary', first_pc=0, last_pc=0,
             unique_pcs=0, unique_edges=0, unique_source_pcs=0,
             source_fetches=0, scheduler_cycles=0, idle_cycles=0, overflow=False),
        dict(event='stop', reason='step_limit', fault='')))
    formats_path.write_bytes(FORMATS)
    report = write_coverage(checkpoint_path, trace_path, formats_path, output)
    assert not report['validation_eligible']
    assert not report['architectural_validation_eligible']
    assert json.loads(output.read_text()) == report


def test_faulting_word_is_probable_and_never_confirmed():
    data = trace(
        {'event': 'coverage_summary', 'first_pc': 0, 'last_pc': 0,
         'unique_pcs': 0, 'unique_edges': 0, 'unique_source_pcs': 0,
         'source_fetches': 0, 'scheduler_cycles': 0, 'idle_cycles': 0,
         'overflow': False},
        {'event': 'stop', 'reason': 'fault', 'fault': 'instruction not implemented',
         'pc': 0x00800020, 'fault_pc': 0x00800020, 'fault_word': 0xffffffff},
    )
    report = build_coverage(checkpoint([(0x20, 0xffffffff)]), data, FORMATS)
    assert not report['validation_eligible']
    assert report['confirmed_instructions'] == []
    assert report['unsupported'] == [
        {'pc': 0x11800020, 'word': 0xffffffff, 'reason': 'instruction not implemented'}]
    assert report['probable_code'][0]['pc'] == 0x11800020


@pytest.mark.parametrize('reason,missing_encoding', [
    ('instruction not implemented', True),
    ('compact instruction not implemented', True),
    ('parallel register write conflict', False),
    ('delayed-result write conflict', False),
    ('unmapped memory read', False),
    ('SPLOOP retained schedule mismatch', False),
    ('circular memory addressing not implemented', False),
])
@pytest.mark.parametrize('stop_reason', ['fault', 'connected stop'])
def test_fault_classification_preserves_non_isa_blockers(reason, missing_encoding,
                                                        stop_reason):
    data = trace(
        {'event': 'coverage_summary', 'first_pc': 0, 'last_pc': 0,
         'overflow': False},
        {'event': 'stop', 'reason': stop_reason, 'fault': reason,
         'pc': 0x00800020, 'fault_pc': 0x00800020, 'fault_word': 0x2627},
    )
    report = build_coverage(checkpoint([(0x20, 0x2627)]), data, FORMATS)
    assert not report['validation_eligible']
    assert report['counts']['execution_faults'] == 1
    assert report['counts']['unsupported_faults'] == int(missing_encoding)
    assert report['faults'] == [
        {'pc': 0x11800020, 'word': 0x2627, 'reason': reason}]
    assert report['unsupported'] == (report['faults'] if missing_encoding else [])
    assert report['confirmed_instructions'] == []
