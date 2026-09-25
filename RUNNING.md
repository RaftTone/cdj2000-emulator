# Running

Everything below assumes you have built both emulators (BUILD.md) and extracted
your own firmware into `firmware/` (FIRMWARE.md). Run every command from the
repository root.

## The whole player, in a window

Use the unified profile selector when you do not want to remember the two
launcher module names:

```sh
python -m tools.cdj_main.launch 2000
python -m tools.cdj_main.launch nxs runs/nxs-deck --ui --seconds 3600
```

`2000` dispatches to the original CDJ-2000 `view_vm` launcher; `nxs` dispatches
to the NXS `nxs_vm` launcher. Arguments after the model are passed through.
The direct commands below remain useful when you want profile-specific help.

```sh
python -m tools.cdj_main.view_vm
```

MAIN boots on QEMU, the GUI board boots on the Blackfin simulator, the two are
linked, and the GUI's framebuffer appears in a window with the player's controls
drawn around it.

The default device view fits the deck to the window, with the firmware LCD
kept separate from the controls. Resize freely, or use **Full screen** and
Escape. **Inspector** opens the unassigned digital/analogue inputs and control
channel tools; **Controls ?** explains the gestures. Ordinary mouse-down holds
a key until mouse-up (including outside its hit box) or deck focus loss.
Closing an attached viewer releases its owned contacts without clearing analog
settings. Shift-click is a long
press, Ctrl/right-click latches a key, and the browse knob accepts drag and
scroll without also sending a push. Arrow keys navigate the focused deck;
holding Enter or Space holds the originally focused control until key-up or
focus loss. Inspector and lab buttons also support mouse and keyboard holds;
auto-repeat does not enqueue additional presses. Releasing one input source
does not release another source or a right-click latch on the same key.
The UTILITY shortcut and mouse browse push remain timed gestures. Ordinary
holds bypass the serialized pulse queue; rapid clicks can still miss firmware
sampling, so hold a key deliberately when testing. Lights represent host input
feedback, not decoded hardware LEDs. The existing `--skin lab` viewer remains
available for bit-level work; `--scale` controls that view's integer zoom.

For the experimental NXS profile, use a new run directory:

```sh
python -m tools.cdj_main.nxs_vm runs/nxs-deck --ui --seconds 3600
```

The launcher owns both emulators; the deck attaches to their framebuffer and
input port. Closing the deck stops that run. This does not remove the NXS
profile's remaining DSP limitations or add jog rotation/audio.
To view an existing NXS run without starting or stopping its emulators:

```sh
python -m tools.cdj_gui.view_ui --attach --device-name CDJ-2000NXS \
  --output runs/nxs-deck/screen.ppm --control-port 5984
```

An attached viewer can also display a saved frame; a static image alone is
not evidence that either emulator is running. After five seconds without a
framebuffer publication, the status bar reports its age and marks emulator
liveness unverified rather than retaining an old FPS value. This does not
mean the emulator is stopped: firmware may simply leave the picture unchanged.

**It takes half a minute to become interesting.** Measured with
`boot_vm --poll-every 5 --frames`: black until about 15 s, the Pioneer logo at
about 20 s, the rekordbox logo at 25 s, and the player screen showing `NO DISC`
-- the handshake with MAIN complete, the record stream running -- at about
35 s. MAIN's handshake words (`GuiCom mode`, the ready words) are set within
the first three seconds; the rest is MAIN's own boot sequence.

**And it may still not get there.** The GUI board's double fault at
`0x00b99196` is a link race (README, "Read this first"): a plain status record
lands on an announcement the firmware is mid-transaction on, halfword 30 reads
0 where it validated 112, and the checksum loop walks off the CPLB map. On the
wall-clock time base it hit four of six 90 s boots with the simulator's
defaults and none of three with `BFIN_LINK_ANNOUNCE_STICKY=1`, which the
launchers therefore set; pass `--gui-env BFIN_LINK_ANNOUNCE_STICKY=` to
`boot_vm` to switch it off for an A/B. The earlier note that the switch made no
difference was measured on the instruction-counted time base, where the race
was much less likely to be hit at all. The simulator writes the fault line to
its log:

```sh
tail "$TEMP/vm-ui-sim.log"      # or vm-gui.log for a headless run
```

A frozen panel with no fault line in the log is a different thing: that is the
firmware simply not having drawn anything new, which is normal for long
stretches.

Closing the window stops both boards. QEMU is asked to quit through its monitor
rather than killed, so its log is flushed instead of truncated.

### With a card

```sh
python -m tools.cdj_main.make_sd_image <contents-dir> runs/card.img --size 128M
python -m tools.cdj_main.view_vm --sd runs/card.img
```

`make_sd_image` builds a FAT32 image with an MBR partition where the firmware
expects one. Giving `--sd` also presses the `SD` source key by default, because
nothing else selects the medium.

If you pass a card image that does not exist, `view_vm` says so and stops. That
check exists because MAIN exits before the GUI ever reaches it, and the window
would otherwise show the boot screen for ever without saying why.

### The buttons

The virtual buttons sit **beside** the picture, never on it. `BROWSE`,
`TAG LIST`, `INFO`, `MENU` across the top and `LINK`, `USB`, `SD`, `DISC` down
the left are backlit plastic on the real player and appear in no frame the
firmware draws -- so drawing them onto the panel would be inventing content.
See the README.

The window refuses to start unless it can reach every input the board decodes.
`view_ui --coverage` prints `48 of 48` and exits non-zero on a gap.

### Driving it from another shell

The window opens a control channel into the running machine, and it is not the
only thing that can talk to it:

```sh
python -m tools.cdj_main.panel_control --port 5984 press sd
python -m tools.cdj_main.panel_control --port 5984 rotary 4 +8
python -m tools.cdj_main.panel_control --port 5984 state
```

A press is a **pulse, not a state**: the firmware's handlers are rising-edge
detectors, so a bit held down for ever is the same as one never pressed. And a
press must be held long enough to land in a status record -- the plans use
2800 ms, and at the board's 300 ms default not one of 24 measured presses ever
arrived. `panel_control` refuses a press below the floor rather than sending one
that cannot be seen.

`--no-control` leaves the channel closed. That is what makes a run a control
run: nothing binds, nothing is merged into the panel payload, and the difference
between two runs is then attributable to the input rather than to the machinery.

## Headless, for a capture

```sh
python -m tools.cdj_main.boot_vm --seconds 150 --output runs/frame.png
```

Same two boards, no window, one frame at the end. It finishes by reading MAIN's
own words back through the QEMU monitor, so the report says what the machine
thought rather than what the picture suggests:

| word | address | means |
|---|---|---|
| panel state | `0x04fe29f4` | non-zero once a panel frame was accepted |
| GuiCom mode | `0x04c06fb0` | 1, 3 or 4 makes the send task serve the GUI |
| ready words | `0x0489bcf4` | both non-zero means the handshake completed |
| bAnsReceive | `0x0489b368` | toggles while answers are coming back |

Useful flags: `--frames DIR --frame-every 2` samples the screen on a fixed grid,
`--caution` decodes MAIN's error store, `--watch ADDR` reads memory as it runs,
and `--no-peer` switches off the canned MAIN answers so that what appears on
screen came from the machine rather than from a file.

## The GUI board on its own

The Blackfin board runs without MAIN if you feed it MAIN's side of the
conversation:

```sh
python -m tools.cdj_gui.main_packet packets/status.bin --mode 2 --player-mask 0xf
python -m tools.cdj_gui.view_ui --packet packets/status.bin
```

The `tools/cdj_gui/build_*` modules generate the rest of the stimulus: status
records, player state, browse lists, marker streams, waveform answers. This is
the fast path when you are working on the display and do not care what MAIN
thinks.

## Did that input change anything?

```sh
# 1. a control run of the same length with nothing pressed, into runs/control/
# 2. the mask of what moves by itself
python -m tools.cdj_main.frame_delta mask runs/control runs/anim-mask.bin --from 60
# 3. score the windows of the real run: <frames-dir> then SECONDS:NAME per input
python -m tools.cdj_main.frame_delta windows runs/frames 150:18.1 175:18.2     --mask runs/anim-mask.bin
```

A single pair, without any of that:

```sh
python -m tools.cdj_main.frame_delta pair runs/before.ppm runs/after.ppm
```

Comparing two frames straight is not enough: parts of the screen animate on
their own, and "the frame changed" is then not evidence of anything. The mask
comes from a control run of the same length with the same switches, and it says
which pixels move by themselves.

A mask from a different world erases exactly the fields that carry the evidence,
so a mask is only usable against the run it was made for.

## Diagnostics

### NXS panel CPU error caused by DMA address modes

The NXS `E-7022: PANEL CPU ERROR` reproduced on 2026-09-09 was a MAIN
memory-initialization bug, not a missing panel identity byte. Startup uses
DMA with a fixed source word and an incrementing destination (`CHCR=0x4431`)
to clear RAM. The former generic DMA path incremented both addresses, reading
past the zero word and filling parts of RAM with unrelated data.

The UDP port table at `0x04687508` consequently contained `0xffff` ports.
Pro DJ Link endpoint creation returned `-41`; its receiver task then spun on
`-18` receive errors. That runnable priority-4 task starved the equally
prioritized panel receive task even though the panel interrupt had copied a
valid frame and woken it.

