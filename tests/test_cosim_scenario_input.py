# SPDX-License-Identifier: GPL-2.0-or-later
"""How cosim_scenario presses keys and turns the encoder when the GUI drops some of them.

The GUI drops a press or a click that lands while it is refreshing its own list (runs/emu-b3: ENTER down at guest
16.110 s, no request, four seconds of silence; runs/emu-b1: four clicks, the cursor stayed on row 0).  What is checked
here is the harness's answer to that, on a fake machine: a repeat after a few guest seconds instead of half the timeout,
at most three presses, clicks that wait for the row they should have reached.
"""
from types import SimpleNamespace

from tools.cdj_main import cosim_scenario as cs


def fake(**kw):
    run = SimpleNamespace(RETRY_AFTER=cs.Run.RETRY_AFTER, PRESSES=cs.Run.PRESSES, repeats=[], proc=None,
                          presses=[], waits=[])
    run.press = lambda key, hold_ms=100: run.presses.append(key)
    run.__dict__.update(kw)
    return run


def test_a_dropped_first_press_is_repeated_after_a_few_seconds_not_half_the_timeout():
    answers = iter([None, "ENTER seen"])
    run = fake()

    def wait(what, check, guest_timeout):
        run.waits.append(guest_timeout)
        return next(answers)

    run.wait = wait
    assert cs.Run.press_for(run, "playlist", "17.0", lambda: None, 20) == "ENTER seen"
    assert run.presses == ["17.0", "17.0"]
    assert run.waits == [cs.Run.RETRY_AFTER, cs.Run.RETRY_AFTER]
    assert run.repeats == ["playlist"]


def test_a_press_that_works_is_pressed_once():
    run = fake(wait=lambda what, check, t: "ok")
    assert cs.Run.press_for(run, "tracks", "17.0", lambda: None, 20) == "ok"
    assert run.presses == ["17.0"] and run.repeats == []


def test_three_presses_at_most_and_the_whole_timeout_is_kept():
    run = fake()
    run.wait = lambda what, check, t: run.waits.append(t)
    assert cs.Run.press_for(run, "tracks", "17.0", lambda: None, 20) is None
    assert len(run.presses) == cs.Run.PRESSES == 3
    assert sum(run.waits) == 20


def test_a_machine_that_died_is_not_pressed_again():
    run = fake(proc=SimpleNamespace(poll=lambda: 1), wait=lambda what, check, t: None)
    assert cs.Run.press_for(run, "tracks", "17.0", lambda: None, 20) is None
    assert run.presses == ["17.0"]


class FakeGui:
    """A GUI whose cursor moves on the clicks that are not dropped and which answers each move with a preview request."""

    def __init__(self, drop):
        self.drop = list(drop)              # True = this click is lost
        self.row = 0
        self.lines = []
        self.clicks = 0
        self.repeats = []

    def panel(self, *words, timeout=30):
        self.clicks += 1
        if not (self.drop.pop(0) if self.drop else False):
            self.row += 1
            self.lines.append((float(self.clicks), "0000 0001 000b 0007 0002 %04x" % self.row))

    def requests(self, since=0):
        return self.lines[since:]

    def wait(self, what, check, guest_timeout, wall_timeout=900):
        return check()


def test_turning_waits_for_each_row_and_clicks_again_when_a_click_is_dropped(monkeypatch):
    monkeypatch.setattr(cs.time, "sleep", lambda s: None)
    gui = FakeGui(drop=[False, True, False, True, True, False, False])
    cs.turn_down(gui, 4, 0)
    assert gui.row == 4                      # four rows down although three clicks were lost
    assert gui.clicks == 7
    assert gui.repeats == ["turn"] * 3


def test_a_click_that_arrived_late_is_not_repeated(monkeypatch):
    monkeypatch.setattr(cs.time, "sleep", lambda s: None)
    gui = FakeGui(drop=[])
    gui.lines = [(0.0, "0000 0001 000b 0007 0002 0002")]    # the preview of a row at or past the wanted one is there
    cs.turn_down(gui, 1, 0)
    assert gui.clicks == 1 and gui.repeats == []


def test_after_three_lost_clicks_it_gives_up_on_that_row_and_goes_on(monkeypatch):
    monkeypatch.setattr(cs.time, "sleep", lambda s: None)
    gui = FakeGui(drop=[True, True, True])
    cs.turn_down(gui, 1, 0)
    assert gui.clicks == 3 and gui.row == 0


class FakeTrackList(FakeGui):
    """A track list: its previews carry 000a, those of the playlist list before it carry 0007 (runs/tmp-6380)."""

    def __init__(self, drop=()):
        super().__init__(drop)
        self.lines = [(0.0, "0000 0001 000b 0007 0002 0004")]      # the playlist list, row 4, from before the ENTER

    def panel(self, *words, timeout=30):
        self.clicks += 1
        if not (self.drop.pop(0) if self.drop else False):
            self.row += 1
            self.lines.append((float(self.clicks), "0000 0001 000b 000a 0002 %04x" % self.row))


def test_a_track_list_click_is_seen_in_the_000a_previews_and_not_by_a_playlist_preview(monkeypatch):
    monkeypatch.setattr(cs.time, "sleep", lambda s: None)
    gui = FakeTrackList()
    cs.turn_down(gui, 1, 0, cs.PREVIEW_TRACKS)
    assert gui.row == 1 and gui.clicks == 1 and gui.repeats == []     # one click, not three


def test_a_dropped_click_in_a_track_list_is_repeated(monkeypatch):
    monkeypatch.setattr(cs.time, "sleep", lambda s: None)
    gui = FakeTrackList(drop=[True, False])
    cs.turn_down(gui, 1, 0, cs.PREVIEW_TRACKS)
    assert gui.row == 1 and gui.clicks == 2 and gui.repeats == ["turn"]


def test_a_playlist_preview_from_before_the_enter_does_not_satisfy_a_track_list_click(monkeypatch):
    monkeypatch.setattr(cs.time, "sleep", lambda s: None)
    gui = FakeTrackList(drop=[True, True, True])
    cs.turn_down(gui, 1, 0, cs.PREVIEW_TRACKS)
    assert gui.clicks == 3 and gui.row == 0                           # the 0007 row 4 in the list did not count


def run_then_keys(monkeypatch, spec):
    sent = []
    monkeypatch.setattr(cs.subprocess, "run", lambda cmd, **kw: (sent.append(cmd), SimpleNamespace(
        stdout="1 2 3", stderr=""))[1])
    run = SimpleNamespace(port=6480, err=cs.ROOT / "no-such-qemu.err", out=cs.ROOT / "no-such-dir",
                          frame=cs.ROOT / "no-such-frame", guest=lambda: 0.0, wait=lambda *a, **k: True)
    cs.then_keys(run, spec)
    return sent[0][sent[0].index("sequence") + 1:]


def test_the_last_press_of_a_fast_group_with_a_normal_pause_holds_like_every_other_normal_press(monkeypatch):
    batch = run_then_keys(monkeypatch, "17.0:0.07,17.1:0.07,17.0:3")
    assert batch == ["17.0:35:35", "17.1:35:35", "17.0:100:300"]


def test_a_hold_given_with_at_wins(monkeypatch):
    assert run_then_keys(monkeypatch, "17.0@30:0.07,17.1@20:3") == ["17.0:30:40", "17.1:20:300"]
