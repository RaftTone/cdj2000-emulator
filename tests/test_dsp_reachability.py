"""Reachability must keep execution evidence and byte-pattern evidence apart."""
import json
import os
from pathlib import Path

import pytest

from tools.cdj_dsp.reachability import (ROOT, base_mnemonic, main, neighbourhood_packets,
                                        packet_instructions, requirement_groups,
                                        unimplemented_rows, verdict)

KNOWN_PRESENT = ('ldw', 'stw', 'bnop', 'mvk')


@pytest.mark.parametrize('name,expected', [
    ('ADD2', 'add2'), ('MPY32 (32-bit result)', 'mpy32'), ('B NRP', 'b'), ('  SWE ', 'swe'),
])
def test_row_name_is_not_an_encoding(name, expected):
    assert base_mnemonic(name) == expected


def test_presence_never_becomes_execution():
    # Candidates above the noise floor are still only candidates.
    assert verdict(0, True, 500, 10, 5) == 'reached-candidate-only'
    assert verdict(1, True, 0, 0, 0) == 'reached-confirmed'
    assert verdict(0, True, 3, 9, 0) == 'not-found'
    assert verdict(0, True, 0, 0, 0) == 'not-found'
    assert verdict(0, False, 999, 0, 9) == 'not-scanned'


def test_a_fetch_packet_with_any_undefined_word_contributes_nothing():
    good = [(0, 'add .L1 a1,a2,a3'), (4, 'nop 1')]
    assert packet_instructions(good) == [(0, 4, 'add', 'a1,a2,a3'), (4, 28, 'nop', '1')]
    assert packet_instructions(good + [(8, '<undefined instruction 0xffffffff>')]) is None
    # A compact fetch-packet header is structure, not a candidate instruction.
    assert packet_instructions([(0, 'nop 2'), (28, '<fetch packet header 0xee000300>')]) == \
        [(0, 28, 'nop', '2')]


def test_neighbourhood_is_whole_fetch_packets():
    assert neighbourhood_packets({0x1000}, 0) == set()
    assert neighbourhood_packets({0x1000}, 64) == {0xfc0, 0xfe0, 0x1000, 0x1020, 0x1040}


def test_shared_mnemonics_are_not_credited_to_the_unimplemented_form():
    probe = dict(
        rows_by_status={'all-probed-forms-rejected': ['ADD2', 'B NRP'],
                        'some-forms-rejected': ['CMPGT']},
        instructions={
            'ADD2': dict(candidates=[dict(word='02000058', detail='instruction not implemented')]),
            'B NRP': dict(candidates=[dict(word='00000362', detail='instruction not implemented')]),
            'CMPGT': dict(candidates=[dict(word='021c0478', detail='instruction not implemented')]),
            'ADD': dict(candidates=[dict(word='02184078', detail=None)]),
        })
    rows = unimplemented_rows(probe)
    assert set(rows) == {'ADD2', 'B NRP', 'CMPGT'}
    assert rows['ADD2']['resolution'] == 'row'
    assert rows['B NRP']['resolution'] == 'mnemonic-shared'
    assert rows['CMPGT']['resolution'] == 'operand-shape'
    assert rows['ADD2']['nonconditional'] is False


def test_group_row_counts_are_checked_against_the_audit_text():
    groups = requirement_groups(dict(rows=[dict(
        id='ISA-PACK8', implementation='unsupported',
        requirement='Packed 4x8: ADD4 SUB4 SADDU4 (3 rows)')]))
    assert groups['ISA-PACK8']['mnemonics'] == ['ADD4', 'SUB4', 'SADDU4']
    assert groups['ISA-PACK8']['parsed_rows'] == groups['ISA-PACK8']['declared_rows'] == 3


