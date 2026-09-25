"""Boot the whole CDJ-2000, watch its screen live, and click on it.

    python -m tools.cdj_main.view_vm --sd <card.img>

MAIN runs on QEMU and the GUI board on the Blackfin simulator, linked to each
other; `tools.cdj_gui.view_ui` puts the GUI's framebuffer in a tkinter window
and refreshes it as the firmware paints.  This is `boot_vm` with a screen
instead of a single capture at the end — same two boards, same link.

QEMU is started with `CDJ_INPUT_PORT`, so `emulator/qemu/cdj2000_input.c` opens
a control channel and the window's buttons reach the running panel.  The
buttons sit **beside** the picture, because `BROWSE`/`TAG LIST`/`INFO`/`MENU`
and `LINK`/`USB`/`SD`/`DISC` are backlit plastic on the real unit and appear in
no frame dump.  `--no-control` leaves the port unset, which is what
makes a run a control run: nothing binds and nothing is merged into the panel
payload.

The same channel is reachable from a second shell without this window:

    python -m tools.cdj_main.panel_control --port 5984 press sd
    python -m tools.cdj_main.panel_control --port 5984 rotary 4 +8

The picture takes a while to become interesting: the GUI paints its chrome
within a few seconds, but MAIN only publishes its operating mode once the panel
handshake completes, and the record stream starts after that.  Leave it running.

Close the window to stop both boards; QEMU is asked to quit through its monitor
so the log is flushed rather than truncated.

**Nothing starts until the window can reach every input.**  `view_ui
--coverage` has always been able to say `48 of 48` and exit non-zero on a gap,
and nothing ever called it -- so the last regression from 46 to 38 was found by
accident rather than by the check that exists for it.  `coverage_gate()` below
runs before the first `Popen`, and a gap names the inputs and refuses.  Two
boards and three minutes of boot are the wrong place to discover that the
button you meant to press was never built; `--ignore-coverage` is there for the
one case where you know and want the window anyway.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
from tools.paths import BFIN_SIM, FIRMWARE, PACKETS, QEMU, RUNS, qemu_environment
TEMP = Path(os.environ.get("TEMP", "."))
PORT = int(os.environ.get("CDJ_LINK_PORT", "5980"))
# 5980 link, 5981 monitor, 5982 the second serial, 5983 the gdb stub; the panel
# control channel takes the next one so nothing collides with boot_vm.
CONTROL_PORT = int(os.environ.get("CDJ_INPUT_PORT", "5984"))

# Payload byte 19 bits 0..3 -- see tools.cdj_main.boot_vm for the whole chain.
#
# **REVERSED UNTIL 2026-08-07.**  `--source-key sd` pressed 19.1, which is the
# USB key, so MAIN reported USB and the GUI asked for a USB browse list on a
# machine with an SD card in it.  Corrected here, in boot_vm and in
# panel_control.BUTTON_NAMES on the same day; MAIN's own
# service-mode name table says 19.0..19.3 = LINK USB SD DISC, r160's status word
# 18 steps with the bit index, and A-025 measured the KIND enumeration as
# 0 LINK, 1 USB, 2 SD, 3 DISC.
#
# All three copies are read by tests/test_panel_names_match_the_firmware.py and
# compared against the firmware image, so this cannot silently drift again.
SOURCE_KEYS = {"link": 0x01, "usb": 0x02, "sd": 0x04, "disc": 0x08}

# What `coverage_gate` returns when it refuses.  Distinct from 1 so a caller can
# tell "the window would not have been able to click everything" from "the run
# itself failed".
COVERAGE_REFUSED = 3


def coverage_gate(report=print) -> int:
    """Refuse to boot two boards for a window that cannot click everything.

    `view_ui.coverage()` compares `panel_control.input_ids()` -- the inputs the
    board's payload decoder actually has -- against the controls the window
    builds, in both directions: an input with no control, and a control naming
    an input the board does not have.  Either is a silent hole, and both have
    happened here: the window offered 38 dedicated controls and one spinbox
    captioned "sweep 0..6" for the analogue half, and *nothing in its output
    said so* until someone counted by hand.

    This runs before anything is started.  A gap after the machine is up costs
    the whole boot; a gap here costs a second and names the input.
    """
    try:
        from tools.cdj_gui import view_ui
    except Exception as error:                   # pragma: no cover - env only
        report("view_vm: cannot check the window's coverage: %s" % error)
        report("  The window is what clicks; if its controls cannot be counted "
               "they cannot be trusted either, so nothing is started.")
        return COVERAGE_REFUSED
    reached, missing, stray = view_ui.coverage()
    line = view_ui.coverage_line()
    if not missing and not stray:
        report("# controls: %s" % line)
        return 0
    report("view_vm: refusing to start -- %s" % line)
    for name in missing:
        report("  no control for input %s" % name)
    for name in stray:
        report("  control claims %s, which this board does not decode" % name)
    report("  Booting both boards would give you a window that cannot press "
           "these; fix tools/cdj_gui/view_ui.py, or pass --ignore-coverage.")
    report("  The same answer without this launcher: "
           "python -m tools.cdj_gui.view_ui --coverage")
    return COVERAGE_REFUSED


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=int, default=2,
                        help="pixel zoom; the panel is 480x234")
    parser.add_argument("--no-main", action="store_true",
                        help="GUI only, against the replayed record stream")
    parser.add_argument("--firmware", default=str(FIRMWARE / "main-firmware.bin"),
                        help="the MAIN flash image to boot, e.g. one written "
                             "by tools.cdj_main.repack_main (default "
                             "firmware/main-firmware.bin)")
    parser.add_argument("--sd", default=os.environ.get("CDJ_SD_IMAGE"),
                        help="FAT32 card image; build one with "
                             "tools.cdj_main.make_sd_image")
    parser.add_argument("--source-key", choices=sorted(SOURCE_KEYS), default=None,
                        help="press a SOURCE key on the panel; defaults to 'sd' "
                             "when a card image is given")
    parser.add_argument("--source-key-at", type=float, default=None,
                        help="virtual seconds at which to press it")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT,
                        help="port for MAIN's runtime panel control channel "
                             "(CDJ_INPUT_PORT); the window's buttons and "
                             "tools.cdj_main.panel_control both speak to it")
    parser.add_argument("--no-control", action="store_true",
                        help="leave CDJ_INPUT_PORT unset, so nothing binds and "
                             "nothing is merged into the panel payload — this "
                             "is what a control run means")
    parser.add_argument("--ignore-coverage", action="store_true",
                        help="start even though the window cannot reach every "
                             "input the board decodes; the gap is printed "
                             "either way")
    args = parser.parse_args()
    control_port = 0 if args.no_control else args.control_port
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):        # a redirected or wrapped stdout
        pass

    # Before the first Popen, always.  A window that cannot click everything is
    # a fact about the window, and it is knowable in a second -- there is no
    # reason to learn it after three minutes of booting two boards.
    gate = coverage_gate()
    if gate and not args.ignore_coverage:
        return gate

    env = qemu_environment()
    main_log = TEMP / "vm-main.log"
    # QEMU's own complaints, which used to go to DEVNULL.  `-D` catches the
    # guest trace; a refusal to start is on stderr and was being thrown away.
    complaints = TEMP / "vm-main-stderr.log"
    board = None

    # A card image that is not there is the whole run.  QEMU exits with
    # `Could not open '<path>'` on stderr, which this launcher sends to
    # DEVNULL; the GUI board is then started against a link nobody is on, and
    # the window sits on the boot screen at 0.6 fps for as long as you leave
    # it.  That failure has now cost two evenings and been diagnosed twice as
    # "the emulator is slow".  It is checkable in a millisecond.
    # The same trap as the card below: QEMU refuses a missing -bios on the
    # stderr nobody reads, and the window waits on the boot screen.
    if not args.no_main and not Path(args.firmware).exists():
        print("view_vm: no MAIN flash image at %s" % args.firmware)
        return 2

    if args.sd and not Path(args.sd).exists():
        print("view_vm: no card image at %s" % args.sd)
        print("  MAIN would exit before the GUI ever reached it, and the "
              "window would show the boot screen for ever without saying why.")
        print("  Build one:  python -m tools.cdj_main.make_sd_image "
              "<card-contents-dir> <path>.img --size 128M")
        print("  Or start without a card:  drop --sd (the browse list is then "
              "empty, which is a state, not a fault).")
        return 2

    source_key = args.source_key or ("sd" if args.sd else None)
    # A card that is the source before the GUI's first browse gives the
    # card's library with the player screen (measured: 2 of 2, at 33 s);
    # a card selected after it meets the GUI's browse loop for the boot
    # source and the key is lost more often than not (1 of 6, see
    # RUNNING.md, "Switching to a medium").  So with a card and no other
    # instruction the card goes in at 10 s and its key is pressed at 12 s.
    if args.source_key_at is None:
        args.source_key_at = 12.0 if (args.sd and source_key == "sd") else 40.0
    keys = os.environ.get("CDJ_PANEL_KEYS", "")
    if source_key and not keys:
        keys = "%g:19:%02x" % (args.source_key_at, SOURCE_KEYS[source_key])
        print(f"# panel: {source_key.upper()} SOURCE key at "
              f"{args.source_key_at:g} s ({keys})")

    if not args.no_main:
        print(f"# MAIN: {QEMU.name} -M cdj2000-main   (link on {PORT})")
        board = subprocess.Popen(
            [
                str(QEMU), "-M", "cdj2000-main",
                "-bios", str(args.firmware),
                "-display", "none", "-no-reboot",
                "-d", "unimp", "-D", str(main_log),
                "-serial", f"tcp:127.0.0.1:{PORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{PORT + 2},server,nowait",
                "-serial", "null",
                "-monitor", f"telnet:127.0.0.1:{PORT + 1},server,nowait",
                *(["-drive", f"if=sd,format=raw,file={args.sd}"] if args.sd else []),
            ],
            env=dict(env, CDJ_TMU_FREQ=os.environ.get("CDJ_TMU_FREQ", "54000000"),
                     CDJ_SD_INSERT=os.environ.get("CDJ_SD_INSERT",
                                              "10" if args.sd else "25"),
                 # A press lands only if MAIN builds a status record while
                 # the key is down, every 3.05 s when nothing else changes;
                 # the board's own 300 ms never spans one.
                 CDJ_PANEL_HOLD_MS=os.environ.get("CDJ_PANEL_HOLD_MS", "3300"),
                     CDJ_PANEL_KEYS=keys,
                     # Unset means no socket, no poll, no merge: cdj2000_input.c
                     # is inert and the run is indistinguishable from one built
                     # without it.
                     CDJ_INPUT_PORT=str(control_port) if control_port else "",
                     CDJ_PANEL_FRAME=os.environ.get(
                         "CDJ_PANEL_FRAME",
                         "00000000000000000000000000000000000400000000"
                         if args.sd else "")),
            stdout=subprocess.DEVNULL, stderr=complaints.open("wb"),
        )
        if control_port:
            print(f"# panel: control channel on 127.0.0.1:{control_port} "
                  f"(python -m tools.cdj_main.panel_control "
                  f"--port {control_port} press sd)")
        time.sleep(3)
        # QEMU refuses a bad card, a taken port or an unknown machine within
        # milliseconds and then it is simply gone.  Nothing downstream can tell
        # that from a MAIN that is merely slow to speak: the GUI blocks on a
        # link no one is on, publishes about 0.6 frames a second, and the
        # window shows the boot screen indefinitely.  So look, once, and say so.
        if board.poll() is not None:
            print(f"view_vm: MAIN exited immediately (code {board.returncode}).")
            for line in complaints.read_text(errors="replace").splitlines()[:6]:
                print("  qemu: %s" % line)
            print("  Not starting the GUI board: without MAIN it would render "
                  "the boot screen at about 0.6 fps and never change, which "
                  "reads as a slow emulator rather than as a missing one.")
            return 2

    # The simulator rewrites the framebuffer and the SPORT capture continuously.
    # Both default into the repository, and a cloud-sync client watching that
    # directory fights the writer: the file is then intermittently unreadable.
    command = [
        sys.executable, "-m", "tools.cdj_gui.view_ui",
        "--simulator", str(BFIN_SIM),
        "--packet", str(PACKETS / "status-standalone.bin"),
        "--output", str(TEMP / "vm-screen-live.ppm"),
        "--tx-output", str(TEMP / "vm-sport-tx-live.bin"),
        "--log", str(TEMP / "vm-ui-sim.log"),
        "--scale", str(args.scale),
        "--env", "BFIN_PARALLEL_WRITEBACK=1",
        "--env", "BFIN_GUI_COLOR=rgb555le",
    ]
    # The GUI board has no panel; the buttons belong to MAIN, so they are only
    # live when MAIN is running with the channel open.
    if control_port and not args.no_main:
        command += ["--control-port", str(control_port)]
    if not args.no_main:
        command += [
            "--env", f"BFIN_MAIN_LINK=127.0.0.1:{PORT}",
            # See boot_vm.py: without this the GUI double-faults at
            # 0x00b99196 in most boots on the wall-clock time base.
            "--env", "BFIN_LINK_ANNOUNCE_STICKY=1",
            # A real MAIN is already transmitting when the GUI boots, so the
            # peer answers until the link takes over.
            "--env", "BFIN_MAIN_PEER=1",
            "--env", f"BFIN_MAIN_PEER_STATUS={PACKETS / 'status-usb-q.bin'}",
            "--env", "BFIN_MAIN_PEER_STATUS_HOLD=250",
            "--env", f"BFIN_MAIN_PEER_PAYLOAD_1={PACKETS / 't1-browse.bin'}",
        ]

    print("# GUI:  live window — close it to stop both boards")
    try:
        subprocess.run(command, cwd=str(ROOT), env=env, check=False)
    finally:
        if board:
            try:
                monitor = socket.create_connection(("127.0.0.1", PORT + 1),
                                                   timeout=5)
                monitor.sendall(b"quit\n")
                time.sleep(1.0)
                monitor.close()
            except OSError:
                pass
            try:
                board.wait(timeout=15)
            except subprocess.TimeoutExpired:
                board.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
