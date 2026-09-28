/*
 * Pioneer CDJ-2000 MAIN board (SuperH SH-4)
 *
 * Board model for reverse engineering the MAIN firmware of a CDJ-2000, so that
 * the real firmware — rather than a hand-written peer — can answer the Blackfin
 * GUI board that is emulated separately in the GNU simulator.
 *
 * The memory map is derived from the firmware image itself, not from a
 * datasheet (none is public):
 *
 *   - the reset vector loads r15 = 0xa8000000, so the top of SDRAM is at
 *     physical 0x08000000;
 *   - the boot DMA copies the packed payload to 0xa7a00000 and a stub to
 *     0xa7de0000, i.e. physical 0x07a00000 and 0x07de0000;
 *   - the unpacked application links at 0xa4000000, i.e. physical 0x04000000;
 *   - every known RAM object lies between those two.
 *
 * One contiguous 64 MiB SDRAM at physical 0x04000000 satisfies all of it.
 *
 * The SoC is not an SH7750/SH7751: it puts the DMAC at 0xff608000 and has
 * peripherals at 0xff401000, 0xff501000, 0xffd40000 and 0xfff10000.  Rather
 * than guess a part number, every peripheral access is trapped and logged, so
 * the requirement list is measured.  Devices are then added one at a time.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "target/sh4/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/block/flash.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "hw/sh4/sh_intc.h"
#include "hw/sd/sd.h"
#include "hw/timer/tmu012.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/system.h"
#include "chardev/char-fe.h"

#include "cdj2000_ata.h"
#include "cdj2000_cosim.h"
#include "cdj2000_dsp.h"
#include "cdj2000_nxs_hpi.h"
#include "cdj_nxs_iic.h"
#include "cdj_nxs_eth.h"

static bool cdj_nxs_profile;
#include "cdj2000_input.h"
#include "cdj2000_usb.h"
#include "cdj2000_usbh.h"

/*
 * CS0 carries an AMD-command-set NOR flash, not a plain ROM.  During boot the
 * firmware runs the JEDEC unlock sequence — 0xaa to 0xaaa, 0x55 to 0x554, then
 * command 0x30 — and data-polls DQ7/DQ5 at 0xa03f4000 (image offset 0x1e95b2).
 * Against a read-only region that poll never ends.
 *
 * The flash is deliberately *not* backed by a block device: the guest erases
 * and rewrites its settings area, and that must not reach the firmware file on
 * disk.  The image is loaded into the device's RAM instead, so every run starts
 * from the same clean state.
 */
#define ROM_BASE        0x00000000
#define ROM_SIZE        (4 * MiB)       /* the image is 2 653 536 bytes */
/*
 * A 32 Mbit top-boot part: 63 sectors of 64 KiB and eight of 8 KiB at the top.
 * The geometry is the firmware's, read off its erase commands: the settings
 * area is erased at 0x3f0000, 0x3f2000, 0x3f4000 and 0x3f6000 (8 KiB apart),
 * and the updater (0x2d68b2 via 0x2d6116) erases 0x40000, 0x50000, ...,
 * 0x3d0000 before it programs the application region (update-18).  With
 * uniform 4 KiB sectors those erases cleared a sixteenth of each sector and
 * a rewritten image could never turn a 0 bit back into a 1.
 */
#define FLASH_MAIN_SECTOR   (64 * KiB)
#define FLASH_MAIN_SECTORS  63
#define FLASH_BOOT_SECTOR   (8 * KiB)
#define FLASH_BOOT_SECTORS  8
#define SDRAM_BASE      0x04000000
#define SDRAM_SIZE      (64 * MiB)

/*
 * SH-4 reaches the same on-chip registers through P4 (0xff000000) and through
 * area 7 (0x1f000000); only the low 29 bits matter.  One trap region covers
 * both, exactly as sh7750.c does for the real thing.
 */
#define PERIPH_A7_BASE  0x1f000000
#define PERIPH_P4_BASE  0xff000000
#define PERIPH_SIZE     0x01000000
#define PERIPH_PRIORITY (-1000)

/*
 * The one peripheral the boot cannot get past: it programs a DMA channel and
 * then spins until the transfer-end bit appears.  Register offsets and the
 * 16-byte TCR unit are read off the copy helper at image offset 0x4da, and
 * confirmed by the first boot trace (SAR=0xa0040000, DAR=0xa7a00000,
 * TCR=0x3a000 for a 0x3a0000-byte copy).
 */
#define DMAC_BASE       0x1f608000
#define DMAC_SIZE       0x1000
#define DMAC_DMAOR      0x0060      /* 16-bit; bit 0 enables the controller */
#define DMAC_CHANNELS   16
#define DMAC_CH_SAR     0x0
#define DMAC_CH_DAR     0x4
#define DMAC_CH_TCR     0x8
#define DMAC_CH_CHCR    0xc
#define CHCR_DE         0x0001      /* start */
#define CHCR_TE         0x0002      /* transfer ended — what the boot polls */
#define CHCR_IE         0x0004      /* interrupt enable: the request is TE && IE */
/*
 * TCR's unit is not fixed: it is whatever CHCR's transfer-size field selects,
 * and the firmware states it twice over rather than leaving it to a datasheet.
 * The boot copy at image offset 0x4da programs CHCR 0x…5438 and passes
 * `size >> 4`; the DSP arm at 0x1c747a programs 0x5430 and passes `size >> 2`.
 * The two differ in bit 3 alone, so that bit is the unit selector for every
 * transfer this board sees.  (The panel halves count bytes and the SD block
 * counts halfwords; both are recognised by their addresses before this applies.)
 */
#define CHCR_TS_WIDE    0x0008
#define DMA_BURST       16
#define DMA_BURST_LONG  4
#define DMA_CHUNK       4096

/*
 * The CCN block.  INTEVT and EXPEVT are CPU state, not bus registers, so they
 * have to be published deliberately: the interrupt entry at VBR+0x600 reads
 * INTEVT, and against a trap that answers zero it takes the spurious branch,
 * executes rte, re-enters on the still-asserted interrupt and live-locks.
 */
#define CCN_BASE        0xff000000
#define CCN_SIZE        0x40
#define CCN_PTEH        0x00
#define CCN_PTEL        0x04
#define CCN_TTB         0x08
#define CCN_TEA         0x0c
#define CCN_MMUCR       0x10
#define CCN_CCR         0x1c
#define CCN_TRA         0x20
#define CCN_EXPEVT      0x24
#define CCN_INTEVT      0x28

/*
 * The timer, and how its interrupt reaches the firmware.
 *
 * The RTOS programs TMU channel 0 the standard SH-4 way — TCOR/TCNT 0x34bb,
 * TCR 0x20 (underflow interrupt), TSTR bit 0 — and its interrupt entry at
 * VBR+0x600 dispatches on INTEVT: handler = [0x04fcd904 + (INTEVT >> 3)].
 *
 * The vector is **not** the architectural TUNI0 code 0x400.  This SoC routes
 * the TMU through its INTC2 as irq 0x2c, and INTEVT = irq * 0x20 = 0x580.
 * Read out of the live machine: at 0x400 the table holds the default stub
 * 0x2e67b0, which walks an empty handler list and returns without touching the
 * timer — so delivering 0x400 produces an interrupt nothing ever acknowledges.
 * The entry for 0x580 resolves to the ISR at image 0x260548, whose function
 * 0x2604da is the only registered handler that touches TSTR (0xffd80004) and
 * TCR0 (0xffd80010), i.e. the one that clears the underflow flag.
 *
 * 0x34bb + 1 counts at P-phi/4 per tick; a 1 ms tick (the firmware has a task
 * called Dummy1ms) puts P-phi at 54 MHz.
 */
#define TMU_BASE        0xffd80000
/*
 * A second three-channel timer block, TMU3..TMU5.  The application only
 * counts on it (TCOR3 is read for elapsed time), but the *loader* -- the
 * recovery image the boot ROM unpacks from flash 0x10000 when the packed
 * application's checksum fails -- has no use for TMU0 at all: its RTOS tick
 * is TMU4 (0x401dfac: TCOR4 0x34bc, TCR4 0x20, TSTR2 bit 1, the same 1 ms as
 * the application's TMU0) and TMU5 is a 10 ms companion (TCOR5 0x20f58).
 * The loader writes their levels into INT2PRI1[20:16] and [12:8] (both 4) and
 * clears INT2MSKR bit 1, the TMU3-5 group; their INTEVT codes are 0xe00,
 * 0xe20 and 0xe40 (SH7764 manual table 13.3).  Without those lines routed
 * the loader boots to its "Cente USBH-MSC sample program" banner, sees the
 * stick, and never issues the bus reset its attach handler schedules through
 * a delay (update-21): every dly_tsk waits on a tick that never comes.
 */
#define TMU2_BASE       0xffdc0000
#define TMU_FREQ        54000000
#define TMU0_IRQ        0x2c
#define TMU1_IRQ        0x2d
#define INTEVT_TMU0     (TMU0_IRQ * 0x20)   /* 0x580 */
#define INTEVT_TMU1     (TMU1_IRQ * 0x20)   /* 0x5a0 */
#define TMU3_IRQ        0x70
#define TMU4_IRQ        0x71
#define TMU5_IRQ        0x72
#define INTEVT_TMU3     (TMU3_IRQ * 0x20)   /* 0xe00 */
#define INTEVT_TMU4     (TMU4_IRQ * 0x20)   /* 0xe20 */
#define INTEVT_TMU5     (TMU5_IRQ * 0x20)   /* 0xe40 */

/*
 * The SoC's own interrupt controller is an SH7780-style INTC2 at 0xffd40000,
 * not an SH7750 INTC: the image writes USERIMASK at 0xffd30000 with the 0xA5
 * key (0x2603b6) and INT2PRI0-7 at 0xffd40000-1c, and contains no reference at
 * all to the SH7750 IPRA/IPRB/IPRC.
 *
 * The three INT2PRI registers the firmware actually programs are modelled
 * below, so the guest sets its own interrupt levels and the board cannot drift
 * out of step with the image.  It also gets the enable gating for free: the
 * boot zeroes INT2PRI0..7 at 0x2603b8 and only writes a level when it wants a
 * source live, and sh_intc treats a zero field as "not enabled", so a source
 * is offered only once the firmware has asked for it.
 *
 * The levels that come out of this, and where the firmware writes them:
 *
 *   TMU0, the tick   INT2PRI0[31:24]  8   0x2604f2-0x26050c
 *   SCIF receive     INT2PRI2[31:24]  4   0x10237c-0x102386
 *   GUI link arm     INT2PRI4[15:8]   4   0x2a39aa-0x2a39b4
 *   GUI link mode    INT2PRI4[7:0]    4   0x2a3a70-0x2a3a7e
 *   second channel   INT2PRI5[7:0]    4   0x2a3a48-0x2a3a50, 0x2a3b0c-0x2a3b1a
 *   TMU4/5, loader   INT2PRI1[20:16], [12:8]  4   0x401dfe6-0x401dffc (loader only)
 *
 * That ordering is what real hardware does and it matters: the tick outranks a
 * link ISR and preempts it.  With the levels the other way round a link ISR
 * runs its whole body at an IMASK that blocks the tick throughout, which
 * starves the RTOS clock under GUI load.
 *
 * Do not take the levels from the +16 word of the 20-byte registration
 * records: it says 4 for the tick and 15 for the links, disagrees with these
 * hardware writes, and the registrar 0x2ecd74 never reads it.
 *
 * The RTOS's own SR levels corroborate the range: tasks run at IMASK 0 (the
 * dispatcher force-clears it with & 0xffffff0f at 0x2e6f9a), the interrupt
 * prologue at 0x2e6426 installs IMASK 10 before it acknowledges anything so
 * that the tick cannot re-enter, and loc_cpu (0x2e6734) installs IMASK 15.
 * Devices belong in 1..8 and both kernel windows sit above them.  Measured
 * with the level hardcoded: 11, 14 and 15 reset the guest in about two
 * seconds, 10 and 8 survive.
 *
 * INT2MSKR/INT2MSKCR at 0xffd40038/3c are deliberately left on the catch-all
 * trap.  Their polarity is inverted relative to QEMU's intc_mask_reg — a set
 * bit means masked, and the boot writes 0xffffffff — so modelling them as
 * INTC_MODE_DUAL_SET would mask every source off.  Priority does the gating.
 */
enum {
    CDJ_INTC_UNUSED = 0,
    CDJ_INTC_TMU0,
    CDJ_INTC_TMU1,
    CDJ_INTC_LINK_RX,
    CDJ_INTC_LINK_TX,
    CDJ_INTC_LINK_DONE,
    CDJ_INTC_SCIF_RX,
    CDJ_INTC_SCIF_TX,
    CDJ_INTC_PANEL_RX,
    CDJ_INTC_PANEL_TX,
    CDJ_INTC_SDHI,
    CDJ_INTC_SDHI_DMA,
    CDJ_INTC_DSP_DMA,
    CDJ_INTC_DMA5,
    CDJ_INTC_DSP_EVENT,
    CDJ_INTC_ATA,
    CDJ_INTC_USB,
    CDJ_INTC_USB_DMA,
    CDJ_INTC_ETH,
    CDJ_INTC_TMU3,
    CDJ_INTC_TMU4,
    CDJ_INTC_TMU5,
    CDJ_INTC_NR_SOURCES,
};

/*
 * The SoC's USB module (the type-A socket; cdj2000_usbh.c) interrupts on
 * USBI, INTEVT 0xc60, level from INT2PRI12[15:8] at 0xffd400b0 (manual
 * section 13.3.9) -- the loader's usbh_load (0x0400b2c8) writes 0x800 there
 * and unmasks bit 17 of INT2MSKCR1 (0xffd400d4).  Its bulk transfers go
 * through DMAC channel 0 (block 0xff608020), whose completion is DMINT0,
 * INTEVT 0x640, level from INT2PRI3[23:16] (0x0400b47c writes 0x40000 to
 * 0xffd4000c); the driver's vector record at 0xa400231c says irq 0x32,
 * handler 0x0400b34e, level 4.
 */
#define USB_IRQ         0x63
#define INTEVT_USB      (USB_IRQ * 0x20)        /* 0xc60 */
#define USB_DMA_IRQ     0x32
#define INTEVT_USB_DMA  (USB_DMA_IRQ * 0x20)    /* 0x640 */

/*
 * The PANEL board.
 *
 * The panel is an M16C part on a second SCIF at 0xffe20000, and MAIN does not
 * touch that UART's data registers itself — two DMAC channels do.  `M16C_Task`
 * (0x2473f8) calls the arm function 0x246e36, which programs both halves of one
 * full-duplex exchange and then starts them (0x246f1e..0x246f76):
 *
 *   0xff608030 (ch3)  SAR = 0xffe20014 (SCFRDR), DAR = buffer, TCR = length
 *   0xff608040 (ch4)  SAR = buffer, DAR = 0xffe2000c (SCFTDR), TCR = length
 *   0xff608060        DMAOR |= 1, then CHCR |= 4 (IE) and |= 1 (DE) on each
 *   0xffd4003c        |= 0x100, unmasking the channel in INTC2
 *
 * so the channel roles are self-describing: whichever half points at SCFRDR is
 * the receive.  The completions arrive as the DMAC's own vectors — read out of
 * the RTOS table at 0x04fcd904, the six live DMAC entries are INTEVT 0x640,
 * 0x660, 0x680, 0x6a0, 0x780 and 0x7a0, i.e. channels 0..5 in order, which puts
 * ch3 on 0x6a0 and ch4 on 0x780.
 *
 * `FUN_a428fdbc` passes length 0x18, and the frame format is fixed by the
 * validator 0x28cdf8, which is short enough to read exactly: 24 bytes, of which
 * 0..21 are payload copied on to 0x04fe29fc, byte 22 is the sum of those with
 * an end-around carry into the low byte, and byte 23 must be 0x8f.  A frame
 * that passes makes `PnlCom_RcvTASK` call 0x28ceb8, which is the only writer of
 * the panel state at 0x04fe29f4 — the word `PnlCom_SndTASK` needs non-zero
 * before it will publish MAIN's operating mode from inside its handshake
 * instead of from its ten-second fallback.
 *
 * The vectors are not a guess: the M16C task's resource table at 0x0009dfbc
 * registers handler 0x04246f88 on vectors 0x33 and 0x34 at level 4, so DMAC
 * channel n takes vector 48 + n and the two halves land on INTEVT 0x660 and
 * 0x680.  That handler is shared, and it decides which channel it is looking at
 * from a second INTC2 status word at 0xffd4004c — bit 1 for the receive channel
 * and bit 2 for the transmit (0x246f96, 0x246fa2).  Unmodelled it reads zero,
 * so the handler returns without acknowledging anything and the level stays
 * asserted; the status word is therefore as much a part of the device as the
 * channels are.  It is computed rather than stored: a completion that the guest
 * has not yet cleared out of CHCR is exactly what "pending" means.
 */
#define PANEL_SCIF_BASE  0xffe20000
#define PANEL_SCIF_SIZE  0x100
#define PANEL_SCIF_TDR   (PANEL_SCIF_BASE + 0x0c)
#define PANEL_SCIF_RDR   (PANEL_SCIF_BASE + 0x14)
#define PANEL_FRAME_LEN  24
#define CDJ_LINK_RX_QUEUE_MAX 64
#define PANEL_FRAME_MARK 0x8f
#define PANEL_DMA_RX_IRQ 0x33                       /* INTEVT 0x660, ch3 */
#define PANEL_DMA_TX_IRQ 0x34                       /* INTEVT 0x680, ch4 */
/*
 * Channel 5 is the audio path's channel: tsk_DJcontTxDspPCM fills the DSP's
 * PCM buffer with it (0x1c212a programs SAR/DAR/TCR and sets DE|IE, run
 * trackload-32: ch5 SAR 0xa450baf0 DAR 0xac0c81e0 TCR 0x930) and then sleeps
 * (tslp_tsk, 0x1c190a..0x1c1912) until the channel's completion interrupt wakes
 * it.  The DJcont init at 0x1c119c registers that handler, 0x1c2158, for irq
 * 0x35 with the 20-byte record at 0xa40668fc, so the vector is the channel's
 * own -- irq 0x30 + n like the panel channels -- and not the DSP boot
 * channel's 0x3d.  Before this was modelled every window-bound transfer raised
 * 0x7a0: the boot download's handler (0x1c7ba2) ran 300 times against a level
 * it never cleared, the sender was never woken, and the first PCM buffer was
 * the last thing MAIN gave the DSP.
 */
#define DMA5_IRQ         0x35                       /* INTEVT 0x6a0, ch5 */
#define DMA5_CHANNEL     5
#define INTC2_DMA_STATUS 0xffd4004c
#define INTC2_DMA_RX     0x0002
#define INTC2_DMA_TX     0x0004
#define INTEVT_PANEL_RX  (PANEL_DMA_RX_IRQ * 0x20)
#define INTEVT_PANEL_TX  (PANEL_DMA_TX_IRQ * 0x20)
#define INTEVT_DMA5      (DMA5_IRQ * 0x20)
#define PANEL_PRIO       4

/*
 * The board-to-board link.
 *
 * 0xff401000 and 0xff501000 are two instances of one controller — the same
 * three driver functions serve both — carrying the GUI board and the PANEL
 * board respectively, matching MAIN's GuiCom and PnlCom task pairs.
 *
 * The register map is read off the driver, and every field was then seen in a
 * live boot trace at the same value:
 *
 *   +0x018  buffer address (a P1/P2 pointer; the top three bits are not part
 *           of the address)
 *   +0x020  bits 31:4 = frame length in 16-byte units
 *   +0x028  control; bit 0 starts the transfer, and the ISR clears it on the
 *           transmit-complete path
 *   +0x040  16
 *   +0x050  frame length in 16-byte units
 *   +0x188  interrupt status, acknowledged by writing back (value & 31).
 *           Bit 2 selects which completion this is: clear = receive, set =
 *           transmit.  Bits 2..4 together mean "there is work"; the handler
 *           returns immediately when all three are clear, and loops back to
 *           re-read this register until they are.
 *   +0x190  enable; the init sequence is
 *           &= ~0x10, &= ~0x08, &= ~0x04, |= 0x02, |= 0x01
 *
 * Interrupts arrive through the INTC2 status word at 0xffd40050 — bit 0 for
 * the GUI channel, bit 5 for the PANEL channel — and the CPU vector is
 * INTEVT = irq * 0x20 with irq 0x50 for GUI and 0x55 for PANEL.  Those irq
 * numbers come from the {irq, handler, priority} table at image 0xa1528:
 * 0x50 -> 0x2a3f4c (the arm that tests bit 0), 0x55 -> 0x2a3db0 (bit 5).
 */
#define LINK_RX_BASE   0xff401000
#define LINK_TX_BASE 0xff501000
#define LINK_SIZE       0x200
#define LINK_MODE_SIZE  0x100
/*
 * Each channel block holds two descriptors and the two links use different
 * ones: the GUI arm 0x2a3930 writes +0x18/+0x20, the panel arm 0x2a39be
 * writes +0x08/+0x10.  Same encoding either way — the length field carries the
 * frame size in 16-byte units in bits 31:4.
 */
#define LINK_RX_BUFFER   0x018
#define LINK_RX_LENGTH   0x020
#define LINK_TX_BUFFER 0x008
#define LINK_TX_LENGTH 0x010
#define LINK_CONTROL    0x028
#define LINK_R40        0x040
#define LINK_R50        0x050
#define LINK_STATUS     0x188
#define LINK_ENABLE     0x190
#define LINK_CTRL_START 0x0001
/*
 * Bit 2 is the completion flag.  Note the branch senses in the handler: it
 * proceeds only when (status & 0x1c) is non-zero, and `tst #4,r0; bf` takes the
 * branch when bit 2 is *set* — that arm is the one which re-arms the 48-byte
 * receive, which is what identifies it.  Overridable while the remaining bits
 * are still being pinned down.
 */
#define LINK_ST_COMPLETE 0x0004

/*
 * A link status halfword in the otherwise unidentified 0xfff10000 block.
 * Bit 2 means "a received frame is pending", and both sides of the protocol
 * are gated on it:
 *
 *   0x2134cc  GuiCom_RcvTASK calls the request verifier 0x2a423e only when the
 *             bit is SET — with the bit clear it branches to 0x213594 and drops
 *             the frame without looking at it;
 *   0x215268  GuiCom_SndTASK sets its ready word 0x489bcf4 only when the bit is
 *             CLEAR, which is why it comes up ready before any frame arrives.
 *
 * It is a plain read/write register — 0x246e36, 0x248dc2 and 0x251200 all
 * read-modify-write it — so a trap that always reads zero silently discards
 * every incoming frame.
 */
#define SOC_BLOCK_BASE  0xfff10000
#define SOC_BLOCK_SIZE  0x1000
#define LINK_FLAG_REG   0xfff10048
#define LINK_FLAG_RX    0x0004
/*
 * PnlCom_SndTASK decides MAIN's operating mode from bit 1 of this register
 * (read at 0x28fbb0 through the pointer it stashes at task entry): set gives
 * mode 1, clear gives mode 2.  Only modes 1, 3 and 4 make GuiCom_SndTASK serve
 * the GUI, so on a machine with no panel board attached MAIN would sit in mode
 * 2 forever.  The bit reads as set because a CDJ always has its panel.
 */
#define PANEL_PRESENT_REG 0x0060
#define PANEL_PRESENT_BIT 0x0002
/*
 * Bit 4 of the same register is the sense line of the USB power switch.
 * MAIN polls it from 0x042918c0 (counter at 0x04fe34d4) and after 50 reads
 * with the bit low calls the message function 0x04250e44 with (0x92, 5000):
 * the caution the NXS GUI shows as "USB Error. Remove the device." (the
 * stock 4.200 GUI has no table entry for 0x92 and stays silent).  A register
 * file answers 0 there, so the poller fired for the whole run
 * (runs/nxs-swap/main-watch146: 0x053560b8 written 0x92 from 0x042c0082
 * without pause).  Holding the bit high from outside with
 * CDJ_MAIN_POKE=0xfff10060=0x10 gave message code 0 in all 14 486 status
 * records of twoboard-6, so the bit reads as set, like the panel bit;
 * CDJ_NO_USB_POWER=1 restores the register-file reading for an A/B.
 */
#define USB_POWER_SENSE_BIT 0x0010

#define INTC2_STATUS    0xffd40050
#define INTC2_STATUS_SIZE       0x10
#define INTC2_STATUS2_OFFSET    0x0c        /* 0xffd4005c */
#define INTC2_DSP_EVENT_BIT     (1u << 24)  /* tested by the DSP stub 0x26260c */
#define LINK_RX_IRQ    0x50
#define LINK_TX_IRQ  0x55
#define INTEVT_LINK_RX   (LINK_RX_IRQ * 0x20)     /* 0xa00 */
#define INTEVT_LINK_TX (LINK_TX_IRQ * 0x20)   /* 0xaa0 */
/*
 * Transmit completion is a third interrupt, not the TX channel's own.  Handler
 * 0x2a3eb4 (irq 0x56) tests INTC2 bit 6, reads its status from 0xff502004 and
 * clears the transmit-in-progress flag 0x7db3541 only when bit 25 is set in
 * *both* 0xff502004 and 0xff502000 (0x2a3f00..0x2a3f14).  Until that flag
 * clears, the next received frame takes the error path at 0x213496 and MAIN
 * never sends again — which is exactly the one-shot transmit seen before.
 */
