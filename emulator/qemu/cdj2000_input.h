/*
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/*
 * Runtime panel input for the CDJ-2000 MAIN board.
 *
 * CDJ_PANEL_KEYS presses buttons on a schedule fixed before the machine boots,
 * at most sixteen of them, which is enough to prove an input changes the screen
 * and not enough to operate the player.  This is the seam for the other kind:
 * something outside the emulator decides, while it runs, which buttons are down
 * and where the rotary stands.
 *
 * The board calls cdj_input_apply() once per panel reply, after the scheduled
 * keys have been merged and before the checksum is taken, so an implementation
 * may set any of the 22 payload bytes and the frame still validates.
 * Persistent `level BYTE MASK 0|1` commands replace selected bits after held
 * buttons, pulses and analogue fields. `clear` removes these overrides so
 * the board's base frame applies again. The configured SD lid takes priority.
 *
 * Behind the seam sits a line-oriented TCP server on 127.0.0.1, opened only
 * when CDJ_INPUT_PORT names a port.  Without that variable this file is inert
 * and a run is indistinguishable from one built without it, which is what makes
 * a control run a control run.  The protocol is described in cdj2000_input.c
 * and spoken by tools/cdj_main/panel_control.py.
 */
#ifndef CDJ2000_INPUT_H
#define CDJ2000_INPUT_H

#include <stdint.h>

/*
 * Merge externally driven input into the panel payload.
 *
 * `payload` is the frame's first `len` bytes -- the 22 the panel checksums,
 * without the checksum byte or the 0x8f marker.  Bytes 2..13 carry the analogue
 * fields (the rotary among them, mirrored to 0x04fe2a20) and bytes 18..21 the
 * 22 button bits (mirrored to 0x04fe2a3c).
 *
 * Buttons are edge-triggered: 0x28ddc8 compares each status byte against the
 * copy 44 bytes on, so a press only registers if the bit goes down *and* comes
 * back up.  Holding a bit set for ever is the same as never pressing it.
 */
void cdj_input_apply(uint8_t *payload, unsigned len);

/*
 * The analogue fields, as the panel decoder at 0x28e1d6 splits payload bytes
 * 2..13: two 8-bit fields and then five 16-bit big-endian ones, landing at
 * 0x04fe2a20 + 0, +4, +8, +12, +16, +20, +24 in that order.
 *
 * Verified live (memory cdj-panel-payload-decoded): payload byte 2 = 0x5a shows
 * up as [0x04fe2a20] = 0x5a, and bytes 4..5 = 12 34 as [0x04fe2a28] = 0x1234.
 *
 * **Field 7 is payload byte 14, and it is the one that had been missing.**  The
 * same decoder does not stop at byte 13:
 *
 *     0028e26e  mov.b @(14,r1),r0      ; payload byte 14, unsigned
 *     0028e276  mov #-40,r0; extu.b    ; 216
 *     0028e27c  mov.w @(r0,r10),r12    ; sign-extended halfword at 0x04fe2af8
 *     0028e282  add r12,r7             ; byte 14 + that bias
 *     0028e284  mov.l r7,@(36,r10)     ; -> 0x04fe2a44
 *
 * and MAIN's own panel simulator names it: the 66-arm dispatcher at 0x1010a4
 * writes one field per arm, and arms 64 and 65 do nothing but `+1` and `-1` on
 * that same halfword at 0x04fe2af8.  An arm pair that steps a signed counter by
 * one in each direction is an encoder; every other arm sets a level, a limit or
 * a switch bit.
 *
 * This matters because it is the reason a rotary sweep over fields 0..6 could
 * only ever have returned zeros: the encoder was never in the range the channel
 * could reach.  Bytes 15..17 carry further switch bits (spread into the status
 * block at 0x04fe29f4 + 74/75/79/86 by the same function) and are deliberately
 * left out of the analogue table. Momentary buttons use pulse/hold commands;
 * persistent switch contacts can use literal `level` commands.
 */
#define CDJ_INPUT_ANALOG_FIELDS 8

/* First payload byte of analogue field `n`, and its width in bytes. */
unsigned cdj_input_analog_byte(unsigned field);
unsigned cdj_input_analog_width(unsigned field);

#endif /* CDJ2000_INPUT_H */
