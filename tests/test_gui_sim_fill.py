"""CDJ_GUI_SDRAM_FILL: the simulator argument list the launchers build from it."""

import pytest

from tools.cdj_gui.sim_fill import memory_fill_args


@pytest.mark.parametrize("env", [{}, {"CDJ_GUI_SDRAM_FILL": ""}, {"CDJ_GUI_SDRAM_FILL": "  "}])
def test_unset_changes_nothing(env):
    assert memory_fill_args(env) == []


@pytest.mark.parametrize("spec,value", [("00", 0), ("ff", 255), ("A5", 165), ("0xa5", 165), (" 7 ", 7)])
def test_a_hex_byte_becomes_the_simulator_option(spec, value):
    assert memory_fill_args({"CDJ_GUI_SDRAM_FILL": spec}) == ["--memory-fill", str(value)]


@pytest.mark.parametrize("spec", ["zz", "100", "-1", "random"])
def test_anything_else_is_refused(spec):
    with pytest.raises(SystemExit):
        memory_fill_args({"CDJ_GUI_SDRAM_FILL": spec})
