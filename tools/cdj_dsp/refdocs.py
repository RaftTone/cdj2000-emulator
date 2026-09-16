"""Fetch, hash and index the TI reference manuals used by the DSP audit.

The manuals are proprietary TI documents: they live under ``build/references``,
which is git-ignored, and only the provenance record
(``build/references/provenance.json``) and the citations in
``DSP_ARCHITECTURE_COVERAGE.md`` are ever committed.

Usage::

    python -m tools.cdj_dsp.refdocs              # fetch (if absent), hash, index
    python -m tools.cdj_dsp.refdocs --check      # verify hashes only

The index is one text file per document with ``\f``-separated PDF pages turned
into explicit ``@@ PDFPAGE n PRINTED p`` markers so that a plain ``grep -n``
gives both the PDF page index and the printed page number that TI shows in the
footer.  The two differ in front matter and sometimes in appendices, so every
citation in the audit records both.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

REFDIR = Path(__file__).resolve().parents[2] / "build" / "references"

DOCS = {
    "sprufe8b": {
        "url": "https://www.ti.com/lit/ug/sprufe8b/sprufe8b.pdf",
        "title": "TMS320C674x DSP CPU and Instruction Set Reference Guide",
        "revision": "SPRUFE8B, July 2010",
        "sha256": "34bc36312d37b092be9741986e70088f0fcefa00d8401a078f2b2981bb7b1d37",
    },
    "spruh91d": {
        "url": "https://www.ti.com/lit/ug/spruh91d/spruh91d.pdf",
        "title": "TMS320C6745/C6747 DSP Technical Reference Manual",
        "revision": "SPRUH91D, March 2013 - Revised September 2016",
        "sha256": "8f00cb85ee803c034794096b2e663293c677fdc09bd8843415520e229fd76e6c",
    },
    "sprs377f": {
        # TI serves the C6747 datasheet from the device symlink path; the
        # literature number SPRS377F appears on every page of the PDF itself.
        "url": "https://www.ti.com/lit/ds/symlink/tms320c6747.pdf",
        "title": "TMS320C6745, TMS320C6747 Fixed- and Floating-Point DSP",
        "revision": "SPRS377F, September 2008 - Revised June 2014",
        "sha256": "297a63b4c4dae68e98d361162b238bde992466991f25fa3ef0e4b82e8bb9a869",
    },
}

# TI alternates the footer layout: odd pages read
# "SPRUFE8B - July 2010   <chapter>   121" and even pages read
# "120   <chapter>   SPRUFE8B - July 2010".  Accept both orientations.
_FOOTER = re.compile(
    r"^(?:SPRU[A-Z0-9]+\b.*?(\d+)|\s*(\d+)\s{2,}.*?SPRU[A-Z0-9]+\b.*)\s*$",
    re.MULTILINE,
)

# Datasheets (SPRS*) carry no literature number in the running footer; they use
# "Copyright (c) <years>, Texas Instruments Incorporated" with the page number
# on the outer edge, again swapping sides between odd and even pages.
_FOOTER_DS = re.compile(
    r"^(?:Copyright\s*.{0,12}\s*Texas Instruments Incorporated\b.*?(\d+)"
    r"|\s*(\d+)\s{2,}.*?Copyright\s*.{0,12}\s*Texas Instruments Incorporated\b.*)\s*$",
    re.MULTILINE,
)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(name: str, spec: dict, *, check_only: bool) -> Path:
    pdf = REFDIR / f"{name}.pdf"
    if not pdf.exists():
        if check_only:
            raise SystemExit(f"missing {pdf}; run without --check to download")
        REFDIR.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(spec["url"]) as resp, pdf.open("wb") as out:
            out.write(resp.read())
    got = sha256(pdf)
    if got != spec["sha256"]:
        raise SystemExit(f"{pdf}: sha256 {got} != recorded {spec['sha256']}")
    return pdf


def index(pdf: Path) -> tuple[Path, list[int | None]]:
    raw = subprocess.run(
        ["pdftotext", "-layout", str(pdf), "-"],
        check=True, capture_output=True,
    ).stdout.decode("utf-8", "replace")
    printed: list[int | None] = []
    out = []
    for i, page in enumerate(raw.split("\f"), start=1):
        hits = [a or b for a, b in _FOOTER.findall(page)]
        if not hits:
            hits = [a or b for a, b in _FOOTER_DS.findall(page)]
        p = int(hits[-1]) if hits else None
        printed.append(p)
        out.append(f"@@ PDFPAGE {i} PRINTED {p if p is not None else '-'}\n{page}")
    txt = pdf.with_suffix(".txt")
    txt.write_text("".join(out), encoding="utf-8")
    return txt, printed


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="verify hashes, do not download")
    args = ap.parse_args(argv)

    record = {}
    for name, spec in DOCS.items():
        pdf = fetch(name, spec, check_only=args.check)
        txt, printed = index(pdf)
        known = [p for p in printed if p is not None]
        record[name] = {
            "url": spec["url"],
            "title": spec["title"],
            "revision": spec["revision"],
            "sha256": spec["sha256"],
            "bytes": pdf.stat().st_size,
            "pdf_pages": len(printed),
            "printed_pages_detected": len(known),
            "text_index": str(txt.relative_to(REFDIR.parents[1])),
        }
        print(f"{name}: {len(printed)} pdf pages, {len(known)} footers, index {txt}")

    prov = REFDIR / "provenance.json"
    prov.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"provenance -> {prov}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
