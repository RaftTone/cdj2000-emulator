"""The launchers' main() functions against their own modules; no firmware."""
import inspect
import symtable

import pytest

from tools.cdj_main import boot_vm, nxs_vm


@pytest.mark.parametrize("module", [boot_vm, nxs_vm], ids=lambda m: m.__name__)
def test_main_assigns_no_name_of_a_module_level_function(module):
    """A name main() assigns anywhere is local to all of it, so a function of
    that name at module level cannot be called from it at all: the parent
    watch of #37 was imported as ``watch``, boot_vm.main() has a list called
    ``watch``, and every run ended in UnboundLocalError before MAIN started."""
    table = symtable.symtable(inspect.getsource(module), module.__file__, "exec")
    main = next(child for child in table.get_children() if child.get_name() == "main")
    assigned = {symbol.get_name() for symbol in main.get_symbols() if symbol.is_local()}
    callables = {name for name, value in vars(module).items()
                 if callable(value) and not name.startswith("__")}
    assert not assigned & callables
