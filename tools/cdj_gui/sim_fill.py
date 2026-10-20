"""CDJ_GUI_SDRAM_FILL: what the GUI board's SDRAM holds when the simulator starts.

The simulator's memory comes up zeroed, a real SDRAM does not.  A GUI buffer that is read before it is
written then works here and not on the deck.  ``CDJ_GUI_SDRAM_FILL=<byte>`` (``00``..``ff`` in hex, or
``0x``-prefixed) makes the launchers pass ``--memory-fill`` to the simulator, *before* ``--memory-region``
because the option only applies to regions added after it.  Unset or empty: nothing changes.

Run a scenario with ``00``, ``ff`` and ``a5`` and compare; a difference is a read of memory nobody wrote.
The MAIN board has the same switch, ``CDJ_SDRAM_FILL`` (RUNNING.md, Environment).
"""

# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

from collections.abc import Mapping

ENV_NAME = "CDJ_GUI_SDRAM_FILL"


def memory_fill_args(env: Mapping[str, str]) -> list[str]:
    """The simulator arguments for the fill in ``env``; empty when it is not set."""

    spec = env.get(ENV_NAME, "").strip()
    if not spec:
        return []
    try:
        value = int(spec, 16)
    except ValueError:
        value = -1
    if not 0 <= value <= 0xFF:
        raise SystemExit(f"{ENV_NAME}={spec!r}: expected one byte in hex (00..ff)")
    return ["--memory-fill", str(value)]
