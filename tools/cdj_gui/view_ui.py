"""Launch the CDJ-2000 GUI simulator, display its framebuffer, and drive it.

The window is laid out like the front of the player, and that layout is the
point rather than decoration:

    Only the inner rectangle is the 480x234 panel.  BROWSE / TAG LIST / INFO /
    MENU across the top and LINK / USB / SD / DISC down the left are hardware
    buttons -- backlit plastic that appears in no frame dump.

So the virtual buttons sit **beside** the picture, never inside it, in the
positions the reference photo `cdj2000-interface-real-unit.jpg` puts them.  A
button that were drawn into the panel image would be inventing LCD content, and
this project has already spent time chasing exactly that mistake.

The picture itself is **cropped, not resampled**: the PPI emits 255 lines, 234
of them active and 21 blanking, so the frame is cut to 480x234 and then zoomed
by whole pixels.  Capturing at 234 instead wraps the capture at the wrong point
and shifts the whole image (memory cdj-display-geometry).

Clicks go out over the control channel of `emulator/qemu/cdj2000_input.c` --
see `tools/cdj_main/panel_control.py`.  Without `--control-port` the buttons
refuse rather than doing nothing, which is the difference between a window that
says it cannot and a window that looks broken.

**The window counts its own controls, out loud.**  `panel_control.input_ids()`
enumerates the 48 inputs the board decodes; `coverage()` compares that against
the controls built here and the status line says how many are reachable.  That
line exists because the same gap was found twice by accident: `plan keys` drove
38 of the 46 and printed nothing about the other eight, and this window offered
38 dedicated controls and one spinbox captioned "sweep 0..6" for the analogue
half -- the same eight, silent in the same way.  A window that is missing a
control must say so on its own face.

**Every label on a key is checked against the firmware**, not against a photo.
`panel_control.FIRMWARE_KEY_NAMES` is MAIN's own SERVICE MODE name table, and
`tests/test_panel_names_match_the_firmware.py` rebuilds it from the image and
compares it with this file, `panel_control` and both launchers.  That test
exists because the four SOURCE keys were labelled backwards here for weeks --
`SD` sent the USB bit -- and every tool involved agreed with every other one
while all of them disagreed with the board.
"""

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
from concurrent.futures import Future, ThreadPoolExecutor
import json
import os
import re
import subprocess
import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk
from typing import NamedTuple

from PIL import Image, ImageDraw, ImageTk

REPO_ROOT = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO_ROOT))
from tools.paths import (BFIN_SIM, BOARDS, FIRMWARE,  # noqa: E402
                         PACKETS, RUNS, board_path)
from tools.cdj_main import panel_control  # noqa: E402
from tools.cdj_main import nxs_panel  # noqa: E402
from tools.cdj_gui import faceplate  # noqa: E402

# ---------------------------------------------------------------- geometry --
#
# The panel is 480x234, built from a 480x140 top and a 480x94 bottom surface.
# The capture is 255 lines because that is what the PPI emits; the last 21 are
# blanking and are cut off here rather than squeezed in.
PANEL_WIDTH = 480
PANEL_HEIGHT = 234
CAPTURE_HEIGHT = 255
PANEL_CROP = (0, 0, PANEL_WIDTH, PANEL_HEIGHT)

# The hardware buttons, in the positions the reference photo puts them.  The
# second field is the key `panel_control.button_mask` understands, or None where
# the payload bit has not been *measured* -- naming a bit from where a label
# sits on the front panel is a guess, and this file does not make guesses.
#
# **The top row is bound as of 2026-08-07, and the evidence is not its
# position.**  It is MAIN's own SERVICE MODE name table, five static hops from
# the payload bit to the printed string (`panel_control.FIRMWARE_KEY_NAMES`),
# which names 20.0..20.3 BROWSE / TAG LIST / INFORMATION / MENU in that order.
# That is the same table -- and the same reading -- that settled the four SOURCE
# keys, where this project's own labels had been reversed for weeks.  Until it
# was read, "BROWSE is probably the top-left key" was a guess about a photo and
# stayed unbound, correctly.
#
# `INFO` is the spelling on the key, because that is what the front
# panel says; the firmware calls the same bit `INFORMATION`, and the test that
# compares the two knows it.
#
# This matters beyond tidiness: the definition of done for this window is
# "click SD, click BROWSE, turn the encoder", and the BROWSE key used to
# refuse.
TOP_BUTTONS: list[tuple[str, str | None]] = [
    ("BROWSE", "20.0"),
    ("TAG LIST", "20.1"),
    ("INFO", "20.2"),
    ("MENU", "20.3"),
]
# **REVERSED UNTIL 2026-08-07.**  These four labels sat over the wrong masks --
# `SD` sent 19.1, the USB key -- for as long as the project has existed; see
# `panel_control.BUTTON_NAMES`.  The labels did not move, the table under them
# did.
LEFT_BUTTONS: list[tuple[str, str | None]] = [
    ("LINK", "link"),
    ("USB", "usb"),
    ("SD", "sd"),
    ("DISC", "disc"),
]

# Where each block sits in the outer grid, as (row, column).  Written down
# rather than left implicit in the build code so that a test can assert what
# The rule: the picture has a cell of its own, and every button is in a
# different one.  Nothing is ever drawn onto the frame.
LAYOUT: dict[str, tuple[int, int]] = {
    "top": (0, 1),      # BROWSE / TAG LIST / INFO / MENU, above the panel
    "left": (1, 0),     # LINK / USB / SD / DISC, left of the panel
    "panel": (1, 1),    # the 480x234 LCD, and nothing else
    "side": (1, 2),     # the 40 raw bits, right of the panel
    "analog": (2, 1),   # the eight analogue fields, below it
    "channel": (3, 1),  # ping / state / release all
    "status": (4, 0),
    "note": (5, 0),
}

MANIFEST = REPO_ROOT / "INPUT_MANIFEST.md"


def crop_panel(frame: Image.Image) -> Image.Image:
    """Cut the PPI's 21 blanking lines away, leaving the 480x234 panel.

    A copy of the top-left rectangle and nothing else -- no scaling, no
    overlay.  Capturing at 234 instead of cropping wraps the capture at the
    wrong point and shifts the whole image down (memory cdj-display-geometry).
    """
    if frame.height > PANEL_HEIGHT or frame.width > PANEL_WIDTH:
        return frame.crop(PANEL_CROP)
    return frame


# A verdict is only worth showing next to the run it came from.  The manifest
# now carries two button tables -- r026's machine had an empty browse pane and
# the spinning "Wait" platter, r096's has a six-category list -- and the same
# bit measures 372 bytes in one and 0 in the other.  So the world travels with
# the verdict all the way onto the button, rather than being dropped at the
# parser and reinvented by whoever reads the window.
#
# There are two heading forms because the manifest has two halves and only one
# of them used to be a table.  The eight analogue fields were measured in r115
# and r117 and then written up in prose, which is how they came to be missing
# from the canonical plan *and* from this window at the same time.  A table
# reads like the buttons' tables and is parsed by the same code.
WORLD_HEADING = re.compile(
    r"^##\s+The (?:buttons|analogue fields) in the (\S+) world")
INPUT_NAME = re.compile(r"(?:\d+\.\d|field\d)")
MARKUP = re.compile(r"[*`]")


def manifest_verdicts(path: Path = MANIFEST) -> dict[str, tuple[str, str]]:
    """(verdict, world) per input id, read out of INPUT_MANIFEST.md.

    Reading it instead of copying it keeps one table in the repository.  The
    manifest is the record of what was measured; this window only displays it.

    Only rows under a `## The buttons|analogue fields in the <run> world`
    heading count.  That is not tidiness: the file has other tables keyed by an
    input name -- the "looking, not only counting" one, for instance -- and
    picking rows up by shape alone would put a caption where a verdict belongs.
    Later sections overwrite earlier ones, so the newest measurement of an input
    is the one shown, and its run name is shown with it.
    """
    verdicts: dict[str, tuple[str, str]] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return verdicts
    world = None
    for line in text.splitlines():
        heading = WORLD_HEADING.match(line)
        if heading:
            world = heading.group(1)
            continue
        if line.startswith("## "):
            world = None
            continue
        if world is None or not line.startswith("|"):
            continue
        cells = [MARKUP.sub("", cell).strip()
                 for cell in line.strip("|").split("|")]
        if not INPUT_NAME.fullmatch(cells[0]) or len(cells) < 3:
            continue
        # The verdict is the last column of every one of these tables; their
        # other columns differ, so counting from the left would need one rule
        # per table.
        verdicts[cells[0]] = (cells[-1], world)
    return verdicts


# ---------------------------------------------------------------- controls --
#
# Every clickable thing in the window, as data, so that "can a human trigger
# every input?" is a question with an arithmetic answer instead of a reading of
# the layout code.
#
# `lines` is what this control would put on the wire, verbatim.  That is the
# part worth having: `tests/test_input_channel.py` feeds all of them through the
# real `cdj2000_input.c` -- compiled, on a real socket -- and checks that each
# one moves the payload byte its `input_id` claims.  A control that emits a line
# the board refuses is then a failing test rather than a click that seems to
# work.


class Control(NamedTuple):
    label: str
    input_id: str | None    # "18.1", "field6", "field6-touch", or None
    kind: str               # button | encoder | slider | touch | channel
    group: str              # top | left | bits | analog | channel
    lines: tuple[str, ...]  # the protocol lines this control can emit
    note: str


def firmware_name(byte: int, bit: int) -> str:
    """What MAIN's SERVICE MODE page prints for this bit, or ''.

    One lookup, so a label in this window and a label in a plan cannot come
    from two different tables.  The table itself is pinned against the image by
    `tests/test_panel_names_match_the_firmware.py`.
    """
    return panel_control.FIRMWARE_KEY_NAMES.get((byte, bit), "")


# How long a legacy click holds a key down.  MAIN copies the key level into
# the status record it builds next, and it builds one every 3.05 s when
# nothing else changes (measured on the link, menu2: 42.59, 45.61, 48.66 ...),
# so a press lands only if it spans one of those.  The plan hold of 2 800 ms
# (panel_control.PLAN_HOLD_MS) was chosen against a measurement plan's 10 s
# attribution window and covers a 3.05 s cadence 92 times in 100; 3 300 ms
# covers it every time.  These timing notes describe the legacy panel path.
WINDOW_HOLD_MS = 3300

# NXS accepts ordinary encoder/button pulses through its live panel contact.
# Keep interactive clicks short; callers can still request a deliberate long
# hold through Shift-click or an explicit control command.
NXS_CLICK_HOLD_MS = 300