After respecting DMA source/destination modes, a stock NXS MAIN/GUI boot
with the unchanged zero panel payload produced a valid endpoint (`31` at
`0x04d10874`), zero UDP receive errors (`0x04d10e60`), and initialized panel
state (`1` at `0x051e2184`). The display no longer showed E-7022. That probe
still showed E-8709, but its serial wiring was incomplete: it exposed only
the request socket and sent the status serial channel to `null`. This was a
probe error, not evidence of a remaining communication failure in `nxs_vm`.
Another boot with no `CDJ_PANEL_FRAME` override confirmed the same state.
A live `analog 2 4660` command then appeared as `12 34` in both the received
frame (`0x04d1209c + 4`) and the validated payload (`0x051e218c + 4`).

Rebuild QEMU after updating the board source. The firmware-free regression
tests exercise the real board registers with the CPU stopped:

```sh
sh scripts/build-qemu-sh4.sh build/qemu
python -m pytest -q tests/test_main_dmac.py
```

The tests cover fixed, incrementing and decrementing addresses, 4- and
16-byte transfers, final register values, and copies crossing the DMA chunk
boundary. Set `CDJ_QEMU` if the binary lives outside `build/qemu/build/`.

### NXS E-8709 versus E-7010

The GUI's `BFIN_MAIN_LINK=host:port` bridge opens **two** TCP connections:
the request channel at `port` and the status channel at `port + 2`. It closes
both if either connection fails. QEMU therefore needs both `-serial`
backends; a listening request port alone is insufficient. `nxs_vm` already
sets these correctly. With its default port, they are 5980 and 5982;
5984 is the separate host panel-control port.

Verification on 2026-09-09 used the normal NXS launcher for 45 seconds,
without a proxy, firmware patches, or functional DSP overrides. The GUI
received 772,448 link bytes, displayed `Not Loaded.`, and reported
`E-7010: DSP DEVICE ERROR`, not E-8709. MAIN was running and communicating.

The tested QEMU binary's DSP interpreter stopped at PC `0xc004f306`, compact
instruction `0x2627`, with `parallel register write conflict`. Its captured
checkpoint was number 65, after 25,364,865 DSP packets. This identifies the
next execution blocker, not its architectural cause: decoding, packet
grouping, and delayed-result/loop timing still need to be distinguished.
The working tree's in-progress C674x changes were not rebuilt or altered by
this communication diagnosis, so this result describes the tested binary.

The later exploratory run `runs/nxs-interactive-exploratory-1` is not proof of
responsive controls: its GUI link byte count stopped at 9,032 by wall time
30 seconds and stayed there through 290 seconds. MAIN's input log nevertheless
records applied key down/up transitions from 139 seconds onward. Repeated UI
clicks also accumulated serialized 3.3-second pulses. Direct mouse contacts
fix that host gesture/queue problem, but do not establish that the firmware's
status delivery or display response is fixed. Native Tk gesture tests and the
compiled C input harness verify delivery to panel payloads, not firmware UI
response. The run ended because its viewer was closed.

A strict panel-delivery recheck on 2026-09-09 used the SPKERNEL-fixed binary
without functional DSP switches, with `CDJ_REQ_STATUS_FRESH=0` and
`CDJ_LINK_LINK_ROWS=off` to preserve request and answer bytes. A ten-second
`down 20 08` / `up 20 08` MENU contact opened the firmware's UTILITY screen.
Control snapshots showed frame counts 7,721, 9,986 and 12,258, the expected
held bit followed by released bits, and an empty pulse queue. GUI link bytes
grew from 492,432 at 40 seconds to 833,528 at 60 seconds. This establishes a
genuine panel-to-MAIN-to-GUI response, not complete boot: the visible E-7206
auth-chip banner and strict SPI1 DSP stop remain. Local temporary evidence is
in `/tmp/cdj-panel-delivery-strict-1`; it is not a committed test fixture.

The follow-up `/tmp/cdj-panel-delivery-visible-1` exercised the actual native
Tk deck against the rebuilt strict timed-SPI binary. A generated mouse-down
on MENU sent `down 20 08`; releasing outside the button sent `up 20 08`.
The control channel reported MENU held at frame 182, all bits released at
frame 221, and queue zero throughout. The captured firmware picture
`viewer-hold.png` visibly shows UTILITY, still with E-7206. DSP execution now
ended each phase by its cooperative budget rather than the earlier SPI fault.
This is visible-viewer interaction evidence, not an error-free boot gate or
evidence that USB/SD track loading is ready.

A later reproducible communication failure was traced to the GUI simulator's
SIC interrupt-mask write ordering, corrected by patch 07 (`69d0d88`). Rebuild
`bin/cdj-run` with `sh scripts/build-bfin-sim.sh build/gdb-17.2` to include it.
The fix prevents a duplicate interrupt from cancelling a newly armed payload
receive. A normal cached-transport boot with detailed tracing disabled now
passes a native Tk MENU hold/outside release, opens UTILITY, and retains free
MAIN message pools after 80 seconds. The focused native/input/transport suite
passes 128 tests.
Do not enable fresh-only delivery as a workaround: it remains diagnostic-only.
E-7206 auth-chip emulation is still unresolved; USB/SD loading and audio playback
are not yet validated.

### Existing tracing tools

```sh
python -m tools.cdj_main.monitor "1,2,GU"      # MAIN's own service monitor
python -m tools.cdj_main.caution --live        # decode the caution store
python -m tools.cdj_main.gui_handshake         # measure the link handshake
python -m tools.cdj_gui.decode_link_dump run/main-link-dump.bin   # what MAIN's link handed the GUI
python -m tools.cdj_main.link_exchanges run/vm-main.log --dump run/main-link-dump.bin   # every request against MAIN's answers
```

`decode_link_dump` reads a `BFIN_MAIN_LINK_DUMP` (every record the simulator
handed the GUI firmware) and prints the status-word changes, every payload with
its list rows or player-state strings, and which announced payload lengths
were never delivered -- the shape of a frame the link lost.

`link_exchanges` reads MAIN's own log (`boot_vm --main-output`), pairs every
request the GUI sent (a delivered frame with bit 15 of word 1 set) with the
payloads MAIN sent before the next request, and prints a table per request
class -- type, cursor, words 3..5 -- of how often it was asked, how often a
payload answered it, which (length and command word), how fast, and how many
times the GUI asked again meanwhile; then the classes MAIN never answered
with a payload, and the frames delivered less than 0.5 ms after the one
before, which is the shape of a request the firmware read twice or not at
all. With `--dump` one payload of every answer signature is decoded from the
GUI's dump. On `trackload-45-final` it says, for instance, that the NXS
GUI's type-9 player-state request is answered by MAIN 4.33 with 224 bytes
under command `0x0009` (not the `0x19` layout the GUI tools build), that the
type-7 encoder-LED commands never get a payload, and that 93 of 8263 frames
went in back to back (see `CDJ_LINK_RX_GAP_US`).

`caution` turns MAIN's internal codes into the `E-nnnn` numbers the player would
show: `E-7010` is the audio DSP, `E-7020` the USB device, `E-7001` the disc
drive. One caution per priority survives, so killing the loudest one usually
reveals another that was pending all along.

## Speed

The GUI board's simulator interprets Blackfin instructions, and it used to
count guest time in instructions: every firmware delay, time-out and animation
stretched by however slow the interpreter was -- a factor of eleven, and a
five-minute boot. It now runs on the **wall clock** (`BFIN_TIME_BASE=wall`, the
default): guest ticks are delivered at `BFIN_CCLK_HZ` per second (400 MHz, the
clock the firmware programs its core timer for), and when the firmware parks
its main loop or executes `IDLE` the simulator sleeps until the next event or
the next record from MAIN. Guest time never runs ahead of the wall clock, so
this board and the QEMU board, which was always on the wall clock, see the same
time. Bursts the interpreter cannot keep up with make guest time fall behind,
up to `BFIN_WALL_LAG_MS` (50); the excess is dropped and reported.

The display DMA is paced per frame at `BFIN_PPI_FPS` (60) instead of one
scanline per simulated cycle, and a receive with nothing to hand over retries
after `BFIN_SPORT_RETRY_US` (1000) of guest time. `--ppi-delay` on the
launchers still forces a per-line tick count for a run that needs the old
pacing.

Give the simulator `BFIN_STATS=<seconds>` and it prints a line per interval to
its log: instructions and MIPS, guest ticks, guest seconds against wall
seconds, time spent parked, dropped lag, frames scanned and published, bytes
from the link. `boot_vm --poll-every 5` prints MAIN's side on the same grid --
the handshake words, the RTOS tick counter and its rate, the CPU time of both
emulators -- and `--poll-output` keeps it as TSV.

A sampling profiler is built into the simulator for Windows, because gprof
on MinGW produces call counts but no samples: `BFIN_SAMPLE_PROFILE=<file>`
records the run thread's instruction pointer about a thousand times a second
(from `BFIN_SAMPLE_PROFILE_AT=<seconds>` on, if given), and `addr2line -f -e
bin/cdj-run.exe` resolves the addresses after rebasing them to `0x140000000`;
the first eight bytes of the file are the module base. This is what showed
that the GUI board sleeps through most of a boot and that the display
conversion, not the interpreter, was the largest consumer of the rest.

