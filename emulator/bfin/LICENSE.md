# Licence of `emulator/bfin/` (`cdj-gui-run`)

`cdj-gui-run`, the program built from this directory, is distributed under
**GPL-3.0-or-later** as a whole; the full text is in [COPYING3](COPYING3).

That is because `bfin_dsp.c` carries GNU sim code from GDB 17.2, which is
GPL-3.0-or-later (its SPDX line says so). Every other file here keeps its own
`GPL-2.0-or-later` SPDX line: the Blackfin core from Stijn Jacobs'
cdj-nxs2-qemu and this project's own files are GPL-2.0-or-later, and
"or later" lets them be combined into a GPL-3.0-or-later program. Taken on
their own, they remain available under GPL-2.0-or-later.

Nothing outside this directory is affected. In particular `emulator/qemu/`
must stay GPL-2.0-or-later: it is compiled into QEMU, which has
GPL-2.0-only files. No file here is linked into QEMU, and `cdj-gui-run`
talks to it only over a socket.
