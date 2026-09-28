"""The panel control channel, checked without booting anything.

Three kinds of check, and the third is the one that earns its keep:

1. The frame model -- the checksum rule and the analogue field layout -- against
   the rule as `emulator/qemu/cdj2000_main.c` states it.
2. The wire protocol, against a real socket, so the bytes on the wire are the
   bytes claimed.
3. **The host model against the C it talks to.**  The field table, the verbs and
   the payload length are parsed straight out of `cdj2000_input.c` and compared,
   so the two halves cannot drift apart in silence.  That is the failure this
   project keeps paying for: a change on one side that leaves the other looking
   like it still works.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import re
import shlex
import socket
import threading
import time
from pathlib import Path

import pytest

from tools.cdj_main import panel_control

ROOT = Path(__file__).resolve().parents[1]
INPUT_C = ROOT / "emulator" / "qemu" / "cdj2000_input.c"
MAIN_C = ROOT / "emulator" / "qemu" / "cdj2000_main.c"
MANIFEST = ROOT / "INPUT_MANIFEST.md"


# ------------------------------------------------------------ frame model --
def test_all_zero_payload_checksums_to_zero():
    """An untouched panel reply must validate, or nothing ever handshakes."""
    assert panel_control.panel_checksum(bytes(22)) == 0


def test_checksum_folds_the_carry_inside_the_loop():
    """0xff + 0x02 is 0x101; the carry is folded at once, giving 0x02.

    A one's-complement sum taken at the end would give 0x01 here.  The
    difference is the whole point of transcribing the loop rather than the
    description of it.
    """
    assert panel_control.panel_checksum(bytes([0xFF, 0x02])) == 0x02
    assert panel_control.panel_checksum(bytes([0xFF, 0xFF])) == 0xFF


def test_boot_vm_default_frame():
    """The frame boot_vm and view_vm send for a mounted card.

    Payload byte 17 bit 2 is the SD slot switch, so the payload is 22 zeros with
    0x04 at 17 and the sum is simply 4.
    """
    spec = "00000000000000000000000000000000000400000000"
    payload = bytes.fromhex(spec)
    assert len(payload) == panel_control.PANEL_PAYLOAD_LEN
    assert payload[17] == 0x04
    assert panel_control.panel_checksum(payload) == 4


def test_frame_is_24_bytes_ending_in_the_marker():
    frame = panel_control.panel_frame(bytes([1, 2, 3]))
    assert len(frame) == panel_control.PANEL_FRAME_LEN == 24
    assert frame[-1] == 0x8F
    assert frame[-2] == panel_control.panel_checksum(frame[:22])


def test_frame_rejects_an_overlong_payload():
    with pytest.raises(ValueError):
        panel_control.panel_frame(bytes(23))


def test_sixteen_bit_analogue_fields_are_big_endian():
    """0x28e1d6 reassembles the pair high byte first.

    Verified live in the memory cdj-panel-payload-decoded: payload bytes 4..5 =
    12 34 shows up at 0x04fe2a28 as 0x1234.
    """
    payload = bytearray(22)
    panel_control.apply_analog(payload, 2, 0x1234)
    assert payload[4] == 0x12 and payload[5] == 0x34


def test_eight_bit_analogue_field_lands_on_payload_byte_two():
    payload = bytearray(22)
    panel_control.apply_analog(payload, 0, 0x5A)
    assert payload[2] == 0x5A
    assert payload[3] == 0


def test_analogue_fields_cover_bytes_two_to_fourteen_exactly():
    """2..13 are the seven levels; 14 is the encoder, and it was missing.

    0x28e1d6 does not stop at byte 13: it reads byte 14 as a level too, adds the
    halfword at 0x04fe2af8 to it and stores the sum at 0x04fe2a44.  MAIN's own
    panel simulator (0x1010a4, 66 arms) devotes arms 64 and 65 to +1 and -1 on
    that halfword and nothing else, which no other arm does.  A sweep over
    fields 0..6 could only ever have returned zeros.
    """
    covered = sorted(
        byte
        for start, width in panel_control.ANALOG_FIELDS
        for byte in range(start, start + width)
    )
    assert covered == list(range(2, 15))
    assert panel_control.ANALOG_FIELDS[7] == (14, 1)


# --------------------------------------------------------- button mapping --
def test_button_names_resolve_to_the_measured_source_bits():
    """**Reversed until 2026-08-07**, and this assertion agreed with it.

    Which is the point of `tests/test_panel_names_match_the_firmware.py`: four
    host tables and this test all said `sd` was 19.1, and none of them was ever
    compared with MAIN's own name table, which says 19.0..19.3 = LINK USB SD
    DISC.  `press sd` sent the USB key on every run this project ever made.
    """
    assert panel_control.button_mask("link") == (19, 0x01)
    assert panel_control.button_mask("usb") == (19, 0x02)
    assert panel_control.button_mask("sd") == (19, 0x04)
    assert panel_control.button_mask("disc") == (19, 0x08)


def test_button_mask_accepts_byte_dot_bit_and_byte_colon_hex():
    assert panel_control.button_mask("18.1") == (18, 0x02)
    assert panel_control.button_mask("20.4") == (20, 0x10)
    assert panel_control.button_mask("19:02") == (19, 0x02)


@pytest.mark.parametrize("bad", ["nope", "18.8", "22.0", "19:00", "19:100"])
def test_button_mask_refuses_nonsense(bad):
    with pytest.raises(ValueError):
        panel_control.button_mask(bad)


# **Empty as of 2026-08-07, and it was one line of bookkeeping for a day.**
#
# 20.3 (`MENU`) and 21.3 (`MEMORY`) are decoded at 0x28e59a and 0x28e61e and
# named by MAIN's own service-mode table, and they were deliberately held out of
# `BUTTON_BITS` because carrying them would grow `plan coverage` from 47 windows
# to 49 (+50 s) and move HEAD under the run that was in flight asking whether
# the machine survives 1 520 s.  That run finished,
# so the pair is in, the denominator is 48, and `plan coverage` is 1 570 s.
#
# The constant stays -- empty -- because it is the place where "the firmware
# decodes a bit that nothing drives" gets written down.  The next such bit goes
# here with a date and a reason, not into a comment.
DECODED_BUT_NOT_DRIVEN: set[tuple[int, int]] = set()


def test_button_bits_are_the_rows_of_the_manifest():
    """The 38 driven bits, and only those.

    It was 22 for months, and 22 was half a panel: the table came from 0x28e44a,
    where payload byte 18 begins, while the decoder starts at 0x28e1ae and reads
    bytes 15, 16 and 17 as bit sources first.  Sixteen inputs that no run could
    have driven -- which is where the missing display changes have to be, since
    r115 and r116 proved the other 29 empty.

    It was 38 for a day, and 38 was two short.  ``0x28e59a`` spreads payload
    20.3 into status 87.0 and ``0x28e61e`` spreads 21.3 into 72.6; the
    firmware's own service-mode table calls them ``MENU`` and ``MEMORY``.  Both
    are in now, and ``DECODED_BUT_NOT_DRIVEN`` above -- empty -- is where the
    next such pair gets written down instead of living in a comment.

    Bits 15.2..15.4, 17.3..17.7, 18.5, 19.5, 20.6 and 20.7 are not decoded at
    all, so they are not inputs on this board and must not appear as buttons.
    """
    rows = set()
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        first = line.strip("|").split("|")[0].strip().strip("*")
        match = re.fullmatch(r"(\d+)\.(\d)", first)
        if match:
            rows.add((int(match.group(1)), int(match.group(2))))
    assert rows, "no bit rows found in INPUT_MANIFEST.md"
    assert rows - set(panel_control.BUTTON_BITS) == DECODED_BUT_NOT_DRIVEN
    assert set(panel_control.BUTTON_BITS) - rows == set()
    assert len(panel_control.BUTTON_BITS) == 40
    for byte, bit in panel_control.BUTTON_BITS:
        assert (byte, bit) not in {(15, 2), (15, 3), (15, 4),
                                   (17, 3), (17, 4), (17, 5), (17, 6), (17, 7),
                                   (18, 5), (19, 5), (20, 6), (20, 7)}


# --------------------------------------------------------- wire encoding ---
def test_press_goes_out_as_bare_hex_the_way_the_manifest_spells_masks():
    assert panel_control.encode_press(19, 0x02, 500) == "press 19 02 500\n"
    # None is the board's default and has to be asked for by name; see the next
    # test for why that is the wrong way round from how it used to be.
    assert panel_control.encode_press(19, 0x02, None) == "press 19 02\n"


def test_a_press_carries_the_measured_hold_unless_told_otherwise():
    """The default is 2 500 ms, and leaving it off is the failure it prevents.

    `encode_press`'s hold used to default to None, i.e. "let the board decide",
    and the board decides 300 ms -- the one hold A-033 measured at 0 of 24
    presses into a status record.  Every caller that did not think about the
    hold therefore sent a press that could not arrive: 40 of the 49 windows of
    `plan coverage`, every button of the operator window, and endgame's source
    rescue.  A default that silently means "nothing happens" is not a default.
    """
    assert (panel_control.encode_press(19, 0x04)
            == "press 19 04 %d\n" % panel_control.PLAN_HOLD_MS)
    assert panel_control.PLAN_HOLD_MS >= panel_control.HOLD_DELIVERY_FLOOR_MS
    assert (panel_control.CHANNEL_HOLD_DEFAULT_MS
            < panel_control.HOLD_DELIVERY_FLOOR_MS)


def test_rotary_and_analog_encode_as_decimal():
    assert panel_control.encode_rotary(4, -8) == "rotary 4 -8\n"
    assert panel_control.encode_analog(0, 90) == "analog 0 90\n"


def test_hold_and_release_use_down_and_up():
    assert panel_control.encode_hold(21, 0x01, True) == "down 21 01\n"
    assert panel_control.encode_hold(21, 0x01, False) == "up 21 01\n"


def test_a_command_cannot_smuggle_a_second_line():
    with pytest.raises(ValueError):
        panel_control.encode("press", "19 02\nclear")


# ------------------------------------------------- the C on the other end --
def c_source() -> str:
    return INPUT_C.read_text(encoding="utf-8")


def test_analogue_field_table_matches_the_c():
    """Parse the table out of cdj2000_input.c and compare it field for field."""
    text = c_source()
    body = re.search(r"cdj_input_analog\[CDJ_INPUT_ANALOG_FIELDS\]\s*=\s*\{(.*?)\};",
                     text, re.S)
    assert body, "the analogue field table moved; the host model cannot follow"
    pairs = [(int(a), int(b))
             for a, b in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", body.group(1))]
    assert pairs == panel_control.ANALOG_FIELDS


def test_every_verb_the_host_sends_is_one_the_c_accepts():
    accepted = set(re.findall(r'strcmp\(verb,\s*"([a-z]+)"\)', c_source()))
    assert accepted, "no verbs found; the command parser moved"
    sent = {
        panel_control.encode("ping").split()[0],
        panel_control.encode("state").split()[0],
        panel_control.encode("clear").split()[0],
        panel_control.encode("step", 1).split()[0],
        panel_control.encode_press(0, 1).split()[0],
        panel_control.encode_hold(0, 1, True).split()[0],
        panel_control.encode_hold(0, 1, False).split()[0],
        panel_control.encode_analog(0, 0).split()[0],
        panel_control.encode_rotary(0, 0).split()[0],
    }
    assert sent <= accepted, f"the C does not know {sorted(sent - accepted)}"


def test_payload_length_agrees_on_both_sides():
    text = c_source()
    match = re.search(r"#define CDJ_INPUT_PAYLOAD_MAX (\d+)", text)
    assert match and int(match.group(1)) == panel_control.PANEL_PAYLOAD_LEN
    field_count = re.search(r"#define CDJ_INPUT_ANALOG_FIELDS (\d+)",
                            (ROOT / "emulator" / "qemu"
                             / "cdj2000_input.h").read_text(encoding="utf-8"))
    assert field_count
    assert int(field_count.group(1)) == len(panel_control.ANALOG_FIELDS)


def test_the_checksum_rule_in_main_c_is_still_the_one_modelled_here():
    """If A changes the frame builder, this test says so instead of the machine."""
    text = MAIN_C.read_text(encoding="utf-8")
    assert "#define PANEL_FRAME_LEN  24" in text
    assert re.search(r"sum\s*=\s*\(sum \+ 1\) & 0xff;", text)
    assert re.search(r"frame\[PANEL_FRAME_LEN - 2\]\s*=\s*sum;", text)
    assert re.search(r"frame\[PANEL_FRAME_LEN - 1\]\s*=\s*PANEL_FRAME_MARK;", text)
    assert re.search(r"#define PANEL_FRAME_MARK\s+0x8f", text, re.I)


def test_the_seam_is_still_called_before_the_checksum():
    """The one line strand A owns.  Without it nothing here reaches the panel."""
    text = MAIN_C.read_text(encoding="utf-8")
    call = text.index("cdj_input_apply(frame, PANEL_FRAME_LEN - 2);")
    checksum = text.index("frame[PANEL_FRAME_LEN - 2] = sum;")
    keys = text.index("frame[key->byte] |= key->mask;")
    assert keys < call < checksum


def test_the_channel_is_silent_unless_cdj_input_port_is_set():
    """A control run has to stay a control run.

    Nothing may bind, and nothing may be merged, when the variable is unset --
    otherwise every comparison against a control run is comparing two different
    machines.
    """
    text = c_source()
    assert 'getenv("CDJ_INPUT_PORT")' in text
    open_body = text[text.index("static void cdj_input_open(void)"):]
    open_body = open_body[:open_body.index("\n}\n")]
    assert re.search(r"if \(!spec \|\| !\*spec\) \{\s*return;", open_body)
    apply_body = text[text.index("void cdj_input_apply("):]
    assert re.search(r"if \(cdj_input_listen_fd < 0 \|\| !len\) \{\s*return;",
                     apply_body)


# ------------------------------------------------------------- the socket --
class Recorder:
    """A stand-in server that speaks the same line protocol."""

    def __init__(self, greeting: bytes = b"ok cdj2000-input\n") -> None:
        self.lines: list[str] = []
        self.greeting = greeting
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self) -> None:
        connection, _ = self.listener.accept()
        with connection:
            connection.sendall(self.greeting)
            pending = b""
            while True:
                try:
                    chunk = connection.recv(4096)
                except OSError:
                    return
                if not chunk:
                    return
                pending += chunk
                while b"\n" in pending:
                    line, _, pending = pending.partition(b"\n")
                    self.lines.append(line.decode())
                    connection.sendall(b"ok %s\n" % line.split(b" ")[0])

    def close(self) -> None:
        self.listener.close()


@pytest.fixture
def recorder():
    server = Recorder()
    yield server
    server.close()


def test_the_client_reads_the_greeting_and_then_one_reply_per_command(recorder):
    with panel_control.PanelControl(port=recorder.port, timeout=5.0) as panel:
        assert panel.press("sd") == "ok press"
        assert panel.rotary(4, -3) == "ok rotary"
        assert panel.analog(0, 0x5A) == "ok analog"
        assert panel.hold("18.1", down=True) == "ok down"
        assert panel.hold("18.1", down=False) == "ok up"
    assert recorder.lines == [
        "press 19 04 %d" % panel_control.PLAN_HOLD_MS,
        "rotary 4 -3",
        "analog 0 90",
        "down 18 02",
        "up 18 02",
    ]


def test_open_returns_the_greeting(recorder):
    panel = panel_control.PanelControl(port=recorder.port, timeout=5.0)
    try:
        assert panel.open() == "ok cdj2000-input"
    finally:
        panel.close()


def test_a_schedule_is_parsed_and_sorted():
    schedule = panel_control.parse_schedule(["60:rotary 4 12", "20:press sd"])
    assert schedule == [(20.0, "press sd"), (60.0, "rotary 4 12")]


@pytest.mark.parametrize("bad", ["press sd", "", "20", ":press sd", "20:"])
def test_a_schedule_entry_without_a_time_is_refused(bad):
    with pytest.raises(ValueError):
        panel_control.parse_schedule([bad])


def test_schedule_commands_resolve_to_protocol_lines():
    assert panel_control.resolve("press sd") == "press 19 04\n"
    assert panel_control.resolve("press 18.1 800") == "press 18 02 800\n"
    assert panel_control.resolve("down 21.0") == "down 21 01\n"
    assert panel_control.resolve("rotary 4 -12") == "rotary 4 -12\n"
    assert panel_control.resolve("clear") == "clear\n"


def test_a_session_sends_everything_in_order_and_records_when(recorder):
    """Names are resolved on the host, so the board never learns one."""
    schedule = panel_control.parse_schedule(["0:press sd", "0:rotary 4 3"])
    with panel_control.PanelControl(port=recorder.port, timeout=5.0) as panel:
        log = panel_control.run_session(panel, schedule, report=lambda *_: None)
    assert [command for _, command, _ in log] == ["press sd", "rotary 4 3"]
    assert recorder.lines == ["press 19 04", "rotary 4 3"]


class HangsUp(Recorder):
    """A server that closes the connection after the first command.

    That is what the guest did in r089: two commands accepted, then the channel
    gone, and the command at t=150 never sent.  A session that gives up there
    loses every measurement after it.
    """

    def serve(self) -> None:
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            with connection:
                connection.sendall(self.greeting)
                pending = b""
                served = 0
                while served < 1:
                    try:
                        chunk = connection.recv(4096)
                    except OSError:
                        return
                    if not chunk:
                        return
                    pending += chunk
                    while b"\n" in pending:
                        line, _, pending = pending.partition(b"\n")
                        self.lines.append(line.decode())
                        connection.sendall(b"ok %s\n" % line.split(b" ")[0])
                        served += 1
            # ... and the next connection is accepted, as the board now does.


def test_a_session_survives_the_channel_going_away():
    """Reconnect and send again, rather than losing the rest of the schedule."""
    server = HangsUp()
    try:
        schedule = panel_control.parse_schedule(
            ["0:analog 0 90", "0:press sd", "0:rotary 4 12"])
        panel = panel_control.PanelControl(port=server.port, timeout=5.0)
        panel.open()
        try:
            log = panel_control.run_session(panel, schedule,
                                            report=lambda *_: None)
        finally:
            panel.close()
    finally:
        server.close()
    assert [command for _, command, _ in log] == [
        "analog 0 90", "press sd", "rotary 4 12"]
    assert server.lines == ["analog 0 90", "press 19 04", "rotary 4 12"]


def test_a_session_refuses_an_unknown_button_before_touching_the_machine():
    """A typo must not cost a run slot half-way through."""
    argv = ["--port", "1", "session", "10:press nosuchkey"]
    assert panel_control.main(argv) == 1


def test_the_transcript_records_absolute_epochs(tmp_path, recorder):
    """Without an absolute clock the session's times cannot be compared with
    the frame sampler's, and r091 showed those two are 45 s apart."""
    path = tmp_path / "session.txt"
    schedule = panel_control.parse_schedule(["0:press sd", "0:rotary 4 3"])
    before = time.time()
    with panel_control.PanelControl(port=recorder.port, timeout=5.0) as panel:
        panel_control.run_session(panel, schedule, report=lambda *_: None,
                                  transcript=str(path))
    after = time.time()

    # The transcript keeps microseconds, so allow a microsecond of rounding at
    # the ends rather than pretending the comparison is exact.
    tolerance = 1e-5
    connect_epoch, entries = panel_control.read_transcript(str(path))
    assert before - tolerance <= connect_epoch <= after + tolerance
    assert [command for _, _, command, _ in entries] == [
        "press sd", "rotary 4 3"]
    for epoch, elapsed, _, reply in entries:
        assert before - tolerance <= epoch <= after + tolerance
        assert 0 <= elapsed < 30
        assert reply.startswith("ok")


