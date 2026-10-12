# cdj2000-emulator

An emulator for the Pioneer CDJ-2000 that runs the player's own firmware.

The CDJ-2000 has two processors, and this emulates both. The SH-4 that runs
the player boots on a QEMU machine written for it; the Blackfin BF531 that
paints the display boots on GNU's Blackfin simulator, patched; the two talk
over the same serial link they use on the real board. Every pixel is drawn by
Pioneer's code, and every button press travels the path a real press travels.

**It is a developer tool for firmware modding and reverse engineering**, a
place to run a change and watch what the machine does with it without risking
a player. It is not a way to use a CDJ-2000 on a desktop.

```
    ┌─────────────────────┐  serial link   ┌────────────────────────┐
    │  MAIN board  SH-4   │◄──────────────►│  GUI board  BF531      │
    │  QEMU: cdj2000-main │                │  GNU sim, patched      │
    │  flash, SDRAM, DMAC │                │  PPI → 480x234 RGB555  │
    │  panel, SD, ATAPI,  │                │  SPORT, DMA, CFI flash │
    │  USB, audio DSP     │                │                        │
    └──────────┬──────────┘                └───────────┬────────────┘
               │  TCP control channel                  │  framebuffer
               ▼                                       ▼
         panel_control                            the window
```

## What works

* Both boards boot their stock firmware (MAIN 4.33, GUI 4.20 or the NXS 1.44
  image), complete their handshake and reach the player screen in about 30 s
  of wall clock. MAIN's RTOS tick runs at its real 1000 Hz.
* The panel is drawn at 480x234 from the display DMA, and all 48 inputs the
  board decodes -- 40 buttons and 8 analogue fields, rotary encoder included --
  are driven from the host through the real panel path. MAIN's own service-mode
  name table says which bit is which key (`INPUT_MANIFEST.md`).
* The disc drive, the audio DSP and the USB device report up, so no caution
  banner stands in the way; MAIN's service monitor and caution store are
  readable over the emulated debug port.
* In the legacy CDJ-2000 workflow, an SD card image with a rekordbox export is the library: categories,
  playlists and track lists on screen, a track loaded with its overview
  waveform, detail waveform, beat grid, cue markers, duration, BPM and key,
  and a time display that runs from PLAY (`tools/cdj_main/twoboard.py` runs
  that recipe into a fresh run directory).
* The legacy DSP model answers the load handshake, keeps the position report,
  locates, loops on the beat, and raises events on its interrupt line.
* The NXS workflow boots stock firmware, browses an SD WAV fixture using native
  panel controls, loads its track name and duration, and advances the native
  playhead with `--functional-dsp-audio`. This proves the firmware playback
  path; the emulator still does not provide audible output.
* A USB stick is a disk image on the SoC's USB host module. With the two
  update keys held at power-on, MAIN's updater takes a `C2KMAIN.UPD` from it
  and rewrites the flash model; the boot ROM's recovery path (the loader at
  flash 0x10000, run when the application checksum fails) does the same; and
  with the GUI board on the link a `C2KGUI.UPD` is streamed to it, checked,
  erased and programmed into the simulator's flash, dumped afterwards for
  comparison. `tools/cdj_main/make_upd.py` builds a MAIN update file from any
  flash image. See "A firmware update" in RUNNING.md.

## What does not

* **No verified live speaker audio.** The experimental NXS McASP1 WAV sink
  has so far been checked at idle boot; connected PLAY output remains to be
  validated. The DSP is a TI Aureus DA710 with a TMS320C674x core (see
  `RUNNING.md`), and `emulator/qemu/cdj_c674x.c` executes its instruction set
  from TI's published SPRUFE8B - partially: see `DSP_ARCHITECTURE_COVERAGE.md`
  for what is and is not implemented. Instruction coverage is incomplete;
  accepting a mnemonic does not establish support for all its encodings or
  correct execution.
  Native NXS playback also depends on DSP device and timing behavior.
* **No jog**, no pitch. The position report runs at nominal speed.
* In the legacy scripted workflow, the detail waveform and beat grid reach the GUI through the link proxy
  (`tools/cdj_main/link_inject.py`), which also injects the browse and load
  requests. The NXS native browser can select and load with the encoder's
  ENTER contact; short clicks and long holds have different meanings.
* Switching sources after boot is unreliable (six of eight); the card given at
  launch is reliable. The USB stick as a music source has not been tried.
* The default GUI simulator (GNU sim, `--gui-sim gdb`) is about thirty times
  slower than the chip on real work, and the live link has intermittent stalls
  and, rarely, a double fault. Run it again; the fault line is in the
  simulator's log. `--gui-sim fast` runs the GUI board on a faster vendored
  Blackfin core; see BUILD.md.
* Pro DJ Link between players is limited to `tools/cdj_main/link_hub.py`
  (emulated decks, and on macOS a real interface with `--bridge`).

## Firmware is not included

