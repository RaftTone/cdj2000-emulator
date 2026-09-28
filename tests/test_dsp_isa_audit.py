"""Keep the manual-derived ISA audit tooling honest.

Neither tool is part of the emulator; both exist so that an instruction-coverage
claim has a denominator that comes from SPRUFE8B rather than from our own
decoder.  The parsing in them is real enough to break silently, so:

* the committed inventory must still match what the parser produces from the
  manual text index, and must still resolve every row (no row without a
  functional unit that TI gives one, no row without a description page);
* the probe's assembler-listing parser and the probe binary must still agree on
  encodings whose verdict we can state independently.

Everything here skips when its prerequisite is absent - the manuals live in
git-ignored build/references and the TI assembler is not part of the repository.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
INDEX = ROOT / "build" / "references" / "sprufe8b.txt"
INVENTORY = ROOT / "analysis" / "dsp" / "isa_manual_inventory.json"
PROBE = ROOT / "tests" / "cstub" / "c674x-isa-probe.c"


def test_committed_inventory_is_internally_consistent():
    data = json.loads(INVENTORY.read_text())
    counts, rows = data["counts"], data["instructions"]
    assert counts["c674x_instruction_rows"] == len(rows)
    assert counts["rows_without_description_page"] == 0
    # A check-marked line the row regex cannot parse is how the two MPY32
    # result-width rows were silently dropped once; it must never be nonzero.
    assert counts["unparsed_checkmark_lines"] == 0
    assert data["parse_diagnostics"]["unparsed_checkmark_lines"] == []
    # Only the Appendix H no-unit instructions may lack a functional unit.
    unitless = {name for name, row in rows.items() if not row["units"]}
    assert unitless == set(data["parse_diagnostics"]["rows_without_unit"])
    assert unitless == {
        "DINT", "IDLE", "NOP", "RINT", "SPKERNEL", "SPKERNELR", "SPLOOP",
        "SPLOOPD", "SPLOOPW", "SPMASK", "SPMASKR", "SWE", "SWENR",
    }
    # Every retained row is a C674x instruction with a real manual citation.
    for name, row in rows.items():
        assert row["families"]["c674x"], name
        first, last = row["description_pages"]
        assert 98 <= first <= last <= 708, (name, row["description_pages"])


def test_inventory_matches_the_manual_text_index():
    if not INDEX.exists():
        pytest.skip("run python -m tools.cdj_dsp.refdocs to fetch the manuals")
    from tools.cdj_dsp.isa_inventory import build

    fresh = build(INDEX)
    committed = json.loads(INVENTORY.read_text())
    assert fresh["counts"] == committed["counts"]
    assert fresh["instructions"] == committed["instructions"]


def test_probe_classifies_encodings_we_can_reason_about_independently(tmp_path):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("requires C compiler")
    binary = tmp_path / "isa-probe"
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator/qemu"), str(PROBE),
         str(ROOT / "emulator/qemu/cdj_c674x.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_uncond.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_mpy.c"), str(ROOT / "emulator/qemu/cdj_c674x_dotp.c"), str(ROOT / "emulator/qemu/cdj_c674x_packed8.c"), str(ROOT / "emulator/qemu/cdj_c674x_packed16.c"), str(ROOT / "emulator/qemu/cdj_c674x_packbits.c"), str(ROOT / "emulator/qemu/cdj_c674x_mpy32.c"), str(ROOT / "emulator/qemu/cdj_c674x_dp.c"), str(ROOT / "emulator/qemu/cdj_c674x_approx.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_sp.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_control.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_loop.c"), "-o", str(binary), '-lm'],
        check=True)
    words = ["02988078", "0280022a", "ffffffff"]
    result = subprocess.run([str(binary), "pointer"], input="\n".join(words) + "\n",
                            capture_output=True, text=True, check=True, timeout=30)
    verdicts = dict(line.split(None, 1) for line in result.stdout.splitlines())
    # ADD .L1 A4,A6,A5 and MVKL .S2 4,B5, both straight from TI's assembler.
    assert verdicts["02988078"] == "accept"
    assert verdicts["0280022a"] == "accept"
    # creg=11111 is a reserved predicate in SPRUFE8B Table 3-1, so a core that
    # accepted it would be wrong; the probe must report the rejection, not hide it.
    assert verdicts["ffffffff"].startswith("execute-reject")


def test_probe_driver_reads_the_assembler_listing(tmp_path):
    ti_bin = Path("/Applications/ti/ti-cgt-c6000_8.5.0.LTS/bin")
    if not (ti_bin / "asm6x").exists():
        pytest.skip("requires the TI C6000 assembler")
    from tools.cdj_dsp.isa_probe import assemble

    rows = [
        {"source": "ADD .L1 A4, A6, A5"},
        {"source": "MVKL .S2 4, B5"},
        {"source": "NOTANINSTRUCTION .L1 A4, A5"},
    ]
    assemble(rows, ti_bin, tmp_path)
    assert rows[0]["word"] == "02988078"
    assert rows[1]["word"] == "0280022A"
    # A rejected line must surface the assembler's reason, never a silent None
    # that the report would then read as an unsupported instruction.
    assert rows[2]["word"] is None
    assert "E0002" in rows[2]["assembler_error"]


# --------------------------------------------------------------------------
# The three audit sweeps.  These assertions exist so that the numbers quoted in
# DSP_ARCHITECTURE_COVERAGE.md stay reproducible: if the core changes, a sweep
# count changes and one of these fails, which is the point.
# --------------------------------------------------------------------------

def _sweep(mode, tmp_path):
    from tools.cdj_dsp.audit_sweeps import build, run
    return run(build(tmp_path), mode)


def test_packet_rejection_rolls_back_every_byte_of_state(tmp_path):
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    from tools.cdj_dsp.audit_sweeps import atomicity

    result = atomicity(_sweep("atomicity", tmp_path))
    assert result["failures"] == 0, result
    # Each case must actually have been rejected, or it proves nothing.
    assert result["cases"] and not any(c["executed"] for c in result["cases"])
    for case in result["cases"]:
        assert case["state_unchanged"] and case["memory_unchanged"], case


def test_compact_decode_gap_is_exactly_the_measured_set(tmp_path):
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    from tools.cdj_dsp.audit_sweeps import compact

    text = _sweep("compact", tmp_path)
    result = compact(text)
    assert result["words_swept"] == 0x10000
    # Decoder-only figure: independent of the probe's registers and memory window.
    # Without a disassembler the sweep can only report an UPPER BOUND, because
    # "compact instruction not implemented" lumps together four different things:
    # encodings the architecture never defines (which the core is right to refuse),
    # s = 0 twins of forms whose figure hardwires s = 1 (ditto), the software-loop
    # family (implemented, but needing a loop this one-instruction probe cannot
    # provide), and real gaps.  6,880 is that upper bound after the 64 compact
    # SPMASKR forms gained their documented outside-loop NOP behavior; the
    # genuine gap is 32 and is asserted separately below.
    assert result["not_implemented_raw"] == 6880
    assert result["defensible_coverage_figure"]["value"] == 6880
    # Nothing may be rejected for reasons that are properties of the probe.
    assert set(result["rejection_reasons"]) == {
        "compact instruction not implemented",
        "reserved compact LSDx1 instruction",
        "unaligned stack access or invalid register pair",
    }, result["rejection_reasons"]
    # Pin the 512-word delta to the format it is supposed to be, rather than to
    # a total that happens to agree, and assert that the Sx1b s = 0 words are
    # still refused.  The masks are read straight off the figures.
    verdicts = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            verdicts[int(parts[0], 16)] = parts[1]
    assert len(verdicts) == 0x10000
    spmaskr = {w for w in range(0x10000) if w & 0x3c7e == 0x3c66}
    assert len(spmaskr) == 64
    assert all(verdicts[w] == "accept" for w in spmaskr)
    sx2op = {w for w in range(0x10000) if w & 0x047e == 0x002e}
    sx1b_s0 = {w for w in range(0x10000) if w & 0x187f == 0x006e}
    assert len(sx2op) == 512 and len(sx1b_s0) == 128
    assert not sx2op & sx1b_s0
    unaccepted = sorted(w for w in sx2op if verdicts[w] != "accept")
    assert not unaccepted, [f"{w:04x}" for w in unaccepted]
    still_closed = sorted(w for w in sx1b_s0 if verdicts[w] == "accept")
    assert not still_closed, [f"{w:04x}" for w in still_closed]
    # The three families that made up the old 104-word "genuine gap", pinned by
    # mask so the claim does not depend on having GNU's disassembler to hand.
    # Figure C-19 Dx5p: bit 12 = 0, bits 11-10 = 11, bits 6-0 = 1110110, with
    # ucst2-0, ucst4-3 and op free - 64 words per value of bit 0.  Figure F-31
    # op 110: bits 15-10 = 110110, bits 6-0 = 1101110, src free - 8 words per
    # value of bit 0.  Both figures hardwire s = 1, so the s = 0 half must stay
    # refused and the s = 1 half must execute.  Figure H-6 Uspldr: bit 15 = 1,
    # bits 13-12 = 00, bits 11-10 = 11, bits 6-1 = 110011, ii and op free - 32
    # words, all refused while SPLOOP reload is unimplemented.
    families = {
        "dx5p s=0": ({w for w in range(0x10000) if w & 0x1c7f == 0x0c76}, 64, False),
        "dx5p s=1": ({w for w in range(0x10000) if w & 0x1c7f == 0x0c77}, 64, True),
        "sx1 mvc ilc s=0": ({w for w in range(0x10000) if w & 0xfc7f == 0xd86e}, 8, False),
        "sx1 mvc ilc s=1": ({w for w in range(0x10000) if w & 0xfc7f == 0xd86f}, 8, True),
        "uspldr": ({w for w in range(0x10000) if w & 0xbc7e == 0x8c66}, 32, False),
    }
    for label, (words, count, accepted) in families.items():
        assert len(words) == count, (label, len(words))
        wrong = sorted(w for w in words
                       if (verdicts[w] == "accept") != accepted)
        assert not wrong, (label, [f"{w:04x}" for w in wrong])
    assert not families["uspldr"][0] & sx1b_s0


def test_control_register_reachability_and_reserved_bits(tmp_path):
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    from tools.cdj_dsp.audit_sweeps import control

    result = control(_sweep("control", tmp_path))
    assert result["ids_swept"] == 32
    # AMR CSR IFR IER ISTP IRP NRP TSCL TSCH ILC RILC FADCR FAUCR FMCR SSR
    # GPLYA GPLYB GFPGFR TSR ITSR. TI dis6x independently encodes TSCL as id 10 (0x022803e2)
    # and TSCH as id 11 (0x002c03e2).
    assert result["readable_ids"] == [0, 1, 2, 4, 5, 6, 7, 10, 11, 13, 14,
                                      18, 19, 20, 21, 22, 23, 24, 26, 27]
    # ISR (3) is write-only; TSCL (10) is the exceptional nominally read-only
    # register whose ignored-value write enables the timestamp counter.
    assert result["writable_ids"] == [0, 1, 2, 3, 4, 5, 6, 7, 10, 13, 14,
                                      18, 19, 20, 21, 22, 23, 24, 26, 27]

    masks = result["rows"]
    # IRP, NRP, ILC and RILC are full 32-bit registers with no reserved field
    # (SPRUFE8B Figures 2-21, 2-24 and the IRP/NRP figures), so storing every bit
    # is correct for these four.
    for ident in (6, 7, 13, 14):
        assert masks[str(ident)]["read_mask"] == "ffffffff"
    # FADCR, FAUCR and FMCR each reserve bits 31-27 and 15-11, which SPRUFE8B
    # Tables 2-25 (printed page 59), 2-26 (printed page 61) and 2-27 (printed
    # page 63) say are "always read as 0" and unaffected by writes; every other
    # bit is R/W by MVC in Figures 2-29/2-30/2-31.  ~0x07ff07ff is exactly the
    # two reserved ranges, so this is the manual's mask, not a measurement.
    for ident in (18, 19, 20):
        assert masks[str(ident)]["read_mask"] == "07ff07ff"
    assert masks["24"]["read_mask"] == "070000ff"


def test_single_precision_rounding_against_an_independent_oracle(tmp_path):
    """Put the audit's SP-rounding characterization test into the suite.

    The oracle is the host FPU under ``fesetround``, not the emulator: for SP
    normal operands the exact product and (for nearby exponents) the exact sum fit
    in a double, so one ``double -> float`` conversion is a single correctly
    rounded SP rounding.  Without this wrapper the measurement was an audit
    artefact that no regression run would ever repeat.
    """
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("requires C compiler")
    source = ROOT / "tests" / "cstub" / "c674x-audit-sp-rounding.c"
    binary = tmp_path / "sp-rounding"
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "emulator/qemu"), str(source),
         str(ROOT / "emulator/qemu/cdj_c674x.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_uncond.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_mpy.c"), str(ROOT / "emulator/qemu/cdj_c674x_dotp.c"), str(ROOT / "emulator/qemu/cdj_c674x_packed8.c"), str(ROOT / "emulator/qemu/cdj_c674x_packed16.c"), str(ROOT / "emulator/qemu/cdj_c674x_packbits.c"), str(ROOT / "emulator/qemu/cdj_c674x_mpy32.c"), str(ROOT / "emulator/qemu/cdj_c674x_dp.c"), str(ROOT / "emulator/qemu/cdj_c674x_approx.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_sp.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_control.c"),
         str(ROOT / "emulator/qemu/cdj_c674x_loop.c"), "-o", str(binary), '-lm'],
        check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            timeout=120, check=True)
    assert "failures=0" in result.stdout, result.stdout


def test_compact_not_implemented_separates_undefined_from_real_gaps(tmp_path):
    """The compact gap is 32 words, not 104 and not 6,944.

    The raw count was quoted as a coverage figure through three commits before
    anyone disassembled what it contained.  6,616 of those words are encodings GNU
    libopcodes does not recognise at all, so refusing them is correct behaviour and
    not a gap; 96 are the compact software-loop family, which is implemented and
    validated by tests/cstub/c674x-spkernel-fields.c but cannot decode in a
    one-instruction packet; 128 are the Figure F-32 Sx1b s = 0 words this core
    refuses on purpose.

    The 104 that remained then made the same mistake one level down, by reading a
    GNU NAME as an architectural definition.  72 of them are the s = 0 twins of
    two forms whose figure hardwires s = 1 - Figure C-19 Dx5p (printed page 730)
    draws bit 0 as a literal "1", and Figure F-31 op 110 (printed page 756)
    parenthesises "(s = 1)" in its mnemonic table - so the architecture no more
    defines them than it defines the 6,616, and cl6x -mv6740 refuses all three
    assemblies (E0800 for ADDAW/SUBAW .D1 B15, W0005 for MVC .S1 B0,ILC).  GNU
    names them anyway, printing cross-path writes no C674x unit can perform.
    tests/cstub/c674x.c already pinned two of those refusals before this bucket
    existed; nothing here relaxes a guard, and the core source hashes recorded in
    analysis/dsp/audit_sweeps.json are unchanged across the reclassification.

    The last 32 are Figure H-6 Uspldr, "[A0]/[B0] SPLOOPD ii", which the SPLOOPD
    description (printed page 485) defines as selecting the SPLOOP reload
    capability for a nested loop.  They stay a genuine gap: cdj_c674x_step names
    them and refuses with "SPLOOPD reload not implemented", and retained-buffer
    reload is deliberately not claimed.  They are counted here, not in the
    software-loop-family bucket, because that bucket means "implemented".
    """
    if not shutil.which("cc"):
        pytest.skip("requires C compiler")
    disassembler = os.environ.get("C6X_DISASSEMBLER")
    if not disassembler or not Path(disassembler).exists():
        pytest.skip("set C6X_DISASSEMBLER to the built tools/cdj_dsp/tic6x_disasm.c frontend")
    from tools.cdj_dsp.audit_sweeps import build, compact, run

    result = compact(run(build(tmp_path), "compact"), Path(disassembler), tmp_path)
    buckets = result["not_implemented_breakdown"]["buckets"]
    assert buckets == {
        "undefined-encoding": 6616,
        "deliberately-fail-closed": 128,
        "software-loop-family": 32,
        "unit-restricted-encoding": 72,
        "genuine-gap": 32,
    }, buckets
    assert sum(buckets.values()) == result["not_implemented_raw"] == 6880
    assert result["defensible_coverage_figure"]["value"] == 32
    # Each reclassified word is accounted for by an encoding mask, not by a name:
    # Figure C-19 Dx5p s = 0 has bits 12-10 = 011 and bits 6-0 = 1110110 with
    # bits 15-13 (ucst2-0), 9-8 (ucst4-3) and 7 (op: ADDAW/SUBAW) free, so
    # 2**6 = 64 words; Figure F-31 op 110 s = 0 has bits 15-10 = 110110 and bits
    # 6-0 = 1101110 with bits 9-7 (src) free, so 2**3 = 8 words.  64 + 8 = 72.
    names = result["not_implemented_breakdown"]["by_mnemonic"]
    assert {k: v for k, v in names.items()
            if k not in ("<undefined>", "bnop", "sploop", "sploopd", "spmaskr")} == {
        "addaw": 32, "subaw": 32, "mvc": 8,
        "[a0] sploopd": 16, "[b0] sploopd": 16}
