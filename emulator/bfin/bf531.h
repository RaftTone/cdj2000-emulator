/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * From hw/cdj/bfin/bf531.h of Stijn Jacobs' cdj-nxs2-qemu,
 * https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, commit 08d5cb1.
 * Changed 2026-10-05: declares bf531_boot_ldr, bf531_flash and
 * bf531_set_ready_toggle; for the MAIN link (bfin-link): the SPORT1 receive
 * as a pull from the host (sport1_rx), bf531_boot_elf, bf531_set_strap,
 * bf531_set_timing, bf531_read/bf531_write, bf531_cycles.
 */
/*
 * ADSP-BF531 SoC around the Blackfin core, as the CDJ-2000/CDJ-2000NXS
 * display board uses it: SDRAM, L1, the boot flash on async bank 0, the SIC,
 * the three GP timers, the PPI with its DMA channel, SPI, GPIO and the EBIU.
 * SPORT1 carries the MAIN link through its DMA channels. No QEMU dependency.
 */
#ifndef BF531_H
#define BF531_H

#include "bfin.h"
#include <stddef.h>

typedef struct bf531 bf531;

typedef struct bf531_host {
    void *opaque;
    /* One PPI frame, RGB565 pixels, row after row. */
    void (*frame)(void *opaque, const uint16_t *px, unsigned w, unsigned h);
    /* One SPORT1 TX packet for MAIN: the whole unit the firmware armed DMA4
     * with. */
    void (*sport1_tx)(void *opaque, const uint8_t *data, size_t len);
    /* One pump of the SPORT1 receive DMA (DMA3): up to cap bytes for the
     * buffer at addr, the channel's current address. Returns the bytes put
     * in dst, a whole number of DMA elements; 0 retries after the SPORT retry
     * time. Fewer than cap lands them and leaves the channel running for the
     * rest, as a native byte burst does (DMA3_CURR_X_COUNT tells the
     * firmware how much came). */
    unsigned (*sport1_rx)(void *opaque, uint8_t *dst, unsigned cap, uint32_t addr);
} bf531_host;

/* The core clock the firmware assumes: its core timer period of 400,000 is
 * its 1 ms tick. */
#define BF531_CCLK_HZ 400000000ull

bf531 *bf531_new(uint32_t sdram_size, const bf531_host *host, FILE *log);
void   bf531_free(bf531 *s);

/* A Pioneer GUI update section: a 0x20-byte title, then the LDR boot stream
 * and the resources the firmware reads back from flash. The stream is loaded
 * as the boot loader would load it from flash, and the section is placed
 * in flash where the firmware reads it back. Returns 0, or -1 when it is
 * not an LDR stream. */
int bf531_load_update(bf531 *s, const uint8_t *img, size_t len);

/* Boots a bare LDR stream (no title) as the boot ROM would, without touching
 * the flash contents: 0, or -1 when it is not an LDR stream. */
int bf531_boot_ldr(bf531 *s, const uint8_t *ldr, size_t len);

/* The 2 MiB boot flash on async banks 0-1, writable before the run starts. */
uint8_t *bf531_flash(bf531 *s, uint32_t *size);

/* PF bits whose level flips on every read of the flag registers: the GUI
 * flash's READY/BUSY line on PF0 (our gdb sim's BFIN_GPIO5_READY_TOGGLE).
 * 0, the default, leaves them as the firmware drove them. */
void bf531_set_ready_toggle(bf531 *s, uint16_t mask);

/* Boots an ELF32 (gui-boot-memory.elf: the boot stream's blocks as PT_LOAD
 * segments) at its entry, as bin/cdj-run loads it: 0, or -1 when it is not
 * one or a segment has no memory behind it. */
int bf531_boot_elf(bf531 *s, const uint8_t *elf, size_t len);

/* Board straps on the PF port (BFIN_GPIO_STRAP=mask:value): the masked bits
 * of the flag registers read as value. */
void bf531_set_strap(bf531 *s, uint16_t mask, uint16_t value);

/* The display frame period and the SPORT receive retry, in core cycles
 * (defaults: a 60 Hz frame and 1 ms, at BF531_CCLK_HZ). */
void bf531_set_timing(bf531 *s, uint64_t ppi_frame, uint64_t sport_retry);

/* The bus as the core sees it, MMRs included (tests, host probes). */
uint32_t bf531_read(bf531 *s, uint32_t addr, unsigned size);
void     bf531_write(bf531 *s, uint32_t addr, uint32_t val, unsigned size);

/* The PF pins the firmware drives as outputs, bit n for PFn; inputs read 0. */
uint16_t bf531_flags(const bf531 *s);

/* Runs the chip for about n core cycles; returns early on an unimplemented
 * instruction (BFIN_STOP_UNDEF) or when it idles with no event left. */
bfin_stop bf531_run(bf531 *s, uint64_t n);

bfin_core *bf531_core(bf531 *s);
uint64_t   bf531_cycles(const bf531 *s);
uint64_t   bf531_frames(const bf531 *s);
const uint8_t *bf531_sdram(const bf531 *s, uint32_t *size);

#endif
