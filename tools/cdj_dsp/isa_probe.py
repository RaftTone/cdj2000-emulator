"""Measure which manual-derived C674x encodings our decoder accepts.

The inventory comes from SPRUFE8B (see :mod:`tools.cdj_dsp.isa_inventory`), the
encodings come from TI's own assembler, and the verdicts come from
``emulator/qemu/cdj_c674x.c`` through ``tests/cstub/c674x-isa-probe.c``.  Nothing
in this path is derived from our decoder, so the result is a measurement of our
coverage against the manual rather than a restatement of what we already decode.

What this establishes, and only this:

* ``accept``  - the core fetched and executed the encoding without rejecting it.
  It is NOT evidence that the architectural result is correct.  Semantic
  fidelity comes from the reference-backed tests, never from this probe.
* ``execute-reject`` with "instruction not implemented" - the encoding is
  genuinely unsupported.  That is a hard, citable coverage gap.
* ``no-encoding`` - we could not synthesise an assembler-legal form for the
  manual syntax, so the instruction is UNKNOWN here, not unsupported.

Scope limits worth stating before anybody quotes a number from the output:

* One representative encoding per syntax form per unit.  A single Table A-1 row
  can cover dozens of opfields and operand types; accepting one does not mean
  accepting all of them.
* 32-bit encodings only.  Compact (16-bit) forms need the header word and the
  Appendix C-H maps and are assessed separately.
* Same-side operands only, so cross-path variants are not probed here.
* Predication, parallelism and multi-instruction execute packets are not probed.

Usage::

    python -m tools.cdj_dsp.isa_probe analysis/dsp/isa_probe.json \\
        --ti-bin /Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INDEX = ROOT / "build" / "references" / "sprufe8b.txt"
INVENTORY = ROOT / "analysis" / "dsp" / "isa_manual_inventory.json"
PROBE_SOURCE = ROOT / "tests" / "cstub" / "c674x-isa-probe.c"
CORE_SOURCES = (
    ROOT / "emulator" / "qemu" / "cdj_c674x.c",
    ROOT / "emulator" / "qemu" / "cdj_c674x_uncond.c",
    ROOT / "emulator" / "qemu" / "cdj_c674x_mpy.c", ROOT / "emulator" / "qemu" / "cdj_c674x_dotp.c", ROOT / "emulator" / "qemu" / "cdj_c674x_packed8.c", ROOT / "emulator" / "qemu" / "cdj_c674x_packed16.c", ROOT / "emulator" / "qemu" / "cdj_c674x_packbits.c", ROOT / "emulator" / "qemu" / "cdj_c674x_mpy32.c", ROOT / "emulator" / "qemu" / "cdj_c674x_dp.c", ROOT / "emulator" / "qemu" / "cdj_c674x_approx.c",
    ROOT / "emulator" / "qemu" / "cdj_c674x_sp.c",
    ROOT / "emulator" / "qemu" / "cdj_c674x_control.c",
    ROOT / "emulator" / "qemu" / "cdj_c674x_loop.c",
)

# A syntax block ends where the next documented section begins.
_STOP = re.compile(
    r"^\s*(Opcode|Compact Instruction Format|Description|Execution|Table \d)\b")
_UNIT_SPEC = re.compile(r"\((?:\.unit|\.[LMSD][12](?:\s+or\s+\.[LMSD][12])*)\)")
_TRAILING_NOTE = re.compile(r"\s*\((?:if|when)\b[^)]*\)\s*$")

# Operand placeholders, longest first so "src2_h:src2_l" is replaced before
# "src2".  Registers stay on one side: cross-path forms are out of scope here.
_OPERANDS = [
    ("src1_o:src1_e", "{S}5:{S}4"),
    ("src2_o:src2_e", "{S}7:{S}6"),
    ("src_o:src_e", "{S}5:{S}4"),
    ("dst_o:dst_e", "{S}9:{S}8"),
    ("src1_h:src1_l", "{S}5:{S}4"),
    ("src2_h:src2_l", "{S}7:{S}6"),
    ("src_h:src_l", "{S}5:{S}4"),
    ("dst_h:dst_l", "{S}9:{S}8"),
    ("*+baseR[offsetR]", "*+{S}4[{S}6]"),
    ("*+baseR[ucst5]", "*+{S}4[4]"),
    ("*+B14/B15[ucst15]", "*+B14[4]"),
    ("*+baseR[ucst15]", "*+B14[4]"),
    ("B14/B15", "B14"),
    ("ucst15", "4"),
    ("ucst5", "4"),
    ("scst5", "4"),
    ("ucst4", "4"),
    ("csta", "4"),
    ("cstb", "4"),
    ("cst16", "4"),
    ("cst21", "4"),
    ("cst5", "4"),
    ("ucst3", "3"),
    ("cst", "4"),
    ("offsetR", "{S}6"),
    ("baseR", "{S}4"),
    ("src1", "{S}4"),
    ("src2", "{S}6"),
    ("dst", "{S}5"),
    ("src", "{S}4"),
    ("label", "$"),
    ("fstg", "0"),
    ("fcyc", "0"),
    ("count", "1"),
    ("ii", "4"),
    ("n", "1"),
]

# Operand bindings tried per syntax form, in order.  "scalar" is the plain
# register binding; "pairs" covers the 64-bit operand classes (dint, dp, i2);
# "dst-pair" and "src-pair" cover the mixed forms such as MPYSPDP and LDDW;
# "last-const" covers operands the syntax writes as a register name but the
# encoding requires as a small constant, such as the BNOP delay count.
_BINDINGS = [
    ("scalar", []),
    ("pairs", [("src1", "{S}5:{S}4"), ("src2", "{S}7:{S}6"), ("dst", "{S}9:{S}8")]),
    ("dst-pair", [("dst", "{S}9:{S}8")]),
    ("src2-pair", [("src2", "{S}7:{S}6")]),
    ("src1-pair", [("src1", "{S}5:{S}4")]),
    ("src-pair", [("src", "{S}9:{S}8")]),
    ("src12-pairs", [("src1", "{S}5:{S}4"), ("src2", "{S}7:{S}6")]),
    ("src2-dst-pairs", [("src2", "{S}7:{S}6"), ("dst", "{S}9:{S}8")]),
    ("src1-dst-pairs", [("src1", "{S}5:{S}4"), ("dst", "{S}9:{S}8")]),
]

# Instructions whose manual syntax names an operand class the substitution table
# cannot guess.  Each entry is an assembler-legal form; the comment says why the
# generic path cannot produce it.
_FALLBACKS = {
    # dst is a control register, named not numbered (SPRUFE8B Table 2-6).
    "MVC": ["MVC {U} {S}4, AMR", "MVC {U} AMR, {S}5"],
    # src2 is the return-pointer register implied by the mnemonic.
    "B IRP": ["B {U} IRP"],
    "B NRP": ["B {U} NRP"],
    # The branch register form takes a register, not a label.
    "B register": ["B {U} {S}4"],
    # ADDKPC's second operand is a label-relative constant and the third a NOP
    # count, which the manual writes as "src1, dst, src2".
    "ADDKPC": ["ADDKPC {U} $, B5, 0"],
    # src1 is a PC-relative displacement, not a register.
    "BDEC": ["BDEC {U} $, {S}5"],
    "BPOS": ["BPOS {U} $, {S}5"],
    # The return register is fixed to A3 on side 1 and B3 on side 2.
    "CALLP": ["CALLP {U} $, {S}3"],
    # The operand is a unit vector, not a register or a constant.
    "SPMASK": ["SPMASK L1"],
    "SPMASKR": ["SPMASKR L1"],
}

# The software-loop family needs surrounding loop structure before the assembler
# will accept it, and it already has dedicated reference-backed tests
# (tests/cstub/c674x-loop.c, tests/cstub/c674x-spkernel-fields.c).  Probing it
# one instruction at a time would report assembler artefacts as coverage gaps,
# so it is excluded here and audited through those tests instead.
_OUT_OF_PROBE_SCOPE = {
    "SPLOOP": "software-loop family: needs loop structure; see tests/cstub/c674x-loop.c",
    "SPLOOPD": "software-loop family: needs loop structure; see tests/cstub/c674x-loop.c",
    "SPLOOPW": "software-loop family: needs loop structure; see tests/cstub/c674x-loop.c",
    "SPKERNEL": "software-loop family: needs loop structure; see tests/cstub/c674x-spkernel-fields.c",
    "SPKERNELR": "software-loop family: needs loop structure; see tests/cstub/c674x-spkernel-fields.c",
}


def pages(index: Path) -> dict[int, str]:
    out = {}
    for block in index.read_text(encoding="utf-8").split("@@ PDFPAGE ")[1:]:
        head, _, body = block.partition("\n")
        out[int(head.split()[0])] = body
    return out


def syntax_block(page_text: dict[int, str], span: list[int]) -> list[str]:
    text = "\n".join(page_text.get(p, "") for p in range(span[0], span[1] + 1))
    start = text.find("Syntax")
    if start < 0:
        return []
    lines = []
    for line in text[start:].splitlines():
        if lines and _STOP.match(line):
            break
        lines.append(line.rstrip())
        if len(lines) > 25:
            break
    return lines


def syntax_forms(name: str, block: list[str]) -> tuple[list[str], list[str]]:
    """Return (syntax forms, units) exactly as the manual writes them.

    Syntax sections are sometimes laid out in two columns (the register-offset
    and constant-offset loads, for instance), so each line can carry more than
    one form; a run of three or more spaces is the column gap.
    """
    base = name.split()[0]
    pattern = re.compile(rf"\b{re.escape(base)}\b")
    forms: list[str] = []
    units: list[str] = []
    for raw in block:
        line = raw
        if line.strip().startswith("Syntax"):
            line = line.replace("Syntax", " " * len("Syntax"), 1)
        for match in re.finditer(r"unit\s*=\s*(.+)$", line):
            for unit in re.findall(r"\.[LMSD][12]", match.group(1)):
                if unit not in units:
                    units.append(unit)
            if "none" in match.group(1) and "none" not in units:
                units.append("none")
        for match in pattern.finditer(line):
            form = re.split(r"\s{3,}", line[match.start():])[0].strip()
            form = _TRAILING_NOTE.sub("", form).strip().rstrip(",")
            if form and form not in forms:
                forms.append(form)
    return forms, sorted(units)


def candidates(name: str, forms: list[str], units: list[str]) -> list[dict]:
    """Turn manual syntax into assembler-legal source lines.

    One candidate per (syntax form, unit).  The fallback table replaces the
    generic substitution for the handful of instructions whose operands are
    named classes rather than registers or constants.
    """
    base = name.split()[0]
    out = []
    fallback = _FALLBACKS.get(name) or _FALLBACKS.get(base)
    for unit in units or ["none"]:
        side = "B" if unit.endswith("2") else "A"
        if fallback:
            for text in fallback:
                out.append({
                    "unit": unit, "syntax": "(audit fallback form)",
                    "source": text.format(U=unit if unit != "none" else "", S=side).strip(),
                })
            continue
        for form in forms:
            body = _UNIT_SPEC.sub("", form, count=1)
            body = body[len(base):].strip() if body.startswith(base) else body
            body = body.strip().lstrip(",").strip()
            if base == "NOP":
                body = body.replace("[count]", "1").replace("[", "").replace("]", "")
            for binding, overrides in _BINDINGS:
                # One pass, longest placeholder first, so "src2_h:src2_l" is not
                # rewritten by the "src2" rule and an override for "src" cannot
                # eat the "src" inside "src1".
                mapping = dict(_OPERANDS)
                mapping.update(dict(overrides))
                pattern = re.compile(
                    "|".join(re.escape(k) for k in
                             sorted(mapping, key=len, reverse=True)))
                text = pattern.sub(
                    lambda m: mapping[m.group(0)].format(S=side), body)
                text = f"{base} {unit} {text}".strip() if unit != "none" \
                    else f"{base} {text}".strip()
                out.append({"unit": unit, "syntax": form, "binding": binding,
                            "source": text})
    # The same (unit, source) can arise from two syntax forms; keep the first.
    seen, unique = set(), []
    for row in out:
        key = (row["unit"], row["source"])
        if key not in seen:
            seen.add(key)
            unique.append(row)
    return unique


CHUNK = 200


def _assemble_chunk(rows: list[dict], ti_bin: Path, workdir: Path) -> str | None:
    """Assemble one chunk; return the listing text, or None if asm6x died.

    asm6x keeps going after a bad line and lists the encoding of every good one,
    so one invocation classifies a whole chunk.  It can also segfault on a
    malformed line and then write nothing, which the caller handles by bisecting.
    """
    source = workdir / "probe.asm"
    listing = workdir / "probe.lst"
    if listing.exists():
        listing.unlink()
    lines = ["\t.text"]
    for index, row in enumerate(rows):
        row["asm_line"] = len(lines) + 1
        lines.append(f"\t{row['source']}\t; {index}")
    source.write_text("\n".join(lines) + "\n", encoding="utf-8")
    subprocess.run(
        [str(ti_bin / "asm6x"), "-mv6740", "-al", source.name],
        cwd=workdir, capture_output=True, text=True, check=False, timeout=600,
    )
    if not listing.exists() or listing.stat().st_size == 0:
        return None
    return listing.read_text(encoding="utf-8", errors="replace")


def _apply_listing(rows: list[dict], listing: str) -> None:
    encoded: dict[int, str] = {}
    errors: dict[int, str] = {}
    for line in listing.splitlines():
        hit = re.match(r"^\s*(\d+)\s+([0-9a-f]{8})\s+([0-9A-F]{8})\s", line)
        if hit:
            encoded[int(hit.group(1))] = hit.group(3)
            continue
        bad = re.search(r'ERROR!\s+at line (\d+):\s*(\[[^\]]+\].*)', line)
        if bad:
            errors[int(bad.group(1))] = bad.group(2).strip()
    for row in rows:
        row["word"] = encoded.get(row["asm_line"])
        row["assembler_error"] = errors.get(row["asm_line"])


def assemble(rows: list[dict], ti_bin: Path, workdir: Path) -> str:
    """Assemble every candidate, bisecting around assembler crashes.

    Returns the first chunk listing, whose banner carries the assembler version.
    """
    banner = None
    pending = [rows[i:i + CHUNK] for i in range(0, len(rows), CHUNK)]
    while pending:
        chunk = pending.pop(0)
        listing = _assemble_chunk(chunk, ti_bin, workdir)
        if listing is not None:
            banner = banner or listing
            _apply_listing(chunk, listing)
            continue
        if len(chunk) == 1:
            chunk[0]["word"] = None
            chunk[0]["assembler_error"] = "asm6x crashed on this candidate"
            continue
        middle = len(chunk) // 2
        pending[:0] = [chunk[:middle], chunk[middle:]]
    if banner is None:
        raise SystemExit("asm6x produced no listing at all; check --ti-bin")
    return banner


PROFILES = ("pointer", "small")


def run_probe(words: list[str], workdir: Path) -> dict[str, dict]:
    """Run every encoding under both register profiles and keep the best verdict.

    A rejection that only happens under one profile is a property of the register
    values, not of the decoder, so acceptance under either profile counts as
    acceptance and the report names the profile that accepted.
    """
    binary = workdir / "isa-probe"
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("no C compiler on PATH")
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator" / "qemu"), str(PROBE_SOURCE),
         *[str(p) for p in CORE_SOURCES], "-o", str(binary), "-lm"],
        check=True, capture_output=True, text=True,
    )
    merged: dict[str, dict] = {}
    for profile in PROFILES:
        result = subprocess.run(
            [str(binary), profile], input="".join(f"{w}\n" for w in words),
            capture_output=True, text=True, check=True, timeout=300,
        )
        for line in result.stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) < 2:
                continue
            word, verdict = parts[0].lower(), parts[1]
            detail = parts[2] if len(parts) > 2 else ""
            previous = merged.get(word)
            if previous is None or (previous["verdict"] != "accept" and verdict == "accept"):
                merged[word] = {"verdict": verdict, "detail": detail, "profile": profile}
            elif previous["verdict"] != "accept" and previous["detail"] != detail:
                previous["detail"] = f"{previous['detail']}; {profile}: {detail}"
    return merged


def build(ti_bin: Path) -> dict:
    inventory = json.loads(INVENTORY.read_text(encoding="utf-8"))
    page_text = pages(INDEX)

    rows: list[dict] = []
    per_instruction: dict[str, dict] = {}
    for name, entry in inventory["instructions"].items():
        block = syntax_block(page_text, entry["description_pages"])
        forms, manual_units = syntax_forms(name, block)
        units = manual_units or (entry["units"] and
                                 [u + s for u in entry["units"] for s in "12"]) or ["none"]
        excluded = _OUT_OF_PROBE_SCOPE.get(name.split()[0])
        generated = [] if excluded else candidates(name, forms, units)
        per_instruction[name] = {
            "out_of_probe_scope": excluded,
            "description_pages": entry["description_pages"],
            "manual_syntax_forms": forms,
            "manual_units": manual_units,
            "units_probed": units,
            "candidates": generated,
        }
        rows.extend(generated)

    with tempfile.TemporaryDirectory(prefix="cdj-isa-probe-") as temporary:
        workdir = Path(temporary)
        # asm6x has no -version flag; it banners its version in the listing.
        assembler_version = assemble(rows, ti_bin, workdir).splitlines()[0].strip()
        words = sorted({row["word"] for row in rows if row["word"]})
        verdicts = run_probe(words, workdir) if words else {}

    for row in rows:
        if not row["word"]:
            row["verdict"] = "no-encoding"
            row["detail"] = row["assembler_error"] or "assembler produced no word"
            continue
        found = verdicts.get(row["word"].lower(), {"verdict": "missing", "detail": "",
                                                   "profile": None})
        row["verdict"] = found["verdict"]
        row["detail"] = found["detail"]
        row["register_profile"] = found["profile"]

    summary = {}
    for name, entry in per_instruction.items():
        verdicts_here = {row["verdict"] for row in entry["candidates"]}
        if entry["out_of_probe_scope"]:
            status = "out-of-probe-scope"
        elif not verdicts_here or verdicts_here == {"no-encoding"}:
            status = "no-encoding"
        elif verdicts_here <= {"accept", "no-encoding"}:
            status = "all-probed-forms-accepted"
        elif "accept" in verdicts_here:
            status = "some-forms-rejected"
        else:
            status = "all-probed-forms-rejected"
        entry["status"] = status
        summary.setdefault(status, []).append(name)

    not_implemented = sorted({
        name for name, entry in per_instruction.items()
        for row in entry["candidates"]
        if "not implemented" in (row["detail"] or "")
    })

    return {
        "schema": 1,
        "what_this_measures": (
            "Decode acceptance of one representative 32-bit encoding per manual "
            "syntax form per functional unit.  Acceptance is not semantic "
            "correctness; rejection with 'instruction not implemented' is a real "
            "coverage gap; 'no-encoding' is unknown, not unsupported."
        ),
        "scope_limits": [
            "One representative encoding per syntax form per unit, not every opfield or operand type.",
            "32-bit encodings only; compact 16-bit forms are assessed against the Appendix C-H maps separately.",
            "Same-side operands only; cross-path variants are not probed.",
            "Single-instruction execute packets only; no predication, parallelism or packet interaction.",
            "Operand values are chosen to keep the probe's memory callbacks in range, not to exercise edge cases.",
            "Each encoding runs under two register profiles; acceptance under either counts, so a value-dependent rejection is not reported as a coverage gap.",
        ],
        "provenance": {
            "manual_inventory": str(INVENTORY.relative_to(ROOT)),
            "manual_inventory_sha256": hashlib.sha256(INVENTORY.read_bytes()).hexdigest(),
            "assembler": "TI asm6x -mv6740",
            "assembler_version": assembler_version,
            "probe_source": str(PROBE_SOURCE.relative_to(ROOT)),
            "probe_source_sha256": hashlib.sha256(PROBE_SOURCE.read_bytes()).hexdigest(),
            "core_sources": {
                str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in CORE_SOURCES
            },
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        },
        "counts": {
            "instruction_rows": len(per_instruction),
            "candidate_encodings": len(rows),
            "distinct_words": len({r["word"] for r in rows if r["word"]}),
            **{f"rows_{k}": len(v) for k, v in sorted(summary.items())},
            "rows_reporting_not_implemented": len(not_implemented),
        },
        "rows_not_implemented": not_implemented,
        "rows_by_status": {k: sorted(v) for k, v in sorted(summary.items())},
        "instructions": per_instruction,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    ap.add_argument("--ti-bin", type=Path, required=True,
                    help="TI C6000 codegen bin directory containing asm6x")
    args = ap.parse_args(argv)
    if not (args.ti_bin / "asm6x").exists():
        raise SystemExit(f"{args.ti_bin}/asm6x not found")
    report = build(args.ti_bin)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps(report["counts"], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