Two switches exist for reproducing a run rather than timing it:
`BFIN_TIME_BASE=insn` is the old instruction-counted time, and
`BFIN_EXIT_AFTER_TICKS=<n>` halts at a guest time, so two builds run to the
same tick count from the same packet file must produce the same picture and
the same instruction count. `BFIN_EXIT_AFTER_WALL=<seconds>` halts after a
wall time, which is what a `gprof` build needs to write its data.

Measured on an i7-13700H, GUI board alone (`run_headless --packet
packets/status.bin --seconds 60`):

| simulator | MIPS | guest ticks/s | wall clock to 1e9 ticks |
|---|---|---|---|
| as shipped before September 2026 | 8.4 | 41 M | 32.7 s |
| probes gated, page cache | 18.7 | 91 M | 10.9 s |
| + CPLB memo, no `getenv` on hot paths | 31.7 | 150 M | 6.6 s |
| + wall-clock time base | guest = wall, 400 M ticks/s | | |
| + display converted only on change | 2e9 ticks in 7.1 s (was 15.0 s), instruction-counted | | |
| + PLL lock event, MMU check without a call | 40 MIPS busy (was 28) on the same firmware timeline; 2.7 s off the top of every boot | | |
| + no `sprintf` per instruction | 45.7 ns per instruction busy (was 48), wall-clock run | | |

Two of those came out of the profiler after the time base was in. The first
`IDLE` of a boot -- the PLL programming sequence, `SIC_IWR`, `PLL_CTL`,
`PLL_DIV`, `IDLE` -- slept 2.68 s, because the PLL model had no lock event
and the only event pending that early was the simulator's own poll event; the
PLL now locks after `PLL_LOCKCNT` system clocks, and an `IDLE` whose wake-up
has already happened returns at once, as the chip does when `SIC_IWR` already
shows the source. The MMU check's memo hit still made two calls per access; a
window per table now answers an aligned, granted access inline. A deliberate
wait is no longer charged as lag either: an event further away than the lag
cap fires once, at its time, instead of being chased in 50 ms slices. The
full boot now shows the player screen at 28-34 s (was 34-36). The LZSS
accelerator was suspected for the same seconds and is innocent: with
`BFIN_FAST_LZSS_TRACE=1` all ten resource banks go through it natively
inside the first second of a boot, and the guest's own byte loop never
runs. The last per-instruction cost the profiler found was the
instruction text itself: every decoder formatted its immediate operands
with `sprintf` for a trace line nothing printed; that is gated now.

**Switching to a medium.** With a card image (`--sd card.img`, a rekordbox
export on it) the launchers put the card in at 10 s and press its key at
12 s, before the GUI's first browse, and the card's library -- the `SD`
header, the six categories, the card's folders and playlists -- is on the
screen together with the player screen, at 31-35 s, seven runs of seven.

Pressing the card's key on a running machine is a different path, and it
had two blockers. The first was ours: the board used to report the card
on the wrong media-state word. Status halfword 26 carries a 3-bit state
per source in MAIN's own order -- bits [11:9] LINK, [8:6] USB, [5:3] SD,
[2:0] DISC, built from the words at `0x0489bd68/6c/70/74` in that order --
and the GUI's key dispatcher (`0xb9b98c`) routes a SOURCE key on the state
of the source MAIN reports as current. The board held `0x0489bd6c` at 1,
which is the stick's word, so the SD key always found 0 and went to the
`Wait` platter (measured with the GUI's table watched: w4). Worse, holding
any of those words is harmful: with it MAIN answered the card's list
request with one-row records for 20 s (e2), without it the lists came at
once (e5, m3). The report is now opt-in (`CDJ_SD_MEDIA_STATE=1`,
`CDJ_SD_MOUNT_S`) and points at the SD word.

The second is the GUI's browse loop for the boot source, and it is a
command nibble. After the player screen the GUI browses the LINK source
(type 1, cursor 3, KIND 0) 30-40 times a second; MAIN, with no network
behind LINK, answers each poll with the one-row list `NO DISC` stamped as
command `0x11`, the answer to a cursor-*1* request. The GUI's consumer
(`0xb7eb48`) compares the command's low nibble with the cursor of the
request it has outstanding, flags the mismatch and re-sends -- for ever,
consuming every answer on the way (`BFIN_RECORD_TRACE`: 88 in 30 s). The
loop ended on its own only when the GUI was slowed down (three machines on
the host), and a link dump of such a run (e4) shows why: under load MAIN's
answers came stamped `0x13`. So the board now stamps the nibble of the
request being answered into MAIN's answer on the way out and re-stamps the
checksum (`CDJ_LINK_LINK_ROWS=match`, the default; `off` sends MAIN's
bytes untouched, `drop`/`empty` were tried and change nothing): the loop
is over by 40 s in six runs of six.

With both in place the SD key on a running machine brought the card's
library in 1.5-5.6 s in 3 runs of 9. In the others the key reached MAIN
(`0x04c084d8` goes to 1) but the GUI never saw halfword 18 change, because
MAIN's status records stopped flowing: 1-11 per 5 s against 200-400 in the
runs that worked. The GUI polls status in two shapes -- `0001 0000 ...`
when it opens a query and `0000 8000 ...` on every repeat -- and MAIN
answers the first at once and the second on its 3 s timer; once one answer
is late the GUI repeats and the rate collapses. `CDJ_REQ_STATUS_FRESH`
(on by default now) rewrites the repeat into the fresh shape, both words
(clearing bit 15 alone leaves an all-zero header, which MAIN answers by
re-sending its last browse answer). With it the key brings the library in
0.6-2.6 s in 6 runs of 8 (n1-n3, t1-t2, p1-p3). The two failures are the
open end: MAIN answering `0000 0000` polls -- the GUI fetching a payload
MAIN's record still announces -- with its last 48-byte browse answer, and
the card's list request with that same stale answer (n1 at 55.66 s).
Give a freshly inserted card ~25 s before pressing its key: MAIN's scan of
the card takes that long at 40 MIPS, and a key before it is done gets
one-row answers the GUI gives up on after ~5 s (m1).

Switching **away** from the card works: the USB key with no stick shows
the platter 1.5-3.5 s after the key, three of three.

**Loading a track.** The track list of a playlist is one 896-byte link
frame, and until 2026-09-03 both link models threw anything over 512 bytes
away -- the board without reporting the completion, which left MAIN's sender
dead for the rest of the run (`runs/nxs-swap/twoboard-10-load`: the answer
announced in 9 545 status records, never delivered). Both carry 4096 bytes
now, the protocol's own limit. What the browse keys of the NXS GUI do on this
MAIN is measured in `runs/nxs-swap/NOTE-trackload-2026-09-03.md`: the select
encoder (`rotary 7`) moves the left column, the pane follows, and no key was
found that sends the "enter" request. So the requests are injected instead:
`tools/cdj_main/link_inject.py` sits between the GUI (`BFIN_MAIN_LINK=
127.0.0.1:5990`) and MAIN and sends, at given seconds after the GUI connected,
an ENTER (`type 1 cursor 3, words 7 1 N` -- row N of MAIN's current list
becomes the list) or a LOAD (`type 7 cursor 1, words 0 N` -- the track at
index N of the current list). From the library screen, `--inject 90:1:3:7:1:0
--inject 110:1:3:7:1:0 --inject 130:1:3:7:1:0 --inject 155:7:1:0:0` opens
PLAYLIST, its first folder, that folder's first playlist -- the track list
draws -- and loads its first track: TRACK 01, overview waveform, and after a
second load the duration, BPM and key of the track (`trackload-12-load`,
frames t165-t250). MAIN's own console (`CDJ_DEBUG_CONSOLE=70`, the file
`TEMP/vm-console.txt`, Shift-JIS) narrates it: `BlackFin → ロード要求`,
`♪WAVE[0,400]`, `♪CUE(U/S)[3]`; arm it after the library is up, at 5 s it
broke the card switch.

The proxy can also edit what MAIN sends. The NXS MAIN fills status record
words 1 and 2 with the beat display's bitfields, MAIN 4.33 sends them as 0
(its builder starts at word 3 -- Ghidra on both, and on the NXS GUI, whose
decoder 0x00d0e346 splits them into the beat in the bar (bits 14..12 and
10..8), a source mode (bits 7..4), a state (3..0) and two 9-bit bars.beat
counters, 0x1ff = blank, that the screen-0 orchestrator 0x00d2d80c hands
the widgets 0x26, 0x40, 0xc1/0x83 and 0x105/0x10b/0x111). `--nxs-prefix
beat[:MODE[:STATE[:C1[:C2]]]][@SECONDS]` writes them into every record,
the beat computed from the record's own time and BPM, checksum redone;
`--status-word IDX=VALUE[/MASK][@SECONDS]` patches any other halfword. Both
go through `twoboard --proxy-arg=...`. trackload-89/90: the words reach the
GUI (the dump shows them) and nothing on the screen changes, in any of five
variants of mode, state and counters. trackload-91 probed three other record words as the layout switch: word 18 bits 5..3 = 4 changed nothing, word 19 bit 14 blanked the overview waveform, word 13 = 2 (the decoder's link-player path) froze the time display and corrupted the frame -- none opened the beat layout. The widgets those
setters address are not in the screen the GUI shows after a load (the
GUI's own screen registry calls it screen 0, `performance`: list on top,
deck strip below).
The simulator's call watch (`BFIN_CALL_WATCH=<pc>,...`, trackload-92/93)
showed the orchestrator 0x00d2d80c never running there: its event 0x10016
reaches screen 0's own dispatcher 0x00d3a97c, and the beat setters belong
to screen 5 (`browser` in the registry, 306 widgets, dispatcher
0x00d45456). The switch is the panel's BROWSE key (20.0, MAIN's own name
table): trackload-94 pressed it at 200 s and the GUI went to the full
performance screen -- title bar, detail waveform with ZOOM/GRID, and the
two-row MASTER/PLAYER phase meter with the beat countdowns, which drew the
proxy's fields: beat lit from the record's time and BPM, `08.1 Bars` and
`04.2 Bars` from the counters 0x21 and 0x12. So with

