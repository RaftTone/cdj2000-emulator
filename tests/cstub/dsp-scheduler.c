/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SELF-VALIDATING BY CONSTRUCTION, and it cannot be otherwise: the bounded
 * deferred scheduler is a host-side policy for how many interpreter steps to
 * run per QEMU timer callback (cdj_dsp_scheduler.h:13-15), not a model of any
 * C6747 or C674x behaviour. No TI document describes it, so there is no
 * external oracle and no printed page to cite; ACTIVATION_STEPS (1000000) and
 * SLICE_STEPS (4096) are this repository's own choices. What follows is
 * therefore a REGRESSION guard plus a checkpoint-ABI guard - the two
 * _Static_asserts and the state-validity rules are what a schema-11
 * checkpoint reader depends on - and must not be read as architectural
 * validation. DSP_ARCHITECTURE_COVERAGE.md section 7 lists this test among
 * the circular ones; this comment is the answer, not a fix, because the
 * subject has no specification to be faithful to. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "cdj_dsp_scheduler.h"

_Static_assert(sizeof(CdjDspScheduler) == 32, "checkpoint ABI");
_Static_assert(_Alignof(CdjDspScheduler) == 8, "checkpoint ABI alignment");

static void finish_activation(CdjDspScheduler *scheduler)
{
    uint64_t activation = scheduler->activation_id;
    while (scheduler->pending && scheduler->activation_id == activation) {
        uint32_t quota = cdj_dsp_scheduler_begin(scheduler);
        assert(quota && cdj_dsp_scheduler_end(scheduler, quota, false, false));
    }
}

int main(void)
{
    CdjDspScheduler scheduler;
    cdj_dsp_scheduler_reset(&scheduler);
    assert(cdj_dsp_scheduler_valid(&scheduler));
    assert(scheduler.mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1);
    assert(scheduler.slice_steps == 4096 && !scheduler.pending);

    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(scheduler.activation_id == 1 && scheduler.remaining == 1000000 &&
           scheduler.pending && !scheduler.rearm);
    assert(cdj_dsp_scheduler_begin(&scheduler) == 4096);
    assert(scheduler.slice_id == 1);
    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(scheduler.rearm && scheduler.remaining == 1000000);
    CdjDspScheduler before = scheduler;
    assert(!cdj_dsp_scheduler_end(&scheduler, 4095, false, false));
    assert(!memcmp(&scheduler, &before, sizeof(scheduler)));
    assert(cdj_dsp_scheduler_end(&scheduler, 4096, false, false));
    assert(scheduler.remaining == 995904 && scheduler.pending && scheduler.rearm);

    /* A coalesced wake creates one, and only one, new bounded activation. */
    finish_activation(&scheduler);
    assert(scheduler.activation_id == 2 && scheduler.remaining == 1000000 &&
           scheduler.pending && !scheduler.rearm);
    finish_activation(&scheduler);
    assert(!scheduler.pending && !scheduler.remaining &&
           scheduler.activation_id == 2);

    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(cdj_dsp_scheduler_begin(&scheduler) == 4096);
    assert(cdj_dsp_scheduler_end(&scheduler, 7, true, false));
    assert(!scheduler.pending && !scheduler.remaining && !scheduler.rearm);
    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(cdj_dsp_scheduler_begin(&scheduler) == 4096);
    assert(cdj_dsp_scheduler_end(&scheduler, 0, false, true));
    assert(!scheduler.pending && !scheduler.remaining);

    CdjDspScheduler legacy = {0};
    assert(cdj_dsp_scheduler_valid(&legacy));
    assert(!cdj_dsp_scheduler_request(&legacy));
    assert(!cdj_dsp_scheduler_begin(&legacy));
    assert(!cdj_dsp_scheduler_end(&legacy, 0, false, false));

    CdjDspScheduler corrupt = scheduler;
    corrupt.pending = 1;
    assert(!cdj_dsp_scheduler_valid(&corrupt));
    corrupt = scheduler;
    corrupt.slice_steps = 1;
    assert(!cdj_dsp_scheduler_valid(&corrupt));
    corrupt = scheduler;
    corrupt.reserved = 1;
    assert(!cdj_dsp_scheduler_valid(&corrupt));

    CdjDspScheduler overflow;
    cdj_dsp_scheduler_reset(&overflow);
    overflow.activation_id = UINT64_MAX;
    overflow.slice_id = UINT64_MAX - 1;
    assert(cdj_dsp_scheduler_valid(&overflow));
    before = overflow;
    assert(!cdj_dsp_scheduler_request(&overflow));
    assert(!memcmp(&overflow, &before, sizeof(overflow)));

    return 0;
}