#define LINK_DONE_IRQ   0x56
#define INTEVT_LINK_DONE (LINK_DONE_IRQ * 0x20)  /* 0xac0 */
/*
 * The console SCIF's receive interrupt.  The ISR is 0x102468: it tests FSR for
 * RDF or DR, reads RDR, and on CR wakes the task that feeds MAIN's monitor.
 */
#define SCIF_RX_IRQ     0x39
#define INTEVT_SCIF_RX  (SCIF_RX_IRQ * 0x20)     /* 0x720 */
/*
 * ...and its transmit interrupt, ISR 0x10274c, which wakes the task that
 * drains the output queue and then clears TIE.
 */
#define SCIF_TX_IRQ     0x3b
#define INTEVT_SCIF_TX  (SCIF_TX_IRQ * 0x20)     /* 0x760 */
/*
 * Both SCIF interrupts take their level from the one INT2PRI2 field the driver
 * programs, but sh_intc's priority registers carry a single source per field,
 * so only the receive half can sit in the table.  The transmit half is given
 * the same level here and left permanently enabled at the controller — the
 * guest's own TIE bit is the real gate, and it clears it in the handler.
 */
#define SCIF_PRIO       4
#define LINK_DONE_BIT   (1u << 6)
#define MODE_STATUS     0x04
#define MODE_DONE       0x08000000
#define MODE_GATE       0x02000000
/* The link levels come from INT2PRI4/5; see the INTC2 comment above. */

/* Reading the same register this many times running means a stuck poll. */
#define SPIN_REPORT     4096

/*
 * CDJ_LINK_TRACE: log every link arm and acknowledge.  Off by default: at
 * ~300 exchanges a second those two lines were 43 000 log writes per five
 * minutes and said nothing the "sent"/"delivered" lines do not.
 */
static bool cdj_sdhi_trace(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("CDJ_SDHI_TRACE");
        enabled = env && *env;
    }
    return enabled;
}

static bool cdj_link_trace(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("CDJ_LINK_TRACE");
        enabled = env && *env;
    }
    return enabled;
}

typedef struct {
    MemoryRegion trap;
    MemoryRegion trap_p4;

    /* Last address read, and how often in a row — cheap stuck-poll detector. */
    hwaddr last_read;
    uint64_t repeats;
    bool reported;

    /*
     * The same for writes.  INT2MSKCR (0xffd4003c) is written on every ISR
     * exit, and logging each one cost 150 000 lines -- three quarters of the
     * -D log -- in a five-minute run, each line a mutex, a vfprintf and an
     * fflush.  The first write to an address is logged, a run of repeats is
     * reported once with its count when the address changes.
     */
    hwaddr last_write;
    uint64_t last_write_value;
    uint64_t write_repeats;

    uint64_t default_value;
} CdjPeriphState;

/*
 * Channels are 16-byte blocks of SAR/DAR/TCR/CHCR.  DMAOR sits at 0x60, in the
 * middle of what would otherwise be channel 6, so that block is not a channel.
 * The boot DMA uses 0x80 and the panel uses 0x30 and 0x40.
 */
enum {
    CDJ_DMA_PLAIN = 0,          /* memory to memory, e.g. the boot copy */
    CDJ_DMA_PANEL_RX,
    CDJ_DMA_PANEL_TX,
    CDJ_DMA_DSP,                /* one end is the audio DSP's shared window */
    CDJ_DMA_USBH,               /* one end is a USB module FIFO burst port */
};

typedef struct {
    uint32_t sar;
    uint32_t dar;
    uint32_t tcr;
    uint32_t chcr;
    /*
     * Taken from the addresses when the channel is started and kept, because
     * completion rewrites them: a transfer to or from a device register leaves
     * that side fixed and advances the other, so testing the registers again
     * afterwards no longer identifies the channel.
     */
    int role;
    bool armed;                 /* panel channels only: waiting on the timer */
} CdjDmacChannel;

typedef struct {
    MemoryRegion iomem;
    MemoryRegion iomem_p4;
    MemoryRegion intc2_status;

    uint32_t dmaor;
    CdjDmacChannel channel[DMAC_CHANNELS];

    /* The panel side: see the PANEL comment above. */
    qemu_irq panel_rx_irq;
    /*
     * A completion is raised on the vector of the channel it ran on, DMINT
     * (index - 2), not on a vector fixed per role: the application runs the
     * panel receive on index 3 and its transmit on index 4 (0x660, 0x680),
     * the loader on 4 and 5 (0x680, 0x6a0) -- and with the vectors fixed the
     * loader's receive completion arrived on 0x660, where it has no handler,
     * and the unacknowledged level re-entered the empty stub forever
     * (update-22).  channel_irq[] is that mapping; the *_line fields
     * remember which line a pending completion is holding up.
     */
    qemu_irq channel_irq[DMAC_CHANNELS];
    qemu_irq panel_rx_line;
    qemu_irq panel_tx_line;
    unsigned panel_rx_index;
    unsigned panel_tx_index;
    qemu_irq sdhi_dma_irq;
    bool sdhi_dma_pending;
    qemu_irq dsp_dma_irq;
    bool dsp_dma_pending;
    qemu_irq dma5_irq;                  /* channel 5's own completion vector */
    bool dma5_pending;
    qemu_irq usbh_dma_irq;              /* DMINT0, the USB driver's channel */
    bool usbh_dma_pending;
    unsigned usbh_dma_channel;
    qemu_irq panel_tx_irq;
    QEMUTimer *panel_timer;
    bool panel_present;
    bool panel_rx_pending;
    bool panel_tx_pending;
    uint64_t panel_exchanges;
    uint8_t panel_reply[PANEL_FRAME_LEN];
} CdjDmacState;

typedef struct {
    SuperHCPU *cpu;
    uint32_t vector;
} CdjResetState;

/*
 * The firmware hands the DMAC P1/P2 addresses (0xa0040000, 0xa7a00000); the
 * hardware sees physical ones.  On SH-4 the top three bits select the window
 * and are not part of the address.
 */
static inline hwaddr cdj_dma_phys(uint32_t address)
{
    return address & 0x1fffffff;
}

/*
 * The SD block transfer is DMAC channel 8 reading the SDHI's data port.  The
 * addresses are not in any literal pool -- 0x1ff042 loads them from a table at
 * 0x07d941e8 + 0x8c, which is why neither a disassembly grep nor the SDHI's own
 * register window shows the engine:
 *
 *   +0x8c 0xff608080 SAR   +0x90 0xff608084 DAR
 *   +0x94 0xff608088 TCR   +0x98 0xff60808c CHCR
 *
 * Three things differ from a memory-to-memory copy and each one alone produces
 * a silent, wrong transfer: the source is a FIFO at its *P4* address, so it must
 * neither be masked by cdj_dma_phys() nor advance; and TCR counts 2-byte units
 * here (0x100 for a 512-byte block), not the boot channel's 16-byte bursts.
 */
#define SDHI_DATA_PORT_P4 0xffe40030
static bool cdj_sdhi_dma_read(unsigned nr_bytes, uint8_t *buffer);
static bool cdj_sdhi_dma_write(unsigned nr_bytes, const uint8_t *buffer);

/*
 * A key press.  The button handlers are rising-edge detectors -- 0x28ddc8
 * compares the live status byte against the copy 44 bytes further on and acts
 * only on a bit that is set now and was clear before -- so a key has to go down
 * and come back up.  CDJ_PANEL_KEYS is a semicolon-separated list of
 *
 *     <seconds>:<payload byte>:<hex mask>
 *
 * against the virtual clock, e.g. "35:19:04" for the SD SOURCE key at 35 s
 * (payload byte 19 bit 2, from the decoder at 0x28e44a; bit 1 is USB, the
 * firmware's own name table in tools/cdj_main/panel_control.py).  CDJ_PANEL_HOLD_MS
 * sets how long each stays down; the default is long enough for several frames.
 */
#define PANEL_KEYS_MAX 16

typedef struct CdjPanelKey {
    int64_t at_ns;
    unsigned byte;
    uint8_t mask;
} CdjPanelKey;

static CdjPanelKey cdj_panel_keys[PANEL_KEYS_MAX];
static int cdj_panel_nr_keys = -1;
static int64_t cdj_panel_hold_ns;

static void cdj_panel_keys_parse(void)
{
    const char *spec = getenv("CDJ_PANEL_KEYS");
    const char *hold = getenv("CDJ_PANEL_HOLD_MS");

    cdj_panel_nr_keys = 0;
    cdj_panel_hold_ns = (hold ? strtoll(hold, NULL, 10) : 300) * 1000000LL;
    while (spec && *spec && cdj_panel_nr_keys < PANEL_KEYS_MAX) {
        CdjPanelKey *key = &cdj_panel_keys[cdj_panel_nr_keys];
        char *end;
        double seconds = strtod(spec, &end);

        if (end == spec || *end != ':') {
            break;
        }
        key->byte = strtoul(end + 1, &end, 10);
        if (*end != ':') {
            break;
        }
        key->mask = strtoul(end + 1, &end, 16);
        key->at_ns = (int64_t)(seconds * 1000000000.0);
        if (key->byte < PANEL_FRAME_LEN - 2u) {
            info_report("cdj2000: panel key byte %u mask %#x at %.2f s",
                        key->byte, key->mask, seconds);
            cdj_panel_nr_keys++;
        }
        if (*end != ';') {
            break;
        }
        spec = end + 1;
    }
}

/*
 * The panel's reply.  Byte 22 is the sum of 0..21 with an end-around carry into
 * the low byte and byte 23 the 0x8f marker, exactly as 0x28cdf8 checks; an
 * all-zero payload therefore checksums to zero.  CDJ_PANEL_FRAME overrides the
 * 22 payload bytes as hex, for driving the panel state machine by hand.
 */
static void cdj_panel_frame(uint8_t *frame)
{
    static const char *spec;
    static bool spec_read;
    unsigned sum = 0;
    int64_t now;
    int i;

    if (!spec_read) {
        spec = getenv("CDJ_PANEL_FRAME");
        spec_read = true;
    }
    memset(frame, 0, PANEL_FRAME_LEN);
    for (i = 0; spec && i < PANEL_FRAME_LEN - 2; i++) {
        char digits[3] = { spec[2 * i], spec[2 * i + 1], 0 };
        if (!digits[0] || !digits[1]) {
            break;
        }
        frame[i] = strtoul(digits, NULL, 16);
    }
    if (cdj_panel_nr_keys < 0) {
        cdj_panel_keys_parse();
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (i = 0; i < cdj_panel_nr_keys; i++) {
        const CdjPanelKey *key = &cdj_panel_keys[i];

        if (now >= key->at_ns && now < key->at_ns + cdj_panel_hold_ns) {
            frame[key->byte] |= key->mask;
        }
    }
    /*
     * Whatever is driving the panel from outside gets the last word, and it
     * has to land here: before the checksum, so the frame still validates, and
     * after the scheduled keys, so a live press is not undone by the schedule.
     */
    cdj_input_apply(frame, PANEL_FRAME_LEN - 2);
    for (i = 0; i < PANEL_FRAME_LEN - 2; i++) {
        sum += frame[i];
        if (sum & 0xff00) {
            sum = (sum + 1) & 0xff;
        }
    }
    frame[PANEL_FRAME_LEN - 2] = sum;
    frame[PANEL_FRAME_LEN - 1] = PANEL_FRAME_MARK;
}

static void cdj_dmac_complete(CdjDmacChannel *channel, hwaddr source,
                              hwaddr destination)
{
    /* Registers report the finished transfer, as the hardware would. */
    channel->sar = source;
    channel->dar = destination;
    channel->tcr = 0;
    channel->chcr = (channel->chcr & ~CHCR_DE) | CHCR_TE;
}

/*
 * A channel that reads the panel UART's receive register or writes its transmit
 * register is one half of a panel exchange, and its TCR counts bytes rather
 * than the boot channel's 16-byte bursts.  Roles are taken from the addresses,
 * not from the channel number, because that is what the firmware programs.
 */
static int cdj_dmac_role(const CdjDmacChannel *channel)
{
    if (channel->sar == PANEL_SCIF_RDR) {
        return CDJ_DMA_PANEL_RX;
    }
    if (channel->dar == PANEL_SCIF_TDR) {
        return CDJ_DMA_PANEL_TX;
    }
    /*
     * The DSP shares the boot channel (offset 0x80) with the SD block reads,
     * so the endpoint is what tells them apart — the same rule as the panel.
     * A DSP transfer is an ordinary memory copy once it is recognised; what it
     * needs on top is the busy bit, the completion interrupt and telling the
     * model that its window changed.
     */
    if (cdj_dsp_is_window(cdj_dma_phys(channel->sar))
        || cdj_dsp_is_window(cdj_dma_phys(channel->dar))) {
        return CDJ_DMA_DSP;
    }
    return CDJ_DMA_PLAIN;
}

/*
 * The panel exchange finishes later, not in the store that starts it.  0x246e36
 * arms the transmit half first and the receive half four instructions later, so
 * completing the transmit synchronously delivers its interrupt in the middle of
 * the arm: the receive channel is then never started, no reply can arrive, and
 * the machine stops making progress with the tick dead.  A short virtual-time
 * delay — a real 24-byte UART exchange is far longer — puts both completions
 * after the arm and keeps the ordering the firmware expects.
 */
#define PANEL_XFER_NS 500000

/*
 * CDJ_PANEL_TX_TRACE: MAIN's frame to the panel (channel 4, the buffer at
 * 0xa4501018 to SCFTDR 0xffe2000c) every time it differs from the last one,
 * with the guest time -- the LEDs and the jog ring's display are in it, and a
 * map of them needs the frames next to what the deck was doing.  Byte count
 * as the channel's TCR says, at most 64.
 */
static void cdj_panel_tx_trace(CdjDmacChannel *channel)
{
    static int enabled = -1;
    static uint8_t last[64];
    static unsigned last_len;
    uint8_t frame[64];
    unsigned len = channel->tcr < sizeof(frame) ? channel->tcr : sizeof(frame);
    unsigned i;

    if (enabled < 0) {
        enabled = getenv("CDJ_PANEL_TX_TRACE") != NULL;
    }
    if (!enabled || !len) {
        return;
    }
    address_space_read(&address_space_memory, cdj_dma_phys(channel->sar),
                       MEMTXATTRS_UNSPECIFIED, frame, len);
    if (len == last_len && !memcmp(frame, last, len)) {
        return;
    }
    memcpy(last, frame, len);
    last_len = len;
    fprintf(stderr, "cdj2000-panel-tx t=%.6f %u bytes from %#010x:",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, len, channel->sar);
    for (i = 0; i < len; i++) {
        fprintf(stderr, " %02x", frame[i]);
    }
    fprintf(stderr, "\n");
}

static void cdj_dmac_panel_done(void *opaque)
{
    CdjDmacState *dmac = opaque;
    unsigned index;

    /* Transmit first: the panel answers a poll, it does not speak unasked. */
    for (index = 0; index < DMAC_CHANNELS; index++) {
        CdjDmacChannel *channel = &dmac->channel[index];

        if (!channel->armed || channel->role != CDJ_DMA_PANEL_TX) {
            continue;
        }
        channel->armed = false;
        cdj_panel_tx_trace(channel);
        /* The device side is a fixed register; only the memory side advances. */
        cdj_dmac_complete(channel, channel->sar + channel->tcr, channel->dar);
        /* The request is TE && IE: a channel run without IE completes quietly. */
        if (channel->chcr & CHCR_IE) {
            dmac->panel_tx_pending = true;
            dmac->panel_tx_index = index;
            dmac->panel_tx_line = dmac->channel_irq[index];
            if (dmac->panel_tx_line) {
                qemu_set_irq(dmac->panel_tx_line, 1);
            }
        }
    }
    for (index = 0; index < DMAC_CHANNELS; index++) {
        CdjDmacChannel *channel = &dmac->channel[index];

        if (!channel->armed || channel->role != CDJ_DMA_PANEL_RX) {
            continue;
        }
        /*
         * CDJ_NO_PANEL leaves the receive half armed and silent, which is what
         * a machine with no panel board fitted actually does — the reference
         * behaviour this device was built to replace.
         */
        if (!dmac->panel_present) {
            continue;
        }
        channel->armed = false;
        cdj_panel_frame(dmac->panel_reply);
        address_space_write(&address_space_memory, cdj_dma_phys(channel->dar),
                            MEMTXATTRS_UNSPECIFIED, dmac->panel_reply,
                            MIN(channel->tcr, PANEL_FRAME_LEN));
        if (dmac->panel_exchanges++ < 4) {
            qemu_log_mask(LOG_UNIMP, "cdj2000-panel: replied %u bytes to "
                          "0x%08x (exchange %" PRIu64 ")\n", channel->tcr,
                          channel->dar, dmac->panel_exchanges);
        }
        cdj_dmac_complete(channel, channel->sar, channel->dar + channel->tcr);
        if (channel->chcr & CHCR_IE) {
            dmac->panel_rx_pending = true;
            dmac->panel_rx_index = index;
            dmac->panel_rx_line = dmac->channel_irq[index];
            if (dmac->panel_rx_line) {
                qemu_set_irq(dmac->panel_rx_line, 1);
            }
        }
    }
}

static void cdj_dmac_dsp_irq(CdjDmacState *dmac, unsigned index)
{
    /* Both legacy shared-window and NXS UHPI transfers use the SH DMAC's
     * completion interrupt. The PCM ISR clears IE, leaving TE for rearm. */
    if (!(dmac->channel[index].chcr & CHCR_IE)) {
        return;
    }
    if (index == DMA5_CHANNEL) {
        if (dmac->dma5_irq && !dmac->dma5_pending) {
            dmac->dma5_pending = true;
            qemu_set_irq(dmac->dma5_irq, 1);
        }
    } else if (dmac->dsp_dma_irq && !dmac->dsp_dma_pending) {
        dmac->dsp_dma_pending = true;
        qemu_set_irq(dmac->dsp_dma_irq, 1);
    }
}

static void cdj_dmac_run(CdjDmacState *dmac, unsigned index)
{
    CdjDmacChannel *channel = &dmac->channel[index];
    hwaddr source = cdj_dma_phys(channel->sar);
    hwaddr destination = cdj_dma_phys(channel->dar);
    uint8_t buffer[DMA_CHUNK];
    uint64_t remaining;
    unsigned unit = (channel->chcr & CHCR_TS_WIDE)
        ? DMA_BURST : DMA_BURST_LONG;
    unsigned source_mode = (channel->chcr >> 12) & 3;
    unsigned destination_mode = (channel->chcr >> 14) & 3;

    if (cdj_nxs_hpi_port(source) || cdj_nxs_hpi_port(destination)) {
        /* UHPI data access is one 32-bit bus cycle; the DSP address advances
         * inside HPID. The host-side fixed register address must not move. */
        unsigned sm = (channel->chcr >> 12) & 3;
        unsigned dm = (channel->chcr >> 14) & 3;
        unsigned ts = ((channel->chcr >> 3) & 3) | ((channel->chcr >> 18) & 4);
        if (ts != 2 || sm > 1 || dm > 1 || !channel->tcr) {
            error_report("nxs-hpi: unsupported DMA CHCR=%#x TCR=%#x", channel->chcr, channel->tcr);
            return;
        }
        channel->role = CDJ_DMA_DSP;
        for (uint32_t word = 0; word < channel->tcr; ++word) {
            address_space_read(&address_space_memory, source, MEMTXATTRS_UNSPECIFIED, buffer, 4);
            address_space_write(&address_space_memory, destination, MEMTXATTRS_UNSPECIFIED, buffer, 4);
            source += sm ? 4 : 0;
            destination += dm ? 4 : 0;
        }
        cdj_dmac_complete(channel, source, destination);
        cdj_dmac_dsp_irq(dmac, index);
        return;
    }
    channel->role = cdj_dmac_role(channel);
    if (getenv("CDJ_DMAC_TRACE")) {
        fprintf(stderr, "cdj2000-dmac %.3f: ch%u SAR %#010x DAR %#010x TCR %#x "
                        "CHCR %#010x role %d\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, index, channel->sar,
                channel->dar, channel->tcr, channel->chcr, channel->role);
    }
    if (channel->role == CDJ_DMA_PANEL_RX || channel->role == CDJ_DMA_PANEL_TX) {
        channel->armed = true;
        timer_mod(dmac->panel_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PANEL_XFER_NS);
        return;
    }

    /*
     * The USB module's FIFO burst ports.  The Cente driver programs the
     * channel before it points the FIFO at a pipe (0x0400b3a0, then
     * 0x0400c81a), so the copy cannot run here: the module does it as packets
     * arrive and reports back through cdj_dmac_usbh_done.  TCR counts 32-byte
     * units on this channel (the driver passes length >> 5).
     */
    if ((source & ~0xffull) == CDJ_USBH_BASE + 0x100
        || (destination & ~0xffull) == CDJ_USBH_BASE + 0x100) {
        bool to_fifo = (destination & ~0xffull) == CDJ_USBH_BASE + 0x100;
        hwaddr port = to_fifo ? destination : source;

        channel->role = CDJ_DMA_USBH;
        dmac->usbh_dma_channel = index;
        if (!cdj_usbh_dma_start(port - CDJ_USBH_BASE, to_fifo,
                                to_fifo ? source : destination,
                                channel->tcr * 32, index)) {
            warn_report_once("cdj2000-dmac: ch%u aims at the USB module's "
                             "FIFO but no module is present", index);
        }
        return;
    }

    if (channel->sar == SDHI_DATA_PORT_P4 || channel->dar == SDHI_DATA_PORT_P4) {
        bool to_card = channel->dar == SDHI_DATA_PORT_P4;
        unsigned nr_bytes = channel->tcr * 2;
        uint8_t *buffer = g_malloc(nr_bytes);

        if (to_card) {
            address_space_read(&address_space_memory, source,
                               MEMTXATTRS_UNSPECIFIED, buffer, nr_bytes);
            cdj_sdhi_dma_write(nr_bytes, buffer);
        } else if (cdj_sdhi_dma_read(nr_bytes, buffer)) {
            address_space_write(&address_space_memory, destination,
                                MEMTXATTRS_UNSPECIFIED, buffer, nr_bytes);
        }
        g_free(buffer);
        /* The device side of a transfer stays put; only the memory side moves. */
        if (to_card) {
            cdj_dmac_complete(channel, source + nr_bytes, channel->dar);
        } else {
            cdj_dmac_complete(channel, channel->sar, destination + nr_bytes);
        }
        /*
         * A level, not a pulse: the handler acknowledges by clearing TE, the
         * same contract the panel channels already use.  Left asserted it would
         * be re-entered forever; pulsed it would be missed.
         */
        if (dmac->sdhi_dma_irq && !dmac->sdhi_dma_pending) {
            dmac->sdhi_dma_pending = true;
            qemu_set_irq(dmac->sdhi_dma_irq, 1);
        }
        return;
    }

    remaining = (uint64_t)channel->tcr * unit;
    if (channel->role == CDJ_DMA_DSP) {
        cdj_dsp_transfer_start();
    }
    while (remaining) {
        size_t chunk = remaining < DMA_CHUNK ? remaining : DMA_CHUNK;

        /* SM/DM: 0 = fixed, 1 = increment, 2 = decrement.  In particular,
         * NXS startup's 0x4431 transfer clears BSS from a fixed zero word.
         * Treating that as memcpy reads past the zero and corrupts the UDP
         * table; lnkfrm then busy-loops and starves PnlCom_RcvTASK (E-7022).
         * Keep the bulk path for incrementing copies, but execute other
         * address modes one transfer unit at a time. */
        if (source_mode != 1 || destination_mode != 1) {
            chunk = unit;
        }
        address_space_read(&address_space_memory, source,
                           MEMTXATTRS_UNSPECIFIED, buffer, chunk);
        address_space_write(&address_space_memory, destination,
                            MEMTXATTRS_UNSPECIFIED, buffer, chunk);
        if (source_mode == 1) {
            source += chunk;
        } else if (source_mode == 2) {
            source -= chunk;
        }
        if (destination_mode == 1) {
            destination += chunk;
        } else if (destination_mode == 2) {
            destination -= chunk;
        }
        remaining -= chunk;
    }
    if (channel->role == CDJ_DMA_DSP) {
        cdj_dsp_transfer_done(cdj_dma_phys(channel->sar),
                              cdj_dma_phys(channel->dar),
                              destination - cdj_dma_phys(channel->dar));
        /*
         * A level, cleared when the guest clears TE — the contract the panel
         * and SD channels already use.  The DSP's own handler (0x1c7ba2 ->
         * 0x1c7b6e) clears CHCR bits 2, 0 and 1 in that order and then sets
         * cflgDspDmaEnd, so it acknowledges exactly the same way.
         *
         * Which vector is the channel's, not the endpoint's: the boot download
         * runs on channel 8 and its handler is registered for irq 0x3d, the
         * PCM sender runs on channel 5 and its wake-up handler for irq 0x35
         * (see DMA5_IRQ).  A channel-5 completion delivered on 0x7a0 storms the
         * boot handler and leaves the sender asleep for ever.
         */
    }
    cdj_dmac_complete(channel, source, destination);
    if (channel->role == CDJ_DMA_DSP) {
        cdj_dmac_dsp_irq(dmac, index);
    }
}

/* DMAOR occupies the block a seventh channel would have used. */
static inline bool cdj_dmac_channel_of(hwaddr offset, unsigned *index)
{
    if (offset >= DMAC_CHANNELS * 16 || (offset & ~0xfu) == DMAC_DMAOR) {
        return false;
    }
    *index = offset >> 4;
    return true;
}

static uint64_t cdj_dmac_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjDmacState *dmac = opaque;
    unsigned index;

    if (offset == DMAC_DMAOR) {
        return dmac->dmaor;
    }
    if (cdj_dmac_channel_of(offset, &index)) {
        switch (offset & 0xf) {
        case DMAC_CH_SAR:  return dmac->channel[index].sar;
        case DMAC_CH_DAR:  return dmac->channel[index].dar;
        case DMAC_CH_TCR:  return dmac->channel[index].tcr;
        case DMAC_CH_CHCR: return dmac->channel[index].chcr;
        }
    }
    qemu_log_mask(LOG_UNIMP, "cdj2000-dmac: read 0x%" HWADDR_PRIx
                  " (%u bytes)\n", DMAC_BASE + offset, size);
    return 0;
}

