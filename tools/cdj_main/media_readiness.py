"""Read-only NXS media readiness observer for an existing debug run.

This module never changes guest state.  It reads the documented MAIN globals
through QMP and reports readiness, card presence, and the mount callback latch.
Reads are non-atomic while the guest runs. Those observations do not prove
that the browser has listed files. Addresses apply to the stock NXS layout.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import time
from pathlib import Path

from tools.cdj_main.qmp import Qmp, QmpError

MODE_STATE = 0x04CF2180
TABLE_BASE = 0x04CF2854
SLOT_STRIDE = 0xA0
SD_SLOT = 2
USB_SLOT = 3
TABLE_ENTRY = TABLE_BASE + SD_SLOT * SLOT_STRIDE
USB_TABLE_ENTRY = TABLE_BASE + USB_SLOT * SLOT_STRIDE
MODE_FALLBACK = 0x04CF222C
LATCH = 0x049832EC
DEVICE = 0x049832F0
SDHI_INFO1 = 0xFFE4001C
FLAGS_BYTE = 0x051E21D0

_VALUE = re.compile(r"0x([0-9a-fA-F]{1,8})")


def parse_word(reply: str) -> int:
    """Extract the value from QEMU HMP ``xp`` output, ignoring its echo."""
    rows = re.findall(r"^[0-9a-fA-F]{8,16}:((?:\s+0x[0-9a-fA-F]{1,8})+)\s*$",
                      reply or "", re.MULTILINE)
    if not rows:
        raise ValueError("QMP memory response has no data row")
    values = _VALUE.findall(rows[0])
    if not values:
        raise ValueError("QMP memory response has no word")
    return int(values[0], 16)


def readiness_snapshot(read_word, source: str = "sd") -> dict:
    """Read the selected source's readiness without changing guest state.

    SD retains the historical multi-arm/mount-latch predicate.  USB uses the
    proven source table entry: value 1 is intermediate and value 2 means the
    browser source is ready.
    """
    if source not in ("sd", "usb"):
        raise ValueError("source must be 'sd' or 'usb'")
    if source == "usb":
        entry = read_word(USB_TABLE_ENTRY)
        result = {
            "source": "usb",
            "table_base": TABLE_BASE,
            "slot": USB_SLOT,
            "table_entry_address": USB_TABLE_ENTRY,
            "table_entry": entry,
            "intermediate": entry == 1,
            "ready": entry == 2,
            "ok": entry == 2,
            "browse_proven": False,
            "meaning": "USB source table: 1=initializing, 2=browser ready",
        }
        return result
    mode = read_word(MODE_STATE)
    table = read_word(TABLE_ENTRY)
    fallback = read_word(MODE_FALLBACK)
    latch = read_word(LATCH)
    info1 = read_word(SDHI_INFO1 & ~3)
    flags_word = read_word(FLAGS_BYTE & ~3)
    device = read_word(DEVICE)
    media_mode = 1 if mode in (4, 5) else fallback
    arms = {"state == 3": mode == 3,
            "table entry nonzero": table != 0,
            "media mode == 0": media_mode == 0}
    card_present = not bool((info1 >> 5) & 1)
    gate_ready = bool(any(arms.values()) and card_present and latch == 1)
    # The mount gate is an intermediate state: a real NXS browse capture saw
    # mode 2/table 1 answer NO CARD, then mode 3/table 2 list files.  Keep
    # mount readiness distinct from source-key readiness.
    browser_source_ready = bool(mode == 3 and table == 2 and
                                card_present and latch == 1)
    result = {
        "source": "sd",
        "mode_state": mode, "table_entry": table,
        "media_mode_fallback": fallback, "media_mode": media_mode,
        "readiness_arms": arms, "ready": any(arms.values()),
        "latch": latch, "card_detect_bit": (info1 >> 5) & 1,
        "card_present": card_present,
        "mount_latched": latch == 1,
        "gate_ready": gate_ready, "ok": gate_ready,
        "browser_source_ready": browser_source_ready,
        "device": device,
        "flags_byte": (flags_word >> (8 * (FLAGS_BYTE & 3))) & 0xff,
        "browse_proven": False,
        "meaning": "readiness and mount latch only; filesystem browse is unverified",
    }
    if 0x04000000 <= device < 0x05000000:
        callback = read_word(device + 0x1c)
        status_word = read_word((device + 0x66) & ~3)
        result.update(callback=callback,
                      status_byte=(status_word >> (8 * ((device + 0x66) & 3))) & 0xff)
    return result


def source_key_ready(snapshot: dict) -> bool:
    """Whether a source press can open the selected browser table."""
    if snapshot['source'] == 'sd':
        return bool(snapshot.get('browser_source_ready'))
    return bool(snapshot['ok'])


def observe_run(run: Path, timeout: float = 0, poll: float = 0.25,
                source: str = "sd", for_source_key: bool = False) -> dict:
    manifest = json.loads((run / "run.json").read_text())
    if not isinstance(manifest, dict) or manifest.get("profile") != "experimental NXS":
        raise ValueError("run manifest is not an experimental NXS profile")
    endpoint = manifest.get("endpoints", {}).get("qmp")
    if not isinstance(endpoint, str) or not endpoint:
        raise ValueError("run has no QMP endpoint; launch with --debug")
    from tools.cdj_main.qmp import parse_endpoint
    endpoint = parse_endpoint(endpoint, relative_to=run)
    if not math.isfinite(timeout) or timeout < 0 or timeout > 3600:
        raise ValueError("timeout must be 0..3600 seconds")
    if not math.isfinite(poll) or poll <= 0 or poll > 60:
        raise ValueError("poll must be >0 and <=60 seconds")
    # One-shot sampling still has a finite total I/O budget.
    deadline = time.monotonic() + (timeout if timeout else 3)
    with Qmp(endpoint, timeout=min(3, timeout or 3)) as qmp:
        def read_word(address):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("media readiness sampling deadline expired")
            qmp.timeout = min(3, remaining)
            return parse_word(qmp.command("human-monitor-command", {
                "command-line": f"xp /1wx 0x{address:x}"}))
        while True:
            result = readiness_snapshot(read_word, source)
            ready = source_key_ready(result) if for_source_key else result['ok']
            if ready or timeout == 0 or time.monotonic() >= deadline:
                return result
            time.sleep(min(poll, max(0, deadline - time.monotonic())))
            if time.monotonic() >= deadline:
                return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("source", nargs="?", choices=("sd", "usb"), default="sd")
    parser.add_argument("--timeout", type=float, default=0)
    parser.add_argument("--poll", type=float, default=0.25)
    args = parser.parse_args(argv)
    try:
        result = observe_run(args.run.resolve(), args.timeout, args.poll, args.source)
        print(json.dumps(result, indent=2, sort_keys=True))
    except (OSError, ValueError, KeyError, json.JSONDecodeError, QmpError,
            TimeoutError, ConnectionError) as error:
        parser.exit(2, f"media-readiness: {error}\n")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
