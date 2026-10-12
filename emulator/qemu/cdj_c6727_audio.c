/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Ported from cdj-nxs2-qemu (Stijn Jacobs), hw/cdj/boards/cdj2000/dsp_audio_out.c at eea1e0f, to QEMU 11
 * and this machine; see cdj_c6727.h.
 */
#include "qemu/osdep.h"
#include "cdj_c6727_audio.h"
#include "cdj_host_audio.h"
#include "cdj_c6727.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "system/system.h"
#include "qemu/timer.h"
/*
 * Host playback of the older decks' DSP output.
 *
 * CDJ_DSP_AUDIO=1[:<ring_ms>][:<prefill_ms>][:<maxlat_ms>]
 *   The DSP sends 44.1 kHz stereo to the DAC over McASP1 as I2S, two 32-bit
 *   slots per frame, each word a left-aligned 24-bit sample. The sink and its
 *   rate control are the NXS2's, in cdj_host_audio.c.
 *
 * CDJ_DSP_PCM_TAP=<file.wav>
 *   Every frame the DSP sends to the DAC, in the order the dMAX sends them on
 *   the DSP's own clock, as 44.1 kHz 16-bit stereo: the file's time line is
 *   the deck's, with no ring, rate control or host clock in between, so a
 *   silence in it is a silence the DSP produced. The header's sizes are
 *   written at exit. (This tap is ours, not cdj-nxs2-qemu's.) Its first
 *   frame and every 10 s of frames log the virtual clock ("PCM tap frame N
 *   at t=..."), to put the file on the same time line as the panel presses.
 */
#define DAC_MCASP       1

static CdjHostAudio *sink;
static FILE *tap;
static uint64_t tap_frames;
static Notifier tap_exit;

static void tap_header(FILE *f, uint64_t frames)
{
    uint32_t data = frames * 4 > UINT32_MAX - 36 ? UINT32_MAX - 36 : frames * 4;
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0"
                    "\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data";

    stl_le_p(h + 4, data + 36);
    stl_le_p(h + 40, data);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

static void tap_close(Notifier *n, void *data)
{
    if (tap) {
        tap_header(tap, tap_frames);
        fclose(tap);
        tap = NULL;
        info_report("dsp: PCM tap closed, %" PRIu64 " frames (%.3f s of DSP time)",
                    tap_frames, tap_frames / 44100.0);
    }
}
static uint32_t left_word;
static bool have_left;

void cdj_dsp_audio_arm(void)
{
    static const CdjHostAudioCfg cfg = {
        .env = "CDJ_DSP_AUDIO", .card = "cdj-dsp", .voice = "cdj-dsp-mcasp1",
        .label = "dsp", .source = "McASP1",
        /* Paused, the DSP program keeps sending the McASP buffer it last
         * filled: 15288 frames (two 7644-frame halves) at deck speed 1.0, bit
         * for bit, on both chips. Right after the stop it first sends the
         * buffer from 17880 frames back. */
        .loop = { .period = 15288, .replay = 17880,
                  .lag_min = 12000, .lag_max = 24000 },
    };

    const char *path = getenv("CDJ_DSP_PCM_TAP");

    sink = cdj_host_audio_open(&cfg);
    if (path && *path) {
        tap = fopen(path, "wb");
        if (!tap) {
            warn_report("dsp: cannot open CDJ_DSP_PCM_TAP %s", path);
        } else {
            tap_header(tap, 0);
            tap_exit.notify = tap_close;
            qemu_add_exit_notifier(&tap_exit);
            info_report("dsp: PCM tap -> %s", path);
        }
    }
}

void cdj_dsp_audio_frame(unsigned mcasp, uint32_t left, uint32_t right)
{
    if (tap && mcasp == DAC_MCASP) {
        uint8_t f[4];

        stw_le_p(f, (int32_t)left >> 16);
        stw_le_p(f + 2, (int32_t)right >> 16);
        fwrite(f, 1, sizeof(f), tap);
        if (tap_frames % 441000 == 0) {
            info_report("dsp: PCM tap frame %" PRIu64 " at t=%.6f", tap_frames,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
        }
        tap_frames++;
    }
    if (sink && mcasp == DAC_MCASP) {
        cdj_host_audio_put(sink, (int32_t)left >> 16, (int32_t)right >> 16);
    }
}

void cdj_dsp_audio_word(unsigned mcasp, uint32_t word)
{
    if (!sink || mcasp != DAC_MCASP) {
        return;
    }
    if (!have_left) {
        left_word = word;
        have_left = true;
        return;
    }
    have_left = false;
    cdj_dsp_audio_frame(mcasp, left_word, word);
}
