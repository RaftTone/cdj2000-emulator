/*
 * Pioneer CDJ-2000 disc drive — putting a real ATAPI drive on the bus.
 *
 * cdj2000_ata.h has the derivation of the window and the interrupt.  This file
 * is only the wiring, and it is deliberately thin: QEMU already has the ATA
 * state machine (`hw/ide/`), and a hand-written stub that answers plausible
 * status bytes would be a guess about a protocol the firmware knows better
 * than we do.  `TYPE_MMIO_IDE` with `shift=2` maps register n at n*4, which is
 * exactly the stride 0x2970b0 proves, so the generic device fits the measured
 * layout without adaptation.
 *
 * The drive is `ide-cd` with no `drive=`.  ide_dev_initfn gives a driveless
 * IDE_CD an anonymous BlockBackend, which is precisely the state the reference
 * photo shows: a drive that is present and answers, with no medium in it.  A
 * real CDJ with an empty tray reports `NO DISC`, and `NO DISC` is wanted —
 * what has to go is the `E-7001` caution, which is a different thing.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/ide/mmio.h"
#include "system/blockdev.h"

#include "cdj2000_ata.h"

/*
 * The chip-level block at 0xfff00080 is the drive's interrupt controller, and
 * the firmware's own access pattern says which register is which.  Read out of
 * a run with the line connected and CDJ_ATA_TRACE=1, the ISR loops on exactly
 * three accesses, 346 808 times in ninety seconds:
 *
 *     read  +0x08 = 0x10     enable: written 0x10 once at init, then only read
 *     read  +0x04 = 0        pending: read every pass
 *     write +0x04 = 0        and written back every pass
 *
 * A register written once during setup and polled forever afterwards is a
 * mask; the one read and written on every pass is the status.  So `+0x04` is
 * pending, `+0x08` is enable, and bit 0x10 is the drive.
 *
 * That loop is also the whole bug this replaced.  Modelled as plain read-back,
 * pending always answered 0, so the ISR concluded the interrupt was not its
 * own and returned — while the IDE core still held its level asserted, which
 * re-entered the ISR immediately.  The machine livelocked before the SD volume
 * ever registered.
 *
 * Pending therefore *mirrors the IDE core's output line* rather than being
 * cleared by the guest's write.  That avoids inventing an acknowledge
 * convention nobody measured: QEMU's IDE lowers its output when the task
 * file's status register is read, which is what servicing the drive does
 * anyway, so the source de-asserts itself and the mirror follows.  The write
 * is still recorded, so a later trace can show whether the firmware also
 * expects write-to-clear.
 *
 * +0x00 (written 0x20 once) and +0x0c (written 0xd440d44 once) are setup
 * registers whose bits are not derived; they stay read-back.
 */
#define CDJ_ATA_CHIP_PENDING    1       /* +0x04 */
#define CDJ_ATA_CHIP_ENABLE     2       /* +0x08 */
#define CDJ_ATA_CHIP_DRIVE_BIT  0x10    /* the bit ATAPI_TSK enables */

typedef struct CdjAtaChip {
    MemoryRegion iomem;
    uint32_t reg[CDJ_ATA_CHIP_SIZE / 4];
    qemu_irq parent_irq;        /* on to the SH-4 INTC, INTEVT 0xc00 */
    bool trace;
} CdjAtaChip;

/* The line the SH-4 sees is the drive's, gated by the firmware's own mask. */
static void cdj_ata_chip_update(CdjAtaChip *chip)
{
    bool level = (chip->reg[CDJ_ATA_CHIP_PENDING] &
                  chip->reg[CDJ_ATA_CHIP_ENABLE]) != 0;

    if (chip->parent_irq) {
        qemu_set_irq(chip->parent_irq, level);
    }
}

static void cdj_ata_chip_set_irq(void *opaque, int n, int level)
{
    CdjAtaChip *chip = opaque;

    if (level) {
        chip->reg[CDJ_ATA_CHIP_PENDING] |= CDJ_ATA_CHIP_DRIVE_BIT;
    } else {
        chip->reg[CDJ_ATA_CHIP_PENDING] &= ~CDJ_ATA_CHIP_DRIVE_BIT;
    }
    if (chip->trace) {
        qemu_log("cdj2000-ata: drive irq %d, pending now %#x\n",
                 level, chip->reg[CDJ_ATA_CHIP_PENDING]);
    }
    cdj_ata_chip_update(chip);
}

static uint64_t cdj_ata_chip_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjAtaChip *chip = opaque;
    uint32_t value = chip->reg[(offset / 4) % ARRAY_SIZE(chip->reg)];

    if (chip->trace) {
        qemu_log("cdj2000-ata: chip read  %#04" HWADDR_PRIx " (%u) = %#x\n",
                 offset, size, value);
    }
    return value;
}

