"""SH7764 TMU1 routing used by the connected AmbiX audio task."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "emulator/qemu/cdj2000_main.c"


def test_tmu1_has_sh7764_vector_priority_and_timer_route():
    source = SOURCE.read_text()
    assert "#define TMU1_IRQ        0x2d" in source
    assert "#define INTEVT_TMU1     (TMU1_IRQ * 0x20)   /* 0x5a0 */" in source
    assert "INTC_VECT(CDJ_INTC_TMU1, INTEVT_TMU1)" in source
    assert "{ CDJ_INTC_TMU0, CDJ_INTC_TMU1, 0, 0 }" in source
    assert ("freq, intc->irqs[CDJ_INTC_TMU0], intc->irqs[CDJ_INTC_TMU1],\n"
            "                NULL, NULL);" in source)


def test_tmu1_is_distinct_from_stock_tick_and_loader_timers():
    source = SOURCE.read_text()
    enum = source[source.index("enum {\n    CDJ_INTC_UNUSED"):]
    enum = enum[:enum.index("};")]
    for name in ("CDJ_INTC_TMU0", "CDJ_INTC_TMU1", "CDJ_INTC_TMU3",
                 "CDJ_INTC_TMU4", "CDJ_INTC_TMU5"):
        assert enum.count(name) == 1