def publication_age_note(mtime_ns: int, now: float) -> str | None:
    """Publication age is observable; an unchanged image cannot prove a stall."""
    age = max(0.0, now - mtime_ns / 1_000_000_000)
    if age < 5:
        return None
    return (f"Last framebuffer publication {int(age)}s ago — "
            "display may be static; emulator liveness unverified")


def run_diagnostics_text(snapshot: dict, manifest: dict | None = None) -> str:
    """Format one bounded run observation for a person at the keyboard.

    This deliberately reports observations and their limits.  In particular,
    a frame age is not presented as a CPU liveness test and link records are
    not promoted to proof that the GUI consumed a reply or that audio played.
    Keeping this formatter outside Tk also gives agents and tests one stable
    vocabulary for the human diagnostics window.
    """
    session = snapshot.get("session") or {}
    frame = snapshot.get("frame") or {}
    link = snapshot.get("link") or {}
    lines = ["RUN  %s" % snapshot.get("run", "(unknown)"),
             "session: %s" % session.get("state", "unknown")]
    if session.get("processes"):
        processes = ", ".join("%s=%s" % item
                               for item in sorted(session["processes"].items()))
        lines.append("processes: " + processes)

    frame_status = frame.get("status", "missing")
    if frame.get("age_seconds") is not None:
        lines.append("frame: %s, %.1fs old (%sx%s)" % (
            frame_status, max(0.0, float(frame["age_seconds"])),
            frame.get("width", "?"), frame.get("height", "?")))
    else:
        lines.append("frame: " + frame_status)

    lines.append("link: %s; %s records; %s" % (
        link.get("status", "missing"), link.get("records", 0),
        "caught up" if link.get("caught_up", True) else "reader is catching up"))
    lines.append("browser: " + str(snapshot.get(
        "progress", "Waiting for a browser reply from firmware.")))
    transport = link.get("transport")
    if isinstance(transport, dict):
        state = "playing" if transport.get("play_requested") else "paused/stopped"
        remaining = transport.get("remaining_seconds")
        duration = transport.get("duration_seconds")
        if isinstance(remaining, (int, float)) and isinstance(duration, (int, float)):
            lines.append("transport: %s; %.3fs remaining / %.3fs duration"
                         % (state, remaining, duration))
        else:
            lines.append("transport: %s; native counter sample" % state)
        lines.append("transport evidence: compare newer records for counter "
                     "movement; this sample does not test audio output")
    media = snapshot.get("media") or ((manifest or {}).get("media") or {})
    configured = media.get("images") if isinstance(media, dict) else None
    if configured:
        lines.append("media: " + ", ".join("%s=%s" % item
                                            for item in sorted(configured.items())))
    if snapshot.get("gui_stats"):
        lines.append("GUI: " + str(snapshot["gui_stats"]))
    actions = snapshot.get("recent_actions") or []
    if actions:
        lines.append("recent UI/agent actions:")
        for action in actions[-5:]:
            outcome_name = action.get("outcome")
            if "ok" in action:
                succeeded = bool(action["ok"])
            else:
                succeeded = outcome_name not in ("error", "failed", "failure")
            outcome = "ok" if succeeded else "FAILED"
            detail = action.get("reply") or action.get("error") or ""
            name = action.get("command") or action.get("action") or "action"
            label = ("%s [%s]" % (name, outcome_name)
                     if outcome_name else name)
            lines.append("  %s %s → %s" % (outcome, label, detail))

    endpoints = snapshot.get("endpoints") or ((manifest or {}).get("endpoints") or {})
    if endpoints:
        gdb_host, gdb_port = endpoints.get("gdb_host"), endpoints.get("gdb_port")
        if gdb_host and gdb_port:
            lines.append("GDB: %s:%s (MAIN only; GUI and host deadlines continue)" %
                         (gdb_host, gdb_port))
        elif (snapshot.get("debug") or (manifest or {}).get("debug") or {}).get("enabled"):
            lines.append("GDB: enabled, endpoint not published")
    debug = snapshot.get("debug") or ((manifest or {}).get("debug") or {})
    if debug.get("main_starts_paused"):
        lines.append("debug pause: MAIN only; GUI and host deadlines continue")

    faults = snapshot.get("recent_fault_lines") or []
    if faults:
        lines.append("faults (bounded log tails):")
        for item in faults[-5:]:
            lines.append("  %s: %s" % (item.get("file", "log"),
                                       item.get("line", item.get("error", ""))))
    else:
        lines.append("faults: none found in bounded log tails (absence is not proof)")
    return "\n".join(lines)


def observe_run(run: Path, observer=None) -> dict:
    """Read one bounded run snapshot without touching Tk or blocking it."""
    from tools.cdj_main import run_state

    # run_state.observe includes the bounded manifest fields needed by the
    # viewer (endpoints, debug scope and media).  Keep the snapshot compact so
    # the clipboard handoff does not duplicate the full run.json.
    return run_state.observe(run, observer)

# A long press.  The firmware tells a short MENU from a held one by whether
# the key is still down in the *next* status record, so on this link "held"
# means held across two of MAIN's 3.05 s record builds.  Measured, MENU
# alone: 1.5 s and 3.3 s open the CUE LINK box (the short-press function),
# 7 s opens the UTILITY screen (its list empty: the entries are payloads
# MAIN does not deliver).  6.5 s spans two builds wherever it starts.
WINDOW_LONG_HOLD_MS = 6500


def hardware_button(label: str, key: str, group: str, note: str) -> Control:
    """One front-panel key, resolved through the one naming table there is."""
    byte, mask = panel_control.button_mask(key)
    bit = mask.bit_length() - 1
    name = firmware_name(byte, bit)
    return Control(label, "%d.%d" % (byte, bit), "button", group,
                   (panel_control.encode_press(byte, mask, WINDOW_HOLD_MS),),
                   "%s; MAIN calls it %s" % (note, name) if name else note)


def button_controls() -> list[Control]:
    """The hardware keys and the 40 decoded bits.

    The eight hardware keys appear twice on purpose -- as BROWSE/TAG LIST/INFO/
    MENU above and LINK/USB/SD/DISC on the left, where the reference photo puts
    them, and as 19.0..19.3 / 20.0..20.3 in the bit grid.  Two controls for one
    input is not a gap; a *label* with no input behind it is, and until MAIN's
    own name table was read the top row was exactly that.
    """
    controls: list[Control] = []
    for label, key in TOP_BUTTONS:
        if key is None:
            controls.append(Control(
                label, None, "button", "top", (),
                "no run has attributed a payload bit to this key"))
        else:
            controls.append(hardware_button(
                label, key, "top",
                "decoded by 0x28e59a/0x28e44a and named by MAIN's SERVICE MODE "
                "table"))
    # UTILITY is not a key of its own: on the player it is MENU held down, and
    # on this link "held down" means held across two of MAIN's 3 s status
    # records (WINDOW_LONG_HOLD_MS).  A key of its own beside MENU says so
    # without asking anyone to remember a modifier.
    byte, mask = panel_control.button_mask("20.3")
    controls.append(Control(
        "UTILITY", "20.3-hold", "hold", "top",
        (panel_control.encode_press(byte, mask, WINDOW_LONG_HOLD_MS),),
        "MENU held down; the firmware calls it UTILITY when the key is still "
        "down in the next status record"))
    for label, key in LEFT_BUTTONS:
        controls.append(hardware_button(
            label, key, "left",
            "SOURCE key, from 0x28ddc8; the four were reversed until "
            "2026-08-07"))
    for byte, bit in panel_control.BUTTON_BITS:
        name = firmware_name(byte, bit)
        controls.append(Control(
            "%d.%d" % (byte, bit), "%d.%d" % (byte, bit), "button", "bits",
            (panel_control.encode_press(byte, 1 << bit, WINDOW_HOLD_MS),),
            "decoded by 0x28e1ae" + ("; %s" % name if name else
                                     "; no name in MAIN's table")))
    return controls


def analog_controls() -> list[Control]:
    """One control per analogue field, plus the flag hiding inside field 6.

    What was here before was a spinbox holding a field number and four arrows
    that moved whichever field the number named.  That is a command line with
    buttons on it: it reaches every field in the sense that a shell reaches
    every file, and its caption -- "which field is the encoder is not
    identified; sweep 0..6" -- told the reader that field 7 was not worth
    trying, on a day when `panel_control.ENCODER_FIELD` had said 7 for weeks.

    Each field now has its own row: a detent pair for turning it, a slider for
    dragging it end to end, and an exact value for the cases where a number is
    what is wanted.  `analog` is the only verb that can reach field 6's touch
    flag, and until this row existed nothing in the project ever sent one.
    """
    controls: list[Control] = []
    for entry in panel_control.ANALOG_CONTROLS:
        identifier = "field%d" % entry.field
        lines = (
            panel_control.encode_rotary(entry.field, -entry.step),
            panel_control.encode_rotary(entry.field, entry.step),
            panel_control.encode_analog(entry.field, entry.low),
            panel_control.encode_analog(entry.field, entry.high),
        )
        controls.append(Control(entry.label, identifier, entry.kind, "analog",
                                lines, entry.evidence))
        if entry.touch_mask:
            controls.append(Control(
                "%s  touch" % entry.label, "%s-touch" % identifier, "touch",
                "analog",
                (panel_control.encode_analog(entry.field, entry.touch_mask),
                 panel_control.encode_analog(entry.field, 0)),
                "bit %d of the pair, tested at 0x28e230 and landing in "
                "0x04fe2a3c bit 2.  `rotary` walks one count per exchange, so "
                "no run has ever reached it" % (entry.touch_mask.bit_length() - 1)))
    return controls


def channel_controls() -> list[Control]:
    """The verbs that are about the channel rather than about an input.

    `state` is the one that earns its place: it is the only way to ask the
    board what it is actually driving, and without it a window can only show
    what it *sent*.  Those two came apart in r089 and the run could not tell a
    key that did nothing from a key that never arrived.
    """
    return [
        Control("ping", None, "channel", "channel",
                (panel_control.encode("ping"),), "is the channel alive"),
        Control("state", None, "channel", "channel",
                (panel_control.encode("state"),),
                "held bits, analogue fields and queue depth, from the board"),
        Control("release all", None, "channel", "channel",
                (panel_control.encode("clear"),),
                "drop every held bit and stop driving the analogue fields"),
    ]


