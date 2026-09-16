/*
 * Pioneer CDJ-2000 disc drive — the ATA task file at 0xfff00000.
 *
 * `E-7001: DISC DRIVE ERROR` is device 6 in the status array at 0x04c0875c,
 * and it is raised for the same reason `E-7020` was: the chip is not on the
 * bus.  The window was found without a datasheet, out of the `-d unimp` log of
 * an ordinary run — MAIN writes 0xfff0001c with 0xA0, 0xEC and 0xA1 (PACKET,
 * IDENTIFY DEVICE, IDENTIFY PACKET DEVICE) and *reads* that same address
 * 44 856 times in one run, which is a status register being polled forever
 * because it answers zero.
 *
 * The layout is the standard task file at a four-byte stride.  0x2970b0
 * computes `0xfff00000 + channel * 60` and writes 6 then 2 to `+0x38` — the
 * ATA soft-reset sequence — so `+0x38` is Device Control / Alternate Status
 * and the stride is fixed by the firmware, not assumed:
 *
 *     +0x00  data                     +0x14  LBA high / byte count high
 *     +0x04  error / features         +0x18  device / head
 *     +0x08  sector count / reason    +0x1c  status / command
 *     +0x0c  LBA low                  +0x38  alt status / device control
 *     +0x10  LBA mid / byte count low
 *
 * Channel stride is 60 (0x297096), so channel 1 would be at +0x3c and is never
 * touched: only channel 0 is used.
 *
 * The interrupt is measured, not guessed: irq 0x60, INTEVT 0xc00, ISR
 * 0x109180, read out of the RTOS thunk block with `isr_map`.  0x10918a loads
 * 0xfff00080, which is the cross-check.  `ATAPI_TSK` gives it a level in
 * INT2PRI6 (0xffd40018, field 0) and unmasks it through 0xffd4003c.
 *
 * 0xfff00080 and +0x88 are a separate chip-level block — written 0xa0/0x20 and
 * 0x10/0 — whose bit meanings are not derived yet.  It is modelled as plain
 * read-back so those accesses stop falling through to the catch-all trap; see
 * cdj2000_ata.c for why that is the honest choice rather than a guess.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CDJ2000_ATA_H
#define CDJ2000_ATA_H

#include "system/memory.h"
#include "hw/core/irq.h"

/* Channel 0's task file.  Channel stride is 60; channel 1 is never used. */
#define CDJ_ATA_BASE        0xfff00000
#define CDJ_ATA_CTRL        0x38        /* alt status / device control */

/* The chip-level block the ISR reads.  Not the task file. */
#define CDJ_ATA_CHIP_BASE   0xfff00080
#define CDJ_ATA_CHIP_SIZE   0x10

/*
 * Attach the drive.  `irq` is the INTC line for INTEVT 0xc00.
 *
 * A real image may be attached with QEMU's normal IDE drive option:
 * `-drive if=ide,media=cdrom,bus=0,unit=0,file=disc.iso,format=raw`.  When no
 * matching drive is supplied the model keeps an empty, present CD drive, so
 * the firmware still sees a real ATAPI device reporting no medium.
 *
 * CDJ_ATAPI_ABSENT=1 skips the whole device, which restores the machine as it
 * was before this file existed and makes the A/B one binary.
 */
void cdj_ata_init(MemoryRegion *system, qemu_irq irq);

#endif /* CDJ2000_ATA_H */