def test_a_transcript_is_written_even_when_the_session_is_cut_short(tmp_path):
    """A partial transcript still says which windows were driven, and a window
    that was never driven is the one thing an evaluation must not score."""
    path = tmp_path / "session.txt"
    server = HangsUp()

    class Fails(panel_control.PanelControl):
        def send_resilient(self, line, deadline=20.0, report=None):
            if "rotary" in line:
                raise OSError("gone")
            return self.send(line)

    panel = Fails(port=server.port, timeout=5.0)
    panel.open()
    try:
        with pytest.raises(OSError):
            panel_control.run_session(
                panel, panel_control.parse_schedule(
                    ["0:press sd", "0:rotary 4 3"]),
                report=lambda *_: None, transcript=str(path))
    finally:
        panel.close()
        server.close()
    _, entries = panel_control.read_transcript(str(path))
    assert [command for _, _, command, _ in entries] == ["press sd"]


def test_a_file_without_a_connect_epoch_is_not_a_transcript(tmp_path):
    path = tmp_path / "nope.txt"
    path.write_text("# panel_control session transcript v1\n", encoding="utf-8")
    with pytest.raises(ValueError):
        panel_control.read_transcript(str(path))


# ---------------------------------------------------- the canonical plan ---
def test_the_keys_plan_drives_every_decoded_bit_exactly_once():
    """Full coverage wants *all* the keys.  One missing row fails it."""
    entries = panel_control.plan_entries("keys")
    driven = {panel_control.button_mask(window)
              for _, window in panel_control.plan_windows(entries)}
    assert driven == {(byte, 1 << bit) for byte, bit in panel_control.BUTTON_BITS}
    assert len(panel_control.plan_windows(entries)) == 40


