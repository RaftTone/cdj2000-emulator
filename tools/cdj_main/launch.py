"""Select the emulator profile from one stable command-line entry point.

Examples::

    python -m tools.cdj_main.launch deck
    python -m tools.cdj_main.launch deck --usb path/to/rekordbox-usb.img
    python -m tools.cdj_main.launch 2000 --sd runs/card.img
    python -m tools.cdj_main.launch nxs runs/nxs-demo --sd runs/card.img --ui

``deck`` is the NXS profile with the defaults an interactive session wants
(see ``deck_arguments``); anything given on its command line wins.  The
profile-specific launchers retain their native arguments and help text;
this module only selects the correct one.
"""

# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import os
import sys
from collections.abc import Sequence


def backend(model: str):
    """Return the profile launcher without starting a board."""

    if model == "2000":
        from tools.cdj_main import view_vm

        return view_vm
    if model == "nxs":
        from tools.cdj_main import nxs_vm

        return nxs_vm
    raise ValueError(f"unsupported emulator profile: {model}")


MEDIA_OPTIONS = ("--sd", "--usb", "--test-track", "--disc")
SOURCE_OPTIONS = ("--source-key", "--source-key-at", "--source-key-retries")


def deck_arguments(forwarded: Sequence[str], environ=os.environ) -> list[str]:
    """nxs_vm arguments for an interactive deck: the viewer, QMP/GDB, an hour,
    playback DSP, media (CDJ_USB, else a generated test track) pressed once
    the media manager is ready.  Ports and the run directory are already
    automatic in nxs_vm, and so are fault-only DSP capture, the DSP thread
    and the fast GUI core."""

    given = {item.split("=", 1)[0] for item in forwarded}
    extra = []
    for option in ("--ui", "--debug"):
        if option not in given:
            extra.append(option)
    if "--seconds" not in given:
        extra += ["--seconds", "3600"]
    if "--dsp-model" not in given and "--functional-dsp-audio" not in given:
        extra.append("--functional-dsp-audio")
    if not given & set(MEDIA_OPTIONS):
        usb = environ.get("CDJ_USB")
        extra += ["--usb", usb] if usb else ["--test-track"]
        given.add("--usb" if usb else "--test-track")
    # The readiness press needs an SD or USB source (--test-track is SD).
    if (given & {"--sd", "--usb", "--test-track"}
            and not given & {*SOURCE_OPTIONS, "--source-key-when-ready"}):
        extra.append("--source-key-when-ready")
    return [*extra, *forwarded]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Launch either the original CDJ-2000 or experimental CDJ-2000NXS "
            "two-board emulator. Profile arguments are passed through."
        )
    )
    parser.add_argument(
        "model",
        choices=("deck", "2000", "nxs"),
        help="deck: the NXS deck with interactive defaults; 2000 uses view_vm; "
             "nxs uses nxs_vm with its own defaults",
    )
    parser.add_argument(
        "profile_args",
        nargs=argparse.REMAINDER,
        help="arguments accepted by the selected profile launcher",
    )
    args = parser.parse_args(argv)
    forwarded = list(args.profile_args)
    if forwarded[:1] == ["--"]:
        forwarded.pop(0)

    if args.model == "deck":
        forwarded = deck_arguments(forwarded)
        args.model = "nxs"
    launcher = backend(args.model)
    # Both profile launchers intentionally parse their own options. Replacing
    # argv keeps their existing diagnostics, defaults and --help output intact.
    old_argv = sys.argv
    sys.argv = [f"{old_argv[0]} {args.model}", *forwarded]
    try:
        return launcher.main()
    finally:
        sys.argv = old_argv


if __name__ == "__main__":
    raise SystemExit(main())