def controls(nxs: bool = False) -> list[Control]:
    buttons = button_controls()
    if nxs:
        translated = []
        for control in buttons:
            if control.group == "bits":
                continue
            if control.input_id is not None:
                target = nxs_panel.deck_input(control.input_id)
                byte, mask = panel_control.button_mask(target)
                duration = WINDOW_LONG_HOLD_MS if control.kind == "hold" else WINDOW_HOLD_MS
                control = control._replace(input_id=target,
                    lines=(panel_control.encode_press(byte, mask, duration),),
                    note="NXS firmware panel decoder 042f5810")
            translated.append(control)
        for byte, bit in nxs_panel.BUTTON_BITS:
            key = f"{byte}.{bit}"
            name = nxs_panel.KEY_NAMES.get((byte, bit), "unassigned")
            translated.append(Control(key, key, "button", "bits",
                (panel_control.encode_press(byte, 1 << bit, WINDOW_HOLD_MS),),
                f"NXS decoder 042f5810; {name}"))
        buttons = translated
    return buttons + analog_controls() + channel_controls()


def coverage(built: list[Control] | None = None, nxs: bool = False
             ) -> tuple[list[str], list[str], list[str]]:
    """(inputs reached, inputs with no control, controls with no input).

    The third list is not padding.  A control that claims an input the decoder
    does not have is the same class of error as a missing one, and it is the
    error a hand-written table makes first.
    """
    built = controls(nxs) if built is None else built
    board = nxs_panel.input_ids() if nxs else panel_control.input_ids()
    # field6-touch is a control for a flag inside field 6 rather than for one
    # of the 48 the manifest enumerates; it is counted as reaching field 6.
    reached = {control.input_id.split("-")[0]
               for control in built if control.input_id}
    return ([name for name in board if name in reached],
            [name for name in board if name not in reached],
            sorted(name for name in reached if name not in board))


def coverage_line(built: list[Control] | None = None, nxs: bool = False) -> str:
    """What the window says about itself, in one line."""
    reached, missing, stray = coverage(built, nxs)
    text = "%d of %d inputs have a control" % (len(reached),
                                               len(reached) + len(missing))
    if missing:
        text += "; NO CONTROL FOR " + ", ".join(missing)
    if stray:
        text += "; NOT ON THIS BOARD: " + ", ".join(stray)
    return text


def button_text(control: Control) -> str:
    """What is written on the key, with a mark where nothing is bound.

    Not only a colour.  The style below sets a foreground, and Windows' default
    ttk theme draws `TButton` from a bitmap and ignores it -- so on the machine
    this window is actually used on, colour alone would say nothing at all.  A
    character in the label survives every theme.
    """
    if control.input_id is None and control.kind != "channel":
        return "%s ?" % control.label
    return control.label


def refusal(control: Control, control_port: int) -> str | None:
    """Why this click cannot be sent, or None if it can.

    A key with no attributed bit **refuses** rather than guessing.
    The refusal has to be audible, though, and it was not: the four top buttons
    were built `disabled`, so their command never ran and the sentence
    explaining them was unreachable code.  A greyed-out button says "no"; it
    does not say "because no run has measured which bit this is", and the
    difference is whether the reader blames the emulation.
    """
    if control.input_id is None and control.kind != "channel":
        return ("%s: no payload bit has been attributed to this key.  "
                "INPUT_MANIFEST.md names a bit only where a run measured it, "
                "and where a label sits on the front panel is not "
                "evidence." % control.label)
    if not control_port:
        return ("%s: no control channel.  Start view_vm without --no-control, "
                "or pass --control-port, and run QEMU with CDJ_INPUT_PORT."
                % control.label)
    return None


def protocol_ok(reply: object) -> bool:
    """Whether a control-channel reply is an explicit success response."""
    if not isinstance(reply, str):
        return False
    tokens = reply.strip().split(None, 1)
    return bool(tokens) and tokens[0].lower() == "ok"


def simulator_path(path: Path) -> str:
    """Return a Windows path that GNU sim's hardware parser will not unescape."""

    return path.resolve().as_posix()