static void cdj_dmac_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CdjDmacState *dmac = opaque;
    unsigned index;

    if (offset == DMAC_DMAOR) {
        dmac->dmaor = value;
        return;
    }
    if (cdj_dmac_channel_of(offset, &index)) {
        CdjDmacChannel *channel = &dmac->channel[index];

        switch (offset & 0xf) {
        case DMAC_CH_SAR:  channel->sar = value; return;
        case DMAC_CH_DAR:  channel->dar = value; return;
        case DMAC_CH_TCR:  channel->tcr = value; return;
        case DMAC_CH_CHCR:
            /*
             * Clearing TE is how the handler acknowledges, so drop the line
             * with it.  Without that the level stays asserted and the ISR is
             * re-entered forever, which is not distinguishable from a hang.
             */
            if (!(value & CHCR_TE)) {
                if (channel->role == CDJ_DMA_PANEL_RX
                    && dmac->panel_rx_pending) {
                    dmac->panel_rx_pending = false;
                    if (dmac->panel_rx_line) {
                        qemu_set_irq(dmac->panel_rx_line, 0);
                    }
                }
                if (channel->role == CDJ_DMA_PANEL_TX
                    && dmac->panel_tx_pending) {
                    dmac->panel_tx_pending = false;
                    if (dmac->panel_tx_line) {
                        qemu_set_irq(dmac->panel_tx_line, 0);
                    }
                }
                if (dmac->sdhi_dma_pending
                    && channel->sar == SDHI_DATA_PORT_P4) {
                    dmac->sdhi_dma_pending = false;
                    qemu_set_irq(dmac->sdhi_dma_irq, 0);
                }
                if (dmac->dsp_dma_pending && channel->role == CDJ_DMA_DSP
                    && index != DMA5_CHANNEL) {
                    dmac->dsp_dma_pending = false;
                    qemu_set_irq(dmac->dsp_dma_irq, 0);
                }
            }
            /*
             * Channel 5's handler (0x1c2158) acknowledges differently: it
             * clears IE, not TE, and leaves TE for the sender to clear when it
             * re-arms the channel.  The request is TE && IE, so either bit
             * going down takes the line with it -- left on TE alone the level
             * re-entered the handler 300 times before the trace gave up
             * (trackload-33) and the tasks never ran again.
             */
            if (dmac->dma5_pending && index == DMA5_CHANNEL
                && (!(value & CHCR_TE) || !(value & CHCR_IE))) {
                dmac->dma5_pending = false;
                qemu_set_irq(dmac->dma5_irq, 0);
            }
            /* The USB driver's channel: either bit going down ends it. */
            if (dmac->usbh_dma_pending && index == dmac->usbh_dma_channel
                && (!(value & CHCR_TE) || !(value & CHCR_IE))) {
                dmac->usbh_dma_pending = false;
                qemu_set_irq(dmac->usbh_dma_irq, 0);
            }
            channel->chcr = value;
            if ((value & CHCR_DE) && (dmac->dmaor & 1)) {
                cdj_dmac_run(dmac, index);
            }
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "cdj2000-dmac: write 0x%" HWADDR_PRIx
                  " (%u bytes) = 0x%" PRIx64 "\n",
                  DMAC_BASE + offset, size, value);
}

static const MemoryRegionOps cdj_dmac_ops = {
    .read = cdj_dmac_read,
    .write = cdj_dmac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * The panel UART itself.  MAIN only ever read-modify-writes its control
 * registers — 0x246e36 clears +0x08, sets and masks +0x18 and clears bit 1 of
 * +0x10 — because the DMAC moves the data.  A plain register file is therefore
 * the whole model it needs; against a trap that reads zero those cycles lose
 * whatever the firmware had put there.
 */
typedef struct {
    MemoryRegion iomem;
    uint16_t reg[PANEL_SCIF_SIZE / 2];
    bool trace;                     /* CDJ_PANEL_SCIF_TRACE */
} CdjPanelScifState;

static uint64_t cdj_panel_scif_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjPanelScifState *scif = opaque;
    uint64_t value = offset + size <= PANEL_SCIF_SIZE ? scif->reg[offset >> 1] : 0;

    if (scif->trace) {
        fprintf(stderr, "cdj2000-panel-scif %.3f: read  %#04" HWADDR_PRIx
                " (%u) = %#06" PRIx64 "\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, offset, size, value);
    }
    return value;
}

static void cdj_panel_scif_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    CdjPanelScifState *scif = opaque;

    if (scif->trace) {
        fprintf(stderr, "cdj2000-panel-scif %.3f: write %#04" HWADDR_PRIx
                " (%u) = %#06" PRIx64 "\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, offset, size, value);
    }
    if (offset + size <= PANEL_SCIF_SIZE) {
        scif->reg[offset >> 1] = value;
    }
}

static const MemoryRegionOps cdj_panel_scif_ops = {
    .read = cdj_panel_scif_read,
    .write = cdj_panel_scif_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_panel_scif_init(MemoryRegion *system)
{
    CdjPanelScifState *scif = g_new0(CdjPanelScifState, 1);

    scif->trace = getenv("CDJ_PANEL_SCIF_TRACE") != NULL;
    memory_region_init_io(&scif->iomem, NULL, &cdj_panel_scif_ops, scif,
                          "cdj2000.panel-scif", PANEL_SCIF_SIZE);
    memory_region_add_subregion(system, PANEL_SCIF_BASE, &scif->iomem);
}

/*
 * The word the shared panel handler dispatches on.  Read-only and derived: a
 * channel is pending exactly while it has a completion the guest has not yet
 * taken out of CHCR.
 */
static uint64_t cdj_intc2_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjDmacState *dmac = opaque;

    /* Bit n is DMINTn, i.e. board index n + 2: the application's receive on
     * index 3 is bit 1 (INTC2_DMA_RX), its transmit on index 4 bit 2. */
    return (dmac->panel_rx_pending ? 1u << (dmac->panel_rx_index - 2) : 0)
         | (dmac->panel_tx_pending ? 1u << (dmac->panel_tx_index - 2) : 0)
         | (dmac->dma5_pending ? 1u << (DMA5_CHANNEL - 2) : 0);
}

static void cdj_intc2_dma_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
}

static const MemoryRegionOps cdj_intc2_dma_ops = {
    .read = cdj_intc2_dma_read,
    .write = cdj_intc2_dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * A transfer the USB module ran on the DMAC's behalf has moved every byte.
 * The channel completes as the SDHI's does -- the device end stays put, the
 * memory end advances -- and the level stays up until the driver's handler
 * clears TE or IE in CHCR.
 */
static void cdj_dmac_usbh_done(void *opaque, unsigned index, uint32_t nr_bytes)
{
    CdjDmacState *dmac = opaque;
    CdjDmacChannel *channel = &dmac->channel[index];
    hwaddr source = cdj_dma_phys(channel->sar);
    hwaddr destination = cdj_dma_phys(channel->dar);

    if ((destination & ~0xffull) == CDJ_USBH_BASE + 0x100) {
        cdj_dmac_complete(channel, source + nr_bytes, channel->dar);
    } else {
        cdj_dmac_complete(channel, channel->sar, destination + nr_bytes);
    }
    if (dmac->usbh_dma_irq && !dmac->usbh_dma_pending) {
        dmac->usbh_dma_pending = true;
        qemu_set_irq(dmac->usbh_dma_irq, 1);
    }
}

static CdjDmacState *cdj_dmac_init(MemoryRegion *system, qemu_irq panel_rx,
                                   qemu_irq panel_tx, qemu_irq sdhi_dma,
                                   qemu_irq dsp_dma, qemu_irq dma5,
                                   qemu_irq usbh_dma)
{
    CdjDmacState *dmac = g_new0(CdjDmacState, 1);

    memory_region_init_io(&dmac->intc2_status, NULL, &cdj_intc2_dma_ops, dmac,
                          "cdj2000.intc2-dma", 4);
    memory_region_add_subregion(system, INTC2_DMA_STATUS, &dmac->intc2_status);

    dmac->panel_rx_irq = panel_rx;
    dmac->panel_tx_irq = panel_tx;
    dmac->sdhi_dma_irq = sdhi_dma;
    dmac->dsp_dma_irq = dsp_dma;
    dmac->dma5_irq = dma5;
    dmac->usbh_dma_irq = usbh_dma;
    /* DMINT0..3 by board index; 0x780/0x7a0 (DMINT4/5) have no user yet. */
    dmac->channel_irq[2] = usbh_dma;
    dmac->channel_irq[3] = panel_rx;
    dmac->channel_irq[4] = panel_tx;
    dmac->channel_irq[5] = dma5;
    dmac->panel_present = !getenv("CDJ_NO_PANEL");
    dmac->panel_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_dmac_panel_done,
                                     dmac);
    memory_region_init_io(&dmac->iomem, NULL, &cdj_dmac_ops, dmac,
                          "cdj2000.dmac", DMAC_SIZE);
    /* Overlaps the catch-all trap, so it needs the higher priority. */
    memory_region_add_subregion_overlap(system, DMAC_BASE, &dmac->iomem, 1);

    memory_region_init_alias(&dmac->iomem_p4, NULL, "cdj2000.dmac-p4",
                             &dmac->iomem, 0, DMAC_SIZE);
    memory_region_add_subregion_overlap(system, P4ADDR(DMAC_BASE),
                                        &dmac->iomem_p4, 1);
    return dmac;
}

static void cdj_cpu_reset(void *opaque)
{
    CdjResetState *reset = opaque;

    cpu_reset(CPU(reset->cpu));
    cpu_set_pc(CPU(reset->cpu), reset->vector);
}

/*
 * Trapped peripheral space.  Everything is logged; nothing is modelled yet.
 * Reads answer with a configurable constant (0 by default).  When the firmware
 * settles into polling one register the log says so once, which is what turns
 * "the boot hangs" into "the boot waits for 0x1ff10058 bit n".
 */
static uint64_t cdj_periph_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjPeriphState *periph = opaque;
    hwaddr address = PERIPH_P4_BASE + offset;

    if (address == periph->last_read) {
        periph->repeats++;
        if (periph->repeats == SPIN_REPORT && !periph->reported) {
            periph->reported = true;
            qemu_log_mask(LOG_UNIMP,
                          "cdj2000-main: spinning on a read of 0x%" HWADDR_PRIx
                          " (%u bytes) — model this register\n", address, size);
        }
    } else {
        periph->last_read = address;
        periph->repeats = 1;
        periph->reported = false;
        qemu_log_mask(LOG_UNIMP,
                      "cdj2000-main: read  0x%" HWADDR_PRIx " (%u bytes)\n",
                      address, size);
    }
    return periph->default_value;
}

static void cdj_periph_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    CdjPeriphState *periph = opaque;
    hwaddr address = PERIPH_P4_BASE + offset;

    if (address == periph->last_write && value == periph->last_write_value) {
        periph->write_repeats++;
        return;
    }
    if (periph->write_repeats > 1) {
        qemu_log_mask(LOG_UNIMP,
                      "cdj2000-main: write 0x%" HWADDR_PRIx " = 0x%" PRIx64
                      " repeated %" PRIu64 " times\n",
                      periph->last_write, periph->last_write_value,
                      periph->write_repeats);
    }
    periph->last_write = address;
    periph->last_write_value = value;
    periph->write_repeats = 1;
    qemu_log_mask(LOG_UNIMP,
                  "cdj2000-main: write 0x%" HWADDR_PRIx " (%u bytes) = 0x%"
                  PRIx64 "\n", address, size, value);
}

static const MemoryRegionOps cdj_periph_ops = {
    .read = cdj_periph_read,
    .write = cdj_periph_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_periph_init(MemoryRegion *system)
{
    CdjPeriphState *periph = g_new0(CdjPeriphState, 1);

    /*
     * Lowest priority, so that every real device added later simply overlays
     * the trap and no explicit ordering is needed.
     */
    memory_region_init_io(&periph->trap, NULL, &cdj_periph_ops, periph,
                          "cdj2000.periph", PERIPH_SIZE);
    memory_region_add_subregion_overlap(system, PERIPH_A7_BASE, &periph->trap,
                                        PERIPH_PRIORITY);

    memory_region_init_alias(&periph->trap_p4, NULL, "cdj2000.periph-p4",
                             &periph->trap, 0, PERIPH_SIZE);
    memory_region_add_subregion_overlap(system, PERIPH_P4_BASE,
                                        &periph->trap_p4, PERIPH_PRIORITY);
}

/*
 * CDJ_BUS_TRACE — find a peripheral we have not modelled by letting the
 * firmware reach for it.
 *
 * cdj_periph_init above traps the SH-4 on-chip block at 0xff000000, so anything
 * unmodelled *there* is already logged.  The external bus is the blind spot:
 * the USB function controller sits at physical 0x01000000, an ordinary chip on
 * a chip-select, and r196 showed that every access in the boot trace belongs to
 * that function controller — the type-B socket facing a computer.  A memory
 * stick goes into the type-A socket and needs a USB *host* controller, which is
 * a different chip at a different address, and an access to it would land in
 * unmapped area-0 space where reads answer 0 and writes vanish **silently**.
 * That silence is indistinguishable from "the driver never ran", which is
 * exactly the question.
 *
 * So this maps the whole of area 0 at a priority below every real device — the
 * flash, the RAM, the USB window and the rest all overlay it untouched — and
 * logs what falls through.  Behaviour is unchanged: reads still answer 0 and
 * writes are still dropped, they are merely no longer invisible.  Addresses are
 * deduplicated so a polling loop reports once rather than filling the log.
 *
 * Off unless CDJ_BUS_TRACE is set, so the default machine is bit-for-bit what
 * it was.
 *
 * Deduplication is per 4 KiB PAGE, not per address.  The first run of this
 * trace was swallowed by a linear 4-byte read sweep from 0x08000000 that filled
 * a 512-address budget by itself and masked everything that came after it — the
 * budget has to survive a memory scan to be able to show a register window.
 * The first access in each page is logged with its exact address, and a page is
 * reported once.
 */
#define CDJ_BUS_TRACE_BASE      0x00000000
#define CDJ_BUS_TRACE_SIZE      0x20000000
#define CDJ_BUS_TRACE_PRIORITY  (-1000)
/*
 * One budget per 16 MiB region rather than one shared budget.  r197 spent its
 * whole 512-entry allowance on a single linear read sweep of area 2 and was
 * blind to everything afterwards, which is precisely the failure this trace
 * exists to avoid.  Excluding area 2 would have worked too, but it discards
 * evidence in advance; a per-region budget keeps the sweep visible and merely
 * stops it starving the other thirty-one regions.
 */
#define CDJ_BUS_TRACE_REGION_BITS 24                       /* 16 MiB each */
#define CDJ_BUS_TRACE_REGIONS   (CDJ_BUS_TRACE_SIZE >> CDJ_BUS_TRACE_REGION_BITS)
#define CDJ_BUS_TRACE_PER_REGION 48

typedef struct {
    MemoryRegion trap;
    hwaddr seen[CDJ_BUS_TRACE_REGIONS][CDJ_BUS_TRACE_PER_REGION];
    unsigned nr_seen[CDJ_BUS_TRACE_REGIONS];
    bool full[CDJ_BUS_TRACE_REGIONS];
} CdjBusTraceState;

static bool cdj_bus_trace_first(CdjBusTraceState *bus, hwaddr address)
{
    hwaddr page = address & ~(hwaddr)0xfff;
    hwaddr offset = address - CDJ_BUS_TRACE_BASE;
    unsigned region = offset >> CDJ_BUS_TRACE_REGION_BITS;
    unsigned i;

    if (region >= CDJ_BUS_TRACE_REGIONS) {
        return false;
    }
    for (i = 0; i < bus->nr_seen[region]; i++) {
        if (bus->seen[region][i] == page) {
            return false;
        }
    }
    if (bus->nr_seen[region] == CDJ_BUS_TRACE_PER_REGION) {
        /*
         * Name the region that filled up, because that is the actionable
         * half: a full region is a sweep, and the other thirty-one still
         * report.  The shared budget this replaced went silent altogether.
         */
        if (!bus->full[region]) {
            bus->full[region] = true;
            qemu_log_mask(LOG_UNIMP,
                          "cdj2000-bus: region %u (0x%08" HWADDR_PRIx ") is "
                          "full at %u distinct pages; further ones there are "
                          "not reported\n",
                          region,
                          (hwaddr)CDJ_BUS_TRACE_BASE +
                          ((hwaddr)region << CDJ_BUS_TRACE_REGION_BITS),
                          bus->nr_seen[region]);
        }
        return false;
    }
    bus->seen[region][bus->nr_seen[region]++] = page;
    return true;
}

static uint64_t cdj_bus_trace_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjBusTraceState *bus = opaque;
    hwaddr address = CDJ_BUS_TRACE_BASE + offset;

    if (cdj_bus_trace_first(bus, address)) {
        qemu_log_mask(LOG_UNIMP,
                      "cdj2000-bus: read  0x%08" HWADDR_PRIx " (%u bytes)"
                      " — unmapped, answered 0\n", address, size);
    }
    return 0;
}

static void cdj_bus_trace_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    CdjBusTraceState *bus = opaque;
    hwaddr address = CDJ_BUS_TRACE_BASE + offset;

    if (cdj_bus_trace_first(bus, address)) {
        qemu_log_mask(LOG_UNIMP,
                      "cdj2000-bus: write 0x%08" HWADDR_PRIx " (%u bytes)"
                      " = 0x%" PRIx64 " — unmapped, dropped\n",
                      address, size, value);
    }
}

static const MemoryRegionOps cdj_bus_trace_ops = {
    .read = cdj_bus_trace_read,
    .write = cdj_bus_trace_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_bus_trace_init(MemoryRegion *system)
{
    CdjBusTraceState *bus;

    if (getenv("CDJ_BUS_TRACE") == NULL) {
        return;
    }
    bus = g_new0(CdjBusTraceState, 1);
    memory_region_init_io(&bus->trap, NULL, &cdj_bus_trace_ops, bus,
                          "cdj2000.bus-trace", CDJ_BUS_TRACE_SIZE);
    memory_region_add_subregion_overlap(system, CDJ_BUS_TRACE_BASE, &bus->trap,
                                        CDJ_BUS_TRACE_PRIORITY);
    info_report("cdj2000: bus trace on — unmapped area-0 accesses are logged");
}

/*
 * The INTC2 status word the link handlers test before doing anything, and
 * the second one at +0xc (0xffd4005c) that the DSP's vector stub 0x26260c
 * tests for bit 24 before it calls the DSP handler 0x1c09a0 (its neighbour
 * 0x2625f6 tests bit 19 there for another device).  The mask and priority
 * registers around them stay trapped, because the CPU-side masking is done
 * by sh_intc and SR.IMASK instead.
 */
typedef struct {
    MemoryRegion iomem;
    uint32_t status;
    uint32_t status2;           /* 0xffd4005c */
} CdjIntc2State;

/*
 * The 0xfff10000 block is the busiest peripheral in the machine and is not
 * identified, but its registers are plainly read-modify-written all over the
 * firmware — the panel send task alone does it to +0x00, +0x20, +0x58 and
 * +0x80 before it will run.  Against a trap that always reads zero every one of
 * those cycles loses whatever was there, so the whole page is modelled as a
 * plain register file: writes stick, reads return what was written.  That is
 * not a guess about semantics, only the minimum that lets read-modify-write
 * behave like memory.
 */
typedef struct {
    MemoryRegion iomem;
    bool panel_present;
    bool usb_power;
    bool dsp_event;             /* the DSP's interrupt line, GPIO 0xfff10040 bit 4 */
    uint16_t reg[SOC_BLOCK_SIZE / 2];
} CdjLinkFlagState;

/*
 * The DSP's interrupt to MAIN, as MAIN sees it: irq 0x7f (INTEVT 0xfe0, the
 * RTOS record at 0xa409f4d4), bit 24 of the second INTC2 status word, which
 * the stub 0x26260c tests before calling the handler 0x1c09a0, and bit 4 of
 * GPIO 0xfff10040, which 0x1c7ce4 polls (and acknowledges with bit 2 of the
 * DSP control register when it finds it set).  The handler reads the event
 * word at window+0xffe8, stores bytes 2 and 3 as the event code, sets the
 * same ACK bit and set_flg()s the DSP task.  The device raises and lowers
 * the line (cdj2000_dsp.c); this is where the line's two status bits live.
 */
#define DSP_EVENT_REG   0xfff10040
#define DSP_EVENT_BIT   0x0010
#define DSP_RESET_REG   0xfff10054
#define DSP_RESET_BIT   0x0040
#define DSP_PHASE_REG   0xfff1005c
#define DSP_PHASE_MASK  0x0007

typedef struct {
    CdjIntc2State *intc2;
    CdjLinkFlagState *flag;
} CdjDspEventSink;

static CdjDspEventSink cdj_dsp_event_sink;

static void cdj_dsp_event_pending(void *opaque, bool raised)
{
    CdjDspEventSink *sink = opaque;

    if (sink->intc2) {
        if (raised) {
            sink->intc2->status2 |= INTC2_DSP_EVENT_BIT;
        } else {
            sink->intc2->status2 &= ~INTC2_DSP_EVENT_BIT;
        }
    }
    if (sink->flag) {
        sink->flag->dsp_event = raised;
    }
}

static void cdj_nxs_hint_level(void *opaque, bool high)
{
    CdjLinkFlagState *flag = opaque;
    flag->dsp_event = high;
}

static uint64_t cdj_link_flag_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjLinkFlagState *flag = opaque;

    if (offset + size > SOC_BLOCK_SIZE) {
        return 0;
    }
    if (offset == PANEL_PRESENT_REG && (flag->panel_present || flag->usb_power)) {
        uint64_t value = flag->reg[offset >> 1];

        if (flag->panel_present) {
            value |= PANEL_PRESENT_BIT;
        }
        if (flag->usb_power) {
            value |= USB_POWER_SENSE_BIT;
        }
        return value;
    }
    if (offset == DSP_EVENT_REG - SOC_BLOCK_BASE && flag->dsp_event) {
        return flag->reg[offset >> 1] | DSP_EVENT_BIT;
    }
    if (size <= 2) {
        return flag->reg[offset >> 1];
    }
    return flag->reg[offset >> 1] | ((uint32_t)flag->reg[(offset >> 1) + 1] << 16);
}

static void cdj_link_flag_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    CdjLinkFlagState *flag = opaque;
    bool old_dsp_reset = false;

    if (offset + size > SOC_BLOCK_SIZE) {
        return;
    }
    if (cdj_nxs_profile && offset == DSP_RESET_REG - SOC_BLOCK_BASE) {
        old_dsp_reset = (flag->reg[offset >> 1] & DSP_RESET_BIT) != 0;
    }
    flag->reg[offset >> 1] = value;
    if (size > 2) {
        flag->reg[(offset >> 1) + 1] = value >> 16;
    }
    if (cdj_nxs_profile && offset == DSP_RESET_REG - SOC_BLOCK_BASE &&
        old_dsp_reset != ((value & DSP_RESET_BIT) != 0)) {
        cdj_nxs_hpi_reset_line((value & DSP_RESET_BIT) != 0);
    }
    if (cdj_nxs_profile && offset == DSP_PHASE_REG - SOC_BLOCK_BASE) {
        cdj_nxs_hpi_boot_phase(value & DSP_PHASE_MASK);
    }
}

/*
 * Bit 2 of 0xfff10048 has two readers that want opposite things:
 * `0x2134cc` (GuiCom_RcvTASK) examines a frame only while it is SET, and
 * `0x215268` (GuiCom_SndTASK) sets its ready word only while it is CLEAR.
 *
 * The bit means **a frame is pending**: set when one is delivered, cleared when
 * the receiver is re-armed, which is the moment the previous frame has been
 * consumed.  So it starts *clear*, which is what "comes up ready before any
 * frame exists" needs, and it is set for exactly the frame the receive task is
 * about to examine.
 *
 * Reading it as "the receiver is armed" instead -- set on arm, cleared on
 * delivery -- looks equally defensible and is wrong, because GuiCom_SndTASK
 * calls the link init itself (`0x2a3c0e`, at `0x215226`) *before* its first
 * loop: the bit is already set the first time the task looks at it, the ready
 * word is never set, the loop never reaches its exit at `0x215260`, and after
 * 10000 ticks `0x21530e` reports device 5 failed.
 *
 * Measured, two-board, one binary, same card:
 *
 *   CDJ_LINK_FLAG_ARMED=1   device 5 FAILED, ready word 0, reg 36,     52 records
 *   default (pending)       device 5 **up**, ready word 1, reg 32,   5637 records
 *
 * The old reading is kept behind the switch so that A/B stays one build.
 */
