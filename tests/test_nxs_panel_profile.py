"""Keep NXS key bindings separate and check names against the real image."""
from pathlib import Path
import struct
import os
from unittest.mock import Mock

import pytest

from tools.cdj_main import nxs_panel, panel_control
from tools.cdj_gui import faceplate, view_ui


def test_sources_and_time_are_shifted_only_in_nxs():
    assert panel_control.button_mask("usb") == (19, 2)
    assert nxs_panel.button_mask("sd") == (19, 0x08)
    assert nxs_panel.button_mask("usb") == (19, 0x04)
    assert nxs_panel.button_mask("enter") == (17, 0x01)
    assert [nxs_panel.deck_input(f"19.{i}") for i in range(5)] == [
        "19.1", "19.2", "19.3", "19.4", "19.5"]
    for source, expected in zip(("LINK", "USB", "SD", "DISC"), range(1, 5)):
        control = next(c for c in view_ui.controls(True) if c.label == source)
        assert control.input_id == f"19.{expected}"
        assert control.lines == (panel_control.encode_press(19, 1 << expected,
                                                           view_ui.WINDOW_HOLD_MS),)


def test_nxs_inventory_and_deck_have_no_lost_or_duplicate_bindings():
    built = view_ui.controls(True)
    reached, missing, stray = view_ui.coverage(built, True)
    assert not missing and not stray
    assert set(reached) == set(nxs_panel.input_ids())
    mapped = [nxs_panel.deck_input(key) for key in faceplate.PLACEMENTS]
    assert len(mapped) == len(set(mapped))
    assert {key for key in mapped if key is not None} <= {c.input_id for c in built}
    assert nxs_panel.deck_input('17.1') is None  # legacy 4-BEAT LOOP is not LOOP MODE evidence
    assert view_ui.coverage(view_ui.controls())[1:] == ([], [])


def test_changed_contacts_match_nxs_firmware_service_names():
    image = Path(__file__).resolve().parents[1] / "firmware/nxs/main-unpacked.bin"
    if not image.exists():
        pytest.skip("NXS firmware is not supplied")
    data = image.read_bytes()
    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]
    def name(code):
        offset = u32(0x3c32f0 + code * 4) - 0xa4000000
        return data[offset:offset + 40].split(b"\0")[0].decode("ascii")
    # Decoder 042f5810: physical contact -> byte/bit in status record.
    decoded = {
        (17, 1): (0x53, 3), (17, 3): (0x53, 4),
        (18, 0): (0x53, 7), (18, 1): (0x4e, 5),
        (18, 2): (0x4e, 0), (18, 3): (0x4d, 7),
        (18, 4): (0x4d, 6), (18, 5): (0x4d, 5),
        (18, 6): (0x4c, 4), (18, 7): (0x4c, 3),
        (19, 0): (0x5e, 4), (19, 1): (0x5f, 7),
        (19, 2): (0x5f, 6), (19, 3): (0x5f, 5),
        (19, 4): (0x5f, 4), (19, 5): (0x4d, 0),
        (19, 6): (0x4c, 5), (19, 7): (0x4c, 6),
        (20, 6): (0x4c, 7), (21, 0): (0x4d, 4),
        (21, 1): (0x53, 6), (21, 2): (0x53, 5),
        (21, 3): (0x4d, 3), (21, 4): (0x4d, 2),
        (21, 5): (0x4d, 1),
    }
    names = {}
    for base, table in [(0x4c, 0xb0eb4), (0x50, 0xb0fac), (0x5c, 0xb0fdc)]:
        while (mask := u32(table)):
            bit = mask.bit_length() - 1
            names[base + bit // 8, bit % 8] = name(u32(table + 4))
            table += 8
    for contact, status in decoded.items():
        assert nxs_panel.KEY_NAMES[contact] == names[status]


def test_nxs_rev_polarity_is_backed_by_decoder_instructions():
    """The NXS decoder's two observed active-low contacts are image-backed."""
    image = Path(__file__).resolve().parents[1] / "firmware/nxs/main-unpacked.bin"
    if not image.exists():
        pytest.skip("NXS firmware is not supplied")
    data = image.read_bytes()

    # Runtime 0x042f5810 is at offset 0x002f5810 in this address-zero image.
    # 042f58b8 loads byte 15, then 042f58d4 tests bit 1.  The short branch's
    # annulled slot clears status bit 6 when the input is zero; the fallthrough
    # OR sets it when the input is one.
    assert data[0x2f58b8:0x2f58ba] == bytes.fromhex("7f84")  # mov.b @(15,r7),r0
    words = [struct.unpack_from("<H", data, address)[0]
             for address in (0x2f58d4, 0x2f58d8, 0x2f58da, 0x2f58dc)]
    assert words == [0xC802, 0x8D01, 0xC9BF, 0xCB40]

    assert nxs_panel.contact_level(15, 0x02, active=False)
    assert not nxs_panel.contact_level(15, 0x02, active=True)
    assert nxs_panel.neutral_frame()[15] & 0x02

    # The same decoder inverts the SD OPEN contact at 042f59e4..59f2:
    # raw bit 2 clear sets status bit 1, while raw bit 2 set clears it.
    words = [struct.unpack_from("<H", data, address)[0]
             for address in (0x2f59e6, 0x2f59ec, 0x2f59ee, 0x2f59f0)]
    assert words == [0xC804, 0x8D01, 0xCB02, 0xC9FD]


@pytest.mark.skipif(os.environ.get('CDJ_TEST_TK') != '1', reason='opt-in native Tk smoke test')
def test_native_nxs_source_contact_and_feedback(tmp_path):
    args = view_ui.parse_args(['--attach', '--nxs-panel', '--output', str(tmp_path / 'screen.ppm')])
    viewer = view_ui.UiViewer(args)
    try:
        viewer.root.update_idletasks()
        viewer.send = Mock(return_value='ok')
        viewer.deck.set_scale(1)
        viewer.root.update_idletasks()
        p = faceplate.PLACEMENTS['19.1']  # USB's visual position
        viewer.deck.event_generate('<ButtonPress-1>', x=int(p.x+p.w/2), y=int(p.y+p.h/2))
        assert (19, 4) in viewer.momentary
        assert '19.1' in viewer.deck.latched
        assert '19.2' not in viewer.deck.latched
        viewer.deck.event_generate('<ButtonRelease-1>', x=-20, y=-20)
        assert not viewer.momentary
        assert '19.1' not in viewer.deck.latched
        assert [call.args[1] for call in viewer.send.call_args_list] == [
            panel_control.encode_hold(19, 4, True), panel_control.encode_hold(19, 4, False)]
        viewer.show_contact('19.0', True)  # REKORDBOX must not light legacy LINK
        assert not viewer.deck.latched
        assert set(viewer.analog_value) == set(range(7))
    finally:
        viewer.root.destroy()