```
python -m tools.cdj_main.twoboard NAME --card CARD --keys keys.txt \
    --env CDJ_DSP_ACK=1 --env CDJ_DSP_POSITION=1 \
    --proxy-arg=--nxs-prefix=beat:1:0:0x21:0x12
```

and a keys file with `200 press 20.0` (BROWSE) before `230 press 16.0`
(PLAY), MAIN 4.33 plus the proxy drive every visible element of the NXS
phase meter.  There is no finer phase than the beat in that record: the
meter's lit segment is the beat in the bar (word 1 bits 14..12), the
countdowns are the two bars.beat counters, and the GUI has no field for the
position within the beat -- when the proxy also sends a beat grid
(`--nxs-markers beat:BPM`), it aligns the beat it announces to that grid.

The beat grid and the cue markers on the waveform are two more payloads the
NXS MAIN sends and MAIN 4.33 does not: command 0x20, the detail waveform
(PWV3 entries of the track's ANLZ file, one byte a column at 150 a second),
and command 0x21, four-byte marker records (type, then the time in
milliseconds as bytes 1, 3, 2), both in 448-halfword parts with the part
number in words 1/2 and the total in words 3/4 (NXS producers 0xa425bd60
and 0xa425c3b8; the GUI's first-frame decoder 0x00d0fa64 and wire handler
0x00d0fe00 stage them, the completion dispatch 0x00d0f1a8 hands them to
the consumers 0x00d2f51c and 0x00d2c118).  The GUI asks for them with
request types 0x20 and 0x21 when the status record's word 18 carries bit
11 (detail waveform) or bit 15 (markers) -- MAIN 4.33 reads a type-0x20
request as a cancel ("ｷｬﾝｾﾙできない") and answers with its last list.
`--nxs-waveform FILE` and `--nxs-markers beat:BPM[:OFFSET[:TYPE[:BAR]]],
cue:MS:TYPE,...` make the proxy answer those requests: the announcing
status record and the part go out together, MAIN's own announcements are
hidden while a transfer runs, every second typed request goes to MAIN to
pace the GUI, and MAIN's answers to those pass through (held back and
dropped, the GUI never completes the command, trackload-119; released in
a burst afterwards, the time display froze, trackload-106).  The
simulator's per-length frame slot must be deep enough to hold the parts
(`--gui-env BFIN_LINK_DEPTH=64`; with the default depth of 1 the firmware
saw every third part, trackload-103/105).  Drawing the detail waveform is
gated on the record as well (0x00d2f418): word 18 bits 5..3, the source,
must be neither 0 nor 4, and MAIN 4.33 sends 0 -- `--status-word
18=0x08/0x38` supplies a 1.
trackload-120/121: with the answers passing, the GUI takes all 34
detail-waveform parts and both marker parts and runs the consumers
0x00d2f51c (0x73c3 bytes) and 0x00d2c118 (0x674 bytes = 413 records)
once each -- and still drew nothing, because the draw gate 0x00d2f418
(called from the GUI task's loop 0x00cfd378) waits for a readiness word
(0x00cd3694) that the widget handler 0x00d2e8e8 sets on the
"detail waveform complete" message only if the first part's words 5/6
repeat the track length of the status record's words 7/8 (minutes,
seconds, frames within 2).  The proxy now copies those words in by
default, and trackload-122 draws it all: the detail waveform with the
beat grid's ticks scrolling under the play head, the cue point's marker
on it, the memory-cue triangles on the overview (`result-overview.png`).

The DSP model's transport, from MAIN's writes into the window
(`CDJ_DSP_TRACE`, trackload-96..118) and the DSP task that makes them
(0x19fca2, Ghidra; it resolves the window as `0xac0c....`): the tempo
slider is panel fields 2/3 and lands in +0x7bc0 as the playback rate,
fixed point with 2^20 = 1.0 (0x0010020c = +0.05 %); +0x7ba0 is a state
request (3 at PLAY, 2 at a pause, 4 for cue standby after a cue return and
at the load's end, 5 at an unload; the task follows the DSP's answer in
+0x7bf8); +0x7c80 = 0x11 / 0x21 is a locate to the position in +0x7c84
(stand there / run from there -- a CUE while playing sends 0x11, state 4,
0x21; the PLAY after it state 2 and 0x21); +0x7c9c = slot * 16 + command
with parameters in +0x7ca0..+0x7cac is the segment-slot interface: 0xc
flushes the slot (before a cue return and before IN), 1 is loop IN and 2
loop OUT with the point in +0x7ca8 as half frames (150 a second, the
position reader's unit) plus +0x7ca4 in samples -- MAIN quantises them to
the beat (7.805 s and 11.708 s = 16 and 24 beats at 123 BPM in
trackload-120/121, identical in both runs) and the DSP plays the segment;
+0x7bc4 is the flag word the task
rebuilds every pass from the deck's flag bytes (bit 31 = byte 0x690 in
state 4), not a transport command.  With `CDJ_DSP_POSITION=1` the model
follows the rate, the state requests, the locates and loops between the
two points.
trackload-120/121 (PLAY 230, pause 240, PLAY 247, CUE 255, PLAY 260, IN 268, OUT 272): the position stands at 9.6 s through the pause, resumes, returns to 0 and waits in standby, runs from the PLAY, and after IN/OUT loops between 7.805 s and 11.708 s; the NXS GUI's REMAIN follows all of it (result-overview.png).  Still open: the jog (panel field 6 plus the JOG TOUCH key 15.5
changed neither the rate nor any command in trackload-96/98/102 -- the
platter's pulses are not what that field carries), RELOOP/EXIT (not
pressed yet), and the flag bytes behind +0x7bc4.

The DSP itself is not a Pioneer custom part: `D710E001BZDHA275` is a TI
Aureus DA710 with a TMS320C67x+ core (UHPI host port, McASP audio, EMIF
SDRAM, a 768 KB internal ROM whose content is not public).  The program
MAIN downloads at boot -- `main-unpacked.bin` 0x1010 (54192 bytes) and
0xe3d0 (nine 32 KB pages and one of 24384) -- disassembles cleanly as
little-endian C67x+ at load address 0x10000000 with the pip package
`tms320c6x-disassembler`; the bytes and the listing are in
`runs/nxs-swap/dsp/`.  A full virtualisation would need a C67x+
interpreter plus the DA710 peripherals and the ROM; the listing's value
now is the host-window protocol it implements (its header dispatcher at
0x1004e1d8 tests the 0x03000100.. codes MAIN writes into +0x8140).

Two tool limits met on the way: gdb write watchpoints on the DSP window
(`--trace w:0xac0c7c9c:4`, the uncached alias MAIN uses -- the physical
0x0c0c.... never fires) slow MAIN to a fifth once the streaming loop
touches the window (trackload-118), and `BFIN_PEEK_WATCH` on a byte
address double-faults the NXS GUI at boot (trackload-115/116).

The 2000 MAIN's patch list for the beat part is the NXS MAIN's set of
status-source producers (the functions that feed its status builder
0xa425ca0c); the proxy is the stand-in until it exists.

The load itself is a handshake with the audio DSP, and the built-in DSP
model answers it only with `CDJ_DSP_ACK=1` (off by default; see the comments
in `emulator/qemu/cdj2000_dsp_model.c` for every word and the run that
measured it). With it MAIN writes the load's parameters, the stream format
and header into the DSP window, reads the file from the card and streams it
into the DSP over DMAC channel 5 -- the whole track, the way the player loads
into the DSP's 32 MB of SDRAM -- and reports the load complete when the last
buffer is in: `Musicﾛｰﾄﾞ要求完了通知受理 PL→全`, NOW LOADING ends, the
tempo field shows the track's BPM (`trackload-36-levels`, ~130 s of guest
time for a 3:17 WAV). Without the switch the load stays pending behind NOW
LOADING, or stops with `E-8302` once the model answers some words but not
others. `CDJ_DSP_TRACE=1` prints every acknowledged request with its
parameters, a per-second census of the control block's changed words and the
two buffer levels. The recipe that held (trackload-49..52, four of four): the
card given at launch (`--sd`), the SD SOURCE key at 60 s (`--source-key sd
--source-key-at 60` -- the card alone leaves the NXS GUI browsing LINK, and
its cursor-3 polls then collide with the injected ENTERs, trackload-47/48),
ENTERs at 90/110/130 and the LOAD at 155 s of the injector's clock; the
receive gap (`CDJ_LINK_RX_GAP_US`) is what made it hold. That recipe is a
tool:

```
python -m tools.cdj_main.twoboard trackload-60 --card runs/nxs-swap/rbstick1g.img
python -m tools.cdj_main.twoboard play-1 --card CARD --keys keys.txt --env CDJ_DSP_ACK=1 \
    --bootvm-arg=--trace=0x41bd304 --dry-run
```