**This repository contains no Pioneer firmware and never will.** You supply
your own copy of the firmware update, which the manufacturer distributes free
to owners, and the extractors turn it into the images the emulators load. See
[FIRMWARE.md](FIRMWARE.md). Nothing here is derived from Pioneer's code: no
images, no disassembly, no screenshots.

## Getting started

### NXS research branch: interactive deck

For firmware development, start a deck in one command:

```sh
python -m tools.cdj_main.launch deck                  # generated test track
CDJ_USB=path/to/usb.img python -m tools.cdj_main.launch deck   # your USB image
python -m tools.cdj_main.launch deck --sd card.img --seconds 600
```

`deck` is `nxs_vm --ui --debug --seconds 3600 --functional-dsp-audio
--source-key-when-ready` with `--usb $CDJ_USB` or `--test-track`; any option
you give wins. Ports are picked automatically (the first free block of 5980,
5990, ...), the run directory is timestamped, and DSP capture is fault-only;
pass `--no-lightweight` when you need every checkpoint and the event
transcript for replay. `python -m tools.cdj_main.nxs_vm --help` groups the
remaining options. `--dsp-thread` (the DSP on its own host thread) and
`--gui-sim fast` are faster opt-ins that will become defaults once qualified.

This creates a timestamped run under `runs/`, prints follow-up commands, and
records the firmware and emulator hashes. The deck shows run progress below
the LCD; **Diagnostics** provides session state, browser replies, frame age,
recent actions, fault lines, and debugger endpoints. Fresh-only link delivery
is the NXS default because cached repeats saturated the GUI receive queue in
connected runs. Use `--cached-link` only to compare the old transport behavior.
The source key waits for the SD or USB browser table instead of a guessed timestamp.

For McASP clock experiments, add `--virtual-mcasp-clock` alongside
`--functional-dsp-audio`. This batches genuine transmit slots from the
configured McASP1 clock while giving the DSP interpreter bounded time slices.
Add `--host-dsp-audio-wav` to write that stream through QEMU's 44.1 kHz stereo
WAV backend to `dsp-audio.wav` in the run directory. These modes are
experimental and do not establish DSP instruction timing or connected PLAY
output.

For an offline clock-coupled experiment, use `--dsp-cycle-mcasp-clock` with
`--functional-dsp-audio --render-dsp-audio-wav`. This advances each McASP1 slot
after the configured number of modeled SYSCLK1 cycles (4,416 cycles per slot
for the observed 44.1 kHz firmware setup). It runs more slowly than audio
wall time and does not feed a live 44.1 kHz voice. A connected load and
continuous nonzero output have not yet been verified in this mode.

For a WAV paced by DSP slot progression, add `--render-dsp-audio-wav` with
`--functional-dsp-audio`. This writes every McASP1 stereo slot pair to
`dsp-render.wav` at the firmware's configured 44.1 kHz format. With the
default packet-based slot scheduler, its header rate labels the sample
sequence; it does not measure real-time playback or drive speakers. The same
speaker limitation applies to the DSP-cycle experiment above.

Agents can control the same run without locating ports:

```sh
python -m tools.cdj_main.dev runs/my-run status --json
python -m tools.cdj_main.dev runs/my-run press enter
python -m tools.cdj_main.dev runs/my-run wait-playback --timeout 120
python -m tools.cdj_main.dev runs/my-run switch rev off
python -m tools.cdj_main.dev runs/my-run qmp registers
python -m tools.cdj_main.dev runs/my-run stop
```

See [DEVELOPING.md](DEVELOPING.md) for track selection, memory inspection,
screenshots, firmware overrides, and reproducible agent handoffs.

For the NXS firmware already prepared under `firmware/nxs/`, use the dedicated
launcher from this repository directory:

```sh
python -m tools.cdj_main.nxs_vm runs/nxs-interactive --seconds 3600 --ui
```

Choose a new run-directory name each time. `--ui` opens the current interactive
deck and connects its buttons to that run's panel port; without it the run is
headless. Close the deck to stop both emulators. Restart an older viewer to pick
up Python changes, and rebuild `bin/cdj-run` after simulator patch changes.

Ordinary hardware buttons support mouse-down/up and Enter/Space holds, including
release outside the button or on focus loss. MENU holds now open the real
firmware's UTILITY screen after the SIC mask-order fix (`69d0d88`). The separate
E-7206 auth-chip error remains. Native NXS SD loading of `TESTTONE.WAV` was
reproduced with stock firmware on 2026-09-15, including the ten-second duration
on the display. USB track loading and audible playback remain unverified.
The legacy functionality described above is not an NXS completion claim.
See [DEVELOPING.md](DEVELOPING.md), [RUNNING.md](RUNNING.md) and
NXS_GUI_STALL.md for current evidence and limitations.

