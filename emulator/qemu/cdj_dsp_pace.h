/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * How closely the CDJ-2000's DSP core is held to MAIN's virtual clock
 * (cdj_dsp_host.c). Pure, so tests/cstub/dsp-pace.c checks it on the host.
 */
#ifndef CDJ_DSP_PACE_H
#define CDJ_DSP_PACE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* The core runs in chunks of DSP time; a chunk may end at most AHEAD past
 * the virtual clock, so the core stops within CHUNK - AHEAD before it. */
#define CDJ_DSP_CHUNK_NS        (100 * 1000)
#define CDJ_DSP_AHEAD_NS        (CDJ_DSP_CHUNK_NS / 2)
#define CDJ_DSP_SLACK_POLL_MAX  (1000 * 1000)

/*
 * The slack from CDJ_<chip>_SLACK_US (@env, NULL when unset): unset, one
 * chunk under -icount and none without it, where holding MAIN does not stop
 * the virtual clock. 0 is no hold. A slack under CHUNK - AHEAD would hold
 * MAIN for a core that has already stopped short of it, for good.
 */
static inline int64_t cdj_dsp_slack_ns(const char *env, bool icount)
{
    int64_t ns;

    if (!env) {
        return icount ? CDJ_DSP_CHUNK_NS : 0;
    }
    ns = (int64_t)strtoull(env, NULL, 0) * 1000;
    return ns && ns < CDJ_DSP_CHUNK_NS - CDJ_DSP_AHEAD_NS
           ? CDJ_DSP_CHUNK_NS - CDJ_DSP_AHEAD_NS : ns;
}

/* How often the hold looks: as often as the slack, between one chunk and
 * the old millisecond. */
static inline int64_t cdj_dsp_slack_poll_ns(int64_t slack_ns)
{
    return slack_ns < CDJ_DSP_CHUNK_NS ? CDJ_DSP_CHUNK_NS
         : slack_ns > CDJ_DSP_SLACK_POLL_MAX ? CDJ_DSP_SLACK_POLL_MAX
         : slack_ns;
}

#endif