def test_the_sweep_drives_every_analogue_field_exactly_once():
    windows = panel_control.plan_windows(
        panel_control.plan_entries("rotary-sweep"))
    assert len(windows) == len(panel_control.ANALOG_FIELDS) == 8
    assert [window for _, window in windows] == [
        "field%d" % index for index in range(8)]


def test_the_two_plans_together_cover_everything_goal_3_lists():
    """40 bits and 8 analogue fields, the last of which is the encoder."""
    covered_bits = {panel_control.button_mask(window) for _, window
                    in panel_control.plan_windows(
                        panel_control.plan_entries("keys"))}
    covered_fields = {int(window[5:]) for _, window
                      in panel_control.plan_windows(
                          panel_control.plan_entries("rotary-sweep"))}
    assert len(covered_bits) == 40
    assert covered_fields == set(range(len(panel_control.ANALOG_FIELDS)))


def test_the_coverage_plan_is_the_union_of_the_other_three():
    """The whole board in one run, because provenance is per run.

    `keys` drives 40 of the 48 inputs INPUT_MANIFEST.md enumerates, and nothing
    in the plan output used to say so: the budget table called `plan keys`
    "38 bits plus the encoder", and it has no rotary in it.
    Covering the board from three runs is possible, but then the manifest table
    carries three provenances, and the rule is that a coverage run
    counts only on the same HEAD and the same binary as the others.
    """
    windows = [window for _, window in panel_control.plan_windows(
        panel_control.plan_entries("coverage"))]
    keys = [window for _, window in panel_control.plan_windows(
        panel_control.plan_entries("keys"))]
    fields = [window for _, window in panel_control.plan_windows(
        panel_control.plan_entries("rotary-sweep"))]
    assert windows == keys + fields + ["rotary-left"]
    assert len(windows) == 40 + 8 + 1 == 49
    assert len(set(windows)) == len(windows)


