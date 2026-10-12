/* SPDX-License-Identifier: GPL-2.0-or-later
 * McASP slot deadlines on the threaded DSP's clock (CDJ_NXS_DSP_AUDIO_CLOCK=
 * virtual).  The DSP thread's time is epoch_ns + (packets - epoch_packets) /
 * rate; it runs at most a quantum ahead of QEMU_CLOCK_VIRTUAL and, when the
 * host is too slow, gives the lag up (slips its epoch forward).  Slots are due
 * at fixed instants of that time at the McASP's configured slot rate, so audio
 * time follows virtual time whatever the host speed: a slot whose deadline a
 * slip jumped over fires late, without the DSP packets the configured clock
 * owed it, and is counted as an underrun instead of stalling time.
 * Pattern after Stijn Jacobs' cdj-nxs2-qemu (CDJ_C6X_MHZ: peripherals on
 * virtual time, a configurable DSP clock), with the author's permission.
 */
#ifndef CDJ_DSP_AUDIO_CLOCK_H
#define CDJ_DSP_AUDIO_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t rate_num;      /* slots per second = rate_num / rate_den */
    uint32_t rate_den;
    int64_t next_ns;        /* deadline of the next slot */
    uint64_t phase;         /* next_ns remainder, in 1/rate_num ns */
    int64_t late_until_ns;  /* deadlines up to here were jumped by a slip */
    uint64_t slots, late;
} CdjDspAudioClock;

/* Packets the DSP clock has executed by time @ns (dsp_thread_packets_at). */
static inline uint64_t cdj_dsp_clock_packets_at(int64_t epoch_ns,
                                                uint64_t epoch_packets,
                                                uint64_t packets_per_us,
                                                int64_t ns)
{
    return epoch_packets +
           (uint64_t)(ns > epoch_ns ? ns - epoch_ns : 0) * packets_per_us / 1000;
}

/* Move next_ns one slot period on (exact: the remainder is carried). */
static inline void cdj_dsp_audio_clock_advance(CdjDspAudioClock *c)
{
    c->phase += (uint64_t)c->rate_den * 1000000000u;
    c->next_ns += (int64_t)(c->phase / c->rate_num);
    c->phase %= c->rate_num;
}

/* (Re)start at @now_ns with slots at rate_num/rate_den per second; the first
 * is due one period later; the slots/late counters carry on.  False if the
 * rate cannot be represented (phase + rate_den * 1e9 must fit in 64 bits). */
static inline bool cdj_dsp_audio_clock_start(CdjDspAudioClock *c,
                                             uint64_t rate_num,
                                             uint32_t rate_den, int64_t now_ns)
{
    if (!rate_num || !rate_den || rate_num > UINT64_MAX / 2) return false;
    c->rate_num = rate_num;
    c->rate_den = rate_den;
    c->next_ns = now_ns;
    c->phase = 0;
    c->late_until_ns = INT64_MIN;
    cdj_dsp_audio_clock_advance(c);
    return true;
}

/* Account one slot fired at its deadline (next_ns), then advance. */
static inline void cdj_dsp_audio_clock_fire(CdjDspAudioClock *c)
{
    ++c->slots;
    if (c->next_ns <= c->late_until_ns) ++c->late;
    cdj_dsp_audio_clock_advance(c);
}

#endif