static bool cdj_link_flag_pending(void)
{
    return getenv("CDJ_LINK_FLAG_ARMED") == NULL;
}

static void cdj_link_flag_rx(CdjLinkFlagState *flag, bool set)
{
    unsigned index = (LINK_FLAG_REG - SOC_BLOCK_BASE) >> 1;

    if (set) {
        flag->reg[index] |= LINK_FLAG_RX;
    } else {
        flag->reg[index] &= ~LINK_FLAG_RX;
    }
}

static const MemoryRegionOps cdj_link_flag_ops = {
    .read = cdj_link_flag_read,
    .write = cdj_link_flag_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

typedef struct CdjLinkState {
    MemoryRegion regs;
    MemoryRegion mode;
    CdjIntc2State *intc2;
    CdjLinkFlagState *flag;
    qemu_irq irq;
    CharFrontend chr;
    bool connected;

    const char *name;
    uint32_t base;
    uint32_t intc2_bit;

    uint32_t buffer_off;
    uint32_t length_off;
    bool transmit;              /* this half sends, rather than receives */
    struct CdjLinkState *owner; /* the half that owns the chardev */

    uint32_t control;
    uint32_t buffer;
    uint32_t length;        /* raw +0x20 */
    uint32_t r40, r50, r00;
    uint32_t status;        /* +0x188 */
    uint32_t enable;        /* +0x190 */
    uint32_t mode_reg;
    uint32_t mode_status;
    qemu_irq done_irq;
    uint32_t rx_status;     /* status bits raised on a completed receive */

    /* Bytes staged from the chardev, delivered once a frame is complete. */
    uint8_t rx[512];
    unsigned rx_filled;

    /*
     * CDJ_LINK_CENSUS=<seconds> -- the guest half of "who freezes the status
     * channel".  The GUI's dump counts records *handed to the firmware*, and
     * the simulator repeats MAIN's last 64-byte record whenever MAIN has not
     * sent a newer one, so that dump cannot say whether MAIN is still
     * transmitting at all.  These counters can: they sit on the register
     * writes the guest performs, so `armed` standing still means MAIN's own
     * task stopped arming, `sent` behind `armed` means the model refused, and
     * `ack`/`gate` standing still means the completion handshake stalled --
     * which is the only way a healthy task can stop arming.  Logging only.
     */
    unsigned long n_armed;
    unsigned long n_sent;
    unsigned long n_bail;       /* armed with no frame length or no buffer */
    unsigned long n_short;      /* the chardev took fewer bytes than offered */
    unsigned long n_rx;
    unsigned long n_ack;        /* guest acknowledged the status register */
    unsigned long n_gate;       /* guest cleared the transfer-done gate */
    /*
     * The transfer-done handler 0x2a3eb4 is a loop: it reads 0xff502004, and
     * leaves only when that read has no bit of 0x0C000000 *and* 0xff502000 has
     * no bit 25.  This model sets bit 25 in 0xff502000 on every transmit and
     * nothing but the guest's next arm (0x2a3d80 writes 0x0009c142) clears it,
     * so if the arm cannot happen the loop has no exit.  A spin is invisible in
     * every other counter and enormous in this one: the loop reads 0xff502004
     * once per iteration at full CPU speed.
     */
    unsigned long n_mode_read;
    unsigned long n_mode_reg;   /* guest wrote 0xff502000 itself */
    unsigned long n_wbytes;     /* bytes the chardev accepted, header included */
    int64_t census_next;
    QEMUTimer *tx_timer;        /* the transmit completion, in flight */
    /*
     * The receive FIFO (CDJ_LINK_RX_QUEUE).  A frame is delivered into the
     * guest's buffer only when the previous one has been acknowledged, so
     * the task that examines it sees every frame in order; frames that
     * arrive meanwhile wait here, the oldest dropped when it is full.
     */
    uint8_t queue[CDJ_LINK_RX_QUEUE_MAX][512];
    unsigned queue_len[CDJ_LINK_RX_QUEUE_MAX];
    unsigned queue_head, queue_count;
    bool rx_pending;            /* delivered, not yet handed past */
    unsigned long n_queued, n_dropped, n_watchdog;
    unsigned long n_answered;   /* handed over because MAIN answered */
    unsigned long n_gapped;     /* held back for CDJ_LINK_RX_GAP_US */
    int64_t rx_delivered_ns;    /* when the last frame went into the buffer */
    QEMUTimer *rx_watchdog;
    QEMUTimer *rx_gap;
    struct CdjLinkState *rx_peer;   /* transmit half: the receive half it paces */
} CdjLinkState;

static void cdj_link_rx_next(CdjLinkState *link);
static void cdj_link_rx_release(CdjLinkState *link);
static bool cdj_link_rx_handover_on_answer(void);
static void cdj_link_rx_answered(CdjLinkState *rx);
static void cdj_link_cosim_pump(CdjLinkState *link);
static bool cdj_link_link_rows(uint8_t *frame, unsigned len);

static int64_t cdj_link_census_every(void)
{
    static int64_t every = -1;

    if (every < 0) {
        const char *env = getenv("CDJ_LINK_CENSUS");
        double seconds = (env && *env) ? strtod(env, NULL) : 0.0;

        if ((env && *env) && seconds <= 0.0) {
            seconds = 10.0;
        }
        every = (int64_t)(seconds * NANOSECONDS_PER_SECOND);
    }
    return every;
}

static void cdj_link_census(CdjLinkState *link)
{
    int64_t every = cdj_link_census_every();
    int64_t now;

    if (every <= 0) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < link->census_next) {
        return;
    }
    link->census_next = now + every;
    qemu_log_mask(LOG_UNIMP,
                  "%s: census t%.1f armed=%lu sent=%lu bail=%lu short=%lu "
                  "rx=%lu ack=%lu gate=%lu moderead=%lu modereg=%lu "
                  "wbytes=%lu queued=%lu dropped=%lu watchdog=%lu "
                  "answered=%lu gapped=%lu\n",
                  link->name,
                  (double)now / NANOSECONDS_PER_SECOND, link->n_armed,
                  link->n_sent, link->n_bail, link->n_short, link->n_rx,
                  link->n_ack, link->n_gate, link->n_mode_read,
                  link->n_mode_reg, link->n_wbytes, link->n_queued,
                  link->n_dropped, link->n_watchdog, link->n_answered,
                  link->n_gapped);
}

static void cdj_intc2_set(CdjLinkState *link, bool raise)
{
    CdjIntc2State *intc2 = link->intc2;

    if (raise) {
        intc2->status |= link->intc2_bit;
    } else {
        intc2->status &= ~link->intc2_bit;
    }
    qemu_set_irq(link->irq, !!(intc2->status & link->intc2_bit));
}

static unsigned cdj_link_frame_len(CdjLinkState *link)
{
    /* +0x20 bits 31:4 count 16-byte units; the driver also mirrors it in +0x50. */
    unsigned units = link->length >> 4;

    if (!units) {
        units = link->r50;
    }
    return units * 16;
}

/*
 * Arming the transmit half hands the frame straight to the chardev and reports
 * completion.  MAIN fills 0x489bd98, copies it to the buffer and arms this
 * channel from 0x2a3cf6(record, 64); there is no separate "go" register.
 *
 * Every frame carries an 8-byte "CDJL" + little-endian length header, because
 * the wire here is a TCP socket and the thing it stands in for is not.  MAIN
 * mixes 64-byte status records with payload records of 48 to 896 bytes (see
 * LINK_FRAME_MAX); a peer reading a flat byte stream cannot tell where one
 * ends, and a single 224-byte frame read
 * as three-and-a-half 64-byte ones leaves it 32 bytes out of phase for the rest
 * of the run -- every record fails its checksum from then on and the GUI puts
 * E-8709 on screen.  The header is skipped by a peer that does not know it (the
 * simulator falls back to raw bytes when the magic is absent), so it costs
 * nothing but makes the boundary explicit.
 */
/*
 * CDJ_LINK_TX_US: how long a transmitted frame is in flight before its
 * completion is reported, in microseconds of guest time.  Off (0) by
 * default: the completion is reported inside the arm write, as this model
 * always did.
 *
 * Why it exists, and why it is off.  With the completion synchronous, MAIN
 * answers the GUI's browse requests in one burst every 3.000 s -- hundreds
 * of 48-byte answers 0.1 ms apart behind one 64-byte status record, then
 * nothing for three seconds -- while the GUI asks sixty times a second
 * (usb3, 130 s).  That looks like a completion landing before the sending
 * task has reached its wait, which a frame's time on the wire (64 bytes at
 * a megabit or so: a few hundred microseconds) would cure.  Measured at
 * 500 us, same binaries otherwise: one boot without a key answered spread
 * out instead of in bursts and reached the player screen at 34 s as before
 * (m1); two boots with a SOURCE key at 40 s never showed the player screen
 * at all -- one stayed black for 130 s (usb8), one sent only status
 * requests and drew nothing (usb7) -- where the same configuration without
 * it shows the player screen at 28 s and the Wait platter six seconds after
 * the key (usb6).  The two boards' protocol tasks are balanced against each
 * other by timing on both sides (dv-bfin_ppi.c's yield budget and holds),
 * and moving this one edge upsets that balance.  Kept for the experiment
 * that finds the matching change on the other side.
 */
static int64_t cdj_link_tx_ns(void)
{
    static int64_t ns = -1;

    if (ns < 0) {
        const char *env = getenv("CDJ_LINK_TX_US");

        ns = (env && *env ? strtoll(env, NULL, 10) : 0) * 1000LL;
        if (ns < 0) {
            ns = 0;
        }
    }
    return ns;
}

/*
 * Two completions are reported, because the firmware waits for both: the
 * channel's own one (handler 0x2a3db0 via INTC2 bit 5), and the separate
 * transfer-done interrupt that handler 0x2a3eb4 services on INTC2 bit 6.
 * Only the latter clears the transmit-in-progress flag 0x7db3541, and it
 * insists on bit 25 being set in the status at 0xff502004 *and* in the
 * control at 0xff502000 (0x2a3f00..0x2a3f14).
 */
static void cdj_link_tx_complete(void *opaque)
{
    CdjLinkState *link = opaque;

    link->control &= ~LINK_CTRL_START;

    link->status |= link->rx_status;
    cdj_intc2_set(link, true);

    link->mode_status |= MODE_DONE | MODE_GATE;
    link->mode_reg |= MODE_GATE;
    if (link->done_irq) {
        link->intc2->status |= LINK_DONE_BIT;
        qemu_set_irq(link->done_irq, 1);
    }
}

/*
 * The longest frame the model carries.  The GUI's receiver validates an
 * announced payload length against 1..2048 halfwords (0xb7f8d2), so 4096
 * bytes is the protocol's own ceiling; the simulator's DMA model reads a
 * receive in chunks of at most 4096 bytes and pairs a frame with a receive of
 * exactly its length, so this must not be raised without changing that.
 *
 * It was 512 until the first playlist's track list.  MAIN announces that
 * answer as 448 halfwords -- 896 bytes, its rows are UTF-16 titles -- and a
 * frame past the buffer was dropped *without* the completion below, which
 * leaves the transmit-in-progress flag 0x7db3541 set: MAIN never transmits
 * again.  twoboard-10-load (2026-09-02): the -D log's last "sent" at
 * t=118.6, 12 160 requests delivered after it, none answered, the status
 * record announcing 448 halfwords for the rest of the run.
 */
#define LINK_FRAME_MAX 4096

static void cdj_link_transmit(CdjLinkState *link)
{
    unsigned frame = cdj_link_frame_len(link);
    CdjLinkState *owner = link->owner ? link->owner : link;
    uint8_t buffer[8 + LINK_FRAME_MAX];

    if (!frame || !link->buffer) {
        /*
         * Nothing to send, but the arm happened: complete it, as the
         * oversized case below does.  Returning without the completion left
         * START set and the transmit-in-progress flag 0x7db3541 at 1, and
         * MAIN never transmitted again -- the GUI's requests kept arriving
         * every five seconds, unanswered, and it put E-8709 on screen.  Seen
         * on the first track load from a native browse (fix-1, t=291.8:
         * control 0x43, length 0).  A DMA with nothing to move finishes; it
         * does not hang the channel.
         */
        link->n_bail++;
        warn_report_once("cdj2000: %s: armed with %s; completed without "
                         "sending", link->name,
                         link->buffer ? "a zero length" : "no buffer");
        qemu_log_mask(LOG_UNIMP, "%s: empty arm (length 0x%x r50 0x%x "
                      "buffer 0x%08x) completed t=%.4f\n", link->name,
                      link->length, link->r50, link->buffer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
        cdj_link_census(link);
        cdj_link_tx_complete(link);
        return;
    }
    if (frame > LINK_FRAME_MAX) {
        /*
         * Dropped, but completed: without the completion the sender is dead
         * for the rest of the run, which is how the 512-byte buffer hid the
         * track list.  Nothing went on the wire, so complete at once.
         */
        link->n_bail++;
        warn_report_once("cdj2000: %s: a %u-byte frame exceeds LINK_FRAME_MAX "
                         "(%u); dropped, completion reported", link->name,
                         frame, (unsigned)LINK_FRAME_MAX);
        cdj_link_census(link);
        cdj_link_tx_complete(link);
        return;
    }
    memcpy(buffer, "CDJL", 4);
    stl_le_p(buffer + 4, frame);
    address_space_read(&address_space_memory, cdj_dma_phys(link->buffer),
                       MEMTXATTRS_UNSPECIFIED, buffer + 8, frame);
    bool send = cdj_link_link_rows(buffer + 8, frame);
    int written;

    if (send && cdj_cosim_active()) {
        /* One guest time with the GUI: the frame goes out stamped. */
        cdj_cosim_send_record(buffer + 8, frame);
        written = frame + 8;
    } else {
        written = owner->connected && send
            ? qemu_chr_fe_write_all(&owner->chr, buffer, frame + 8)
            : (send ? -1 : (int)(frame + 8));
    }

    link->n_sent++;
    if (written > 0) {
        link->n_wbytes += (unsigned)written;
    }
    if (written != (int)(frame + 8)) {
        link->n_short++;
    }
    qemu_log_mask(LOG_UNIMP, "%s: sent %u bytes from 0x%08x (connected=%d "
                  "written=%d, first words %02x%02x %02x%02x) t=%.4f\n",
                  link->name, frame, link->buffer, owner->connected, written,
                  buffer[9], buffer[8], buffer[11], buffer[10],
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    cdj_link_census(link);
    if (link->rx_peer && cdj_link_rx_handover_on_answer()) {
        cdj_link_rx_answered(link->rx_peer);
    }

    if (cdj_link_tx_ns() > 0 && link->tx_timer) {
        /* In flight: START stays set until the frame is out. */
        timer_mod(link->tx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cdj_link_tx_ns());
    } else {
        cdj_link_tx_complete(link);
    }
}

static uint64_t cdj_link_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjLinkState *link = opaque;

    if (offset == link->buffer_off) {
        return link->buffer;
    }
    if (offset == link->length_off) {
        return link->length;
    }
    switch (offset) {
    case 0x000:         return link->r00;
    case LINK_CONTROL:  return link->control;
    case LINK_R40:      return link->r40;
    case LINK_R50:      return link->r50;
    case LINK_STATUS:   return link->status;
    case LINK_ENABLE:   return link->enable;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read 0x%" HWADDR_PRIx " (%u bytes)\n",
                      link->name, link->base + offset, size);
        return 0;
    }
}

static void cdj_link_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CdjLinkState *link = opaque;

    if (offset == link->buffer_off) {
        link->buffer = value;
        if (!link->transmit) {
            cdj_link_cosim_pump(link);
        }
        return;
    }
    if (offset == link->length_off) {
        link->length = value;
        if (!link->transmit) {
            cdj_link_cosim_pump(link);
        }
        return;
    }
    switch (offset) {
    case 0x000:         link->r00 = value; return;
    case LINK_R40:      link->r40 = value; return;
    case LINK_R50:      link->r50 = value; return;
    case LINK_ENABLE:   link->enable = value; return;

    case LINK_CONTROL:
        /*
         * With CDJ_LINK_TX_US the previous frame may still be in flight
         * when the next arm arrives; the firmware's ISR chain arms the next
         * queued record from the completion handler, so on the chip it
         * never does, but a poll-and-arm path would.  Complete the frame
         * in flight first rather than lose the new one to the edge test.
         */
        if ((value & LINK_CTRL_START) && link->tx_timer
            && timer_pending(link->tx_timer)) {
            timer_del(link->tx_timer);
            cdj_link_tx_complete(link);
            link->n_short++;    /* counted: a flush is a model artefact */
        }
        if ((value & LINK_CTRL_START) && !(link->control & LINK_CTRL_START)) {
            if (cdj_link_trace()) {
                qemu_log_mask(LOG_UNIMP, "%s: armed buffer=0x%08x len=%u"
                              " t=%.4f\n",
                              link->name, link->buffer,
                              cdj_link_frame_len(link),
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
            }
            link->n_armed++;
            link->control = value;
            if (!link->transmit && link->rx_pending
                && !cdj_link_rx_handover_on_answer()) {
                cdj_link_rx_next(link);
            }
            /*
             * Under the pending reading, arming says nothing about whether a
             * frame is waiting, so it must not touch the bit.  Clearing here
             * was measurably wrong: the ISR re-arms before GuiCom_RcvTASK gets
             * to look, so 0x2134cc found the bit clear and 0x213594 discarded
             * every frame unexamined -- traced, both branches, same caller
             * 0x2133f4.  Clearing belongs to the guest, which read-modify-
             * writes this register at 0x246e36, 0x248dc2 and 0x251200.
             */
            if (link->flag && !link->transmit && !cdj_link_flag_pending()) {
                cdj_link_flag_rx(link->flag, true);
            }
            if (link->transmit) {
                cdj_link_transmit(link);
            } else {
                cdj_link_cosim_pump(link);
            }
            return;
        }
        link->control = value;
        return;

    case LINK_STATUS:
        /* Written back as (value & 31) to acknowledge. */
        if (cdj_link_trace()) {
            qemu_log_mask(LOG_UNIMP, "%s: ack status 0x%02x (was 0x%02x)"
                          " t=%.4f\n",
                          link->name, (unsigned)(value & 0x1f), link->status,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
        }
        link->n_ack++;
        cdj_link_census(link);
        link->status &= ~(value & 0x1f);
        if (!(link->status & 0x1c)) {
            cdj_intc2_set(link, false);
        }
        if (!link->transmit && link->rx_pending
            && !(link->status & link->rx_status)
            && !cdj_link_rx_handover_on_answer()) {
            cdj_link_rx_next(link);
        }
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: write 0x%" HWADDR_PRIx " (%u bytes) = 0x%" PRIx64 "\n",
                      link->name, link->base + offset, size, value);
    }
}

static const MemoryRegionOps cdj_link_ops = {
    .read = cdj_link_read,
    .write = cdj_link_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t cdj_link_mode_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjLinkState *link = opaque;

    switch (offset) {
    case 0:           return link->mode_reg;
    case MODE_STATUS: link->n_mode_read++; return link->mode_status;
    default:          return 0;
    }
}

static void cdj_link_mode_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    CdjLinkState *link = opaque;

    switch (offset) {
    case 0:
        link->n_mode_reg++;
        link->mode_reg = value;
        return;
    case MODE_STATUS:
        link->mode_status = value;
        if (!(link->mode_status & MODE_GATE) && link->done_irq) {
            link->n_gate++;
            if (cdj_link_trace()) {
                qemu_log_mask(LOG_UNIMP, "%s: gate cleared t=%.4f\n",
                              link->name,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
            }
            cdj_link_census(link);
            qemu_set_irq(link->done_irq, 0);
            link->intc2->status &= ~LINK_DONE_BIT;
        }
        return;
    }
}