def test_the_coverage_plan_costs_less_than_running_its_parts():
    """Splitting pays the t300 wait and the connect allowance three times."""
    whole = panel_control.plan_seconds(panel_control.plan_entries("coverage"))
    parts = sum(panel_control.plan_seconds(panel_control.plan_entries(name,
                                                                     field=7))
                for name in ("keys", "rotary-sweep", "rotary"))
    assert whole == 1570
    assert parts == 1345 + 545 + 395 == 2285
    assert whole < parts


def test_the_coverage_plan_walks_the_encoder_both_ways():
    entries = panel_control.plan_entries("coverage")
    commands = [command for _, command, window in entries if window]
    assert commands[-2:] == ["rotary %d 12" % panel_control.ENCODER_FIELD,
                             "rotary %d -24" % panel_control.ENCODER_FIELD]


def test_an_incomplete_plan_says_that_it_is_incomplete():
    """A plan that covers part of the board must not read like the whole one."""
    assert any("INCOMPLETE" in line for line in panel_control.plan_coverage("keys"))
    assert any("INCOMPLETE" in line
               for line in panel_control.plan_coverage("rotary-sweep"))
    assert not any("INCOMPLETE" in line
                   for line in panel_control.plan_coverage("coverage"))


def test_the_coverage_plan_emits_both_lines_and_they_agree():
    entries = panel_control.plan_entries("coverage")
    session = shlex.split(panel_control.session_command(entries, 5984))
    evaluation = shlex.split(panel_control.frame_delta_command(
        entries, "frames", None, "look"))
    assert session[session.index("--transcript") + 1] \
        == evaluation[evaluation.index("--align") + 1]
    driven = {item.split(":", 1)[0] for item in session if ":" in item
              and not item.startswith("-")}
    scored = {item.split(":", 1)[0] for item in evaluation if ":" in item
              and not item.startswith("-")}
    # Every scored window is driven; the probe is driven and not scored.
    assert scored < driven
    assert len(driven) - len(scored) == 1


