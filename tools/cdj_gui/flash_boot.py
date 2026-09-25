# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the GUI from what is in its flash, not from an update file.

    python -m tools.cdj_gui.flash_boot FLASH.bin OUTDIR [--at 0x10000]

The simulator does not run a boot ROM: it loads an ELF.  Everything else
builds that ELF from a C2KGUI.UPD, so "the update wrote the flash, and the
board starts from the flash" was never tested -- a stream that parses in the
file but was programmed wrong, or that only the file's copy of it can boot,
would pass.  This builds the ELF from the flash image itself: the Blackfin
boot stream the GUI's updater programs at flash 0x10000 (run update-30, and
every BFIN_CFI_DUMP since), parsed block by block as the BF533-family boot
ROM does -- 10-byte headers, zero-fill, ignore, the final block -- into the
memory it leaves, with the entry the final block names.  It writes

  gui-boot-memory.elf   what the simulator loads
  gui-board.hw          emulator/cdj2000-gui.hw with this flash as the CFI part
  flash-boot.json       the blocks, the stream's end, where the first resource
                        bank is and so the LZSS accelerator's shift

What it cannot do: the 64 KiB in front of the body is the board's own
first-stage loader, which no update carries and this repository has no dump
of, so its part of the chain (and anything it checks) is not run.  INIT blocks
are loaded like any other block and not called: the simulator's memory is
there without the SDRAM set-up they do on the board.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from .firmware import (BLACKFIN_BF531_ENTRY_POINT, BLACKFIN_BF533_ENTRY_POINT,
                       blocks_memory_spans, build_blackfin_elf, parse_boot_stream)

ROOT = Path(__file__).resolve().parents[2]
BODY_AT = 0x10000          # where the GUI's updater programs the body
# The first packed resource bank of the stock image: the LZSS accelerator's
# table in the simulator (bfin_fast_lzss) starts with it at 0x200ebba0.
BANK0_AT = 0xEBBA0
BANK0_PROBE = 4096


def bank0_shift(image: bytes) -> int | None:
    """Where the stock first bank sits in IMAGE, relative to its stock place;
    None when it is not there (a GUI with other resources: the accelerator
    then declines and the GUI unpacks them itself, slower but right).

    Not the difference of the streams' ends: a mod that grows its stream into
    the 64 KiB gap in front of the tail leaves the banks where they were
    (NEW FIRMWARE rc5: stream 0x2206 longer, first bank still at 0xEBBA0), and
    a wrong shift makes the accelerator decline silently."""
    probe = (ROOT / "firmware/gui-flash-image.bin").read_bytes()[BANK0_AT:BANK0_AT + BANK0_PROBE]
    if image[BANK0_AT:BANK0_AT + BANK0_PROBE] == probe:
        return 0
    at = image.find(probe)
    return at - BANK0_AT if at >= 0 else None


def boot_from_flash(flash: Path, out: Path, at: int = BODY_AT) -> dict:
    image = flash.read_bytes()
    blocks, end = parse_boot_stream(image[at:])
    entry = (BLACKFIN_BF533_ENTRY_POINT if blocks[-1].uses_bf533_reset_vector
             else BLACKFIN_BF531_ENTRY_POINT)
    spans = blocks_memory_spans(blocks)
    out.mkdir(parents=True, exist_ok=True)
    (out / "gui-boot-memory.elf").write_bytes(build_blackfin_elf(spans, entry))

    board = (ROOT / "emulator/cdj2000-gui.hw").read_text()
    board, n = re.subn(r'"firmware/gui-flash-image\.bin"', f'"{flash.resolve()}"', board)
    if n != 1:
        raise SystemExit("flash_boot: cannot find the flash line in emulator/cdj2000-gui.hw")
    (out / "gui-board.hw").write_text(board)

    shift = bank0_shift(image)
    report = {
        "flash": str(flash.resolve()),
        "stream_at": f"0x{at:x}",
        "stream_end": f"0x{at + end:x}",
        "blocks": [{"target": f"0x{b.target:08x}", "count": b.count,
                    "flags": b.flag_names()} for b in blocks],
        "entry": f"0x{entry:08x}",
        "spans": [f"0x{s.address:08x}-0x{s.end:08x}" for s in spans],
        "resource_bank0_shift": shift,
    }
    (out / "flash-boot.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("flash", type=Path, help="a 2 MiB GUI flash image or BFIN_CFI_DUMP")
    parser.add_argument("out", type=Path)
    parser.add_argument("--at", type=lambda v: int(v, 0), default=BODY_AT,
                        help="flash offset of the boot stream (0x10000)")
    args = parser.parse_args(argv)
    report = boot_from_flash(args.flash, args.out, args.at)
    print(f"{len(report['blocks'])} blocks, stream ends at {report['stream_end']}, "
          f"entry {report['entry']}, first resource bank "
          + (f"shifted {report['resource_bank0_shift']:+#x}"
             if report['resource_bank0_shift'] is not None else "not the stock one"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