`twoboard` starts MAIN (`boot_vm`, the card's work copy at launch, the SD key
at 60 s), the injecting proxy (`link_inject`, the four requests above unless
`--inject` or `--no-inject` says otherwise) and the GUI (`run_headless`),
presses panel keys from a `SECONDS ARGS` file through `panel_control`, and
leaves logs, frames, request dump, MAIN console and a README with every
command line in a fresh directory under `runs/nxs-swap/` -- an existing one is
refused, so runs never overwrite each other. `--env` is MAIN board environment
(`CDJ_*`), `--gui-env` the simulator's, `--bootvm-arg` anything else for
`boot_vm`; the gdb stub stays free for `--trace` because nothing is polled
unless `--poll-words` asks. Still blank: the time fields of the status record (words
5..8 read `0xbbbb` while a track is loaded, 0 once it is unloaded). The slot
table at window+0x7ce0 can now carry a state and an advancing position
(`CDJ_DSP_SLOT_REPORT`, trackload-59), but MAIN's player task reads it only
on its own command path (the copy at 0x1b39cc), which a PLAY in the
emulator does not take. Where the time really comes from was traced with
write watchpoints (`boot_vm --trace w:ADDR[:LEN]`, trackload-61..66): the
status record's word 5 is written at 0x216a24 -- 0xbbbb when 0x2cd378()
returns -1 -- and 0x2cd378 reads the deck's position word 0x4832214, which
only the stream worker's position function (0x1a8e60.., writers 0x1a8f00 and
0x1a979c, reading the DSP's per-buffer status at +0x81a0 and the fill
levels) ever sets. That function never runs in the emulator: the worker
stays in its load state waiting for the DSP, and neither consumption alone
(`CDJ_DSP_CONSUME`, trackload-67) nor a class-0 event (trackload-69) wakes
it into playback. The chain is known to its root: the position word is
written by the decoder task's state-3/4 handler, which only the stream
worker's state-1 handler commands, which only two player paths request --
the second load variant (taken when the deck's word X+416 is 1 at load
time) and the play handler when X+408 is 6. The emulator's load leaves
X+416 at 0 and PLAY sets X+408 to 4, so neither path runs (trackload-74..76;
X+416 = 1 turns out to be needle search, trackload-78).

That chain was the wrong one. Ghidra on the reader that writes the deck's
time words (trackload-84..88) says: X+620 (`0x4832214`) is the track LENGTH
in CD frames (-1 = unknown, which blanks the time display), and the elapsed
position is read from the DSP by 0x19e568, a function the tick 0x286248
calls every 10 ms: it takes the report block at window +0x7bf0..+0x7c60 --
+0x7bf0 status (0 = valid), +0x7bf4 low 16 bits = sample offset inside the
current frame (0..587, /294 = half frame), +0x7c10 = position in CD frames
(75 a second), +0x7c14 = the id of the load-queue record being played
(MAIN looks it up in the ring at 0x4836908 and takes the track length from
it) -- and writes X+0x224/0x226/0x228 (minutes, seconds, frames) and
X+0x218 (frames * 2 + half). The status record carries those as the time;
0x2cd378 only supplies the length for REMAIN and the end warning. So the
poke of trackload-83 drove the length, and the GUI's REMAIN display showed
length minus zero. The DSP model keeps that block with

```
CDJ_DSP_POSITION=1
```

taking the record id from the PCM-channel command +0x8100 = 2 (+0x8120),
starting the position at 0 with the load's closing +0x7ba0 = 4 and running
it from +0x7ba0 = 3 (PLAY) in real time. trackload-88, no poke: the deck's
length word becomes the record's 0x39e2 (3:17) at the load, the time words
count from PLAY, the GUI's REMAIN display TRACK 01 bleibt, X+620 = 0x39e2 (3:17) ab 170 s aus Datensatz 1, X+0x224 zählt ab PLAY; REMAIN 03:17 bei 230 s, 03:08 bei 240, 02:43 bei 265, 02:14 bei 295 (Echtzeit), Cursor wandert, kein Fehler (`result-overview.png`) and the waveform
cursor moves. Pitch, jog, cue and loop are not in the model's position yet
(it only runs, at nominal speed), and beat grid and phase meter are the next
things to trace from the GUI side. There is no audio path.

**Both boards in one guest time (co-simulation).** The GUI simulator runs about
thirty million instructions a second where the BF531 runs four hundred, and on the
wall clock that speed was part of the protocol: MAIN allows the GUI 3 ms per answer,
falls back to forced sends when it is late and resets it after ~7 s (E-8709). With
`--cosim` each board keeps guest time of its own -- MAIN under `-icount`, the GUI on
`BFIN_TIME_BASE=virtual` -- and `emulator/qemu/cdj2000_cosim.c` keeps them together
over one connection on PORT+5 with a 100 us link latency, so the GUI answers in its
own time as on the chip, however long the host takes. Both skip idle time; a run goes
at 0.4-0.7x real time.

```
python -m tools.cdj_main.boot_vm --cosim --sd CARD ...          # stock pair
python -m tools.cdj_main.nxs_vm RUN --cosim ...                 # NXS GUI, cdj2000-main
python -m tools.cdj_main.cosim_scenario --card CARD --playlist-row 4 --out runs/cosim/NAME [--show]
python -m tools.cdj_main.cosim_scenario --card CARD --out runs/cosim/NAME --manual --show
```

`cosim_scenario` is the track-load self-test: it boots the stock pair on its own
ports (6480.. by default; it refuses busy ones), presses its way from the SD library
to a playlist's track, loads it and presses PLAY, each step waiting for the event it
causes, and checks that MAIN's status records carry a running time (words 5..8). It
writes a table (`REPORT.md`), a frame per step and the logs. 2026-09-23, stock MAIN
4.33 + GUI 4.20, an MP3 from a rekordbox stick: library 13 s, playlist 16 s, track
list 22 s, LOAD 26 s, load closed 74 s, PLAY 76 s, REMAIN counting 04:52 -> 04:41 by
88 s of guest time (`runs/cosim/scenario-9`). `--manual --show` starts the same
machine with the deck window and presses nothing.

**Sound: the PCM tap.** `cosim_scenario --audio` records what MAIN streams into
the DSP (`CDJ_DSP_STREAM_DUMP`) and where the position model says the deck is
(`CDJ_DSP_TRANSPORT_LOG`, every 10 ms of guest time while it plays), and
`tools/cdj_main/deck_audio.py` renders the two into `deck.wav`. For an MP3 the stream
is the file's audio data itself: 8192-byte blocks in the two halves of the staging
area (+0x81e0 / +0xbea0), each followed by a 24-byte tail (`0, 1, 0, 8, 0, 0` as u32),
block n of record 1 = file offset 0x9a000 + 8192 n, the first block zeros. Joined per
record and decoded with ffmpeg, then played by the transport log -- silence while the
deck stands, jumps at CUE and loops, the tempo word as the rate. Measured 2026-09-23
(`runs/cosim/audio-2`): 20 s after PLAY, correlation 1.0000 with the original file
over 15 s, 22.7 ms in (the zero first block). This is not the DSP: no master tempo,
EQ or effects, and a WAV track's PCM stream is not handled yet.

`gui_update_check` is the gate for a GUI update file, the GUI board's counterpart of
`safety_check`: in one guest time, stock MAIN 4.33 in the update key mode installs
`--upd FILE` on the stock GUI 4.20 over the link, then the GUI running the new body,
started from the flash that install left, takes the stock `C2KGUI.UPD` back. Each time
the flash is dumped (the GUI exits by itself when MAIN's side of the link closes, which
writes `BFIN_CFI_DUMP`) and must hold the file's body at 0x10000 byte for byte with
sector 0 -- the CDJ-2000's GUI loader, in no update file -- and the top 16 KiB untouched.
2026-09-23: PASS both ways for an NXS GUI body (Ver1.20 -> Ver4.20 on the rollback,
992 records of 2048 bytes each way). Beyond it: the real sector-0 loader (not dumped)
and a power cut while programming.

**More of the player's own paths (2026-09-24).** `cosim_scenario --usb` plugs the
card image in as a USB stick instead of the SD card (USB SOURCE key at 12 s); stock
4.33 loads, plays and counts from it (`runs/cosim/usb-7`), writes included: MAIN writes
to the stick at boot and at a load, and those writes used to STALL for good. Two USB
host rules fixed that: a DMA-filled OUT buffer goes on the bus in the host's schedule
(a microframe later), not the moment the DMAC is done, so the driver's own BVAL after
the DMA meets a full buffer instead of sending a zero-length packet; and CURPIPE = 0 on
D0FIFO/D1FIFO selects no pipe, so a transfer the driver starts before it picks the
pipe waits for it. `CDJ_USBH_TRACE=scsi` prints the mass-storage commands and their
statuses only; `=scsi+write` adds the full register trace for 1500 lines after the
first WRITE(10).

`--load2-row N` adds the next track while the first plays (steps `load2`, `stream2`);
`--then KEY:SECONDS[,...]` presses more keys afterwards (NEW FIRMWARE's `nf_cosim`
syntax). The DSP model now does what the DSP program does on the mid-manager command
`+0x7cb0 = 1/2` (0x800429e0 -> 0x80034c08): the record table, both fill levels, the
position report and the status blocks cleared, `+0x7cd4 = 0xff`. Before that a second
LOAD (and a CALL to a memory cue) found the old levels still up and MAIN never sent
the new stream (`CDJ_DSP_FLUSH=0` restores the old behaviour for an A/B). Still open
after it: MAIN then re-opens stream buffer 2 every 40 ms, because the model does not
fill the status blocks +0x8180/+0x81a0 (the record byte at +0x81a4 MAIN compares).

