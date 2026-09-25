"""The track-load self-test: both boards in one guest time, browse, load, play.

    python -m tools.cdj_main.cosim_scenario --card runs/cards/aconcert/card.img \
        --playlist-row 4 --out runs/cosim/scenario-1

Starts `boot_vm --cosim` (stock MAIN 4.33 and GUI 4.20 unless told otherwise)
on its own ports, then does what a person at the player does, one step at a
time, each step waiting for the event it causes rather than for a number of
seconds:

  library   the SD library is up (the GUI asks for its first list)
  playlist  ENCODER PUSH on row 0 -> the GUI's ENTER for PLAYLIST
  select    detents down to --playlist-row -> the GUI's preview of that row
  tracks    ENCODER PUSH -> ENTER for the row, MAIN answers with the list
  load      detents to --track-row, ENCODER PUSH -> the GUI's LOAD (type 7
            cursor 1), not the long-press overlay (type 7 cursor 0)
  loaded    the DSP model sees the load closed (+0x7ba0 = 4, then 2)
  play      PLAY -> the DSP model's position runs
  time      MAIN's status records carry a running time (words 5..8)

and with --load2-row N, the next track while the first one plays:

  load2     BROWSE, N detents, ENCODER PUSH -> the GUI's LOAD again
  stream2   MAIN sets the DSP a new stream (+0x8100 = 3 and a stream open),
            which the first load always does and a second one must too

--then KEY:SECONDS[,...] then presses more keys (payload byte.bit, 20.0 =
BROWSE; rot+N / rot-N turns the select encoder; KEY@MS holds it MS ms), each
followed by SECONDS of guest time and a frame then-N-KEY.png -- the syntax of
NEW FIRMWARE's nf_cosim.py.

and writes a table of what passed at which guest second, the frame at each
step (PNG) and the logs, into --out.  A step that times out ends the run: the
table says where it stopped, which is the point of the thing.  Guest time is
the co-simulation's, so the timeouts are in guest seconds too.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable


# The glass is 480x234 (LCD DWX3126; the GUI's DMA0 Y_COUNT is 234 plus
# blanking), but the PPI capture is 255 lines: rows 234..254 are memory past the
# end of the frame buffer and never reach the panel -- a real deck shows none of
# them (hardware check, 24.09.2026).  Screenshots are cropped like the viewer's
# (tools/cdj_gui/view_ui.py PANEL_CROP); the raw PPM frames keep all 255 rows.
PANEL_WIDTH, PANEL_HEIGHT = 480, 234


def save_panel(frame, target):
    from PIL import Image
    Image.open(frame).crop((0, 0, PANEL_WIDTH, PANEL_HEIGHT)).save(target)


class Run:
    def __init__(self, args):
        self.args = args
        self.port = args.port
        self.out = Path(args.out)
        self.temp = ROOT / f"runs/tmp-{self.port}"
        self.main_log = self.temp / f"vm-main-{self.port}.log"
        self.frame = self.temp / f"vm-frame-{self.port}.ppm"
        self.err = self.out / "qemu.err"
        self.rows: list[tuple[str, str, float | None, str]] = []
        self.proc = None
        self.viewer = None
        self._main_pos = 0
        self._main_lines: list[str] = []
        self.repeats: list[str] = []

    # ------------------------------------------------------------- process --
    def start(self):
        busy = []
        for port in range(self.port, self.port + 6):
            with socket.socket() as probe:
                if probe.connect_ex(("127.0.0.1", port)) == 0:
                    busy.append(port)
        if busy:
            raise SystemExit(f"cosim_scenario: port(s) {busy} are in use -- another run "
                             f"(maybe another session's); pick --port")
        if self.out.exists():
            raise SystemExit(f"cosim_scenario: {self.out} exists; runs are never overwritten")
        self.out.mkdir(parents=True)
        self.temp.mkdir(parents=True, exist_ok=True)
        # The logs of the port's previous run would read as this run's events.
        for stale in (self.main_log, self.frame, self.temp / f"vm-gui-{self.port}.log"):
            stale.unlink(missing_ok=True)
        env = dict(os.environ,
                   CDJ_LINK_PORT=str(self.port), CDJ_INPUT_PORT=str(self.port + 4),
                   TEMP=str(self.temp), TMPDIR=str(self.temp),
                   CDJ_QEMU=str(ROOT / "build/qemu/build/qemu-system-sh4"),
                   CDJ_DSP_ACK="1", CDJ_DSP_POSITION="1", CDJ_COSIM_CENSUS="1",
                   PYTHONUNBUFFERED="1")
        if self.args.dsp_trace:
            env["CDJ_DSP_TRACE"] = "1"
        if self.args.audio:
            env["CDJ_DSP_STREAM_DUMP"] = str((self.out / "dsp-stream.bin").resolve())
            env["CDJ_DSP_TRANSPORT_LOG"] = str((self.out / "dsp-transport.log").resolve())
        if self.args.simulator:
            env["CDJ_BFIN_SIM"] = str(Path(self.args.simulator).resolve())
        for item in self.args.env:
            name, _, value = item.partition("=")
            env[name] = value
        medium = (["--usb-stick", str(self.args.card), "--source-key", "usb",
                   "--source-key-at", "12"] if self.args.usb else ["--sd", str(self.args.card)])
        command = [PY, "-m", "tools.cdj_main.boot_vm", "--cosim", *medium,
                   "--seconds", str(self.args.wall),
                   "--frames", str(self.out / "frames"), "--frame-every", "5",
                   "--stderr", str(self.err), "--gui-env", "BFIN_STATS=10",
                   "--gui-env", "BFIN_LINK_FRESH_ONLY=1",
                   # The 200-byte reads are the GUI draining the line after a
                   # record (0xb7ef40: re-arm while bytes keep coming, stop
                   # when the DMA count stands still).  Zeros for them made
                   # the wire never go quiet and cost 3 s after every answer.
                   "--gui-env", "BFIN_SPORT_RX_ZERO_200=",
                   "--gui-env", f"BFIN_MAIN_LINK_DUMP={self.out / 'main-link-dump.bin'}"]
        for item in self.args.gui_env:
            command += ["--gui-env", item]
        if self.args.firmware:
            command += ["--firmware", str(self.args.firmware)]
        command += self.args.boot_arg
        (self.out / "command.txt").write_text(" ".join(command) + "\n")
        self.proc = subprocess.Popen(command, cwd=ROOT, env=env,
                                     stdout=open(self.out / "boot_vm.out", "w"),
                                     stderr=subprocess.STDOUT, start_new_session=True)
        if self.args.show:
            time.sleep(6)
            self.viewer = subprocess.Popen(
                [PY, "-m", "tools.cdj_gui.view_ui", "--attach", "--output", str(self.frame),
                 "--control-port", str(self.port + 4), "--click-hold-ms", "150"],
                cwd=ROOT, stdout=open(self.out / "viewer.log", "w"),
                stderr=subprocess.STDOUT, start_new_session=True)

    def stop(self):
        # boot_vm first gets SIGINT: its own summary (the --trace hits among
        # it) is printed on the way out, which SIGTERM would cut short.
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGINT)
                self.proc.wait(timeout=40)
            except subprocess.TimeoutExpired:
                pass
        for proc in (self.viewer if not self.args.keep_viewer else None, self.proc):
            if proc and proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=20)
                except (ProcessLookupError, PermissionError, subprocess.TimeoutExpired):
                    pass
        for name in (f"vm-main-{self.port}.log", f"vm-gui-{self.port}.log"):
            if (self.temp / name).exists():
                shutil.copyfile(self.temp / name, self.out / name)

    # ---------------------------------------------------------------- time --
    def guest(self) -> float:
        """The co-simulation's guest time, from MAIN's once-a-second census."""
        try:
            text = self.err.read_text(errors="replace")
        except FileNotFoundError:
            return 0.0
        found = re.findall(r"cdj2000-cosim: t=([\d.]+)", text)
        return float(found[-1]) if found else 0.0

    def main_lines(self) -> list[str]:
        try:
            with open(self.main_log, errors="replace") as f:
                f.seek(self._main_pos)
                chunk = f.read()
                self._main_pos = f.tell()
        except FileNotFoundError:
            return self._main_lines
        self._main_lines.extend(chunk.splitlines())
        return self._main_lines

    def requests(self, since: int = 0) -> list[tuple[float, str]]:
        out = []
        for line in self.main_lines()[since:]:
            m = re.search(r"link-rx: delivered 48 bytes.*words ([0-9a-f ]+?) (?:queued \d+ )?t=([\d.]+)", line)
            if m:
                out.append((float(m.group(2)), m.group(1)))
        return out

    def wait(self, what, check, guest_timeout: float, wall_timeout: float = 900):
        """Poll CHECK until it returns something true; None on timeout."""
        start_guest, start_wall = self.guest(), time.time()
        while True:
            if self.proc.poll() is not None:
                return None
            got = check()
            if got:
                return got
            if self.guest() - start_guest > guest_timeout or time.time() - start_wall > wall_timeout:
                return None
            time.sleep(0.5)

    # ---------------------------------------------------------------- input --
    def panel(self, *words):
        subprocess.run([PY, "-m", "tools.cdj_main.panel_control", "--port", str(self.port + 4),
                        *words], cwd=ROOT, capture_output=True, timeout=30)

    def press(self, key, hold_ms=100):
        self.panel("press", key, "--hold-ms", str(hold_ms))

    def press_for(self, what, key, check, guest_timeout):
        """Press KEY and wait for CHECK; if nothing shows in half the time,
        press once more.  A press that reached MAIN and still did nothing in
        the GUI happens now and then (r55-1: ENCODER PUSH down 16.275 s, up
        16.377 s, no ENTER); the second press is logged, never silent."""
        self.press(key)
        got = self.wait(what, check, guest_timeout / 2)
        if got or (self.proc and self.proc.poll() is not None):
            return got
        print(f"  ({what}: no effect from {key}, pressed again)", flush=True)
        self.repeats.append(what)
        self.press(key)
        return self.wait(what, check, guest_timeout / 2)

    # --------------------------------------------------------------- report --
    def snap(self, name):
        try:
            save_panel(self.frame, self.out / f"{len(self.rows):02d}-{name}.png")
        except Exception as error:      # a missing frame is not a failed step
            print(f"  (no frame for {name}: {error})")

    def step(self, name, ok, note=""):
        t = self.guest()
        self.rows.append((name, "ok" if ok else "FAIL", t, note))
        print(f"{name:9s} {'ok' if ok else 'FAIL':4s} guest t={t:8.2f}  {note}", flush=True)
        self.snap(name)
        if ok and name == self.args.stop_after:
            return False            # ends the scenario here, as asked
        return ok

    def report(self):
        lines = ["| step | result | guest s | note |", "|---|---|---|---|"]
        lines += [f"| {n} | {r} | {t:.2f} | {note}"
                  f"{' (pressed twice)' if n in self.repeats else ''} |"
                  for n, r, t, note in self.rows]
        (self.out / "REPORT.md").write_text("\n".join(lines) + "\n")
        print("\n".join(lines))


def request_matching(run: Run, since: int, pattern: str):
    def check():
        for t, words in run.requests(since):
            if re.search(pattern, words):
                return (t, words)
        return None
    return check


def payload_after(run: Run, since: int):
    """MAIN sent something longer than a status record after line SINCE."""
    def check():
        for line in run.main_lines()[since:]:
            m = re.search(r"link-tx: sent (\d+) bytes", line)
            if m and int(m.group(1)) > 64:
                return line
        return None
    return check


def status_times(dump: Path, offset: int) -> list[tuple[int, ...]]:
    """Halfwords 5..8 -- the time fields -- of every status record handed to
    the GUI after OFFSET in BFIN_MAIN_LINK_DUMP ("SPRX" + length + body)."""
    out = []
    try:
        data = dump.read_bytes()[offset:]
    except FileNotFoundError:
        return out
    at = 0
    while at + 8 <= len(data) and data[at:at + 4] == b"SPRX":
        length = int.from_bytes(data[at + 4:at + 8], "little")
        body = data[at + 8:at + 8 + length]
        if length == 64 and len(body) == 64:
            out.append(tuple(int.from_bytes(body[2 * i:2 * i + 2], "little") for i in range(5, 9)))
        at += 8 + length
    return out


def err_matching(run: Run, pattern: str, after: int = 0):
    def check():
        try:
            text = run.err.read_text(errors="replace")
        except FileNotFoundError:
            return None
        m = re.search(pattern, text[after:])
        return m.group(0) if m else None
    return check


def scenario(run: Run, args) -> None:
    run.start()
    if args.manual:
        print(f"both boards in one guest time on ports {run.port}..{run.port + 5}; "
              f"the deck window is yours.  Ctrl-C ends the run.", flush=True)
        try:
            run.proc.wait()
        except KeyboardInterrupt:
            pass
        return
    # library: the GUI's first preview request of the SD root
    run.wait("boot", lambda: run.guest() > 1, 60)
    got = run.wait("library", request_matching(run, 0, r"^0000 0001 000b "), 120)
    if not run.step("library", bool(got), got[1] if got else "no browse request"):
        return
    run.wait("settle", lambda: run.guest() >= got[0] + args.library_settle,
             args.library_settle + 30)

    for _ in range(args.root_row):
        run.panel("rotary", "7", "+1")
        time.sleep(0.3)
    if args.root_row:
        run.wait("root", request_matching(run, 0, r"^0000 0001 000b 0007 0002 %04x" % args.root_row), 20)
    mark = len(run.main_lines())
    got = run.press_for("playlist", "17.0", request_matching(
        run, mark, r"^0000 0001 0003 0007 0001 %04x" % args.root_row), 20)
    if not run.step("playlist", bool(got), got[1] if got else "no ENTER request"):
        return
    run.wait("settle", lambda: run.guest() >= got[0] + 2, 20)

    for _ in range(args.playlist_row):
        run.panel("rotary", "7", "+1")
        time.sleep(0.3)
    want = r"^0000 0001 000b 0007 0002 %04x" % args.playlist_row
    got = run.wait("select", request_matching(run, mark, want), 20)
    if not run.step("select", bool(got), got[1] if got else f"no preview of row {args.playlist_row}"):
        return
    run.wait("settle", lambda: run.guest() >= got[0] + 1, 20)

    mark = len(run.main_lines())
    want = r"^0000 0001 0003 0007 0001 %04x" % args.playlist_row
    got = run.press_for("tracks", "17.0", request_matching(run, mark, want), 20)
    if not run.step("tracks", bool(got), got[1] if got else "no ENTER for the playlist"):
        return
    # the list is on screen once MAIN has answered with it
    run.wait("list", payload_after(run, mark), 30)
    run.wait("settle", lambda: run.guest() >= got[0] + 3, 30)

    for _ in range(args.track_row):
        run.panel("rotary", "7", "+1")
        time.sleep(0.3)
    mark = len(run.main_lines())
    got = run.press_for("load", "17.0", request_matching(run, mark, r"^0000 0007 "), 20)
    if not got:
        run.step("load", False, "no type-7 request")
        return
    if not run.step("load", got[1].startswith("0000 0007 0001"), got[1]):
        return

    err_mark = len(run.err.read_text(errors="replace")) if run.err.exists() else 0
    got = run.wait("loaded", err_matching(run, r"control \+0x7ba0 command 0x00000002", err_mark),
                   args.load_timeout)
    if not run.step("loaded", bool(got), "load closed (+0x7ba0 = 4, 2)" if got else "load never closed"):
        return
    loaded_at = run.guest()
    run.wait("settle", lambda: run.guest() >= loaded_at + 2, 10)

    err_mark = len(run.err.read_text(errors="replace"))
    run.press("16.0")
    got = run.wait("play", err_matching(run, r"cdj2000-dsp: position \d+ ms", err_mark), 20)
    run.step("play", bool(got), got or "the DSP model's position does not run")
    dump = run.out / "main-link-dump.bin"
    offset = dump.stat().st_size if dump.exists() else 0
    start = run.guest()
    run.wait("run", lambda: run.guest() >= start + args.play_seconds, args.play_seconds + 30)
    times = status_times(dump, offset)
    moving = [t for t in times if 0xbbbb not in t]
    ok = len(set(moving)) >= 3
    note = (f"words 5..8 {times[0]} .. {times[-1]}, {len(set(moving))} distinct"
            if times else "no status records after PLAY")
    if not run.step("time", ok, note):
        return

    if args.load2_row is not None:
        run.press("20.0")                       # BROWSE: back to the track list
        at = run.guest()
        run.wait("settle", lambda: run.guest() >= at + 3, 30)
        for _ in range(abs(args.load2_row)):
            run.panel("rotary", "7", "%+d" % (1 if args.load2_row > 0 else -1))
            time.sleep(0.3)
        at = run.guest()
        run.wait("settle", lambda: run.guest() >= at + 2, 30)
        mark = len(run.main_lines())
        err_mark = len(run.err.read_text(errors="replace"))
        run.press("17.0")
        got = run.wait("load2", request_matching(run, mark, r"^0000 0007 "), 20)
        if not run.step("load2", bool(got) and got[1].startswith("0000 0007 0001"),
                        got[1] if got else "no type-7 request"):
            return
        got = run.wait("stream2", err_matching(
            run, r"control \+0x8100 command 0x00000003 \(\+4\.\.(?: [0-9a-f]{8}){9}\)", err_mark),
            args.load_timeout)
        if not run.step("stream2", bool(got), got or "no new stream (+0x8100 = 3) for the second track"):
            return
    then_keys(run, args.then)


def then_keys(run: Run, spec: str) -> None:
    """--then: more keys after the scenario, each with guest seconds after it."""
    for n, item in enumerate(x for x in spec.split(",") if x):
        key, _, secs = item.rpartition(":")
        if key.startswith("rot"):
            run.panel("rotary", "7", key[3:])
        else:
            key, _, hold = key.partition("@")
            run.press(key, int(hold or 100))
        at = run.guest()
        run.wait("then", lambda: run.guest() >= at + float(secs or 3), float(secs or 3) + 60)
        try:
            save_panel(run.frame, run.out / f"then-{n + 1}-{key}.png")
        except Exception as error:
            print(f"  (no frame for then {n + 1}: {error})")
        print(f"then {n + 1}: {key}, guest {at:.1f} -> {run.guest():.1f}", flush=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--card", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("CDJ_LINK_PORT", "6480")),
                        help="the run takes PORT..PORT+5 (CDJ_LINK_PORT, else 6480)")
    parser.add_argument("--manual", action="store_true",
                        help="start the machine (with --show, the window) and press "
                             "nothing: the deck is yours")
    parser.add_argument("--root-row", type=int, default=0,
                        help="the PLAYLIST row of the card's root menu (a stick's own settings may "
                             "put it first; rekordbox's default order has it fifth, row 4)")
    parser.add_argument("--library-settle", type=float, default=3, metavar="S",
                        help="guest seconds between the library and the first press; "
                             "a big card's GUI is still busy after 3 (aconcert-1g)")
    parser.add_argument("--playlist-row", type=int, default=0)
    parser.add_argument("--track-row", type=int, default=0)
    parser.add_argument("--load-timeout", type=float, default=240,
                        help="guest seconds the DSP load may take")
    parser.add_argument("--wall", type=int, default=3600, help="wall-clock budget")
    parser.add_argument("--firmware", type=Path)
    parser.add_argument("--simulator", help="GUI simulator binary (CDJ_BFIN_SIM)")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--gui-env", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--boot-arg", action="append", default=[], metavar="ARG",
                        help="extra boot_vm argument, e.g. --boot-arg=--trace=0x42596ee")
    parser.add_argument("--dsp-trace", action="store_true")
    parser.add_argument("--play-seconds", type=float, default=12,
                        help="guest seconds to let the deck play after PLAY")
    parser.add_argument("--audio", action="store_true",
                        help="record the DSP stream and transport and render deck.wav "
                             "(tools/cdj_main/deck_audio.py)")
    parser.add_argument("--show", action="store_true", help="open the deck window")
    parser.add_argument("--keep-viewer", action="store_true")
    parser.add_argument("--usb", action="store_true",
                        help="plug --card in as a USB memory stick instead of the SD slot "
                             "(USB SOURCE key at 12 s)")
    parser.add_argument("--load2-row", type=int, metavar="N",
                        help="after the time step, BROWSE and load the track N rows "
                             "below (negative: above) the first one; steps load2, stream2")
    parser.add_argument("--then", default="", metavar="KEY:SECONDS[,...]",
                        help="keys after the scenario (nf_cosim.py's syntax)")
    parser.add_argument("--stop-after", metavar="STEP",
                        help="end the scenario after this step passes")
    parser.add_argument("--keep", action="store_true",
                        help="leave the machine running after the last step")
    args = parser.parse_args(argv)

    run = Run(args)
    try:
        scenario(run, args)
    finally:
        run.report()
        if not args.keep:
            run.stop()
        if args.audio and (run.out / "dsp-transport.log").exists():
            subprocess.run([PY, "-m", "tools.cdj_main.deck_audio",
                            "--stream", str(run.out / "dsp-stream.bin"),
                            "--transport", str(run.out / "dsp-transport.log"),
                            "--out", str(run.out / "deck.wav")], cwd=ROOT)
    return 0 if run.rows and all(r[1] == "ok" for r in run.rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