def test_the_rotary_plan_goes_both_ways_and_back_through_the_start():
    entries = panel_control.plan_entries("rotary", field=4)
    windows = panel_control.plan_windows(entries)
    commands = [command for _, command, window in entries if window]
    assert commands == ["rotary 4 12", "rotary 4 -24"]
    assert [window for _, window in windows] == ["rotary-right", "rotary-left"]


def test_the_rotary_plan_will_not_guess_the_field():
    with pytest.raises(ValueError):
        panel_control.plan_entries("rotary")


def test_windows_are_never_closer_than_the_measured_spacing():
    """r024/r026 used 5-15 s and left four rows unattributable."""
    for name in ("keys", "rotary-sweep"):
        entries = panel_control.plan_entries(name)
        gaps = [b[0] - a[0] for a, b in zip(entries, entries[1:])]
        assert gaps and min(gaps) >= panel_control.PLAN_SPACING >= 25.0


def test_nothing_is_driven_before_the_browse_phase():
    """The GUI reaches it around 115 s; earlier is a startup screen.

    The probe is exempt and deliberately so: it moves no payload byte, so it
    cannot be measured against a startup screen or anything else.
    """
    windows = panel_control.plan_windows(panel_control.plan_entries("keys"))
    assert windows[0][0] >= 150.0


