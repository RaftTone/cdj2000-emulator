"""Validate analysis/dsp/coverage_inventory.json and generate its summary counts.

The inventory is the single source of every number in
DSP_ARCHITECTURE_COVERAGE.md.  This module exists so that those numbers are
generated rather than typed, and so that a row cannot quietly lose the
distinctions the audit depends on:

* implementation, validation and fidelity are separate dimensions and each must
  carry one of its documented values - "it works" is not a coverage claim;
* a row that no verifier challenged is reported as ``not-challenged`` and never
  folded in with the upheld rows;
* every row cites a manual document, section and printed page, and an
  implementation location;
* a row whose coverage is incomplete names its next acceptance test.

    python -m tools.cdj_dsp.coverage_matrix            # validate and print counts
    python -m tools.cdj_dsp.coverage_matrix --markdown # emit the matrix table
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "analysis" / "dsp" / "coverage_inventory.json"

IMPLEMENTATION = {"supported", "partial", "unsupported", "unknown"}
VALIDATION = {"reference-backed-tests", "firmware-observation-only", "untested",
              "not-assessed"}
FIDELITY = {"strict", "approximation", "unknown"}
VERDICT = {"upheld", "downgraded", "overturned", "unresolved", "not-challenged"}

REQUIRED = ("id", "track", "requirement", "manual_document", "manual_section",
            "manual_printed_page", "implementation", "implementation_location",
            "validation", "fidelity", "next_acceptance_test")


def validate(data: dict) -> list[str]:
    problems = []
    rows = data["rows"]
    seen = set()
    for row in rows:
        rid = row.get("id", "<missing id>")
        for field in REQUIRED:
            if not row.get(field):
                problems.append(f"{rid}: missing {field}")
        if rid in seen:
            problems.append(f"{rid}: duplicate row id")
        seen.add(rid)
        if row.get("implementation") not in IMPLEMENTATION:
            problems.append(f"{rid}: implementation={row.get('implementation')!r}")
        if row.get("validation") not in VALIDATION:
            problems.append(f"{rid}: validation={row.get('validation')!r}")
        if row.get("fidelity") not in FIDELITY:
            problems.append(f"{rid}: fidelity={row.get('fidelity')!r}")
        verdict = (row.get("verification") or {}).get("verdict")
        if verdict not in VERDICT:
            problems.append(f"{rid}: verification.verdict={verdict!r}")
        # A row claiming reference-backed tests must name at least one test and
        # say where its expected values came from.
        if row.get("validation") == "reference-backed-tests":
            tests = row.get("tests") or []
            if not tests:
                problems.append(f"{rid}: reference-backed-tests with no test named")
            for test in tests:
                if not test.get("expected_values"):
                    problems.append(
                        f"{rid}: test {test.get('name')!r} does not state expected-value provenance")
                # A cited test has to be something a later reader can actually run.
                # A scratchpad path dies with the session that made it, and an
                # emulator source location is the implementation, not a test of it.
                where = test.get("file") or ""
                if "scratchpad" in where or "/tmp/" in where:
                    problems.append(
                        f"{rid}: test {test.get('name')!r} cites {where!r}, which is not a "
                        "committed test and cannot be rerun")
                if where.startswith("emulator/"):
                    problems.append(
                        f"{rid}: test {test.get('name')!r} cites emulator source {where!r} "
                        "rather than a test")
    return problems


def counts(data: dict) -> dict:
    rows = data["rows"]
    tally = lambda f: dict(collections.Counter(r.get(f) for r in rows).most_common())
    verdicts = collections.Counter(
        (r.get("verification") or {}).get("verdict") for r in rows)
    # Rows that claim something positive AND were never challenged: the set a
    # reader must treat with least confidence.
    unchallenged_positive = sorted(
        r["id"] for r in rows
        if (r.get("verification") or {}).get("verdict") == "not-challenged"
        and (r.get("implementation") in ("supported", "partial")
             or r.get("validation") == "reference-backed-tests"
             or r.get("fidelity") == "strict"))
    circular = sorted(
        r["id"] for r in rows
        for t in (r.get("tests") or [])
        if t.get("expected_values") in ("derived-from-emulator", "mixed", "unclear"))
    return {
        "rows": len(rows),
        "by_track": tally("track"),
        "by_implementation": tally("implementation"),
        "by_validation": tally("validation"),
        "by_fidelity": tally("fidelity"),
        "by_verification_verdict": dict(verdicts.most_common()),
        "rows_corrected_by_verifier": sum(1 for r in rows if "verification_corrections" in r),
        "rows_adjusted_by_coordinator": sum(1 for r in rows if "coordinator_adjustment" in r),
        "positive_rows_never_challenged": unchallenged_positive,
        "rows_with_non_independent_expected_values": sorted(set(circular)),
    }


def markdown(data: dict) -> str:
    lines = ["| ID | Requirement | Manual | Impl | Validation | Fidelity | Verdict |",
             "|---|---|---|---|---|---|---|"]
    for row in sorted(data["rows"], key=lambda r: (r["track"], r["id"])):
        ref = f"{row['manual_document']} {row['manual_section']} p{row['manual_printed_page']}"
        lines.append(
            f"| `{row['id']}` | {row['requirement'][:110]} | {ref[:70]} | "
            f"{row['implementation']} | {row['validation']} | {row['fidelity']} | "
            f"{(row.get('verification') or {}).get('verdict')} |")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inventory", type=Path, default=INVENTORY)
    ap.add_argument("--markdown", action="store_true", help="emit the matrix table")
    args = ap.parse_args(argv)

    data = json.loads(args.inventory.read_text(encoding="utf-8"))
    problems = validate(data)
    if problems:
        for problem in problems:
            print(f"INVALID {problem}", file=sys.stderr)
        return 1
    if args.markdown:
        print(markdown(data))
    else:
        print(json.dumps(counts(data), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
