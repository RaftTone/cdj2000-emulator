"""Reproducible C674x audit sweeps: compact space, control registers, rollback.

These three measurements were first made with throwaway scratch programs, whose
numbers no one could regenerate once the session ended.  An audit number that
cannot be reproduced is not evidence, so the sweeps live in
``tests/cstub/c674x-isa-probe.c`` and this module drives them and records the
result with the hashes of everything that produced it.

    python -m tools.cdj_dsp.audit_sweeps analysis/dsp/audit_sweeps.json

What each sweep does and does not establish:

* ``compact`` - all 65,536 16-bit words under 13 named compact-header
  configurations and 3 named register profiles.  A word counts as accepted if any
  configuration accepts it, and the record names which.  This is DECODE
  ACCEPTANCE, not semantics.  The defensible coverage figure is the count of
  words rejected as "compact instruction not implemented": it is a property of
  the decoder alone, unlike the accepted total, which moves with the probe's
  memory window and register values.
* ``control`` - drives a real ``MVC`` in both directions for all 32 control
  register ids, using encodings taken from TI's assembler.  The recorded mask is
  what reads back after writing all ones, so it is directly comparable to the
  reserved-bit columns of the SPRUFE8B per-register field tables.
* ``atomicity`` - a rejected execute packet must leave CPU state and memory
  bit-identical.  Failures here would mean a partially applied packet.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "tests" / "cstub" / "c674x-isa-probe.c"
CORE = (ROOT / "emulator" / "qemu" / "cdj_c674x.c",
        ROOT / "emulator" / "qemu" / "cdj_c674x_uncond.c",
        ROOT / "emulator" / "qemu" / "cdj_c674x_mpy.c", ROOT / "emulator" / "qemu" / "cdj_c674x_dotp.c", ROOT / "emulator" / "qemu" / "cdj_c674x_packed8.c", ROOT / "emulator" / "qemu" / "cdj_c674x_packed16.c", ROOT / "emulator" / "qemu" / "cdj_c674x_packbits.c", ROOT / "emulator" / "qemu" / "cdj_c674x_mpy32.c", ROOT / "emulator" / "qemu" / "cdj_c674x_dp.c", ROOT / "emulator" / "qemu" / "cdj_c674x_approx.c",
        ROOT / "emulator" / "qemu" / "cdj_c674x_sp.c",
        ROOT / "emulator" / "qemu" / "cdj_c674x_control.c",
        ROOT / "emulator" / "qemu" / "cdj_c674x_loop.c")


def build(workdir: Path) -> Path:
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("no C compiler on PATH")
    binary = workdir / "audit-probe"
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator" / "qemu"), str(PROBE),
         *[str(p) for p in CORE], "-o", str(binary)],
        check=True, capture_output=True, text=True)
    return binary


def run(binary: Path, mode: str) -> str:
    # atomicity exits nonzero when a rollback fails, which is a result, not an error.
    return subprocess.run([str(binary), mode], capture_output=True, text=True,
                          timeout=1800).stdout


# Mnemonics GNU names for the compact software-loop family.  These need an active
# software loop around them, which a one-instruction probe packet cannot provide,
# so the core refusing them in isolation says nothing about whether they work.
# tools/cdj_dsp/isa_probe.py excludes the same family from the 32-bit sweep for
# the same reason.
LOOP_FAMILY = frozenset({"sploop", "sploopd", "sploopw", "spkernel", "spkernelr",
                         "spmask", "spmaskr"})

# Figure F-32 Sx1b register BNOP with s = 0 is refused on purpose while the
# manual's self-contradiction is unresolved; see the Sx1b arm in cdj_c674x.c.
DELIBERATE = frozenset({"bnop"})

# Compact formats whose figure HARDWIRES the s bit, where GNU libopcodes does
# not enforce it.  GNU happily names an instruction for the s = 0 twin of each
# form below, and those names were read as a decode gap.  They are not: the
# figure fixes bit 0, and TI's own assembler refuses the corresponding source.
#
#   Figure C-19 Dx5p (printed page 730) draws bit 0 as a literal "1" with "s=1"
#   written underneath - contrast Figure C-18 Dx5 on the same page, whose bit 0
#   is an unconstrained "s" - and notes "src2 = dst = B15".  The ADDAW
#   description (printed page 123) says the s bit picks both unit and register
#   file: "s = 1 indicates the unit is D2 and dst is in the B register file", so
#   a B15 destination forces s = 1, and Dx5p has no x bit to cross with.
#   cl6x -mv6740 refuses "ADDAW .D1 B15,4,B15" with E0800 "Functional unit
#   specifier disagrees with operation" and "SUBAW .D1 B15,4,B15" with E0800
#   "Functional unit side requested does not match side needed"; both assemble
#   on .D2.  GNU prints "addaw .D1X b15,0,b15", a cross-path WRITE no C674x
#   unit can perform.
#
#   Figure F-31 Sx1 op 110 (printed page 756) spells the restriction out in the
#   mnemonic table itself: "MVC (.unit) src, ILC (s = 1)".  cl6x refuses
#   "MVC .S1 B0,ILC" with W0005 "Operation requires .S2 unit".
#
# Refusing these is correct behaviour, so they belong with the undefined
# encodings and not in a gap bucket.  The core's refusals are pinned by
# tests/cstub/c674x.c; narrowing either guard to admit s = 0 would accept an
# encoding hardware rejects.  Each entry is (label, mask, match).
UNIT_RESTRICTED = (
    ("addaw/subaw, Figure C-19 Dx5p s = 0", 0x1c7f, 0x0c76),
    ("mvc ILC, Figure F-31 op 110 s = 0", 0xfc7f, 0xd86e),
)


def classify_rejected(words, disassembler, workdir):
    """Name each rejected 16-bit word with GNU's TI C6x disassembler.

    Without this, "compact instruction not implemented" counts four very
    different things as one number: encodings the architecture does not define at
    all (which the core is RIGHT to refuse), forms whose figure hardwires s = 1
    so the s = 0 twin is equally undefined (ditto), instructions that need a
    software loop around them, and genuine gaps.  The raw count has been quoted
    as a coverage figure and it overstates the gap by two orders of magnitude.

    GNU is the naming oracle here, not the legality oracle.  It names the s = 0
    twins listed in UNIT_RESTRICTED and prints impossible cross-path writes for
    them, so a bucket must not be assigned from a GNU name alone.
    """
    import struct
    names = {}
    header = 0xE0200000          # mixed fetch packet, slot 0 short
    batch = 4096
    for start in range(0, len(words), batch):
        chunk = words[start:start + batch]
        blob = bytearray()
        for word in chunk:
            packet = bytearray(32)
            struct.pack_into("<H", packet, 0, word)
            struct.pack_into("<I", packet, 28, header)
            blob += packet
        image = workdir / "compact-reject.bin"
        image.write_bytes(bytes(blob))
        addresses = "".join(f"{0x11800000 + i * 32:#x}\n" for i in range(len(chunk)))
        result = subprocess.run(
            [str(disassembler), str(image), "0x11800000", "--stdin"],
            input=addresses, text=True, capture_output=True, timeout=600)
        for line, word in zip(result.stdout.splitlines(), chunk):
            text = line.split("\t")[1] if "\t" in line else ""
            tokens = text.split()
            first = tokens[0] if tokens else "<undefined>"
            # GNU emits "<undefined>" for an encoding it does not recognise; the
            # angle bracket is the reliable marker once the line is split.
            if first.startswith("<"):
                names[word] = "<undefined>"
            elif first.startswith("["):
                # A compact predicate precedes the mnemonic: keep both, so the
                # report distinguishes Figure H-6 from Figure H-5, and let
                # mnemonic() strip the predicate for bucketing.
                names[word] = " ".join(tokens[:2])
            else:
                names[word] = first
    return names


def compact(text: str, disassembler=None, workdir=None) -> dict:
    accepted, rejects, configs = {}, collections.Counter(), collections.Counter()
    not_implemented = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == "accept":
            accepted[parts[0]] = (parts[2], parts[3])
            configs[f"{parts[2]}/{parts[3]}"] += 1
        elif len(parts) >= 3:
            reason = " ".join(parts[2:])
            rejects[reason] += 1
            if reason == "compact instruction not implemented":
                not_implemented.append(int(parts[0], 16))
    words = len(accepted) + sum(rejects.values())
    report = {
        "words_swept": words,
        "accepted": len(accepted),
        "rejected": sum(rejects.values()),
        "rejection_reasons": dict(rejects.most_common()),
        "accepting_configurations": dict(configs.most_common()),
        "not_implemented_raw": len(not_implemented),
    }

    if disassembler and not_implemented:
        names = classify_rejected(sorted(not_implemented), disassembler, workdir)
        buckets = collections.Counter()
        for word in not_implemented:
            name = names.get(word, "<undefined>")
            if name == "<undefined>":
                buckets["undefined-encoding"] += 1
            elif name in DELIBERATE:
                buckets["deliberately-fail-closed"] += 1
            elif name in LOOP_FAMILY:
                # Unpredicated Figure H-5/H-7/H-8 forms only.  The predicated
                # Figure H-6 name keeps its "[a0]"/"[b0]" prefix and so falls
                # through to genuine-gap below, which is where it belongs: it
                # selects the SPLOOP RELOAD capability (SPLOOPD description,
                # printed page 485, "it indicates that the loop is a nested loop
                # using the SPLOOP reload capability"), and cdj_c674x_step
                # refuses it by name with "SPLOOPD reload not implemented".
                buckets["software-loop-family"] += 1
            elif any(word & mask == match for _, mask, match in UNIT_RESTRICTED):
                buckets["unit-restricted-encoding"] += 1
            else:
                buckets["genuine-gap"] += 1
        by_mnemonic = collections.Counter(names.get(w, "<undefined>") for w in not_implemented)
        report["not_implemented_breakdown"] = {
            "classified_by": "GNU libopcodes TI C6x disassembler, one word per compact fetch packet",
            "buckets": dict(buckets.most_common()),
            "by_mnemonic": dict(by_mnemonic.most_common()),
            "what_each_means": {
                "undefined-encoding": "the disassembler names no instruction; the architecture "
                                      "does not define it and refusing it is correct, not a gap",
                "software-loop-family": "implemented, but needs an active software loop that a "
                                        "one-instruction probe packet cannot provide",
                "deliberately-fail-closed": "refused on purpose while a manual "
                                            "self-contradiction is unresolved",
                "unit-restricted-encoding": "the disassembler names an instruction, but the "
                                            "SPRUFE8B figure hardwires the s bit for that form "
                                            "and TI's assembler refuses the s = 0 source; "
                                            "refusing it is correct, not a gap. See "
                                            "UNIT_RESTRICTED in this module for the citations",
                "genuine-gap": "an instruction the architecture defines, that we do not execute",
            },
            "unit_restricted_forms": [label for label, _, _ in UNIT_RESTRICTED],
        }
        report["defensible_coverage_figure"] = {
            "metric": "16-bit words that are a genuine compact gap",
            "value": buckets["genuine-gap"],
            "of": words,
            "why": ("The raw not-implemented count conflates undefined encodings and "
                    "s-bit-restricted forms, which the core is right to refuse, with real "
                    "gaps. Only the genuine-gap bucket is a coverage figure, and it is no "
                    "longer a DECODE gap: what remains is Figure H-6 Uspldr, the predicated "
                    "SPLOOPD that selects the SPLOOP reload capability, which "
                    "cdj_c674x_step recognises by name and refuses as not implemented."),
        }
    else:
        report["defensible_coverage_figure"] = {
            "metric": "16-bit words rejected as \"compact instruction not implemented\" "
                      "(UPPER BOUND: undefined encodings and loop-context refusals not separated)",
            "value": len(not_implemented),
            "of": words,
            "why": ("Independent of the probe's memory window and register values, but an "
                    "upper bound only: pass --disassembler to separate undefined "
                    "encodings and the software-loop family from real gaps."),
        }
    return report


def control(text: str) -> dict:
    rows = {}
    for line in text.splitlines():
        m = re.match(r"\s*(\d+) write=(\w+) read=(\w+) mask=([0-9a-f]{8})", line)
        if m:
            rows[int(m.group(1))] = {
                "write": m.group(2), "read": m.group(3), "read_mask": m.group(4),
            }
    readable = sorted(i for i, r in rows.items() if r["read"] == "accept")
    writable = sorted(i for i, r in rows.items() if r["write"] == "accept")
    unmasked = sorted(i for i, r in rows.items()
                      if r["read"] == "accept" and r["read_mask"] == "ffffffff")
    return {
        "ids_swept": len(rows),
        "readable_ids": readable,
        "writable_ids": writable,
        "absent_ids": sorted(set(range(32)) - set(readable) - set(writable)),
        "read_accept_but_unmasked_ids": unmasked,
        "note": ("read_mask is what reads back after writing 0xffffffff, so a mask "
                 "of ffffffff means the model stores every bit.  Compare each mask "
                 "against that register's SPRUFE8B field table: any bit the manual "
                 "marks Reserved must read 0."),
        "rows": {str(k): v for k, v in sorted(rows.items())},
    }


def atomicity(text: str) -> dict:
    cases, failures = [], None
    for line in text.splitlines():
        m = re.match(r"(\S+)\s+execute=(\d)\s+state_unchanged=(\d)\s+memory_unchanged=(\d)", line)
        if m:
            cases.append({"case": m.group(1), "executed": m.group(2) == "1",
                          "state_unchanged": m.group(3) == "1",
                          "memory_unchanged": m.group(4) == "1"})
        m = re.match(r"atomicity failures=(\d+)", line)
        if m:
            failures = int(m.group(1))
    return {"cases": cases, "failures": failures}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    ap.add_argument("--disassembler", type=Path,
                    help="built tools/cdj_dsp/tic6x_disasm.c frontend; without it the "
                         "compact not-implemented count is reported as an upper bound only")
    args = ap.parse_args(argv)

    import tempfile
    with tempfile.TemporaryDirectory(prefix="cdj-audit-sweeps-") as tmp:
        binary = build(Path(tmp))
        report = {
            "schema": 1,
            "provenance": {
                "probe_source": str(PROBE.relative_to(ROOT)),
                "probe_source_sha256": sha(PROBE),
                "core_sources": {str(p.relative_to(ROOT)): sha(p) for p in CORE},
                "generator_sha256": sha(Path(__file__)),
            },
            "compact": compact(run(binary, "compact"), args.disassembler, Path(tmp)),
            "control_registers": control(run(binary, "control")),
            "packet_atomicity": atomicity(run(binary, "atomicity")),
        }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps({
        "compact_not_implemented": report["compact"]["defensible_coverage_figure"]["value"],
        "compact_accepted": report["compact"]["accepted"],
        "control_readable": len(report["control_registers"]["readable_ids"]),
        "control_absent": len(report["control_registers"]["absent_ids"]),
        "control_unmasked": report["control_registers"]["read_accept_but_unmasked_ids"],
        "atomicity_failures": report["packet_atomicity"]["failures"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