def test_the_connect_allowance_comes_from_a_run_and_says_which():
    """45 s, measured in r091.  It was 25 s by estimate, and that was wrong in
    the direction that costs a run: at 25 s spacing a 20-second error slides
    every window most of a position."""
    assert panel_control.PLAN_CONNECT_ALLOWANCE == 45.0
    assert panel_control.PLAN_CONNECT_ALLOWANCE_SOURCE == "r091"


def test_the_plan_states_the_run_it_needs():
    """The numbers written down: 1345 s for the keys, 545 for the sweep.

    Asserted concretely *and* derived, so that changing a constant cannot move
    the requirement without this test naming the new figure.  It moved once:
    It has moved three times, each time for a measured reason: r113's churn ran
    to t194.8 (t150 -> t210), no press before t150 has ever reached the key
    dispatcher in four traced runs (t210 -> t300), and the bit inventory went
    from 22 to 38 when the decoder turned out to start three bytes earlier than
    the table did.  Twenty-two minutes is the price of driving every input the
    board has, and --parts is there for when that is not payable in one go.
    """
    keys = panel_control.plan_entries("keys")
    assert panel_control.plan_seconds(keys) == 1345
    assert panel_control.plan_seconds(keys) == (
        keys[-1][0] + panel_control.PLAN_TAIL
        + panel_control.PLAN_CONNECT_ALLOWANCE)
    assert panel_control.plan_seconds(
        panel_control.plan_entries("rotary-sweep")) == 545


def test_a_plan_that_would_not_fit_refuses_to_be_generated():
    """r088's death: a schedule accepted that could not fit, and nothing said so."""
    entries = panel_control.plan_entries("keys")
    with pytest.raises(panel_control.PlanTooLong):
        panel_control.check_plan(entries, 340)
    # And the old 1205, which was enough only while the plan started at t210.
    with pytest.raises(panel_control.PlanTooLong):
        panel_control.check_plan(entries, 1205)
    panel_control.check_plan(entries, 1345)


def test_the_cli_refuses_it_too_rather_than_printing_something_unrunnable():
    assert panel_control.main(["plan", "keys", "--seconds", "340"]) == 1
    assert panel_control.main(["plan", "keys", "--seconds", "1345"]) == 0


def test_splitting_is_offered_instead_of_shrinking_the_spacing():
    entries = panel_control.plan_entries("keys")
    halves = panel_control.split_plan(entries, 2)
    assert [len(panel_control.plan_windows(half)) for half in halves] == [20, 20]
    for half in halves:
        windows = panel_control.plan_windows(half)
        gaps = [b[0] - a[0] for a, b in zip(windows, windows[1:])]
        assert min(gaps) >= panel_control.PLAN_SPACING
        assert panel_control.plan_seconds(half) == 845
        panel_control.check_plan(half, 845)
        # Each part is its own run and needs its own probe.
        assert [command for _, command, window in half if window is None] == [
            panel_control.PLAN_PROBE]
    # Two halves cost more wall clock than one run, which is why splitting is
    # the fallback and not the default.
    assert 2 * 845 > panel_control.plan_seconds(entries)
    # No input is lost by splitting.
    assert ({window for _, window in panel_control.plan_windows(
                halves[0] + halves[1])}
            == {window for _, window in panel_control.plan_windows(entries)})


def schedule_words(command: str) -> list[str]:
    """The SECONDS:… arguments of an emitted command, as a shell would see them.

    shlex, not str.split: the point is that the printed line is one a human can
    paste, and the quoting around `150:"press 18.0"` is part of that.
    """
    return [word for word in shlex.split(command)
            if ":" in word and word[0].isdigit()]


def test_the_emitted_session_is_one_this_file_can_actually_run():
    """The plan is the truth: what it prints has to parse back through the
    same code that sends it, or the two drift apart the first time either
    changes."""
    entries = panel_control.plan_entries("keys")
    command = panel_control.session_command(entries, 5984)
    schedule = panel_control.parse_schedule(schedule_words(command))
    assert len(schedule) == 41, "40 inputs plus the early probe"
    for (when, text), (plan_when, plan_text, _) in zip(schedule, entries):
        assert when == plan_when
        assert text == plan_text
        panel_control.resolve(text)          # must not raise


def option_of(command: str, flag: str) -> str | None:
    """The value of `flag` in an emitted command, as a shell would see it."""
    words = shlex.split(command)
    return words[words.index(flag) + 1] if flag in words else None