def fixture(tmp_path):
    (tmp_path / 'probe.json').write_text(json.dumps(dict(
        rows_by_status={'all-probed-forms-rejected': ['ADD2', 'SWE']},
        instructions={
            'ADD2': dict(candidates=[dict(word='02000058',
                                          detail='instruction not implemented')]),
            'SWE': dict(candidates=[dict(word='10000000',
                                         detail='instruction not implemented')]),
        })))
    (tmp_path / 'inventory.json').write_text(json.dumps(dict(rows=[
        dict(id='ISA-PACK16', implementation='unsupported',
             requirement='Packed 2x16: ADD2 (1 rows)')])))
    (tmp_path / 'semantic.json').write_text(json.dumps(dict(instructions=[
        dict(pc=0x11800020, mnemonic='ldw', compact=False, source_fetches=7,
             operands='*+a4[3],b6', word=0x100),
        dict(pc=0x11800024, mnemonic='cmpgt', compact=False, source_fetches=7,
             operands='a5:a4,a6,a0', word=0x200)])))
    return [str(tmp_path / 'out.json'), '--probe', str(tmp_path / 'probe.json'),
            '--coverage-inventory', str(tmp_path / 'inventory.json'),
            '--semantic-inventory', str(tmp_path / 'semantic.json'),
            '--coverage', str(tmp_path / 'none-*.json'),
            '--image', str(tmp_path / 'none-*.bin'), '--no-compact-sweep']


def test_tool_runs_without_a_disassembler_and_reports_not_scanned(tmp_path):
    assert main(fixture(tmp_path)) == 0
    report = json.loads((tmp_path / 'out.json').read_text())
    assert report['confirmed_executed_by_mnemonic']['ldw']['addresses'] == 1
    assert report['confirmed_executed_by_mnemonic']['cmpgt']['long40_addresses'] == 1
    assert report['rows']['ADD2']['verdict'] == 'not-scanned'
    assert report['rows']['SWE']['nonconditional'] is True
    assert report['encoding_forms']['nonconditional_extensions']['rows'] == ['SWE']
    assert report['validation_eligible'] is False


def semantic_inventory():
    path = os.environ.get('CDJ_DSP_SEMANTIC_INVENTORY',
                          str(ROOT / 'runs' / 'dsp-semantic-inventory-3.json'))
    if not Path(path).is_file():
        pytest.skip('no semantic inventory; set CDJ_DSP_SEMANTIC_INVENTORY')
    return path


def test_known_present_instructions_keep_nonzero_confirmed_executed(tmp_path):
    """A silent regression in the confirmed-executed side must fail here."""
    argv = fixture(tmp_path)
    argv[argv.index('--semantic-inventory') + 1] = semantic_inventory()
    assert main(argv) == 0
    report = json.loads((tmp_path / 'out.json').read_text())
    executed = report['confirmed_executed_by_mnemonic']
    for mnemonic in KNOWN_PRESENT:
        assert executed[mnemonic]['addresses'] > 0, mnemonic
        assert executed[mnemonic]['source_fetch_observations'] > 0, mnemonic
    assert report['counts']['confirmed_executed_addresses'] > 1000
    assert {row['verdict'] for row in report['rows'].values()} <= {
        'reached-confirmed', 'reached-candidate-only', 'not-found', 'not-scanned'}


def test_static_scan_separates_firmware_from_its_own_noise_floor(tmp_path):
    disassembler = os.environ.get('C6X_DISASSEMBLER')
    if not disassembler:
        pytest.skip('set C6X_DISASSEMBLER to the built tic6x frontend')
    images = sorted(Path(semantic_inventory()).parent.glob('*/dsp-l2.bin'))[:2]
    if not images:
        pytest.skip('no captured L2 images beside the semantic inventory')
    argv = fixture(tmp_path)
    argv[argv.index('--semantic-inventory') + 1] = semantic_inventory()
    argv[argv.index('--image') + 1] = str(images[0].parent.parent / '*' / 'dsp-l2.bin')
    argv += ['--disassembler', disassembler]
    assert main(argv) == 0
    report = json.loads((tmp_path / 'out.json').read_text())
    scan = report['static_scan']
    assert scan['state'] == 'scanned'
    # Real code decodes completely far more often than a blob of the same bytes shuffled.
    firmware = scan['totals']['fully_decoding_fetch_packets']
    for control in scan['noise_floor']['controls'].values():
        assert control['totals']['unique_fetch_packets'] == \
            scan['totals']['unique_fetch_packets']
        assert control['totals']['fully_decoding_fetch_packets'] < firmware
