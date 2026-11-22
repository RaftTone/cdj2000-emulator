/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ2000_DSP_LEVELS_H
#define CDJ2000_DSP_LEVELS_H
/*
 * What the behavioural DSP model books into the two fill levels (+0x7ccc,
 * +0x7cd0), as pure functions so tests/cstub/dsp-levels.c can check them.
 *
 * The levels are CD frames of audio (1/75 s).  The real DSP program (the
 * C6727 core, CDJ_DSP_CORE=c6727, stock MAIN 4.33, a 16 bit WAV with
 * analysis files) books 4 for every 9408-byte PCM block -- it grows
 * +0x7cd0 by 40 every ten blocks -- to 2246 after the load's 591 blocks,
 * takes 75 a second out of it while the deck plays, and MAIN pushes the
 * next burst of 100 blocks whenever it has fallen to about 1870.
 * The model used to book 40 per block, ten times too much, and never took
 * anything out: MAIN saw a full ring after 60 blocks (3 s of audio) and
 * stopped reading for good.
 */
#include <stdbool.h>
#include <stdint.h>

/* A PCM block is 2352 frames (4 CD frames at 44.1 kHz) whatever the sample
   size: 9408 bytes for 16 bit, 14112 for 24 bit, 18816 for 32 bit float. */
#define CDJ_DSP_PCM_FRAMES_PER_BLOCK    2352
#define CDJ_DSP_PCM_BLOCK_UNITS         4

/* The CD frames a data transfer of BYTES bytes adds to its buffer's level.
   *PCM tells the caller whether it was a PCM block: a whole number of 2352
   bytes, at least 9408 (a coded block of 8192 is neither).  Coded streams
   (MP3) keep the old measure, 40 units for 9408 bytes: their
   bytes-per-frame depends on the bitrate and has not been measured. */
static inline int32_t cdj_dsp_transfer_units(unsigned bytes, bool *pcm)
{
    *pcm = bytes >= 4 * CDJ_DSP_PCM_FRAMES_PER_BLOCK
           && bytes % CDJ_DSP_PCM_FRAMES_PER_BLOCK == 0;
    return *pcm ? CDJ_DSP_PCM_BLOCK_UNITS : (int32_t)((uint64_t)bytes * 40 / 9408);
}

/* Whole CD frames played in PLAYED_NS of audio time, the part of a frame
   below the step kept in *CARRY_NS for the next call. */
static inline int64_t cdj_dsp_played_frames(int64_t *carry_ns, int64_t played_ns)
{
    int64_t total = *carry_ns + played_ns;
    int64_t frames = total * 75 / 1000000000LL;

    *carry_ns = total - frames * 1000000000LL / 75;
    return frames;
}

#endif