def test_the_emitted_evaluation_lines_up_with_the_session():
    """Same times, same names, one command each -- or an evaluation would be
    reading windows the run never drove."""
    from tools.cdj_main import frame_delta

    entries = panel_control.plan_entries("keys")
    session = panel_control.parse_schedule(
        schedule_words(panel_control.session_command(entries, 5984)))
    windows = frame_delta.parse_windows(
        schedule_words(panel_control.frame_delta_command(entries, "f", "m", "l")))
    assert windows == panel_control.plan_windows(entries)
    # The probe is driven and not scored, so the session has exactly one entry
    # the evaluation does not -- and every window must still have been driven.
    assert len(session) == len(windows) + 1
    assert set(when for when, _ in windows) <= set(when for when, _ in session)


def test_the_printed_pair_carries_its_own_alignment():
    """The lines are meant to be pasted verbatim, so the anchor has to be in them.

    A session without --transcript leaves nothing for --align to measure the
    45-second clock offset from, and by the time the unaligned evaluation says
    so the run is over and the anchor is gone -- only --shift by hand is left.
    Both flags therefore come off the same string, and this checks that they do
    rather than that somebody remembered.
    """
    entries = panel_control.plan_entries("keys")
    session = panel_control.session_command(entries, 5984)
    evaluation = panel_control.frame_delta_command(entries, "f", "m", "l")

    written = option_of(session, "--transcript")
    read = option_of(evaluation, "--align")
    assert written, "the printed session does not record a transcript"
    assert read, "the printed evaluation does not align against one"
    assert written == read


def test_the_pair_still_matches_when_the_path_is_chosen(capsys):
    """Including through the CLI, where the two lines are printed separately."""
    assert panel_control.main(
        ["plan", "rotary-sweep", "--transcript", "runs/r099/s.txt"]) == 0
    printed = [line for line in capsys.readouterr().out.splitlines()
               if line.startswith("python -m")]
    session = next(line for line in printed if " session " in line)
    evaluation = next(line for line in printed if "frame_delta windows" in line)
    assert (option_of(session, "--transcript")
            == option_of(evaluation, "--align") == "runs/r099/s.txt")


def test_the_printed_mask_is_not_the_one_from_another_world(capsys):
    """r093 pasted this line verbatim and lost its answer to the wrong mask.

    `runs/anim-mask.bin` is r048's, from a machine with an empty browse
    pane and the spinning "Wait" platter.  All four of r093's measurable
    windows landed on that platter, which the mask does not cover, so the run
    said nothing.  The printed line is meant to be copied, which makes a stale
    default a way to lose a twelve-minute run rather than a matter of taste.
    """
    assert panel_control.main(["plan", "keys"]) == 0
    output = capsys.readouterr().out
    evaluation = next(line for line in output.splitlines()
                      if "frame_delta windows" in line)
    assert option_of(evaluation, "--mask") == panel_control.PLAN_MASK
    assert option_of(evaluation, "--mask") != "runs/anim-mask.bin"
    # No mask at all now, and the header says why -- r113 showed that the other
    # direction fails too: no mask, on a screen that was moving anyway.
    assert panel_control.PLAN_MASK is None
    assert "--mask" not in evaluation
    assert "self-baseline" in output


def test_the_settle_stays_below_the_measured_answer_time(capsys):
    """r096 measured 7.8 s; a settle at or above that destroys the row.

    The sampler writes nothing while the screen stands, so if the display
    answers before the settle expires the answering frame is the only one there
    is -- and skipping it takes the next input's repaint.  The two errors are
    not symmetric, so the margin goes downwards.
    """
    assert panel_control.PLAN_SETTLE < panel_control.PLAN_RESPONSE
    assert panel_control.PLAN_SETTLE <= panel_control.PLAN_RESPONSE / 2
    assert panel_control.main(["plan", "keys"]) == 0
    output = capsys.readouterr().out
    evaluation = next(line for line in output.splitlines()
                      if "frame_delta windows" in line)
    assert (float(option_of(evaluation, "--settle"))
            == panel_control.PLAN_SETTLE)


def test_the_answer_time_carries_its_run_id():
    """A constant without a run id is a guess.  Same rule as r091's allowance."""
    assert panel_control.PLAN_RESPONSE_SOURCE.startswith("r134")
    assert panel_control.PLAN_START_SOURCE.startswith("r131-r134")
    assert panel_control.PLAN_CONNECT_ALLOWANCE_SOURCE == "r091"


def test_the_attribution_limit_matches_the_tool_that_enforces_it():
    """panel_control repeats the number so that driving needs no numpy."""
    from tools.cdj_main import frame_delta

    assert (panel_control.PLAN_ATTRIBUTION_LIMIT
            == frame_delta.MAX_WINDOW_SECONDS)


def test_the_answer_time_does_not_quietly_lengthen_the_run(capsys):
    """The cost, computed rather than assumed: the run length does not move.

    The binding constraint is attribution, not spacing -- an answer has to land
    inside 10 s of its press, and 25 s of spacing already keeps the next press
    15 s clear of that window.  A slower answer is not something a wider gap
    can buy off.  What *did* lengthen the run is where it starts, which is a
    different reason and is stated as one.
    """
    entries = panel_control.plan_entries("keys")
    assert panel_control.plan_seconds(entries) == 1345.0
    assert panel_control.PLAN_SPACING > panel_control.PLAN_ATTRIBUTION_LIMIT

    assert panel_control.main(["plan", "keys"]) == 0
    output = capsys.readouterr().out
    assert "The binding limit on the *spacing* is attribution" in output
    assert "--seconds 1345" in output
    # And the thin headroom is stated rather than left to be discovered.
    assert "9.2 s of headroom" not in output   # plenty of headroom now


