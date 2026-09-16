# Agent-assisted firmware development

The NXS workflow runs your MAIN and GUI firmware and exposes observations and
controls through one run directory. Python dependencies and both emulator
binaries must already be installed; see README.md and BUILD.md for setup.

## Start a development session

From the repository root:

```sh
python -m tools.cdj_main.nxs_vm --test-track --debug --lightweight --ui \
  --seconds 1800 --fresh-link --functional-dsp-audio \
  --source-key-at 300 --source-key-retries 0
```

The launcher chooses a fresh timestamped directory under `runs/` and prints its
path and follow-up commands. You can also give an explicit new path as the first
argument. Occupied ports and missing inputs are reported before starting the
boards. Use `--port 6380` for a second independent session.

`--test-track` creates a 512 MiB FAT32 image with a ten-second stereo PCM WAV
inside the run. It cannot be combined with `--sd`; use `--sd IMAGE` to reuse an
existing fixture. Guest media writes use disposable overlays. `--lightweight`
omits the large normal DSP event capture while retaining fault-triggered
checkpoints; omit it when the full event transcript is needed. `--fresh-link`
is an explicit experimental transport mode, recorded in the manifest; it is the
mode used for the native-loading validation below.

`--functional-dsp-audio` supplies the existing coarse McASP clock so the DSP
can process sample slots; it does not enable audible output or promise real-time
speed. Strict defaults leave that clock stopped. The neutral NXS panel frame
holds the active-low REV contact high. All-zero raw input selects reverse and
can hold a loaded track at its start boundary. The default source press above
waits for media initialization; retries during browsing can reset the selection.

The default 60-second run is useful for a boot smoke test. Give a longer duration
for browsing: media readiness can take several minutes, and an early source
press can produce **NO CARD** even though the image is attached. Source retries
are scheduled in virtual seconds, which need not match host seconds.

For a changed firmware build, add `--main-firmware PATH` and/or
`--gui-firmware DIRECTORY`. The GUI directory must contain the matched
`gui-boot-memory.elf` and `gui-flash-image.bin`. The launcher creates a run-local
board configuration pointing to that flash. Original stock files are preserved;
before/after hashes identify the inputs used by each run.

## Observe and control the same run

Replace `runs/my-run` with the path printed at launch:

```sh
python -m tools.cdj_main.dev runs/my-run status --json
python -m tools.cdj_main.dev runs/my-run wait-browser TESTTONE.WAV --timeout 600
python -m tools.cdj_main.dev runs/my-run screenshot runs/my-run/browser.png
```

The CLI resolves panel and debugger endpoints from `run.json`; it uses the NXS
button map. `enter` means the encoder push, `back` means RETURN, and `rotary`
moves the select encoder by signed steps. Presses default to 100 ms; specify
`--hold-ms` for a deliberate hold. The NXS browser treats long holds differently
from short selection clicks. Do not apply the legacy CDJ-2000 multi-second
selection recipe to this workflow.

When SD shows `[FOLDER]` with `TESTTONE.WAV` in the preview, press ENTER once to
open the folder. Inspect the next screenshot; when the track itself is selected,
press ENTER again to request loading. A panel `ok press` reply means the input
was queued. A short press can miss a firmware sample: inspect the screen and
link observations before retrying. Avoid blindly queuing several ENTER presses.

Use `press enter --hold-ms 300` for each selection, `rotary 1` to move the
highlight, and a new screenshot filename for each inspection. Once the selected
track has been opened, wait for its duration:

```sh
python -m tools.cdj_main.dev runs/my-run wait-duration --timeout 180
```

`wait-browser` requires a complete matching browser reply and catches up with
the available link log before declaring a match. It returns 1 on timeout and
2 on errors; mutations also fail for stopped sessions. By default, both waits
can match the latest record already present when the command starts. Use
`--after-record N` to require a matching record newer than the cumulative
`records` value from a previous wait or persistent observer on the same log.
Do not use numbers from a one-shot status marked `local to tail`; those use a
different origin. A matching preview row
establishes listing, not completed loading. The JSON status includes raw
duration/status replies so an agent can check further progress. Screenshots
require a complete framebuffer, crop only display blanking lines, and refuse
to overwrite an existing output.

After the track is selected and loading is requested, `wait-duration` waits for
a complete command-5 duration reply with a nonzero field in words 3 and 4. It returns the
raw payload and the decoded field for inspection; observing that reply does not
prove that playback has started. It uses a bounded 120-second timeout by
default, returns 1 when no complete duration arrives, and returns 2 when the
run stops or fails first.

Check the counter after loading; stock settings may start playback immediately.
If `link.transport.play_requested` is false, send PLAY, then check again:

```sh
python -m tools.cdj_main.dev runs/my-run press play --hold-ms 300
python -m tools.cdj_main.dev runs/my-run wait-playback --timeout 120
```

`play_requested` describes the native transport flag, not observed movement.
Use `switch rev off` or `switch rev on` to set REV by its logical state. Named
`press rev` is refused because a momentary high pulse cannot operate an
active-low switch correctly. The deck also translates REV polarity. Raw
inspector contacts remain literal electrical levels.

