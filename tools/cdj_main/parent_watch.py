"""Make emulator processes die with the launcher, even on SIGKILL.

The launcher owns the write end of a pipe (a ``Lifeline``) and gives the read
end to every child as ``CDJ_PARENT_FD``.  Nobody ever writes, so a read that
returns 0 means every holder of the write end is gone.  The kernel closes it
when the launcher dies, however it dies.  ``qemu-system-sh4``
(emulator/qemu/cdj_parent_watch.c) and ``cdj-run``
(patches/15-gdb-17.2-parent-watch.patch) watch it themselves; Python children
call ``watch()``.  macOS has no PR_SET_PDEATHSIG, hence the pipe.

A launcher that was itself started by something else (``nxs_vm`` under
``emulator_ui_session``) has no pipe from it, so ``watch()`` falls back to
polling for a changed parent pid.  Set ``CDJ_NO_PARENT_WATCH=1`` to disable
that (for example under ``nohup``).

Idea (a pipe, not getppid polling, so ``kill -9`` is covered) from the
evaluation of Stijn-Jacobs/cdj-nxs2-qemu, whose launcher-only handling
(launcher/chain.py) polls ``os.getppid()`` and so cannot survive its own kill.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import os
import subprocess
import threading
import time

ENV = "CDJ_PARENT_FD"


class Lifeline:
    """Spawn children that exit when this process dies."""

    def __init__(self) -> None:
        self.read_fd = self.write_fd = -1
        if os.name != "nt":  # pass_fds is POSIX only
            self.read_fd, self.write_fd = os.pipe()

    def popen(self, command, **kwargs) -> subprocess.Popen:
        if self.read_fd >= 0:
            env = dict(kwargs.pop("env", None) or os.environ)
            env[ENV] = str(self.read_fd)
            kwargs["env"] = env
            kwargs["pass_fds"] = (*kwargs.get("pass_fds", ()), self.read_fd)
        return subprocess.Popen(command, **kwargs)


def inherited_fds() -> tuple:
    """pass_fds for a Python child that starts its own simulator."""
    fd = os.environ.get(ENV, "")
    return (int(fd),) if fd.isdigit() and os.name != "nt" else ()


def watch(on_death, grace: float = 15.0) -> None:
    """Call ``on_death()`` when the launcher dies; ``os._exit`` after ``grace``."""
    fd = os.environ.get(ENV, "")
    ppid = os.getppid()

    def run() -> None:
        if fd.isdigit():
            try:
                while os.read(int(fd), 64):
                    pass
            except OSError:
                pass
        else:
            while os.getppid() == ppid:
                time.sleep(0.5)
        try:
            on_death()
        finally:
            time.sleep(grace)
            os._exit(1)

    if os.name == "nt" or os.environ.get("CDJ_NO_PARENT_WATCH"):
        return
    if fd.isdigit() or ppid > 1:
        threading.Thread(target=run, name="parent-watch", daemon=True).start()


def exit_with_parent() -> None:
    """For helpers with nothing to clean up (viewers, proxies)."""
    watch(lambda: os._exit(0), grace=0)
