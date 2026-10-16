/*
 * Pioneer CDJ-2000 USB controller — the seam between the bus and the model.
 *
 * `E-7020: USB-B DEVICE ERROR` is not a GUI message and not a guess.  MAIN keeps
 * an eight-entry device-status array at 0x04c0875c; 0x24fd20 writes
 * `array[device] = state` (1 up, 2 failed), scans it and turns the first failed
 * device into a caution code — device 1 into 2 (E-7010, the DSP), device 2 into
 * 62, **device 3 into 61 (E-7020)**, device 4 into 63, device 6 into 1, device 7
 * into 68 — before posting it.  Device 3 is reported by `USBFD_TSK` (0x2dea9e),
 * the USB *function* task, which is the type-B socket that faces a computer.
 *
 * What fails is nine instructions long.  `0x2399c8`:
 *
 *     r2 = 0xA1000000                 ; #-95 << 8 << 16
 *     r4 = 0
 *   loop:
 *     W[r2] = 1                       ; enable the module
 *     if (W[r2] & 1) goto up          ; wait for it to read back
 *     if (--r4 as int8 != 0) goto loop  ; 255 attempts
 *   up:
 *     B[0x07db2cbc] = r4
 *     return r4 == 0                  ; non-zero return means "never came up"
 *
 * and USBFD_TSK turns a non-zero return into `0x24fd20(3, 2)`.  Physical
 * 0x01000000 is not mapped by the board — CS0 carries 4 MiB of NOR flash at 0 —
 * so the write vanishes, the read answers 0, the loop runs its 255 attempts and
 * the caution is raised.  That is the entire bug.
 *
 * The controller is a 16-bit register file, and every access the firmware makes
 * to it is a read-modify-write of bits it set itself:
 *
 *   0x2399bc   W[0x00] = 0                                   module off
 *   0x2399c8   W[0x00] = 1, poll bit 0                        module on
 *   0x2397cc   W[0x00] |= 0x8000, then |= 0x0d01; W[0x0e] |= 2
 *   0x239840   W[0x10] = 0x80, |= 0x8000, |= 0x1000, |= 0x080c, |= 0x0700
 *   0x239802   copies a descriptor block from 0xa40723f4 into W[0x28],
 *              W[0x2a], W[0x2c], W[0x30], W[0x32] and masks bits in the last two
 *   0x2dea9e   reads W[0x18] bit 7 -- the strings beside it are
 *              "USBF Init Vbus = %d", "USBF Vbus ON", "USBF Vbus OFF"
 *   0x2df2d4   W[0x00] &= ~0x02ff, |= 0x0100 or 0x0300
 *
 * Only one bit is ever waited on and the firmware sets it itself, so a register
 * file that remembers what was written satisfies the whole bring-up without
 * inventing a single value.  The one input the guest cannot produce is VBUS,
 * and the model owns that.
 *
 * **No USB traffic was modelled at all** when this was written -- no reset, no
 * enumeration, no descriptors, no endpoints, no MIDI, HID or audio.  That was
 * the same scope as cdj2000_dsp.c, which models no audio path: the device
 * existed so that the bring-up succeeds and MAIN stops reporting the port as
 * broken.  The chip itself (a Renesas M66291GP) is now modelled behind this
 * window, with a host on the other side of the cable; that is
 * cdj2000_usb_model.h and cdj2000_usb_model.c (RaftTone, from the M66291GP/HP
 * data sheet).  Everything above is LycheeAPPF's derivation and still holds.
 *
 * Copyright (C) 2026 LycheeAPPF
 * Copyright (C) 2026 RaftTone (the additions marked above)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CDJ2000_USB_H
#define CDJ2000_USB_H

#include "exec/hwaddr.h"
#include "hw/core/irq.h"
#include "system/memory.h"

/*
 * The register window (CDJ_USB_BASE, CDJ_USB_SIZE), the offsets the firmware
 * proves, the model's constructor and its read and write entry points are in
 * cdj2000_usb_model.h, which does not depend on QEMU so that the chip can be
 * tested on its own.
 */
#include "cdj2000_usb_model.h"

/*
 * The device side, called from the board.  `int0` is the M66291's INT0 pin as
 * the SH7763 sees it (NULL leaves it unconnected, which is the machine as it
 * was), and `level` is told the pin's level as well, for the INTC2 status bit
 * that goes with it.  INT1 is not wired on this board; the firmware assigns
 * every source to INT0.
 */
typedef void (*CdjUsbLevelFn)(void *opaque, bool asserted);

void cdj_usb_init(MemoryRegion *system, qemu_irq int0, CdjUsbLevelFn level,
                  void *opaque);

#endif /* CDJ2000_USB_H */
