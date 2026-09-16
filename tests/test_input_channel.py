"""What the runtime control channel actually does to the panel payload.

`emulator/qemu/cdj2000_input.c` is compiled here against the stub headers in
`tests/cstub/`, given a real loopback socket and a real client, and stepped one
panel exchange at a time with a virtual clock the harness controls.  So these
are measurements of the shipped code, not of a Python restatement of it -- and
they cost no run slot, which is the scarce thing in this project.

What they cannot show is that MAIN reacts.  That needs the machine.  What they
do show is that the frame carries what it
is meant to carry, so that when the run happens, a negative result means the
firmware and not the plumbing.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

import pytest

from tools.cdj_gui import view_ui
from tools.cdj_main import panel_control

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests" / "cstub" / "harness.c"
PAYLOAD_LEN = 22
# FRAME_NS in tests/cstub/harness.c: the fake virtual clock advances this much
# per panel exchange, so a hold in milliseconds is a count of exchanges here.
HARNESS_EXCHANGE_MS = 100

# The default MSYS2 location, which is where scripts/build-qemu-sh4.sh expects
# the toolchain too.  A second gcc earlier on PATH (C:\MinGW here) links against
# a different C runtime and fails with undefined _gnu_exception_handler, so the
# compiler is taken from beside pkg-config rather than from PATH order.
MINGW64 = Path(r"C:\msys64\mingw64\bin")


def toolchain() -> tuple[Path, Path] | None:
    """(gcc, bin directory) of a toolchain whose gcc and pkg-config match."""
    for candidate in (MINGW64, ):
        if (candidate / "gcc.exe").exists() and (candidate / "pkg-config.exe").exists():
            return candidate / "gcc.exe", candidate
    found = shutil.which("pkg-config")
    if found:
        here = Path(found).parent
        gcc = here / ("gcc.exe" if sys.platform == "win32" else "gcc")
        if gcc.exists():
            return gcc, here
        # macOS uses Apple's compiler with Homebrew's pkg-config/GLib.
        # Only Windows needs the strict same-directory runtime pairing.
        if sys.platform != "win32" and (compiler := shutil.which("cc")):
            return Path(compiler), here
    return None


pytestmark = pytest.mark.skipif(
    toolchain() is None,
    reason="needs a toolchain with matching gcc and pkg-config (MSYS2 mingw64)",
)


def environment() -> dict[str, str]:
    """PATH with the toolchain first.

    Not a nicety: mingw64's gcc.exe needs its own libgcc/gmp/isl DLLs from that
    directory, and without them it exits 1 having printed nothing at all -- a
    failure that reads exactly like a broken source file.  The harness needs
    glib from the same place.
    """
    _, bindir = toolchain()
    settings = dict(os.environ)
    settings["PATH"] = str(bindir) + os.pathsep + settings.get("PATH", "")
    settings.pop("CDJ_INPUT_PORT", None)
    settings.pop("CDJ_NXS_SD_LID", None)
    settings.pop("CDJ_TEST_PANEL_BASE", None)
    return settings


@pytest.fixture(scope="session")
def harness(tmp_path_factory) -> Path:
    """Build the harness once for the whole session."""
    gcc, bindir = toolchain()
    settings = environment()
    target = tmp_path_factory.mktemp("cstub") / "cdj-input-harness.exe"
    flags = subprocess.run([str(bindir / "pkg-config"), "--cflags", "--libs",
                            "glib-2.0"],
                           capture_output=True, text=True, check=True,
                           env=settings)
    command = [
        str(gcc), "-std=gnu11", "-O1", "-Wall", "-Werror",
        "-o", str(target), str(HARNESS),
        "-I", str(ROOT / "tests" / "cstub"),
        "-I", str(ROOT / "emulator" / "qemu"),
        *flags.stdout.split(),
    ]
    if sys.platform == "win32":
        command.append("-lws2_32")
    built = subprocess.run(command, capture_output=True, text=True,
                           env=settings)
    if built.returncode:
        pytest.fail("the harness does not build (exit %d):\n%s%s"
                    % (built.returncode, built.stdout, built.stderr))
    return target


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run(harness: Path, scenario: str, port: int = -1,
        extra: list[str] | None = None) -> list[bytes]:
    """Run one scenario and return the payload of every panel exchange."""
    settings = environment()
    if port != 0:
        settings["CDJ_INPUT_PORT"] = str(free_port() if port == -1 else port)
    finished = subprocess.run([str(harness), scenario, *(extra or [])],
                              capture_output=True,
                              text=True, timeout=60, env=settings)
    assert finished.returncode == 0, finished.stderr
    frames = []
    for line in finished.stdout.splitlines():
        if not line.startswith("f "):
            continue
        _, _, _, payload = line.split(" ", 3)
        assert len(payload) == 2 * PAYLOAD_LEN
        frames.append(bytes.fromhex(payload))
    assert frames, "the harness produced no frames"
    return frames


class Segment(NamedTuple):
    command: str
    replies: list[str]
    frames: list[bytes]


def script(harness: Path, steps: list[tuple[str, int]],
           where: Path | None = None, *,
           defer_replies: bool = False,
           base_payload: bytes | None = None) -> list[Segment]:
    """Drive a list of (command, exchanges-afterwards) and cut the trace up.

    The harness echoes `# command` before sending and `# reply` for everything
    the board answers, so each command's frames and its answer can be read
    together.  That is what makes it possible to check a line this file did not
    write -- the operator window's, for instance.
    """
    path = (where or Path(tempfile.mkdtemp())) / "script.txt"
    with path.open("w", encoding="ascii") as handle:
        for command, count in steps:
            handle.write(command + "\n")
            handle.write("frames %d\n" % count)

    settings = environment()
    settings["CDJ_INPUT_PORT"] = str(free_port())
    if base_payload is not None:
        assert len(base_payload) == PAYLOAD_LEN
        settings['CDJ_TEST_PANEL_BASE'] = base_payload.hex()
    scenario = 'script-deferred-replies' if defer_replies else 'script'
    finished = subprocess.run([str(harness), scenario, str(path)],
                              capture_output=True, text=True, timeout=300,
                              env=settings)
    assert finished.returncode == 0, finished.stderr

    segments: list[Segment] = []
    current: Segment | None = None
    for line in finished.stdout.splitlines():
        if line.startswith("# command "):
            current = Segment(line[len("# command "):], [], [])
            segments.append(current)
        elif line.startswith("# reply ") and current is not None:
            current.replies.append(line[len("# reply "):])
        elif line.startswith("f ") and current is not None:
            current.frames.append(bytes.fromhex(line.split(" ", 3)[3]))
    assert [segment.command for segment in segments] == \
        [command for command, _ in steps], "the harness lost a command"
    return segments


def test_literal_levels_override_base_holds_pulses_and_analog(harness, tmp_path):
    base = bytearray(PAYLOAD_LEN)
    base[15] = 0x82  # Neutral REV plus an unrelated static contact.
    steps = [('level 15 02 0', 3), ('down 15 06', 3),
             ('press 15 02 1', 8), ('level 15 06 1', 3),
             ('up 15 06', 3), ('level 15 04 0', 3),
             ('analog 0 255', 3), ('level 2 81 0', 3),
             ('state', 2), ('clear', 3), ('state', 2)]
    segments = script(harness, steps, tmp_path, base_payload=bytes(base))
    for segment, expected in zip(segments[:6], [0x80, 0x84, 0x84,
                                               0x86, 0x86, 0x82]):
        assert all(frame[15] == expected for frame in segment.frames)
    assert all(frame[2] == 0xff for frame in segments[6].frames)
    assert all(frame[2] == 0x7e for frame in segments[7].frames)
    state = next(r for r in segments[8].replies if r.startswith('ok state '))
    fields = dict(token.split('=', 1) for token in state.split()[2:])
    mask = bytes.fromhex(fields['level_mask'])
    value = bytes.fromhex(fields['level_value'])
    assert len(mask) == len(value) == PAYLOAD_LEN
    assert mask[15] == 6 and value[15] == 2
    assert mask[2] == 0x81 and value[2] == 0
    assert all(frame == bytes(base) for frame in segments[9].frames)
    assert f'level_mask={"00" * PAYLOAD_LEN}' in segments[10].replies[0]
    assert f'level_value={"00" * PAYLOAD_LEN}' in segments[10].replies[0]


def test_literal_level_validation_and_sd_lid_priority(harness, tmp_path):
    invalid = ['level', 'level 15 02', 'level -1 02 0', 'level 22 02 0',
               'level bad 02 0', 'level 15 00 0', 'level 15 100 0',
               'level 15 gg 0', 'level 15 02 -1', 'level 15 02 2',
               'level 15 02 yes', 'level 15 02 1 extra']
    steps = [('level 15 02 1', 2), *[(command, 2) for command in invalid],
             ('sd-lid closed', 2), ('level 17 04 0', 2),
             ('sd-lid open', 2), ('level 17 04 1', 2)]
    segments = script(harness, steps, tmp_path)
    for segment in segments[1:1 + len(invalid)]:
        assert any(reply.startswith('err level ') for reply in segment.replies)
        assert all(frame[15] == 2 for frame in segment.frames)
    for segment, closed in zip(segments[-4:], [True, True, False, False]):
        assert all(bool(frame[17] & 4) == closed for frame in segment.frames)


def test_sd_lid_is_persistent_and_overrides_raw_button_commands(harness, tmp_path):
    steps = [('sd-lid closed', 3), ('clear', 3), ('up 17 04', 3),
             ('sd-lid open', 3), ('down 17 04', 3), ('clear', 3),
             ('sd-lid toggle', 3), ('sd-lid invalid', 3),
             ('sd-lid open extra', 3), ('sd-lid state', 3)]
    segments = script(harness, steps, tmp_path)
    for segment, closed in zip(segments, [True, True, True, False, False,
                                          False, True, True, True, True]):
        assert all(bool(frame[17] & 4) == closed for frame in segment.frames)
    assert 'ok sd-lid closed' in segments[-1].replies
    assert any(r.startswith('err ') for r in segments[-2].replies)


@pytest.mark.parametrize('state,closed', [('closed', True), ('open', False)])
def test_sd_lid_default_works_without_control_socket(harness, state, closed):
    settings = environment()
    settings['CDJ_NXS_SD_LID'] = state
    result = subprocess.run([str(harness), 'quiet'], env=settings,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    frames = [bytes.fromhex(line.split(' ', 3)[3])
              for line in result.stdout.splitlines() if line.startswith('f ')]
    assert frames
    assert all(bool(frame[17] & 4) == closed for frame in frames)


def test_script_waits_for_replies_before_starting_the_next_segment(harness, tmp_path):
    # Deliberately collect nothing during the simulated frames: replies must
    # still belong to their own command, not whichever command is printed next.
    segments = script(harness, [('ping', 1), ('clear', 1), ('ping', 1)],
                      tmp_path, defer_replies=True)
    assert [[reply for reply in segment.replies
             if reply != 'ok cdj2000-input'] for segment in segments] == [
                 ['ok pong'], ['ok clear'], ['ok pong']]
    assert all(len(segment.frames) == 1 for segment in segments[:-1])
    assert len(segments[-1].frames) == 5  # existing final four-frame drain


def test_harness_reassembles_fragmented_reply_lines(harness):
    settings = environment()
    settings.pop('CDJ_INPUT_PORT', None)
    completed = subprocess.run([str(harness), 'reply-fragments'], env=settings,
                               capture_output=True, text=True, timeout=10)
    assert completed.returncode == 0, completed.stderr
    replies = [line for line in completed.stdout.splitlines()
               if line.startswith('# reply ') and line != '# reply ok cdj2000-input']
    assert replies == ['# reply ok pong', '# reply ok clear']


def test_harness_missing_reply_is_a_bounded_failure(harness):
    settings = environment()
    settings.pop('CDJ_INPUT_PORT', None)
    completed = subprocess.run([str(harness), 'reply-timeout'], env=settings,
                               capture_output=True, text=True, timeout=10)
    assert completed.returncode == 2
    assert 'timed out waiting for reply 1' in completed.stderr


def runs_of(frames: list[bytes], byte: int, mask: int) -> list[tuple[int, int]]:
    """Every stretch of consecutive frames in which the bits are set."""
    stretches: list[tuple[int, int]] = []
    start = None
    for index, payload in enumerate(frames):
        if payload[byte] & mask:
            if start is None:
                start = index
        elif start is not None:
            stretches.append((start, index - 1))
            start = None
    if start is not None:
        stretches.append((start, len(frames) - 1))
    return stretches


def field(frames: list[bytes], first: int, width: int) -> list[int]:
    return [int.from_bytes(payload[first:first + width], "big")
            for payload in frames]


# ------------------------------------------------------- the control case --
def test_viewer_contacts_reach_payload_without_entering_pulse_queue(harness, tmp_path):
    from unittest.mock import Mock

    viewer = object.__new__(view_ui.UiViewer)
    viewer.held, viewer.momentary = {}, {}
    viewer.contact_sources = {}
    viewer.deck = None
    viewer.send = Mock(return_value="ok")
    control = next(c for c in view_ui.controls() if c.input_id == "16.0")
    assert viewer.contact(control, True)
    assert viewer.contact(control, False)
    commands = [call.args[1].strip() for call in viewer.send.call_args_list]
    segments = script(harness, [(commands[0], 50), (commands[1], 5)], tmp_path)
    assert segments[0].frames and segments[1].frames
    assert all(frame[16] & 1 for frame in segments[0].frames)
    assert all(not (frame[16] & 1) for frame in segments[1].frames)


def test_without_the_port_nothing_is_merged(harness):
    """A run without CDJ_INPUT_PORT has to be a control run in the strict sense.

    Every masked comparison in INPUT_MANIFEST.md rests on the control run
    differing from the measured run in the input and in nothing else.  If this
    file did anything at all when unconfigured, every one of those numbers would
    be measuring two different machines.
    """
    frames = run(harness, "silent", port=0)
    assert all(payload == bytes(PAYLOAD_LEN) for payload in frames)


# --------------------------------------------------------------- a click ---
def test_a_press_is_a_pulse_and_not_a_state(harness):
    """Down and back up: the handlers at 0x28ddc8 detect edges, not levels."""
    frames = run(harness, "press")
    stretches = runs_of(frames, 19, 0x02)
    assert len(stretches) == 1, f"expected exactly one pulse, got {stretches}"
    start, end = stretches[0]
    assert end - start + 1 >= 2, "a pulse must span at least two exchanges"
    assert end < len(frames) - 1, "the bit never came back up"
    assert all(payload[19] == 0 for payload in frames[end + 1:])


def test_the_default_hold_lasts_the_three_hundred_milliseconds_it_claims(harness):
    frames = run(harness, "press")
    start, end = runs_of(frames, 19, 0x02)[0]
    # 100 ms of virtual time per exchange, so 300 ms is four frames counting the
    # one the press starts on.
    assert end - start + 1 == 4


def test_a_hold_shorter_than_an_exchange_still_reaches_the_wire(harness):
    """CDJ_INPUT_MIN_FRAMES, and the reason it exists.

    A hold measured only in nanoseconds can expire between two panel exchanges,
    and then the bit is never once transmitted -- a press that looks sent and
    was not.
    """
    frames = run(harness, "press-short")
    stretches = runs_of(frames, 19, 0x02)
    assert len(stretches) == 1
    start, end = stretches[0]
    assert end - start + 1 == 2


def test_two_presses_never_merge_into_one_edge(harness):
    """Queued, not overlaid: a merged pair would register as a single key."""
    frames = run(harness, "two-presses")
    first = runs_of(frames, 19, 0x02)
    second = runs_of(frames, 18, 0x01)
    assert len(first) == 1 and len(second) == 1
    assert first[0][1] < second[0][0], "the second press started too early"
    assert second[0][0] - first[0][1] >= 2, "no quiet exchange between them"
    for payload in frames:
        assert not (payload[19] & 0x02 and payload[18] & 0x01)


def test_a_held_bit_stays_down_until_it_is_released(harness):
    """For chords and for anything the firmware times, a level is wanted."""
    frames = run(harness, "hold")
    stretches = runs_of(frames, 21, 0x01)
    assert len(stretches) == 1
    start, end = stretches[0]
    assert end - start + 1 >= 4
    assert frames[-1][21] == 0


def test_a_command_still_arrives_after_hundreds_of_idle_exchanges(harness):
    """r089's shape, as far as the host can reproduce it.

    Two commands 128 seconds apart, and the second never arrived because the
    guest had closed the channel in between.  Six hundred exchanges with an
    idle socket do *not* reproduce it here, which is itself the finding: the
    cause is in the QEMU process rather than in this file's own bookkeeping.
    The test stays because it pins the half that can be checked.
    """
    frames = run(harness, "quiet")
    stretches = runs_of(frames, 19, 0x08)
    assert len(stretches) == 1
    assert stretches[0][0] > 500, "the press should land after the idle stretch"
    assert all(payload[2] == 0x5A for payload in frames[3:]), \
        "the analogue field set before the idle stretch must still be driven"


def test_silence_immediately_after_connecting_does_not_kill_the_channel(harness):
    """r094's shape, which is the one `quiet` does not have.

    `quiet` sends a command and *then* goes silent -- that is r091, and r091
    survived.  r094 lost every command of a `keys` run whose first entry was at
    t=150, so the channel sat idle from the moment it opened.  That is the only
    difference between the run that worked and the run that did not.

    This also idles in **wall-clock seconds** rather than in exchanges.  600
    exchanges pass in milliseconds, so anything counted in seconds is invisible
    to a test that only counts iterations -- the gap r094 fell through.

    **It does not reproduce.**  Run by hand at the real duration:

        harness idle-after-connect 150

    gives 45 003 exchanges across 150 real seconds of silence beginning at the
    moment of connection, and the command afterwards arrives.  Together with the
    audit (there is nothing time-based on either side of this channel -- no
    receive timeout, no keepalive, no idle limit), that leaves the cause inside
    the QEMU process and outside this file.  The short version runs here to keep
    the shape covered; it is not evidence about the guest.
    """
    frames = run(harness, "idle-after-connect", extra=["3"])
    assert runs_of(frames, 19, 0x08), \
        "the command after the idle stretch never reached the payload"


def test_a_new_client_takes_over_from_an_abandoned_one(harness):
    """The fix for r089, and the reason it is a fix whatever closed the socket.

    Before it, accept() was only reached while no client was attached, so a
    connection the server still believed in locked everyone else out for the
    rest of the run -- and every later measurement would have read as "the key
    does nothing".  Here the first client is left open and unused, exactly as a
    half-dead connection looks from the board, and the second one has to win.

    This fails without the change: byte 19 stays 0 for the whole trace.
    """
    frames = run(harness, "abandoned")
    assert runs_of(frames, 19, 0x08), \
        "the second client's press never reached the payload"


def test_a_client_that_hangs_up_cleanly_is_replaced(harness):
    frames = run(harness, "reconnect")
    assert len(runs_of(frames, 19, 0x08)) == 1


def test_a_refused_command_changes_nothing(harness):
    frames = run(harness, "bad")
    assert all(payload == bytes(PAYLOAD_LEN) for payload in frames)


def test_clear_releases_the_bits_and_stops_driving_the_analogue(harness):
    frames = run(harness, "clear")
    assert frames[-1] == bytes(PAYLOAD_LEN)
    assert any(payload[21] & 0x01 for payload in frames)
    assert any(payload[2] == 0x5A for payload in frames)


# -------------------------------------------------------------- the knob ---
def test_the_analogue_fields_land_where_the_live_probe_found_them(harness):
    """The values from the memory cdj-panel-payload-decoded, on the wire.

    payload byte 2 = 0x5a showed up at 0x04fe2a20, and bytes 4..5 = 12 34 at
    0x04fe2a28 as 0x1234 -- so field 0 is one byte at 2 and field 2 is a 16-bit
    big-endian pair at 4.
    """
    frames = run(harness, "analog")
    assert frames[-1][2] == 0x5A
    assert frames[-1][3] == 0x00
    assert frames[-1][4] == 0x12 and frames[-1][5] == 0x34


def test_the_rotary_ramps_one_step_per_exchange(harness):
    """The thing CDJ_PANEL_KEYS cannot do, which is why it never drove this.

    An encoder that jumps five counts between two frames is not what the
    firmware sees on hardware; it has to walk.
    """
    values = field(run(harness, "rotary"), 8, 2)
    walk = values[values.index(1):]
    assert walk[:5] == [1, 2, 3, 4, 5]
    assert set(walk[5:]) == {5}, "the ramp overshot or kept moving"


def test_the_rotary_turns_the_other_way_and_wraps_like_a_counter(harness):
    values = field(run(harness, "rotary-back"), 4, 2)
    walk = values[values.index(2):]
    assert walk[:5] == [2, 1, 0, 0xFFFF, 0xFFFE]
    assert set(walk[5:]) == {0xFFFE}


def test_the_step_size_is_settable(harness):
    values = field(run(harness, "rotary-step"), 8, 2)
    walk = values[values.index(4):]
    assert walk[:3] == [4, 8, 12]
    assert set(walk[3:]) == {12}, "a coarse step must still stop on the target"


# ------------------------------------------------- the window, on the wire --
def window_steps() -> list[tuple[str, int]]:
    """Every line every control of the operator window can emit, one by one.

    `clear` between them, so each segment starts from a payload of zeroes and
    what the line did is the whole difference.  The exchange counts are the
    ramp's: `rotary` walks one count per exchange, so a 200-count detent needs
    200 frames before it has arrived.

    **A press is counted from its own hold, not from a constant.**  It used to
    be a flat 12 exchanges, which was enough for the board's 300 ms default and
    silently too few for anything longer: when the window's buttons went to
    2 500 ms this test read "the bit never came back up", which is a true
    statement about a run that was stopped in the middle of the press rather
    than about the press.  Deriving it means the harness follows
    `panel_control.PLAN_HOLD_MS` wherever it goes next.
    """
    steps: list[tuple[str, int]] = []
    for control in view_ui.controls():
        for line in control.lines:
            command = line.strip()
            parts = command.split()
            if parts[0] == "rotary":
                after = abs(int(parts[2])) + 8
            elif parts[0] == "press":
                hold = (int(parts[3]) if len(parts) > 3
                        else panel_control.CHANNEL_HOLD_DEFAULT_MS)
                # Down for the hold, up for the gap, both in 100 ms exchanges
                # (FRAME_NS in tests/cstub/harness.c), plus room for
                # CDJ_INPUT_MIN_FRAMES at each end.
                after = ((hold + panel_control.CHANNEL_GAP_MS)
                         // HARNESS_EXCHANGE_MS) + 8
            else:
                after = 4
            steps.append(("clear", 2))
            steps.append((command, after))
    return steps


def test_every_control_of_the_window_moves_the_payload(harness, tmp_path):
    """The click surface, checked at the payload instead of at the widget.

    `view_ui.coverage()` says all 46 inputs have a control; that is arithmetic
    over a table in the same repository as the table it counts.  This is the
    other half: every line those controls emit goes through the **real**
    `cdj2000_input.c`, and the payload has to carry what the control's own
    `input_id` claims -- the right byte, the right bit, the right big-endian
    value.  A control whose label and whose wire format disagree fails here,
    and so does one that emits a line the board refuses.
    """
    lines = [line for control in view_ui.controls() for line in control.lines]
    assert lines, "the window emits nothing at all"
    segments = script(harness, window_steps(), tmp_path)
    # Every second one: `window_steps` emits a `clear` before each line, and
    # one of the lines *is* `clear` -- the "release all" control -- so the
    # separator cannot be told from the payload by its name.
    driving = segments[1::2]
    assert len(driving) == len(lines)

    for control, segment in zip(
            [control for control in view_ui.controls()
             for _ in control.lines], driving):
        parts = segment.command.split()
        refused = [reply for reply in segment.replies
                   if reply.startswith("err")]
        assert not refused, ("the board refused %r from %s: %s"
                             % (segment.command, control.label, refused))

        if parts[0] == "press":
            byte, mask = int(parts[1]), int(parts[2], 16)
            assert (byte, mask) == panel_control.button_mask(control.input_id), \
                "%s sends a bit its own input_id does not name" % control.label
            stretches = runs_of(segment.frames, byte, mask)
            assert len(stretches) == 1, \
                "%s: expected one pulse, got %s" % (control.label, stretches)
            assert segment.frames[-1][byte] == 0, \
                "%s: the bit never came back up" % control.label
        elif parts[0] in ("rotary", "analog"):
            index, value = int(parts[1]), int(parts[2])
            assert "field%d" % index == control.input_id.split("-")[0], \
                "%s drives a field its own input_id does not name" % control.label
            start, width = panel_control.ANALOG_FIELDS[index]
            arrived = field(segment.frames, start, width)[-1]
            assert arrived == value & ((1 << (8 * width)) - 1), \
                ("%s: %r left the field at %#x" % (control.label,
                                                   segment.command, arrived))
        else:
            assert control.kind == "channel"
            assert all(payload == bytes(PAYLOAD_LEN)
                       for payload in segment.frames), \
                "%s touched the payload" % control.label
            assert any(reply.startswith("ok") for reply in segment.replies), \
                "%s got no answer: %s" % (control.label, segment.replies)


def test_the_touch_flag_reaches_bit_fifteen_of_the_pair(harness, tmp_path):
    """The input no run has ever sent, and the reason `rotary` cannot send it.

    0x28e230 tests bytes 12/13 against 0x8000 before masking the position to
    0x1ff, so the flag has a destination of its own (0x04fe2a3c bit 2).  The
    ramp walks one count per exchange, so from rest 0x8000 is 32 768 exchanges
    away -- which is why every run in INPUT_MANIFEST.md that drove field 6 with
    `rotary` left this arm untaken.
    """
    touch = panel_control.ANALOG_TOUCH_MASK
    segments = script(harness, [
        ("clear", 2),
        (panel_control.encode_analog(6, touch | 300).strip(), 4),
        ("clear", 2),
        (panel_control.encode_rotary(6, 12).strip(), 20),
    ], tmp_path)
    flagged = field(segments[1].frames, 12, 2)[-1]
    assert flagged & touch, "the touch flag is not on the wire"
    assert flagged & panel_control.ANALOG_POSITION_MASK == 300

    # And the same field driven the way every run so far has driven it: from
    # rest, the ramp cannot reach the flag.  That is the finding rather than a
    # defect -- it is why `field6` reads `no-op, proven` for a flag whose arm
    # at 0x28e26c has never once been taken.
    ramped = field(segments[3].frames, 12, 2)[-1]
    assert ramped == 12
    assert not ramped & touch