static void cdj_ata_chip_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    CdjAtaChip *chip = opaque;
    unsigned index = (offset / 4) % ARRAY_SIZE(chip->reg);

    /*
     * Pending belongs to the source, not to the guest: it is recomputed from
     * the IDE line in cdj_ata_chip_set_irq, so a write here must not fabricate
     * a level the drive is not asserting.
     */
    if (index != CDJ_ATA_CHIP_PENDING) {
        chip->reg[index] = (uint32_t)value;
    }
    if (chip->trace) {
        qemu_log("cdj2000-ata: chip write %#04" HWADDR_PRIx " (%u) = %#x\n",
                 offset, size, (uint32_t)value);
    }
    if (index == CDJ_ATA_CHIP_ENABLE) {
        cdj_ata_chip_update(chip);
    }
}

static const MemoryRegionOps cdj_ata_chip_ops = {
    .read = cdj_ata_chip_read,
    .write = cdj_ata_chip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_ata_init(MemoryRegion *system, qemu_irq irq)
{
    DeviceState *ide, *cd;
    SysBusDevice *sbd;
    BusState *bus;
    CdjAtaChip *chip;
    DriveInfo *drive;

    if (getenv("CDJ_ATAPI_ABSENT")) {
        qemu_log("cdj2000-ata: CDJ_ATAPI_ABSENT -- no drive on the bus, "
                 "which is the machine before this device existed\n");
        return;
    }

    /*
     * The chip block comes first: the drive's line runs into it, not straight
     * to the INTC, so it has to exist before the IDE device is connected.
     */
    chip = g_new0(CdjAtaChip, 1);
    chip->trace = getenv("CDJ_ATA_TRACE") != NULL;
    /*
     * CDJ_ATA_NO_IRQ leaves the line unconnected, which separates two
     * questions that otherwise arrive together: does the task file answer at
     * all, and is the interrupt handled?  It is what showed the livelock:
     * with the line off the card mounts and device 6 comes up, with it on and
     * pending stuck at zero the machine never got that far.
     */
    if (!getenv("CDJ_ATA_NO_IRQ")) {
        chip->parent_irq = irq;
    } else {
        qemu_log("cdj2000-ata: CDJ_ATA_NO_IRQ -- the drive answers but never "
                 "interrupts\n");
    }

    ide = qdev_new("mmio-ide");
    sbd = SYS_BUS_DEVICE(ide);
    qdev_prop_set_uint32(ide, "shift", 2);
    sysbus_connect_irq(sbd, 0,
                       qemu_allocate_irq(cdj_ata_chip_set_irq, chip, 0));
    sysbus_realize_and_unref(sbd, &error_fatal);

    /*
     * Region 0 is 16 registers wide, so at shift 2 it spans 0x00..0x3f and
     * would answer at +0x38 as well — as device/head, because 0x38 >> 2 is 14
     * and mmio_ide_read masks the register number with 7.  The control region
     * therefore has to go *over* it, not merely at a different address.
     */
    sysbus_mmio_map(sbd, 0, CDJ_ATA_BASE);
    sysbus_mmio_map_overlap(sbd, 1, CDJ_ATA_BASE + CDJ_ATA_CTRL, 1);

    /*
     * mmio-ide creates exactly one child bus and its name comes from a global
     * counter, so take the bus itself rather than looking up "ide.0" — the
     * name would silently change the day another IDE bus is created first.
     */
    bus = QLIST_FIRST(&ide->child_bus);
    g_assert(bus != NULL);

    cd = qdev_new("ide-cd");
    qdev_prop_set_uint32(cd, "unit", 0);

    /*
     * Keep the measured empty-drive default, but let a firmware developer
     * attach a real disc image using QEMU's normal IDE drive syntax:
     *
     *   -drive if=ide,media=cdrom,bus=0,unit=0,file=disc.iso,format=raw
     *
     * drive_get() is deliberately used instead of opening a path from an
     * environment variable.  QEMU owns the backend lifetime, locking and
     * format probing, and this also makes the image visible to standard
     * tooling.  With no matching -drive, ide-cd retains its anonymous empty
     * backend and the existing NO DISC behavior is unchanged.
     */
    drive = drive_get(IF_IDE, 0, 0);
    if (drive != NULL) {
        qdev_prop_set_drive_err(cd, "drive", blk_by_legacy_dinfo(drive),
                                &error_fatal);
        qemu_log("cdj2000-ata: attached IDE CD backend from -drive bus=0,unit=0\n");
    }
    qdev_realize_and_unref(cd, bus, &error_fatal);

    memory_region_init_io(&chip->iomem, NULL, &cdj_ata_chip_ops, chip,
                          "cdj2000.ata-chip", CDJ_ATA_CHIP_SIZE);
    memory_region_add_subregion(system, CDJ_ATA_CHIP_BASE, &chip->iomem);

    qemu_log("cdj2000-ata: task file at %#x, drive present with %s\n",
             CDJ_ATA_BASE, drive ? "attached medium" : "no medium");
}
