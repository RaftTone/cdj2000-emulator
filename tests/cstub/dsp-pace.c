/* SPDX-License-Identifier: GPL-2.0-or-later */
/* emulator/qemu/cdj_dsp_pace.h: how closely the C6727 core is held to MAIN's
 * virtual clock. tests/test_c6727_pace.py. */
#include <assert.h>
#include "cdj_dsp_pace.h"

int main(void)
{
    const int64_t chunk = CDJ_DSP_CHUNK_NS, floor = chunk - CDJ_DSP_AHEAD_NS;
    const char *given[] = { NULL, "0", "10", "100", "2000", "30000" };

    /* Unset: one chunk under -icount, nothing without it. */
    assert(cdj_dsp_slack_ns(NULL, true) == chunk);
    assert(cdj_dsp_slack_ns(NULL, false) == 0);
    /* Set: as given, 0 = no hold, never under the floor. */
    assert(cdj_dsp_slack_ns("0", true) == 0);
    assert(cdj_dsp_slack_ns("10", true) == floor);
    assert(cdj_dsp_slack_ns("2000", true) == 2000 * 1000);
    /* The hold looks as often as its slack, one chunk to a millisecond. */
    assert(cdj_dsp_slack_poll_ns(0) == chunk);
    assert(cdj_dsp_slack_poll_ns(chunk) == chunk);
    assert(cdj_dsp_slack_poll_ns(300 * 1000) == 300 * 1000);
    assert(cdj_dsp_slack_poll_ns(2000 * 1000) == 1000 * 1000);

    /* Wherever the core thread stops (its next chunk would end more than
     * AHEAD past the clock), any hold is already met, so MAIN is never held
     * for a core that will not run. */
    for (unsigned i = 0; i < sizeof(given) / sizeof(given[0]); i++) {
        int64_t slack = cdj_dsp_slack_ns(given[i], true);
        const int64_t virt = 1000 * 1000 * 1000;

        for (int64_t dsp = virt - 2 * chunk; dsp <= virt + chunk; dsp++) {
            if (dsp + chunk > virt + CDJ_DSP_AHEAD_NS && slack) {
                assert(dsp >= virt - slack);
            }
        }
    }
    return 0;
}