class UiViewer:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.root = tk.Tk()
        self.root.title(f"{args.device_name} · Emulator")
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.configure(background="#121418")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        self.photo: ImageTk.PhotoImage | None = None
        self.last_mtime_ns = -1
        self.boot_started = time.monotonic()
        self.has_firmware_picture = False
        self.log_stream = None
        self.process: subprocess.Popen[bytes] | None = None
        self.run_path: Path | None = (args.run.resolve()
                                       if getattr(args, "run", None) else None)
        self.run_observer = None
        self.run_snapshot: dict | None = None
        self.diagnostics_future: Future | None = None
        self.diagnostics_executor: ThreadPoolExecutor | None = None
        self.diagnostics_text: tk.Text | None = None
        self.diagnostics_error = ""

        # `deck` is the device skin's canvas and None for the lab skin; the
        # picture path branches on it rather than on the argument, so a skin
        # that fails to build shows the old window instead of no window.
        self.deck: faceplate.Faceplate | None = None
        self.image_label: ttk.Label | None = None
        self.shown = self.published = 0
        self.rate_since = time.monotonic()
        self.fps = 0.0
        self.stopped = False

        self.panel: panel_control.PanelControl | None = None
        self.last_control_error: str | None = None
        self.control_note = tk.StringVar(value="")
        self.run_progress = tk.StringVar(value="")
        self.status = tk.StringVar(value="Starting Blackfin firmware…")
        self.held: dict[tuple[int, int], Control] = {}
        self.momentary: dict[tuple[int, int], Control] = {}
        self.contact_sources: dict[tuple[int, int], set[object]] = {}
        # Key -> the time its click's press is over (hold plus the board's
        # gap).  The board queues presses one behind the other, so a click
        # repeated while the first is still down does not land sooner: it
        # lands 3.6 s later and, for a key that toggles something (MENU
        # opens the UTILITY menu and closes it again), undoes the first.
        # That is what made the menu "close again after a few seconds"
        # under repeated clicking.  A click inside the window is refused
        # with the time left, and the screen is expected a few seconds
        # after that.
        self.in_flight: dict[str, float] = {}
        self.analog_value: dict[int, tk.StringVar] = {}
        self.analog_position: dict[int, tk.DoubleVar] = {}
        self.analog_touch: dict[int, tk.BooleanVar] = {}
        self.status_light: tk.Canvas | None = None

        self.configure_theme()
        self.build_layout()
        self.build_diagnostics_window()
        self.status.trace_add("write", self.status_changed)
        self.status_changed()
        self.show_boot_panel()
        self.start_simulator()
        self.root.after(self.args.refresh_ms, self.refresh)
        if self.run_path is not None:
            self.diagnostics_executor = ThreadPoolExecutor(
                max_workers=1, thread_name_prefix="cdj-run-observer")
            self.root.after(0, self.poll_run_diagnostics)

    # ------------------------------------------------------------- layout --
    def configure_theme(self) -> None:
        """A compact dark instrument theme shared by both window skins."""
        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("TFrame", background="#181b20")
        style.configure("TLabel", background="#181b20", foreground="#cdd2db",
                        font=("TkDefaultFont", 10))
        style.configure("Muted.TLabel", foreground="#858d9b")
        style.configure("Title.TLabel", foreground="#f1f3f7",
                        font=("TkDefaultFont", 15, "bold"))
        style.configure("Eyebrow.TLabel", foreground="#6faaf2",
                        font=("TkDefaultFont", 8, "bold"))
        style.configure("Badge.TLabel", background="#202a36",
                        foreground="#9dc7fa", padding=(8, 4),
                        font=("TkDefaultFont", 8, "bold"))
        style.configure("TLabelframe", background="#181b20",
                        bordercolor="#363c46", relief="solid", borderwidth=1)
        style.configure("TLabelframe.Label", background="#181b20",
                        foreground="#9ca5b4", font=("TkDefaultFont", 9, "bold"))
        style.configure("TButton", background="#2b3038", foreground="#e5e8ee",
                        bordercolor="#505864", lightcolor="#59616e",
                        darkcolor="#16191d", padding=(8, 6),
                        font=("TkDefaultFont", 9, "bold"))
        style.map("TButton",
                  background=[("pressed", "#1f5b9e"),
                              ("active", "#39414c")],
                  bordercolor=[("focus", "#74b7ff"),
                               ("active", "#788393")],
                  foreground=[("disabled", "#6c737e")])
        style.configure("Quiet.TButton", background="#20242a",
                        foreground="#b7bec9", padding=(7, 5))
        style.configure("Unbound.TButton", foreground="#f19a9f",
                        background="#332428")
        style.map("Unbound.TButton", foreground=[("active", "#ffc2c5")],
                  background=[("active", "#493035")])
        style.configure("TEntry", fieldbackground="#0e1013", foreground="#eef1f5",
                        insertcolor="#eef1f5", bordercolor="#4a525e",
                        padding=(5, 4))
        style.configure("TCheckbutton", background="#181b20",
                        foreground="#c5cad3")
        style.map("TCheckbutton", background=[("active", "#181b20")])
        style.configure("Horizontal.TScale", background="#181b20",
                        troughcolor="#0d0f12", bordercolor="#343a43")

    def status_changed(self, *_args) -> None:
        """Keep the footer's status light meaningful without parsing state."""
        if self.status_light is None:
            return
        message = self.status.get().lower()
        if "exited" in message or "error" in message or "lost" in message:
            color = "#f05f6c"
        elif "boot" in message or "starting" in message or "waiting" in message:
            color = "#e8bb43"
        else:
            color = "#3fd083"
        self.status_light.itemconfigure("light", fill=color, outline=color)

    def build_layout(self) -> None:
        if self.args.skin == "device":
            self.build_deck()
        else:
            self.build_lab()

    def build_diagnostics_window(self) -> None:
        """Create the run observer window; reads happen on a worker thread."""
        self.diagnostics_window = tk.Toplevel(self.root)
        self.diagnostics_window.title("CDJ-2000 · Run diagnostics")
        self.diagnostics_window.withdraw()
        self.diagnostics_window.protocol("WM_DELETE_WINDOW",
                                         self.diagnostics_window.withdraw)
        self.diagnostics_window.geometry("760x520")
        self.diagnostics_window.columnconfigure(0, weight=1)
        self.diagnostics_window.rowconfigure(0, weight=1)
        self.diagnostics_text = tk.Text(
            self.diagnostics_window, background="#0e1013", foreground="#d7dce5",
            insertbackground="#eef1f5", relief="flat", wrap="word",
            padx=12, pady=10, state="disabled")
        self.diagnostics_text.grid(row=0, column=0, sticky="nsew")
        actions = ttk.Frame(self.diagnostics_window, padding=(10, 7))
        actions.grid(row=1, column=0, sticky="ew")
        ttk.Button(actions, text="Refresh now",
                   command=self.request_run_diagnostics).pack(side="left")
        ttk.Button(actions, text="Copy snapshot",
                   command=self.copy_run_snapshot).pack(side="left", padx=7)
        ttk.Button(actions, text="Close",
                   command=self.diagnostics_window.withdraw).pack(side="right")
        self._set_diagnostics_text(
            "Run diagnostics are available when launched with --run.\n"
            "The framebuffer remains the firmware's unmodified 480×234 image.")

    def show_diagnostics(self) -> None:
        self.diagnostics_window.deiconify()
        self.diagnostics_window.lift()
        self.request_run_diagnostics()

    def _set_diagnostics_text(self, text: str) -> None:
        if self.diagnostics_text is None or not self.diagnostics_text.winfo_exists():
            return
        self.diagnostics_text.configure(state="normal")
        self.diagnostics_text.delete("1.0", "end")
        self.diagnostics_text.insert("1.0", text)
        self.diagnostics_text.configure(state="disabled")

    def request_run_diagnostics(self) -> None:
        """Start at most one bounded observer read; never read files in Tk."""
        if self.run_path is None:
            self._set_diagnostics_text(
                "No run directory was supplied.\n\n"
                "Start nxs_vm with --ui (it passes --run automatically), or "
                "launch view_ui with --run RUN_DIR.")
            return
        if self.diagnostics_executor is None or self.diagnostics_future is not None:
            return
        if self.run_observer is None:
            from tools.cdj_main import run_state
            self.run_observer = run_state.LinkObserver()
        self.diagnostics_future = self.diagnostics_executor.submit(
            observe_run, self.run_path, self.run_observer)

    def poll_run_diagnostics(self) -> None:
        """Apply completed observer work on Tk's thread and schedule another poll."""
        future = self.diagnostics_future
        if future is not None and future.done():
            self.diagnostics_future = None
            try:
                self.run_snapshot = future.result()
                self.diagnostics_error = ""
                self.run_progress.set(self.run_snapshot.get(
                    "progress", "Run observation available"))
                self._set_diagnostics_text(run_diagnostics_text(
                    self.run_snapshot, self.run_snapshot.get("manifest")))
            except Exception as error:  # diagnostic UI must not kill the deck
                self.diagnostics_error = str(error)
                self.run_progress.set("Run diagnostics unavailable: %s" % error)
                self._set_diagnostics_text(
                    "Run diagnostics unavailable: %s\n\n"
                    "The framebuffer viewer is still running." % error)
        self.request_run_diagnostics()
        self.root.after(1000, self.poll_run_diagnostics)

    def copy_run_snapshot(self) -> None:
        """Put a machine-readable bounded snapshot on the clipboard for an agent."""
        value = self.run_snapshot
        text = (json.dumps(value, indent=2, sort_keys=True)
                if value is not None else self.diagnostics_error or
                "No run snapshot has completed yet.")
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self.control_note.set("Copied bounded run diagnostics to clipboard")
        except tk.TclError as error:
            self.control_note.set("Could not copy diagnostics: %s" % error)

    # ------------------------------------------------------------- deck --
    def build_deck(self) -> None:
        """The front panel as a deck: chassis, backlit keys, LCD in the middle.

        The lab layout below is still one flag away (`--skin lab`) and is still
        what the bit-level work wants -- forty keys captioned `18.3` with the
        last verdict beside them is the right window for attributing a bit, and
        the wrong one for finding out whether the machine works.
        """
        self.root.configure(background="#121418")
        outer = tk.Frame(self.root, background="#121418")
        outer.grid(row=0, column=0, sticky="nsew")
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(1, weight=1)
        self.root.geometry("%dx%d" % (
            min(1180, self.root.winfo_screenwidth() - 80),
            min(940, self.root.winfo_screenheight() - 100)))
        self.root.minsize(700, 610)

        header = ttk.Frame(outer, padding=(14, 10, 14, 9))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(1, weight=1)
        title = ttk.Frame(header)
        title.grid(row=0, column=0, sticky="w")
        ttk.Label(title, text="PLAYER / 01",
                  style="Eyebrow.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(title, text=self.args.device_name,
                  style="Title.TLabel").grid(row=1, column=0, sticky="w")
        badges = ttk.Frame(header)
        badges.grid(row=0, column=2, rowspan=2, sticky="e")
        self.zoom_note = tk.StringVar(value="FIT")
        ttk.Label(badges, textvariable=self.zoom_note, style="Badge.TLabel").grid(
            row=0, column=0, padx=(0, 10))
        ttk.Button(badges, text="Inspector", command=self.show_inspector).grid(
            row=0, column=1, padx=4)
        ttk.Button(badges, text="Diagnostics",
                   command=self.show_diagnostics).grid(row=0, column=2, padx=4)
        ttk.Button(badges, text="Controls ?", command=self.show_help).grid(
            row=0, column=3, padx=4)
        ttk.Button(badges, text="Full screen", command=self.toggle_fullscreen).grid(
            row=0, column=4, padx=4)
        if self.args.nxs_panel:
            self.sd_lid_text = tk.StringVar(value="SD lid: unknown")
            ttk.Button(badges, textvariable=self.sd_lid_text,
                       command=lambda: self.sd_lid_command("toggle")).grid(
                           row=1, column=0, columnspan=2, sticky="e")
            self.root.after(1000, self.poll_sd_lid)

        built = controls(self.args.nxs_panel)
        self.by_id = {control.input_id: control for control in built
                      if control.input_id}
        self.viewport = tk.Frame(outer, background="#0b0d10")
        self.viewport.grid(row=1, column=0, sticky="nsew")
        self.deck = faceplate.Faceplate(
            self.viewport, 1,
            resolve=lambda key: self.by_id.get(
                nxs_panel.deck_input(key) if self.args.nxs_panel else key),
            click=self.click,
            rotate=lambda field, delta: self.rotate(
                panel_control.ANALOG_CONTROLS[field], delta),
            long_press=self.long_press, hold=self.toggle_hold,
            contact=self.contact,
            key_contact=lambda c, down: self.contact(c, down, source="deck-keyboard"))
        self.deck.place(relx=0.5, rely=0.5, anchor="center")
        self.resize_timer = None
        self.viewport.bind("<Configure>", self.queue_resize)
        self.root.bind("<Escape>", lambda _e: self.root.attributes("-fullscreen", False))
        # The picture lives on the canvas now; `image_label` stays as the
        # attribute the boot panel and refresh write through so both skins
        # travel the same code path.
        self.image_label = None

        self.inspector = tk.Toplevel(self.root)
        self.inspector.title("CDJ-2000 · Input inspector")
        self.inspector.withdraw()
        self.inspector.protocol("WM_DELETE_WINDOW", self.inspector.withdraw)
        self.inspector.geometry("%dx520" % min(1100, self.root.winfo_screenwidth() - 80))
        self.inspector.columnconfigure(0, weight=1)
        self.inspector.rowconfigure(0, weight=1)
        scroll = tk.Canvas(self.inspector, background="#181b20", highlightthickness=0)
        scroll.grid(row=0, column=0, sticky="nsew")
        for orient, row, col, sticky in [("vertical", 0, 1, "ns"), ("horizontal", 1, 0, "ew")]:
            bar = ttk.Scrollbar(self.inspector, orient=orient,
                                command=scroll.yview if orient == "vertical" else scroll.xview)
            bar.grid(row=row, column=col, sticky=sticky)
            scroll.configure(**{("yscrollcommand" if orient == "vertical" else "xscrollcommand"): bar.set})
        rack = ttk.Frame(scroll, padding=(16, 12))
        scroll.create_window(0, 0, window=rack, anchor="nw")
        rack.bind("<Configure>", lambda _e: scroll.configure(scrollregion=scroll.bbox("all")))
        self.build_rack(rack, built)

        footer = ttk.Frame(outer, padding=(14, 8, 14, 12))
        footer.grid(row=2, column=0, sticky="ew")
        footer.columnconfigure(1, weight=1)
        self.status_light = tk.Canvas(footer, width=14, height=14,
                                      highlightthickness=0,
                                      background="#181b20")
        self.status_light.create_oval(3, 3, 11, 11, fill="#e8bb43",
                                      outline="#e8bb43", tags="light")
        self.status_light.grid(row=0, column=0, sticky="n", pady=(2, 0),
                               padx=(0, 7))
        ttk.Label(footer, textvariable=self.status, wraplength=400).grid(row=0, column=1,
                                                         sticky="w")
        ttk.Label(footer, text="FIRMWARE LCD · 480 × 234", style="Muted.TLabel").grid(
            row=0, column=2, sticky="e", padx=(12, 0))
        ttk.Separator(footer).grid(row=1, column=0, columnspan=3, sticky="ew",
                                   pady=7)
        ttk.Label(footer, textvariable=self.control_note, style="Muted.TLabel",
                  wraplength=650,
                  justify="left").grid(row=2, column=0, columnspan=3,
                                       sticky="w")
        ttk.Label(footer, textvariable=self.run_progress, style="Muted.TLabel",
                  wraplength=900, justify="left").grid(
                      row=3, column=0, columnspan=3, sticky="w", pady=(5, 0))
        self.announce_channel(built)

    def queue_resize(self, _event=None) -> None:
        if self.resize_timer is not None:
            self.root.after_cancel(self.resize_timer)
        self.resize_timer = self.root.after(90, self.fit_deck)

    def fit_deck(self) -> None:
        self.resize_timer = None
        scale = faceplate.fit_scale(self.viewport.winfo_width(), self.viewport.winfo_height())
        self.deck.set_scale(scale)
        self.zoom_note.set("FIT · %d%%" % round(scale * 100))

    def show_inspector(self) -> None:
        self.inspector.deiconify()
        self.inspector.lift()

    def toggle_fullscreen(self) -> None:
        self.root.attributes("-fullscreen", not self.root.attributes("-fullscreen"))

    def show_help(self) -> None:
        from tkinter import messagebox
        messagebox.showinfo("Deck controls", "Hold the mouse down to hold a key; release to let go.\n"
            "Shift-click: long press. Ctrl-click or right-click: latch / release.\n"
            "Browse knob: drag or scroll to turn; click to push.\n"
            "Keyboard: Tab to the deck, arrows to navigate; hold Enter / Space to hold a key.\n"
            "Escape leaves full screen.\n\n"
            "Inspector contains every unassigned input and the channel controls.\n"
            "Lights show host input feedback, not measured hardware LEDs.\n"
            "The jog center is not an emulated jog display. Jog rotation and tempo "
            "are not yet mapped to verified hardware controls.\n\n"
            "Firmware responses can take several seconds. Ordinary button holds "
            "do not queue pulses. Shift-click and UTILITY use timed long presses.",
            parent=self.root)

    def build_rack(self, parent: tk.Misc, built: list[Control]) -> None:
        """Every input the deck does not draw, and the channel's own verbs.

        Defined as the complement of the deck rather than as a second list, so
        an input can never be in neither.  `coverage()` still counts controls,
        not positions, so 48 of 48 means the same thing it did before.
        """
        nxs = self.args.nxs_panel
        board = nxs_panel.input_ids() if nxs else panel_control.input_ids()
        placed = {nxs_panel.deck_input(key) if nxs else key
                  for key in faceplate.PLACEMENTS}
        placed.add("field7")  # the rotary gesture shares the encoder-push knob
        leftover = [key for key in board if key not in placed]
        by_id = {control.input_id: control for control in built
                 if control.input_id}

        bits = [name for name in leftover if "." in name]
        if bits:
            box = ttk.LabelFrame(parent, padding=7, text="Unassigned digital inputs")
            box.grid(row=0, column=0, sticky="w", padx=(0, 10))
            for column, name in enumerate(bits):
                label = nxs_panel.KEY_NAMES.get(tuple(map(int, name.split('.'))), name) if nxs else name
                ttk.Button(box, text=label, width=max(6, len(label)),
                           command=lambda n=name: self.click(by_id[n])).grid(
                               row=0, column=column, padx=2)

        fields = [name for name in leftover if name.startswith("field")]
        if fields:
            box = ttk.LabelFrame(parent, padding=7, text="Unassigned analogue inputs · hardware mapping unverified")
            box.grid(row=1, column=0, sticky="w", pady=12)
            wanted = {int(name[5:]) for name in fields}
            self.build_analog(box, only=wanted)

        box = ttk.LabelFrame(parent, padding=7, text="CONTROL CHANNEL")
        box.grid(row=2, column=0, sticky="nw")
        for column, control in enumerate(channel_controls()):
            ttk.Button(box, text=control.label.upper(), width=11,
                       style="Quiet.TButton",
                       command=lambda c=control: self.send(c, c.lines[0])
                       ).grid(row=0, column=column, padx=4)

    def announce_channel(self, built: list[Control]) -> None:
        channel_note = ("control channel on 127.0.0.1:%d"
                        % self.args.control_port if self.args.control_port
                        else "no control channel: every control will refuse "
                             "(start with --control-port)")
        self.control_note.set("%s | %s" % (coverage_line(built, self.args.nxs_panel), channel_note))

    # -------------------------------------------------------------- lab --
    def build_lab(self) -> None:
        """Front-panel geometry: buttons around the picture, never on it."""
        outer = ttk.Frame(self.root, padding=10)
        outer.grid(row=0, column=0, sticky="nsew")

        built = controls(self.args.nxs_panel)
        by_group: dict[str, list[Control]] = {}
        for control in built:
            by_group.setdefault(control.group, []).append(control)

        # BROWSE / TAG LIST / INFO / MENU, above the panel.
        top = ttk.Frame(outer)
        top.grid(row=LAYOUT["top"][0], column=LAYOUT["top"][1], sticky="w",
                 pady=(0, 6))
        for index, control in enumerate(by_group["top"]):
            self.hardware_button(top, control).grid(row=0, column=index,
                                                    padx=(0, 6))

        # LINK / USB / SD / DISC, left of the panel.
        left = ttk.Frame(outer)
        left.grid(row=LAYOUT["left"][0], column=LAYOUT["left"][1], sticky="n",
                  padx=(0, 8))
        for index, control in enumerate(by_group["left"]):
            self.hardware_button(left, control).grid(row=index, column=0,
                                                     pady=(0, 6), sticky="ew")

        # The LCD, and nothing else.
        self.image_label = ttk.Label(outer, borderwidth=0)
        self.image_label.grid(row=LAYOUT["panel"][0], column=LAYOUT["panel"][1],
                              sticky="nw")

        # The raw bits beside the panel, the analogue fields below it, and the
        # channel's own verbs under those.
        side = ttk.Frame(outer)
        side.grid(row=LAYOUT["side"][0], column=LAYOUT["side"][1], sticky="n",
                  padx=(10, 0))
        self.build_bit_grid(side, by_group["bits"])

        analog = ttk.Frame(outer)
        analog.grid(row=LAYOUT["analog"][0], column=LAYOUT["analog"][1],
                    columnspan=2, sticky="w", pady=(10, 0))
        self.build_analog(analog)

        channel = ttk.Frame(outer)
        channel.grid(row=LAYOUT["channel"][0], column=LAYOUT["channel"][1],
                     columnspan=2, sticky="w", pady=(8, 0))
        for index, control in enumerate(by_group["channel"]):
            ttk.Button(channel, text=control.label, width=12,
                       command=lambda c=control: self.send(c, c.lines[0])
                       ).grid(row=0, column=index, padx=(0, 6))
        ttk.Button(channel, text="Diagnostics", width=12,
                   command=self.show_diagnostics).grid(
                       row=0, column=len(by_group["channel"]), padx=(12, 0))

        ttk.Label(outer, textvariable=self.status).grid(
            row=LAYOUT["status"][0], column=LAYOUT["status"][1], columnspan=3,
            sticky="w", pady=(8, 0))
        ttk.Label(outer, textvariable=self.control_note,
                  foreground="#804000").grid(row=LAYOUT["note"][0],
                                             column=LAYOUT["note"][1],
                                             columnspan=3, sticky="w")
        ttk.Label(outer, textvariable=self.run_progress,
                  foreground="#5c78a2", wraplength=980, justify="left").grid(
                      row=LAYOUT["note"][0] + 1, column=LAYOUT["note"][1],
                      columnspan=3, sticky="w", pady=(5, 0))
        # The window's own coverage, on the window.  If a control is ever lost
        # again, this line is where it shows up rather than in a report six
        # weeks later.
        channel_note = ("control channel on 127.0.0.1:%d"
                        % self.args.control_port if self.args.control_port
                        else "no control channel: every control will refuse "
                             "(start with --control-port)")
        self.control_note.set("%s | %s" % (coverage_line(built, self.args.nxs_panel), channel_note))

    def hardware_button(self, parent: tk.Misc, control: Control) -> ttk.Button:
        """A front-panel key.

        Enabled even when it cannot send, because the click is how the reason
        gets said.  `refusal` decides; the style says which ones will refuse
        before anybody clicks.
        """
        button = ttk.Button(parent, text=button_text(control), width=11,
                            command=lambda: self.click(control))
        self.bind_hardware_contact(button, control)
        # Shift-click holds the key long enough to count as held (UTILITY on
        # MENU); "break" keeps the plain click from firing on top of it.
        button.bind("<Shift-Button-1>",
                    lambda _event, c=control: self.long_press(c) or "break")
        if control.input_id is None:
            button.configure(style="Unbound.TButton")
        return button

    def bind_hardware_contact(self, button: ttk.Button, control: Control) -> None:
        """Inspector/lab buttons use the same contact contract as the deck."""
        if control.input_id is None or control.kind != "button":
            return
        active: set[str] = set()
        releases: dict[str, str] = {}

        def paint():
            if button.winfo_exists():
                button.state(["pressed" if active else "!pressed"])

        def release(token):
            timer = releases.pop(token, None)
            if timer is not None:
                button.after_cancel(timer)
            if token in active:
                self.contact(control, False, source=(str(button), token))
                active.discard(token)
            paint()
            return "break"

        def down(token):
            timer = releases.pop(token, None)
            if timer is not None:
                button.after_cancel(timer)
            if token not in active and self.contact(control, True, source=(str(button), token)):
                active.add(token)
                button.focus_set()
            paint()
            return "break"

        def key_up(token):
            if token in active and token not in releases:
                releases[token] = button.after_idle(lambda: release(token))
            return "break"

        def cancel(_event):
            for token in tuple(active):
                release(token)

        button.bind("<ButtonPress-1>", lambda _e: down("pointer"))
        button.bind("<ButtonRelease-1>", lambda _e: release("pointer"))
        for key in ("Return", "space"):
            button.bind(f"<KeyPress-{key}>", lambda _e, k=key: down(k))
            button.bind(f"<KeyRelease-{key}>", lambda _e, k=key: key_up(k))
        button.bind("<FocusOut>", cancel, add="+")
        button.bind("<Destroy>", cancel, add="+")

    def build_bit_grid(self, parent: tk.Misc, bits: list[Control]) -> None:
        """Every decoded payload bit, with MAIN's name and the last verdict.

        The name comes first because it is the stronger of the two statements:
        a verdict describes one run on one screen, while `PLAY` is what the
        firmware itself calls that bit on every run there will ever be.  For
        the four SOURCE keys the two disagreed for weeks and only the run-shaped
        one was on display.
        """
        verdicts = manifest_verdicts()
        box = ttk.LabelFrame(parent, text="panel bits — MAIN's own names, then "
                                          "INPUT_MANIFEST.md; Shift-click is a "
                                          "long press, right-click holds a bit "
                                          "down",
                             padding=6)
        box.grid(row=0, column=0, sticky="ew")
        # Two columns since the inventory went from 22 bits to 40: one column of
        # 40 rows is taller than the panel it sits beside, and a control you
        # have to scroll to is one nobody presses.
        per_column = (len(bits) + 1) // 2
        for index, control in enumerate(bits):
            row = index % per_column
            column = 2 * (index // per_column)
            button = ttk.Button(box, text=control.label, width=6,
                                command=lambda c=control: self.click(c))
            self.bind_hardware_contact(button, control)
            button.bind("<Button-3>",
                        lambda _event, c=control: self.toggle_hold(c))
            button.bind("<Shift-Button-1>",
                        lambda _event, c=control: self.long_press(c) or "break")
            button.grid(row=row, column=column, pady=1)
            verdict, world = verdicts.get(control.input_id, ("", ""))
            # The run name is part of the finding, not decoration: 18.1 is
            # "changes the display" in r026 and 0 in r096, on different screens.
            byte, bit = (int(part) for part in control.input_id.split("."))
            if self.args.nxs_panel:
                name = nxs_panel.KEY_NAMES.get((byte, bit), "")
                verdict, world = "", ""  # legacy run annotations are not NXS evidence
            else:
                name = firmware_name(byte, bit)
            shown = "%s  [%s]" % (verdict[:30], world) if world else verdict[:30]
            ttk.Label(box, text="%-13s %s" % (name, shown),
                      foreground="#555555").grid(
                row=row, column=column + 1, sticky="w", padx=(6, 12))

    def build_analog(self, parent: tk.Misc,
                     only: set[int] | None = None) -> None:
        """One row per analogue field: detents, a drag, and an exact value.

        A jog wheel, a fader and a detented encoder are not buttons and cannot
        be driven by one.  Each row offers all three gestures because which one
        the firmware wants is, for six of the eight fields, still unmeasured --
        and offering only the gesture that happens to be implemented is how the
        encoder went unturned for months.
        """
        verdicts = manifest_verdicts()
        if only is None:
            box = ttk.LabelFrame(parent, text="analogue fields (payload 2..14)",
                                 padding=6)
            box.grid(row=0, column=0, sticky="ew")
        else:
            box = parent
        wanted = [entry for entry in panel_control.ANALOG_CONTROLS
                  if only is None or entry.field in only]
        for row, entry in enumerate(wanted):
            # Two variables rather than one shared between the slider and the
            # box.  Tk's scale insists its variable holds a number, so a box
            # bound to the same variable turns a half-typed value into a
            # background error inside Tk -- which lands in a console nobody is
            # reading, i.e. exactly the silence this window is being cured of.
            self.analog_value[entry.field] = tk.StringVar(value=str(entry.low))
            position = tk.DoubleVar(value=float(entry.low))
            self.analog_position[entry.field] = position

            ttk.Label(box, text=entry.label, width=34).grid(
                row=row, column=0, sticky="w", pady=1)
            ttk.Button(box, text="◀ −%d" % entry.step, width=8,
                       command=lambda e=entry: self.rotate(e, -e.step)).grid(
                           row=row, column=1, padx=(0, 2))
            ttk.Button(box, text="+%d ▶" % entry.step, width=8,
                       command=lambda e=entry: self.rotate(e, e.step)).grid(
                           row=row, column=2, padx=(0, 6))
            scale = ttk.Scale(box, from_=entry.low, to=entry.high, length=160,
                              variable=position)
            # On release, not on every pixel of the drag: the ramp walks one
            # count per panel exchange, so a command per pixel would queue a
            # thousand targets to reach the one the finger stopped on.
            scale.bind("<ButtonRelease-1>",
                       lambda _event, e=entry: self.drag(e))
            scale.grid(row=row, column=3, padx=(0, 6))
            ttk.Entry(box, textvariable=self.analog_value[entry.field],
                      width=8).grid(row=row, column=4, padx=(0, 4))
            ttk.Button(box, text="set", width=5,
                       command=lambda e=entry: self.set_analog(e)).grid(
                           row=row, column=5, padx=(0, 6))
            if entry.touch_mask:
                touch = tk.BooleanVar(value=False)
                self.analog_touch[entry.field] = touch
                ttk.Checkbutton(box, text="touch (bit %d)"
                                % (entry.touch_mask.bit_length() - 1),
                                variable=touch,
                                command=lambda e=entry: self.set_analog(e)
                                ).grid(row=row, column=6, sticky="w")
            verdict, world = verdicts.get("field%d" % entry.field, ("", ""))
            shown = "%s  [%s]" % (verdict[:34], world) if world else ""
            ttk.Label(box, text=shown, foreground="#555555").grid(
                row=row, column=7, sticky="w", padx=(6, 0))

    # ------------------------------------------------------------ control --
    def record_ui_action(self, control: Control, line: str, *, ok: bool,
                         reply: str | None = None,
                         error: object | None = None) -> None:
        """Append one command outcome to the run transcript for agents."""
        run_path = getattr(self, "run_path", None)
        if run_path is None:
            return
        try:
            from tools.cdj_main.run_state import record_action
            action = dict(source="view_ui", label=control.label,
                          input_id=control.input_id, command=line.strip(), ok=ok)
            if reply is not None:
                action["reply"] = reply
            if error is not None:
                action["error"] = str(error)
            record_action(run_path, action)
        except (OSError, ImportError, TypeError, ValueError):
            # Logging is best effort; the command result remains authoritative.
            return

    def control(self) -> panel_control.PanelControl | None:
        """The control channel, opened on first use and reopened after a drop.

        Lazily, because QEMU is started alongside this window and the port is
        not listening yet when the layout is built.
        """
        if not self.args.control_port:
            self.last_control_error = "no control channel"
            return None
        if self.panel is not None:
            self.last_control_error = None
            return self.panel
        try:
            panel = panel_control.PanelControl(port=self.args.control_port,
                                               timeout=0.5)
            greeting = panel.open()
        except OSError as error:
            self.last_control_error = str(error)
            self.control_note.set("control channel not up yet: %s" % error)
            return None
        self.panel = panel
        self.last_control_error = None
        self.control_note.set("control channel: %s" % greeting)
        return panel

    def forget_control(self, error: object) -> None:
        if self.panel is not None:
            self.panel.close()
            self.panel = None
        self.control_note.set("control channel lost: %s" % error)
        self.last_control_error = str(error)

    def send(self, control: Control, line: str) -> str | None:
        """One protocol line, or a spoken reason why not.

        Every click in this window ends here, so there is exactly one place
        where a refusal can be swallowed -- and it does not swallow one.
        """
        reason = refusal(control, self.args.control_port)
        if reason:
            self.control_note.set("refused — " + reason)
            self.record_ui_action(control, line, ok=False, error=reason)
            return None
        if not line.strip():
            # Unreachable by construction -- the only controls with no line are
            # the unbound keys, and `refusal` has already turned those away.
            # It is here so that "sends nothing" can never become "sends an
            # empty line and looks like it worked".
            self.control_note.set("refused — %s has nothing to send"
                                  % control.label)
            self.record_ui_action(control, line, ok=False,
                                  error="control has no command line")
            return None
        panel = self.control()
        if panel is None:
            note = getattr(self, "control_note", None)
            note_text = (note.get() if note is not None and hasattr(note, "get")
                         else "channel unavailable")
            self.record_ui_action(
                control, line, ok=False,
                error=getattr(self, "last_control_error", None) or
                note_text)
            return None
        try:
            reply = panel.send(line)
        except (OSError, ValueError) as error:
            self.forget_control(error)
            self.record_ui_action(control, line, ok=False, error=error)
            return None
        ok = protocol_ok(reply)
        self.record_ui_action(control, line, ok=ok, reply=reply,
                              error=None if ok else reply)
        self.control_note.set("%s: %s -> %s"
                              % (control.label, line.strip(), reply))
        if line.strip() == "clear" and ok:
            self.held.clear()
            self.momentary.clear()
            self.contact_sources.clear()
            self.in_flight.clear()
            if self.deck is not None:
                for name in list(self.deck.latched):
                    self.deck.set_latched(name, False)
        return reply if ok else None

    def contact_line(self, control: Control, active: bool) -> str:
        """Encode one logical contact level for this board profile.

        NXS REV is an active-low physical switch.  Its raw panel bit must be
        high at rest and low while active, so it cannot use the ordinary
        OR-only ``down``/``up`` protocol.  Keep this translation at the GUI
        boundary: the bit inspector sends literal electrical levels.
        """
        bit_id = control.input_id.split("-")[0]
        byte, mask = panel_control.button_mask(bit_id)
        if self.is_nxs_reverse(control):
            raw_high = nxs_panel.contact_level(byte, mask, active)
            return panel_control.encode_level(byte, mask, raw_high)
        if self.is_nxs_reverse_raw(control):
            # The inspector deliberately exposes the electrical level: its
            # down/up verbs mean high/low, with no active-low interpretation.
            return panel_control.encode_level(byte, mask, active)
        return panel_control.encode_hold(byte, mask, active)

    def is_nxs_reverse(self, control: Control) -> bool:
        """Whether a control is the NXS active-low REV switch."""
        return (getattr(getattr(self, "args", None), "nxs_panel", False) and
                control.input_id is not None and
                control.input_id.split("-")[0] == "15.1" and
                # The bit grid is a raw inspector.  It must retain the
                # literal electrical level so it can diagnose the inversion.
                control.group != "bits")

    def is_nxs_reverse_raw(self, control: Control) -> bool:
        """Whether an NXS REV control belongs to the literal bit inspector."""
        return (getattr(getattr(self, "args", None), "nxs_panel", False) and
                control.input_id is not None and
                control.input_id.split("-")[0] == "15.1" and
                control.group == "bits")

    def neutral_line(self, control: Control) -> str:
        """Release a contact to the board profile's electrical neutral."""
        bit_id = control.input_id.split("-")[0]
        byte, mask = panel_control.button_mask(bit_id)
        if (getattr(getattr(self, "args", None), "nxs_panel", False) and
                bit_id == "15.1"):
            return panel_control.encode_level(
                byte, mask, nxs_panel.contact_level(byte, mask, False))
        return panel_control.encode_hold(byte, mask, False)

    def contact(self, control: Control, down: bool, *, source: object = "deck-pointer") -> bool:
        """Combine pointer/keyboard/widget ownership, independently of latches."""
        if self.is_sd_lid(control):
            sources = getattr(self, "sd_lid_sources", set())
            if down and source not in sources:
                if not self.sd_lid_command("toggle"):
                    return False
                sources.add(source)
            elif not down:
                sources.discard(source)
            self.sd_lid_sources = sources
            return True
        if control.input_id is None or control.kind not in ("button", "hold"):
            self.click(control)
            return False
        bit_id = control.input_id.split("-")[0]
        byte, mask = panel_control.button_mask(bit_id)
        key = (byte, mask)
        owners = self.contact_sources.get(key, set())
        if down == (source in owners):
            return True
        next_owners = owners | {source} if down else owners - {source}
        # A release must not undo another active contact or an explicit latch.
        if key not in self.held and bool(owners) != bool(next_owners):
            if self.send(control, self.contact_line(control, down)) is None:
                return False
        if next_owners:
            self.contact_sources[key] = next_owners
            self.momentary[key] = control
        else:
            self.contact_sources.pop(key, None)
            self.momentary.pop(key, None)
        self.show_contact(bit_id, key in self.held or bool(next_owners))
        return True

    def show_contact(self, bit_id: str, down: bool) -> None:
        if getattr(getattr(self, "args", None), "nxs_panel", False):
            bit_id = next((key for key in faceplate.PLACEMENTS
                           if nxs_panel.deck_input(key) == bit_id), None)
        if self.deck is not None and bit_id in faceplate.PLACEMENTS:
            self.deck.set_latched(bit_id, down)
            if bit_id == "20.3":
                self.deck.set_latched("20.3-hold", down)

    def click(self, control: Control) -> None:
        if self.is_sd_lid(control):
            self.sd_lid_command("toggle")
            return
        if control.kind == "hold" and control.input_id is not None:
            self.long_press(control)
            return
        if control.kind == "button" and control.input_id is not None:
            if getattr(self.args, "nxs_panel", False):
                self.press(control, NXS_CLICK_HOLD_MS,
                           "observe browser status and the framebuffer")
            else:
                self.press(control, WINDOW_HOLD_MS,
                           "the screen follows about 5 s after the click")
            return
        if control.lines:
            self.send(control, control.lines[0])
        else:
            self.send(control, "")

    def long_press(self, control: Control) -> None:
        """Shift-click, or the UTILITY key, using an explicit long pulse."""
        if self.is_sd_lid(control):
            self.sd_lid_command("toggle")
            return
        if control.kind not in ("button", "hold") or control.input_id is None:
            self.click(control)
            return
        if getattr(self.args, "nxs_panel", False):
            self.press(control, WINDOW_LONG_HOLD_MS,
                       "an explicit long pulse; observe browser feedback")
        else:
            self.press(control, WINDOW_LONG_HOLD_MS,
                       "a long press, held across two of MAIN's 3 s status "
                       "records; UTILITY on MENU follows a few seconds after "
                       "release")

    def press(self, control: Control, hold_ms: int, then: str) -> None:
        """One press of `hold_ms`, refused while the previous one is down."""
        now = time.monotonic()
        # "20.3-hold" is the same key as "20.3"; a press of either is a press
        # of the bit, and the other must wait for it too.
        bit_id = control.input_id.split("-")[0]
        over = self.in_flight.get(bit_id, 0.0)
        if now < over:
            if getattr(self.args, "nxs_panel", False):
                self.control_note.set(
                    "%s: previous pulse still in flight for %.1f s; "
                    "observe browser status before sending another"
                    % (control.label, over - now))
            else:
                self.control_note.set(
                    "%s: press still down for %.1f s -- MAIN samples it into "
                    "its next status record and the screen follows a few "
                    "seconds later; a second press would undo a toggle like "
                    "MENU" % (control.label, over - now))
            return
        byte, mask = panel_control.button_mask(bit_id)
        if (self.is_nxs_reverse(control) or
                self.is_nxs_reverse_raw(control)):
            # ``press`` is implemented as a timed level for REV; the generic
            # panel ``press`` command can only assert bits.
            if self.send(control, self.contact_line(control, True)) is None:
                return
            key = (byte, mask)
            # Keep a pending pulse in the same ownership set used by close().
            # Otherwise closing the viewer before the timer fires would leave
            # the active-low contact asserted in MAIN.
            self.momentary[key] = control
            self.in_flight[bit_id] = now + hold_ms / 1000.0

            def release() -> None:
                if key in self.held or key in self.contact_sources:
                    return
                if self.send(control, self.contact_line(control, False)) is not None:
                    self.momentary.pop(key, None)

            self.root.after(hold_ms, release)
            if getattr(self.args, "nxs_panel", False):
                self.control_note.set("%s: queued %.0f ms pulse; %s"
                                      % (control.label, hold_ms, then))
            else:
                self.control_note.set("%s: held %.1f s; %s"
                                      % (control.label, hold_ms / 1000.0, then))
            return
        if self.send(control, panel_control.encode_press(byte, mask,
                                                         hold_ms)) is not None:
            self.in_flight[bit_id] = (
                now + panel_control.press_period_s(hold_ms))
            if getattr(self.args, "nxs_panel", False):
                self.control_note.set("%s: queued %.0f ms pulse; %s"
                                      % (control.label, hold_ms, then))
            else:
                self.control_note.set("%s: held %.1f s; %s"
                                      % (control.label, hold_ms / 1000.0, then))

    def toggle_hold(self, control: Control) -> None:
        """Right-click: hold the bit down, right-click again to release it."""
        if self.is_sd_lid(control):
            self.sd_lid_command("toggle")
            return
        if control.input_id is None:
            self.click(control)
            return
        bit_id = control.input_id.split("-")[0]
        byte, mask = panel_control.button_mask(bit_id)
        key = (byte, mask)
        down = key not in self.held
        if (key in self.momentary or
                self.send(control, self.contact_line(control, down)) is not None):
            if down:
                self.held[key] = control
            else:
                self.held.pop(key, None)
            self.show_contact(bit_id, down or key in self.momentary)
            self.control_note.set("%s %s (held: %s)"
                                  % (control.label, "held down" if down
                                     else "released",
                                     ", ".join(sorted(c.label for c
                                                      in self.held.values()))
                                     or "none"))

    def is_sd_lid(self, control: Control) -> bool:
        return (getattr(getattr(self, "args", None), "nxs_panel", False) and
                control.input_id == "17.2")

    def sd_lid_command(self, action: str, *, background: bool = False) -> bool:
        control = Control("SD lid", "17.2", "switch", "bits", (),
                          "Persistent physical SD lid contact; click to toggle")
        logged = False
        probe_ok = False
        if action == "state":
            # Keep the state probe separate from the input-send path: tests and
            # callers may provide a lightweight PanelControl mock, while the
            # command still belongs in the shared action transcript.  A
            # background poll records its first good value and changes, while
            # explicit user requests are always retained.
            panel = self.control()
            if panel is None:
                reply = None
                self.record_ui_action(
                    control, "sd-lid state\n", ok=False,
                    error=getattr(self, "last_control_error", None) or
                    getattr(self, "control_note", "channel unavailable"))
                logged = True
            else:
                try:
                    reply = panel.send("sd-lid state\n")
                except (OSError, ValueError) as error:
                    self.forget_control(error)
                    self.record_ui_action(control, "sd-lid state\n", ok=False,
                                          error=error)
                    reply = None
                    logged = True
                else:
                    probe_ok = protocol_ok(reply)
        else:
            reply = self.send(control, "sd-lid " + action + "\n")
        state = "unknown"
        if protocol_ok(reply):
            tokens = reply.strip().split()
            if len(tokens) == 3 and tokens[1].lower() == "sd-lid":
                state = tokens[2].lower()
        if state not in ("open", "closed"):
            state = "unknown"
        if action == "state" and not logged:
            valid = probe_ok and state in ("open", "closed")
            previous = getattr(self, "_last_sd_lid_poll_state", None)
            if not background or not valid or previous != state:
                self.record_ui_action(
                    control, "sd-lid state\n", ok=valid,
                    reply=reply, error=None if valid else reply)
            if background and valid:
                self._last_sd_lid_poll_state = state
        if hasattr(self, "sd_lid_text"):
            self.sd_lid_text.set("SD lid: " + state)
        if reply and action != "state":
            self.control_note.set("SD lid: " + state + " (persistent physical switch)")
        return state in ("open", "closed")

    def poll_sd_lid(self) -> None:
        self.sd_lid_command("state", background=True)
        self.root.after(2000, self.poll_sd_lid)

    def field_value(self, entry: panel_control.AnalogControl) -> int | None:
        """The number in the row's box, or None with the reason said out loud.

        Base 0, so `0x8000` is typeable -- which matters for exactly one field:
        the touch flag lives at bit 15 of the bytes 12/13 pair and a decimal
        32768 reads like a position that is out of range.

        A box that does not hold a number used to be a silent click.  `IntVar`
        was bound straight to the widget and `IntVar.get()` raises `TclError`,
        which is not a `ValueError` and so escaped into Tk's own handler: the
        window did nothing and said nothing, which is the failure this whole
        pass is about.
        """
        text = self.analog_value[entry.field].get().strip()
        try:
            value = int(text, 0)
        except (tk.TclError, ValueError):
            self.control_note.set(
                "refused — %s: %r is not a number" % (entry.label, text))
            return None
        limit = entry.high | entry.touch_mask
        if not entry.low <= value <= limit:
            self.control_note.set(
                "refused — %s: %d is outside %d..%d, which is what the decoder "
                "keeps of this field" % (entry.label, value, entry.low, limit))
            return None
        return value

    def drag(self, entry: panel_control.AnalogControl) -> None:
        """The slider was let go: copy where it stopped into the box and send."""
        self.analog_value[entry.field].set(
            str(int(round(self.analog_position[entry.field].get()))))
        self.set_analog(entry)

    def analog_control(self, entry: panel_control.AnalogControl) -> Control:
        for control in analog_controls():
            if control.input_id == "field%d" % entry.field:
                return control
        raise KeyError(entry.field)

    def rotate(self, entry: panel_control.AnalogControl, delta: int) -> None:
        """One detent, as an encoder: the board walks a count per exchange."""
        self.send(self.analog_control(entry),
                  panel_control.encode_rotary(entry.field, delta))

    def set_analog(self, entry: panel_control.AnalogControl) -> None:
        """The slider, the value box and the touch flag all land here."""
        value = self.field_value(entry)
        if value is None:
            return
        touch = self.analog_touch.get(entry.field)
        if touch is not None and touch.get():
            value |= entry.touch_mask
        self.send(self.analog_control(entry),
                  panel_control.encode_analog(entry.field, value))

    # ------------------------------------------------------------ picture --
    def scaled(self, frame: Image.Image) -> Image.Image:
        """Crop the blanking away, then zoom by whole pixels."""
        frame = crop_panel(frame)
        if self.args.scale != 1:
            frame = frame.resize(
                (frame.width * self.args.scale, frame.height * self.args.scale),
                Image.Resampling.NEAREST,
            )
        return frame

    def show_panel(self, frame: Image.Image) -> None:
        """Put one 480x234 panel frame on screen, whichever skin is up.

        The deck pastes into a Tk image it already owns; the lab layout builds
        a new one because its label has no other way to take a picture.  The
        difference is 5.2 ms against 8.0 ms per frame *and* 1.8 MB of Tk image
        churn per frame that the deck no longer does.
        """
        if self.deck is not None:
            self.deck.set_frame(crop_panel(frame))
            return
        self.photo = ImageTk.PhotoImage(self.scaled(frame))
        self.image_label.configure(image=self.photo)

    def show_boot_panel(self) -> None:
        frame = Image.new("RGB", (PANEL_WIDTH, PANEL_HEIGHT), (8, 12, 20))
        draw = ImageDraw.Draw(frame)
        draw.rectangle((16, 16, 463, 217), outline=(35, 95, 210), width=2)
        draw.text((174, 100), "CDJ-2000 GUI firmware", fill=(100, 160, 255))
        draw.text((185, 121), "Booting Blackfin CPU...", fill=(205, 215, 230))
        self.show_panel(frame)

    def start_simulator(self) -> None:
        if self.args.attach:
            self.status.set("Attached viewer · waiting for a firmware frame")
            return
        output = self.args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.unlink(missing_ok=True)

        packet = self.args.packet.resolve()
        if not packet.exists():
            raise FileNotFoundError(f"MAIN record stream does not exist: {packet}")

        log_path = self.args.log.resolve()
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_stream = log_path.open("wb")

        env = os.environ.copy()
        env.update(
            {
                "BFIN_GUI_OUTPUT": str(output),
                "BFIN_GUI_HEIGHT": str(self.args.height),
                "BFIN_GUI_COLOR": "rgb555le",
                "BFIN_PARALLEL_WRITEBACK": "1",
                "BFIN_FAST_LZSS": str(
                    (FIRMWARE / "gui-flash-image.bin").resolve()
                ),
                "BFIN_GPIO5_READY_TOGGLE": "1",
                "BFIN_SPORT_RX_INPUT": str(packet),
                "BFIN_SPORT_RX_RECORDS": "1",
                "BFIN_SPORT_RX_ZERO_200": "1",
                "BFIN_SPORT_TX_OUTPUT": str(self.args.tx_output.resolve()),
            }
        )
        # --env wins over the defaults above, so a caller can replace the
        # replayed record stream with a live link to the MAIN board.
        for setting in self.args.env:
            name, _, value = setting.partition("=")
            if value == "":
                env.pop(name, None)
            else:
                env[name] = value
        command = [
            simulator_path(self.args.simulator),
            "--model",
            "bf531",
            "--environment",
            "operating",
            "--memory-region",
            "0,64M",
            "--hw-board-file",
            board_path(self.args.board),
            simulator_path(self.args.elf),
        ]
        creationflags = ((subprocess.CREATE_NO_WINDOW
                          | subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
                         if sys.platform == "win32" else 0)
        self.process = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=env,
            # No console for the simulator: the UART model polls stdin
            # (bfin_uart_get_status -> sim_io_poll_read), and on MinGW that
            # read blocks inside the run loop if a console is attached.
            stdin=subprocess.DEVNULL,
            stdout=self.log_stream,
            stderr=subprocess.STDOUT,
            creationflags=creationflags,
        )

    def refresh(self) -> None:
        """Show the newest published frame, and say how many are being lost.

        **The 100 ms this used to sleep was throwing away most of the picture.**
        Measured 2026-08-07 on this machine: the GUI board completes 41 frames
        per second on its own and 32 with QEMU and the live link attached, while
        one whole pass of this method -- open, convert, crop, zoom, hand to Tk --
        costs 8.0 ms.  A tenth of a second between passes was never a cost of
        emulation; it was three frames in four dropped by the viewer, and the
        board was never the reason the window looked slow.

        The counters below exist so that claim keeps being checked at runtime
        rather than resting on one afternoon's measurement: `shown` is what you
        see, `published` is what the simulator put on disk, and if they diverge
        the status line says so instead of leaving "it feels slow" to be blamed
        on the emulator again.
        """
        if self.process is None and not self.args.attach:
            return
        # Schedule first, so a slow pass shortens the next gap instead of
        # adding to it.  The old code re-armed only after the work and after
        # every early return, which made the effective interval drift upwards
        # under exactly the load that matters.
        self.root.after(self.args.refresh_ms, self.refresh)

        return_code = self.process.poll() if self.process is not None else None
        if return_code is not None:
            self.status.set(f"Simulator exited with code {return_code}; "
                            f"see {self.args.log}")
            self.stopped = True
            return

        try:
            mtime_ns = self.args.output.stat().st_mtime_ns
            age_note = publication_age_note(mtime_ns, time.time())
            if mtime_ns == self.last_mtime_ns:
                if age_note:
                    self.status.set(age_note)
                    self.fps = 0.0
                    self.shown = self.published = 0
                    self.rate_since = time.monotonic()
                return
            with Image.open(self.args.output) as source:
                frame = source.convert("RGB")
                source_size = source.size
            self.last_mtime_ns = mtime_ns
            self.published += 1
            if not self.has_firmware_picture and frame.getbbox() is None:
                elapsed = int(time.monotonic() - self.boot_started)
                self.status.set(
                    f"Firmware booting — {elapsed}s elapsed; "
                    f"first frame usually appears within 10s")
                return
            self.has_firmware_picture = True
            self.show_panel(frame)
            self.shown += 1
        except (FileNotFoundError, OSError):
            # A frame caught mid-publish, or the rename losing a race with this
            # read.  Both are single dropped frames at 30 fps, not errors.
            if time.monotonic() - self.boot_started >= 5:
                self.status.set("Waiting for a complete framebuffer — "
                                "emulator liveness unverified")
            return

        now = time.monotonic()
        if now - self.rate_since >= 1.0:
            self.fps = self.shown / (now - self.rate_since)
            self.shown = self.published = 0
            self.rate_since = now
        self.status.set(age_note or
            f"{self.fps:4.1f} fps — {source_size[0]}×{source_size[1]} captured, "
            f"shown as {PANEL_WIDTH}×{PANEL_HEIGHT} at "
            f"{self.deck.scale if self.deck is not None else self.args.scale:.2f}x "
            f"(polling every {self.args.refresh_ms} ms)")

    def close(self) -> None:
        # In attach mode the machine outlives this window. Release only our
        # contacts, not analog values or queued input from another controller.
        for (byte, mask), control in (self.held | self.momentary).items():
            self.send(control, self.neutral_line(control))
        # Destroy/FocusOut callbacks must not reconnect after transport close.
        self.held.clear()
        self.momentary.clear()
        self.contact_sources.clear()
        if self.panel is not None:
            self.panel.close()
            self.panel = None
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.log_stream is not None:
            self.log_stream.close()
        diagnostics_executor = getattr(self, "diagnostics_executor", None)
        if diagnostics_executor is not None:
            diagnostics_executor.shutdown(wait=False, cancel_futures=True)
            self.diagnostics_executor = None
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--simulator",
        type=Path,
        default=BFIN_SIM,
    )
    parser.add_argument("--elf", type=Path, default=FIRMWARE / "gui-boot-memory.elf")
    parser.add_argument(
        "--board", type=Path, default=BOARDS / "cdj2000-gui.hw"
    )
    parser.add_argument(
        "--packet",
        type=Path,
        default=PACKETS / "main-records-neutral-runtime.bin",
    )
    parser.add_argument("--output", type=Path, default=RUNS / "cdj-screen-live.ppm")
    parser.add_argument("--tx-output", type=Path, default=RUNS / "sport-tx-live.bin")
    parser.add_argument("--log", type=Path, default=RUNS / "ui-sim.log")
    parser.add_argument("--height", type=int, default=CAPTURE_HEIGHT,
                        help="lines to capture; the PPI emits 255 and the frame "
                             "is cropped to 234, because capturing 234 wraps at "
                             "the wrong point")
    parser.add_argument("--scale", type=int, default=2,
                        help="integer LCD scale in lab view; device view fits the window")
    parser.add_argument("--attach", action="store_true",
                        help="watch --output without starting or stopping a simulator")
    parser.add_argument("--run", type=Path, default=None,
                        help="run directory to observe and record UI actions; "
                             "nxs_vm --ui supplies this automatically")
    parser.add_argument("--device-name", default="CDJ-2000",
                        help="device label in the window header")
    parser.add_argument("--nxs-panel", action="store_true",
                        help="use verified NXS persistent SD-lid contact semantics")
    parser.add_argument("--skin", choices=("device", "lab"), default="device",
                        help="'device' draws the CDJ-2000 front panel around "
                             "the picture; 'lab' is the bit-level window, "
                             "which is still the right one for attributing a "
                             "payload bit")
    parser.add_argument("--refresh-ms", type=int, default=25,
                        help="how often to look for a new frame. One pass "
                             "costs ~8 ms and the board publishes ~32 fps "
                             "under the live link, so the old 100 ms dropped "
                             "roughly three frames in four")
    parser.add_argument("--ppi-delay", type=int, default=0,
                        help="ticks per display scanline (BFIN_PPI_DMA_DELAY); "
                             "0, the default, lets the simulator pace the "
                             "display per frame on its wall-clock time base")
    parser.add_argument("--control-port", type=int, default=0,
                        help="port of MAIN's CDJ_INPUT_PORT control channel; "
                             "0 leaves the controls refusing rather than inert")
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="extra simulator environment variable; may be repeated",
    )
    parser.add_argument("--coverage", action="store_true",
                        help="print which of the board's inputs this window "
                             "can reach, and exit without opening it")
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = parser.parse_args(raw_argv)

    # A run directory is the handoff between nxs_vm and its viewer.  Make the
    # short form safe: it observes the existing run instead of starting a
    # second simulator over the run's framebuffer, and it reuses the endpoint
    # and profile recorded by the launcher.  Explicit command-line values win.
    if args.run is not None:
        args.run = args.run.resolve()
        manifest = {}
        try:
            value = json.loads((args.run / "run.json").read_text())
            if isinstance(value, dict):
                manifest = value
        except (OSError, ValueError):
            pass
        has_option = lambda name: any(
            token == name or token.startswith(name + "=") for token in raw_argv)
        if not has_option("--output"):
            args.output = args.run / "screen.ppm"
        if not has_option("--control-port"):
            try:
                endpoint = (manifest.get("endpoints") or {}).get("panel_port")
                if endpoint is not None:
                    args.control_port = int(endpoint)
            except (TypeError, ValueError):
                pass
        if not has_option("--nxs-panel"):
            args.nxs_panel = "nxs" in str(manifest.get("profile", "")).lower()
        if not has_option("--attach"):
            args.attach = True

    if args.coverage:
        return args
    for name in (() if args.attach else ("simulator", "elf", "board", "packet")):
        path = getattr(args, name)
        if not path.exists():
            parser.error(f"{name} does not exist: {path}")
    if args.height <= 0 or args.scale <= 0 or args.ppi_delay < 0:
        parser.error("height and scale must be positive, ppi-delay 0 or more")
    if args.refresh_ms < 5:
        parser.error("refresh-ms below 5 spends the whole Tk main loop looking "
                     "for frames; one pass costs about 8 ms")
    return args


def print_coverage(nxs: bool = False) -> int:
    """`--coverage`: the window's own answer to "can I click everything?".

    No Tk, no simulator, no run.  It exists because the honest version of that
    question is a number, and because a number nobody can print is a number
    nobody checks.
    """
    built = controls(nxs)
    reached, missing, stray = coverage(built, nxs)
    print(coverage_line(built, nxs))
    for control in built:
        print("  %-34s %-14s %-8s %s"
              % (control.label, control.input_id or "-", control.kind,
                 control.lines[0].strip() if control.lines else "refuses"))
    return 1 if missing or stray else 0


def main() -> int:
    args = parse_args()
    if args.coverage:
        return print_coverage(args.nxs_panel)
    UiViewer(args).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
