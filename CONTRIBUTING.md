# Contributing

Thanks for looking. This project runs the CDJ-2000's own firmware on two
emulated processors; see [README.md](README.md) for what it does and does not
do, and [BUILD.md](BUILD.md) and [FIRMWARE.md](FIRMWARE.md) to get a working
setup. Once it runs, [DEVELOPING.md](DEVELOPING.md) describes how to drive a
CDJ-2000NXS run, inspect it and hand its evidence to someone else.

## The one hard rule: no firmware

**This repository contains no firmware and never will.** That covers:

* update files (`*.UPD`, `*.LDR`) and anything unpacked from them (`*.bin`,
  `*.elf`, flash images, resource tails, decompressed code or data);
* disassembly or decompilation output of the firmware, and dumps of its tables
  beyond a few values quoted to make a point;
* screenshots or frame dumps that reproduce firmware artwork in bulk.

This applies to commits, pull requests, issues, comments and attachments alike.
The `.gitignore` already excludes the usual file types; do not force-add past
it. Tests that need a firmware image must skip when it is absent -- see
[FIRMWARE.md](FIRMWARE.md) for how you supply your own.

If firmware ends up in a comment or an issue by accident, tell a maintainer so
it can be removed (deleting it yourself does not clear GitHub's caches).

Describe what the firmware does in your own words and with measurements: sizes,
offsets, hashes, register names, message formats. That is what
[INPUT_MANIFEST.md](INPUT_MANIFEST.md) and the notes in `BUILD.md` do, and it
is the style to follow.

## Before you open a pull request

* Run `python -m pytest tests/ -q`. CI runs it on Python 3.10 and 3.12, without
  firmware, and also checks that the GDB patches still apply to a pristine
  `gdb-17.2`.
* Read [CHANGE_SCOPE.md](CHANGE_SCOPE.md): it says which code belongs to the
  CDJ-2000 profile, which to NXS and which to both. `emulator/qemu/cdj2000_main.c`
  serves both, so a change there must be tested against `cdj2000-main` and
  `cdj2000nxs-main`, and a run is evidence for its own profile only.
* Say what you measured. A claim that the board now does X is best backed by a
  run directory report or a test, and a verdict in `INPUT_MANIFEST.md` is a
  measurement in a named world, not an intention. `DEVELOPING.md` names the
  evidence levels for a track-load run (*listed*, *loaded*, *counter-moving*);
  use them rather than claiming more than the run showed.
* Keep changes focused. A large change is easier to review as a few pull
  requests.

## Licence

By contributing you agree that your work is licensed as the rest of the
repository: `GPL-2.0-or-later`, except the patches against GNU GDB, which stay
`GPL-3.0-or-later`. See [LICENSE](LICENSE) and
[THIRD_PARTY.md](THIRD_PARTY.md). Do not contribute code you cannot license
this way, and do not copy from other emulators or from the firmware.

## Security problems

Report them privately; see [SECURITY.md](SECURITY.md).
