"""Boot the whole CDJ-2000: MAIN on QEMU and the GUI board on the Blackfin sim.

    python -m tools.cdj_main.boot_vm [--seconds 150] [--output frame.png]

QEMU serves the two link sockets and the Blackfin simulator connects to them as
a client, so the GUI is attached before MAIN's first frame rather than joining
part-way through.  MAIN's panel channel is modelled on the board itself, so
nothing has to be poked in by hand any more: `PnlCom_SndTASK` publishes the
operating mode out of its panel handshake within a few seconds, `GuiCom_SndTASK`
wins its ten-second startup deadline, and the record stream runs.

The run ends by reading MAIN's own words back through the QEMU monitor, so the
report says what the machine thought rather than what the picture suggests:

    panel state   0x04fe29f4   non-zero once a panel frame has been accepted
    GuiCom mode   0x04c06fb0   1, 3 or 4 makes the send task serve the GUI
    ready words   0x0489bcf4   both non-zero means the handshake completed
    bAnsReceive   0x0489b368   toggles while answers are coming back

QEMU is stopped with a monitor `quit`, not a kill, so the log is flushed.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from tools.cdj_main import caution
from tools.cdj_main.procs import stop_tree

ROOT = Path(__file__).resolve().parents[2]


def frame_sampler(source: Path, target: Path, every: float,
                  stop: threading.Event, report: dict) -> None:
    """Copy the live frame aside every `every` seconds, and record every tick.

    The simulator rewrites its PPM whenever a frame completes, and it does so
    through a temporary file plus a rename, so a copy taken at any moment is a
    whole frame rather than a torn one.  Naming by elapsed seconds is what
    makes a pair usable as evidence: an input pressed at T is bracketed by the
    samples either side of T, and a control run with no input sampled on the
    same grid separates a real change from the screen's own animation.

    Two things this used to get wrong, both of which cost real runs.

    **A sample is only written when the bytes changed**, which is right for disk
    but wrong for evidence: a window over a stretch where the screen stood still
    has no files in it, and an evaluator then reports "no frame on one side"
    -- exactly what it reports for a stretch that was never recorded.  Those are
    opposite findings ("this input changed nothing" vs "this input was not
    measured") and they were indistinguishable.  So every tick is now written to
    `index.tsv` whether or not it produced a file, and the pair
    (elapsed, status) tells them apart afterwards.

    **And the thread died on the first write error.**  `write_bytes` sat outside
    the `try`, so one `OSError` -- a scratchpad hiccup, a full disk, a virus
    scanner holding the file -- ended sampling for the rest of the run without a
    word.  `r093` recorded nothing after t235.8 of a 370 s run and `r094` nine
    frames in 745 s; that silence is what made both unusable, and it is the
    reason a run must now say how far its recording reached.
    """
    target.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    last = None
    ticks = 0
    written = 0
    failures = 0
    last_change = None
    index = target / "index.tsv"
    with index.open("w", encoding="utf-8") as stream:
        stream.write("# elapsed\tstatus\tfile\n")
        while not stop.wait(every):
            elapsed = time.monotonic() - started
            ticks += 1
            status, name = "same", ""
            try:
                # The simulator publishes each completed frame by renaming a
                # temporary over this path, and on Windows a read that lands in
                # that window fails with PermissionError (or, more rarely,
                # FileNotFoundError).  It is a race of microseconds, so retry
                # briefly rather than lose the tick: in r096, 53 ticks failed
                # this way and **seven of them fell on a key window**, which
                # frame_delta then had to report as NOT MEASURED -- a third of
                # the run's evidence, thrown away for want of a retry.
                data = None
                for attempt in range(8):
                    try:
                        data = source.read_bytes()
                        break
                    except OSError:
                        if attempt == 7:
                            raise
                        time.sleep(0.01)
                if not data:
                    status = "empty"
                elif data == last:
                    status = "same"
                else:
                    name = "t%06.1f.ppm" % elapsed
                    (target / name).write_bytes(data)
                    last = data
                    last_change = elapsed
                    written += 1
                    status = "new"
            except OSError as error:
                # Never let one failed tick end the recording: that is the
                # silent stop this docstring is about.
                failures += 1
                status = "error:%s" % type(error).__name__
                name = ""
            stream.write("%.1f\t%s\t%s\n" % (elapsed, status, name))
            stream.flush()
            report.update(ticks=ticks, written=written, failures=failures,
                          last_tick=elapsed, last_change=last_change)
from tools.paths import BFIN_SIM, FIRMWARE, PACKETS, QEMU, RUNS, qemu_environment
TEMP = Path(os.environ.get("TEMP", "."))
PORT = int(os.environ.get("CDJ_LINK_PORT", "5980"))

# The four SOURCE keys, as payload byte 19 bits 0..3.  The decoder at 0x28e44a
# spreads them into panel status byte 87 bits 7..4 and the handler 0x28ddc8
# turns a rising edge into the one-hot flag at 0x04c084d0+n*4, which is what
# 0x24bd7a reports as status word 18 bits 2:0 -- as **n + 1**, not as n.
# Without a press MAIN reports 4, which is DISC, not LINK.
#
# THE NAMES WERE REVERSED UNTIL 2026-08-07, and every run this project ever made
# pressed USB while calling it SD.  Two independent readings, neither of which
# needed a new run:
#
#   * r160's coverage plan pressed all four in turn, and the status word 18 of
#     its 22 493 delivered records is a staircase -- 4 before the t40 key, then
#     2 / 1 / 2 / 3 / 4 tracking 19.1 / 19.0 / 19.1 / 19.2 / 19.3.  So
#     w18 = bit index + 1.
#   * The GUI turns that byte straight into the browse request's word 4, with
#     one writer per link and no remap: 0xb7e442 (B[0x4b43d0] = w18 & 7) ->
#     0xb9b5c8 ([0x6aea04] = that - 1, clamped) -> 0xb9952c (post_event) ->
#     0xb7dbce (W[0xf01048]).  A-025 measured MAIN's KIND enumeration as
#     0 LINK, 1 USB, 2 SD, 3 DISC, so bit index n IS the KIND.
#
# MAIN's own service-mode name table says the same (INPUT_MANIFEST.md's name
# table).  Both boards always agreed; this table did not,
# which is the whole of "the GUI asks for USB while the card is SD": it asks for
# USB because MAIN says USB because we pressed USB.
#
# tools/cdj_main/view_vm.py carries the same constant; the two must not drift,
# and tests/test_panel_names_match_the_firmware.py is what keeps them together.
SOURCE_KEYS = {"link": 0x01, "usb": 0x02, "sd": 0x04, "disc": 0x08}

WATCH = [
    ("panel state", 0x04FE29F4, "xp /1wx 0x04fe29f4"),
    # 0x04c084d0 + 4n is the one-hot flag for payload bit 19.n, and n is the
    # KIND: 0 LINK, 1 USB, 2 SD, 3 DISC.  These four labels were shifted by one
    # for the same reason SOURCE_KEYS was.
    ("source LINK", 0x04C084D0, "xp /4wx 0x04c084d0"),
    ("source USB", 0x04C084D4, None),
    ("source SD", 0x04C084D8, None),
    ("source DISC", 0x04C084DC, None),
    ("GuiCom mode", 0x04C06FB0, "xp /1wx 0x04c06fb0"),
    ("ready word", 0x0489BCF4, "xp /2wx 0x0489bcf4"),
    ("ready word+4", 0x0489BCF8, None),
    ("bAnsReceive", 0x0489B368, "xp /1wx 0x0489b368"),
    ("link flag", 0xFFF10048, "xp /1wx 0xfff10048"),
    # The RTOS system time, one count per tick interrupt (patches/README.md).
    # Its rate against the wall clock is the one number that says whether
    # MAIN's guest time is keeping up with the host.
    ("rtos ticks", 0x04FC45EC, "xp /1wx 0x04fc45ec"),
]

# What --poll-every reads on each round: the boot milestones and the tick.
POLL_WATCH = [entry for entry in WATCH
              if entry[0] in ("panel state", "GuiCom mode", "ready word",
                              "bAnsReceive", "rtos ticks")]
POLL_HEADER = ("elapsed", "panel", "guicom", "ready", "ready4", "rtos",
               "rtos_per_s", "cpu_qemu_s", "cpu_sim_s")


# Writing a word into the running guest.
#
# The QEMU monitor can read memory (`xp`) but has no counterpart that writes it,
# so a poke has to go through the gdb stub.  Nothing needs to be installed for
# that: the packet is `M<address>,<length>:<hex>` with a two-digit modulo-256
# checksum, and the stub answers `OK`.  Connecting stops the machine and `D`
# (detach) starts it again, which is why every poke opens its own connection --
# a held-open stub would leave the guest paused between writes.
#
# Addresses are given as physical, the same as everywhere else in this file, and
# sent through the P2 window (`0xa0000000`), which is untranslated and uncached:
# a P0 address would need the guest's TLB to agree, and a cached one would let
# MAIN keep reading a stale line.
def console_chardev(setting: str | None, log: Path) -> str:
    """The QEMU chardev for MAIN's console SCIF: off, a log file, or a TCP
    server when CDJ_DEBUG_CONSOLE is "tcp:HOST:PORT"."""
    if not setting:
        return "null"
    if setting.startswith("tcp:"):
        return setting if ",server" in setting else setting + ",server,nowait"
    return f"file:{log}"


def gdb_poke(port: int, address: int, value: int, size: int = 4) -> str:
    payload = value.to_bytes(size, "little").hex()
    body = "M%x,%x:%s" % (0xA0000000 | (address & 0x1FFFFFFF), size, payload)
    packet = "$%s#%02x" % (body, sum(body.encode()) & 0xFF)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as stub:
            stub.settimeout(3.0)
            stub.sendall(b"+" + packet.encode())
            reply = stub.recv(256).decode("ascii", "replace")
            stub.sendall(b"+$D#44")
            time.sleep(0.05)
        return reply
    except OSError as error:
        return "error: %s" % error


def poke_thread(port: int, pokes, at: float, hold: float, interval: float,
                stop: threading.Event):
    """Write the pokes once `at` seconds in, then hold them against the firmware.

    Rewriting matters for more than a value the firmware might overwrite: a
    one-shot flag like the record builder's 0x0489db00 is *consumed*, so it
    marks a single record and the GUI, which decodes roughly one record in two
    hundred, will almost certainly miss it.  A short interval keeps a useful
    fraction of the stream carrying the bit.
    """
    if stop.wait(at):
        return
    first = True
    while True:
        for address, value in pokes:
            reply = gdb_poke(port, address, value)
            if first:
                print("# poke 0x%08x = %d -> %s" % (address, value, reply.strip()))
        first = False
        if hold <= 0 or stop.wait(interval):
            return


def parse_poke(text: str) -> tuple[int, int]:
    address, _, value = text.partition("=")
    if not value:
        raise argparse.ArgumentTypeError("expected ADDRESS=VALUE, got %r" % text)
    return int(address, 0), int(value, 0)


# MAIN's SDRAM as the firmware itself addresses it: code and the two heaps all
# live under 0x08000000, and everything below 0x04000000 is either the flash
# window or a small integer that happens to be word-aligned.  Following one of
# those would fill the report with `xp` errors and hide the real targets.
CHAIN_LOW = 0x04000000
CHAIN_HIGH = 0x08000000
CHAIN_FANOUT = 24


def looks_like_ram(value: int) -> bool:
    return (value & 3) == 0 and CHAIN_LOW <= value < CHAIN_HIGH


def cpu_seconds(names=("qemu-system-sh4", "cdj-run")) -> dict[str, float]:
    """CPU seconds consumed so far by qemu-system-sh4 and cdj-run, by name.

    Read through wmic on Windows (user + kernel time, 100 ns units) so a poll
    costs one short process rather than a PowerShell start-up.  Anywhere else,
    or if wmic is missing, the dict is empty and the caller prints -1.  Names
    are prefixes, so an A/B binary such as qemu-system-sh4-legacy.exe is
    counted under "qemu-system-sh4".
    """
    if os.name != "nt":
        return {}
    try:
        text = subprocess.run(
            ["wmic", "process", "where",
             " or ".join("name like '%s%%'" % name for name in names),
             "get", "Name,UserModeTime,KernelModeTime", "/format:csv"],
            # wmic answers in the console code page, and a German error text
            # ("ungueltig" with its umlaut, 0x81 in cp850) once QEMU has been
            # killed from outside is not cp1252: decode leniently, or the
            # reader thread dies and the poll row reads -1 for the rest of
            # the run (update-23).
            capture_output=True, text=True, errors="replace", timeout=5,
            creationflags=subprocess.CREATE_NO_WINDOW).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    totals: dict[str, float] = {}
    for line in text.splitlines():
        parts = line.strip().split(",")
        if len(parts) != 4 or not parts[1].isdigit():
            continue
        _, kernel, name, user = parts
        key = next((prefix for prefix in names
                    if name.lower().startswith(prefix.lower())), name)
        totals[key] = totals.get(key, 0.0) + (int(user) + int(kernel)) / 1e7
    return totals


def monitor(sock: socket.socket, watch=None, settle: float = 0.1) -> dict[int, int]:
    for _, _, command in (watch if watch is not None else WATCH):
        if command:
            sock.sendall((command + "\n").encode())
            time.sleep(settle)
    text = ""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            text += chunk.decode("ascii", "replace")
    except socket.timeout:
        pass
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    words: dict[int, int] = {}
    for line in text.splitlines():
        line = line.strip()
        if not re.match(r"^[0-9a-f]{8}:", line):
            continue
        address, _, rest = line.partition(":")
        base = int(address, 16)
        for index, value in enumerate(rest.split()):
            words[base + 4 * index] = int(value, 16)
    return words


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=150,
                        help="wall-clock budget; the compositor needs ~100 s")
    parser.add_argument("--output", default=str(TEMP / "cdj2000-vm.png"))
    parser.add_argument("--sd", default=os.environ.get("CDJ_SD_IMAGE"),
                        help="FAT32 card image; build one with "
                             "tools.cdj_main.make_sd_image")
    parser.add_argument("--usb-stick", default=os.environ.get("CDJ_USB_STICK"),
                        help="raw disk image plugged into the type-A socket as "
                             "a USB memory stick (QEMU's usb-storage on the "
                             "SoC's USB module); a firmware update needs the "
                             ".UPD files in its root")
    parser.add_argument("--source-key", choices=sorted(SOURCE_KEYS),
                        default=None,
                        help="press a SOURCE key on the panel; defaults to "
                             "'sd' when a card image is given, because nothing "
                             "else selects the medium (0x28ddc8)")
    parser.add_argument("--source-key-at", type=float, default=None,
                        help="virtual seconds at which to press it, after the "
                             "card has been mounted")
    parser.add_argument("--poke", type=parse_poke, action="append", default=[],
                        metavar="ADDRESS=VALUE",
                        help="write a word into MAIN's memory while it runs, "
                             "through the gdb stub; repeatable")
    parser.add_argument("--poke-at", type=float, default=60.0,
                        help="wall-clock seconds into the run at which to poke")
    parser.add_argument("--ppi-delay", type=int, default=0,
                        help="ticks per display scanline for the simulator "
                             "(BFIN_PPI_DMA_DELAY).  0, the default, lets it "
                             "pace the display per frame on its wall-clock "
                             "time base; the link retry is BFIN_SPORT_RETRY_US")
    parser.add_argument("--no-peer", action="store_true",
                        help="serve the GUI from the live MAIN board alone, "
                             "without the canned bootstrap records")
    parser.add_argument("--cosim", action="store_true",
                        help="both boards in one guest time "
                             "(emulator/qemu/cdj2000_cosim.c): MAIN under "
                             "-icount, started with the GUI, the GUI on its "
                             "virtual time base, the link on port PORT+5 with "
                             "a fixed latency.  Neither board then depends on "
                             "the other simulator's speed; implies --no-peer")
    parser.add_argument("--cosim-quantum-us", type=int, default=100,
                        metavar="US",
                        help="--cosim link latency and lookahead (100)")
    parser.add_argument("--cosim-shift", type=int, default=2, metavar="N",
                        help="--cosim: MAIN runs 2^N ns per instruction "
                             "(-icount shift=N); 2 is 250 MIPS")
    parser.add_argument("--gui-board", metavar="FILE",
                        help="the GUI simulator's board file (run_headless --board), "
                             "e.g. one whose flash is the dump of an earlier update run")
    parser.add_argument("--gui-env", action="append", default=[],
                        metavar="NAME=VALUE",
                        help="extra environment variable for the Blackfin "
                             "simulator, e.g. BFIN_GUI_STATE_WRITE_START=0x4b4398")
    # A patched GUI image is a PAIR: the decompressed boot memory (the ELF the
    # simulator loads) and the packed flash the LZSS accelerator reads
    # (BFIN_FAST_LZSS, plus its DELTA in BFIN_FAST_LZSS_SHIFT).  --gui-env can
    # already set the second; without this option the first stayed at
    # firmware/gui-boot-memory.elf, so a run that thought it was testing a
    # patched image was in fact running the stock one against a patched flash --
    # which decompresses garbage rather than failing cleanly.
    parser.add_argument("--gui-elf", metavar="FILE",
                        help="boot-memory ELF for the Blackfin simulator; pass "
                             "the matching BFIN_FAST_LZSS/_SHIFT via --gui-env")
    parser.add_argument("--gui-flash", metavar="FILE",
                        help="boot the GUI from this 2 MiB flash image (e.g. a "
                             "BFIN_CFI_DUMP after an update): the ELF is built from "
                             "the boot stream in it at 0x10000, and the file is the "
                             "CFI flash too (tools/cdj_gui/flash_boot.py)")
    parser.add_argument("--watch", action="append", default=[],
                        metavar="ADDRESS[:WORDS]",
                        help="also read this address back at the end, e.g. "
                             "0x489bd98:16 for the whole status record")
    # A table of pointers is worth nothing without what it points at, and the
    # targets are heap addresses that are not known until the table has been
    # read.  Splitting that over two runs reads two different heaps, so the
    # follow-up happens in the same monitor session: read the table, then send
    # a second batch of `xp` for every value that looks like a RAM pointer.
    parser.add_argument("--watch-chain", action="append", default=[],
                        metavar="ADDRESS:COUNT[:WORDS[:DEPTH]]",
                        help="read COUNT words at ADDRESS, then follow every "
                             "value that looks like a MAIN RAM pointer and "
                             "read WORDS words there, DEPTH levels deep.  One "
                             "monitor session, one heap.")
    # There is deliberately no --break here.  A gdb breakpoint stops the whole
    # machine at every hit, and this run exists to keep MAIN's timing races
    # intact -- the ten-second GuiCom deadline and the GUI's three-tick receive
    # timeout both lose under that.  tools.cdj_main.caution --live --trace runs
    # MAIN alone, where stopping it costs nothing.
    parser.add_argument("--firmware", default=None, metavar="IMAGE",
                        help="the MAIN flash image to boot instead of "
                             "firmware/main-firmware.bin, e.g. a --pmemsave "
                             "dump taken after an update")
    parser.add_argument("--qemu-arg", action="append", metavar="ARG",
                        help="an extra argument for qemu-system-sh4, repeatable "
                             "(--qemu-arg=-trace --qemu-arg=pflash_* logs the "
                             "flash model's commands into the -D log)")
    parser.add_argument("--pmemsave", default=None, metavar="START,SIZE,FILE",
                        help="before quitting, save SIZE bytes of guest physical "
                             "memory from START to FILE through the monitor -- "
                             "0,0x400000,flash.bin is the NOR flash after a "
                             "firmware update")
    parser.add_argument("--caution", action="store_true",
                        help="read MAIN's caution store back as well and decode "
                             "it -- which device failed, what is pending behind "
                             "the banner, and every code raised this boot")
    parser.add_argument("--poke-hold", type=float, default=1.0,
                        help="keep rewriting the pokes (0 writes them once, "
                             "which is how to tell whether the firmware clears "
                             "them again)")
    parser.add_argument("--poke-interval", type=float, default=1.0,
                        help="seconds between rewrites; lower it for a one-shot "
                             "flag the firmware consumes")
    parser.add_argument("--trace", metavar="ADDR[,ADDR]",
                        help="hold gdb breakpoints here and report pc/r4/r5/pr "
                             "per hit, repeats counted; w:ADDR[:LEN] is a "
                             "write watchpoint instead, reporting the storing "
                             "instruction.  Needed for anything "
                             "only the two-board run reaches -- MAIN alone "
                             "never receives a GUI request.  Mutually "
                             "exclusive with --poke: there is one gdb stub and "
                             "both would want it")
    parser.add_argument("--trace-max", type=int, default=2000, metavar="N",
                        help="drop a breakpoint after this many uniform hits")
    parser.add_argument("--trace-cap", type=int, default=400, metavar="N",
                        help="hard limit on the number of stops ONE address may "
                             "cost, whatever it reports.  --trace-max only "
                             "counts hits that said nothing new, so a site whose "
                             "arguments differ every time is never dropped: "
                             "r072 spent 231 173 stops on a single breakpoint "
                             "and measured the guest being held, not the "
                             "firmware.  0 disables the cap")
    parser.add_argument("--frames", metavar="DIR",
                        help="snapshot the live frame into DIR every "
                             "--frame-every seconds, named by elapsed wall "
                             "clock.  The simulator rewrites its PPM on every "
                             "completed frame (gui.c, curr_line == height), so "
                             "this yields before/after pairs from one run "
                             "instead of one run per input")
    parser.add_argument("--frame-every", type=float, default=2.0,
                        metavar="SECONDS")
    parser.add_argument("--gui-output", metavar="FILE",
                        help="keep the whole GUI-side stream (run_headless "
                             "plus the simulator's stderr).  Only the last 800 "
                             "characters are printed otherwise, which is fine "
                             "for a summary and useless for a trace")
    parser.add_argument("--main-output", metavar="FILE",
                        help="copy MAIN's -D log (the fixed path "
                             "TEMP/vm-main.log, which the next run deletes) "
                             "into the run's own directory.  Everything about "
                             "MAIN's side of the link -- armed/sent/delivered "
                             "and the CDJ_LINK_CENSUS lines -- lives only "
                             "there, so a run without this keeps no record of "
                             "whether MAIN was still transmitting")
    parser.add_argument("--poll-words", default="",
                        help="extra MAIN words to read on every --poll-every "
                             "round and print after the milestones, as "
                             "comma-separated physical addresses, e.g. "
                             "0x4c084d8,0x489bdbc -- the source flag and a "
                             "status-record halfword pair, for timing a key "
                             "against the record that carries it")
    parser.add_argument("--poll-every", type=float, default=0.0,
                        metavar="SECONDS",
                        help="while the boards run, read the milestone words "
                             "and the RTOS tick back every SECONDS and print a "
                             "line per round with the wall clock and the CPU "
                             "time of both emulators.  This is the boot "
                             "timeline; 0 (the default) reads only at the end")
    parser.add_argument("--poll-output", metavar="FILE",
                        help="write the --poll-every rounds as TSV")
    parser.add_argument("--no-gui", action="store_true",
                        help="run MAIN alone for --seconds: no simulator, no "
                             "link client.  With --poll-every this measures "
                             "the RTOS tick rate of the board by itself")
    parser.add_argument("--stderr", metavar="FILE",
                        help="keep MAIN's stderr here.  Without it the stream "
                             "goes to DEVNULL and CDJ_WATCH reports nothing -- "
                             "the watch writes to stderr, not to the -D log, so "
                             "a run looks like 'nobody writes that word' when "
                             "the evidence was simply thrown away")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.trace and args.poke:
        parser.error("--trace and --poke both need the gdb stub; run them "
                     "in separate runs")

    env = qemu_environment()
    if args.cosim:
        # The canned bootstrap records are a wall-clock stand-in for MAIN;
        # in one guest time MAIN answers from its first record on.
        args.no_peer = True
        env = dict(env, CDJ_COSIM=str(PORT + 5),
                   CDJ_COSIM_QUANTUM_US=str(args.cosim_quantum_us))
        print(f"# cosim: one guest time, link on {PORT + 5}, latency "
              f"{args.cosim_quantum_us} us, MAIN -icount shift={args.cosim_shift}")
    # The fixed names are what RUNNING.md and the notes refer to.  A run on a
    # non-default CDJ_LINK_PORT is a second machine beside the first, and it
    # must not delete or share the first one's logs.
    suffix = "" if PORT == 5980 else f"-{PORT}"
    main_log = TEMP / f"vm-main{suffix}.log"
    gui_log = TEMP / f"vm-gui{suffix}.log"
    console_log = TEMP / f"vm-console{suffix}.txt"
    frame = TEMP / f"vm-frame{suffix}.ppm"
    if args.gui_flash:
        from tools.cdj_gui.flash_boot import boot_from_flash

        booted = TEMP / f"gui-flash{suffix}"
        report = boot_from_flash(Path(args.gui_flash), booted)
        args.gui_elf = str(booted / "gui-boot-memory.elf")
        args.gui_board = str(booted / "gui-board.hw")
        shift = report["resource_bank0_shift"]
        if not any(v.startswith("BFIN_FAST_LZSS") for v in args.gui_env):
            # The accelerator reads the flash at the addresses its table names,
            # whatever is there: without the stock first bank in this flash it
            # is switched off, not left on the stock image run_headless
            # defaults to.
            args.gui_env += ([f"BFIN_FAST_LZSS={Path(args.gui_flash).resolve()}",
                              f"BFIN_FAST_LZSS_SHIFT={shift:#x}"] if shift is not None
                             else ["BFIN_FAST_LZSS="])
        print(f"# GUI:  from the flash {args.gui_flash}: {len(report['blocks'])} boot "
              f"blocks at 0x10000..{report['stream_end']}, entry {report['entry']}, "
              + (f"first resource bank {shift:+#x} from stock" if shift is not None
                 else "no stock resource bank (the GUI unpacks its own)")
              + " (the sector-0 loader is not run)")
    # A previous run that was killed rather than closed leaves its QEMU behind,
    # and that orphan still holds this log open.  Windows then refuses the
    # unlink with WinError 32, and an unhandled PermissionError names the file
    # but not the cause -- which is a confusing way to discover that the last
    # run never went away.  Say what is holding it and what to do about it.
    held = []
    for path in (main_log, gui_log, console_log, frame):
        if not path.exists():
            continue
        try:
            path.unlink()
        except PermissionError:
            held.append(path)
    if held:
        names = ", ".join(path.name for path in held)
        print(f"# {names} is still open: a previous run is still alive.",
              file=sys.stderr)
        print("#   tasklist | findstr /i \"qemu-system-sh4 cdj-run\"",
              file=sys.stderr)
        print("#   taskkill /F /T /IM qemu-system-sh4.exe",
              file=sys.stderr)
        print("#   taskkill /F /T /IM cdj-run.exe", file=sys.stderr)
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

    print(f"# MAIN: {QEMU.name} -M cdj2000-main")
    board_stderr = open(args.stderr, "wb") if args.stderr else subprocess.DEVNULL
    if args.stderr:
        print(f"# MAIN stderr -> {args.stderr}")
    board = subprocess.Popen(
        [
            str(QEMU), "-M", "cdj2000-main",
            "-bios", str(args.firmware or FIRMWARE / "main-firmware.bin"),
            "-display", "none", "-no-reboot", "-d", "unimp", "-D", str(main_log),
            "-serial", f"tcp:127.0.0.1:{PORT},server,nowait",
            "-serial", f"tcp:127.0.0.1:{PORT + 2},server,nowait",
            # MAIN's own console is the third SCIF (SCIF0, 0xffe00000).  It stays
            # off unless CDJ_DEBUG_CONSOLE asks for it, exactly as on a stock
            # player: any value logs it to a file, "tcp:HOST:PORT" serves it both
            # ways, so the 232C command processor and, after its "CM 9", the
            # stock Cente shell (tsk, d, lnk, ...) can be typed at.
            "-serial", console_chardev(os.environ.get("CDJ_DEBUG_CONSOLE"), console_log),
            "-monitor", f"telnet:127.0.0.1:{PORT + 1},server,nowait",
            # Reading is the monitor's job; writing is the stub's.  It is always
            # offered and costs nothing until something connects.
            "-gdb", f"tcp:127.0.0.1:{PORT + 3}",
            # --cosim: MAIN's time is its instruction count, and it waits
            # paused until the GUI connects, so both boards start at 0.
            *(["-icount", f"shift={args.cosim_shift},sleep=off", "-S"]
              if args.cosim else []),
            *(args.qemu_arg or []),
            # The card is inserted a while after reset on purpose: the poller at
            # 0x1ff164 arms its mount gate only while the slot is empty, and the
            # gate itself comes from the panel -- payload byte 17 bit 2 is the
            # slot switch, so the frame below is what "a card is in" means.
            *(["-drive", f"if=sd,format=raw,file={args.sd}"] if args.sd else []),
            # The stick is there from reset: the loader's updater waits only
            # three seconds for a drive letter (0x040215f4) before it hands
            # over to the application.
            *(["-drive", f"if=none,id=usbstick,format=raw,file={args.usb_stick}",
               "-device", "usb-storage,drive=usbstick,removable=on"]
              if args.usb_stick else []),
        ],
        env=dict(env, CDJ_TMU_FREQ=os.environ.get("CDJ_TMU_FREQ", "54000000"),
                 CDJ_SD_INSERT=os.environ.get("CDJ_SD_INSERT",
                                              "10" if args.sd else "25"),
                 # A press lands only if MAIN builds a status record while
                 # the key is down, every 3.05 s when nothing else changes;
                 # the board's own 300 ms never spans one.
                 CDJ_PANEL_HOLD_MS=os.environ.get("CDJ_PANEL_HOLD_MS", "3300"),
                 CDJ_PANEL_KEYS=keys,
                 CDJ_PANEL_FRAME=os.environ.get(
                     "CDJ_PANEL_FRAME",
                     "00000000000000000000000000000000000400000000"
                     if args.sd else "")),
        stdout=subprocess.DEVNULL, stderr=board_stderr,
        # The vCPU thread runs flat out and the main-loop thread has to get
        # the 1 ms tick out on time; above normal keeps a busy host from
        # delaying either.
        creationflags=(subprocess.ABOVE_NORMAL_PRIORITY_CLASS
                       if os.name == "nt" else 0),
    )
    gui = mon = None
    stop_pokes = threading.Event()
    pokes = None
    stop_trace = threading.Event()
    tracer = None
    trace_hits: dict[tuple[int, ...], list] = {}
    trace_at = []
    trace_watch = []
    for item in (args.trace.split(",") if args.trace else []):
        if item.startswith("w:"):
            parts = item[2:].split(":")
            trace_watch.append((int(parts[0], 0), int(parts[1], 0) if len(parts) > 1 else 4))
        else:
            trace_at.append(int(item, 0))
    if trace_at or trace_watch:
        tracer = threading.Thread(
            target=caution.trace_thread,
            args=(PORT + 3, trace_at, args.trace_max, stop_trace, trace_hits,
                  args.trace_cap, trace_watch),
            daemon=True)
        tracer.start()
        print("# trace: " + ", ".join("0x%08x" % a for a in trace_at)
              + "".join(" w:0x%08x:%d" % w for w in trace_watch))
    stop_frames = threading.Event()
    sampler = None
    sampler_report: dict = {}
    if args.frames:
        sampler = threading.Thread(
            target=frame_sampler,
            args=(frame, Path(args.frames), args.frame_every, stop_frames,
                  sampler_report),
            daemon=True)
        sampler.start()
        print(f"# frames: every {args.frame_every:g} s -> {args.frames}")
    try:
        time.sleep(3)
        mon = socket.create_connection(("127.0.0.1", PORT + 1), timeout=5)
        mon.settimeout(1.0)

        if args.poke:
            pokes = threading.Thread(
                target=poke_thread,
                args=(PORT + 3, args.poke, args.poke_at, args.poke_hold,
                      args.poke_interval, stop_pokes),
                daemon=True)
            pokes.start()
            print("# poke: " + ", ".join("0x%08x=%d" % p for p in args.poke)
                  + f" at {args.poke_at:g} s"
                  + (", held" if args.poke_hold > 0 else ", once"))

        if args.no_gui:
            print("# GUI:  none (--no-gui); MAIN runs alone")
            gui = subprocess.Popen(
                [sys.executable, "-c",
                 "import time, sys; time.sleep(float(sys.argv[1]))",
                 str(args.seconds)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        else:
            print(f"# GUI:  Blackfin simulator, link client on {PORT}")
        if args.gui_elf:
            # Which image ran belongs in the log, not in the invocation that
            # produced it: a run whose provenance has to be reconstructed from
            # a shell history is a run nobody can re-check.
            print(f"# GUI:  boot memory {args.gui_elf}")
            for value in args.gui_env:
                if value.startswith("BFIN_FAST_LZSS"):
                    print(f"# GUI:  {value}")
        gui = gui if args.no_gui else subprocess.Popen(
            [
                sys.executable, "-m", "tools.cdj_gui.run_headless",
                "--seconds", str(args.seconds),
                "--simulator", str(BFIN_SIM),
                *(["--elf", args.gui_elf] if args.gui_elf else []),
                *(["--board", args.gui_board] if args.gui_board else []),
                "--packet", str(PACKETS / "status-standalone.bin"),
                "--output", str(frame), "--log", str(gui_log),
                # The TX dump reopens its file per record; the default lands
                # inside the repository, and a cloud-sync client watching that
                # directory will fight the writer.
                "--tx-output", str(TEMP / f"vm-sport-tx{suffix}.bin"),
                *(["--ppi-delay", str(args.ppi_delay)] if args.ppi_delay else []),
                "--env", "BFIN_PARALLEL_WRITEBACK=1",
                "--env", "BFIN_GUI_COLOR=rgb555le",
                "--env", f"BFIN_MAIN_LINK=127.0.0.1:{PORT}",
                *(["--env", f"BFIN_COSIM=127.0.0.1:{PORT + 5}",
                   "--env", f"BFIN_COSIM_QUANTUM_US={args.cosim_quantum_us}",
                   # The 200-byte reads are the GUI draining the line after
                   # each record (0xb7ef40: it re-arms while bytes keep
                   # coming and stops when the DMA count stands still).  A
                   # quiet wire gives them nothing; zeros made it never go
                   # quiet and cost 3 s after every answer from MAIN.
                   "--env", "BFIN_SPORT_RX_ZERO_200=",
                   # A wire delivers what MAIN sent, once.  Repeating the last
                   # record and payload every retry was the wall clock's
                   # stand-in for a MAIN that is always ahead; in one guest
                   # time the GUI re-parsed the repeats, answered them, and
                   # MAIN retransmitted the update's first payload 38 times.
                   "--env", "BFIN_LINK_FRESH_ONLY=1"]
                  if args.cosim else []),
                # The GUI's interpreter is still thirty times slower than the
                # real chip on real work, so an announcement-plus-payload
                # transaction takes it longer than MAIN's status interval, and
                # the next plain record then lands on the validated
                # announcement: halfword 30 reads 0, (0-1)*2 underflows, the
                # checksum loop walks off the CPLB map and the board
                # double-faults at 0x00b99196.  Carrying the announcement onto
                # fresh records until the payload has gone over is what the
                # wire guarantees the firmware anyway.  Measured on the
                # wall-clock time base: without it four of six 90 s boots
                # faulted, with it none of three.  BFIN_LINK_ANNOUNCE_STICKY=
                # (empty) via --gui-env switches it off for an A/B.
                "--env", "BFIN_LINK_ANNOUNCE_STICKY=1",
                # The file-based peer is a bootstrap, not a second MAIN: it
                # answers only until the live link has a record.  Its records
                # are a capture of a real player with a USB stick, so while it
                # is answering the GUI shows USB state 1 whatever MAIN says --
                # which is exactly what makes a live change look like no change.
                # --no-peer takes it away and leaves only the board.
                *([] if args.no_peer else [
                    "--env", "BFIN_MAIN_PEER=1",
                    "--env", "BFIN_MAIN_PEER_STATUS=" + str(PACKETS / "status-usb-q.bin"),
                    "--env", "BFIN_MAIN_PEER_STATUS_HOLD=250",
                    "--env", "BFIN_MAIN_PEER_PAYLOAD_1=" + str(PACKETS / "t1-browse.bin"),
                ]),
                *sum((["--env", value] for value in args.gui_env), []),
            ],
            cwd=str(ROOT), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            # The simulator's stderr rides on this pipe when --log is not
            # given, and it is not cp1252; decode leniently or the reader
            # thread dies mid-run with a UnicodeDecodeError.
            encoding="utf-8", errors="replace",
        )
        poll_rows: list[tuple] = []
        poll_extra = [("0x%x" % int(text, 0), int(text, 0),
                       "xp /1wx 0x%08x" % int(text, 0))
                      for text in args.poll_words.split(",") if text.strip()]
        if args.poll_every > 0:
            # Elapsed is counted from the GUI launch; QEMU started ~3 s before.
            started = time.monotonic()
            next_poll = started
            previous = None
            deadline = started + args.seconds + 180
            while gui.poll() is None and time.monotonic() < deadline:
                now = time.monotonic()
                if now < next_poll:
                    time.sleep(min(0.2, next_poll - now))
                    continue
                next_poll += args.poll_every
                elapsed = now - started
                mon.settimeout(0.3)
                try:
                    words = monitor(mon, POLL_WATCH + poll_extra, settle=0.02)
                except OSError:
                    words = {}
                mon.settimeout(1.0)
                cpu = cpu_seconds()
                rtos = words.get(0x04FC45EC, -1)
                rate = -1.0
                if previous and rtos >= 0 and previous[1] >= 0:
                    rate = (rtos - previous[1]) / max(elapsed - previous[0], 1e-6)
                row = (round(elapsed, 1), words.get(0x04FE29F4, -1),
                       words.get(0x04C06FB0, -1), words.get(0x0489BCF4, -1),
                       words.get(0x0489BCF8, -1), rtos, round(rate, 1),
                       round(cpu.get("qemu-system-sh4", -1.0), 1),
                       round(cpu.get("cdj-run", -1.0), 1))
                poll_rows.append(row)
                print("# t=%6.1f panel=%d GuiCom=%d ready=%d/%d rtos=%d "
                      "(%+.0f/s) cpu qemu=%.1fs sim=%.1fs%s" % (
                          row[0], row[1], row[2], row[3], row[4], row[5],
                          row[6], row[7], row[8],
                          "".join(" %s=%08x" % (name, words.get(address, -1)
                                                & 0xffffffff)
                                  for name, address, _ in poll_extra)),
                      flush=True)
                previous = (elapsed, rtos)
            if gui.poll() is None:
                stop_tree(gui)
            output = gui.communicate()[0]
            if args.poll_output:
                with open(args.poll_output, "w", encoding="utf-8") as stream:
                    stream.write("\t".join(POLL_HEADER) + "\n")
                    for row in poll_rows:
                        stream.write("\t".join(str(v) for v in row) + "\n")
                print(f"# poll -> {args.poll_output} ({len(poll_rows)} rounds)")
        else:
            try:
                output = gui.communicate(timeout=args.seconds + 180)[0]
            except subprocess.TimeoutExpired:
                stop_tree(gui)
                output = gui.communicate()[0]
        # Only the tail is printed, because a healthy run ends in a few lines
        # of summary.  A traced run is the opposite: BFIN_ROUTER_TRACE and its
        # relatives write hundreds of lines to the simulator's stderr, and the
        # 800-character tail throws away exactly the early ones that say when
        # something first happened.  --gui-output keeps the whole stream.
        if args.gui_output:
            # run_headless prints two summary lines on its stdout and hands the
            # simulator's own stdout+stderr to --log, which is the FIXED path
            # TEMP/vm-gui.log -- so the next run overwrites it.  Writing only
            # `output` here produced a two-line "whole stream" in r131 and lost
            # every trace line the run was started for; the lines survived only
            # because they were copied out of TEMP by hand afterwards.  Both
            # halves go into the per-run file now, simulator first, so the file
            # named on the command line is the complete record of the run.
            simulator = (gui_log.read_text(errors="replace")
                         if gui_log.exists() else "")
            Path(args.gui_output).write_text(
                simulator + (output or ""), errors="replace")
            print(f"# GUI output -> {args.gui_output} "
                  f"({len(simulator)} B simulator + {len(output or '')} B runner)")
        print((output or "").strip()[-800:])

        stop_pokes.set()
        watch = WATCH + [("poked", address, "xp /1wx %#x" % address)
                         for address, _ in args.poke]
        extra = []
        for spec in args.watch:
            address, _, count = spec.partition(":")
            address, count = int(address, 0), int(count or "1", 0)
            extra.append((address, count))
            watch.append(("watch", address, "xp /%dwx %#x" % (count, address)))
        chains = []
        for spec in args.watch_chain:
            parts = spec.split(":")
            root = int(parts[0], 0)
            count = int(parts[1], 0) if len(parts) > 1 else 4
            span = int(parts[2], 0) if len(parts) > 2 else 8
            depth = int(parts[3], 0) if len(parts) > 3 else 1
            chains.append((root, count, span, depth))
            watch.append(("chain", root, "xp /%dwx %#x" % (count, root)))
        if args.caution:
            watch += [("caution", address, "xp /%dwx %#x" % (count, address))
                      for address, count in caution.regions()]
        words = monitor(mon, watch)
        chain_levels = []
        for root, count, span, depth in chains:
            frontier = [(root, count)]
            levels = [list(frontier)]
            seen = {root}
            for _ in range(depth):
                followed = []
                for base, length in frontier:
                    for i in range(length):
                        value = words.get(base + 4 * i, 0)
                        if not looks_like_ram(value) or value in seen:
                            continue
                        seen.add(value)
                        followed.append((value, span))
                followed = followed[:CHAIN_FANOUT]
                if not followed:
                    break
                words.update(monitor(mon, [
                    ("chain", address, "xp /%dwx %#x" % (length, address))
                    for address, length in followed]))
                levels.append(followed)
                frontier = followed
            chain_levels.append((root, levels))
        print("\n# MAIN, read back through the monitor")
        for label, address, _ in watch:
            if label != "watch":
                print(f"  {label:14s} 0x{address:08x} = {words.get(address, -1)}")
        for address, count in extra:
            cells = " ".join("%08x" % words.get(address + 4 * i, 0)
                             for i in range(count))
            print(f"  watch          0x{address:08x} = {cells}")
        for root, levels in chain_levels:
            print(f"  chain from     0x{root:08x}")
            for depth, level in enumerate(levels):
                for address, length in level:
                    cells = " ".join("%08x" % words.get(address + 4 * i, 0)
                                     for i in range(length))
                    print(f"    L{depth} 0x{address:08x} = {cells}")
        if args.caution:
            caution.report(words)
        if args.pmemsave:
            # The monitor splits its line on spaces, so the dump goes through
            # a path without any and is moved afterwards; and it reads the
            # size as an expression, so an unquoted "/Users/..." after it is
            # a division ("invalid char 'U' in expression").
            start, size, path = args.pmemsave.split(",", 2)
            staging = TEMP / f"pmemsave{suffix}.bin"
            mon.sendall(f'pmemsave {int(start, 0):#x} {int(size, 0):#x} '
                        f'"{staging.as_posix()}"\n'.encode())
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                time.sleep(0.5)
                if staging.exists() and staging.stat().st_size >= int(size, 0):
                    break
            time.sleep(0.5)
            if staging.exists():
                shutil.move(str(staging), path)
                print(f"# pmemsave {start} {size} -> {path}")
            else:
                print(f"# pmemsave failed: {staging} was not written")
        mon.sendall(b"quit\n")
        time.sleep(1.5)
    finally:
        stop_pokes.set()
        stop_frames.set()
        stop_trace.set()
        if sampler is not None:
            sampler.join(timeout=5)
            # A recording that stopped early has to say so.  Silence here is
            # what made r093 and r094 unreadable: an evaluator cannot tell a
            # window the sampler never reached from one where nothing moved.
            ticks = sampler_report.get("ticks", 0)
            last_tick = sampler_report.get("last_tick") or 0.0
            last_change = sampler_report.get("last_change")
            print("\n# frames: %d ticks, %d written, %d read/write errors; "
                  "last tick t%.1f, last change %s"
                  % (ticks, sampler_report.get("written", 0),
                     sampler_report.get("failures", 0), last_tick,
                     "t%.1f" % last_change if last_change is not None
                     else "never"))
            if ticks == 0 or last_tick < args.seconds - 3 * args.frame_every:
                print("# frames: INCOMPLETE -- the recording stops at t%.1f of "
                      "a %g s run, so any window after that is unmeasured, not "
                      "unchanged" % (last_tick, args.seconds))
            elif last_change is not None and last_change < last_tick - 30:
                print("# frames: the screen stood still from t%.1f to the end; "
                      "windows after that are measured and empty, which is a "
                      "result, not a hole (see index.tsv)" % last_change)
        if tracer:
            tracer.join(timeout=5)
        if trace_hits:
            print("\n# trace hits, first seen first, repeats counted")
            for key, record in trace_hits.items():
                entry, r4, r5, r6, r7, caller = key[:6]
                count, first, last = record
                peek = (" [r4+%s]=%#010x" % (os.environ.get("CDJ_TRACE_PEEK"), key[6])
                        if len(key) > 6 else "")
                print("  0x%08x  r4=%#010x r5=%#010x r6=%#010x r7=%#010x%s "
                      "from 0x%08x  x%-6d t%.1f..t%.1f"
                      % (entry, r4, r5, r6, r7, peek, caller, count, first, last))
            print("  -- per address: "
                  + ", ".join("0x%08x x%d" % (address, total)
                              for address, total in sorted(
                                  {k[0]: sum(v[0] for j, v in trace_hits.items()
                                             if j[0] == k[0])
                                   for k in trace_hits}.items())))
        elif trace_at:
            print("\n# trace: not one of the %d breakpoints was ever hit"
                  % len(trace_at))
        if mon:
            mon.close()
        stop_tree(gui)
        try:
            board.wait(timeout=20)
        except subprocess.TimeoutExpired:
            board.kill()

    if main_log.exists():
        text = main_log.read_text(errors="replace")
        print(f"\n# link: MAIN sent {text.count('link-tx: sent')} records, "
              f"received {text.count('link-rx: delivered')} requests; "
              f"panel exchanges started {text.count('cdj2000-panel')}+")
        census = [line for line in text.splitlines() if ": census t" in line]
        if census:
            print("# link census (CDJ_LINK_CENSUS), first and last of each half:")
            for name in sorted({line.split(":")[0] for line in census}):
                half = [line for line in census if line.startswith(name)]
                print("  " + half[0].strip())
                if len(half) > 1:
                    print("  " + half[-1].strip())
        if args.main_output:
            Path(args.main_output).write_text(text, errors="replace")
            print(f"# MAIN log -> {args.main_output} ({len(text)} B)")
    if gui_log.exists():
        log = gui_log.read_text(errors="replace")
        print("# GUI: " + ", ".join(f"{event} {log.count(event)}"
                                    for event in ("link-open", "link-tx", "link-rx")))
    if frame.exists():
        try:
            from PIL import Image
            Image.open(frame).save(args.output)
            print(f"\n# frame: {args.output}")
        except ImportError:
            print(f"\n# frame: {frame} (install pillow for PNG)")
    else:
        print("\n# no frame was captured")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
