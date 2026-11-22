/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "cdj2000_dsp_levels.h"

int main(void)
{
    bool pcm;

    /* PCM blocks are 4 CD frames whatever the sample size (the real DSP: +40 every ten blocks). */
    assert(cdj_dsp_transfer_units(9408, &pcm) == 4 && pcm);     /* 16 bit */
    assert(cdj_dsp_transfer_units(14112, &pcm) == 4 && pcm);    /* 24 bit */
    assert(cdj_dsp_transfer_units(18816, &pcm) == 4 && pcm);    /* 32 bit float */
    /* A coded (MP3) block keeps the old measure and is not PCM. */
    assert(cdj_dsp_transfer_units(8192, &pcm) == 34 && !pcm);
    assert(cdj_dsp_transfer_units(4096, &pcm) == 17 && !pcm);

    /* Playing: 75 CD frames a second, the remainder carried over. */
    int64_t carry = 0, frames = 0;
    for (int i = 0; i < 1000; i++) {
        frames += cdj_dsp_played_frames(&carry, 1000000);   /* 1000 steps of 1 ms */
    }
    assert(frames == 75 && carry < 1000000000LL / 75);
    carry = 0;
    assert(cdj_dsp_played_frames(&carry, 13333333) == 0);   /* just under a frame */
    assert(cdj_dsp_played_frames(&carry, 1) == 1);          /* 13333334 ns: one frame */

    /*
     * The ring against MAIN's refill rule (the deck: a burst of 100 blocks when the level
     * ahead has fallen below 1870, a start fill to 2250): over 120 s of play MAIN has to
     * keep reading.  With 40 units a block and nothing played out (the old accounting) the
     * ring is "full" after 57 blocks and it never reads again.
     */
    int32_t ahead = 0;
    int blocks = 0, bursts = 0;
    int64_t played_carry = 0;
    for (int ms = 0; ms < 120000; ms++) {
        if (ahead < 1870 || (blocks == 0 && ahead < 2250)) {
            bursts++;
            do {
                ahead += cdj_dsp_transfer_units(9408, &pcm);
                blocks++;
            } while (ahead < 2250 && (bursts == 1 || blocks % 100 != 0));
        }
        int64_t take = cdj_dsp_played_frames(&played_carry, 1000000);
        ahead = take < ahead ? ahead - (int32_t)take : 0;
    }
    assert(blocks > 562 + 20 * 90);     /* the start fill, then a burst about every 5.3 s */
    assert(ahead >= 1870 - 100 && ahead < 2300);
    return 0;
}