The NXS launcher also accepts experimental `--sd IMAGE` and `--usb IMAGE`
mounts. Generate a plain WAV/FAT32 fixture with
`python -m tools.cdj_main.test_media runs/test-media`, then supply
`runs/test-media/test-track.img`. Each image uses a disposable QEMU overlay;
guest writes are discarded when the run closes. Image attachment alone does not
establish firmware track loading or audible playback.
Add `--trace-media` to record SD-controller and USB-host register activity in
`main-stderr.log`. This diagnostic adds host overhead and can change timing;
it does not enable the legacy fake media-state RAM write.
For insertion diagnostics, `--sd-insert-seconds 110` schedules the existing
card-presence transition 110 virtual seconds after reset; `0` keeps the slot
empty. This requires `--sd` or `--test-track`. The default remains the controller's 20 seconds.
Virtual seconds are not a promise about wall-clock boot time.

The media manager can take longer than the first source press. Add
`--source-key-retries 5` to retry the named source automatically (the default
interval is 120 virtual seconds), or set `--source-key-at` explicitly when a
firmware change has a known readiness point.

To quickly inspect what a run achieved, use the read-only run report:

```sh
python -m tools.cdj_main.run_report runs/my-run
python -m tools.cdj_main.run_report runs/my-run --json
```

It summarizes visible media entries, native load responses, framebuffer
observations, common emulator fault lines, and the raw artifact files. The
report keeps audio playback separate from track-load evidence.

While a run is still active, `python -m tools.cdj_main.run_state runs/my-run`
prints the latest browser reply and frame status. Add `--json` for an agent
to consume the bounded snapshot, including the current session endpoints and
recent diagnostic lines.

### Original CDJ-2000 setup

```sh
pip install -r requirements.txt
sh scripts/build-bfin-sim.sh                                   # the GUI board
git clone --depth 1 https://gitlab.com/qemu-project/qemu.git /c/qemu-src
sh scripts/build-qemu-sh4.sh /c/qemu-src                       # the MAIN board
# put C2KGUI.UPD and C2KMAIN.UPD in firmware/, then:
python -m tools.cdj_gui.extract     firmware/C2KGUI.UPD  firmware
python -m tools.cdj_gui.main_unpack firmware/C2KMAIN.UPD firmware
python -m tools.cdj_main.view_vm
```

[BUILD.md](BUILD.md) has the platform notes. [RUNNING.md](RUNNING.md) has
everything you can do once it boots: the environment knobs, the two-board
recipe, the speed numbers, the update procedure, and the measurements behind
each claim above.

## One rule about the UI

Only the inner rectangle is the 480x234 panel. `BROWSE` / `TAG LIST` / `INFO`
/ `MENU` and `LINK` / `USB` / `SD` / `DISC` are hardware buttons, backlit
plastic that appears in no frame the firmware draws. Virtual buttons belong
beside the panel image, never in it; `tests/test_panel_layout.py` enforces
that the captured frame is shown untouched.

## Layout

| path | what |
|---|---|
| `emulator/qemu/` | the SH-4 MAIN board: machine, panel, link, SD, ATAPI, USB host, DSP |
| `emulator/*.hw` | GNU sim board descriptions for the Blackfin side |
| `patches/` | what has to change in QEMU and in GDB's simulator, and why |
| `tools/cdj_main/` | launchers, panel control, the two-board recipe, card and update images |
| `tools/cdj_gui/` | the viewer, the firmware extractors, link decoders, stimulus generators |
| `tests/` | the host-side test suite; most of it needs no emulator |
| `INPUT_MANIFEST.md` | all 48 inputs, what was done with each, what was measured |

`patches/README.md` is worth reading on its own: four omissions in QEMU's SH-4
interrupt handling that are invisible to Linux and fatal to a uITRON RTOS, a
Blackfin packed-ALU instruction that committed a cycle early, and the AMD
command set the CDJ's flash actually speaks.

For firmware-free integration checks against the built QEMU board, run
`sh scripts/test-qemu-integration.sh`. It rebuilds the board and exercises
the actual HPI, SH-4 interrupt/timer, DMA and ATA paths; a missing QEMU build
cannot turn those tests into skips.

## Licence

`GPL-2.0-or-later`. See [LICENSE](LICENSE), and [THIRD_PARTY.md](THIRD_PARTY.md)
for what is patched and under what terms.

One exception: `cdj-gui-run`, built from `emulator/bfin/`, is
GPL-3.0-or-later as a whole, because `emulator/bfin/bfin_dsp.c` carries GNU
sim (GDB 17.2) code; see [emulator/bfin/LICENSE.md](emulator/bfin/LICENSE.md).
The rest of the repository, `emulator/qemu/` in particular, stays
GPL-2.0-or-later.

## Not affiliated with Pioneer

This is an independent project, not endorsed by, affiliated with, or supported
by Pioneer DJ, AlphaTheta, or any successor. "CDJ" and "Pioneer" are their
trademarks and are used here only to say which hardware this emulates.
