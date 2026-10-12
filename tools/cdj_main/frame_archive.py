# SPDX-License-Identifier: GPL-2.0-or-later
"""Dense frames of a chosen stretch of a run, thinned as they arrive.

boot_vm --frames samples the live frame on a wall-clock tick (2 s by default), which is far too coarse
for an animation, and the simulator can only publish every frame it draws (patch 15,
BFIN_GUI_ARCHIVE_DIR): a few hundred megabytes of PPM for a minute of a running GUI.  This keeps what a
GIF needs and nothing else:

    boot_vm ... --dense-frames DIR --dense-window 40:70 --dense-fps 10 --dense-gif run.gif

While the guest clock is inside the window (less a second of margin) the simulator is armed.  Every frame
it archives is stamped with guest seconds; the first one at or after each 1/FPS step is kept as a PNG named
by that stamp, the rest are deleted the moment they appear.  At the end the kept frames are written as a GIF
whose delays follow the stamps (a frame stays until the next one is due), so what the GIF shows runs at
guest speed however long the emulator took.
"""
from __future__ import annotations

import re
import threading
import time
from pathlib import Path
from typing import Callable, Optional

FRAME_NAME = re.compile(r"^f(\d+)-t(\d+(?:\.\d+)?)\.ppm$")
GUEST_STAMP = re.compile(rb" t=([0-9.]+)")
SIM_WITH_ARCHIVE = "cdj-run-frames"


def parse_window(text: str) -> tuple[float, float]:
    """'40:70' -> (40.0, 70.0); '40:' runs to the end of the run; ':70' starts at zero."""
    start, _, end = text.partition(":")
    lo = float(start) if start else 0.0
    hi = float(end) if end else float("inf")
    if hi <= lo:
        raise ValueError(f"the window {text!r} ends before it starts")
    return lo, hi


def next_due(due: float, stamp: float, step: float) -> float:
    """The next grid time after a kept frame: the grid stays anchored at the window start, so the rate is
    exactly FPS however the frames fall (a 60 a second GUI would otherwise drift to 8.6 a second)."""
    while due <= stamp + 1e-9:
        due += step
    return due


def pick(stamps: list[float], fps: float, window: tuple[float, float]) -> list[float]:
    """The stamps to keep: inside the window, the first at or after each 1/fps step."""
    lo, hi = window
    kept: list[float] = []
    due = lo
    for stamp in sorted(stamps):
        if stamp < due or stamp >= hi:
            continue
        kept.append(stamp)
        due = next_due(due, stamp, 1.0 / fps)
    return kept


def guest_seconds(log: Path) -> Optional[float]:
    """The last guest time stamp (` t=SECONDS`) MAIN's link log carries, or None before the first."""
    try:
        with open(log, "rb") as stream:
            stream.seek(max(0, stream.seek(0, 2) - 65536))
            found = GUEST_STAMP.findall(stream.read())
    except OSError:
        return None
    return float(found[-1]) if found else None


def archive_simulator(simulator: Path) -> Path:
    """A simulator that has patch 15: the given one, or its cdj-run-frames sibling."""
    for candidate in (simulator, simulator.with_name(SIM_WITH_ARCHIVE + simulator.suffix)):
        try:
            if candidate.is_file() and b"BFIN_GUI_ARCHIVE_DIR" in candidate.read_bytes():
                return candidate
        except OSError:
            pass
    raise SystemExit(f"--dense-frames needs a simulator built with patches/15-gdb-17.2-gui-frame-archive.patch; "
                     f"{simulator} has none and there is no {SIM_WITH_ARCHIVE} beside it")


def ppm_to_png(source: Path, target: Path) -> None:
    from PIL import Image
    with Image.open(source) as image:
        image.convert("RGB").save(target, optimize=False)


def write_gif(frames: list[tuple[float, Path]], out: Path, end: Optional[float] = None) -> int:
    """Frames [(guest seconds, png)] as a GIF; each lasts until the next stamp (the last one 1 s or until END)."""
    from PIL import Image
    if not frames:
        return 0
    images, delays = [], []
    for n, (stamp, path) in enumerate(frames):
        later = frames[n + 1][0] if n + 1 < len(frames) else (end if end and end > stamp else stamp + 1.0)
        with Image.open(path) as image:
            images.append(image.convert("P", palette=Image.ADAPTIVE, colors=128))
        delays.append(max(20, round((later - stamp) * 1000 / 10) * 10))     # a GIF counts in 10 ms
    images[0].save(out, save_all=True, append_images=images[1:], duration=delays, loop=0, disposal=1)
    return len(images)


class DenseFrames:
    """Arms the simulator's frame archive inside a guest-time window and thins what it writes."""

    def __init__(self, directory: Path, window: tuple[float, float], fps: float,
                 guest: Callable[[], Optional[float]], gif: Optional[Path] = None):
        self.dir = Path(directory)
        self.window = window
        self.fps = fps
        self.guest = guest
        self.gif = gif
        self.png = self.dir / "png"
        self.kept: list[tuple[float, Path]] = []
        self.seen = 0
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self._due = window[0]

    def env(self) -> str:
        return f"BFIN_GUI_ARCHIVE_DIR={self.dir.resolve()}"

    def start(self) -> None:
        self.png.mkdir(parents=True, exist_ok=True)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self) -> int:
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=10)
        self.sweep()
        (self.dir / "ARM").unlink(missing_ok=True)
        return write_gif(self.kept, self.gif, self.window[1] if self.window[1] != float("inf") else None) \
            if self.gif else len(self.kept)

    def _arm(self, wanted: bool) -> None:
        arm = self.dir / "ARM"
        if wanted:
            arm.touch(exist_ok=True)
        else:
            arm.unlink(missing_ok=True)

    def sweep(self) -> None:
        """Keep the frames that are due, delete every PPM."""
        for path in sorted(self.dir.glob("f*-t*.ppm")):
            found = FRAME_NAME.match(path.name)
            if not found:
                continue
            stamp = float(found.group(2))
            self.seen += 1
            try:
                if self.window[0] <= stamp < self.window[1] and stamp >= self._due:
                    target = self.png / f"t{stamp:010.4f}.png"
                    ppm_to_png(path, target)
                    self.kept.append((stamp, target))
                    self._due = next_due(self._due, stamp, 1.0 / self.fps)
                path.unlink(missing_ok=True)
            except OSError:
                pass        # the simulator is still writing it; the next sweep takes it
            except Exception:
                try:
                    path.unlink(missing_ok=True)
                except OSError:
                    pass    # on Windows a file the simulator still has open cannot go; the next sweep takes it

    def _run(self) -> None:
        lo, hi = self.window
        while not self.stop_event.wait(0.05):
            now = self.guest()
            self._arm(now is not None and lo - 1.0 <= now < hi + 0.5)
            self.sweep()
