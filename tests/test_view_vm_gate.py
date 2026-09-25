"""`view_vm` must not boot two boards for a window that cannot click everything.

`view_ui --coverage` has been able to answer this from the start: it prints
`46 of 46 inputs have a control` and exits non-zero on any gap.  Nothing called
it.  So when the window silently fell back to 38 of 46 -- the four top keys
built `disabled`, and the eight analogue fields hidden behind one spinbox -- the
number that would have said so was never computed by anything a person runs.

The gate added to `view_vm` closes that, and this file tests **the gate rather
than the window**: no tkinter, no simulator, no QEMU.  The property that matters
is an ordering one -- the check has to be ahead of the first `Popen`, because a
gap found after the boot has already cost the boot.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.cdj_main import view_vm  # noqa: E402
from tools.cdj_gui import view_ui   # noqa: E402


class CoverageGateTests(unittest.TestCase):
    def test_the_window_covers_the_board_today(self) -> None:
        """The gate's own input, measured.  If this fails, the gate is right to
        refuse and the fix belongs in `view_ui.py`, not here."""
        reached, missing, stray = view_ui.coverage()
        self.assertEqual(missing, [], view_ui.coverage_line())
        self.assertEqual(stray, [], view_ui.coverage_line())
        self.assertEqual(len(reached), len(view_ui.panel_control.input_ids()))

    def test_the_gate_passes_and_says_the_number(self) -> None:
        said: list[str] = []
        self.assertEqual(view_vm.coverage_gate(report=said.append), 0)
        self.assertTrue(any("of" in line and "inputs have a control" in line
                            for line in said), said)

    def test_a_gap_names_the_input_and_refuses(self) -> None:
        """Simulated at `coverage()`, which is the only thing the gate reads."""
        said: list[str] = []
        with mock.patch.object(view_ui, "coverage",
                               return_value=(["19.0"], ["20.1", "field7"], [])):
            code = view_vm.coverage_gate(report=said.append)
        self.assertEqual(code, view_vm.COVERAGE_REFUSED)
        text = "\n".join(said)
        self.assertIn("20.1", text)
        self.assertIn("field7", text)
        self.assertIn("refusing to start", text)

    def test_a_control_for_an_input_the_board_lacks_also_refuses(self) -> None:
        """The direction a hand-written table gets wrong first: a button that
        claims a bit the payload decoder does not have.  It presses nothing and
        looks like it worked."""
        said: list[str] = []
        with mock.patch.object(view_ui, "coverage",
                               return_value=(["19.0"], [], ["21.7"])):
            code = view_vm.coverage_gate(report=said.append)
        self.assertEqual(code, view_vm.COVERAGE_REFUSED)
        self.assertIn("21.7", "\n".join(said))
        self.assertIn("does not decode", "\n".join(said))


class GateRunsBeforeAnythingStartsTests(unittest.TestCase):
    """The ordering property, enforced by making every process start explode."""

    def _explode(self, *_args, **_kwargs):
        raise AssertionError("a process was started before the coverage gate")

    def test_main_refuses_without_starting_a_process(self) -> None:
        said = io.StringIO()
        with mock.patch.object(view_ui, "coverage",
                               return_value=([], ["19.0"], [])), \
             mock.patch.object(view_vm.subprocess, "Popen", self._explode), \
             mock.patch.object(view_vm.subprocess, "run", self._explode), \
             mock.patch.object(view_vm.socket, "create_connection", self._explode), \
             mock.patch.object(view_vm.time, "sleep", self._explode), \
             mock.patch.object(sys, "argv", ["view_vm"]), \
             contextlib.redirect_stdout(said):
            code = view_vm.main()
        self.assertEqual(code, view_vm.COVERAGE_REFUSED)
        self.assertIn("19.0", said.getvalue())

    def test_ignore_coverage_still_prints_the_gap(self) -> None:
        """The escape hatch must not be a silent one: `--ignore-coverage` starts
        the machine, but the missing input is on the transcript either way."""
        said = io.StringIO()
        started: list[str] = []

        def note_popen(*_args, **_kwargs):
            started.append("popen")
            raise SystemExit(0)                  # stop before the real work

        # view_vm refuses to start without a MAIN flash image (QEMU would drop
        # a missing -bios silently); CI has no firmware, so hand it a stand-in.
        with tempfile.NamedTemporaryFile(suffix=".bin") as image, \
             mock.patch.object(view_ui, "coverage",
                               return_value=([], ["19.0"], [])), \
             mock.patch.object(view_vm.subprocess, "Popen", note_popen), \
             mock.patch.object(sys, "argv",
                               ["view_vm", "--ignore-coverage",
                                "--firmware", image.name]), \
             contextlib.redirect_stdout(said):
            with self.assertRaises(SystemExit):
                view_vm.main()
        self.assertEqual(started, ["popen"])
        self.assertIn("19.0", said.getvalue())

    def test_no_control_is_not_a_way_around_the_gate(self) -> None:
        """`--no-control` makes the buttons inert, which is a statement about
        the channel and not about whether the buttons exist."""
        with mock.patch.object(view_ui, "coverage",
                               return_value=([], ["19.0"], [])), \
             mock.patch.object(view_vm.subprocess, "Popen", self._explode), \
             mock.patch.object(view_vm.subprocess, "run", self._explode), \
             mock.patch.object(sys, "argv", ["view_vm", "--no-control"]), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(view_vm.main(), view_vm.COVERAGE_REFUSED)

    def test_an_unimportable_window_refuses_rather_than_guesses(self) -> None:
        """If the coverage cannot be computed it cannot be promised."""
        said: list[str] = []
        with mock.patch.dict(sys.modules, {"tools.cdj_gui.view_ui": None}), \
             mock.patch.object(view_ui, "coverage",
                               side_effect=AssertionError("must not be called")):
            with mock.patch("builtins.__import__",
                            side_effect=ImportError("no tkinter")):
                code = view_vm.coverage_gate(report=said.append)
        self.assertEqual(code, view_vm.COVERAGE_REFUSED)
        self.assertIn("cannot check", "\n".join(said))


if __name__ == "__main__":
    unittest.main()
