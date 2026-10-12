# NXS panel profile

The legacy CDJ-2000 key map is not valid for the NXS panel. NXS MAIN's
decoder at `042f5810` maps raw byte 19 bits 0..4 to status +5e/10,
+5f/80, +5f/40, +5f/20, +5f/10. Service masks at unpacked offsets
`b0eb4`, `b0fac`, `b0fdc` and names at `3c32f0` identify these as
REKORDBOX, LINK, USB, SD, DISC. TIME/ACUE moves to 19.5.

The same decoder also moves REC/PREVIOUS/NEXT/SEARCH, JOG MODE,
TEMPO RANGE, MASTER TEMPO, TEMPO RESET, CALL, DELETE, and MEMORY.
`tools/cdj_main/nxs_panel.py` records the separate NXS map. Its changed
contacts are checked against the real NXS service-name tables in
`tests/test_nxs_panel_profile.py`. This verifies names from decoded status
bits; it does not claim every control has received an end-to-end live test.

With `--nxs-panel` (already supplied by the NXS launcher), both viewer skins
use NXS contacts. The faceplate's existing visual positions resolve by
function, and pressed-state feedback maps back to those positions. Raw
inspector inputs remain literal bytes/bits; NXS-only contacts are available
there. Legacy names and coverage are unchanged, and legacy run annotations
are not presented as NXS evidence. Analogue-field attribution is unchanged.

## Live evidence and limits

The old `runs/nxs-usb-track-long-1` USB-labeled 19/02 probe displayed LINK,
consistent with the corrected map. In the strict-DSP run
`runs/nxs-track-load-sd-1`, source selection uses actual 19/08 contacts,
and BROWSE 20/01 toggles between the browser and player view. The browser
still displays Wait at the time of this change: this is not track-load proof.

This longer SD run has advanced beyond identification to CMD17, CMD18,
CMD25, automatic CMD12, and firmware filesystem writes into the temporary
overlay. A stopped snapshot `after-writes-ram.bin` records SD mode
`04cf2180=3`. Earlier short runs ending at mode 2 did not establish a
permanent SD initialization failure. The strict DSP has passed 2.6 billion
packets without the previous interrupt-entry conflict. Continued browser and
track-load investigation is required; TESTTONE.WAV is not yet verified loaded.

Focused profile/layout/control/socket regression run: 119 passed, 2 optional
skips. Name/profile/layout set: 28 passed, 8 optional skips.

Full suite at the first profile commit: 430 passed, 29 optional skips.
Native Tk follow-up: 35 passed, including actual NXS USB mouse down/up,
the corresponding faceplate highlight, no LINK highlight for the raw
REKORDBOX contact, and unchanged legacy encoder/inspector coverage.

Further decoder/table comparison identifies 17.1 as LOOP MODE (status
+53 bit 3, service code 35) and 17.3 as SLIP (+53 bit 4, code 34).
The legacy 4-BEAT LOOP visual slot is not assigned an invented NXS contact;
the verified functions remain available in the raw inspector. Native Tk
profile/layout tests after this correction: 35 passed.

## REV contact polarity

The NXS image supplies the polarity evidence at decoder entry `0x042f5810`,
not through the legacy map. At image offset `0x002f58b8` the decoder loads raw
byte 15 (`mov.b @(15,r7),r0`). At `0x042f58d4` it tests bit 1 and the following
short branch sequence is `bt.s`, `and #0xbf`, `or #0x40`. Therefore raw byte
15 bit 1 clear leaves status bit 6 clear, while the bit set leaves status bit 6
set. The decoder does not invert this bit.

The electrical interpretation is established by the native capture
`runs/agent-play-checkpoints`: with REV low, the loaded deck selected negative
direction and remained at the start boundary; with REV high, direction became
forward and the native remaining counter fell from 1500 to 1496 across five
fresh samples. Thus a high raw level is the verified neutral REV contact level;
an all-zero panel frame asserts reverse. The separate NXS decoder sequence at
`0x042f59e4` (`tst #4`, `bt.s`, `or #2`, `and #0xfd`) proves the SD OPEN contact
(raw byte 17 bit 2) is also active low: raw high clears the status bit, which is
the closed/neutral state. The launcher already forces that level for
`CDJ_NXS_SD_LID=closed`. The remaining zero-default contacts have not been
proven neutral by an equivalent native test.