static const MemoryRegionOps cdj_link_mode_ops = {
    .read = cdj_link_mode_read,
    .write = cdj_link_mode_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * A frame is only accepted while the driver has a receive armed, so the guest
 * can never be handed a frame it has nowhere to put.
 */
static int cdj_link_can_receive(void *opaque)
{
    CdjLinkState *link = opaque;
    unsigned frame = cdj_link_frame_len(link);

    if (!(link->control & LINK_CTRL_START) || !frame || !link->buffer) {
        return 0;
    }
    return frame - link->rx_filled;
}

/*
 * One frame per armed receive -- **opt-in, because it stalls the board**.
 *
 * `cdj_link_receive` leaves LINK_CTRL_START set after delivering a frame, so
 * `cdj_link_can_receive` immediately reports room again and the next frame
 * overwrites the buffer before GuiCom_RcvTASK has looked at it.  Two measured
 * symptoms of the same defect: in r081 **439 more type-0 frames were examined
 * than the GUI ever sent** -- the task re-CRCs whatever is still in 0x04500000,
 * because bit 2 of 0xfff10048 also stands permanently -- while of 1 084 type-1
 * requests only 99 were seen, because type 1 arrives in bursts and a burst
 * collapses to its last frame.
 *
 * Clearing START on delivery does fix the double read.  Measured, same binary,
 * same card, 600 s each:
 *
 *   r084  CDJ_LINK_RX_ONESHOT=1              1 508 requests, 1 510 examined
 *   r085  CDJ_LINK_RX_ONESHOT=1 + old yield     10 requests
 *   r083  neither (this default)              8 289 requests, ~1 in 11 examined
 *
 * One to one is exactly right and worth nothing at 1 508 requests -- and at 10
 * it is a dead board.  MAIN evidently does not re-arm the receive by writing
 * LINK_CTRL_START often enough for that gate; the arm the model keys on
 * (0x246e36, 0x248dc2, 0x251200) is not on the hot path.  So the honest state
 * is: the defect is real, this cure is worse, and the switch keeps the
 * measurement reproducible from the same build rather than from a memory.
 */
static bool cdj_link_rx_one_shot(void)
{
    return getenv("CDJ_LINK_RX_ONESHOT") != NULL;
}

/* The firmware's 16-bit record checksum (GUI 0x00b7cb2a), reproduced.  A
 * 48-byte request stores it little-endian at offset 46 over the first 46
 * bytes -- verified against four request classes out of r156's TX dump, all
 * four exact. */
static uint16_t cdj_link_crc(const uint8_t *data, size_t len)
{
    uint32_t state = 0;
    size_t i;
    unsigned bit;

    for (i = 0; i < len; i++) {
        state |= data[i];
        for (bit = 0; bit < 8; bit++) {
            state <<= 1;
            if (state & (1u << 24)) {
                state ^= 0x01102100;
            }
        }
    }
    for (bit = 0; bit < 16; bit++) {
        state <<= 1;
        if (state & (1u << 24)) {
            state ^= 0x01102100;
        }
    }
    return (state >> 8) & 0xffff;
}

/*
 * CDJ_REQ_KIND=<n> -- make the browse request name the medium that is mounted.
 *
 * Word 4 of a type-1 request is the KIND MAIN answers for (LINK/USB/SD/DISC =
 * 0/1/2/3).  `r156` measured **506 of 506** cursor-1 type-1 requests carrying
 * 1 (USB) on a machine whose only medium is the SD card and whose own status
 * says so (`0x04c084d4 = 1`).  MAIN then answers exactly the question it was
 * asked: `0x150490` hands that word to `0x14e4d4` as both the row index and
 * the selector, `sel 1` stamps slot 3, and slot 3 is not the card -- slot 2
 * is, and it is the only row whose category flag the mount notifier
 * (`0x142c3e`) ever set.
 *
 * So this rewrites the one word, on the way in, and re-stamps the checksum --
 * without that the request is dropped and the switch would silently measure
 * nothing.  It is off unless the variable is set, so the same build gives the
 * A and the B side.
 *
 * **Cursor 1 only, and that is a measured boundary, not caution.**  `r158`
 * rewrote every type-1 request and the GUI never left the left column: 1 779
 * cursor-3 requests, no cursor 11 at all, and an empty right pane.  `r157`,
 * which patched only the cursor-1 arm inside MAIN, saw the GUI ask **cursor 11
 * with word 4 = 2 on its own** 2 223 times and fill the pane with the card's
 * folder and playlist.  Cursor 1 is the only arm that writes `[ctx+4]`
 * (`0x150492`); the other cursors read their own word 4 for their own pane, and
 * overwriting it takes the GUI's answer away from it.  `CDJ_REQ_CURSOR`
 * widens this again for anyone who wants to repeat r158.
 */
static void cdj_link_force_req_kind(uint8_t *frame, unsigned len)
{
    static int kind = -2;
    static int only_cursor;
    static unsigned long rewrites;
    unsigned type, cursor, was;
    uint16_t crc;

    if (kind == -2) {
        const char *env = getenv("CDJ_REQ_KIND");
        const char *which = getenv("CDJ_REQ_CURSOR");

        kind = (env && *env) ? (int)strtol(env, NULL, 0) : -1;
        only_cursor = (which && *which) ? (int)strtol(which, NULL, 0) : 1;
    }
    if (kind < 0 || len != 48) {
        return;
    }
    type = (frame[2] | (frame[3] << 8)) & 0x3fff;
    if (type != 1) {
        return;
    }
    cursor = frame[4] | (frame[5] << 8);
    if (only_cursor >= 0 && cursor != (unsigned)only_cursor) {
        return;
    }
    was = frame[8] | (frame[9] << 8);
    if (was == (unsigned)kind) {
        return;
    }
    frame[8] = (uint8_t)kind;
    frame[9] = (uint8_t)(kind >> 8);
    crc = cdj_link_crc(frame, 46);
    frame[46] = (uint8_t)crc;
    frame[47] = (uint8_t)(crc >> 8);
    if (rewrites++ == 0) {
        info_report("cdj2000: CDJ_REQ_KIND: type-1 word 4 %u -> %d "
                    "(cursor %u), checksum re-stamped", was, kind, cursor);
    }
}

/*
 * CDJ_REQ_NOCANCEL=<ms> -- clear bit 15 of a type-1 request's word 1, at most
 * once every <ms> of virtual time, and re-stamp the checksum.
 *
 * Bit 15 is the bit MAIN's classifier `0x213670` tests first: set goes to
 * `0x21368c`, the arm whose console line is `★★ｷｬﾝｾﾙだ!`; clear goes to
 * `0x2137c4`, the arm that copies `[mgr+20]` into `[mgr+0x93c]` and then does
 * the braf dispatch.  The GUI sets it on nearly every request -- `word1 =
 * 0x8000 | type` -- and clears it exactly when it *opens* a query.  Counted over
 * six saved TX dumps, MAIN's answers follow the cleared bit and nothing else:
 *
 *     run   type-1 total   bit 15 CLEAR   answers longer than 64 B
 *     r165        4 965             12         56  (the card's two entries)
 *     r173        8 280              6         47
 *     r178        1 737              5         50
 *     r179        4 118              4          4
 *     r180       14 539          **0**     **0**
 *     r181       21 691          **0**     **0**
 *
 * and in r165 the twelve sit in the order the panes open -- one cursor 1, then
 * five cursor 3, then six cursor 11 -- with thousands of bit-15-set polls
 * between them.  Both zeros are runs under `BFIN_LINK_NO_ZERO200`, where the
 * GUI polls 24 times a second and never opens anything.
 *
 * The rate limit exists because "clear it on every request" is not the shape
 * the firmware ever sees: r165's GUI opened a query about once every 75 s.  0
 * or 1 rewrites every matching request.  `CDJ_REQ_CURSOR` selects the cursor,
 * default 1, the same boundary `CDJ_REQ_KIND` uses and for the same measured
 * reason.  Off unless set, so one build carries both sides of the A/B.
 */
static void cdj_link_clear_req_cancel(uint8_t *frame, unsigned len)
{
    static int64_t period_ms = -2;
    static int only_cursor;
    static int64_t next_ms;
    static unsigned long rewrites;
    int64_t now_ms;
    unsigned type, cursor, word1;
    uint16_t crc;

    if (period_ms == -2) {
        const char *env = getenv("CDJ_REQ_NOCANCEL");
        const char *which = getenv("CDJ_REQ_CURSOR");

        period_ms = (env && *env) ? (int64_t)strtoll(env, NULL, 0) : -1;
        only_cursor = (which && *which) ? (int)strtol(which, NULL, 0) : 1;
    }
    if (period_ms < 0 || len != 48) {
        return;
    }
    word1 = frame[2] | (frame[3] << 8);
    type = word1 & 0x3fff;
    if (type != 1 || !(word1 & 0x8000)) {
        return;
    }
    cursor = frame[4] | (frame[5] << 8);
    if (only_cursor >= 0 && cursor != (unsigned)only_cursor) {
        return;
    }
    now_ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
    if (now_ms < next_ms) {
        return;
    }
    next_ms = now_ms + period_ms;

    word1 &= ~0x8000u;
    frame[2] = (uint8_t)word1;
    frame[3] = (uint8_t)(word1 >> 8);
    crc = cdj_link_crc(frame, 46);
    frame[46] = (uint8_t)crc;
    frame[47] = (uint8_t)(crc >> 8);
    if (rewrites++ == 0) {
        info_report("cdj2000: CDJ_REQ_NOCANCEL: type-1 cursor %u word 1 bit 15 "
                    "cleared, checksum re-stamped, at most every %" PRId64 " ms",
                    cursor, period_ms);
    }
}

/*
 * CDJ_REQ_STATUS_FRESH -- turn every repeated type-0 (status) request into
 * the GUI's own fresh one (word 0 = 1, bit 15 of word 1 clear), checksum
 * re-stamped, so MAIN answers each one at once.  On by default; 0 sends
 * the GUI's bytes untouched.
 *
 * Measured with the loop cure (CDJ_LINK_LINK_ROWS) in place, SD key at
 * 55 s on a running machine: off, 3 of 9 (m1-m3, h1-h3, k1-k3 with only
 * bit 15 cleared); on, 6 of 8 (n1-n3, t1-t2, p1-p3), the library 0.6-2.6 s
 * after the key.  The two failures are the same picture as before: MAIN
 * answering "0000 0000" polls with its last 48-byte browse answer instead
 * of a status record, and the card's list request with that stale answer.
 *
 * Bit 15 is the repeat/cancel bit: MAIN answers a status request with it
 * clear immediately (measured: 50 a second, 1:1, usb7) and one with it set
 * on its 3 s timer.  The GUI clears it when it opens a query and sets it on
 * every repeat -- so once one answer is missed the GUI repeats, MAIN slows
 * to 3 s, and the GUI keeps repeating: the "slow regime" of every failed
 * SOURCE key.  The simulator's record path on the GUI side gets its turns
 * from arriving status records (dv-bfin_ppi.c, the yield budget), so at 3 s
 * a cadence the card's lists reach the firmware at 0.3 a second and the
 * GUI's browse loop runs for tens of seconds; at 20-30 a second they reach
 * it at once.  Every status record MAIN sends is its live state, so
 * answering a repeat is not an error on the wire, only work MAIN would
 * otherwise defer.
 */
static void cdj_link_status_fresh(uint8_t *frame, unsigned len)
{
    static int enabled = -1;
    static unsigned long rewrites;
    unsigned word1;
    uint16_t crc;

    if (enabled < 0) {
        const char *env = getenv("CDJ_REQ_STATUS_FRESH");

        enabled = !(env && *env == '0');
    }
    if (!enabled || len != 48) {
        return;
    }
    word1 = frame[2] | (frame[3] << 8);
    if ((word1 & 0x3fff) != 0 || !(word1 & 0x8000)) {
        return;
    }
    /*
     * The GUI's own fresh status poll is "0001 0000 ...": word 0 at 1 as
     * well.  Clearing bit 15 alone leaves an all-zero header, and MAIN
     * answers that shape by re-sending its last browse answer (k1: every
     * "0000 0000" poll got a 48-byte 0x11 frame, every "0001 0000" a
     * status record).  So the repeat becomes the fresh poll, both words.
     */
    word1 &= ~0x8000u;
    frame[0] = 1;
    frame[1] = 0;
    frame[2] = (uint8_t)word1;
    frame[3] = (uint8_t)(word1 >> 8);
    crc = cdj_link_crc(frame, 46);
    frame[46] = (uint8_t)crc;
    frame[47] = (uint8_t)(crc >> 8);
    if (rewrites++ == 0) {
        info_report("cdj2000: CDJ_REQ_STATUS_FRESH: type-0 requests have bit "
                    "15 cleared, checksum re-stamped");
    }
}

/*
 * CDJ_LINK_RX_QUEUE -- deliver the GUI's frames one per acknowledge.
 *
 * Every frame used to be written into the guest's buffer the moment it
 * arrived, and START stayed set, so the next frame overwrote it before
 * GuiCom_RcvTASK had looked: r081 measured 439 more type-0 frames examined
 * than sent and one type-1 request in eleven seen.  Clearing START on
 * delivery (CDJ_LINK_RX_ONESHOT) fixed the double read and starved the
 * board, because the GUI's socket then backed up behind a receive MAIN had
 * not re-armed.  The FIFO takes the frame off the socket at once and hands
 * it to the guest when the previous one has been acknowledged in the
 * status register (or the receive re-armed) and CDJ_LINK_RX_GAP_US has
 * passed since it went in, oldest dropped when 64 wait, with a 50 ms
 * watchdog in case a frame is never acknowledged.  What this
 * buys is measured on the SOURCE key: MAIN's status answers and the card's
 * lists reach the GUI at the rate the GUI asks, which is what the GUI's
 * browse loop needs to finish.  CDJ_LINK_RX_QUEUE=0 restores the overwrite.
 */
static bool cdj_link_rx_queue_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("CDJ_LINK_RX_QUEUE");

        enabled = !(env && *env == '0');
    }
    return enabled;
}

/*
 * CDJ_LINK_RX_GAP_US -- the least guest time between two frames going into
 * the buffer.  Default 2000 (2 ms); 0 restores the old immediacy.
 *
 * The GUI's frames reach this board in bursts -- the simulator runs ahead
 * and behind, and TCP batches -- while on the wire they are spaced by the
 * GUI's own cycle: 15-20 a second at rest, 140 a second when it is asking
 * for something (trackload-47/48), i.e. never closer than 7 ms.  MAIN's
 * receive ISR (0x2a3f4c) acknowledges, re-arms and wakes GuiCom_RcvTASK
 * within microseconds, so on the wire the task has always read a frame long
 * before the next one lands; here two frames of a burst went into the buffer
 * 0.1 ms apart and the task read the second twice, or the first not at all
 * (trackload-42: an injected LOAD read twice, "MusicID多重要求", the load
 * failed; trackload-45: 93 of 8263 deliveries closer than 0.5 ms to the one
 * before).  The gap keeps the bursts but spaces the deliveries: a frame that
 * would land less than the gap after the previous one waits in the FIFO and
 * a timer hands it over when the gap is up.  2 ms is well under the GUI's
 * fastest cycle and well over the task's latency.
 */
static int64_t cdj_link_rx_gap_ns(void)
{
    static int64_t gap = -1;

    if (gap < 0) {
        const char *env = getenv("CDJ_LINK_RX_GAP_US");

        gap = (env && *env ? strtoll(env, NULL, 10) : 2000) * 1000LL;
        if (gap < 0) {
            gap = 0;
        }
    }
    return gap;
}

/*
 * CDJ_LINK_RX_HANDOVER=ack|answer -- what lets the next queued frame into the
 * buffer.  Default: ack, i.e. the ISR's acknowledge (or its re-arm), spaced
 * by the gap above.
 *
 * "ack" was the first FIFO: the frame after the current one went in as soon
 * as the ISR wrote the status register back.  That is microseconds after the
 * interrupt, inside the ISR's own loop (0x2a3f4c re-reads the status and
 * services the new frame at once), long before GuiCom_RcvTASK -- woken by
 * that ISR with wup_tsk, 0x2a4030 -- has run.  Two frames then produce two
 * wake-ups but one buffer content, and the task (0x2133d0: tslp_tsk, then
 * 0x21345a examines whatever is at 0xa4500000 while bit 2 of 0xfff10048
 * stands) reads the second frame twice.  Measured in trackload-42-final: a
 * status poll delivered at t=164.9897, the injected LOAD at t=164.9899, and
 * MAIN's console took the load twice ("LOAD_WORK中のｴﾝﾀｰﾛｰﾄﾞ", "MusicID多重
 * 要求") and failed it; trackload-45-final, same recipe, had 106 ms between
 * the two frames and one load.  93 of 8263 deliveries in that run were
 * closer than 0.5 ms to the one before: every one a request that may be
 * read twice or, if it was the first of the pair, not at all.
 *
 * On the wire this cannot happen.  The GUI sends a request, waits for MAIN's
 * status record, and only then sends the next one, so the next frame is
 * never in the buffer before MAIN has answered the current one -- and MAIN
 * answers after GuiCom_RcvTASK has processed it (the transmit at 0x2a3cf6
 * clears its own frame-pending flag 0x7db353c).  "answer" therefore hands the
 * next frame over when MAIN's transmit half sends, whatever it sends, or after
 * the 50 ms watchdog if it never does; the ISR's acknowledge no longer moves
 * the queue -- and neither does the re-arm, because the ISR re-arms the
 * receive itself (0x2a3ff4, before the wake-up): trackload-47-launch, with
 * the acknowledge alone disarmed, handed 713 of 749 queued frames over on
 * that arm, inside the ISR's loop.  So in this mode only MAIN's transmit and
 * the watchdog move the queue.
 *
 * Measured, and that is why it is not the default: MAIN does not answer
 * frames, it sends a status record on its own cycle, about 16 a second
 * whatever the GUI sends (3-launch 17/s, 45 16/s, 47 13/s, 48 16/s).  Paced
 * by those, the FIFO fell behind the GUI (trackload-48-launch2: 45 497
 * frames queued, 39 788 dropped, 4 909 handed over by the 50 ms watchdog),
 * the GUI asked again and again (57 000 frames for 6 301 records), and a
 * record sent while GuiCom_RcvTASK was still reading the buffer replaced the
 * frame under it -- the task's CRC-error count 0x489bc88 reached 298, against
 * 1 with the gap.  Kept for the A/B; the gap is the model of the wire.
 */
static bool cdj_link_rx_handover_on_answer(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("CDJ_LINK_RX_HANDOVER");

        enabled = env && !strcmp(env, "answer");
    }
    return enabled;
}

/* MAIN answered: the receive half may take the next waiting frame. */
static void cdj_link_rx_answered(CdjLinkState *rx)
{
    if (rx->rx_pending) {
        rx->n_answered++;
        cdj_link_rx_next(rx);
    }
}

/* The last request handed to the guest: type, cursor, KIND (type-1 words). */
static unsigned cdj_last_req_type = ~0u, cdj_last_req_cursor, cdj_last_req_kind;

/*
 * CDJ_LINK_LINK_ROWS=match|drop|empty|off -- MAIN's answer to the GUI's
 * LINK browse.  Default: match.
 *
 * After the player screen the GUI browses the LINK source (type 1, cursor
 * 3, KIND 0) in a loop, 30-40 requests a second.  MAIN -- with no network
 * behind its LINK source -- answers each of those with the one-row list
 * "NO DISC" (id 88) stamped as command 0x11, the answer to a cursor-1
 * request; the GUI's consumer 0xb7eb48 compares the command's low nibble
 * with the cursor of the request it has outstanding (3), sets an error
 * bit on the mismatch and re-sends the request.  That is the loop, and
 * while it runs a SOURCE key opens the library screen but the card's list
 * request never goes out (f1-f3, v1-v4: 0 of 6).  The loop ended on its
 * own only when the GUI was slowed down (three machines on the host, or
 * probes on: e2, e4, e5), and the e4 dump shows why: under load MAIN's
 * answers to the same polls came stamped 0x13.
 *
 * "match" stamps the low nibble of the command with the cursor of the
 * request being answered and re-stamps the checksum -- the loop is over
 * by 40 s in 3 of 3 (m1-m3) and the SD key then brings the card's library
 * in 1.5 s (m3).  "empty" (count 0, row blanked) and "drop" (not sent)
 * were tried first and change nothing (r1, r2): the loop is not about the
 * row, it is about the nibble.  "off" sends MAIN's bytes untouched.
 */
static int cdj_link_link_rows_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *env = getenv("CDJ_LINK_LINK_ROWS");

        mode = 3;
        if (env && !strcmp(env, "drop")) {
            mode = 1;
        } else if (env && !strcmp(env, "empty")) {
            mode = 2;
        } else if (env && (!strcmp(env, "off") || !strcmp(env, "0"))) {
            mode = 0;
        }
    }
    return mode;
}

/* Returns false when the frame is not to be sent. */
static bool cdj_link_link_rows(uint8_t *frame, unsigned len)
{
    static unsigned long rewrites;
    int mode = cdj_link_link_rows_mode();
    uint16_t crc;

    if (!mode || len != 48 || cdj_last_req_type != 1) {
        return true;
    }
    if (mode == 3) {
        /*
         * "match": the answer's command carries the cursor it answers in
         * its low nibble (0x11 cursor 1, 0x13 cursor 3) and the GUI's
         * consumer 0xb7eb48 compares that nibble with the cursor of the
         * request it has outstanding -- a mismatch sets an error bit and
         * the request is re-sent.  MAIN answers the LINK browse's cursor-3
         * polls with 0x11 (e4 dump: 62 of them before the first 0x13),
         * which is the loop.  Stamp the nibble the GUI is waiting for.
         */
        unsigned cmd = frame[0] | (frame[1] << 8);
        unsigned want = (cmd & 0xfff0) | (cdj_last_req_cursor & 0xf);

        if ((cmd & 0xff00) != 0 || cmd == want || cdj_last_req_cursor > 15) {
            return true;
        }
        frame[0] = (uint8_t)want;
        crc = cdj_link_crc(frame, 46);
        frame[46] = (uint8_t)crc;
        frame[47] = (uint8_t)(crc >> 8);
        if (rewrites++ == 0) {
            info_report("cdj2000: CDJ_LINK_LINK_ROWS=match: answer command "
                        "0x%02x -> 0x%02x for the cursor-%u request",
                        cmd, want, cdj_last_req_cursor);
        }
        return true;
    }
    if (cdj_last_req_cursor != 3 || cdj_last_req_kind != 0) {
        return true;
    }
    if ((frame[0] | (frame[1] << 8)) != 0x0011) {
        return true;
    }
    if (rewrites++ == 0) {
        info_report("cdj2000: CDJ_LINK_LINK_ROWS=%s: MAIN's one-row answer "
                    "to the LINK browse (type 1 cursor 3 KIND 0) is %s",
                    mode == 1 ? "drop" : "empty",
                    mode == 1 ? "not sent" : "sent with no rows");
    }
    if (mode == 1) {
        return false;
    }
    memset(frame + 16, 0, 46 - 16);       /* count, id, length, text */
    crc = cdj_link_crc(frame, 46);
    frame[46] = (uint8_t)crc;
    frame[47] = (uint8_t)(crc >> 8);
    return true;
}

static void cdj_link_deliver(CdjLinkState *link, const uint8_t *frame,
                             unsigned len)
{
    uint8_t copy[512];

    memcpy(copy, frame, len);
    cdj_link_force_req_kind(copy, len);
    cdj_link_clear_req_cancel(copy, len);
    cdj_link_status_fresh(copy, len);
    if (len == 48) {
        cdj_last_req_type = (copy[2] | (copy[3] << 8)) & 0x3fff;
        cdj_last_req_cursor = copy[4] | (copy[5] << 8);
        cdj_last_req_kind = copy[8] | (copy[9] << 8);
    }
    address_space_write(&address_space_memory, cdj_dma_phys(link->buffer),
                        MEMTXATTRS_UNSPECIFIED, copy, len);

    if (cdj_link_rx_one_shot()) {
        link->control &= ~LINK_CTRL_START;
    }

    link->status |= link->rx_status;
    link->rx_pending = true;
    link->rx_delivered_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (link->rx_watchdog) {
        timer_mod(link->rx_watchdog, link->rx_delivered_ns + 50 * SCALE_MS);
    }
    if (link->flag) {
        cdj_link_flag_rx(link->flag, cdj_link_flag_pending());
    }
    /* The request header, little-endian halfwords as the GUI lays them
     * out: 0 tag, 1 type, 2 cursor, 3 word 3, 4 KIND, 5 word 5. */
    qemu_log_mask(LOG_UNIMP, "%s: delivered %u bytes to 0x%08x, status 0x%02x"
                  " words %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x"
                  " queued %u t=%.4f\n",
                  link->name, len, link->buffer, link->status,
                  copy[1], copy[0], copy[3], copy[2], copy[5], copy[4],
                  copy[7], copy[6], copy[9], copy[8], copy[11], copy[10],
                  link->queue_count,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    link->n_rx++;
    cdj_link_census(link);
    cdj_intc2_set(link, true);
}

/*
 * The buffer is free: hand the oldest waiting frame over -- unless the gap
 * since the last delivery is not up yet, in which case a timer does it.
 */
static void cdj_link_rx_release(CdjLinkState *link)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t due = link->rx_delivered_ns + cdj_link_rx_gap_ns();
    unsigned slot;

    if (link->rx_pending || !link->queue_count) {
        return;
    }
    if (now < due) {
        if (link->rx_gap && !timer_pending(link->rx_gap)) {
            link->n_gapped++;
            timer_mod(link->rx_gap, due);
        }
        return;
    }
    slot = link->queue_head;
    link->queue_head = (slot + 1) % CDJ_LINK_RX_QUEUE_MAX;
    link->queue_count--;
    cdj_link_deliver(link, link->queue[slot], link->queue_len[slot]);
}

static void cdj_link_rx_gap_timer(void *opaque)
{
    cdj_link_rx_release(opaque);
}

/* The guest acknowledged (or re-armed): hand over the next frame waiting. */
static void cdj_link_rx_next(CdjLinkState *link)
{
    link->rx_pending = false;
    if (link->rx_watchdog) {
        timer_del(link->rx_watchdog);
    }
    cdj_link_rx_release(link);
}

static void cdj_link_rx_watchdog(void *opaque)
{
    CdjLinkState *link = opaque;

    if (link->rx_pending) {
        link->n_watchdog++;
        cdj_link_rx_next(link);
    }
}

static void cdj_link_receive(void *opaque, const uint8_t *data, int size)
{
    CdjLinkState *link = opaque;
    unsigned frame = cdj_link_frame_len(link);

    if (size <= 0 || !frame) {
        return;
    }
    if (link->rx_filled + size > sizeof(link->rx)) {
        size = sizeof(link->rx) - link->rx_filled;
    }
    memcpy(link->rx + link->rx_filled, data, size);
    link->rx_filled += size;

    if (link->rx_filled < frame) {
        return;                 /* partial frame, wait for the rest */
    }
    link->rx_filled = 0;

    if (cdj_link_rx_queue_enabled() && frame <= sizeof(link->queue[0])) {
        if (link->rx_pending || link->queue_count
            || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
               < link->rx_delivered_ns + cdj_link_rx_gap_ns()) {
            unsigned slot;

            if (link->queue_count == CDJ_LINK_RX_QUEUE_MAX) {
                link->queue_head = (link->queue_head + 1)
                                   % CDJ_LINK_RX_QUEUE_MAX;
                link->queue_count--;
                link->n_dropped++;
            }
            slot = (link->queue_head + link->queue_count)
                   % CDJ_LINK_RX_QUEUE_MAX;
            memcpy(link->queue[slot], link->rx, frame);
            link->queue_len[slot] = frame;
            link->queue_count++;
            link->n_queued++;
            cdj_link_rx_release(link);      /* arms the gap timer if that is all */
            return;
        }
        cdj_link_deliver(link, link->rx, frame);
        return;
    }

    cdj_link_force_req_kind(link->rx, frame);
    cdj_link_clear_req_cancel(link->rx, frame);
    cdj_link_status_fresh(link->rx, frame);
    address_space_write(&address_space_memory, cdj_dma_phys(link->buffer),
                        MEMTXATTRS_UNSPECIFIED, link->rx, frame);

    if (cdj_link_rx_one_shot()) {
        link->control &= ~LINK_CTRL_START;
    }

    link->status |= link->rx_status;
    if (link->flag) {
        cdj_link_flag_rx(link->flag, cdj_link_flag_pending());
    }
    /* The request header, little-endian halfwords as the GUI lays them
     * out: 0 tag, 1 type, 2 cursor, 3 word 3, 4 KIND, 5 word 5. */
    qemu_log_mask(LOG_UNIMP, "%s: delivered %u bytes to 0x%08x, status 0x%02x"
                  " words %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x"
                  " t=%.4f\n",
                  link->name, frame, link->buffer, link->status,
                  link->rx[1], link->rx[0], link->rx[3], link->rx[2],
                  link->rx[5], link->rx[4], link->rx[7], link->rx[6],
                  link->rx[9], link->rx[8], link->rx[11], link->rx[10],
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    link->n_rx++;
    cdj_link_census(link);
    cdj_intc2_set(link, true);
}

/*
 * CDJ_COSIM: the GUI's request bytes reach MAIN at their guest time
 * (cdj2000_cosim.c) and wait here until MAIN has a receive armed -- exactly
 * what the chardev did with them, whose can_read said the same thing.
 */
static uint8_t cdj_link_cosim_backlog[65536];
static unsigned cdj_link_cosim_backlog_len;

static void cdj_link_cosim_pump(CdjLinkState *link)
{
    while (cdj_link_cosim_backlog_len) {
        int room = cdj_link_can_receive(link);
        unsigned n = cdj_link_cosim_backlog_len;

        if (room <= 0) {
            return;
        }
        if (n > (unsigned)room) {
            n = room;
        }
        cdj_link_receive(link, cdj_link_cosim_backlog, n);
        memmove(cdj_link_cosim_backlog, cdj_link_cosim_backlog + n,
                cdj_link_cosim_backlog_len - n);
        cdj_link_cosim_backlog_len -= n;
    }
}

static void cdj_link_cosim_poll(void *opaque)
{
    cdj_link_cosim_pump(opaque);
}

static void cdj_link_cosim_request(void *opaque, const uint8_t *data,
                                   unsigned len)
{
    CdjLinkState *link = opaque;

    if (len > sizeof(cdj_link_cosim_backlog) - cdj_link_cosim_backlog_len) {
        warn_report_once("cdj2000: co-simulated requests overflowed the "
                         "backlog; the oldest are dropped");
        cdj_link_cosim_backlog_len = 0;
        if (len > sizeof(cdj_link_cosim_backlog)) {
            return;
        }
    }
    memcpy(cdj_link_cosim_backlog + cdj_link_cosim_backlog_len, data, len);
    cdj_link_cosim_backlog_len += len;
    cdj_link_cosim_pump(link);
}

static uint64_t cdj_intc2_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjIntc2State *intc2 = opaque;

    if (offset == INTC2_STATUS2_OFFSET) {
        return intc2->status2;
    }
    return offset ? 0 : intc2->status;
}

