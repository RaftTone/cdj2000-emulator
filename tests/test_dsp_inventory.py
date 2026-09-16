"""Discovery scans must respect mixed fetch layout and never imply execution."""
import struct

import pytest

from tools.cdj_dsp.inventory import (
    BASE, CHECKPOINT_HEADER, SHARED_RAM_BASE, SHARED_RAM_SIZE, SDRAM_BASE,
    _fnv1a, build_report, decode_one, expression, read_format_specs,
    read_formats, read_input,
)


def test_mixed_layout_header_flags_and_trace_evidence():
    data = bytearray(0x40000)
    struct.pack_into('<IHHI', data, 0x20, 0x4683e000, 0xec6e, 0x2d66, 0x02809428)
    struct.pack_into('<I', data, 0x3c, 0xe0400008)
    formats = read_formats('''
        FMT(nop, 16, 0x0c6e, 0x1fff, ignored)
        FMT(mask, 16, 0x2c66, 0x3c7e, ignored)
        FMT(loop, 32, 0x3e000, 0x7ffffe, ignored)
        FMT(mvk, 32, 0x28, 0x7c, ignored)
    ''')
    report = build_report(data, [(BASE + 0x20, BASE + 0x40)], formats,
                          [{'event': 'step', 'pc': 0x00800026}])
    rows = report['instructions']
    assert [r['width'] for r in rows[:4]] == [32, 16, 16, 32]
    assert [r['families'] for r in rows[:4]] == [['loop'], ['nop'], ['mask'], ['mvk']]
    assert rows[2]['parallel'] and rows[2]['trace_pc_visits'] == 1
    assert all(r['pc'] != BASE + 0x3c for r in rows)
    assert report['discovery_only'] and not report['execution_performed']
    assert expression('DSZ(4) | SAT(1) | BR(1) | 0x4') == 0x130004
    with pytest.raises(ValueError):
        expression('__import__("os")')
    with pytest.raises(ValueError):
        build_report(data, [(BASE + 0x22, BASE + 0x24)], formats, [])


def test_header_bits_select_distinct_formats():
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 28, 0xe0208000)
    formats = read_formats('''FMT(arithmetic, 16, BR(0), BR(1), ignored)
                             FMT(branch, 16, BR(1), BR(1), ignored)''')
    report = build_report(data, [(BASE, BASE + 2)], formats, [])
    assert report['instructions'][0]['families'] == ['branch']


def test_balanced_format_parser_preserves_fields_and_specific_decode():
    source = '''
        /* commas inside fields must not split FMT arguments */
        FMT(generic, 32, 0x20, 0x3c,
            CFLDS3(FLD(s, 1, 1), FLD(x, 12, 1),
                   COMPFLD(cst, BFLD2(BFLD(7, 2, 0), BFLD(13, 3, 2)))))
        FMT(specific, 32, 0xa0, 0xfc,
            NFLDS1(FLD(dst, 23, 5)))
    '''
    specs = read_format_specs(source)
    assert [spec['name'] for spec in specs] == ['generic', 'specific']
    assert specs[0]['fields']['creg'] == (29, 3)
    assert specs[0]['fields']['x'] == (12, 1)
    assert specs[0]['composite_fields'] == ['cst']
    data = bytearray(0x40)
    struct.pack_into('<I', data, 0, 0xa0)
    row = decode_one(data, BASE, BASE, specs)
    assert row['families'] == ['specific'] and row['next_pc'] == BASE + 4


def test_schema5_checkpoint_shared_ram_sparse_sdram_inventory_and_corruption_rejection():
    state = bytes(16)
    l2 = bytes(0x40000)
    shared = bytearray(SHARED_RAM_SIZE)
    struct.pack_into('<I', shared, 0x20, 0x89abcdef)
    bitmap = bytearray(1024)
    bitmap[2 // 8] |= 1 << (2 % 8)
    page = bytearray(4096)
    struct.pack_into('<I', page, 0, 0x12345678)
    payload = state + l2 + shared + bitmap + page
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP5\0', 5, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 1,
        len(payload), _fnv1a(payload),
    )
    memories, info = read_input(header + payload)
    assert info['kind'] == 'checkpoint' and info['schema'] == 5
    assert info['shared_ram_captured'] and info['present_sdram_pages'] == 1
    formats = read_formats('FMT(test, 32, 0x12345678, 0xffffffff, ignored)')
    start = SDRAM_BASE + 2 * 4096
    report = build_report(memories, [(start, start + 4)], formats,
                          [{'event': 'step', 'pc': start}])
    assert report['instructions'][0]['families'] == ['test']
    assert report['instructions'][0]['trace_pc_visits'] == 1
    shared_formats = read_formats('FMT(shared, 32, 0x89abcdef, 0xffffffff, ignored)')
    shared_report = build_report(memories,
                                 [(SHARED_RAM_BASE + 0x20, SHARED_RAM_BASE + 0x24)],
                                 shared_formats, [])
    assert shared_report['instructions'][0]['families'] == ['shared']
    damaged = bytearray(header + payload)
    damaged[-1] ^= 1
    with pytest.raises(ValueError, match='checksum'):
        read_input(damaged)


