/*
 * The CDJ-2000's real DSP: TI C6727 on the C66x core of emulator/c6x/.
 *
 * The chip model (cdj_c6727.c), its host port and core thread
 * (cdj_dsp_host.c), dMAX (cdj_c6727_dmax.c) and audio out (cdj_c6727_audio.c,
 * cdj_host_audio.c, cdj_loop_mute.c) are ported from cdj-nxs2-qemu by Stijn
 * Jacobs (https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, hw/cdj/boards/cdj2000
 * and hw/cdj/common at eea1e0f) to QEMU 11 and this machine. The machine picks
 * it with CDJ_DSP_CORE=c6727; without it the behavioural model in
 * cdj2000_dsp_model.c answers as before.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CDJ_C6727_H
#define CDJ_C6727_H

#include "exec/hwaddr.h"
#include "system/memory.h"
#include "migration/vmstate.h"
#include "qemu/notify.h"

/* The wires between the board's port latch (0xfff10000) and the DSP: MAIN
 * drives a 2-bit command (+0x5c bits 1:0), the DSP answers on a busy line
 * (+0x40 bit 4, its HINT pin, active low). */
typedef struct CdjDspWires {
    void *opaque;
    void (*command)(void *opaque, unsigned bits);
    bool (*busy)(void *opaque);
} CdjDspWires;

/* The C6727 behind a full-address host port on MAIN's area 3 (@hpi_base,
 * 1 MiB: HPIC at +0, the 64 KiB window at +0xC0000). Returns its end of the
 * latch wires. */
const CdjDspWires *cdj_c6727_init(MemoryRegion *sysmem, hwaddr hpi_base);
/* The DSP application's polling main loop, which the core skips while it is
 * a fixed point (c66x_set_idle_loop): its head and the stack range its passes
 * may store to. fw 4.33: 0x80047B80, 0x10005000..0x10006600. */
void cdj_c6727_set_idle_loop(uint32_t head, uint32_t stack_lo,
                             uint32_t stack_hi);

/* CDJ_REPORT=1: print the chip's counters at exit. */
bool cdj_report_enabled(void);
void cdj_add_exit_report(Notifier *n);
/* On Apple Silicon, ask for a performance core for this thread. */
void cdj_thread_prefer_fast_core(void);

/* A span of plain fields, migrated as bytes. */
#define CDJ_VMSTATE_SPAN(_state, _first, _last)                              \
    VMSTATE_BUFFER_UNSAFE(_first, _state, 0,                                 \
                          offsetof(_state, _last) + sizeof_field(_state, _last) \
                          - offsetof(_state, _first))

/* A GByteArray * field, migrated as its length and bytes; NULL saves as empty. */
extern const VMStateInfo cdj_vmstate_info_bytes;
#define CDJ_VMSTATE_BYTES(_field, _state) {                                  \
    .name = stringify(_field),                                               \
    .size = sizeof(GByteArray *),                                            \
    .info = &cdj_vmstate_info_bytes,                                         \
    .flags = VMS_SINGLE,                                                     \
    .offset = vmstate_offset_value(_state, _field, GByteArray *),            \
}

#endif /* CDJ_C6727_H */