static void cdj_intc2_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    /* Status is driven by the devices; the guest only reads it. */
}

static const MemoryRegionOps cdj_intc2_ops = {
    .read = cdj_intc2_read,
    .write = cdj_intc2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static CdjLinkState *cdj_link_init(MemoryRegion *system, CdjIntc2State *intc2,
                                   CdjLinkFlagState *flag,
                                   const char *name, uint32_t base,
                                   uint32_t mode_base, uint32_t intc2_bit,
                                   uint32_t buffer_off, uint32_t length_off,
                                   bool transmit, CdjLinkState *owner,
                                   qemu_irq irq, Chardev *chr)
{
    CdjLinkState *link = g_new0(CdjLinkState, 1);

    const char *override = getenv("CDJ_LINK_RX_STATUS");

    link->flag = flag;
    link->buffer_off = buffer_off;
    link->length_off = length_off;
    link->transmit = transmit;
    link->owner = owner;
    link->intc2 = intc2;
    link->name = name;
    if (transmit) {
        link->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_link_tx_complete,
                                      link);
    } else {
        link->rx_watchdog = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         cdj_link_rx_watchdog, link);
        link->rx_gap = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    cdj_link_rx_gap_timer, link);
    }
    link->base = base;
    link->intc2_bit = intc2_bit;
    link->irq = irq;
    link->rx_status = override ? strtoul(override, NULL, 0) : LINK_ST_COMPLETE;

    memory_region_init_io(&link->regs, NULL, &cdj_link_ops, link,
                          name, LINK_SIZE);
    memory_region_add_subregion(system, base, &link->regs);

    memory_region_init_io(&link->mode, NULL, &cdj_link_mode_ops, link,
                          name, LINK_MODE_SIZE);
    memory_region_add_subregion(system, mode_base, &link->mode);

    /*
     * Only the receiving half owns the chardev — a Chardev takes exactly one
     * frontend, and the transmitting half writes through its peer's.
     */
    /*
     * Each half owns its own chardev: a Chardev accepts exactly one frontend,
     * so the two directions cannot share one.  Receive is -serial 0, transmit
     * is -serial 1.
     */
    if (chr) {
        qemu_chr_fe_init(&link->chr, chr, &error_abort);
        link->connected = true;
        /*
         * Handlers must be registered even on the transmit-only half: a
         * chardev is not opened until a frontend attaches to it, and writes to
         * an unopened socket backend just fail.
         */
        qemu_chr_fe_set_handlers(&link->chr,
                                 transmit ? NULL : cdj_link_can_receive,
                                 transmit ? NULL : cdj_link_receive,
                                 NULL, NULL, link, NULL, true);
    }
    return link;
}

/*
 * MAIN's own debug console.  The firmware carries Tx232c/Rx232c/Er232c tasks
 * and a printf at 0x219568, and the register use at 0xffe00000 is a SCIF's:
 * a byte write to +0x04 (bit rate), +0x18 written 6 then 0x80 (FIFO control),
 * and polling of +0x10 and +0x24.  Wiring QEMU's sh_serial there makes the
 * firmware narrate itself — including the "GUIcmd:" command matrix and
 * "Unknown ListKind(%d) from GUI".
 */
#define SCIF_BASE       0xffe00000
#define SCIF_SIZE       0x40
#define SCIF_SCR        0x08    /* serial control */
#define SCIF_TDR        0x0c    /* transmit FIFO data */
#define SCIF_FSR        0x10    /* status */
#define SCIF_RDR        0x14    /* receive FIFO data */
#define SCIF_FSR_DR     0x01    /* receive data present, below the trigger */
#define SCIF_FSR_RDF    0x02    /* receive FIFO reached its trigger level */
#define SCIF_FSR_TDFE   0x20    /* transmit FIFO empty */
#define SCIF_FSR_TEND   0x40    /* transmission ended */
#define SCIF_SCR_RIE    0x40    /* receive interrupt enable */
#define SCIF_SCR_TIE    0x80    /* transmit interrupt enable */
/*
 * The real part has a 16-byte receive FIFO, and modelling it is not decoration.
 * A one-byte holding register forces the front end into flow control — a
 * can_receive that answers zero detaches the chardev's read handler until
 * qemu_chr_fe_accept_input runs — and one missed re-arm silently stops all
 * further input: the guest looks fine, the socket accepts writes, and nothing
 * ever arrives again.  With the FIFO there is always room and the question does
 * not arise.
 */
#define SCIF_FIFO_DEPTH 16

/*
 * Both halves are modelled, but only as far as the firmware actually uses them.
 *
 * Transmit: the status register always reports the transmitter idle, so the
 * character loop at 0x29f13a — which spins on TDFE with no timeout and no RTOS
 * yield — never waits, and every byte written to the FIFO goes straight out.
 *
 * Receive: this is what carries MAIN's service monitor.  The receive ISR
 * 0x102468 (INTEVT 0x720) proceeds when FSR has RDF or DR set, reads one byte
 * from RDR, appends it to the line buffer at 0x045a1802 echoing as it goes, and
 * on CR shifts the line across and wakes the Rx232c task, which posts it to
 * cmdman.  cmdman matches it against the 38-entry command table at 0xa405c310
 * — WB/WW/WL to write memory, RB/RW/RL to read it, UG for the debug dump.
 *
 * The receive flags are write-to-clear *per bit*, and modelling that exactly is
 * what makes the ISR terminate.  It clears them one at a time
 * (0x102554-0x102562):
 *
 *     FSR &= ~RDF    -> writes 0x61, DR still set
 *     FSR &= ~DR     -> writes 0x62, RDF still set
 *
 * so neither write ever has both bits clear.  A model that regenerates both
 * bits on every read can therefore never be acknowledged: the ISR returns with
 * the source still asserted, and because the level is held it is re-entered
 * immediately — measured at about 14 500 interrupts a second, which starves
 * everything else without ever looking like a crash.  Keep the two bits as
 * state and let each write clear the bits it presents as zero.
 *
 * Transmit needs an interrupt as well, and without it the console goes quiet
 * after its first few characters.  Output is queued by 0x1026be into an RTOS
 * queue and drained by a task, and that task is woken by the transmit
 * interrupt: the ISR 0x10274c (INTEVT 0x760) signals it and then clears TIE
 * with SCSCR2 &= 0xff7f.  So TIE means "tell me when the transmitter has
 * room", and since this transmitter is always idle the line is simply asserted
 * whenever TIE is set — the handler clearing TIE is what lowers it again.
 *
 * QEMU's own sh_serial would do more but insists on a chardev with an id, which
 * rules out a plain "-serial null" slot.
 */
typedef struct {
    MemoryRegion iomem;
    CharFrontend chr;
    bool connected;

    qemu_irq rx_irq;
    qemu_irq tx_irq;
    uint16_t scr;       /* the guest's SCSCR2; bit 6 gates the interrupt */
    uint16_t rx_flags;  /* the RDF and DR bits, as the guest last left them */
    uint8_t fifo[SCIF_FIFO_DEPTH];
    unsigned int head, count;
} CdjConsoleState;

static void cdj_console_update_irq(CdjConsoleState *console)
{
    if (console->rx_irq) {
        qemu_set_irq(console->rx_irq,
                     console->rx_flags && (console->scr & SCIF_SCR_RIE));
    }
    if (console->tx_irq) {
        /* TDFE is permanently set here, so TIE alone decides. */
        qemu_set_irq(console->tx_irq, !!(console->scr & SCIF_SCR_TIE));
    }
}

static uint64_t cdj_console_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjConsoleState *console = opaque;

    switch (offset) {
    case SCIF_SCR:
        return console->scr;
    case SCIF_FSR:
        return SCIF_FSR_TDFE | SCIF_FSR_TEND | console->rx_flags;
    case SCIF_RDR:
        /* Reading the data register pops one byte off the FIFO. */
        if (console->count) {
            uint8_t byte = console->fifo[console->head];

            console->head = (console->head + 1) % SCIF_FIFO_DEPTH;
            console->count--;
            return byte;
        }
        return 0;
    default:
        return 0;
    }
}

static void cdj_console_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    CdjConsoleState *console = opaque;
    uint8_t byte = value;

    switch (offset) {
    case SCIF_SCR:
        console->scr = value;
        cdj_console_update_irq(console);
        break;
    case SCIF_TDR:
        if (console->connected) {
            qemu_chr_fe_write_all(&console->chr, &byte, 1);
        }
        break;
    case SCIF_FSR:
        /*
         * Write-to-clear, bit by bit: a bit written as zero is acknowledged,
         * one written as one is left alone.  Once the guest has cleared both,
         * they come straight back if the FIFO still holds anything — the ISR
         * takes exactly one byte per interrupt, so the source has to re-assert
         * for each of them.
         */
        if (console->rx_flags) {
            console->rx_flags &= value;
            if (!console->rx_flags) {
                if (console->count) {
                    console->rx_flags = SCIF_FSR_RDF | SCIF_FSR_DR;
                }
                cdj_console_update_irq(console);
                qemu_chr_fe_accept_input(&console->chr);
            }
        }
        break;
    default:
        break;
    }
}

static int cdj_console_can_receive(void *opaque)
{
    CdjConsoleState *console = opaque;

    return SCIF_FIFO_DEPTH - console->count;
}

static void cdj_console_receive(void *opaque, const uint8_t *buf, int size)
{
    CdjConsoleState *console = opaque;
    int i;

    for (i = 0; i < size && console->count < SCIF_FIFO_DEPTH; i++) {
        console->fifo[(console->head + console->count) % SCIF_FIFO_DEPTH] =
            buf[i];
        console->count++;
    }
    if (console->count) {
        console->rx_flags = SCIF_FSR_RDF | SCIF_FSR_DR;
        cdj_console_update_irq(console);
    }
}

static const MemoryRegionOps cdj_console_ops = {
    .read = cdj_console_read,
    .write = cdj_console_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_console_init(MemoryRegion *system, Chardev *chr,
                             qemu_irq rx_irq, qemu_irq tx_irq)
{
    CdjConsoleState *console = g_new0(CdjConsoleState, 1);

    memory_region_init_io(&console->iomem, NULL, &cdj_console_ops, console,
                          "cdj2000.console", SCIF_SIZE);
    memory_region_add_subregion(system, SCIF_BASE, &console->iomem);
    console->rx_irq = rx_irq;
    console->tx_irq = tx_irq;

    if (chr) {
        qemu_chr_fe_init(&console->chr, chr, &error_abort);
        qemu_chr_fe_set_handlers(&console->chr, cdj_console_can_receive,
                                 cdj_console_receive, NULL, NULL, console,
                                 NULL, true);
        console->connected = true;
    }
}

/*
 * Turning the debug output on.
 *
 * Having a working SCIF is only half of it.  Every caller of the string
 * printer 0x29f126 sits behind `if ([0x07db3440] == 0) return`, and that word
 * is read at eight sites (0x29e83e, 0x29ead4, 0x29eb48, 0x29ebd2, 0x29ede6,
 * 0x29ee6e, 0x29f216, 0x29f280) and written by nothing anywhere in the image —
 * on a real unit the service-mode shell pokes it.  With it clear the firmware
 * is silent no matter how well the serial port works.
 *
 * It cannot be set at reset, because it lives in RAM the boot clears, so it is
 * written on a virtual-clock timer once the RTOS is up.  CDJ_DEBUG_CONSOLE
 * gives the delay in seconds of guest time and defaults to five.
 *
 * The flag is only the master switch.  What actually gets printed is chosen by
 * a verbosity threshold at 0x0489bcb4, and the firmware's own way of setting it
 * is a GUI command: the handler at 0x2130a0, inside GuiCom_RcvTASK's
 * dispatcher, prints one of the "GUIcmd: $$$ Dubug Dump ON + Level=..."
 * strings at 0x7271c-0x727a0 and stores
 *
 *      0xff  Debug Dump OFF          (0x2130be)
 *       200  ON + Level=ERR          (0x2130d0)
 *       100  ON + Level=CMD          (0x2130e2)
 *        10  ON, the verbose level   (0x2130f2)
 *
 * so a lower number lets more through.  CDJ_DEBUG_LEVEL picks it and defaults
 * to 10; setting it directly saves having to synthesise the GUI command.
 *
 * This is a debug aid, not hardware: a stock CDJ boots with the console off,
 * so it stays off unless asked for.
 */
#define DEBUG_CONSOLE_FLAG  0x07db3440
#define DEBUG_CONSOLE_LEVEL 0x0489bcb4

static void cdj_debug_console_write(hwaddr address, uint32_t value)
{
    uint32_t word = cpu_to_le32(value);

    address_space_write(&address_space_memory, address,
                        MEMTXATTRS_UNSPECIFIED, &word, sizeof(word));
}

/*
 * CDJ_MAIN_POKE — hold chosen SDRAM words at chosen values from inside the
 * model, without stopping the guest.
 *
 * `boot_vm --poke` writes through the gdb stub, and every write stops and
 * restarts the machine.  r121 held two words at 2 Hz for 230 s and paid two
 * thirds of its throughput: 3 269 requests against r118's 8 917 in a *shorter*
 * run, and not one type-1 browse request in the whole dump.  That is enough to
 * keep a run out of the phase it was meant to measure, so the flag arrives and
 * the measurement is worthless anyway.  A timer inside the model writes the
 * same words and costs nothing measurable.
 *
 *   CDJ_MAIN_POKE=0x489db00=1,0x489bd6c=2
 *   CDJ_MAIN_POKE_AT=<virtual seconds before the first write, default 60>
 *   CDJ_MAIN_POKE_EVERY_MS=<rewrite interval, default 100; 0 writes once>
 *
 * An entry may be ADDR=VALUE/RATE[@AT]: RATE is added per second of guest
 * time from that entry's own start second AT (default the global one), so
 * the word advances instead of being held -- 0x4832214=0/75@230 makes the
 * deck's position word (trackload-64/65: the status record's time fields
 * come from it, in CD sectors, -1 = blank) count 75 a second from PLAY at
 * 230 s.  That stands in for the DSP's position report, whose form is still
 * open (trackload-55..82), and proves the chain to the GUI's time display.
 *
 * Addresses are physical, as everywhere else in this file, and the values are
 * 32-bit little-endian — the width of MAIN's one-shot flags and state words.
 * This is a diagnostic: nothing here runs unless the variable is set.
 */
#define CDJ_MAIN_POKE_MAX 8

typedef struct CdjMainPoke {
    hwaddr address[CDJ_MAIN_POKE_MAX];
    uint32_t value[CDJ_MAIN_POKE_MAX];
    int64_t rate[CDJ_MAIN_POKE_MAX];        /* added per second, 0 = hold */
    int64_t start_ns[CDJ_MAIN_POKE_MAX];    /* this entry's first write */
    unsigned count;
    uint64_t period_ns;
    QEMUTimer *timer;
} CdjMainPoke;

static void cdj_main_poke_fire(void *opaque)
{
    CdjMainPoke *poke = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned i;

    for (i = 0; i < poke->count; i++) {
        uint32_t value = poke->value[i];
        uint32_t word;

        if (now < poke->start_ns[i]) {
            continue;
        }
        if (poke->rate[i]) {
            value += (uint32_t)(poke->rate[i] * ((now - poke->start_ns[i]) / SCALE_MS) / 1000);
        }
        word = cpu_to_le32(value);
        address_space_write(&address_space_memory, poke->address[i],
                            MEMTXATTRS_UNSPECIFIED, &word, sizeof(word));
    }
    if (poke->period_ns) {
        timer_mod(poke->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + poke->period_ns);
    }
}

static void cdj_main_poke_init(void)
{
    const char *spec = getenv("CDJ_MAIN_POKE");
    const char *at = getenv("CDJ_MAIN_POKE_AT");
    const char *every = getenv("CDJ_MAIN_POKE_EVERY_MS");
    CdjMainPoke *poke;
    uint64_t seconds;
    uint64_t period_ms;
    char *copy, *token, *save = NULL;

    if (!spec || !*spec) {
        return;
    }
    poke = g_new0(CdjMainPoke, 1);
    copy = g_strdup(spec);
    for (token = strtok_r(copy, ",", &save); token && poke->count <
             CDJ_MAIN_POKE_MAX; token = strtok_r(NULL, ",", &save)) {
        char *equals = strchr(token, '=');

        if (!equals) {
            continue;
        }
        char *slash = strchr(equals + 1, '/');
        char *atsign = strchr(equals + 1, '@');

        *equals = '\0';
        poke->address[poke->count] = strtoull(token, NULL, 0);
        poke->value[poke->count] = strtoul(equals + 1, NULL, 0);
        poke->rate[poke->count] = slash ? strtoll(slash + 1, NULL, 0) : 0;
        poke->start_ns[poke->count] = atsign
            ? (int64_t)strtoull(atsign + 1, NULL, 0) * NANOSECONDS_PER_SECOND : -1;
        poke->count++;
    }
    g_free(copy);
    if (!poke->count) {
        g_free(poke);
        return;
    }
    seconds = at ? strtoull(at, NULL, 0) : 60;
    {
        unsigned i;

        for (i = 0; i < poke->count; i++) {
            if (poke->start_ns[i] < 0) {
                poke->start_ns[i] = (int64_t)seconds * NANOSECONDS_PER_SECOND;
            }
        }
    }
    period_ms = every ? strtoull(every, NULL, 0) : 100;
    poke->period_ns = period_ms * SCALE_MS;
    poke->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_main_poke_fire, poke);
    timer_mod(poke->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           seconds * NANOSECONDS_PER_SECOND);
    fprintf(stderr, "cdj2000-poke: %u word(s) from %" PRIu64 " s, every %"
            PRIu64 " ms\n", poke->count, seconds, period_ms);
}

static void cdj_debug_console_arm(void *opaque)
{
    uint32_t level = (uint32_t)(uintptr_t)opaque;

    cdj_debug_console_write(DEBUG_CONSOLE_FLAG, 1);
    cdj_debug_console_write(DEBUG_CONSOLE_LEVEL, level);
    qemu_log_mask(LOG_UNIMP, "cdj2000: debug console on, level %u\n", level);
}

static void cdj_debug_console_init(void)
{
    const char *delay = getenv("CDJ_DEBUG_CONSOLE");
    const char *verbosity = getenv("CDJ_DEBUG_LEVEL");
    QEMUTimer *timer;
    uint64_t seconds;
    uint32_t level;

    if (!delay) {
        return;
    }
    seconds = strtoull(delay, NULL, 0);
    if (!seconds) {
        seconds = 5;
    }
    level = verbosity ? strtoul(verbosity, NULL, 0) : 10;
    timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_debug_console_arm,
                         (void *)(uintptr_t)level);
    timer_mod(timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     seconds * NANOSECONDS_PER_SECOND);
}

/*
 * The SD card slot: a Renesas SDHI (TMIO-compatible) host controller.
 *
 * It is identified by the command encodings the driver at file offsets
 * 0x1ff000-0x200b00 writes to +0x00 — 0x300 is CMD0 GO_IDLE_STATE, 0x408 with
 * argument 0x1aa is CMD8 SEND_IF_COND, 0x437 CMD55, 0x446 ACMD6
 * SET_BUS_WIDTH, 0x410 CMD16, 0x1c4d ACMD13 — and by +0x28 flipping 0xc0ee to
 * 0x40ee right after ACMD6, which is the 1-bit to 4-bit bus switch.  The
 * offsets are the Linux TMIO MMC ones: status at +0x1c, data at +0x30, clock
 * control at +0x24, DMA enable at +0xd8.
 *
 * Against the catch-all trap, which answers zero, this device costs about a
 * quarter of the guest's CPU and contains one loop that never ends:
 *
 *   0x1ff45c  sd_wait_info1(mask), unrolled twice inside five inside a 65535
 *             iteration outer loop, so ~655 000 MMIO reads, yielding to the
 *             RTOS only every eighth outer pass.  Callers 0x1ff518 (mask 1,
 *             CMDRESPEND, the hot one), 0x1ffd8c and 0x1ffff6 (mask 4).
 *   0x1ff4b8  spins on +0x1e mask 0x0100, RXRDY.
 *   0x1ff846  spins on +0x1e bit 14, CMD_BUSY, with no iteration cap at all.
 *
 * The model is the smallest one that says "the slot is empty" without lying:
 * every wait is satisfied on its first read, and the driver is told the
 * command timed out rather than being left to retry.
 *
 * Returning 0xffff from +0x1c instead would set bit 5, SIGSTATE, which asserts
 * card-present and drives the 30-iteration ACMD41 initialisation loop at
 * 0x1ff678 -- a slower machine and a card that is not there.
 */
/*
 * The register map above is read off the driver rather than a datasheet;
 * 0x1ff518 fixes all of it in nine instructions.  The command word carries the
 * response type and the data phase in the same encoding Linux's tmio_mmc uses,
 * which is how the observed words decode: 0x300 CMD0 (no response),
 * 0x408 CMD8 (R1), 0x437 CMD55,
 * 0x446 ACMD6, 0x1c4d ACMD13 (R1 + data + read).  Bit 6 marks an application
 * command, which is the firmware's own bookkeeping and not part of the request.
 */
/*
 * The SD host's interrupt.  Read out of the running machine rather than guessed:
 * the RTOS vector table at 0x04fcd904 is indexed by irq, each thunk carries its
 * ISR at -0x14 as a P0 address, and irq 0x4e resolves to 0x041fffa4 -- a handler
 * that tests INFO2 bit 8 (RXRDY), clears it by writing back ~0x100, and sets the
 * event flag the block-read loop in 0x1ffd8c waits on.  Its priority is the 7
 * the driver programs into the second INTC2 block at 0x1feeb8.
 *
 * irq 0x3c and 0x3d share 0x041ff148, the card-detect side; they are not what
 * the data phase waits for and are left alone.
 */
#define SDHI_IRQ        0x4e
#define INTEVT_SDHI     (SDHI_IRQ * 0x20)   /* 0x9c0 */
/*
 * The block transfer's completion.  irq 0x3c and 0x3d share ISR 0x1ff148,
 * which sets event bit 4 -- the second wait in the read loop at 0x1ffd8c,
 * the one taken after the DMA has moved the block.
 */
#define SDHI_DMA_IRQ    0x3c
#define INTEVT_SDHI_DMA (SDHI_DMA_IRQ * 0x20)   /* 0x780 */
#define SDHI_PRIO       7

/*
 * The audio DSP's DMA completion.  The RTOS vector table registers 0x1c7ba2 on
 * irq 0x3d, and that handler is unmistakably the DSP's: it clears bits 2, 0 and
 * 1 of CHCR at 0xff60808c and then sets cflgDspDmaEnd.
 *
 * Note the neighbour above: this board also drives irq 0x3c for the SD block
 * transfer, on the *same* DMAC channel.  One channel, two consumers, two
 * vectors — which is why the role is decided from the transfer's endpoints and
 * never from the channel number.
 */
#define DSP_DMA_IRQ     0x3d
#define INTEVT_DSP_DMA  (DSP_DMA_IRQ * 0x20)    /* 0x7a0 */
#define DSP_PRIO        4
/* The DSP's own interrupt to MAIN; see CdjDspEventSink. */
#define DSP_EVENT_IRQ   0x7f
#define INTEVT_DSP_EVENT (DSP_EVENT_IRQ * 0x20)  /* 0xfe0 */

/*
 * The disc drive.  irq 0x60 with ISR 0x109180, read out of the RTOS thunk
 * block with `isr_map`; 0x10918a loads 0xfff00080, which is the cross-check.
 * Unlike the three above, ATAPI_TSK programs its own level, into INT2PRI6
 * field 0 — so this source is gated by the firmware's own write and needs no
 * hardcoded prio here.  cdj2000_ata.h has the task file's derivation.
 */
#define ATA_IRQ         0x60
#define INTEVT_ATA      (ATA_IRQ * 0x20)        /* 0xc00 */

#define SDHI_BASE       0xffe40000
/* Widened past the TMIO register file so anything the driver's DMA path
   touches above it is seen rather than silently absorbed elsewhere.  */
#define SDHI_SIZE       0x1000
#define SDHI_CMD        0x00
#define SDHI_ARG0       0x04
#define SDHI_ARG1       0x06
#define SDHI_STOP       0x08
#define SDHI_SECCNT     0x0a
#define SDHI_RSP        0x0c    /* .. 0x1a, eight halfwords */
#define SDHI_INFO1      0x1c    /* bit 0 CMDRESPEND, bit 2 DATAEND, bit 5 card */
#define SDHI_INFO2      0x1e    /* bit 6 CMDTIMEOUT, bit 8 RXRDY, bit 14 busy */
#define SDHI_INFO1_MASK 0x20
#define SDHI_INFO2_MASK 0x22
#define SDHI_CLKCTL     0x24
#define SDHI_SIZEREG    0x26
#define SDHI_OPTION     0x28
#define SDHI_DATA       0x30
#define SDHI_DMAEN      0xd8