**Jumps, cues and the next track (2026-09-24).** What MAIN 4.33 expects from the DSP
beyond a plain load, each rule taken from MAIN's or the DSP program's code and now in the
model (each can be switched off with its variable set to `0`, for an A/B):

- *Jobs* (`CDJ_DSP_JOB`, `CDJ_DSP_JOB_ADVANCE`). MAIN's seek routine 0x041b699a moves the
  deck to a target (a memory cue, the cue point) in steps: it posts a job through
  `+0x7ba4 = 1`, `+0x7ba8` = a count of half frames (1/150 s; negative = backwards),
  `+0x7bac` = a parameter, and reads `+0x7ba4` back until it is 0 (0x0419fef2). The count
  is min(3/4 of the distance, the level on that side * 2), so the steps shrink: 204, 68,
  23, 6, 1, 1 half frames on a jump to a cue at 15.26 s (`runs/cosim/cue-12`). The model
  answers 0, moves the position by the count and moves the level from one side to the
  other. Unanswered, the jump ended in E-8302 (000F) (reason -13, 0x041b6b7e).
- *Levels.* `+0x7cd0` is the audio the DSP holds ahead of the position and `+0x7ccc` the
  audio behind it, in CD frames (a backward job takes its count from `+0x7ccc`,
  0x041b6a0e). Byte 1 of a class-1 header is the stream buffer (1 = ahead, 2 = behind;
  byte 2 is open = 1 / data = 2); the model used byte 2, which booked all of the data
  behind the position.
- *The flush* (`CDJ_DSP_FLUSH`): `+0x7cb0 = 1/2` clears the DSP's record table, both levels,
  the position report and the status blocks, and reports `+0x7cd4 = 0xff`.
- *Seek fields:* a class-1 header of buffer 1 with `+0x8154/+0x8158/+0x815c` starts the
  record at frame `+0x815c` plus `+0x8158` samples (1152-sample MPEG frames). It places the
  deck only for the stream that named the record, before any job or state request: after a
  jump has landed MAIN re-opens buffer 1 from another frame, and the deck stays on the cue.
- *Cue and hot cue slots:* in `+0x7c80`, `+0x7c84` is a slot number, not a position (plus 5
  for 0x12/0x22). On stock 4.33, slot 0 is the cue point, 1 is hot cue A and 2 is B.
  `0x11/0x12` *record* a slot at the running position if it is not held yet (0x80043ea8)
  and leave the play state alone: REC MODE + A while playing keeps playing.
  `0x21/0x22` *jump* to a held slot and play, unless the deck is in cue standby
  (0x80043f20). A hot cue called from the card (CALL held, then B) arrives as
  `+0x7c9c = 0x31` command 1 with B's point, then `0x21` slot 2; the model records that
  point, so B can be jumped to again later. `+0x7cb0 = 1` (a new track) empties the slots
  and `+0x7cb0 = 2` keeps them (0x800429e0). Slot 0 keeps its own bookkeeping: the
  track's start after a load, and where a jump landed.
- *Event 5* (`CDJ_DSP_EVENTS`): posted after a record joins the DSP's table (0x8003079c)
  and after each job, as the DSP does when its levels move.
- *Status-block record bytes* (`CDJ_DSP_STATUS_RECORD`): the record of a stream open goes
  into the low byte of `+0x81a4` (buffer 1) or `+0x8184` (buffer 2), which MAIN's stream
  worker compares before and after an open (0x041ad1f2, 0x041ad374).

With these, stock 4.33 on aconcert-1g does CALL > to a memory cue, PLAY from it, CUE back to
it and PLAY again with no error (`runs/cosim/cue-13`), and a second LOAD while playing shows
the new track with its overview and BPM and runs its time (`runs/cosim/load2-8`). On a
track with hot cues (`runs/cosim/hc-2`): REC MODE + A records A at 9.82 s and the deck
plays on, A jumps back there and plays, CALL + B streams from B and parks on it (14.788 s,
B = 14.789 s), PLAY plays from B, and B again jumps to it. Open: the second LOAD streams
nothing new into buffer 1 when the next track was not preloaded; after a hot cue jump
MAIN's next PLAY sends `+0x7ba0 = 3`, as if the deck were paused (one PLAY press does
nothing visible, the second pauses), which belongs to the still open meaning of the
`+0x7ba0` requests 2 and 3; and the red NEEDLE label blinks during playback, which a real
player does not do.

**The link stall after TAG LIST was the GUI simulator's.** A list answer of exactly
32 words (`拡張 SndSize=32W`) is a 64-byte payload, the length of a status record, and the
live link took it for one: it carried the pending announcement onto the payload's bytes
58..63, re-stamped them as a record, and kept the announcement pending, so the GUI never
finished the receive and MAIN waited 5 s on every exchange after it ("bAnsReceive ...
(Cmd=0x0)"). The receive's buffer says which it is -- a record goes to 0x00f00000, a
payload to 0x00f00040 -- and the link now asks it (patch 14, installed in `bin/cdj-run`
2026-09-24). On aconcert-1g the stall came on the first library browse; now the stock
self-test passes there and TAG LIST, BROWSE, INFO, BROWSE keep 333 exchanges a second
(`runs/cosim/b9-2`).

**A jump to a cue.** After the flush stock MAIN re-streams from the cue with the DSP's
own start fields on the first buffer-1 data header: +0x814c the byte, +0x8154 frames to
decode and discard, +0x815c the first frame heard, +0x8158 samples into it (r55, CALL >
to a memory cue: frame 532 + 1008 = 13.92 s). The model now puts the position there
(`starts record N at frame F` in its log). Buffer 2 is not the deck's: loading track 2
it gets the last second of track 1. MAIN re-opening buffer 2 every 40 ms is the open
problem (the status block +0x81a0/+0x81a4 is empty in the model).

`boot_vm --gui-flash FILE` boots the GUI from a flash image instead of an update file:
the ELF is built from the boot stream in FILE at 0x10000 -- where the GUI's updater
programs it -- and FILE is the CFI flash (`tools/cdj_gui/flash_boot.py`). So a
`BFIN_CFI_DUMP` taken after an update can be the next run's GUI. The stock and NXS dumps
of `runs/guiupd/check-1` rebuild the same ELF, byte for byte, as their update files, and
the stock one boots to the browser (`runs/cosim/guiflash-1`). The board's sector-0 loader
is still not run; nothing here has a dump of it.

Three things made that possible besides the shared time, all in the same change: the
GUI's 200-byte reads are its line drain after every record, and the zeros
`run_headless` feeds them (`BFIN_SPORT_RX_ZERO_200`) kept the line from ever going
quiet -- three seconds after every answer from MAIN, the old "3.7 s per step"
(`--cosim` removes them); the DSP model names the record being played from the first
stream open after a load (see `cdj_dsp_model_stream_open`), which is what the time
display needs for compressed tracks; and the display is placed by the DMA's frame
start (patch 14), which put the browser's title row back on screen.

**The update file is not what the emulator boots, but the emulator can take
it.** The board loads `firmware/main-firmware.bin` -- the address-zero flash
image, decoded from `C2KMAIN.UPD` by `tools.cdj_gui.main_unpack` -- into its
NOR flash model: a CFI02 device in RAM with the geometry the firmware's own
erase commands describe (63 sectors of 64 KiB and eight of 8 KiB at the top),
so the settings sectors the firmware erases and rewrites never reach the file.
A modified image is tested by putting it there. The other way in is the
player's own: a firmware update from a USB stick, and that runs end to end,
see "A firmware update" below.

## A firmware update

The type-A socket is the SH7764's own USB 2.0 host/function module at P4
0xfe400000 (hardware manual section 21; `emulator/qemu/cdj2000_usbh.c`),
not the chip at 0x01000000, which is the type-B function controller.  The
module is a QEMU host controller with one root port, so a stick is QEMU's
`usb-storage` on a raw disk image -- `boot_vm --usb-stick IMAGE` adds
`-drive if=none,id=usbstick,format=raw,file=IMAGE -device
usb-storage,drive=usbstick,removable=on` -- and the descriptors, the bulk-only
transport and the SCSI commands are QEMU's.  What the board models is the
register contract the Cente USBH driver relies on: pipes, the FIFO ports, the
DCP's setup/data/status stages, the transaction counter (the counted packet
puts a SHTNAK pipe to NAK, which is what lets the driver reload it for the
CSW), BRDY/NRDY/BEMP/BCHG/SACK/SIGN, USBI on INTEVT 0xc60 with its level from
INT2PRI12, and the DMAC side door: bulk reads of a maximum packet or more go
through DMAC channel 0 reading the D0FIFO burst port at 0xfe400180 (TCR in
32-byte units), which the board's DMAC hands to the module and completes on
DMINT0 (INTEVT 0x640).  `CDJ_USBH_TRACE=1` prints every register access,
packet and DMA; the board registers two ports because QEMU slips a full-speed
hub in front of a device plugged into a bus with a single free port, and the
driver then enumerated the hub (update-3).

