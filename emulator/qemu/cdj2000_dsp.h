/*
 * Pioneer CDJ-2000 audio DSP — the seam between the bus and the model.
 *
 * The CDJ-2000 carries a separate audio DSP with its own memory.  MAIN reaches
 * it through three things and nothing else:
 *
 *   - a 64 KiB shared window at physical 0x0C0C0000 (P2 0xAC0C0000),
 *   - one DMAC channel that fills or drains that window,
 *   - a handful of 16-bit control registers around 0xfff10040.
 *
 * cdj2000_dsp.c models exactly that bus surface and knows nothing about what
 * the DSP *does*.  Everything the virtual DSP "is" lives behind the five
 * functions below, implemented by cdj2000_dsp_model.c.  That separation is
 * deliberate: the built-in model is a behavioural stand-in with no audio path
 * at all, and a real engine — in-process or out through the optional chardev —
 * can replace it without the device changing.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CDJ2000_DSP_H
#define CDJ2000_DSP_H

#include "exec/hwaddr.h"
#include "hw/core/irq.h"
#include "chardev/char.h"
#include "system/memory.h"

/*
 * The shared window.  Both numbers are read off the firmware rather than
 * assumed: 0x1c75f4 computes "0x10000 minus how far the destination already is
 * past 0xAC0C0000" and clamps every transfer to it, which fixes the base and
 * the size at once.  Only the low 29 bits reach the bus, so the P0/P1/P2
 * mirrors all land on the same physical page.
 */
#define CDJ_DSP_WINDOW_BASE   0x0C0C0000
#define CDJ_DSP_WINDOW_SIZE   0x10000

/*
 * The mailbox at the top of the window.  Three four-line accessors sit side by
 * side and name half of it outright:
 *
 *   0x2b0d6c   return L[0xac0cfffc]      read the answer
 *   0x2b0d72   L[0xac0cffec] = 0         lower the request
 *   0x2b0d7a   L[0xac0cffec] = 1         raise the request
 *
 * and the bring-up's own poller at 0x1c778a names the other half.  It loads the
 * mailbox base from [0x483d4e4] — which state 3 sets to 0xAC0C0000 + W[+0x0e] of
 * the firmware header, i.e. 0xac0cfff0 — and then:
 *
 *   loop:  if (L[base + 4] == 1) goto up      ; 0xac0cfff4
 *          delay 6750; 3000 times; return -1
 *   up:    L[0xac0cffec] = 0                  ; lower the request
 *          L[base] = 1                        ; 0xac0cfff0
 *          return 6
 *
 * So 0xac0cfff4 is the DSP saying "I am running", and it is the whole of what
 * bring-up state 5 waits for.  Offsets are relative to the window base.
 */
#define CDJ_DSP_MAIL_EVENT    0xffe8      /* the DSP's event word, see cdj_dsp_event */
#define CDJ_DSP_MAIL_REQ      0xffec
#define CDJ_DSP_MAIL_BASE     0xfff0
#define CDJ_DSP_MAIL_UP       0xfff4
#define CDJ_DSP_MAIL_ACK      0xfffc

typedef struct CdjDspModel CdjDspModel;

/*
 * Create a model.  With a chardev the doorbell is forwarded to whatever is on
 * the other end; without one the built-in transport model answers.  The
 * built-in one is the default because the DSP link is DMA plus polled
 * registers with 3 ms deadlines, and a process boundary in that path turns a
 * deterministic run into an occasionally deterministic one.
 */
CdjDspModel *cdj_dsp_model_new(Chardev *external);

/*
 * The run bit went up: the DSP restarts.  It is handed the window because
 * resetting means its side of the mailbox goes quiet again, and that is the
 * model's statement to make, not the bus's.
 */
void cdj_dsp_model_reset(CdjDspModel *model, uint8_t *window, size_t length);

/*
 * A block of the DSP's firmware has landed in the window.  MAIN downloads it
 * from inside its own image (header at 0xa4001000, payload at 0xa4001010) in
 * pages, so this is called once per page with the window offset it went to.
 */
void cdj_dsp_model_firmware(CdjDspModel *model, uint8_t *window,
                            size_t length, uint32_t offset, unsigned bytes);

/*
 * MAIN read the mailbox's "up" word and found it set.  The firmware pages are
 * staged through the same window region the control block lives in (+0x7800
 * onwards), so at this point the block still holds the last page's bytes; a
 * DSP that has just booted has initialised its own memory instead.
 */
void cdj_dsp_model_up_seen(CdjDspModel *model, uint8_t *window, size_t length);

/*
 * MAIN raised the request word.  The model reads the command out of the window
 * and writes its answer back into the same window; returning true means it
 * answered and the device should acknowledge.
 */
bool cdj_dsp_model_doorbell(CdjDspModel *model, uint8_t *window, size_t length);

/* Virtual time has passed: advance whatever the model keeps running. */
void cdj_dsp_model_tick(CdjDspModel *model, uint8_t *window, size_t length);

/*
 * The position report alone, on its own finer timer (CDJ_DSP_POSITION_US):
 * MAIN reads it every ~3 ms and times its beats against it.
 */
void cdj_dsp_model_position_tick(CdjDspModel *model, uint8_t *window, size_t length);

/*
 * The DSP's interrupt line to MAIN.  The device raises `irq` (irq 0x7f,
 * INTEVT 0xfe0) and tells the board through `pending` so that the two status
 * bits MAIN tests -- bit 24 of 0xffd4005c in the vector stub 0x26260c, bit 4
 * of GPIO 0xfff10040 in the poll 0x1c7ce4 -- follow the line.  MAIN's handler
 * 0x1c09a0 reads the event word at window+0xffe8, takes bytes 2 and 3 as the
 * event code and acknowledges with bit 2 of the control register, which
 * lowers the line again.
 */
typedef void (*CdjDspPendingFn)(void *opaque, bool raised);

/* The device side, called from the board. */
void cdj_dsp_init(MemoryRegion *system, Chardev *external, qemu_irq irq,
                  CdjDspPendingFn pending, void *pending_opaque);

/*
 * Post an event to MAIN: the word goes to window+0xffe8 and the line goes up
 * until MAIN acknowledges.  `code` lands in bytes 2 and 3 the way 0x1c09a0
 * reads them (byte 2 high, byte 3 low).  For the model.
 */
void cdj_dsp_event(unsigned code);

/* True when a DMA endpoint is the DSP's window — used to pick the DMA role. */
bool cdj_dsp_is_window(hwaddr address);

/*
 * A DMA transfer touching the window has finished.  The device hands the
 * window to the model and clears its busy bit.
 */
void cdj_dsp_transfer_done(hwaddr source, hwaddr destination, unsigned bytes);

/* A DMA transfer touching the window has started: the DSP is busy. */
void cdj_dsp_transfer_start(void);

#endif /* CDJ2000_DSP_H */
