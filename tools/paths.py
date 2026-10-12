"""Where this checkout keeps the things a run needs.

None of these directories ship with the repository.  They are created by the
build and by the runs themselves, and they are all in .gitignore:

    firmware/   Boot images built from your own copy of the Pioneer firmware
                update -- gui-boot-memory.elf, gui-flash-image.bin,
                main-firmware.bin.  See FIRMWARE.md.
    bin/        The Blackfin simulator you built.  See BUILD.md.
    packets/    Stimulus the tools generate: status records, browse scenes,
                marker streams.  See RUNNING.md.
    runs/       What a run produces: frames, logs, transmit captures.

Every one of them can be moved with an environment variable, and every tool
takes an explicit path on the command line as well, so nothing here is more
than a default.

One exception, and it is worth knowing: the board files in emulator/ name their
flash image as `firmware/gui-flash-image.bin`, and GNU sim resolves that against
its working directory, which the launchers set to the repository root.  So
CDJ_FIRMWARE_DIR moves everything the Python side reads, but not the flash image
the simulator opens for itself.  To put that somewhere else, copy the board file
and pass it with `--board`.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

FIRMWARE = Path(os.environ.get("CDJ_FIRMWARE_DIR", REPO_ROOT / "firmware"))
PACKETS = Path(os.environ.get("CDJ_PACKETS_DIR", REPO_ROOT / "packets"))
RUNS = Path(os.environ.get("CDJ_RUNS_DIR", REPO_ROOT / "runs"))
BIN = Path(os.environ.get("CDJ_BIN_DIR", REPO_ROOT / "bin"))
BOARDS = REPO_ROOT / "emulator"

_EXE = ".exe" if os.name == "nt" else ""

# The Blackfin simulator, as scripts/build-bfin-sim.sh installs it.
BFIN_SIM = Path(os.environ.get("CDJ_BFIN_SIM", BIN / ("cdj-run" + _EXE)))

# The GUI board on the vendored Blackfin core (emulator/bfin/), as
# scripts/build-cdj-gui-run.sh installs it.  GUI-only so far.
GUI_RUN = Path(os.environ.get("CDJ_GUI_RUN", BIN / ("cdj-gui-run" + _EXE)))

# qemu-system-sh4 with the cdj2000-main board, as scripts/build-qemu-sh4.sh
# builds it.  A bare name is resolved through PATH; set CDJ_QEMU to the full
# path of the binary you built if it is not installed.
QEMU = Path(os.environ.get("CDJ_QEMU", "qemu-system-sh4" + _EXE))


def qemu_environment(base: dict | None = None) -> dict:
    """A copy of the environment QEMU can actually start in.

    A QEMU built under MSYS2 MINGW64 needs that toolchain's DLLs on PATH, and
    the directory is not on it by default.  CDJ_QEMU_DLL_DIR names it; the
    MSYS2 default is used when it exists and nothing is set.  On a system where
    neither applies -- a Linux build, or a Windows QEMU with its DLLs beside
    it -- the environment is handed back unchanged rather than polluted.
    """
    env = dict(os.environ if base is None else base)
    dll_dir = os.environ.get("CDJ_QEMU_DLL_DIR")
    if dll_dir is None and os.name == "nt":
        default = Path("C:/msys64/mingw64/bin")
        dll_dir = str(default) if default.is_dir() else None
    if dll_dir:
        env["PATH"] = dll_dir + os.pathsep + env.get("PATH", "")
    return env


def board_path(path, relative_to=REPO_ROOT) -> str:
    """A --hw-board-file argument GNU sim's own parser will not split.

    The simulator does not take that option as a plain argv string: it hands it
    to its internal hardware-command parser, which splits on whitespace.  A
    checkout under a directory with a space in its name therefore fails with

        Command `hw-file' requires only one argument

    and the board is never loaded -- the simulator then runs with no flash and
    double-faults a fraction of a second in, which looks nothing like a path
    problem.  No amount of quoting helps; the split happens after argv.

    The launchers run the simulator with its working directory set to the
    repository root, and no directory inside the repository has a space in its
    name.  So a board file inside the tree is passed relative and the parser
    sees one token.  A path outside the tree is passed whole, because a wrong
    path fails worse than one that merely might.
    """
    resolved = Path(path).resolve()
    try:
        return resolved.relative_to(Path(relative_to).resolve()).as_posix()
    except ValueError:
        return resolved.as_posix()