#define SDHI_INFO1_CMDRESPEND (1 << 0)
#define SDHI_INFO1_DATAEND    (1 << 2)
#define SDHI_INFO1_CARD       (1 << 5)
#define SDHI_INFO2_CMDTIMEOUT (1 << 6)
#define SDHI_INFO2_RXRDY      (1 << 8)
#define SDHI_INFO2_TXRQ       (1 << 9)
#define SDHI_INFO2_CBSY       (1 << 14)

#define SDHI_CMD_RESP_MASK    0x0700
#define SDHI_CMD_RESP_NONE    0x0300
#define SDHI_CMD_RESP_R2      0x0600
#define SDHI_CMD_DATA         0x0800
#define SDHI_CMD_READ         0x1000
#define SDHI_CMD_MULTI        0x2000

/* CTL_STOP bit 8: end a block-counted transfer with an automatic CMD12. */
#define SDHI_STOP_SEC_ENABLE  (1 << 8)

#define TYPE_CDJ_SDHI "cdj2000-sdhi"
OBJECT_DECLARE_SIMPLE_TYPE(CdjSdhiState, CDJ_SDHI)

struct CdjSdhiState {
    SysBusDevice parent_obj;

    SDBus sdbus;
    MemoryRegion iomem;

    uint16_t arg0, arg1, seccnt, sizereg, option, clkctl, stop;
    uint16_t info1, info2, info1_mask, info2_mask;
    uint16_t rsp[8];

    uint8_t buf[512];
    unsigned buf_len, buf_pos;
    unsigned blocks_left;
    bool multi;
    bool writing;

    /*
     * The firmware wants the *transition*, not the state.  Its card poller at
     * 0x1ff164 reads INFO1 bit 5, and the mount path at 0x1ff184 only proceeds
     * when the gate at 0x04899bec is 1 -- which is written, to 1, exclusively on
     * the card-*absent* branch at 0x1ff1c6.  A slot that reports a card from the
     * first poll therefore never arms the gate and never mounts anything, which
     * is exactly what a card present at reset produced: INFO1 read 0x0020 and
     * not one command was issued in 60 s.
     */
    bool inserted;
    bool card_high;
    QEMUTimer *insert_timer;
    qemu_irq irq;
    QEMUTimer *data_timer;
};

/*
 * A data phase must not complete inside the store that starts it.  Raising
 * the interrupt straight out of the CMD write ran ISR 0x1fffa4 before the
 * driver had armed its wait, so the event was consumed early and the
 * 500-tick wait_event in 0x1ffd8c timed out anyway -- the same trap the
 * panel DMA hit.  A short virtual-time delay restores the ordering; a real
 * 512-byte SD burst is far longer than this.
 */
#define SDHI_XFER_NS 20000

/*
 * The data phase is interrupt-driven, not polled: 0x1ffd8c waits on an RTOS
 * event with a 500-tick timeout, and ISR 0x1fffa4 sets that event when INFO2
 * bit 8 (RXRDY) is set.  INFO2_MASK is "1 = masked" -- the driver writes 0 to
 * it before the transfer and 0x100 afterwards.
 */
static void cdj_sdhi_update_irq(CdjSdhiState *s)
{
    unsigned pending = s->info2 & ~s->info2_mask
                       & (SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);

    if (s->irq) {
        qemu_set_irq(s->irq, pending ? 1 : 0);
    }
}

static void cdj_sdhi_fill_block(CdjSdhiState *s)
{
    unsigned i;

    for (i = 0; i < s->buf_len; i++) {
        s->buf[i] = sdbus_read_byte(&s->sdbus);
    }
    s->buf_pos = 0;
    s->info2 |= SDHI_INFO2_RXRDY;
    cdj_sdhi_update_irq(s);
}

/*
 * sdbus_do_command() hands back the response big-endian and without the leading
 * command byte or the trailing CRC, exactly as sdhci.c consumes it.  The driver
 * reads it as little-endian halfwords from +0x0c (0x1ff568 assembles
 * W[+0x0e] << 16 | W[+0x0c]), so halfword N of the register file is halfword N
 * of the response counted from its least significant end.
 */
static void cdj_sdhi_store_response(CdjSdhiState *s, const uint8_t *rsp,
                                    size_t len)
{
    unsigned i;

    memset(s->rsp, 0, sizeof(s->rsp));
    if (len == 4) {
        uint32_t value = ldl_be_p(rsp);

        s->rsp[0] = value & 0xffff;
        s->rsp[1] = value >> 16;
    } else if (len == 16) {
        /*
         * 0x1ff57e reads a long response as a 15-byte big-endian stream:
         * dest[0] is the *low* byte of the halfword at +0x1a, then the high and
         * low bytes of +0x18, +0x16, ... down to +0x0c.  So the 120-bit CID or
         * CSD sits right-justified across rsp[0..7] with its first byte alone
         * in rsp[7], and the card's CRC byte -- response[15] -- never reaches
         * the register file.  Getting this wrong is not obvious from the trace:
         * CMD2 and CMD9 still "succeed", the driver just rejects the CSD it
         * decodes and restarts the whole initialisation, forever.
         */
        s->rsp[7] = rsp[0];
        for (i = 0; i < 7; i++) {
            s->rsp[6 - i] = ((uint16_t)rsp[1 + 2 * i] << 8) | rsp[2 + 2 * i];
        }
    }
}

static void cdj_sdhi_command(CdjSdhiState *s, uint16_t cmd)
{
    SDRequest request = {
        .cmd = cmd & 0x3f,
        .arg = s->arg0 | ((uint32_t)s->arg1 << 16),
    };
    uint8_t response[16];
    size_t len;

    /*
     * DATAEND has to be dropped here.  The driver only ever clears bit 3 of
     * INFO1 before a command (0x1ff518), so a DATAEND left over from the
     * previous transfer would still be set when it checks, and it would treat
     * the next data phase as already finished.
     */
    s->info1 &= ~SDHI_INFO1_DATAEND;
    s->info2 &= ~(SDHI_INFO2_CMDTIMEOUT | SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);
    s->buf_len = 0;
    s->buf_pos = 0;
    s->blocks_left = 0;

    len = sdbus_do_command(&s->sdbus, &request, response, sizeof(response));

    if (getenv("CDJ_SDHI_TRACE")) {
        fprintf(stderr, "cdj2000-sdhi: cmd %#06x (CMD%u) arg %#010x -> %u bytes\n",
                cmd, request.cmd, request.arg, (unsigned)len);
    }

    if ((cmd & SDHI_CMD_RESP_MASK) == SDHI_CMD_RESP_NONE) {
        /* CMD0 and friends: the card answering nothing is the correct answer. */
    } else if (len != 4 && len != 16) {
        /*
         * Report a response timeout rather than leaving the wait spinning:
         * 0x1ff45c has a 65535-iteration cap but only yields to the RTOS every
         * eighth pass, so an unanswered command costs real time.
         */
        s->info2 |= SDHI_INFO2_CMDTIMEOUT;
        s->info1 |= SDHI_INFO1_CMDRESPEND;
        cdj_sdhi_update_irq(s);
        return;
    } else {
        cdj_sdhi_store_response(s, response, len);
    }
    s->info1 |= SDHI_INFO1_CMDRESPEND;

    if (cmd & SDHI_CMD_DATA) {
        s->buf_len = s->sizereg ? s->sizereg : 512;
        if (s->buf_len > sizeof(s->buf)) {
            s->buf_len = sizeof(s->buf);
        }
        s->multi = (cmd & SDHI_CMD_MULTI) != 0;
        s->blocks_left = s->multi && s->seccnt ? s->seccnt : 1;
        s->writing = (cmd & SDHI_CMD_READ) == 0;
        /*
         * Both directions are announced by the timer.  A write used to raise
         * TXRQ here, inside the store of the command register, which is the
         * ordering SDHI_XFER_NS exists to avoid: 0x20012e arms its wait after
         * the command, so an interrupt delivered from within the store is
         * consumed before there is anything waiting for it.
         */
        timer_mod(s->data_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SDHI_XFER_NS);
    }
    cdj_sdhi_update_irq(s);
}

/*
 * The end of a data phase.  The driver never sends CMD12 itself: it writes
 * CTL_STOP bit 8 before a multi-block command and leaves the stop to the
 * controller.  sd.c only leaves sd_sendingdata_state on CMD12 unless a CMD23
 * set a block count first, and this firmware sends neither -- so without the
 * automatic stop the card streams forever, answers the next CMD13 with state 5
 * (data) instead of 4 (tran), and 0x1ffd8c rejects every following read with
 * -9.  Single-block reads never showed it: sd.c returns to the transfer state
 * on its own when a CMD17 block runs out.
 */
static void cdj_sdhi_data_end(CdjSdhiState *s)
{
    s->blocks_left = 0;
    s->info1 |= SDHI_INFO1_DATAEND;

    if (s->multi && (s->stop & SDHI_STOP_SEC_ENABLE)) {
        SDRequest request = { .cmd = 12, .arg = 0 };
        uint8_t response[16];

        sdbus_do_command(&s->sdbus, &request, response, sizeof(response));
        if (getenv("CDJ_SDHI_TRACE")) {
            fprintf(stderr, "cdj2000-sdhi: auto CMD12\n");
        }
    }
    s->multi = false;
}

/*
 * One block has been drained, by DMA or by hand.  0x1ffd8c runs one iteration
 * per block -- wait for RXRDY, arm a 512-byte DMA, wait for DMA-end -- so every
 * block after the first has to be announced with its own RXRDY, and it has to
 * arrive after the driver has armed the next wait.  That is the same ordering
 * SDHI_XFER_NS exists for; delivering the next block inside the drain sets
 * DATAEND while 63 blocks are still outstanding and the driver waits 500 ticks
 * for an interrupt that will never come.
 */
static void cdj_sdhi_block_done(CdjSdhiState *s)
{
    s->info2 &= ~(SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);
    cdj_sdhi_update_irq(s);

    if (s->blocks_left > 1) {
        s->blocks_left--;
        timer_mod(s->data_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SDHI_XFER_NS);
    } else {
        cdj_sdhi_data_end(s);
    }
}

/* A full block has been handed over; give it to the card and move on. */
static void cdj_sdhi_flush_block(CdjSdhiState *s)
{
    unsigned i;

    for (i = 0; i < s->buf_len; i++) {
        sdbus_write_byte(&s->sdbus, s->buf[i]);
    }
    s->buf_pos = 0;
    cdj_sdhi_block_done(s);
}

static uint16_t cdj_sdhi_read_halfword(CdjSdhiState *s, hwaddr offset)
{
    switch (offset) {
    case SDHI_ARG0:
        return s->arg0;
    case SDHI_ARG1:
        return s->arg1;
    case SDHI_SECCNT:
        return s->seccnt;
    case SDHI_STOP:
        return s->stop;
    case SDHI_RSP ... SDHI_RSP + 14:
        return s->rsp[(offset - SDHI_RSP) / 2];
    case SDHI_INFO1:
        /*
         * Card presence is a live pin, not a latched bit: the reset that would
         * have sampled it runs before the card device is attached to the bus,
         * and the driver re-reads INFO1 on every poll anyway.  The pin only
         * changes once the insert timer has fired; see the comment on
         * ->inserted.
         *
         * Bit 5 is active *low*: set means the slot is empty.  Both readers say
         * so and they agree with each other -- 0x1ff164 and 0x1ff24e share the
         * gate `if (INFO1 & 0x20) goto absent; if (ready() == 1) goto present;`
         * and 0x1ff24e's absent arm is the one that calls the unregister,
         * 0x2e27b4.  Reading it active high still produces the 1->0 edge the
         * mount gate at 0x04899bec needs, which is why the block layer worked
         * anyway, but it leaves the machine permanently in the *removed* state:
         * the volume is registered and unregistered in a loop, drive letter `b`
         * keeps a type of 0, and every `b:/PIONEER/rekordbox/export.pdb` open
         * fails in the default arm of 0x2dfd84.
         *
         * CDJ_SDHI_CARD_HIGH=1 restores the old sense for an A/B against the
         * same binary.
         */
        return s->info1 | ((s->inserted && sdbus_get_inserted(&s->sdbus))
                           == s->card_high ? SDHI_INFO1_CARD : 0);
    case SDHI_INFO2:
        return s->info2;
    case SDHI_INFO1_MASK:
        return s->info1_mask;
    case SDHI_INFO2_MASK:
        return s->info2_mask;
    case SDHI_CLKCTL:
        return s->clkctl;
    case SDHI_SIZEREG:
        return s->sizereg;
    case SDHI_OPTION:
        return s->option;
    case SDHI_DATA: {
        uint16_t value;

        if (s->buf_pos + 1 >= s->buf_len) {
            /* Serve the tail, then advance to the next block or finish. */
            value = s->buf_pos < s->buf_len ? s->buf[s->buf_pos] : 0;
            s->buf_pos = s->buf_len;
        } else {
            value = s->buf[s->buf_pos] | (s->buf[s->buf_pos + 1] << 8);
            s->buf_pos += 2;
        }
        if (s->buf_pos >= s->buf_len) {
            cdj_sdhi_block_done(s);
        }
        return value;
    }
    default:
        if (offset >= 0x100 && getenv("CDJ_SDHI_TRACE")) {
            fprintf(stderr, "cdj2000-sdhi: READ HIGH +%#06x\n",
                    (unsigned)offset);
        }
        return 0;
    }
}

static uint64_t cdj_sdhi_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjSdhiState *s = opaque;
    uint64_t value = cdj_sdhi_read_halfword(s, offset & ~1ULL);
    static unsigned long reads;
    static uint64_t last_value = ~0ULL;
    static hwaddr last_offset = ~(hwaddr)0;

    /*
     * The status waits poll hard (0x1ff45c is unrolled 2x5 inside a 65535 outer
     * loop), so report changes rather than samples.  A plain "first N" cap is
     * worse than useless here: the card poller runs about three times a second,
     * so a cap of 24 hides the insertion it was added to observe.
     */
    ++reads;
    if (cdj_sdhi_trace()
        && (offset != last_offset || value != last_value)) {
        fprintf(stderr, "cdj2000-sdhi: read %u @ +%#04x -> %#06x (#%lu) pc %#010x\n",
                size, (unsigned)offset, (unsigned)value, reads,
                current_cpu ? (unsigned)SUPERH_CPU(current_cpu)->env.pc : 0);
        last_offset = offset;
        last_value = value;
    }

    /*
     * The driver reads these as halfwords, but a wider access has to see both
     * status registers rather than only the one it starts on: CTL_STATUS is one
     * 32-bit word whose upper half is INFO2.
     */
    if (size == 4) {
        value |= (uint64_t)cdj_sdhi_read_halfword(s, (offset & ~3ULL) + 2) << 16;
    }
    if (size == 1) {
        value = (value >> (8 * (offset & 1))) & 0xff;
    }
    return value;
}

static void cdj_sdhi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CdjSdhiState *s = opaque;

    if (cdj_sdhi_trace() && (offset & ~1ULL) != SDHI_INFO1
        && (offset & ~1ULL) != SDHI_INFO2) {
        fprintf(stderr, "cdj2000-sdhi: write %u @ +%#04x = %#06x  pc %#010x\n",
                size, (unsigned)offset, (unsigned)value,
                current_cpu ? (unsigned)SUPERH_CPU(current_cpu)->env.pc : 0);
    }

    switch (offset & ~1ULL) {
    case SDHI_CMD:
        cdj_sdhi_command(s, value & 0xffff);
        break;
    case SDHI_ARG0:
        s->arg0 = value;
        break;
    case SDHI_ARG1:
        s->arg1 = value;
        break;
    case SDHI_SECCNT:
        s->seccnt = value;
        break;
    case SDHI_STOP:
        s->stop = value;
        break;
    case SDHI_DATA:
        /*
         * The by-hand path, taken when the source buffer is odd-aligned
         * (0x200334 tests bit 0 exactly as the read side does at 0x1ffec8).
         * A block is handed to the card only once it is whole, because sd.c
         * counts the bytes it receives and a short block desynchronises it.
         */
        if (!s->writing || s->buf_pos >= s->buf_len) {
            break;
        }
        s->buf[s->buf_pos++] = value & 0xff;
        if (size > 1 && s->buf_pos < s->buf_len) {
            s->buf[s->buf_pos++] = (value >> 8) & 0xff;
        }
        if (s->buf_pos >= s->buf_len) {
            cdj_sdhi_flush_block(s);
        }
        break;
    case SDHI_INFO1:
        /*
         * Write-to-clear, and only the bits the driver names.  The card-present
         * bit is the device's to own: 0x1ff518 clears bit 3 before every
         * command, and a status register that simply took the written value
         * would drop the card between two commands.
         */
        if (size == 4) {
            s->info1 &= (value & 0xffff) | SDHI_INFO1_CARD;
            s->info2 &= value >> 16;
        } else {
            s->info1 &= value | SDHI_INFO1_CARD;
        }
        break;
    case SDHI_INFO2:
        s->info2 &= value;
        cdj_sdhi_update_irq(s);
        break;
    case SDHI_INFO1_MASK:
        s->info1_mask = value;
        break;
    case SDHI_INFO2_MASK:
        s->info2_mask = value;
        cdj_sdhi_update_irq(s);
        break;
    case SDHI_CLKCTL:
        s->clkctl = value;
        break;
    case SDHI_SIZEREG:
        s->sizereg = value;
        break;
    case SDHI_OPTION:
        s->option = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps cdj_sdhi_ops = {
    .read = cdj_sdhi_read,
    .write = cdj_sdhi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_sdhi_data_ready(void *opaque)
{
    CdjSdhiState *s = opaque;

    if (!s->buf_len) {
        return;
    }
    if (s->writing) {
        /* Ready to take a block: the driver's half of RXRDY. */
        s->buf_pos = 0;
        s->info2 |= SDHI_INFO2_TXRQ;
        cdj_sdhi_update_irq(s);
    } else {
        cdj_sdhi_fill_block(s);
    }
}

/*
 * The CDJ-2000 media state the GUI routes on (not an NXS address map).
 *
 * Status halfword 26 carries one 3-bit state per source, built by 0x218afe
 * from the four words at 0x0489bd68, one per source in MAIN's own numbering
 * (1 LINK 0x..68, 2 USB 0x..6c, 3 SD 0x..70, 4 DISC 0x..74).  The GUI's
 * parser (0xb7dfe6) spreads halfword 26 into a table at 0x4b439c in the
 * same order -- bits [11:9] first -- and its key dispatcher (0xb9b98c)
 * routes a SOURCE key on the state at index (halfword 18 - 1), i.e. the
 * source MAIN reports as current.  The screen router (0xb9b706) takes 0
 * to the Wait platter, 1 to the library, and only from the library does
 * the GUI ask MAIN for the card's lists.
 *
 * Measured with the GUI's own table watched (w4, v1): a 1 written to
 * 0x0489bd6c lands in the table at index 1 (USB) and the SD key still
 * routes on index 2 = 0, the platter; a 1 written to 0x0489bd70 lands at
 * index 2 and the same key routes on state 1.  The earlier order
 * "(DISC, SD, USB, LINK)" was a guess that put the card's state on the
 * stick's word.
 * MAIN writes those words through one accessor, 0x14e0be, from its browse
 * answer builder -- and only on the media branch, which needs the database
 * context open ([ctx+0x2d5c]) -- so a card that is mounted is never reported
 * as such until the GUI browses it, and the GUI never browses it while the
 * state says 0.  Measured, SD key on the test card: state 0 at the key ->
 * platter for ever (sd1, sd2, w2); state 1 at the key -> the GUI asks for
 * the card's lists at once and draws PLAYLIST / SEARCH / ARTIST / ALBUM /
 * TRACK / KEY from it (e4, e5).
 *
 * CDJ_SD_MEDIA_STATE=1 makes the board report the card itself: from
 * CDJ_SD_MOUNT_S (5) seconds after the insertion it holds the SD state
 * word at 1, ten times a second.  It is OFF by default, because holding
 * the word is measurably harmful: with it held (e2) MAIN answered the
 * GUI's list request for the card with one-row records for 20 s; without
 * it (e5, f1-f3) MAIN answered the same request with the card's lists at
 * once and the library was on screen 2.6 s after the SD key.  MAIN's own
 * media manager sets the word when the GUI browses the card, and the key
 * gets there without help as long as the GUI's browse loop for the empty
 * boot source has ended (see BFIN_LINK_BUDGET_ANY / BFIN_LINK_DEPTH in the
 * launchers).  The report stays as a diagnostic for the routing chain.
 */
#define CDJ_SD_STATE_WORD 0x0489bd70ULL

static void cdj_sd_media_state_report(CdjSdhiState *s)
{
    const char *enable = getenv("CDJ_SD_MEDIA_STATE");
    const char *mount = getenv("CDJ_SD_MOUNT_S");
    uint64_t seconds = mount ? strtoull(mount, NULL, 0) : 5;
    CdjMainPoke *poke;

    if (!(enable && *enable && *enable != '0')
        || !sdbus_get_inserted(&s->sdbus)) {
        return;
    }
    poke = g_new0(CdjMainPoke, 1);
    poke->address[0] = CDJ_SD_STATE_WORD;
    poke->value[0] = 1;
    poke->count = 1;
    poke->period_ns = 100 * SCALE_MS;
    poke->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_main_poke_fire, poke);
    timer_mod(poke->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           seconds * NANOSECONDS_PER_SECOND);
    qemu_log_mask(LOG_UNIMP, "cdj2000-sd: card inserted; reporting it mounted "
                  "(media state 1) from %" PRIu64 " s on\n", seconds);
}

static void cdj_sdhi_insert(void *opaque)
{
    CdjSdhiState *s = opaque;

    s->inserted = true;
    cdj_sd_media_state_report(s);
}

static void cdj_sdhi_reset(DeviceState *dev)
{
    CdjSdhiState *s = CDJ_SDHI(dev);
    const char *delay = getenv("CDJ_SD_INSERT");
    unsigned seconds = delay ? strtoul(delay, NULL, 0) : 20;

    s->info1 = 0;
    s->info2 = 0;
    s->buf_len = s->buf_pos = s->blocks_left = 0;
    s->multi = false;
    s->writing = false;
    s->stop = 0;
    s->sizereg = 512;
    s->inserted = false;
    s->card_high = getenv("CDJ_SDHI_CARD_HIGH") != NULL;
    s->info2_mask = 0xffff;
    cdj_sdhi_update_irq(s);

    /*
     * Report an empty slot first and insert the card a while later, because the
     * poller only arms its mount gate while the slot is empty.  Twenty seconds
     * of virtual time is well past the RTOS coming up; CDJ_SD_INSERT overrides
     * it, and 0 means "never", which reproduces an empty slot exactly.
     */
    if (seconds != 0) {
        timer_mod(s->insert_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                   (int64_t)seconds * NANOSECONDS_PER_SECOND);
    }
}

static void cdj_sdhi_realize(DeviceState *dev, Error **errp)
{
    CdjSdhiState *s = CDJ_SDHI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &cdj_sdhi_ops, s,
                          "cdj2000.sdhi", SDHI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, dev, "sd-bus");
    s->insert_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_sdhi_insert, s);
    s->data_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_sdhi_data_ready, s);
}

static void cdj_sdhi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cdj_sdhi_realize;
    device_class_set_legacy_reset(dc, cdj_sdhi_reset);
}

static const TypeInfo cdj_sdhi_info = {
    .name = TYPE_CDJ_SDHI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CdjSdhiState),
    .class_init = cdj_sdhi_class_init,
};

static void cdj_sdhi_register_types(void)
{
    type_register_static(&cdj_sdhi_info);
}
type_init(cdj_sdhi_register_types)

/*
 * A write watch on SDRAM words.
 *
 * Some state words are written through a pointer held in a struct, so no
 * literal pool names them and no amount of grepping over the disassembly finds
 * the writer -- 0x04c0854c, which gates the whole SD mount, is one of those.
 * GDB is no help either: SH-4 reaches the same physical word through P0, P1 and
 * P2, and a gdbstub watchpoint is on one virtual address.
 *
 * So overlay four bytes of RAM with a device that keeps the value itself and
 * reports every write with the guest PC.  CDJ_WATCH=<hex address> turns it on;
 * each address must be 4-byte aligned.  This is a diagnostic, not part of the
 * machine.
 *
 * The spec is a comma-separated list, because a run costs two to three minutes
 * of wall clock and the questions here come in pairs that only mean something
 * together: "did the list dispatcher run" is worth little without "how many
 * messages did the database task dequeue", measured on the *same* run and the
 * same request stream.  Splitting them across two runs compares two different
 * streams -- the number of type-1 requests MAIN examines varies by a factor of
 * several between runs -- and that is exactly how a real difference gets read
 * as noise.  Unlike a breakpoint, a watch never stops the guest, so adding a
 * second one costs nothing that matters.
 */
typedef struct CdjWatch {
    uint32_t value;
    hwaddr base;
} CdjWatch;

static uint64_t cdj_watch_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjWatch *w = opaque;

    return (w->value >> (8 * offset)) & ((1ULL << (8 * size)) - 1);
}