def test_a_split_plan_gives_each_run_its_own_transcript(capsys):
    """A shared path would let run two overwrite run one's anchor, and the
    first evaluation would then align against the wrong clock."""
    assert panel_control.main(["plan", "keys", "--parts", "2"]) == 0
    printed = [line for line in capsys.readouterr().out.splitlines()
               if line.startswith("python -m")]
    sessions = [line for line in printed if " session " in line]
    evaluations = [line for line in printed if "frame_delta windows" in line]
    assert len(sessions) == len(evaluations) == 2
    paths = [option_of(line, "--transcript") for line in sessions]
    assert paths == [option_of(line, "--align") for line in evaluations]
    assert len(set(paths)) == 2, f"both runs would write {paths}"
    for path in paths:
        assert path.endswith(".txt")


def test_the_transcript_suffix_survives_a_path_without_an_extension():
    assert (panel_control.plan_transcript("runs/r0NN/session.txt", "keys-1")
            == "runs/r0NN/session-keys-1.txt")
    assert (panel_control.plan_transcript("runs/r0NN/session", "keys-1")
            == "runs/r0NN/session-keys-1")
    # A dot in a directory name must not be mistaken for an extension.
    assert (panel_control.plan_transcript("ev.dir/session", "keys-1")
            == "ev.dir/session-keys-1")


def test_a_closed_channel_raises_rather_than_silently_dropping_a_press():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def hang_up() -> None:
        connection, _ = listener.accept()
        connection.close()

    threading.Thread(target=hang_up, daemon=True).start()
    panel = panel_control.PanelControl(port=port, timeout=5.0)
    try:
        with pytest.raises(ConnectionError):
            panel.open()
    finally:
        panel.close()
        listener.close()


def test_state_lamps_and_press_ids_parse():
    reply = ("ok state frames=12 commands=3 queue=0 phase=idle held=00 "
             "level_mask=00 level_value=00 a0=-0/0 sd_lid=raw "
             "lamps=CUE:on,PLAY_PAUSE:blink,SOURCE_SD:3 last_press=4:100-103")
    state = panel_control.parse_state(reply)
    assert state["last_press"] == "4:100-103"
    assert panel_control.lamps_of(state) == {
        "CUE": "on", "PLAY_PAUSE": "blink", "SOURCE_SD": "3"}
    assert panel_control.lamps_of({"lamps": "-"}) == {}
    assert panel_control.press_id_of("ok press id=17") == 17
    assert panel_control.press_id_of("ok press") is None


def test_jog_holds_the_enable_bit_and_walks_the_ring_counter():
    # The proven path (beat/skips-jog): byte 15 bit 7 held while analogue
    # field 4 moves, 72 counts per step.
    assert panel_control.button_mask(panel_control.JOG_ENABLE) == (15, 0x80)
    assert panel_control.JOG_FIELD == 4
    assert panel_control.ANALOG_FIELDS[panel_control.JOG_FIELD] == (8, 2)

    sent: list[str] = []

    class Fake(panel_control.PanelControl):
        def send(self, line: str) -> str:
            sent.append(line.strip())
            return "ok"

        def state(self) -> str:
            return "ok state frames=1 a4=360/360"

    Fake().jog(5)
    assert sent == ["down 15 80", "rotary 4 360", "up 15 80"]


def test_jog_backwards_reaches_a_negative_target(monkeypatch):
    # A driven field reports "a4=-360/-360"; the sign is the value's own.
    monkeypatch.setattr(panel_control.time, "sleep", lambda _: None)
    sent: list[str] = []

    class Fake(panel_control.PanelControl):
        def send(self, line: str) -> str:
            sent.append(line.strip())
            return "ok"

        def state(self) -> str:
            return "ok state frames=1 a4=-360/-360"

    Fake().jog(-5, timeout=1.0)
    assert sent == ["down 15 80", "rotary 4 -360", "up 15 80"]


def _bend(state: str, reverse: bool) -> list[str]:
    sent: list[str] = []

    class Fake(panel_control.PanelControl):
        def send(self, line: str) -> str:
            sent.append(line.strip())
            return "ok"

        def state(self) -> str:
            return state

    Fake().bend(0.05, reverse=reverse)
    return sent


def test_bend_sets_the_period_spins_and_rests():
    sent = _bend("ok state frames=1 a4=100/100", reverse=True)
    assert sent[:2] == ["down 15 80", "analog 5 %d" % panel_control.JOG_BEND_PERIOD]
    assert sent[2] == "analog 4 %d" % (100 - panel_control.JOG_BEND_STEP)
    assert sent[-2:] == ["analog 5 0", "up 15 80"]


def test_bend_starts_from_a_negative_count():
    # After jog(-N) the ring counter is negative; the bend continues from it.
    sent = _bend("ok state frames=1 a4=-360/-360", reverse=False)
    assert sent[2] == "analog 4 %d" % ((-360 + panel_control.JOG_BEND_STEP) & 0xFFFF)