`wait-playback` first catches up with the link, then requires two decreases and
at least 150 frames (one second) of total counter movement across fresh records
at a constant duration. Use `--min-frames N` to change that threshold. Brief
buffered movement followed by a stall does not satisfy the default check. Duplicate records,
old playback history, and a loaded duration cannot satisfy it. A seek or duration
change resets the sequence. The result establishes counter movement; it does
not test audio or distinguish playback from repeated jog input. It returns 1
on timeout and 2 on errors. Verify native pause and resume separately.

The deck's **Diagnostics** window shows run state, recent actions, browser
progress, frame age, and recent faults. **Copy snapshot** copies bounded JSON.
`link.transport` decodes valid native time fields into remaining, elapsed, and
duration frames at 150 frames per second. A single counter sample does not
prove playback: compare newer record numbers for decreasing remaining time,
then verify that PLAY toggles pause and holds the counter steady.
Attach another viewer without reconstructing its options:

```sh
python -m tools.cdj_gui.view_ui --run runs/my-run
```

Closing a separately attached viewer leaves the emulators running. Closing the
viewer launched by `nxs_vm --ui` stops that launcher's boards.

## Debug MAIN

```sh
python -m tools.cdj_main.dev runs/my-run qmp status
python -m tools.cdj_main.dev runs/my-run qmp pause
python -m tools.cdj_main.dev runs/my-run qmp registers
python -m tools.cdj_main.dev runs/my-run memory 0x04000000 --length 64
python -m tools.cdj_main.dev runs/my-run qmp resume
```

`press play` sends the firmware's native PLAY control; pressing it again
toggles the track back to pause. This is separate from `qmp pause`, which
freezes the emulated MAIN CPU for debugger inspection.

These commands require `--debug`. Add `--debug-paused` to start MAIN at reset.
Pause affects MAIN only: the GUI simulator and host run deadline keep advancing.
The memory command reads MAIN physical addresses and is capped at 4096 bytes;
observations while running are not atomic. It returns QEMU's raw monitor text.
For source-level stepping and breakpoints, connect an SH-4-capable GDB to the
localhost endpoint printed by the launcher (base port + 3), using symbols that
match your firmware build.

Request an on-demand DSP checkpoint at the next safe DSP/HPI boundary while
the run is active:

```sh
python -m tools.cdj_main.dev runs/my-run checkpoint --timeout 30
```

The command serializes clients, creates an exclusive request marker, and waits for the board's
completion response. A successful response validates the checkpoint and
copies it into `runs/my-run/debug-checkpoints/<checkpoint-name>/`, alongside a
diagnostic provenance manifest. The live `dsp-checkpoints/manifest.json` is
left alone, so an in-progress full capture cannot be clobbered. The returned
sidecar is replayable for fault diagnosis:

```sh
python -m tools.cdj_dsp.replay \
  runs/my-run/debug-checkpoints/000001.cdjdsp \
  runs/my-run/dsp-debug-replay --steps 10000
```

This snapshot has fault-only provenance: it does not include a connected event
transcript and remains ineligible for architectural validation. Omit
`--events`. A timeout returns a nonzero status and leaves the request marker
in place for the running board to finish; it does not claim that capture
failed or cancel the request. The command requires a live run with the
checkpoint endpoint recorded in `run.json`. Replay flags must match the captured
run: add `--functional-dsp-audio` for `coarse-packet-slots`, and
`--functional-dsp-timing` for `functional-runahead`. Use the scheduler mode
recorded in the manifest. Directory replay rejects mismatched modes.

Lightweight runs automatically retain DSP checkpoints when a fault occurs,
and also support the explicit checkpoint command above. Replay a fault capture
for diagnosis with the checkpoint directory; omit `--events` because that run
has no connected event transcript:

```sh
python -m tools.cdj_dsp.replay runs/my-run/dsp-checkpoints \
  runs/my-run/dsp-fault-replay --steps 10000
```

This path records `diagnostic_connected_checkpoint` provenance and keeps
architectural validation disabled. It is useful for inspecting the fault and
its surrounding trace, not for claiming a complete connected replay.

## Stop and hand off evidence

```sh
python -m tools.cdj_main.dev runs/my-run stop
python -m tools.cdj_main.run_state runs/my-run --json
python -m tools.cdj_main.run_report runs/my-run --json
```

`stop` requests shutdown; it does not claim the boards have already stopped.
The launcher consumes a run-local request, terminates its own child processes,
and finalizes `session.json`, `result.json`, and input hashes. Check for session
state `stopped` before treating final artifacts as complete. This requires the
updated launcher; a process started by older code does not consume the marker.

Useful handoff artifacts:

| File | Contents |
| --- | --- |
| `run.json` | Commands, endpoints, options, media paths, input hashes |
| `session.json`, `result.json` | Lifecycle and exit results |
| `actions.jsonl` | UI/CLI requests, replies, failures, timestamps |
| `screen.ppm`, captured PNGs | Firmware framebuffer observations |
| `main-link.bin` | Delivered MAIN-to-GUI records |
| `main-stderr.log`, `main.log`, `gui.log` | Device, CPU and simulator diagnostics |
| `frames/manifest.json` | Optional periodic captures via `--frame-interval N` |

One-shot status reads the latest 1 MiB of link data, with record numbers scoped
to that window; older browser and duration replies may be absent. The deck and
browser waits retain an incremental observer; `caught_up: false` means their
link observations are still historical. The completed-run report streams full
logs, counts all records, and retains the latest 100 browser rows and diagnostic
lines. Neither an unchanged frame nor a lack of recent fault lines proves CPU
liveness. A successful `wait-playback` action proves fresh native counter
movement; it still does not test audio output.

## Troubleshoot and hand off a failed run

When a step does not advance, collect a bounded snapshot before pressing more
buttons or stopping the run. This preserves the failure timing and avoids
turning a missed sample into an ambiguous retry sequence:

```sh
python -m tools.cdj_main.dev runs/my-run diagnose runs/my-run/handoff-01
python -m tools.cdj_main.run_state runs/my-run --json > runs/my-run/state-at-failure.json
python -m tools.cdj_main.run_report runs/my-run --json > runs/my-run/report-at-failure.json
python -m tools.cdj_main.dev runs/my-run screenshot runs/my-run/failure.png
```

`diagnose` creates a new directory containing status, launch/session/result
metadata, the framebuffer, 64 KiB tails of logs and actions, and an inventory
with capture hashes. It works on stopped runs and requires no debugger endpoint.
It reports missing artifacts as partial evidence and refuses to replace an
existing directory. Capture is not atomic; retain the original run for full
link traces, firmware, media, and checkpoints. The separate full `run_report`
command above can take longer on large traces.

For a live NXS run launched with `--debug`, inspect the SD mount gate before
trying another source press:

```sh
python -m tools.cdj_main.dev runs/my-run wait-media --timeout 120
```

This reads the stock NXS readiness globals through QMP and waits for readiness,
card presence, and the mount latch together. Its success does not prove files
were listed; follow with `wait-browser TESTTONE.WAV`. Reads are non-atomic and
the addresses are specific to the supported stock firmware. A changed firmware
layout requires updating the observer. `--timeout 0` returns one sample.

Record the exact command, virtual second, and record number that preceded the
failure. Virtual seconds are emulator time and do not equal wall-clock seconds;
host load, QEMU scheduling, and diagnostic tracing can change elapsed wall time.
`dev press --hold-ms` uses guest virtual milliseconds (default 100); wait
timeouts and poll intervals use wall-clock seconds.
Each wait has its own predicate, so a timeout means that predicate was not
observed, not that the firmware is dead. Check `session.json` and `result.json`
after `stop`; a requested stop is not a completed stop.

For a DSP or emulator fault, preserve `main-stderr.log`, `main.log`, `gui.log`,
`actions.jsonl`, `main-link.bin`, and any `dsp-checkpoints/` or `frames/`
directories together with `run.json`. Do not edit or rename run files while an
emulator is active. A checkpoint timeout leaves its request marker for the
board to finish; inspect the final session state before deciding whether the
capture failed. Replay completed checkpoints with the scheduler and functional
DSP flags recorded in their manifest.

Use these evidence labels in a handoff: *listed* (a browser preview row),
*loaded* (a nonzero command-5 duration reply and matching frame), and
*counter-moving* (fresh decreasing native time records). `wait-playback`
establishes only the last label, which can still result from repeated jog input.
None establishes PCM generation, an audible signal, or hardware audio output;
those require separate audio capture and validation.

Include the run path, repository revision, resolved emulator and firmware hashes
from `run.json`, command-line flags, first failing predicate, and artifact paths
in the handoff. A new agent should begin with `run_state` and `run_report`, then
inspect existing evidence before issuing another mutation.

## Live validation, 2026-09-15

Stock NXS MAIN (`02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85`)
and the existing stock GUI were exercised with the deterministic WAV fixture,
fresh-only link delivery, strict DSP defaults, and native panel controls.
The run progressed from NO CARD to folder and track listings. A missed short
click required a retry; subsequent native loading produced a command-5 duration
payload and a screen showing `TESTTONE.WAV`, `TRACK 01`, `REMAIN 00:10.000`.
With `--functional-dsp-audio`, fresh native status records then advanced the
10-second playhead by 150 frames with 63 decreasing samples. PLAY cleared the
native `play_requested` flag and held the counter steady; a second PLAY resumed
movement. This validates the firmware media and transport path without claiming
an audible signal.

Local evidence is under `runs/agent-dev-stock-20260915/`, including
`actions.jsonl`, `loaded-testtone.png`, and the raw link log. These captures stay
local and are not committed. A separate launcher smoke test exercised generated
media, the GUI firmware override, the deck, paused reset and QMP resume. This
validates the development workflow, test-track loading, and native counter
playback, not audio output.