static void cdj_watch_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    CdjWatch *w = opaque;
    uint32_t before = w->value;
    uint32_t mask = size >= 4 ? 0xffffffffu
                              : (((1u << (8 * size)) - 1) << (8 * offset));

    w->value = (w->value & ~mask) | ((uint32_t)value << (8 * offset) & mask);
    fprintf(stderr, "cdj2000-watch: %#" HWADDR_PRIx " %u-byte write %#x: "
                    "%#010x -> %#010x  pc %#010x\n",
            w->base + offset, size, (unsigned)value, before, w->value,
            current_cpu ? (unsigned)SUPERH_CPU(current_cpu)->env.pc : 0);
}

static const MemoryRegionOps cdj_watch_ops = {
    .read = cdj_watch_read,
    .write = cdj_watch_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_watch_one(MemoryRegion *system, hwaddr base)
{
    MemoryRegion *region = g_new(MemoryRegion, 1);
    CdjWatch *watch = g_new0(CdjWatch, 1);

    watch->base = base;
    memory_region_init_io(region, NULL, &cdj_watch_ops, watch,
                          "cdj2000.watch", 4);
    /* Priority 1 so it wins over the SDRAM region underneath. */
    memory_region_add_subregion_overlap(system, watch->base, region, 1);
    info_report("cdj2000: watching writes to %#" HWADDR_PRIx, watch->base);
}

static void cdj_watch_init(MemoryRegion *system)
{
    const char *spec = getenv("CDJ_WATCH");

    if (spec == NULL || *spec == '\0') {
        return;
    }
    while (*spec != '\0') {
        char *end;
        hwaddr base;

        while (*spec == ',' || *spec == ' ') {
            spec++;
        }
        if (*spec == '\0') {
            break;
        }
        base = strtoull(spec, &end, 16) & ~3ULL;
        if (end == spec) {
            /* Refuse to guess: a typo that silently watches nothing reads
             * exactly like "nobody writes that word", which is the wrong
             * answer to keep. */
            error_report("cdj2000: CDJ_WATCH: not a hex address: %s", spec);
            exit(1);
        }
        cdj_watch_one(system, base);
        spec = end;
    }
}

static CdjSdhiState *cdj_sdhi_singleton;

/*
 * Drain the block the card handed over into the DMA's buffer.  Returns false
 * when nothing is staged, so a stray channel start cannot invent data.
 */
static bool cdj_sdhi_dma_read(unsigned nr_bytes, uint8_t *buffer)
{
    CdjSdhiState *s = cdj_sdhi_singleton;
    unsigned i;

    if (s == NULL || s->buf_len == 0) {
        return false;
    }
    /*
     * At most one block per DMA.  Reaching into the next block here would be
     * right for a controller-driven burst, but this driver arms one 512-byte
     * channel per block and waits for a fresh RXRDY in between, so the block
     * boundary belongs to cdj_sdhi_block_done().
     */
    for (i = 0; i < nr_bytes && s->buf_pos < s->buf_len; i++) {
        buffer[i] = s->buf[s->buf_pos++];
    }
    if (s->buf_pos >= s->buf_len) {
        cdj_sdhi_block_done(s);
    }
    return true;
}

/* The same contract in the other direction: one block per armed channel. */
static bool cdj_sdhi_dma_write(unsigned nr_bytes, const uint8_t *buffer)
{
    CdjSdhiState *s = cdj_sdhi_singleton;
    unsigned i;

    if (s == NULL || s->buf_len == 0 || !s->writing) {
        return false;
    }
    for (i = 0; i < nr_bytes && s->buf_pos < s->buf_len; i++) {
        s->buf[s->buf_pos++] = buffer[i];
    }
    if (s->buf_pos >= s->buf_len) {
        cdj_sdhi_flush_block(s);
    }
    return true;
}

static void cdj_sdhi_init(MemoryRegion *system, qemu_irq irq)
{
    DeviceState *host = qdev_new(TYPE_CDJ_SDHI);
    DriveInfo *drive = drive_get(IF_SD, 0, 0);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
    CDJ_SDHI(host)->irq = irq;
    cdj_sdhi_singleton = CDJ_SDHI(host);
    memory_region_add_subregion(system, SDHI_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(host), 0));

    if (drive) {
        DeviceState *card = qdev_new(TYPE_SD_CARD);

        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(drive),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                               &error_fatal);
    }
    /* No -sd image: the slot reports empty, as it did before. */
}

/*
 * The two link channels take their data from -serial: the GUI board on the
 * first, the PANEL board on the second, and MAIN's console on the third, e.g.
 *   -serial tcp:127.0.0.1:5570,server,nowait -serial null -serial stdio
 */
static void cdj_link_board_init(MemoryRegion *system, struct intc_desc *intc)
{
    CdjIntc2State *intc2 = g_new0(CdjIntc2State, 1);
    CdjLinkFlagState *flag = g_new0(CdjLinkFlagState, 1);

    memory_region_init_io(&intc2->iomem, NULL, &cdj_intc2_ops, intc2,
                          "cdj2000.intc2-status", INTC2_STATUS_SIZE);
    memory_region_add_subregion(system, INTC2_STATUS, &intc2->iomem);
    cdj_dsp_event_sink.intc2 = intc2;
    cdj_dsp_event_sink.flag = flag;

    flag->panel_present = !getenv("CDJ_NO_PANEL");
    flag->usb_power = !getenv("CDJ_NO_USB_POWER");
    memory_region_init_io(&flag->iomem, NULL, &cdj_link_flag_ops, flag,
                          "cdj2000.soc-block", SOC_BLOCK_SIZE);
    memory_region_add_subregion(system, SOC_BLOCK_BASE, &flag->iomem);

    /*
     * One link, two halves.  0xff401000 receives the GUI's 48-byte requests
     * into 0xa4500000; 0xff501000 transmits the 64-byte status record, which
     * 0x2a3cf6 stages at 0xa4500800 before arming it.  They take separate
     * chardevs (-serial 0 in, -serial 1 out).
     */
    CdjLinkState *rx = cdj_link_init(system, intc2, flag,
                  "cdj2000.link-rx", LINK_RX_BASE,
                  LINK_RX_BASE + 0x1000, 1u << 0,
                  LINK_RX_BUFFER, LINK_RX_LENGTH, false, NULL,
                  intc->irqs[CDJ_INTC_LINK_RX], serial_hd(0));
    CdjLinkState *tx = cdj_link_init(system, intc2, NULL,
                  "cdj2000.link-tx", LINK_TX_BASE,
                  LINK_TX_BASE + 0x1000, 1u << 5,
                  LINK_TX_BUFFER, LINK_TX_LENGTH, true, NULL,
                  intc->irqs[CDJ_INTC_LINK_TX], serial_hd(1));
    tx->done_irq = intc->irqs[CDJ_INTC_LINK_DONE];
    tx->rx_peer = rx;           /* MAIN's answers pace the receive FIFO */
    cdj_cosim_init(cdj_link_cosim_request, cdj_link_cosim_poll, rx);
    cdj_console_init(system, serial_hd(2), intc->irqs[CDJ_INTC_SCIF_RX],
                     intc->irqs[CDJ_INTC_SCIF_TX]);
}

static uint64_t cdj_ccn_read(void *opaque, hwaddr offset, unsigned size)
{
    CPUSH4State *env = &((SuperHCPU *)opaque)->env;

    switch (offset) {
    case CCN_PTEH:   return env->pteh;
    case CCN_PTEL:   return env->ptel;
    case CCN_TTB:    return env->ttb;
    case CCN_TEA:    return env->tea;
    case CCN_MMUCR:  return env->mmucr;
    case CCN_TRA:    return env->tra;
    case CCN_EXPEVT: return env->expevt;
    case CCN_INTEVT: return env->intevt;
    case CCN_CCR:    return 0;   /* caches are not modelled */
    default:
        qemu_log_mask(LOG_UNIMP, "cdj2000-ccn: read 0x%" HWADDR_PRIx
                      " (%u bytes)\n", CCN_BASE + offset, size);
        return 0;
    }
}

static void cdj_ccn_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    CPUSH4State *env = &((SuperHCPU *)opaque)->env;

    switch (offset) {
    case CCN_PTEH:   env->pteh = value; return;
    case CCN_PTEL:   env->ptel = value; return;
    case CCN_TTB:    env->ttb = value; return;
    case CCN_TEA:    env->tea = value; return;
    case CCN_MMUCR:  env->mmucr = value; return;
    case CCN_TRA:    env->tra = value & 0x7ff; return;
    case CCN_EXPEVT: env->expevt = value & 0x7ff; return;
    case CCN_INTEVT: env->intevt = value & 0x7ff; return;
    case CCN_CCR:    return;     /* cache control is a no-op here */
    default:
        qemu_log_mask(LOG_UNIMP, "cdj2000-ccn: write 0x%" HWADDR_PRIx
                      " (%u bytes) = 0x%" PRIx64 "\n",
                      CCN_BASE + offset, size, value);
    }
}

static const MemoryRegionOps cdj_ccn_ops = {
    .read = cdj_ccn_read,
    .write = cdj_ccn_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * P4 addresses reach the bus unmasked (target/sh4 leaves >= 0xe0000000 alone),
 * so peripherals are mapped at their P4 address, not the area-7 alias.
 */
static void cdj_intc_timer_init(MemoryRegion *system, SuperHCPU *cpu)
{
    static struct intc_vect vectors[] = {
        INTC_VECT(CDJ_INTC_TMU0, INTEVT_TMU0),
        INTC_VECT(CDJ_INTC_TMU1, INTEVT_TMU1),
        INTC_VECT(CDJ_INTC_LINK_RX, INTEVT_LINK_RX),
        INTC_VECT(CDJ_INTC_LINK_TX, INTEVT_LINK_TX),
        INTC_VECT(CDJ_INTC_LINK_DONE, INTEVT_LINK_DONE),
        INTC_VECT(CDJ_INTC_SCIF_RX, INTEVT_SCIF_RX),
        INTC_VECT(CDJ_INTC_SCIF_TX, INTEVT_SCIF_TX),
        INTC_VECT(CDJ_INTC_PANEL_RX, INTEVT_PANEL_RX),
        INTC_VECT(CDJ_INTC_PANEL_TX, INTEVT_PANEL_TX),
        INTC_VECT(CDJ_INTC_SDHI, INTEVT_SDHI),
        INTC_VECT(CDJ_INTC_SDHI_DMA, INTEVT_SDHI_DMA),
        INTC_VECT(CDJ_INTC_DSP_DMA, INTEVT_DSP_DMA),
        INTC_VECT(CDJ_INTC_DMA5, INTEVT_DMA5),
        INTC_VECT(CDJ_INTC_DSP_EVENT, INTEVT_DSP_EVENT),
        INTC_VECT(CDJ_INTC_ATA, INTEVT_ATA),
        INTC_VECT(CDJ_INTC_USB, INTEVT_USB),
        INTC_VECT(CDJ_INTC_USB_DMA, INTEVT_USB_DMA),
        INTC_VECT(CDJ_INTC_ETH, 0x920), /* SH7764 EtherC, manual Table13.2 */
        INTC_VECT(CDJ_INTC_TMU3, INTEVT_TMU3),
        INTC_VECT(CDJ_INTC_TMU4, INTEVT_TMU4),
        INTC_VECT(CDJ_INTC_TMU5, INTEVT_TMU5),
    };
    /*
     * enum_ids are MSB-field first: sh_intc_write shifts a field's mask by
     * (first - k) * field_width, so for a 32-bit register of 8-bit fields
     * index 0 is bits 31:24 and index 3 is bits 7:0.  The zero entries are
     * fields the stock firmware leaves at 0. The connected AmbiX profile
     * programs INT2PRI0[23:16] for TMU1, so channel 1 must retain its own
     * source even though stock never enables it.
     */
    static struct intc_prio_reg prio_registers[] = {
        { 0xffd40000, 0, 32, 8,
          { CDJ_INTC_TMU0, CDJ_INTC_TMU1, 0, 0 } },
        /* INT2PRI1: TUNI3, TUNI4, TUNI5, reserved -- the loader's tick. */
        { 0xffd40004, 0, 32, 8, { CDJ_INTC_TMU3, CDJ_INTC_TMU4, CDJ_INTC_TMU5, 0 } },
        { 0xffd40008, 0, 32, 8, { CDJ_INTC_SCIF_RX, 0, 0, 0 } },
        { 0xffd40010, 0, 32, 8, { 0, 0, CDJ_INTC_LINK_RX, CDJ_INTC_LINK_TX } },
        { 0xffd40014, 0, 32, 8, { 0, 0, 0, CDJ_INTC_LINK_DONE } },
        /* ATAPI_TSK writes its level here; field 0 is bits 31:24. */
        { 0xffd40018, 0, 32, 8, { CDJ_INTC_ATA, 0, 0, 0 } },
        /* INT2PRI3: H-UDI, DMAC (DMINT0..), reserved, reserved. */
        { 0xffd4000c, 0, 32, 8, { 0, CDJ_INTC_USB_DMA, 0, 0 } },
        /* INT2PRI12: VDC2, reserved, USB, EtherC. */
        { 0xffd400b0, 0, 32, 8, { 0, 0, CDJ_INTC_USB, CDJ_INTC_ETH } },
    };
    struct intc_desc *intc = g_new0(struct intc_desc, 1);
    MemoryRegion *ccn = g_new(MemoryRegion, 1);

    memory_region_init_io(ccn, NULL, &cdj_ccn_ops, cpu, "cdj2000.ccn",
                          CCN_SIZE);
    memory_region_add_subregion(system, CCN_BASE, ccn);

    /*
     * sh_intc_init must see the priority registers before the sources are
     * registered: sh_intc_register_source counts a source's appearances in
     * them into enable_max, and a source whose enable_count never reaches
     * enable_max is never pending.
     */
    sh_intc_init(system, intc, CDJ_INTC_NR_SOURCES, NULL, 0,
                 _INTC_ARRAY(prio_registers));
    sh_intc_register_sources(intc, _INTC_ARRAY(vectors), NULL, 0);
    /*
     * These three are unmasked through INT2MSKCR (0xffd4003c) rather than
     * given a level in INT2PRI, so nothing records a priority for them and
     * sh_intc would leave them at 0, which reads as "disabled".
     */
    intc->sources[CDJ_INTC_SCIF_TX].prio = SCIF_PRIO;
    intc->sources[CDJ_INTC_PANEL_RX].prio = PANEL_PRIO;
    intc->sources[CDJ_INTC_PANEL_TX].prio = PANEL_PRIO;
    /* Enabled through the second INTC2 block at 0xffd400b0, which the
       board does not model, so nothing else would record its level.  */
    intc->sources[CDJ_INTC_SDHI].prio = SDHI_PRIO;
    intc->sources[CDJ_INTC_SDHI_DMA].prio = SDHI_PRIO;
    /* The DSP's DMA vector is registered by the driver, not through INT2PRI. */
    intc->sources[CDJ_INTC_DSP_DMA].prio = DSP_PRIO;
    /* Likewise channel 5's: its record at 0xa40668fc says level 4. */
    intc->sources[CDJ_INTC_DMA5].prio = PANEL_PRIO;
    intc->sources[CDJ_INTC_DSP_EVENT].prio = DSP_PRIO;
    cpu->env.intc_handle = intc;

    cdj_sdhi_init(system, intc->irqs[CDJ_INTC_SDHI]);
    if (cdj_nxs_profile) cdj_nxs_eth_init(intc->irqs[CDJ_INTC_ETH]);
    cdj_link_board_init(system, intc);
    cdj_panel_scif_init(system);
    /*
     * CDJ_DSP_CHARDEV names a chardev to hand the DSP's mailbox to; without it
     * the built-in transport model answers, which is the normal case.
     */
    const char *dsp_chardev = getenv("CDJ_DSP_CHARDEV");

    if (cdj_nxs_profile) {
        cdj_nxs_hpi_init(system, cdj_nxs_hint_level, cdj_dsp_event_sink.flag);
    } else {
    cdj_dsp_init(system, dsp_chardev ? qemu_chr_find(dsp_chardev) : NULL,
                 intc->irqs[CDJ_INTC_DSP_EVENT], cdj_dsp_event_pending,
                 &cdj_dsp_event_sink);
    }
    /*
     * The USB controller sits on the external bus at physical 0x01000000, well
     * clear of CS0's 4 MiB of flash.  Without it USBFD_TSK's enable-and-poll at
     * 0x2399c8 never sees its bit and MAIN reports device 3 broken, which is
     * `E-7020: USB-B DEVICE ERROR`.
     */
    cdj_usb_init(system);
    /*
     * The disc drive.  Without it 0xfff0001c answers zero, ATAPI_TSK polls the
     * status register 44 856 times in a two-minute run and MAIN reports device
     * 6 broken, which is `E-7001: DISC DRIVE ERROR`.
     */
    cdj_ata_init(system, intc->irqs[CDJ_INTC_ATA]);
    CdjDmacState *dmac = cdj_dmac_init(system, intc->irqs[CDJ_INTC_PANEL_RX],
                                       intc->irqs[CDJ_INTC_PANEL_TX],
                                       intc->irqs[CDJ_INTC_SDHI_DMA],
                                       intc->irqs[CDJ_INTC_DSP_DMA],
                                       intc->irqs[CDJ_INTC_DMA5],
                                       intc->irqs[CDJ_INTC_USB_DMA]);
    /*
     * The SoC's own USB module, host side: the type-A socket.  A stick is
     * `-drive if=none,id=usbstick,file=... -device usb-storage,drive=usbstick`
     * (boot_vm --usb-stick); with nothing plugged in the port is empty and
     * the driver sees SE0, as on a player with an empty socket.
     */
    cdj_usbh_init(system, intc->irqs[CDJ_INTC_USB], cdj_dmac_usbh_done, dmac);

    /*
     * CDJ_TMU_FREQ multiplies the peripheral clock.  The firmware has several
     * ten-second timeouts on RTOS system time, and under TCG the machine runs
     * several times slower than real time, so reaching them costs minutes of
     * wall clock.  Raising the timer frequency makes guest time pass faster
     * relative to the host without changing anything the firmware can observe
     * apart from rate.  Unset, the real 54 MHz is used.
     */
    const char *freq_env = getenv("CDJ_TMU_FREQ");
    uint32_t freq = freq_env ? strtoul(freq_env, NULL, 0) : TMU_FREQ;

    tmu012_init(system, TMU_BASE, TMU012_FEAT_TOCR | TMU012_FEAT_3CHAN,
                freq, intc->irqs[CDJ_INTC_TMU0], intc->irqs[CDJ_INTC_TMU1],
                NULL, NULL);
    tmu012_init(system, TMU2_BASE, TMU012_FEAT_3CHAN,
                freq, intc->irqs[CDJ_INTC_TMU3], intc->irqs[CDJ_INTC_TMU4],
                intc->irqs[CDJ_INTC_TMU5], NULL);
}

#ifdef _WIN32
/*
 * The RTOS tick is a 1 ms TMU period, and QEMU's main loop waits for it
 * with a millisecond g_poll on the Windows scheduler, whose default
 * resolution is 15.6 ms and which Windows 11 additionally coarsens for
 * processes without a foreground window.  A tick that fires late enough to
 * overlap the next one is coalesced by the level-sensitive TMU line, and the
 * guest loses it: measured 720 ticks a second of the 1000 programmed.  Ask
 * for a 0.5 ms resolution and opt out of the background throttling.  Both
 * calls are looked up at run time so the build needs no extra library.
 */
static void cdj_host_timer_resolution(void)
{
    typedef LONG (WINAPI *set_resolution_t)(ULONG, BOOLEAN, PULONG);
    typedef BOOL (WINAPI *set_information_t)(HANDLE, int, LPVOID, DWORD);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    set_resolution_t set_resolution = ntdll
        ? (set_resolution_t)(void *)GetProcAddress(ntdll, "NtSetTimerResolution")
        : NULL;
    set_information_t set_information = kernel32
        ? (set_information_t)(void *)GetProcAddress(kernel32,
                                                    "SetProcessInformation")
        : NULL;
    ULONG actual = 0;

    if (set_information) {
        /* PROCESS_POWER_THROTTLING_STATE, version 1; control bit 4 is
         * PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, state 0 means
         * "do not throttle".  ProcessPowerThrottling is class 4. */
        struct {
            ULONG version, control_mask, state_mask;
        } throttling = { 1, 4, 0 };
        set_information(GetCurrentProcess(), 4, &throttling, sizeof(throttling));
    }
    if (set_resolution) {
        set_resolution(5000, TRUE, &actual);
    }
    info_report("cdj2000-main: host timer resolution %.2f ms",
                actual / 10000.0);
}
#endif

static void cdj2000_main_init(MachineState *machine)
{
    MemoryRegion *system = get_system_memory();
    MemoryRegion *sdram = g_new(MemoryRegion, 1);
    CdjResetState *reset;
    SuperHCPU *cpu;
    const char *firmware = machine->firmware;

#ifdef _WIN32
    cdj_host_timer_resolution();
#endif
    ssize_t loaded;

    cdj_nxs_profile = !strcmp(object_get_typename(OBJECT(machine)), MACHINE_TYPE_NAME("cdj2000nxs-main"));
    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));

    memory_region_init_ram(sdram, NULL, "cdj2000.sdram", machine->ram_size,
                           &error_fatal);
    memory_region_add_subregion(system, SDRAM_BASE, sdram);

    /*
     * Word-wide device, so the unlock addresses are the usual 0x555/0x2aa and
     * the guest's byte addresses 0xaaa/0x554 land on them.
     */
    {
        DeviceState *flash = qdev_new(TYPE_PFLASH_CFI02);

        qdev_prop_set_uint32(flash, "num-blocks0", FLASH_MAIN_SECTORS);
        qdev_prop_set_uint32(flash, "sector-length0", FLASH_MAIN_SECTOR);
        qdev_prop_set_uint32(flash, "num-blocks1", FLASH_BOOT_SECTORS);
        qdev_prop_set_uint32(flash, "sector-length1", FLASH_BOOT_SECTOR);
        qdev_prop_set_uint8(flash, "width", 2);
        qdev_prop_set_uint8(flash, "mappings", 1);
        qdev_prop_set_uint8(flash, "big-endian", 0);
        qdev_prop_set_uint16(flash, "id0", 0x0001);
        qdev_prop_set_uint16(flash, "id1", 0x227e);
        qdev_prop_set_uint16(flash, "id2", 0x2220);
        qdev_prop_set_uint16(flash, "id3", 0x2200);
        qdev_prop_set_uint16(flash, "unlock-addr0", 0x555);
        qdev_prop_set_uint16(flash, "unlock-addr1", 0x2aa);
        qdev_prop_set_string(flash, "name", "cdj2000.flash");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, ROM_BASE);
    }

    cdj_periph_init(system);
    if (cdj_nxs_profile) {
        cdj_nxs_iic_init(system);
    }
    cdj_bus_trace_init(system);
    cdj_watch_init(system);
    cdj_intc_timer_init(system, cpu);
    cdj_debug_console_init();
    cdj_main_poke_init();

    if (!firmware) {
        error_report("cdj2000-main: pass the MAIN image with -bios "
                     "(firmware/main-firmware.bin)");
        exit(1);
    }
    loaded = load_image_targphys(firmware, ROM_BASE, ROM_SIZE, NULL);
    if (loaded < 0) {
        error_report("cdj2000-main: cannot load '%s'", firmware);
        exit(1);
    }
    /*
     * A blank NOR part reads 0xff, and the ROM loader leaves whatever the
     * image does not cover at zero.  The application erases the settings
     * sectors at 0x3e0000 itself before it uses them, so it never noticed;
     * the loader's recovery run does not touch them and dumped zeros where
     * the part would give 0xff (update-24).  Fill the remainder as a second
     * ROM blob so both firmwares see a blank part from reset onwards.
     */
    if (loaded < ROM_SIZE) {
        void *blank = g_malloc(ROM_SIZE - loaded);

        memset(blank, 0xff, ROM_SIZE - loaded);
        rom_add_blob_fixed("cdj2000.flash-blank", blank, ROM_SIZE - loaded,
                           ROM_BASE + loaded);
        g_free(blank);
    }

    reset = g_new0(CdjResetState, 1);
    reset->cpu = cpu;
    /* Reset through the uncached P2 mirror, which is what the hardware does. */
    reset->vector = ROM_BASE | 0xa0000000;
    qemu_register_reset(cdj_cpu_reset, reset);
}

static void cdj2000_main_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-2000 MAIN board (SH-4)";
    mc->init = cdj2000_main_init;
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->default_ram_size = SDRAM_SIZE;
    /* SDRAM is allocated by the board, as on r2d — no default_ram_id here. */
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

DEFINE_MACHINE("cdj2000-main", cdj2000_main_machine_init)

/* Experimental NXS profile. The boot stack is at physical 0x0c000000;
 * retaining the original player's 64 MiB RAM makes its RAM test fail.
 * Peripheral and C674x emulation are still being validated for this profile.
 */
static void cdj2000_nxs_main_machine_init(MachineClass *mc)
{
    cdj2000_main_machine_init(mc);
    mc->desc = "Pioneer CDJ-2000NXS MAIN board (experimental)";
    mc->default_ram_size = 128 * MiB;
}

DEFINE_MACHINE("cdj2000nxs-main", cdj2000_nxs_main_machine_init)
