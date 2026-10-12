"""Run the GUI board GUI-only on cdj-gui-run, the vendored Blackfin core.

    python -m tools.cdj_gui.gui_run [--boot IMAGE] [--flash FLASH]
                                    [--seconds 60] [--output screen.ppm]

The fast counterpart of ``run_headless`` / ``nxs_vm``'s GUI process with no
MAIN: the same firmware (by default firmware/nxs/gui-flash-image.bin, the
flash bin/cdj-run's board file maps, whose boot stream is what
gui-boot-memory.elf was built from) and the same frame file -- a P6 PPM of the
PPI's 480x255 capture in rgb555le, cropped to 480x234 by the viewers.

--seconds is *virtual* time (400 MHz core cycles), not wall time: the core
runs as fast as it can, unpaced.  A mod's update body (e.g. a build/merged-gui/gui-body.bin) or a C2KGUI.UPD boots directly with --boot.
There is no MAIN link yet; the GUI reaches E-8709 as it does under bin/cdj-run
with no MAIN.
"""

# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools.paths import FIRMWARE, GUI_RUN, RUNS


def command(boot: Path, output: Path, seconds: float, flash: Path | None = None,
            runner: Path = GUI_RUN, extra: list[str] | None = None) -> list[str]:
    cmd = [str(runner), "-s", repr(seconds), "-o", str(output)]
    if flash is not None:
        cmd += ["-f", str(flash)]
    return cmd + list(extra or []) + [str(boot)]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--runner", type=Path, default=GUI_RUN)
    parser.add_argument("--boot", type=Path,
                        default=FIRMWARE / "nxs" / "gui-flash-image.bin",
                        help="UPD, update body, bare LDR or 2 MiB flash image")
    parser.add_argument("--flash", type=Path, default=None,
                        help="separate flash image for the resources")
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="virtual seconds to run")
    parser.add_argument("--output", type=Path, default=RUNS / "gui-run" / "screen.ppm")
    args = parser.parse_args(argv)
    for name in ("runner", "boot", "flash"):
        path = getattr(args, name)
        if path is not None and not path.exists():
            parser.error(f"{name} does not exist: {path}"
                         + ("; run sh scripts/build-cdj-gui-run.sh" if name == "runner" else ""))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(command(args.boot, args.output, args.seconds, args.flash,
                                    args.runner), stdin=subprocess.DEVNULL)
    print(f"frame={args.output}" if args.output.exists() else "frame: none produced")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
