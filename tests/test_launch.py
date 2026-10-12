"""The unified launcher selects the requested hardware profile."""

from tools.cdj_main import launch, nxs_vm, view_vm


def test_profile_backends_are_explicit():
    assert launch.backend("2000") is view_vm
    assert launch.backend("nxs") is nxs_vm


def test_deck_defaults_yield_to_explicit_options():
    assert launch.deck_arguments([], environ={}) == [
        "--ui", "--debug", "--seconds", "3600", "--functional-dsp-audio",
        "--test-track", "--source-key-when-ready"]
    assert launch.deck_arguments([], environ={"CDJ_USB": "u.img"})[-3:] == [
        "--usb", "u.img", "--source-key-when-ready"]
    explicit = ["--seconds=60", "--sd", "c.img", "--source-key", "none", "--dsp-model"]
    assert launch.deck_arguments(explicit, environ={"CDJ_USB": "u.img"}) == [
        "--ui", "--debug", *explicit]
