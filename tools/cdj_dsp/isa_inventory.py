"""Derive the C674x instruction inventory from SPRUFE8B, not from our decoder.

The denominator for any instruction-coverage claim has to come from the manual.
This reads the text index produced by :mod:`tools.cdj_dsp.refdocs` and extracts
three independent manual facts per instruction:

* Table A-1 (Appendix A, printed pages 709-714) - which DSP families implement
  it, so we keep only rows with a C674x check mark, plus the footnote that marks
  "also available in compact form".
* Table B-1 (Appendix B, printed pages 715-720) - which functional units it
  executes on.
* The running header ``NAME - Title`` on every page of section 3.12, which gives
  the printed page where each instruction description starts.

Rows that appear in one table and not another are reported rather than silently
merged: a name mismatch is a parsing bug or a genuine manual inconsistency and
either way the audit has to see it.

Usage::

    python -m tools.cdj_dsp.isa_inventory analysis/dsp/isa_manual_inventory.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INDEX = ROOT / "build" / "references" / "sprufe8b.txt"

# Printed page ranges of the three sources.  SPRUFE8B has a zero offset between
# PDF page index and printed page number throughout (refdocs verifies this), so
# one set of numbers serves both.
TABLE_A1 = (709, 714)
TABLE_B1 = (715, 720)
SECTION_312 = (98, 708)

CHECK = "✓"
# "ADD" / "B displacement" / "LDB (15-bit offset)" / "MPY32 (32-bit result)".
# Names are upper case with digits; TI disambiguates a handful of rows with a
# trailing qualifier.  The set is closed on purpose: a qualifier we do not know
# must surface as an unparsed row rather than vanish (see
# parse_diagnostics.unparsed_checkmark_lines), because a silently dropped row
# understates both the denominator and the numerator of every coverage count.
_QUALIFIED = (
    r"displacement|register|IRP|NRP"
    r"|\(15-bit offset\)|\(32-bit result\)|\(64-bit result\)"
)
_NAME = rf"[A-Z][A-Z0-9]*(?:\s+(?:{_QUALIFIED}))?"
_A1_ROW = re.compile(rf"^\s{{2,}}({_NAME})\s{{2,}}(.*?)\s*$")
_UNITS = (".L", ".M", ".S", ".D")
# Headers name one or several instructions: "ADD", "MVKH/MVKLH", "LDB(U)".
_HEADER = re.compile(
    r"^(?:www\.ti\.com\s+)?([A-Z][A-Z0-9()/]*(?:\s+(?:IRP|NRP))?)\s+—\s+(.+?)"
    r"(?:\s+www\.ti\.com)?\s*$")
# Table A-1 splits a few instructions into several rows; the manual distinguishes
# their descriptions by title.  The third element restricts a marker to the names
# it belongs to: "Into Signed 64-Bit Result" appears in the MPY32SU, MPY32US and
# MPYI titles too, and only MPY32 is split by result width in Table A-1.
_QUALIFIERS = (
    ("displacement", "Branch Using a Displacement", None),
    ("register", "Branch Using a Register", None),
    ("IRP", "Interrupt Return Pointer", None),
    ("NRP", "NMI Return Pointer", None),
    ("(64-bit result)", "Into Signed 64-Bit Result", {"MPY32"}),
    ("(32-bit result)", "Into 32-Bit Result", {"MPY32"}),
)


def pages(index: Path) -> dict[int, str]:
    text = index.read_text(encoding="utf-8")
    out = {}
    for block in text.split("@@ PDFPAGE ")[1:]:
        head, _, body = block.partition("\n")
        pdf_page, _, printed = head.split()[0], None, head.split()[-1]
        out[int(pdf_page)] = body
    return out


def unparsed_rows(page_text: dict[int, str], span: tuple[int, int]) -> list[str]:
    """Check-marked lines in a table span that ``_A1_ROW`` does not recognise.

    The previous cross-check compared Table A-1 against Table B-1, which is
    blind to any name shape the shared regex rejects in both: that is how the
    two MPY32 result-width rows went missing.  This looks for the check mark
    itself, so it cannot be fooled the same way.
    """
    out = []
    for page in range(span[0], span[1] + 1):
        for line in page_text.get(page, "").splitlines():
            if CHECK not in line or _A1_ROW.match(line):
                continue
            if re.match(r"^\s*(Instruction|Table|\(\d\))", line):
                continue
            out.append(f"printed page {page}: {line.strip()[:120]}")
    return out


def parse_table(page_text: dict[int, str], span: tuple[int, int],
                headers: list[str]) -> dict[str, list[bool]]:
    """Parse a check-mark table whose columns are named by ``headers``.

    The header line is located on each page so that column centres come from the
    manual's own layout rather than a hard-coded offset.
    """
    rows: dict[str, list[bool]] = {}
    for page in range(span[0], span[1] + 1):
        body = page_text.get(page, "")
        centres = None
        for line in body.splitlines():
            if centres is None:
                found = [line.find(h) for h in headers]
                if all(i >= 0 for i in found) and "Instruction" in line:
                    centres = [i + len(h) / 2 for i, h in zip(found, headers)]
                continue
            match = _A1_ROW.match(line)
            if not match:
                continue
            name, tail = match.group(1).strip(), match.group(2)
            start = len(line) - len(tail)
            marks = [start + i for i, ch in enumerate(tail) if ch == CHECK]
            if not marks:
                continue
            flags = [False] * len(headers)
            for m in marks:
                flags[min(range(len(centres)), key=lambda c: abs(centres[c] - m))] = True
            rows.setdefault(name, [False] * len(headers))
            rows[name] = [a or b for a, b in zip(rows[name], flags)]
    return rows


def compact_names(page_text: dict[int, str], span: tuple[int, int]) -> set[str]:
    """Names whose C674x cell carries the "also available in compact form" mark.

    TI attaches the footnote to the C64x+ column, and the footnote text says the
    instruction has a compact form; the C674x compact encodings live in the
    Appendix C-H 16-bit opcode maps.  We record the footnote as a manual hint and
    the audit confirms each compact encoding against those maps.
    """
    out = set()
    for page in range(span[0], span[1] + 1):
        for line in page_text.get(page, "").splitlines():
            match = _A1_ROW.match(line)
            if match and re.search(r"✓\s*\(\d\)", line):
                out.add(match.group(1).strip())
    return out


def header_names(label: str) -> list[str]:
    """Expand a running-header label into the instruction names it covers.

    ``MVKH/MVKLH`` documents two instructions on one page and ``LDB(U)`` is TI's
    shorthand for the signed and unsigned loads sharing a description.
    """
    names = []
    label = label.split()[0]  # "B IRP" is the B description; the title qualifies it
    for part in label.split("/"):
        match = re.fullmatch(r"([A-Z][A-Z0-9]*)\(([A-Z0-9]+)\)", part)
        if match:
            names += [match.group(1), match.group(1) + match.group(2)]
        elif re.fullmatch(r"[A-Z][A-Z0-9]*", part):
            names.append(part)
    return names


def description_pages(page_text: dict[int, str], span: tuple[int, int]) -> dict[str, dict]:
    """Map instruction name -> description page range from the running headers.

    TI splits the 5-bit-offset and 15-bit-offset load descriptions into separate
    pages with otherwise identical headers; Table A-1 names the second form
    "LDB (15-bit offset)", so the title decides which row a page belongs to.
    """
    out: dict[str, dict] = {}
    for page in range(span[0], span[1] + 1):
        for line in page_text.get(page, "").splitlines()[:2]:
            match = _HEADER.match(line.rstrip())
            if not match:
                continue
            label, title = match.group(1), match.group(2).strip()
            for name in header_names(label):
                key = name
                if "15-Bit" in title and name.startswith(("LD", "ST")):
                    key = f"{name} (15-bit offset)"
                for suffix, marker, only in _QUALIFIERS:
                    if marker in title and (only is None or name in only):
                        key = f"{name} {suffix}"
                entry = out.setdefault(
                    key, {"title": title, "first_page": page, "last_page": page})
                if entry["title"] != title:
                    continue  # a later, differently titled description of the same name
                entry["last_page"] = max(entry["last_page"], page)
                entry["first_page"] = min(entry["first_page"], page)
    return out


def build(index: Path) -> dict:
    page_text = pages(index)
    a1 = parse_table(page_text, TABLE_A1,
                     ["C62x DSP", "C64x DSP", "C64x+ DSP", "C67x DSP", "C67x+ DSP", "C674x DSP"])
    b1 = parse_table(page_text, TABLE_B1, [".L Unit", ".M Unit", ".S Unit", ".D Unit"])
    compact = compact_names(page_text, TABLE_A1)
    descriptions = description_pages(page_text, SECTION_312)

    instructions = {}
    for name, families in sorted(a1.items()):
        if not families[-1]:
            continue  # not a C674x instruction
        base = name.split()[0]
        desc = descriptions.get(name) or descriptions.get(base) or {}
        instructions[name] = {
            "base_mnemonic": base,
            "families": {
                k: v for k, v in zip(
                    ["c62x", "c64x", "c64x_plus", "c67x", "c67x_plus", "c674x"], families)
            },
            "units": [u for u, v in zip(_UNITS, b1.get(name, [False] * 4)) if v],
            "compact_form_footnote": name in compact,
            "description_title": desc.get("title"),
            "description_pages": (
                [desc["first_page"], desc["last_page"]] if desc else None),
        }

    unparsed = (unparsed_rows(page_text, TABLE_A1) +
                unparsed_rows(page_text, TABLE_B1))
    missing_units = sorted(n for n, r in instructions.items() if not r["units"])
    only_b1 = sorted(set(b1) - set(a1))
    no_description = sorted(n for n, r in instructions.items() if r["description_pages"] is None)

    return {
        "schema": 1,
        "source": {
            "document": "SPRUFE8B",
            "revision": "July 2010",
            "url": "https://www.ti.com/lit/ug/sprufe8b/sprufe8b.pdf",
            "sha256": "34bc36312d37b092be9741986e70088f0fcefa00d8401a078f2b2981bb7b1d37",
            "printed_page_equals_pdf_page": True,
            "table_a1_pages": list(TABLE_A1),
            "table_b1_pages": list(TABLE_B1),
            "section_3_12_pages": list(SECTION_312),
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        },
        "scope": (
            "Manual-derived denominator: every Table A-1 row with a C674x check "
            "mark.  Rows are instruction names as TI writes them, including the "
            "separate B displacement/register/IRP/NRP and 15-bit-offset load "
            "rows.  This is not a count of distinct encodings: one row can cover "
            "many opfields, operand types and unit variants."
        ),
        "caveats": [
            "A row is an instruction name, not an encoding or a semantic variant.",
            "Table A-1's compact-form footnote sits in the C64x+ column; compact "
            "C674x encodings must be confirmed against the Appendix C-H 16-bit maps.",
            "Instructions with no Table B-1 unit execute on no .L/.M/.S/.D unit "
            "(for example NOP and IDLE); see Appendix H.",
            "Pseudo-operations and assembler aliases documented outside Table A-1 "
            "are not included.",
        ],
        "counts": {
            "c674x_instruction_rows": len(instructions),
            "table_a1_rows": len(a1),
            "rows_with_compact_footnote": sum(
                1 for r in instructions.values() if r["compact_form_footnote"]),
            "rows_without_unit": len(missing_units),
            "rows_without_description_page": len(no_description),
            "unparsed_checkmark_lines": len(unparsed),
            "distinct_base_mnemonics": len({r["base_mnemonic"] for r in instructions.values()}),
        },
        "parse_diagnostics": {
            "rows_without_unit": missing_units,
            "in_table_b1_only": only_b1,
            "rows_without_description_page": no_description,
            "unparsed_checkmark_lines": unparsed,
        },
        "instructions": instructions,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    ap.add_argument("--index", type=Path, default=INDEX)
    args = ap.parse_args(argv)
    if not args.index.exists():
        raise SystemExit(f"{args.index} missing; run python -m tools.cdj_dsp.refdocs first")
    report = build(args.index)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report["counts"], indent=2))
    print(json.dumps(report["parse_diagnostics"], indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