def test_schema1_checkpoint_remains_readable_without_invented_shared_ram():
    state = bytes(16)
    l2 = bytes(0x40000)
    bitmap = bytes(1024)
    payload = state + l2 + bitmap
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP1\0', 1, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    memories, info = read_input(header + payload)
    assert info['schema'] == 1 and not info['shared_ram_captured']
    assert SHARED_RAM_BASE not in memories


@pytest.mark.parametrize(('schema', 'magic'), [
    (10, b'CDJDSP10'),
    (11, b'CDJDSP11'),
    (12, b'CDJDSP12'),
])
def test_current_checkpoint_magics_are_accepted_by_metadata_readers(
        tmp_path, schema, magic):
    from tools.cdj_dsp.replay import checkpoint_info
    from tools.cdj_main.nxs_vm import checkpoint_metadata

    state = (struct.pack('<QQIIBBBB', 0, 0, 0, 4096, 0, 0, 1, 0) +
             bytes(4) if schema >= 11 else bytes(16))
    l2 = bytes(0x40000)
    shared = bytes(SHARED_RAM_SIZE)
    bitmap = bytes(1024)
    l1d = bytes(0x8000) if schema >= 12 else b''
    payload = state + l2 + shared + l1d + bitmap
    header = CHECKPOINT_HEADER.pack(
        magic, schema, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    raw = header + payload
    _, inventory_info = read_input(raw)
    replay_info = checkpoint_info(raw)
    path = tmp_path / f'schema{schema}.cdjdsp'
    path.write_bytes(raw)
    vm_info = checkpoint_metadata(path)
    assert inventory_info['schema'] == schema
    assert replay_info['schema'] == schema
    assert vm_info['schema'] == schema
    expected_scheduler = schema >= 11
    assert inventory_info['scheduler_state_captured'] is expected_scheduler
    assert replay_info['scheduler_state_captured'] is expected_scheduler
    assert vm_info['scheduler_state_captured'] is expected_scheduler
    expected_mode = 'deferred-v1' if schema >= 11 else 'legacy'
    assert inventory_info['dsp_scheduler_mode'] == expected_mode
    assert replay_info['dsp_scheduler_mode'] == expected_mode
    assert vm_info['dsp_scheduler_mode'] == expected_mode


def test_schema11_invalid_scheduler_mode_is_rejected_by_metadata_readers(tmp_path):
    from tools.cdj_dsp.replay import checkpoint_info
    from tools.cdj_main.nxs_vm import checkpoint_metadata

    state = struct.pack('<QQIIBBBB', 0, 0, 0, 4096, 0, 0, 2, 0) + bytes(4)
    l2 = bytes(0x40000)
    shared = bytes(SHARED_RAM_SIZE)
    bitmap = bytes(1024)
    payload = state + l2 + shared + bitmap
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP11', 11, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    raw = header + payload
    with pytest.raises(ValueError, match='scheduler state is invalid'):
        read_input(raw)
    with pytest.raises(ValueError, match='scheduler state is invalid'):
        checkpoint_info(raw)
    path = tmp_path / 'invalid-scheduler.cdjdsp'
    path.write_bytes(raw)
    with pytest.raises(RuntimeError, match='scheduler state is invalid'):
        checkpoint_metadata(path)


def test_checkpoint_metadata_rejects_valid_checksum_with_invalid_geometry(tmp_path):
    from tools.cdj_main.nxs_vm import checkpoint_metadata

    state = bytes(16)
    l2 = bytes(0x40000)
    bitmap = bytes(1024)
    payload = state + l2 + bitmap
    # Keep the checksum and total payload length valid while lying about the
    # fixed L2 geometry.  Metadata readers must agree with replay readers.
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP1\0', 1, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x20000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    path = tmp_path / 'invalid-geometry.cdjdsp'
    path.write_bytes(header + payload)
    with pytest.raises(RuntimeError, match='incompatible or incomplete'):
        checkpoint_metadata(path)


def test_checkpoint_metadata_rejects_valid_checksum_with_corrupt_sparse_bitmap(tmp_path):
    from tools.cdj_main.nxs_vm import checkpoint_metadata

    state = bytes(16)
    l2 = bytes(0x40000)
    bitmap = bytearray(1024)
    bitmap[0] = 1  # The header claims zero present SDRAM pages.
    payload = state + l2 + bitmap
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP1\0', 1, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    path = tmp_path / 'invalid-sparse-bitmap.cdjdsp'
    path.write_bytes(header + payload)
    with pytest.raises(RuntimeError, match='incompatible or incomplete'):
        checkpoint_metadata(path)
