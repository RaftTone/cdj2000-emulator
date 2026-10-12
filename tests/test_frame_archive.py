# SPDX-License-Identifier: GPL-2.0-or-later
"""The dense-frame thinning: which archived frames a GIF keeps, and that the rest are deleted."""
from pathlib import Path

import pytest

from tools.cdj_main import frame_archive as fa


def test_window_text():
    assert fa.parse_window("40:70") == (40.0, 70.0)
    assert fa.parse_window("40:") == (40.0, float("inf"))
    assert fa.parse_window(":5") == (0.0, 5.0)
    with pytest.raises(ValueError):
        fa.parse_window("70:40")


def test_pick_keeps_the_first_frame_of_each_step_inside_the_window():
    stamps = [round(40 + i / 60, 4) for i in range(0, 600)]          # a 60 a second GUI for ten seconds
    kept = fa.pick(stamps, 10, (42, 44))
    assert len(kept) == 20
    assert all(42 <= t < 44 for t in kept)
    assert all(0.1 - 1 / 60 < b - a < 0.1 + 1 / 60 + 1e-3 for a, b in zip(kept, kept[1:]))


def test_pick_follows_a_screen_that_changes_rarely():
    assert fa.pick([1.0, 1.02, 3.5, 3.52, 9.0], 10, (0, 10)) == [1.0, 3.5, 9.0]


def test_guest_clock_from_the_link_log(tmp_path):
    log = tmp_path / "vm-main.log"
    assert fa.guest_seconds(log) is None
    log.write_text("x queued 0 t=1.5\ny queued 0 t=27.2259\n")
    assert fa.guest_seconds(log) == 27.2259


def _ppm(path: Path, value: int):
    path.write_bytes(b"P6\n4 2\n255\n" + bytes([value]) * 24)


def test_sweep_keeps_thinned_pngs_deletes_every_ppm_and_writes_a_gif(tmp_path):
    pytest.importorskip("PIL")
    dense = fa.DenseFrames(tmp_path, (10, 11), 10, lambda: None, tmp_path / "out.gif")
    (tmp_path / "png").mkdir()
    for n in range(70):
        _ppm(tmp_path / f"f{n + 1:06d}-t{9.9 + n / 60:010.4f}.ppm", n * 3)
    dense.sweep()
    assert not list(tmp_path.glob("*.ppm"))
    assert len(dense.kept) == 10 and all(10 <= t < 11 for t, _ in dense.kept)
    assert len(list((tmp_path / "png").glob("*.png"))) == 10
    assert dense.stop() == 10
    assert (tmp_path / "out.gif").stat().st_size > 0


def test_sweep_survives_a_file_that_cannot_be_deleted(tmp_path, monkeypatch):
    """On Windows a PPM the simulator still has open raises PermissionError on delete; that must not end the thread."""
    dense = fa.DenseFrames(tmp_path, (0, 100), 10, lambda: None)
    (tmp_path / "png").mkdir()
    _ppm(tmp_path / "f000001-t00001.0000.ppm", 1)
    _ppm(tmp_path / "f000002-t00002.0000.ppm", 2)
    real_unlink = Path.unlink

    def unlink(self, missing_ok=False):
        if self.name.startswith("f000001"):
            raise PermissionError(13, "in use", str(self))
        return real_unlink(self, missing_ok=missing_ok)

    monkeypatch.setattr(Path, "unlink", unlink)
    monkeypatch.setattr(fa, "ppm_to_png", lambda src, dst: (_ for _ in ()).throw(ValueError("bad frame")))
    dense.sweep()                                   # neither the ValueError nor the PermissionError escapes
    assert [p.name for p in tmp_path.glob("*.ppm")] == ["f000001-t00001.0000.ppm"]
    monkeypatch.setattr(Path, "unlink", real_unlink)
    dense.sweep()                                   # the next sweep takes it
    assert not list(tmp_path.glob("*.ppm"))
