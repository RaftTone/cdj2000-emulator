"""Run the CDJ-2000 GUI firmware headlessly for scripted experiments.

``view_ui`` drives the same simulator behind a tkinter window, which makes it
unusable for automated A/B runs.  This launcher takes the same environment,
adds the state-read range trace, and stops after a fixed wall-clock budget.
"""

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

from tools.cdj_main.parent_watch import inherited_fds

from tools.paths import (BFIN_SIM, BOARDS, FIRMWARE, PACKETS, REPO_ROOT,
                         RUNS, board_path)


def simulator_path(path: Path) -> str:
    """Return a path GNU sim's hardware parser will not unescape."""

    return path.resolve().as_posix()


def run(args: argparse.Namespace) -> int:
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)

    env = os.environ.copy()
    env.update(
        {
            "BFIN_GUI_OUTPUT": str(output),
            "BFIN_GUI_HEIGHT": str(args.height),
            "BFIN_FAST_LZSS": str((FIRMWARE / "gui-flash-image.bin").resolve()),
            # The CDJ panel is little-endian RGB555.  The board file says so, but
            # machs.c finishes bfin_ppi@0 before --hw-board-file is parsed, so the
            # device never sees the property and falls back to RGB565 — which
            # renders every palette-blitted background as green/pink streaks.
            "BFIN_GUI_COLOR": "rgb555le",
            "BFIN_PARALLEL_WRITEBACK": "1",
            "BFIN_GPIO5_READY_TOGGLE": "1",
            "BFIN_SPORT_RX_INPUT": str(args.packet.resolve()),
            "BFIN_SPORT_RX_RECORDS": "1",
            "BFIN_SPORT_RX_ZERO_200": "1",
            "BFIN_SPORT_TX_OUTPUT": str(args.tx_output.resolve()),
        }
    )
    if args.ppi_delay:
        env["BFIN_PPI_DMA_DELAY"] = str(args.ppi_delay)
    if args.state_start is not None and args.state_end is not None:
        env["BFIN_GUI_STATE_READ_START"] = str(args.state_start)
        env["BFIN_GUI_STATE_READ_END"] = str(args.state_end)
    if args.write_start is not None and args.write_end is not None:
        env["BFIN_GUI_STATE_WRITE_START"] = str(args.write_start)
        env["BFIN_GUI_STATE_WRITE_END"] = str(args.write_end)
    for assignment in args.env:
        name, _, value = assignment.partition("=")
        if value == "":
            # The simulator tests these with getenv(), so an empty string is
            # still "set".  `--env NAME=` therefore has to remove it, which is
            # the only way to switch off a default such as
            # BFIN_SPORT_RX_ZERO_200 from the outside.
            env.pop(name, None)
        else:
            env[name] = value

    command = [
        simulator_path(args.simulator),
        "--model",
        "bf531",
        "--environment",
        "operating",
        "--memory-region",
        "0,64M",
        "--hw-board-file",
        board_path(args.board),
        *args.sim_option,
        simulator_path(args.elf),
    ]

    log_path = args.log.resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("wb") as log_stream:
        process = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=env,
            # No console for the simulator: the UART model polls stdin, and on
            # MinGW that read blocks inside the run loop with a console attached.
            stdin=subprocess.DEVNULL,
            pass_fds=inherited_fds(),  # the launcher's lifeline; see parent_watch
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            creationflags=(
                (subprocess.CREATE_NO_WINDOW
                 | subprocess.ABOVE_NORMAL_PRIORITY_CLASS)
                if sys.platform == "win32" else 0
            ),
        )
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            time.sleep(0.25)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    print(f"log={log_path} ({log_path.stat().st_size} bytes)")
    if output.exists():
        print(f"frame={output} ({output.stat().st_size} bytes)")
    else:
        print("frame: none produced")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--simulator", type=Path, default=BFIN_SIM
    )
    parser.add_argument("--elf", type=Path, default=FIRMWARE / "gui-boot-memory.elf")
    parser.add_argument(
        "--board", type=Path, default=BOARDS / "cdj2000-gui.hw"
    )
    parser.add_argument(
        "--packet", type=Path, default=PACKETS / "main-records-neutral-runtime.bin"
    )
    parser.add_argument("--output", type=Path, default=RUNS / "headless-screen.ppm")
    parser.add_argument("--tx-output", type=Path, default=RUNS / "headless-tx.bin")
    parser.add_argument("--log", type=Path, default=RUNS / "headless.log")
    parser.add_argument("--height", type=int, default=255)
    parser.add_argument("--ppi-delay", type=int, default=0,
                        help="ticks per display scanline (BFIN_PPI_DMA_DELAY); "
                             "0, the default, lets the simulator pace the "
                             "display per frame at BFIN_PPI_FPS on its "
                             "wall-clock time base")
    parser.add_argument(
        "--seconds", type=float, default=60.0, help="wall-clock run budget"
    )
    parser.add_argument(
        "--state-start", type=lambda value: int(value, 0), default=None
    )
    parser.add_argument("--state-end", type=lambda value: int(value, 0), default=None)
    parser.add_argument(
        "--write-start", type=lambda value: int(value, 0), default=None
    )
    parser.add_argument("--write-end", type=lambda value: int(value, 0), default=None)
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="extra simulator environment variable; may be repeated",
    )
    parser.add_argument(
        "--sim-option",
        action="append",
        default=[],
        metavar="OPTION",
        help="extra argument for the simulator itself, e.g. --mmu-skip-cplbs; "
        "may be repeated",
    )
    args = parser.parse_args()
    for name in ("simulator", "elf", "board", "packet"):
        path = getattr(args, name)
        if not path.exists():
            parser.error(f"{name} does not exist: {path}")
    return args


def main() -> int:
    from tools.cdj_main.parent_watch import exit_with_parent
    exit_with_parent()
    return run(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
