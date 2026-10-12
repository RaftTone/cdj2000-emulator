"""Run MAIN and the NXS GUI together and load a track, the way that held.

The recipe is the one trackload-49..53 (September 2026) measured four of four:

* the card is given at launch (``boot_vm --sd``), so MAIN mounts it during boot;
* the SD SOURCE key is pressed at 60 s of guest time -- with the card alone the
  NXS GUI keeps browsing LINK and its cursor-3 polls collide with the ENTERs;
* three ENTERs (type 1 cursor 3, the list request the GUI sends for a press)
  at 90, 110 and 130 s of the injector's clock open the playlist tree down to
  the first track, and a LOAD (type 7 cursor 1) at 155 s loads it;
* MAIN's frames are queued 2 ms apart (``CDJ_LINK_RX_GAP_US``, the default),
  which is what stopped GuiCom_RcvTASK reading one frame twice.

Everything is a subprocess of tools already in the repo: ``boot_vm`` for MAIN,
``link_inject`` as the proxy that injects the requests, ``run_headless`` for
the GUI, ``panel_control`` for panel keys on a schedule.  The run directory
gets the logs, the frames, the request dump, MAIN's console and a README with
the exact command lines and environment, and it is never reused: an existing
directory is an error, so no measurement overwrites another.

    python -m tools.cdj_main.twoboard trackload-60 --card runs/nxs-swap/rbstick1g.img
    python -m tools.cdj_main.twoboard play-1 --card CARD --keys keys.txt \
        --env CDJ_DSP_ACK=1 --bootvm-arg=--trace=0x41bd304

A keys file has one ``SECONDS ARGS`` line per press, ``ARGS`` being what
``panel_control`` takes (``230 press 16.0`` is PLAY at 230 s after launch);
``#`` starts a comment.  ``--dry-run`` prints the plan and does nothing.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from tools.cdj_main.parent_watch import Lifeline
from tools.cdj_main.procs import stop_tree

REPO = Path(__file__).resolve().parents[2]
RUNS = REPO / "runs" / "nxs-swap"

# The recipe that held: ENTER, ENTER, ENTER, LOAD (injector clock = seconds
# after the GUI connected to the proxy).
DEFAULT_INJECTS = ("90:1:3:7:1:0", "110:1:3:7:1:0", "130:1:3:7:1:0", "155:7:1:0:0")
DEFAULT_ELF = "firmware/nxs/gui-boot-memory.elf"
DEFAULT_BOARD = "emulator/cdj2000-gui-nxs.hw"
DEFAULT_PACKET = "packets/status-standalone.bin"
DEFAULT_QEMU = "C:/qemu-src/build/qemu-system-sh4.exe"
GUI_ENV = ("BFIN_PARALLEL_WRITEBACK=1", "BFIN_GUI_COLOR=rgb555le",
           "BFIN_LINK_ANNOUNCE_STICKY=1", "BFIN_EXCEPTION_TRACE=1")


@dataclass
class Plan:
    """Every command line of a run, before anything is started."""
    run_dir: Path
    work_card: Path
    boot_vm: list[str]
    proxy: list[str]
    gui: list[str]
    keys: list[tuple[int, list[str]]]
    env: dict[str, str] = field(default_factory=dict)


def parse_keys(text: str) -> list[tuple[int, list[str]]]:
    """``SECONDS ARGS`` lines for panel_control, comments and blanks skipped."""
    keys = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        at, *args = line.split()
        keys.append((int(at), args))
    return sorted(keys, key=lambda item: item[0])


def build_plan(args: argparse.Namespace, environ: dict[str, str] | None = None) -> Plan:
    environ = dict(os.environ if environ is None else environ)
    run_dir = Path(args.runs_dir) / args.name
    card = Path(args.card)
    work_card = Path(args.work_dir) / (card.stem + "-work" + card.suffix)
    main_log = run_dir / "vm-main.log"
    env = {"CDJ_QEMU": environ.get("CDJ_QEMU", DEFAULT_QEMU),
           "CDJ_INPUT_PORT": str(args.input_port)}
    for item in args.env:
        name, _, value = item.partition("=")
        env[name] = value
    boot_vm = [sys.executable, "-m", "tools.cdj_main.boot_vm", "--no-gui", "--no-peer",
               "--seconds", str(args.seconds), "--poll-every", "10", "--caution",
               "--frames", str(run_dir / "frames"), "--frame-every", str(args.frame_every),
               "--sd", str(work_card),
               "--source-key", "sd", "--source-key-at", str(args.source_key_at),
               "--main-output", str(main_log), "--stderr", str(run_dir / "qemu-stderr.log")]
    if args.poll_words and args.poll_words != "none":
        boot_vm += ["--poll-words", args.poll_words, "--poll-output", str(run_dir / "poll.tsv")]
    boot_vm += list(args.bootvm_arg)
    proxy = [sys.executable, "-m", "tools.cdj_main.link_inject",
             "--listen-port", str(args.proxy_port), "--main-port", str(args.main_port),
             "--timeout", str(args.seconds + 20),
             "--request-dump", str(run_dir / "gui-requests.bin")]
    injects = list(args.inject) if args.inject else ([] if args.no_inject else list(DEFAULT_INJECTS))
    for item in injects:
        proxy += ["--inject", item]
    proxy += list(args.proxy_arg)
    gui = [sys.executable, "-m", "tools.cdj_gui.run_headless", "--elf", args.elf,
           "--board", args.board, "--packet", args.packet,
           "--seconds", str(max(args.seconds - 5, 1)),
           "--output", str(Path(tempfile.gettempdir()) / "vm-frame.ppm"),
           "--log", str(run_dir / "gui.log")]
    for item in GUI_ENV + (f"BFIN_MAIN_LINK=127.0.0.1:{args.proxy_port}",
                           f"BFIN_MAIN_LINK_DUMP={run_dir / 'main-link-dump.bin'}"):
        gui += ["--env", item]
    for item in args.gui_env:
        gui += ["--env", item]
    keys = parse_keys(Path(args.keys).read_text(encoding="utf-8")) if args.keys else []
    return Plan(run_dir, work_card, boot_vm, proxy, gui, keys, env)


def describe(plan: Plan) -> str:
    lines = [f"run dir: {plan.run_dir}", f"card work copy: {plan.work_card}",
             "env: " + " ".join(f"{k}={v}" for k, v in sorted(plan.env.items())),
             "boot_vm: " + " ".join(plan.boot_vm), "proxy: " + " ".join(plan.proxy),
             "gui: " + " ".join(plan.gui),
             "keys: " + ("; ".join(f"{at} {' '.join(a)}" for at, a in plan.keys) or "none")]
    return "\n".join(lines)


def _md5(path: Path) -> str:
    try:
        return hashlib.md5(path.read_bytes()).hexdigest()[:12]
    except OSError:
        return "?"


def _emulators_running() -> bool:
    if sys.platform != "win32":
        return False
    out = subprocess.run(["tasklist"], capture_output=True, text=True, check=False).stdout.lower()
    return "qemu-system" in out or "cdj-run" in out


def run(plan: Plan, card: Path) -> int:
    if plan.run_dir.exists():
        print(f"run dir exists, refusing to reuse it: {plan.run_dir}", file=sys.stderr)
        return 2
    if _emulators_running():
        print("a qemu-system or cdj-run process is already running", file=sys.stderr)
        return 2
    (plan.run_dir / "frames").mkdir(parents=True)
    plan.work_card.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(card, plan.work_card)
    env = dict(os.environ)
    env.update(plan.env)
    qemu = Path(env["CDJ_QEMU"])
    readme = [f"date: {dt.datetime.now():%Y-%m-%d %H:%M:%S}",
              f"qemu: {qemu} md5 {_md5(qemu)}",
              f"sim: bin/cdj-run.exe md5 {_md5(REPO / 'bin' / 'cdj-run.exe')}",
              f"card: {card} -> work copy {plan.work_card} (fresh), given at launch",
              "MAIN env: " + " ".join(f"{k}={v}" for k, v in sorted(env.items()) if k.startswith("CDJ_")),
              describe(plan), ""]
    (plan.run_dir / "README.txt").write_text("\n".join(readme), encoding="utf-8")
    start = time.monotonic()
    procs = []
    lifeline = Lifeline()
    try:
        with open(plan.run_dir / "boot_vm.log", "w", encoding="utf-8") as log:
            main = lifeline.popen(plan.boot_vm, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=REPO)
        procs.append(main)
        time.sleep(3)
        if main.poll() is not None:
            print("boot_vm exited at once; see boot_vm.log", file=sys.stderr)
            return 3
        with open(plan.run_dir / "proxy.log", "w", encoding="utf-8") as log:
            proxy = lifeline.popen(plan.proxy, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=REPO)
        procs.append(proxy)
        time.sleep(2)
        with open(plan.run_dir / "run_headless.out", "w", encoding="utf-8") as log:
            gui = lifeline.popen(plan.gui, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=REPO)
        procs.append(gui)
        panel = [sys.executable, "-m", "tools.cdj_main.panel_control", "--port", plan.env["CDJ_INPUT_PORT"]]
        for at, args in plan.keys:
            wait = at - (time.monotonic() - start)
            if wait > 0:
                time.sleep(wait)
            print(f"--- t{time.monotonic() - start:.0f} {' '.join(args)}", flush=True)
            subprocess.run(panel + args, env=env, cwd=REPO, check=False)
        gui.wait()
        main.wait()
        proxy.wait()
    finally:
        for process in procs:
            stop_tree(process)
    console = Path(tempfile.gettempdir()) / "vm-console.txt"
    if console.exists():
        shutil.copyfile(console, plan.run_dir / "vm-console.txt")
    summary(plan)
    return 0


def summary(plan: Plan) -> None:
    proxy_log = plan.run_dir / "proxy.log"
    if proxy_log.exists():
        print("=== proxy log:")
        print(proxy_log.read_text(encoding="utf-8", errors="replace").rstrip())
    gui_log = plan.run_dir / "gui.log"
    if gui_log.exists():
        text = gui_log.read_text(encoding="utf-8", errors="replace")
        faults = sum(text.count(word) for word in ("bfin fault", "illegal", "double fault", "_cec_raise"))
        print(f"=== GUI faults: {faults}")
    index = plan.run_dir / "frames" / "index.tsv"
    if index.exists():
        changed = [line.split("\t")[0] for line in index.read_text(encoding="utf-8").splitlines()
                   if "\tnew" in line]
        print("=== frames changed at: " + " ".join(changed))
    main_log = plan.run_dir / "vm-main.log"
    if main_log.exists():
        print("=== exchanges:")
        subprocess.run([sys.executable, "-m", "tools.cdj_main.link_exchanges", str(main_log)],
                       cwd=REPO, check=False)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("name", help="run directory name under --runs-dir; must not exist yet")
    parser.add_argument("--card", required=True, help="FAT32 card image with a rekordbox export; copied, never written")
    parser.add_argument("--seconds", type=int, default=260, help="wall-clock budget for MAIN (GUI gets 5 s less)")
    parser.add_argument("--keys", help="panel key schedule file: SECONDS ARGS per line")
    parser.add_argument("--inject", action="append", default=[], metavar="SECONDS:TYPE:CURSOR[:W3..]",
                        help="a GUI request for the proxy to inject; replaces the default recipe")
    parser.add_argument("--no-inject", action="store_true", help="inject nothing (browse by hand or not at all)")
    parser.add_argument("--source-key-at", type=int, default=60, help="guest second of the SD SOURCE key press")
    parser.add_argument("--frame-every", type=int, default=5)
    parser.add_argument("--poll-words", default="none",
                        help="MAIN words for boot_vm to poll every 10 s; 'none' keeps the gdb stub free for --trace")
    parser.add_argument("--bootvm-arg", action="append", default=[], help="extra boot_vm argument, e.g. --bootvm-arg=--trace=0x41bd304")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="MAIN board environment (CDJ_*)")
    parser.add_argument("--gui-env", action="append", default=[], metavar="NAME=VALUE", help="extra simulator environment (BFIN_*)")
    parser.add_argument("--elf", default=DEFAULT_ELF)
    parser.add_argument("--board", default=DEFAULT_BOARD)
    parser.add_argument("--packet", default=DEFAULT_PACKET)
    parser.add_argument("--runs-dir", default=str(RUNS))
    parser.add_argument("--work-dir", default=os.environ.get("CDJ_CARD_WORK_DIR", "C:/cdj-build/cards"),
                        help="where the card's work copy goes (the guest writes to it)")
    parser.add_argument("--input-port", type=int, default=5984)
    parser.add_argument("--main-port", type=int, default=5980)
    parser.add_argument("--proxy-port", type=int, default=5990)
    parser.add_argument("--proxy-arg", action="append", default=[],
                        help="extra link_inject argument, e.g. --proxy-arg=--nxs-prefix=beat")
    parser.add_argument("--dry-run", action="store_true", help="print the plan, start nothing")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    plan = build_plan(args)
    if args.dry_run:
        print(describe(plan))
        return 0
    return run(plan, Path(args.card))


if __name__ == "__main__":
    sys.exit(main())