The updater is the application's (`UpDtae_TASK`, state machine 0x2d68b2),
not the loader's: the boot ROM's second stage only unpacks the loader at flash
0x10000 when the packed application's checksum fails, so the loader is the
recovery path.  The task runs only in the boot mode the panel reports: with
payload byte 16 bit 2 and byte 19 bit 1 -- the RELOOP/EXIT and USB keys, by
the firmware's own service-mode name table -- down at power-on,
0x28d3cc sets GuiCom mode 2 with sub-mode 1 and the task looks for
`C2KGUI.UPD`, `C2KDRIV.UPD`, `C2KMAIN.UPD` and `C2KPANL.UPD` in the stick's
root for three seconds after its start (the mount lands at 3.5 s, in time).
`CDJ_PANEL_FRAME=00000000000000000000000000000000040000020000` is those two
keys held.  A file is taken only if its header version (bytes 0x13, 0x15,
0x16) is greater than the running 4.33 (0x2d58e4), and its CRC-16/XMODEM
trailer must match.  The MAIN file is programmed by 0x2d6116: the
application area 0x40000..0x3dffff is erased in 64 KiB sectors and the
S-records above 0x40000 are written word by word; the boot ROM and loader
are carried in the file but not touched.  The GUI, drive and panel files go
to their boards over the link and the panel UART, which needs the other
boards running.

`tools/cdj_main/make_upd.py IMAGE OUT.UPD --version 4.35` builds a file the
updater accepts from any flash image -- header, S-records (all-zero records
omitted, as the original converter did), CRC -- and rebuilds the stock
`C2KMAIN.UPD` byte for byte from the stock image.  The recipe, run
update-19: a 64 MiB FAT32 stick made with `make_sd_image` from a directory
holding the file, `boot_vm --no-gui --no-peer --seconds 900 --firmware
IMAGE --usb-stick STICK --qemu-arg=-trace --qemu-arg=pflash_* --pmemsave
0,0x400000,flash-after.bin` with the panel frame above; the -D log then
carries every erase and program, `flash-after.bin` is the flash when the
console has printed `*** Update END ! ***`, and `--firmware flash-after.bin`
boots it.  Programming runs at about 6 KB/s of guest time (every word is an
unlock sequence and a status poll), so the 2.4 MB take some seven minutes.

**The recovery path** (runs update-21..24).  When the packed application's
checksum is wrong -- `--firmware` an image with one 64 KiB sector inside it
blanked, the shape of an update that lost power -- the boot ROM's second
stage unpacks the loader at flash 0x10000 instead, and the debug console
(`CDJ_DEBUG_CONSOLE=1`, the third serial port) shows its banner, `Cente
USBH-MSC sample program` with a `login:` prompt that wants nothing.  The
loader mounts the stick with its own USB stack ("UHMS[0]: drive C:") but
runs its updater only in the same key mode as the application: with no keys
it idles for ever (its mode word 0x46a9c24 reads 1, the updater wants 2),
with `CDJ_PANEL_FRAME=...040000020000` it erases 0x40000..0x3dffff and
programs the file in under two minutes of guest time, five times faster than
the application's updater, and prints the same `*** Update END ! ***`.  The
flash afterwards is the stock image byte for byte except the marker the UPD
carried, with the ROM and loader untouched.  Both updaters read the header's
flag byte 0x1f: `'0'` programs from 0x40000, `'1'` from address 0 -- the ROM
and the loader included -- which is why `make_upd` refuses to write anything
but `'0'`.

**The GUI update over the link** (runs update-26..28).  With the GUI board
running -- the original 2000 firmware, `firmware/gui-boot-memory.elf` on
`emulator/cdj2000-gui.hw`, connected as in the two-board recipe -- and a
stick holding `C2KGUI.UPD`, the updater's index-1 path hands the file to the
GuiCom engine (0x4212924/0x42154f0: message 0x6d8 with the buffer and the
length, 0x1ec024) and MAIN streams it: status records with halfword 13 at
0x102/0x105, one 16-byte opener, then 992 records of 2048 bytes -- a 4-byte
header and 2044 bytes of file each (`decode_link_dump` shows them).  The
file must be newer than what the GUI reports (0x2d58e4 compares the four
header digits with 0x04c08614, the GUI's 4200), so the stock 4.200 file is
dropped in state 6 with "nothing to do" (update-26) and a copy with the
header bumped to 4.210 and the CRC-16/XMODEM recomputed over all but the
last four bytes, stored big-endian in the last two, goes through
(update-27): the GUI draws "GUI  Ver4.20 -> Ver4.21" with a progress bar,
runs its checksum (0xb7f348) and its commit engine (0xb799c8: erase the
part, program the image -- the routines `CDJ2000-revival/evidence/r228`
read out of the firmware), reaches 100 % at t = 170 s and shows "Firmware
update is complete. Turn the power off/on before using."; MAIN's updater
goes 9 -> 0xf0 -> 0xff and prints `*** Update END ! ***`.  The simulator's
flash is memory only; `BFIN_CFI_DUMP=<path>` (with `BFIN_EXIT_AFTER_WALL`
so the simulator exits by itself) writes it out at the end.  Update-30's
dump, 33 sector erases and 1 007 620 program commands later, is the stock
image's sector 0 untouched followed by the *whole update body* -- boot
stream and resource tail, contiguous, 0x1ec000 bytes -- at flash 0x10000,
with not one byte differing.  That is the GUI's real flash layout, and it is
not what `physical_flash_image()` assumed (stream at 0, gap behind it): the
64 KiB in front of the stream are never touched by an update and are not in
the file.  The resource tail sits at the same place either way, so the
simulator's image worked all along.  The transfer is not always delivered:
update-29's first 2048-byte record was announced and never handed to the
firmware, and the run went nowhere -- a link-model stall to keep in mind.

Two board bugs stood in the loader's way and are fixed: it ticks from TMU4
in the second timer unit (0xffdc0000), whose interrupts the board had not
routed (INT2PRI1, INTEVT 0xe00/0xe20/0xe40), and it runs the panel DMA on
channels 4/5 where the application uses 3/4, so completions now arrive on
the vector of the channel that ran them (DMINT0..3 = 0x640..0x6a0) instead
of on a vector fixed per role.

**What the SOURCE key costs.** Measured with `boot_vm --source-key usb
--source-key-at 40` and `CDJ_PANEL_HOLD_MS=2800` (the default 300 ms hold
reaches MAIN -- `0x04c084d4` goes to 1 -- but the GUI never learns of it): the
`Wait` platter appears six seconds after the key, animates at about 2.4 frames
a second, and the GUI's browse requests switch to the USB source about a
minute later. None of that is interpreter speed: the GUI board runs at
2-5 MIPS the whole time. The platter is the router's answer to a source whose
media state (status word 26) MAIN leaves at zero, which without a USB host it
always does; the latency and the frame rate are the rate at which the GUI
parses MAIN's status records, and MAIN's answers to the GUI's ~60 requests a
second arrive in one burst every 3.000 s -- hundreds of them 0.1 ms apart
behind one status record. MAIN builds a fresh status record on that same
3 s cadence when nothing changes, and a key is copied into the record built
while it is down, which is why `panel_control` holds a press for 2.8 s.

Both smelled like a completion reported inside the arm write, before the
task that waits for it is waiting, so each side got a switch that delays it
by a frame's time on the wire (the SPORT transmits at SCLK/34, about
2.9 Mbit/s, so a 64-byte record is ~180 us; the receive clock comes from
MAIN): `CDJ_LINK_TX_US` on the board and `BFIN_SPORT_RX_US` on the
simulator, the latter on the 64-byte record receives only. Measured, each
alone at 500 us and 200 us: the GUI-side delay left the screen black until
t94 in one run and the GUI silent for 45 s in another, the board-side delay
left two boots of three without a player screen. Together with a deep
receive ring on the GUI side (`BFIN_LINK_DEPTH=128`, so a plain record
cannot land on an announcement before its payload is read) the board-side
delay boots cleanly -- three of three -- and in one run of three the
browse requests moved to the USB source within ten seconds of the key
instead of a minute; the platter still came four seconds after the key at
~2 fps, and MAIN's idle cadence stayed at 3 s. That is not enough to change
a default: the two protocol tasks are balanced against each other by timing
on both sides, and every switch is off unless set. The launchers keep the
configuration that boots six times of seven.

The same cadence decides every key. A click in the `view_vm` window holds
the key 3.3 s, long enough to span one of MAIN's records, and the screen
follows about five seconds after the click. The firmware tells a short
press from a held one by whether the key is still down in the *next*
record, so a held key on this link is one held across two record builds:
Shift-click holds 6.5 s. Measured on `MENU`: 1.5 s and 3.3 s open the CUE
LINK box, 7 s opens the UTILITY screen, with its list empty because the
entries are payloads MAIN does not deliver. A second click on the same key
while the first is still down is refused with the time left: the board
queues presses one behind the other, and a queued MENU closes what the
first one opened, which is what repeated clicking looked like. Right-click
holds a key down until the next right-click.

MAIN's RTOS tick, read back through the monitor: 120-880 a second with the
old interrupt patch depending on host load, ~830 of the programmed 1000 with
the decline fix, the real 54 MHz timer clock and the Windows timer resolution
raised to 0.5 ms, and the full 1000 with QEMU's main loop waiting for less
than a millisecond (a patched `util/main-loop.c`; `CDJ_MAIN_LOOP_HIRES=0`
switches it off). At the full rate the GUI board double-faulted at
`0x00b99196` in every run until the launchers set
`BFIN_LINK_ANNOUNCE_STICKY=1`; with it, three of three 90 s boots at 1000
ticks a second were clean. `CDJ_TMU_FREQ` still overrides the timer clock; the
old default of 270 MHz asked for 5000 interrupts a second that the guest never
serviced, and is gone from the launchers.

