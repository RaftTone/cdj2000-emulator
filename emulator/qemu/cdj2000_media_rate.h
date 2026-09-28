/*
 * Pioneer CDJ-2000 -- how fast the SD card and the USB stick read, in guest
 * time, and a log of the reads.
 *
 * Both media models hand data over as fast as the host reads the image file:
 * the SD host 20 us of guest time per 512-byte block (SDHI_XFER_NS, which is
 * there for interrupt ordering, not for speed), the USB host a bulk packet the
 * moment its IN token goes out.  A LOAD that walks export.pdb and the track's
 * analysis files is therefore over in a fraction of the time a deck takes.
 *
 * CDJ_SD_READ_BPS and CDJ_USB_READ_BPS put a rate on the data of a read, in
 * bytes per second of guest time (QEMU_CLOCK_VIRTUAL): each SD block, and each
 * bulk IN packet on the USB side, reaches the driver no earlier than
 * size / rate after it was asked for.  The bytes are the same; only the moment
 * they arrive changes.  Unset or 0 keeps each model's own timing exactly.
 *
 * The rate is per transfer unit, and the guest's own time between two units
 * comes on top: a driver that needs 100 us to arm the next 512-byte block sees
 * at most 512 / (512 / rate + 100 us).  That is what a deck does too -- the
 * SDHI has one block buffer and holds the card clock while it is full -- and it
 * is why the number to set is the one that reproduces a time measured on a
 * deck, not a card's rating.  RUNNING.md ("Environment") says how to measure.
 *
 * CDJ_READ_LOG=1 prints one line per burst of reads on either medium (a burst
 * ends after READ_LOG_GAP_NS of guest time without data): its start and end in
 * guest seconds, the bytes, the read commands and how much of it the rate
 * added.  CDJ_READ_LOG=2 also prints every read command with its block address
 * as it is issued, which a walk of the image's FAT turns into a file name.
 *
 * Header-only: two small users, and the repository's meson hook lists .c
 * files only.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CDJ2000_MEDIA_RATE_H
#define CDJ2000_MEDIA_RATE_H

#include "qemu/error-report.h"
#include "qemu/timer.h"

#define READ_LOG_GAP_NS (50 * 1000 * 1000)

typedef struct CdjMediaRate {
    const char *name;           /* "sd", "usb": the log's prefix */
    uint64_t bps;               /* bytes per guest second; 0 = no limit */
    int log;                    /* CDJ_READ_LOG: 0 off, 1 bursts, 2 + commands */
    QEMUTimer *burst_timer;     /* ends a burst after READ_LOG_GAP_NS quiet */
    bool in_burst;
    int64_t first_ns, last_ns;
    uint64_t bytes, commands;
    int64_t added_ns;           /* the part of the burst the rate added */
} CdjMediaRate;

/*
 * A plain decimal count, or nothing.  A typo must not quietly mean "no
 * limit": a run that was meant to be slow and was not reads exactly like a
 * deck that is as fast as the model.
 */
static inline uint64_t cdj_media_rate_env(const char *variable)
{
    const char *spec = getenv(variable);
    uint64_t value = 0;
    const char *c;

    if (spec == NULL || *spec == '\0') {
        return 0;
    }
    for (c = spec; *c; c++) {
        if (*c < '0' || *c > '9' || value > UINT64_MAX / 10 - 1) {
            error_report("cdj2000: %s: not a number of bytes per second: %s",
                         variable, spec);
            exit(1);
        }
        value = value * 10 + (uint64_t)(*c - '0');
    }
    return value;
}

static inline void cdj_media_rate_burst_end(void *opaque)
{
    CdjMediaRate *r = opaque;
    double span = (r->last_ns - r->first_ns) / 1e9;

    if (!r->in_burst) {
        return;
    }
    fprintf(stderr, "cdj2000-%s: reads t=%.6f..%.6f (%.3f s): %" PRIu64
            " bytes, %" PRIu64 " commands, %.3f s added by the rate%s\n",
            r->name, r->first_ns / 1e9, r->last_ns / 1e9, span, r->bytes,
            r->commands, r->added_ns / 1e9, r->bps ? "" : " (no limit)");
    r->in_burst = false;
    r->bytes = r->commands = 0;
    r->added_ns = 0;
}

/*
 * `variable` is the rate's environment variable; CDJ_READ_LOG is shared.
 * Called from realize, where virtual-clock timers may be created.
 */
static inline void cdj_media_rate_init(CdjMediaRate *r, const char *name,
                                       const char *variable)
{
    const char *log = getenv("CDJ_READ_LOG");

    r->name = name;
    r->bps = cdj_media_rate_env(variable);
    r->log = log && *log ? atoi(log) : 0;
    if (r->log) {
        r->burst_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      cdj_media_rate_burst_end, r);
    }
    if (r->bps) {
        fprintf(stderr, "cdj2000-%s: reads limited to %" PRIu64
                " bytes per guest second (%s)\n", name, r->bps, variable);
    }
}

/* The guest time `bytes` take at the rate; 0 without one. */
static inline int64_t cdj_media_rate_ns(const CdjMediaRate *r, unsigned bytes)
{
    if (!r->bps) {
        return 0;
    }
    return (int64_t)((uint64_t)bytes * NANOSECONDS_PER_SECOND / r->bps);
}

static inline void cdj_media_rate_touch(CdjMediaRate *r, int64_t now)
{
    if (!r->in_burst) {
        r->in_burst = true;
        r->first_ns = now;
    }
    r->last_ns = now;
    timer_mod(r->burst_timer, now + READ_LOG_GAP_NS);
}

/* A read command went out: `what` names it, `address` and `count` in blocks. */
static inline void cdj_media_rate_command(CdjMediaRate *r, const char *what,
                                          uint32_t address, uint32_t count)
{
    int64_t now;

    if (!r->log) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    cdj_media_rate_touch(r, now);
    r->commands++;
    if (r->log >= 2) {
        fprintf(stderr, "cdj2000-%s: read %s block %#x x%u t=%.6f\n",
                r->name, what, address, count, now / 1e9);
    }
}

/* `bytes` of read data reached the driver, `added_ns` of it the rate's. */
static inline void cdj_media_rate_data(CdjMediaRate *r, unsigned bytes,
                                       int64_t added_ns)
{
    if (!r->log) {
        return;
    }
    cdj_media_rate_touch(r, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    r->bytes += bytes;
    r->added_ns += added_ns;
}

#endif /* CDJ2000_MEDIA_RATE_H */