Measurements are only worth anything on an idle host. A configure script or a
compile running alongside halves the simulator's throughput, and it did so
twice during this work before the rule was learnt.

## Environment

| variable | does |
|---|---|
| `CDJ_QEMU` | path to `qemu-system-sh4` if it is not on `PATH` |
| `CDJ_BFIN_SIM` | path to the Blackfin simulator, default `bin/cdj-run` |
| `CDJ_QEMU_DLL_DIR` | directory of the DLLs a MSYS2-built QEMU needs |
| `CDJ_FIRMWARE_DIR`, `CDJ_PACKETS_DIR`, `CDJ_RUNS_DIR`, `CDJ_BIN_DIR` | move the working directories |

The board itself takes a long list of its own, all read with `getenv` in
`emulator/qemu/`: `CDJ_INPUT_PORT`, `CDJ_PANEL_KEYS`, `CDJ_SD_INSERT`,
`CDJ_DSP_ABSENT`, `CDJ_USB_ABSENT`, `CDJ_ATAPI_ABSENT`, `CDJ_BUS_TRACE`,
`CDJ_USBH_TRACE` (the USB host module: registers, packets, DMA),
`CDJ_PANEL_SCIF_TRACE`, `CDJ_DMAC_TRACE`, `CDJ_WATCH` (writes to a word, with
the PC), `CDJ_PANEL_FRAME` (the panel's 22 payload bytes, held),
`CDJ_LINK_TRACE` (arm, acknowledge and gate lines with virtual-clock stamps,
and the header words of every request delivered), `CDJ_LINK_TX_US` (off),
`CDJ_LINK_RX_GAP_US` (the least guest time between two GUI frames going into
MAIN's receive buffer, default 2000: the simulator delivers frames in bursts
and two of them 0.1 ms apart made GuiCom_RcvTASK read the second twice --
an injected LOAD taken twice failed the load in `trackload-42-final`; 0
restores the burst) and `CDJ_LINK_RX_HANDOVER=answer` (hand the next queued
frame over only when MAIN transmits; measured worse in `trackload-48-launch2`
and kept for the A/B),
`CDJ_NO_PANEL` and `CDJ_NO_USB_POWER` (the two input bits of GPIO
`0xfff10060` the board holds high: the panel-present bit and the USB power
switch's sense line, whose absence made MAIN raise caution `0x92` "USB Error"
every 50 polls), `CDJ_DSP_ACK` (the DSP model zeroes its control block once
MAIN has seen it up and answers the request words a track load and its PCM
stream write there; off, an experiment -- see "Loading a track"),
`CDJ_DSP_TRACE` (firmware pages, mailbox, every acknowledged request -- the
format command +0x8100 with its whole parameter block +0x8104..+0x8124 -- a
per-second census of the control block, every event posted),
`CDJ_DSP_READ_TRACE[=LO-HI,...]` (MAIN's reads of the window, which is RAM
and otherwise leaves no trace: an overlay over the hex offset ranges, by
default `7b80-7d00,8100-81e0`, reports each new word/PC pair as `new reader
+0x7ccc by pc 0x041be168` and a per-second `reads t=...: +OFF@PC=COUNT`
census; a transfer's own copy counts as `dma`, gdb and monitor accesses not
at all; slows those window pages), `CDJ_DSP_EVENT_PROBE=<start s>[:<interval s>[:<codes>]]`
(post DSP event codes to MAIN on its interrupt line -- irq 0x7f, bit 24 of
0xffd4005c, GPIO 0xfff10040 bit 4, the code in bytes 2/3 of window+0xffe8:
byte 2 is the class the player task switches on, byte 3 a parameter --
one every interval from the start second on; `<codes>` is a range
`<first>-<last>` (default 1..13) or a list `0x100,0x200,...`. Measured:
every code is acknowledged within a millisecond (trackload-50/51b); class
1, 2 and 5 events are dropped unless the report number below has changed
(trackload-55); a class-1 event with a new number is "segment finished" --
MAIN pops its segment queue, finds nothing, unloads the track and its time
fields turn from blank to 00:00 (trackload-56/57); class 3 makes MAIN write
window+0x7ba4 = 1 and wait 6 s for the DSP to clear it, then E-8302 000F
(trackload-55)), `CDJ_DSP_REPORT_ID=<n>` (the DSP's report sequence number
at window+0x7cd4, which MAIN compares with its copy before handling a class
1/2/5 event and copies afterwards; the model writes n+1, n+2, ... there
before each event it posts), `CDJ_DSP_SLOT_REPORT=<state>` (after the
load's closing commands 4 and 2 write that state into the four slot-table
entries at window+0x7ce0 with the position words +96/+100 -- 22050ths of a
second and CD sectors, the reader divides +96 by 294 -- then post the event
`CDJ_DSP_SLOT_EVENT` (default 0x100, 0 = none); PLAY makes it state 3, and
from then on the position advances and the entries are rewritten every
`CDJ_DSP_SLOT_PERIOD_MS` (500); state 2 plus the event got E-8302 in
trackload-52/57 -- the entry is read, its vocabulary is still open),
`CDJ_DSP_PLAY_EVENT=<code>` (the event posted with each periodic state-3
report instead; default none -- class 0 code 1 there made MAIN re-stream
in a loop and stop with E-8302 C611, trackload-69), `CDJ_DSP_CONSUME=<units
per second>` (while the slot state is 3 the DSP consumes: both fill levels
+0x7cd0/+0x7ccc go down and the per-buffer status words +0x81a0/+0x8180 go
up by that many units -- 40 units book one 9408-byte PCM transfer of 53.3 ms,
so 750 is real time; trackload-67 measured that MAIN does not poll the
levels: nothing happened until an event came; since trackload-71 only
buffer 1 is consumed), `CDJ_DSP_REFILL_EVENT=<code>` with
`CDJ_DSP_REFILL_LOW` (default 20: post that event once whenever buffer 1's
level falls under the mark -- trackload-71: MAIN answered class 0 code 1
with a stop and E-8302 even before the buffer was empty, so class 0 is not
the refill path while playing), `CDJ_DSP_STATUS_FLAGS=1` (raise the two
buffer-active bits the stream worker reports to the player, +0x81ac bit 24
and +0x818c bit 25, while the slot state is 3; trackload-73: no visible
effect). What did hold: trackload-72 -- class-5 events with a fresh report
number every 500 ms while playing (`CDJ_DSP_PLAY_EVENT=0x500
CDJ_DSP_REPORT_ID=1 CDJ_DSP_CONSUME=750`) made MAIN stream buffer-1 data
following the position in +0x81a0 every half second without an error; the
deck position word and the time display still did not move),
`CDJ_DSP_POSITION=1` (the DSP's position report block +0x7bf0..+0x7c60 that
MAIN's reader 0x19e568 takes every tick: position in CD frames at +0x7c10,
sample offset in the frame at +0x7bf4, the load-queue record id at +0x7c14
from the +0x8100 = 2 command's +0x8120; trackload-88 -- the time display
runs from PLAY with the track's real length, see "Switching to a medium"),
`CDJ_DMAC_TRACE` (every DMA start
with channel, SAR, DAR, TCR, CHCR and role), `CDJ_SDHI_TRACE` (every SD
command; walking the card image's FAT for the block addresses says which
file a read was -- `runs/nxs-swap/trackload-39-final/fatmap.py` does that)
and more. The simulator likewise: `BFIN_MAIN_LINK`, `BFIN_GUI_OUTPUT`,
`BFIN_GUI_COLOR`, `BFIN_PPI_DMA_DELAY`, `BFIN_SPORT_TX_OUTPUT`,
`BFIN_SPORT_RX_US` (off), `BFIN_LINK_REPEAT_ANNOUNCED` (on: a payload MAIN's
record still announces is handed over again when the firmware arms for it),
and the time-base knobs above: `BFIN_TIME_BASE`,
`BFIN_CCLK_HZ`, `BFIN_PPI_FPS`, `BFIN_SPORT_RETRY_US`, `BFIN_WALL_LAG_MS`,
`BFIN_STATS`, `BFIN_EXIT_AFTER_WALL`, `BFIN_EXIT_AFTER_TICKS`,
`BFIN_MEM_FAST`. Each is documented where it is read.

The simulator's diagnostic probes -- every `BFIN_*_TRACE`, `BFIN_PC_*`,
`BFIN_*_DUMP`, watch, peek and poke variable -- are behind one gate. Setting
any of them puts the whole per-instruction probe path back, which is what the
probes need and costs about a third of the throughput; a plain run pays one
branch per instruction for them. The simulator says `bfin: probes on (NAME is
set)` on its log when that happens, so a slow run can be explained.

## One QEMU at a time

Run QEMU serially. Several TCG instances distort the timing races this firmware
depends on, and a distorted race is a void measurement. The runners use fixed
ports and take no lock, so an orphaned process either fails the next run or --
worse -- talks to it. `tools/cdj_main/procs.py` cleans up; it exists because
fourteen orphaned simulators were once found at once.
