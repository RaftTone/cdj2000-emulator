/*
 * Pioneer CDJ-2000 audio DSP — the model.
 *
 * This is everything the virtual DSP *is*.  cdj2000_dsp.c owns the window, the
 * DMA and the registers and knows nothing about meaning; this file knows only
 * meaning and never touches a register.  The point of the split is that a real
 * engine can take this file's place, in-process or through the chardev, without
 * the device changing.
 *
 * What it deliberately is not: there is no audio path here at all.  No PCM, no
 * decoder, no filters, no output.  MAIN needs a DSP that answers and keeps a
 * position running; that is what this provides.
 *
 * The command vocabulary is being filled in from evidence rather than guessed.
 * What is settled so far:
 *
 *   - the request word is 0xac0cffec, the answer word 0xac0cfffc, and 0xac0cfff0
 *     carries an argument (every firmware record header repeats it as W[+0x0e]);
 *   - MAIN downloads two firmware records into the window, the second to offset
 *     0x7800 — which is where the addresses it reads all over the image
 *     (0xac0c7ba0, 0xac0c7ccc, 0xac0c8140, 0xac0c81a0 …) live, so that region is
 *     the shared control block rather than code;
 *   - DspTASK (0x1c80aa) dispatches on a byte through a 13-entry table at
 *     0x1c8054, and tsk_DJcontTxDspPCM/DEC each own a _cmd and a _ret buffer.
 *
 * Until each field is measured on a running machine it is left alone: writing
 * plausible values into a block the firmware then checksums is a good way to
 * turn a missing device into a wrong one, which is harder to diagnose.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"

#include "cdj2000_dsp.h"
#include "cdj2000_input.h"

/* Where MAIN's second firmware record lands, i.e. the shared control block. */
#define DSP_CONTROL_OFFSET   0x7800
/* The two fill levels MAIN reads and the count word of a stream header. */
#define DSP_LEVEL_BUFFER1   0x7cd0
#define DSP_LEVEL_BUFFER2   0x7ccc
#define DSP_HEADER_COUNT    0x8144
#define DSP_HOT_SLOTS       10      /* +0x7c80 slots: 0 the cue, 1.. hot cue A.., +5 for 0x12/0x22 */
#define DSP_LOOP_OUT_SLOT   5       /* slot 5 + n: the OUT of the loop on slot n */

/* Transport states.  Named after what the deck does, not after a wire value —
   the wire values are still being measured. */
typedef enum {
    CDJ_DSP_STOPPED = 0,
    CDJ_DSP_CUED,
    CDJ_DSP_PLAYING,
} CdjDspTransport;

struct CdjDspModel {
    CharFrontend external;
    bool have_external;

    /* What MAIN downloaded, so a run can say whether the transfer arrived. */
    uint64_t firmware_bytes;
    unsigned firmware_records;
    uint32_t last_offset;

    CdjDspTransport transport;
    int64_t position_ms;                /* playing position */
    int64_t last_tick_ns;

    /* CDJ_DSP_RATE_LOG (on unless 0): the rate word +0x7bc0, last seen */
    bool rate_log;
    bool rate_seen;
    uint32_t rate_last;
    uint32_t rate_flags_last;           /* +0x7bc4, as the rate log last saw it */
    uint32_t rate_hold_last;            /* +0x7bcc, ditto */
    bool rate_valid_seen;               /* MAIN has written a real rate */
    int32_t tempo_ppm;                  /* parts per million, 0 = nominal */

    bool running;                       /* code loaded and the run bit up */
    bool absent;                        /* CDJ_DSP_ABSENT: never answer */
    bool ack_control;                   /* CDJ_DSP_ACK: clear control-block commands */
    bool control_cleared;               /* CDJ_DSP_ACK: block zeroed once MAIN saw "up" */
    unsigned stream_buffer;             /* CDJ_DSP_ACK: buffer the last data header named, 1 or 2 */

    /* CDJ_DSP_SLOT_REPORT: the slot table entry MAIN reads after an event. */
    bool slot_report;
    unsigned slot_loaded_state;         /* CDJ_DSP_SLOT_REPORT=<n>: the state written after the load */
    bool saw_load_end;                  /* +0x7ba0 = 4 seen: the load's closing sequence */
    unsigned slot_state;                /* what the entries say: 0 none, 2 loaded, 3 playing */
    unsigned slot_event;                /* CDJ_DSP_SLOT_EVENT: the event posted with a report */
    unsigned play_event;                /* CDJ_DSP_PLAY_EVENT: the event posted with each periodic state-3 report */
    int64_t slot_pos_ms;                /* the position the entries report */
    int64_t slot_period_ns;             /* CDJ_DSP_SLOT_PERIOD_MS: repeat while playing */
    int64_t slot_last_ns;               /* when the position was last advanced */
    int64_t consume_rate;               /* CDJ_DSP_CONSUME: level units per second while playing */
    int64_t consume_carry_ms;
    int64_t consumed;                   /* total units taken since PLAY */
    int64_t consume_report_ns;
    bool status_flags;                  /* CDJ_DSP_STATUS_FLAGS: buffer-active bits while playing */
    bool status_flags_set;
    unsigned refill_event;              /* CDJ_DSP_REFILL_EVENT: posted when buffer 1 runs low */
    int32_t refill_low;                 /* CDJ_DSP_REFILL_LOW: the level that asks for more */
    bool refill_asked;                  /* posted for the current dip already */

    /* CDJ_DSP_POSITION: the position report block +0x7bf0.. MAIN reads every tick. */
    bool pos_report;
    unsigned pos_state;                 /* 0 nothing loaded, 2 loaded/stopped, 3 playing */
    int64_t pos_ms;                     /* position of the deck, milliseconds */
    int64_t cue_ms;                     /* slot 0, the cue point: where 0x11/0x21 slot 0 go */
    int64_t hot_ms[DSP_HOT_SLOTS];      /* slots 1..9 (hot cues): what 0x11/0x12 recorded, -1 empty */
    int64_t pos_last_ns;                /* last advance while playing */
    int64_t pos_rem_ns;                 /* audio time not yet a whole ms, carried over */
    int64_t pos_print_ns;
    bool pos_standby;                   /* +0x7ba0 = 4 (cue standby): 0x21 does not run */
    int64_t loop_in_ms;                 /* segment slot 1: command 1 (IN) and 2 (OUT) */
    int64_t loop_out_ms;
    bool loop_on;                       /* both points set; 0xc (flush) clears */
    /*
     * CDJ_DSP_LOOP_SELECT: the loop table of the 4.33 DSP, see
     * cdj_dsp_model_loop_select.  Per loop n (the one on slot n, 0..4): the
     * end command 9 cached (0x10024f98 + 16 n), and whether an OUT was built
     * for it (0x10024fe8 + 16 n, from slot 5 + n).
     */
    bool loop_select;
    bool slot_copy;                     /* CDJ_DSP_SLOT_COPY: +0x7c80 = 0x31 / 0x33 */
    bool loop_select_seen;
    uint32_t loop_select_word;          /* +0x7bc8 as last seen */
    int64_t seg_end_ms[DSP_LOOP_OUT_SLOT];
    bool seg_end_valid[DSP_LOOP_OUT_SLOT];
    bool loop_built[DSP_LOOP_OUT_SLOT];
    uint32_t pos_record;                /* +0x8120 of the last +0x8100 command: the record id the report names */
    uint32_t fmt_record;                /* +0x8120 of the last format command, any kind */
    bool fmt_seen;
    bool flush_reset;                   /* +0x7cb0 = 1/2 resets as 0x80034c08 does */
    bool job_answer;                    /* +0x7ba4 = 1 (a job) answered with 0 */
    bool job_consume;                   /* CDJ_DSP_JOB_CONSUME: the job empties buffer 1's level */
    bool job_advance;                   /* CDJ_DSP_JOB_ADVANCE: ... and moves the position by it */
    bool events;                        /* CDJ_DSP_EVENTS: post event 5 as the DSP does */
    bool slot_msg;                      /* CDJ_DSP_SLOT_MSG: +0x7c00 after a slot is recorded or jumped to */
    bool slot_ready_event;              /* CDJ_DSP_SLOT_READY_EVENT: event 1 with the slot, as well */
    bool states433;                     /* CDJ_DSP_STATES: +0x7ba0 run/stand by the DSP's state table */
    bool loop_in_slot0;                 /* CDJ_DSP_LOOP_SLOT0: 0x11 slot 0 is also the loop's IN */
    bool loop_slots;                    /* CDJ_DSP_LOOP_SLOTS: slot 5 + n is loop n's OUT */
    bool reverse;                       /* CDJ_DSP_REVERSE: +0x7bc4 bit 31 plays backwards */
    bool slot_entry;                    /* CDJ_DSP_SLOT_ENTRY: the +0x7ce0 slot entry after 0x11/0x21 */
    bool auto_cue;                      /* CDJ_DSP_AUTO_CUE: request 7 answered with state 8 */
    bool status_record;                 /* CDJ_DSP_STATUS_RECORD: the buffers' record bytes */
    bool status_current;                /* CDJ_DSP_STATUS_CURRENT: both blocks name the record played */
    bool status_first;                  /* CDJ_DSP_STATUS_FIRST: ... or the first one registered after a flush */
    uint32_t first_record;              /* +0x8120 of the first +0x8100 = 3 since the flush, 0 = none */
    uint64_t jobs;
    uint32_t seek_applied[3];           /* +0x8154/8/c of the last start taken */
    bool seek_armed;                    /* the record was just named: its start is the position */
    uint32_t rec_queue[10];             /* records opened since the load began, in order */
    unsigned rec_count;

    /* CDJ_DSP_EVENT_PROBE: post event codes to MAIN on a schedule. */
    int64_t probe_start_ns;
    int64_t probe_interval_ns;
    int64_t probe_last_ns;
    unsigned probe_list[32];            /* the codes, in posting order */
    unsigned probe_count;
    unsigned probe_next;                /* index of the next code to post */
    unsigned report_id;                 /* CDJ_DSP_REPORT_ID: +0x7cd4, bumped before every event */

    /*
     * CDJ_DSP_TRACE: a copy of the control block as last reported, so each
     * second's report names only the words that changed since the previous one.
     */
    uint8_t *census;
    int64_t census_ns;
    FILE *stream_dump;                  /* CDJ_DSP_STREAM_DUMP */
    FILE *transport_log;                /* CDJ_DSP_TRANSPORT_LOG */
    int64_t transport_last_ms;
    unsigned transport_last_state;
    uint64_t commands;
    uint64_t acknowledged;
    bool trace;
};

/*
 * CDJ_DSP_ACK=1 -- acknowledge the commands MAIN writes into the shared control
 * block.  Off by default; an experiment with its own switch, because the
 * built-in model declines everything it does not understand.
 *
 * What is measured (runs/nxs-swap/trackload-20-dsppoll, 2026-09-03): a track
 * load never rings the mailbox doorbell at all.  It writes 1 into the control
 * block at window+0x7ba0 and MAIN's DSP state machine (0x19feb0) then polls
 * that word and its neighbours with `cmp/pl`: a positive value is a request
 * still pending, zero is done, negative is an error, and the word at +4 is
 * read with `cmp/pz` as the result.  The same machine writes 5 and other codes
 * into the same word (0x1a0a86..0x1a0a90) and polls window+0x7b80 the same way.
 * Nothing answered, so the load stayed pending for ever behind MAIN's
 * NOW LOADING blink.
 *
 * With this on, every 10 ms tick turns a request word (the table below) into
 * 0 and, for the command words, writes 0 into the result word at +4, which is
 * the "done, no error" reading of the poller (the firmware record leaves a large positive
 * constant there, which the poller reads as "still pending" -- trackload-23
 * completed the load's first command that way and MAIN then stopped the
 * player with E-8302 qualifier 0x200f).  A command is a small positive code: 1, 5 and 6 are
 * the ones MAIN's machine writes (0x1a0a86..0x1a0a90).  The firmware record
 * MAIN downloads fills the same words with large constants (0x01c1e02b,
 * 0x2fc37011 -- trackload-22 cleared those at t=1.3 s before the rule was
 * narrowed), so anything at or above DSP_ACK_COMMAND_LIMIT is left alone.
 * Each acknowledgement is reported on stderr, so a run says which commands
 * MAIN issued and in what order -- that is the vocabulary the rest of this
 * file is waiting for.  Nothing here produces audio.
 */
#define DSP_ACK_COMMAND_LIMIT 0x100

typedef struct {
    unsigned offset;        /* window offset of the request word */
    int32_t limit;          /* values at or above this are not requests */
    bool clear_result;      /* the word at +4 is this request's result */
} DspAckWord;

/*
 * The request words measured so far.  Every one follows the same rule --
 * MAIN writes a positive value, polls the word with cmp/pl, and moves on once
 * the DSP has made it zero (negative would be the DSP's error code):
 *
 *   +0x7ba0  the player's DSP command word (0x1a0a86..0x1a0a90 write 1, 5, 6;
 *            the handshake helper 0x19fec0/0x19fef0 reads +0x7ba0 and the
 *            result at +0x7ba4), +0x7b80 the same helper's third channel
 *            (0x19ff2e);
 *   +0x7c9c  the load's parameter block: 0x1a0a3a..0x1a0a62 fills
 *            +0x7ca0..+0x7cac from the track record, writes a size-like
 *            positive value into +0x7c9c and 1 into +0x7ba0, and 0x1a037a..
 *            0x1a038e then polls +0x7c9c (trackload-27: 299 polls in 1.2 s,
 *            nothing answered, load reported failed 8.7 s later);
 *   +0x7cb0  the DJcont mid-manager's command word: 0x1b9d40..0x1b9d72 writes
 *            parameters into +0x7cb8..+0x7cc4, a code into +0x7cb0 (2 there,
 *            1 from the other writers), then polls it 3001 times 2 ms and
 *            gives up with -10 (trackload-26/27 reached the supervisor's
 *            "error STOP" branch with exactly that -10 in hand);
 *   +0x8100  the stream format word: 0x1aed5c first waits (2 ms polls, no
 *            limit) for +0x8100 to read 0, fills +0x8104..+0x8124 from the
 *            track's format record (a WAV: +0x8108 = 2, +0x810c = 2,
 *            +0x8110 = 0x2c, +0x811c = 0x39e2, +0x8120/+0x8124 = 1) and
 *            writes the kind, 2 for PCM (3 and 4 for the compressed kinds),
 *            into +0x8100 (trackload-29: written at t=165 and never
 *            answered, NOW LOADING for the remaining 165 s of the run);
 *   +0x8140  the stream header: 0x1a3962..0x1a3990 writes the stream's
 *            parameters to +0x8144..+0x814c and 0x04010000 or 0x04020000
 *            into +0x8140, hands the stream to tsk_DJcontTxDspPCM and waits
 *            (0x1a39f2.., +0x816c raised meanwhile; 0x1a4a90 waits 2501 x
 *            2 ms for the same word and returns -4) until the DSP has made it
 *            zero; a negative value there is the DSP's error code, which the
 *            PCM sender maps to E8302/E8304 (0x1c1ee2..0x1c1f1a);
 *   +0x81c4  the PCM buffer word: tsk_DJcontTxDspPCM (0x1c189a..0x1c1a72)
 *            fills +0x81e0 or +0xbea0, writes the buffer number to +0x81c8 and
 *            the sample count to +0x81cc, then 1 into +0x81c4 -- 2 for the
 *            last buffer -- and cmd_tx (0x1c1c0c) waits for it to read 0,
 *            1001 times 2 ms, else -5 ("DJcont DSP timeout"); a negative
 *            +0x8140 meanwhile is the DSP's error code.
 *
 * The limit keeps a leftover firmware byte pattern from being taken for a
 * request (trackload-22); the load's size word is the one request that is not
 * a small code, so it has none.  Only the two command words own a result word
 * at +4 -- +0x7ca0 is the parameter block and +0x81c8 the buffer number, and
 * a DSP does not erase its caller's parameters.
 */
static const DspAckWord dsp_ack_words[] = {
    { 0x7b80, DSP_ACK_COMMAND_LIMIT, true },
    { 0x7ba0, DSP_ACK_COMMAND_LIMIT, true },
    { 0x7c80, DSP_ACK_COMMAND_LIMIT, false },   /* trackload-100: 0x11 after a CUE, else E-8302 000F */
    { 0x7c9c, INT32_MAX, false },
    { 0x7cb0, DSP_ACK_COMMAND_LIMIT, false },
    { 0x8100, DSP_ACK_COMMAND_LIMIT, false },
    { 0x8140, INT32_MAX, false },
    { 0x81c4, DSP_ACK_COMMAND_LIMIT, false },
};

/*
 * The loop table of the 4.33 DSP (CDJ_DSP_LOOP_SELECT).
 *
 * - Command 9 on segment slot n + 1 caches loop n's END (0x80044054 ->
 *   0x8002f364 -> 0x8002f830: entry 0x10024f98 + 16 n = {1, half frames,
 *   samples, +0x7cac}); command 12 clears that entry's flag (0x8002feb0) and
 *   command 13 all five.
 * - Command 1 writes slot n's own record, the IN (for n = 0 the cue slot),
 *   and drops loop n's OUT: slot 5 + n's point = -1 and entry 0x10024fe8 +
 *   16 n = 0 (0x8002f45c..0x8002f4a4).  Command 2 writes slot 5 + n's record,
 *   the OUT (0x8002f44c).  Recording slot 5 + n where the deck is (0x12)
 *   builds loop n's entry from slot n's point (0x8002d044).
 * - The END the play pass turns back at is the cached one while its flag is
 *   set, slot 5 + n's point otherwise (0x8002baec -> 0x8002bc2c); its start
 *   is slot n's point (0x8002bac8).  A loop is defined when either exists
 *   (0x8002eecc).
 * - Which loop plays is MAIN's word +0x7bc8, read every command pass
 *   (0x80043c68): n + 1 makes slot n the active loop slot 0x100255e4 if loop
 *   n is defined, anything else -1 (0x80043ca4..0x80043cd8).  The play pass
 *   loops only on the active slot (0x80017960..0x800179e8).  So EXIT and
 *   RELOOP are MAIN taking that word away and giving it back; the jump MAIN
 *   sends with RELOOP (0x21 slot 0) only moves the deck to the IN.
 */
static void cdj_dsp_model_loops_clear(CdjDspModel *model)
{
    for (unsigned n = 0; n < DSP_LOOP_OUT_SLOT; n++) {
        model->seg_end_valid[n] = false;
        model->seg_end_ms[n] = -1;
        model->loop_built[n] = false;
    }
}

static int64_t cdj_dsp_model_loop_in(CdjDspModel *model, unsigned n)
{
    return n == 0 ? model->cue_ms : model->hot_ms[n];
}

static void cdj_dsp_model_loop_select(CdjDspModel *model, uint8_t *window,
                                      size_t length, int64_t now)
{
    uint32_t word;
    bool on = false;
    int64_t in = -1, out = -1;

    if (!model->loop_select || !window || length < 0x7bcc) {
        return;
    }
    word = ldl_le_p(window + 0x7bc8);
    if (word >= 1 && word <= DSP_LOOP_OUT_SLOT) {
        unsigned n = word - 1;

        in = cdj_dsp_model_loop_in(model, n);
        out = model->seg_end_valid[n] ? model->seg_end_ms[n]
                                      : model->hot_ms[DSP_LOOP_OUT_SLOT + n];
        on = (model->seg_end_valid[n] || model->loop_built[n])
            && in >= 0 && out > in;
    }
    if (model->rate_log
        && (!model->loop_select_seen || word != model->loop_select_word
            || on != model->loop_on
            || (on && (in != model->loop_in_ms || out != model->loop_out_ms)))) {
        fprintf(stderr, "cdj2000-dsp: loop select +0x7bc8 = %u -> %s", word,
                on ? "looping" : "no loop");
        if (on) {
            fprintf(stderr, " %" PRId64 "..%" PRId64 " ms", in, out);
        }
        fprintf(stderr, " at %" PRId64 " ms t=%.3f\n", model->pos_ms, now / 1e9);
    }
    model->loop_select_seen = true;
    model->loop_select_word = word;
    model->loop_on = on;
    if (on) {
        model->loop_in_ms = in;
        model->loop_out_ms = out;
    }
}
#define DSP_ACK_COMMAND_WORDS ARRAY_SIZE(dsp_ack_words)

CdjDspModel *cdj_dsp_model_new(Chardev *external)
{
    CdjDspModel *model = g_new0(CdjDspModel, 1);

    model->trace = getenv("CDJ_DSP_TRACE") != NULL;
    /*
     * CDJ_DSP_STREAM_DUMP=<file>: every stream transfer into the window after
     * the DSP is up, as "CDJS", the guest time in ns (u64), the window offset,
     * the byte count, the last format command's record id, +0x81c8 and
     * +0x81cc (u32 each, little endian), then the bytes -- what the DSP would
     * have been given to play, for working out its framing against the file.
     */
    if (getenv("CDJ_DSP_STREAM_DUMP") && *getenv("CDJ_DSP_STREAM_DUMP")) {
        model->stream_dump = fopen(getenv("CDJ_DSP_STREAM_DUMP"), "wb");
    }
    /*
     * CDJ_DSP_TRANSPORT_LOG=<file>: the deck's transport as the position model
     * keeps it -- one line per 10 ms tick while it plays and one per change
     * otherwise: guest ns, position ms, state (2 standing, 3 playing), the
     * rate word (2^20 = 1.0) and the record played.  With the stream dump it
     * is what tools/cdj_main/deck_audio.py renders to sound.
     */
    if (getenv("CDJ_DSP_TRANSPORT_LOG") && *getenv("CDJ_DSP_TRANSPORT_LOG")) {
        model->transport_log = fopen(getenv("CDJ_DSP_TRANSPORT_LOG"), "w");
        model->transport_last_ms = -1;
    }
    /*
     * CDJ_DSP_ABSENT keeps the window and the DMA but never answers, which is
     * the machine as it was before this device existed.  It is the control for
     * every claim made about the DSP: "the banner is gone" only means something
     * against a run in which it is still there.
     */
    model->absent = getenv("CDJ_DSP_ABSENT") != NULL;
    model->ack_control = getenv("CDJ_DSP_ACK") != NULL;
    model->slot_report = getenv("CDJ_DSP_SLOT_REPORT") != NULL;
    model->slot_loaded_state = model->slot_report
        ? (unsigned)strtoul(getenv("CDJ_DSP_SLOT_REPORT"), NULL, 0) : 0;
    if (model->slot_report && (model->slot_loaded_state == 0
                               || model->slot_loaded_state == 3)) {
        model->slot_loaded_state = 2;
    }
    model->slot_event = getenv("CDJ_DSP_SLOT_EVENT")
        ? (unsigned)strtoul(getenv("CDJ_DSP_SLOT_EVENT"), NULL, 0) : 0x100;
    model->play_event = getenv("CDJ_DSP_PLAY_EVENT")
        ? (unsigned)strtoul(getenv("CDJ_DSP_PLAY_EVENT"), NULL, 0) : 0;
    /*
     * CDJ_DSP_CONSUME=<units per second>: playback as the stream worker sees
     * it.  ([0x4832214] was taken for the deck's position word here; it is
     * the track LENGTH, and the position comes from the report block --
     * see CDJ_DSP_POSITION below, trackload-84..88.)  The stream worker's
     * function 0x1a8e60.. (writers 0x1a8f00 and 0x1a979c) sets that word
     * from -1 only, and trackload-65/66 measured that with nothing consumed
     * it never runs -- the worker waits for the fill levels to drop.  A
     * data transfer books 40 units
     * for 9408 bytes of 16-bit stereo PCM (2352 frames, 53.3 ms), so real
     * time is 750 units a second.  While the slot state is 3 the model takes
     * that many out of both levels and adds them to +0x81a0/+0x8180, the
     * per-buffer status words the worker reads next to the levels.
     */
    model->consume_rate = getenv("CDJ_DSP_CONSUME")
        ? strtol(getenv("CDJ_DSP_CONSUME"), NULL, 0) : 0;
    /*
     * trackload-69/70: every class-0 event code (1, 2, 3, 5, 6, 7, 10) while
     * playing got the same answer from MAIN -- +0x7cb0 = 2 with the record of
     * slot <param>, then a 0x03000100 data header for buffer 1 whose offset
     * word followed the model's +0x81a0 count exactly (0x1daf ten seconds
     * after PLAY at 750 a second, 0x4227 at twenty-two), then +0x7ba0 = 2, 1.
     * So the DSP's per-buffer status word IS the play position MAIN streams
     * from, and a class-0 event is "buffer <param> wants data".  Both runs
     * ended in an error stop (E-8302 C611) -- with buffer 1 already at level
     * 0, an underrun.  Hence: consume from buffer 1 only, and ask before it
     * is empty.
     */
    /*
     * The stream worker's load-state handler 0x1aaf28 answers the player
     * with four bytes built from the two fill levels and two flag bits it
     * reads next to them: byte 3 of +0x818c bit 1 (buffer 2) and byte 3 of
     * +0x81ac bit 0 (buffer 1).  Nothing in the model ever set them.  With
     * CDJ_DSP_STATUS_FLAGS the bits are up while the slot state is 3.
     */
    model->status_flags = getenv("CDJ_DSP_STATUS_FLAGS") != NULL;
    /*
     * +0x7cb0 = 1 or 2 (the DJcont mid-manager's command) is a flush in the
     * DSP program, not an acknowledgement alone: the main loop (0x80040fd8)
     * copies the word, runs 0x800429e0 and only then writes the 0 back.
     * Both codes clear the 10-entry record table (0x8002ad84 sets the current
     * record bytes b14+896..898 to 0xff, 0x8002a2a0 wipes 0x10024e50) -- code
     * 1 also re-initialises the rest (0x8002abc4) -- and 0x80034c08 then
     * zeroes the position report +0x7c10..+0x7c2c, both status blocks
     * +0x8180..+0x81bf, the fill levels +0x7ccc/+0x7cd0 and +0x81c4, and
     * reports the current record, now 0xff, in +0x7cd4, which MAIN reads
     * right after its poll (0x041b85f4).  Without it a second LOAD found the
     * old track's levels still up, took the DSP for loaded and never sent
     * the new stream.  CDJ_DSP_FLUSH=0 leaves the word acknowledged only.
     */
    model->flush_reset = g_strcmp0(getenv("CDJ_DSP_FLUSH"), "0") != 0;
    /*
     * +0x7ba4 is not only the result of a +0x7ba0 command.  MAIN's DSP task
     * hands the DSP a job through it (0x041a0aa8..0x041a0aba): +0x7ba8 = the
     * count, +0x7bac = a parameter, +0x7ba4 = 1, and from then on reads
     * +0x7ba4 back (0x0419fef2): positive is "still working", 0 is done
     * (0x0419ff06: finished a few passes later), negative is a failure
     * code.  The DSP program writes its answer there (0x80035870 writes 0,
     * 0x80035898 0 or -1, 0x800358bc another result).  On a jump to a cue
     * the player gives that job right after the stream open -- the count is
     * the buffer-1 level in half frames, 0x041b6ad2 -- and waits 2001 x 3
     * ticks for it (0x041b6b60) before it sends the next state; unanswered it
     * ended in E-8302 (000F) (reason -13, 0x041b6b7e).  A load never showed
     * it only because the following +0x7ba0 acknowledgement cleared the word.
     * The model has no decoder to run the job, so it answers it done at once,
     * as it acknowledges every other command.  CDJ_DSP_JOB=0 leaves it.
     */
    model->job_answer = g_strcmp0(getenv("CDJ_DSP_JOB"), "0") != 0;
    /*
     * CDJ_DSP_JOB_CONSUME=1 -- an experiment, not a rule read in the DSP
     * code yet: the player computes the job's count as buffer 1's level
     * (+0x7cd0) in half frames (0x041b6ad2: level * 2, one less past half a
     * frame in +0x7bf4) and posts the same job again for as long as that
     * level stays up (cue-7: 2357 jobs of 68, then 204).  If the job is
     * "take that much out of buffer 1", the level has to drop by it.
     */
    model->job_consume = getenv("CDJ_DSP_JOB_CONSUME") != NULL;
    /*
     * CDJ_DSP_JOB_ADVANCE (on unless =0; implies _CONSUME).  The
     * job comes from MAIN's seek routine 0x041b699a, which compares the
     * DSP-reported position (X+0x218, half frames: +0x7c10 * 2 + half) with
     * its target and posts jobs of min(3/4 of the distance, level * 2)
     * (0x041b6b22..0x041b6b4e) until the two match -- and only then goes on
     * to the next state (cue-8 and NXS r59 stand on the cue without it).  So
     * the job moves the play position forward by its count, in half frames
     * of 1/150 s.  A job with +0x7bac other than 0 is logged and not moved:
     * no run has shown one yet.
     */
    model->job_advance = g_strcmp0(getenv("CDJ_DSP_JOB_ADVANCE"), "0") != 0;
    /*
     * CDJ_DSP_EVENTS (on unless =0) -- built on two places in the DSP
     * program that post event 5 (the byte pair at +0xffea/+0xffeb = 5, 0,
     * then the host interrupt, 0x800358e0): right after a record is added
     * to the record table (0x8003079c, after b14+801 counts it) and when the
     * level behind the position has moved by more than 20 since the last
     * report (0x800304d0..0x80030578, then 0x80045010 rewrites both
     * levels).  MAIN's stream worker reads the levels again on it; without
     * it a jump to a cue used up the level ahead with its first job and then
     * waited for data nobody asked for.  Events 1 and 2 (a hot-cue slot
     * whose counter passed 40, 0x8002eb14 / 0x8002ea7c) are not posted yet.
     */
    model->events = g_strcmp0(getenv("CDJ_DSP_EVENTS"), "0") != 0;
    /*
     * CDJ_DSP_STATUS_RECORD (on unless =0).  Each buffer has a 32-byte
     * status block the DSP copies from its channel structure (+0x81a0 for
     * buffer 1, 0x80034f38; +0x8180 for buffer 2, 0x80034f68).  MAIN's
     * stream worker reads the low byte of its second word (+0x81a4 /
     * +0x8184) before and after it opens a stream (0x041ad1f2, 0x041ad374)
     * and takes it as the record the DSP now holds there.  With it left 0,
     * a second LOAD re-opened buffer 2 with the same start 37 times in 1.5 s
     * and never went on to buffer 1 (load2-6/7).  The model puts the record
     * of each stream open there.
     */
    model->status_record = g_strcmp0(getenv("CDJ_DSP_STATUS_RECORD"), "0") != 0;
    /*
     * CDJ_DSP_STATUS_CURRENT (on unless =0): which record those bytes name.
     * The DSP fills both blocks from its CURRENT record, b14+896, at every
     * place it publishes them -- the class-1 stream handler (0x80041104..
     * 0x80041188), the record registration (0x80041b30..0x80041b90), the
     * record switch (0x8002ec90..0x8002ed28) and 0x80030638..0x800306a0:
     * buffer 1 from the channel in its descriptor's +4 (0x80034494), buffer 2
     * from +0 (0x80034454), each through 0x8002b67c and 0x80034f38/0x80034f68.
     * It never names the record of a buffer-2 stream it was just given.
     * MAIN's DSP-task check at 0x041acdf6..0x041ace56 takes a change of
     * +0x8184 as the deck now playing another record and re-reads the track
     * for it (0x041b2aa2), which clears the track's beat grid (track info +
     * 0x104, written 0 at 0x041b2b10): with the tail of the previous track
     * named there at a load, MAIN never found a beat (0x0419f414: grid 0),
     * sent no beat packets and reported BPM 0xffff (beat-1/2).  Off, each
     * open names its own record in its own buffer's block, as before.
     */
    model->status_current = g_strcmp0(getenv("CDJ_DSP_STATUS_CURRENT"), "0") != 0;
    /*
     * CDJ_DSP_STATUS_FIRST (on unless =0): the current record right after a
     * flush.  Flush 2 (0x8002ad84) leaves current and next at 0xff; a record
     * registered while they are equal becomes next (0x8002b2e4) and then
     * current at the record switch (0x8002ec90), which publishes it in both
     * blocks (0x8002ecec, 0x8002ed28).  MAIN's quantized hot cue call
     * re-streams as 1, flush 2, +0x8100 = 3 (record 1), 4, then a buffer-2
     * open of record 1 before any buffer-1 open.  With the blocks left at 0
     * (nothing played yet) MAIN's open routine saw no change of +0x81a0 /
     * +0x81a4 across the open (0x041ad252 before, 0x041ad374 after,
     * 0x041ad386 / 0x041ad3cc) and opened buffer 2 again, about 20 times a
     * second (bt-6, h1).  Until a buffer-1 open names the record to play, the
     * blocks name the first record +0x8100 = 3 registered after the flush.
     */
    model->status_first = g_strcmp0(getenv("CDJ_DSP_STATUS_FIRST"), "0") != 0;
    /*
     * The slot messages (see cdj_dsp_model_slot_msg).  CDJ_DSP_SLOT_MSG is off
     * unless set: with it on, MAIN stopped talking to the GUI (E-8709) at the
     * REC MODE + B press after a PLAY that jumped to the cue (s24-a/b/c; off:
     * s24-d records B, jumps, CUE and PLAY work).  The message's form or
     * moment is not the DSP's yet.  CDJ_DSP_SLOT_READY_EVENT (event 1 with
     * the slot) is off unless set, until a run shows MAIN wants it.
     */
    model->slot_msg = getenv("CDJ_DSP_SLOT_MSG")
        && g_strcmp0(getenv("CDJ_DSP_SLOT_MSG"), "0") != 0;
    model->slot_ready_event = getenv("CDJ_DSP_SLOT_READY_EVENT")
        && g_strcmp0(getenv("CDJ_DSP_SLOT_READY_EVENT"), "0") != 0;
    /*
     * The state requests as the DSP program runs them (see the +0x7ba0
     * handler): on unless CDJ_DSP_STATES=0, which brings back the old
     * reading (3 runs, everything else stands).  CDJ_DSP_LOOP_SLOT0 (on
     * unless 0) makes the cue slot's record the loop's IN.
     * CDJ_DSP_SLOT_ENTRY (on unless 0) writes the slot entry MAIN copies
     * after a slot operation: +32 (CD frames) and +96 (samples) are what
     * MAIN saves for a recorded hot cue (s25-v1: B recorded at 13000 ms was
     * saved as 1950 half frames); the rest of the layout is only partly known.
     */
    model->states433 = g_strcmp0(getenv("CDJ_DSP_STATES"), "0") != 0;
    model->loop_in_slot0 = g_strcmp0(getenv("CDJ_DSP_LOOP_SLOT0"), "0") != 0;
    /* CDJ_DSP_LOOP_SLOTS (on unless 0): see the +0x7c80 slots 5..9 below. */
    model->loop_slots = g_strcmp0(getenv("CDJ_DSP_LOOP_SLOTS"), "0") != 0;
    /*
     * CDJ_DSP_REVERSE (on unless 0): +0x7bc4 bit 31 is the direction.  MAIN's
     * DSP task copies it from deck byte X+0x690, the DIRECTION lever (see
     * below); it was set while playing with the lever at REV and during a
     * backward scratch, and clear forwards and when paused (panel/lamps-
     * scratch lm-6, lm-7).  The DSP's command pass keeps it as get(20)
     * (0x80043a08), and its frame pass takes a separate path on it
     * (0x800239b4 .. 0x800239cc -> 0x80023a74) and marks a change of
     * direction (0x80015054 .. 0x8001507c).  So the position runs backwards
     * at the same rate, down to the start of the track.  lm-8: with the lever
     * at REV from t=39.08 the position went from 11.98 s to 8.95 s at t=42.10,
     * and forwards again after FWD; a backward scratch (0.63 s at 1.11) put it
     * at 13.76 s at t=52.10, 0.70 s back, where forwards would give 15.16 s.
     */
    model->reverse = g_strcmp0(getenv("CDJ_DSP_REVERSE"), "0") != 0;
    /* The loop MAIN selects in +0x7bc8 (cdj_dsp_model_loop_select): on unless 0. */
    model->loop_select = g_strcmp0(getenv("CDJ_DSP_LOOP_SELECT"), "0") != 0;
    /* The slot copies +0x7c80 = 0x31 / 0x33 (see there): on unless 0. */
    model->slot_copy = g_strcmp0(getenv("CDJ_DSP_SLOT_COPY"), "0") != 0;
    model->slot_entry = g_strcmp0(getenv("CDJ_DSP_SLOT_ENTRY"), "0") != 0;
    /* The AUTO CUE search's answer (see the +0x7ba0 handler): on unless 0. */
    model->auto_cue = g_strcmp0(getenv("CDJ_DSP_AUTO_CUE"), "0") != 0;
    if (model->job_advance) {
        model->job_consume = true;
    }
    model->refill_event = getenv("CDJ_DSP_REFILL_EVENT")
        ? (unsigned)strtoul(getenv("CDJ_DSP_REFILL_EVENT"), NULL, 0) : 0;
    model->refill_low = getenv("CDJ_DSP_REFILL_LOW")
        ? (int32_t)strtol(getenv("CDJ_DSP_REFILL_LOW"), NULL, 0) : 20;
    /*
     * CDJ_DSP_POSITION=1: keep the position report block that MAIN's reader
     * 0x19e568 takes every tick (trackload-84: it runs from boot, X+420 set,
     * and after the load reaches its normal path 0x19eca0 every tick, turning
     * +0x7c10 into the deck's elapsed time X+0x224..0x228 and X+0x218 --
     * with the block zeroed those stayed 0 for 40558 ticks).  The block:
     * +0x7bf0 status (0 = valid), +0x7bf4 low 16 bits = sample offset inside
     * the current CD frame (0..587, /294 = half frame), +0x7c10 = position in
     * CD frames (75/s), +0x7c14 = the id of the load-queue record being
     * played (MAIN looks the record up by it, trackload-85/86; byte 2 of
     * that word is the needle path's "valid" test).  (The player's
     * 0x1b323e reads +0x7ccc, buffer 2's level, the same way -- word * 2 +
     * (sub >= 294) -- not this block.)  The position runs while +0x7ba0 last
     * said 3 (PLAY), stands after 5, and starts at 0 with the load's closing 4.
     */
    model->pos_report = getenv("CDJ_DSP_POSITION") != NULL;
    /*
     * CDJ_DSP_RATE_LOG: a line each time the playback rate +0x7bc0 changes
     * (see cdj_dsp_model_rate_log), and each time MAIN's loop selection
     * +0x7bc8 does.  On by default where the position model runs
     * (CDJ_DSP_POSITION) or with CDJ_DSP_TRACE; =1 / =0 force it.  Off, a
     * plain boot's stderr stays as it was: during init the window still
     * holds whatever was loaded there, which these lines would print.
     */
    if (getenv("CDJ_DSP_RATE_LOG")) {
        model->rate_log = g_strcmp0(getenv("CDJ_DSP_RATE_LOG"), "0") != 0;
    } else {
        model->rate_log = model->pos_report || model->trace;
    }
    for (unsigned i = 0; i < DSP_HOT_SLOTS; i++) {
        model->hot_ms[i] = -1;
    }
    cdj_dsp_model_loops_clear(model);
    model->slot_period_ns = (int64_t)(getenv("CDJ_DSP_SLOT_PERIOD_MS")
        ? strtol(getenv("CDJ_DSP_SLOT_PERIOD_MS"), NULL, 0) : 500) * 1000000;
    /*
     * CDJ_DSP_EVENT_PROBE=<start s>[:<interval s>[:<first>-<last>]] -- from
     * <start> seconds of guest time on, post the event codes <first>..<last>
     * (default 1..13, DspTASK's table has 13 entries) one every <interval>
     * seconds (default 4) and leave it to the console and the census to say
     * what MAIN made of each.  The codes' meaning is not known; this is how
     * it gets measured.  See cdj_dsp_event in cdj2000_dsp.c for the line.
     *
     * trackload-50-eventprobe (185:4, after a load): every code acknowledged
     * within a millisecond; the player task reported an error stop five
     * times ("ｴﾗｰ停止通知をﾌﾞﾟﾚｰﾔｰﾀｽｸから受理した", GUI: E-8302 CANNOT PLAY
     * TRACK (C611), waveform cleared) and codes 5, 6, 7 and 10 were each
     * followed by the player commands 2 then 1 in +0x7ba0.  A code without
     * its parameters is an error to MAIN; which one carries the position is
     * the next measurement.
     */
    /*
     * CDJ_DSP_REPORT_ID=<n>: the word at +0x7cd4 is an ID the DSP reports and
     * MAIN only reads (the census never saw MAIN write it).  The player task's
     * event dispatcher (0x1b36c4, 0x1b3700, 0x1b3790) drops a class 1, 2 or 5
     * event when its copy at [0x4835ac8+60] equals that word, and the class
     * handler 0x1bd304 captures the word and compares again after the worker
     * task has answered (0x1bd43a).  trackload-55: with the word 0 every
     * class 1/2/5 event was dropped (handlers never hit), class 3 ran and
     * timed out after 6 s.  trackload-56, the word written 1 before the first
     * event: that event ran the handler through to the worker task's answer
     * and the update path 0x1bd440 -- MAIN then reported a track change to
     * none ("曲変化(TrNo=-1)", the table being empty) and the status record's
     * time fields went from blank to 00:00 -- and every later event was
     * dropped again, MAIN having copied the 1.  So the word is a report
     * sequence number: with this switch the model writes <n>+1, <n>+2, ...
     * there before each event it posts.
     */
    if (getenv("CDJ_DSP_REPORT_ID")) {
        model->report_id = (unsigned)strtoul(getenv("CDJ_DSP_REPORT_ID"), NULL, 0);
    }
    {
        const char *probe = getenv("CDJ_DSP_EVENT_PROBE");

        if (probe && *probe) {
            double start = 0, interval = 4;
            unsigned first = 1, last = 13, i;
            const char *codes = NULL;
            char *end = NULL;

            /*
             * <start>[:<interval>[:<codes>]] -- <codes> is either a range
             * <first>-<last> or a comma-separated list, each entry in C
             * notation (0x100 is class 1 parameter 0: byte 2 of the event word
             * is the class the player task switches on at 0x1b36a4, byte 3
             * the parameter it passes on).  trackload-54 was meant to post
             * such a list and posted 1..13 again because this parser did not
             * exist; the run is marked invalid in its README.
             */
            start = strtod(probe, &end);
            if (end && *end == ':') {
                interval = strtod(end + 1, &end);
            }
            if (end && *end == ':') {
                codes = end + 1;
            }
            if (interval <= 0) {
                interval = 4;
            }
            model->probe_start_ns = (int64_t)(start * 1e9);
            model->probe_interval_ns = (int64_t)(interval * 1e9);
            model->probe_count = 0;
            if (codes && strchr(codes, ',')) {
                while (*codes && model->probe_count < ARRAY_SIZE(model->probe_list)) {
                    model->probe_list[model->probe_count++] =
                        (unsigned)strtoul(codes, &end, 0);
                    if (end == codes) {
                        break;
                    }
                    codes = (*end == ',') ? end + 1 : end;
                }
            } else {
                if (codes) {
                    sscanf(codes, "%u-%u", &first, &last);
                }
                for (i = first; i <= last
                     && model->probe_count < ARRAY_SIZE(model->probe_list); i++) {
                    model->probe_list[model->probe_count++] = i;
                }
            }
            fprintf(stderr, "cdj2000-dsp: event probe: %u codes from t=%.1f "
                    "every %.1f s:", model->probe_count, start, interval);
            for (i = 0; i < model->probe_count; i++) {
                fprintf(stderr, " 0x%x", model->probe_list[i]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (external) {
        qemu_chr_fe_init(&model->external, external, &error_abort);
        model->have_external = true;
    }
    return model;
}

void cdj_dsp_model_reset(CdjDspModel *model, uint8_t *window, size_t length)
{
    model->transport = CDJ_DSP_STOPPED;
    model->position_ms = 0;
    model->tempo_ppm = 0;
    model->running = false;
    model->control_cleared = false;
    model->last_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* a reset DSP reloads its program, and its slot records with it */
    for (unsigned i = 0; i < DSP_HOT_SLOTS; i++) {
        model->hot_ms[i] = -1;
    }
    cdj_dsp_model_loops_clear(model);
    if (window) {
        /* A DSP being reset is not running, and must not claim to be. */
        stl_le_p(window + CDJ_DSP_MAIL_UP, 0);
        stl_le_p(window + CDJ_DSP_MAIL_ACK, 0);
    }
    if (model->trace) {
        fprintf(stderr, "cdj2000-dsp: model reset after %" PRIu64
                " bytes of firmware in %u pages\n",
                model->firmware_bytes, model->firmware_records);
    }
}

void cdj_dsp_model_firmware(CdjDspModel *model, uint8_t *window,
                            size_t length, uint32_t offset, unsigned bytes)
{
    if (model->control_cleared && model->stream_dump && offset + bytes <= length) {
        uint8_t head[4 + 8 + 5 * 4] = { 'C', 'D', 'J', 'S' };

        stq_le_p(head + 4, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        stl_le_p(head + 12, offset);
        stl_le_p(head + 16, bytes);
        stl_le_p(head + 20, model->fmt_record);
        stl_le_p(head + 24, ldl_le_p(window + 0x81c8));
        stl_le_p(head + 28, ldl_le_p(window + 0x81cc));
        fwrite(head, 1, sizeof(head), model->stream_dump);
        fwrite(window + offset, 1, bytes, model->stream_dump);
        fflush(model->stream_dump);
    }
    if (model->control_cleared) {
        /*
         * Once MAIN has seen the DSP up, a transfer into the window is stream
         * data -- tsk_DJcontTxDspPCM's 9408-byte buffers on DMAC channel 5 --
         * not another firmware page.
         *
         * MAIN does not write a header for every one of them: trackload-36
         * had 3704 data transfers and 3515 headers, trackload-39 1654 and 72,
         * and in the latter the stream crawled -- MAIN paces itself by the
         * level the DSP reports against what it has sent, so a level kept by
         * headers alone falls behind and stalls it (pairing headers with
         * DMAs double-booked instead, trackload-41: 6837 for 3704).  A DSP
         * counts what it is given: every data transfer is booked here, 40
         * units per 9408 bytes, to the buffer the last header named.
         */
        if (model->trace) {
            fprintf(stderr, "cdj2000-dsp: %u bytes into window+0x%04x "
                    "(stream data)\n", bytes, offset);
        }
        if (model->ack_control && bytes >= 4096 && length > 0x7cd4) {
            unsigned level = model->stream_buffer == 1 ? DSP_LEVEL_BUFFER1
                                                       : DSP_LEVEL_BUFFER2;
            int32_t units = (int32_t)((uint64_t)bytes * 40 / 9408);
            int32_t was = (int32_t)ldl_le_p(window + level);

            stl_le_p(window + level, was + units);
            if (model->trace) {
                fprintf(stderr, "cdj2000-dsp: buffer %u took %d -> level %d "
                        "(+0x%04x)\n", model->stream_buffer, units,
                        was + units, level);
            }
        }
        return;
    }
    model->firmware_records++;
    model->firmware_bytes += bytes;
    model->last_offset = offset;
    if (model->trace) {
        /*
         * The first eight bytes identify which record this is far better than a
         * count does: record 0's payload starts 2a 66 b2 07, and the shared
         * control block at 0x7800 does not.
         */
        const uint8_t *at = window + (offset < length ? offset : 0);

        fprintf(stderr, "cdj2000-dsp: firmware page %u at window+0x%04x "
                "%u bytes  %02x %02x %02x %02x\n",
                model->firmware_records, offset, bytes,
                at[0], at[1], at[2], at[3]);
    }

    /*
     * A real DSP boots as soon as it has code and reports that in the mailbox;
     * bring-up state 5 (0x1c7924 -> the poller 0x1c778a) waits for exactly
     * this word and gives up after three attempts of 3000 polls each, which is
     * the 9000 reads a run without it produces.
     *
     * Raising it on the first page rather than the last is deliberate: nothing
     * reads the word before state 5, and there is no field anywhere in the
     * transfer that says which page is the last one — inferring it from a short
     * page would be a guess, and a wrong guess here looks exactly like a hang.
     */
    if (!model->running && !model->absent) {
        model->running = true;
        stl_le_p(window + CDJ_DSP_MAIL_UP, 1);
        if (model->trace) {
            fprintf(stderr, "cdj2000-dsp: reporting running at window+0x%04x\n",
                    CDJ_DSP_MAIL_UP);
        }
    }
}

/*
 * The control block after boot.  Pages 2..11 of the firmware are staged
 * through window+0x7800 (trackload-26 stderr: every one lands there, the last
 * is 24384 bytes), so once the download is over the block holds page 11's
 * bytes: +0x7b80 = 0x01c1e02b, +0x7ba0 = 0x2fc37011, +0x8140 = 0x020000fa,
 * +0x81c4 = 0x031402e6.  MAIN reads those as a command still pending (+0x7ba0,
 * cmp/pl) and as the PCM channel still busy (+0x81c4, which cmd_tx 0x1c1c0c
 * waits to see 0 for 1001 x 2 ms before giving up with -5).  A DSP that has
 * booted has initialised its memory; this does the same, once, at the moment
 * MAIN first reads the "up" word -- in every run so far that read follows the
 * last page.  Under CDJ_DSP_ACK, like the acknowledgements, so the default
 * machine is unchanged.
 */
#define DSP_CONTROL_END 0xffe0          /* the mailbox starts here */

void cdj_dsp_model_up_seen(CdjDspModel *model, uint8_t *window, size_t length)
{
    if (!model->ack_control || model->control_cleared || !window
        || length < DSP_CONTROL_END) {
        return;
    }
    memset(window + DSP_CONTROL_OFFSET, 0, DSP_CONTROL_END - DSP_CONTROL_OFFSET);
    model->control_cleared = true;
    fprintf(stderr, "cdj2000-dsp: control block +0x%04x..+0x%04x zeroed after "
            "%u firmware pages, t=%.3f\n", DSP_CONTROL_OFFSET, DSP_CONTROL_END,
            model->firmware_records,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
}

/*
 * MAIN raised the request word.  With a chardev attached the whole window's
 * control block goes out and the answer comes back into it; otherwise the
 * built-in model answers.
 *
 * Returning true tells the device to acknowledge.  Returning false leaves the
 * answer word alone, which is what a real DSP that has not finished would do —
 * and is the honest thing to return while a command is not yet understood,
 * because a false acknowledgement makes MAIN believe a value it never got.
 */
bool cdj_dsp_model_doorbell(CdjDspModel *model, uint8_t *window, size_t length)
{
    uint8_t *control = window + DSP_CONTROL_OFFSET;

    model->commands++;
    if (model->trace) {
        fprintf(stderr, "cdj2000-dsp: doorbell %" PRIu64 "  arg=%08x  "
                "control %02x %02x %02x %02x %02x %02x %02x %02x\n",
                model->commands,
                ldl_le_p(window + CDJ_DSP_MAIL_BASE),
                control[0], control[1], control[2], control[3],
                control[4], control[5], control[6], control[7]);
    }

    if (model->have_external) {
        /*
         * Frame: a 4-byte length, then the control block.  The semantics stay
         * in the window, so the far end needs no protocol of its own beyond
         * knowing where the block starts.
         */
        uint8_t header[4];
        const unsigned block = length - DSP_CONTROL_OFFSET;

        stl_le_p(header, block);
        qemu_chr_fe_write_all(&model->external, header, sizeof(header));
        qemu_chr_fe_write_all(&model->external, control, block);
        /*
         * Deliberately not blocking on a reply here: the caller is inside a
         * guest store and the link has 3 ms deadlines.  An external engine
         * answers into the window and the next doorbell picks it up.
         */
        return true;
    }

    /*
     * The built-in model.  Command decoding is not written yet — see the file
     * comment.  Acknowledging without understanding would be worse than not
     * answering, so this reports and declines until the vocabulary is measured.
     */
    return false;
}

/*
 * CDJ_DSP_TRACE: once a second, name every word of the control block that
 * changed since the last report.  This is how the block's vocabulary gets
 * measured: a breakpoint shows one writer, this shows every write, including
 * the ones made through pointers that no literal pool names.  Words the model
 * zeroes itself show up too (as the write MAIN made, if the tick sees it
 * first, or not at all when acknowledged in between -- the acknowledgement
 * line covers those).  Up to 24 words per report; a bigger burst is counted.
 */
#define DSP_CENSUS_INTERVAL_NS  (1000 * 1000 * 1000)
#define DSP_CENSUS_END          0xffe0
#define DSP_CENSUS_MAX_WORDS    24

static void cdj_dsp_model_census(CdjDspModel *model, uint8_t *window,
                                 size_t length, int64_t now)
{
    unsigned offset, reported = 0, changed = 0;

    if (!model->trace || !window || length < DSP_CENSUS_END) {
        return;
    }
    if (!model->census) {
        model->census = g_malloc0(DSP_CENSUS_END - DSP_CONTROL_OFFSET);
        memcpy(model->census, window + DSP_CONTROL_OFFSET,
               DSP_CENSUS_END - DSP_CONTROL_OFFSET);
        model->census_ns = now;
        return;
    }
    if (now - model->census_ns < DSP_CENSUS_INTERVAL_NS) {
        return;
    }
    model->census_ns = now;
    for (offset = DSP_CONTROL_OFFSET; offset < DSP_CENSUS_END; offset += 4) {
        uint32_t was = ldl_le_p(model->census + offset - DSP_CONTROL_OFFSET);
        uint32_t is = ldl_le_p(window + offset);

        if (was == is) {
            continue;
        }
        if (changed == 0) {
            fprintf(stderr, "cdj2000-dsp: census t=%.1f", now / 1e9);
        }
        changed++;
        if (reported < DSP_CENSUS_MAX_WORDS) {
            fprintf(stderr, " +0x%04x=%08x", offset, is);
            reported++;
        }
        stl_le_p(model->census + offset - DSP_CONTROL_OFFSET, is);
    }
    if (changed) {
        if (changed > reported) {
            fprintf(stderr, " (+%u more)", changed - reported);
        }
        fprintf(stderr, "\n");
    }
}

/*
 * The stream header at +0x8140 and the two fill levels.
 *
 * MAIN keeps two buffers in the DSP and reads their fill levels at +0x7ccc
 * (buffer 2) and +0x7cd0 (buffer 1).  Byte 3 of the header is its class, and
 * where the buffer sits depends on the class.  The DSP program's own header
 * dispatcher (4.33, main-unpacked.bin 0xe3d0.. loaded at DSP 0x80000000; the
 * window is DSP 0x10030000, so +0x8140 is 0x10038140) takes exactly these
 * words, 0x80040e28..0x80040f5c:
 *
 *   class 1  0x01010100 0x01020100 0x01010200 0x01010101 0x01020101
 *            0x01010102 0x01020102       -> 0x80041208
 *   class 2  0x02000100 0x02000200       -> 0x80042570, the drop
 *   class 3  0x03000100 0x03000200       -> 0x80042734
 *   class 4  0x04010000 0x04020000       -> 0x80041e68
 *
 * A drop names its buffer in byte 1: 0x80042570 passes index 0 for
 * 0x02000100 and 1 for 0x02000200 to the drop proper (0x8002afec), and
 * MAIN's drop routine (0x1be0ec..0x1be1be) sends 0x02000100 with
 * min(+0x7cd0, 40) and 0x02000200 with min(+0x7ccc, 40) in +0x8144 -- byte 1
 * = 1 is buffer 1, 2 is buffer 2, and the level's unit is the count's unit.
 * A negative count is 20 to the DSP (0x800425d4).  The DSP writes both levels
 * in one place, 0x80045010: +0x7ccc and +0x7cd0 from the halfwords +0x10 and
 * +0x12 of the current deck's 22-byte record.
 *
 * Class 1 is the stream's data path.  MAIN 4.33 writes 0x01010100 once as
 * the open after a format command, then 0x01020100 with 40 in +0x8144 for
 * every transfer (cosim scenario-8, an MP3: one open per record, 2246 data
 * headers in the first minute); 0x01010200 is a third writer's (0x1a2a9e,
 * 0x1a6626).  The DSP handler tests bit 17 (byte 2 = 2), bit 9 (byte 1 = 2)
 * and the low nibble separately, takes the record id on the open (see
 * cdj_dsp_model_stream_open) and consumes the doorbell buffers that follow
 * (0x80041310, 0x80045fe4).  The model reads byte 2 of a class-1 header as the
 * buffer those transfers fill.  That is what makes the load's pre-fill below
 * come out, not yet something traced to the level halfwords in the DSP code.
 *
 * The load's pre-fill (0x1b6ffe..0x1b7048) sends buffers until +0x7ccc >= 160
 * and +0x7cd0 >= 40 (the other branch, 0x1b6faa.., wants 80 and 160), and only
 * then reports the load done.  trackload-34: with nothing keeping the levels,
 * MAIN streamed the whole file at the acknowledgement rate and NOW LOADING
 * never ended.  A DSP that has taken a transfer adds its count to the level;
 * this does that.  Nothing is played, so nothing is subtracted yet.
 */

/*
 * How much a buffer holds: the whole track.  trackload-36 and -41: with every
 * transfer taken at once MAIN pushed the whole file (3704 transfers of 9408
 * bytes plus 1620 of 8192, 33 MB) and reported the load done at the last one
 * -- and that is the machine: the CDJ-2000 loads a track into the DSP's own
 * 32 MB SDRAM (IC505, K4S561632J, service manual p. 48), which is why the
 * card can be pulled while it plays.  trackload-37 tried a six-transfer
 * capacity instead: the seventh header stayed pending, MAIN waited on it
 * without a timeout (0x1a3a10: 2 ms polls, no limit) and the load never
 * finished.  So a data header is always taken; the levels only tell MAIN how
 * much is buffered.
 *
 * The format word: 2 is the load's PCM format; 3 (parameters 0, 2, 0x480,
 * 0x30) opens the second phase of the same load, in which MAIN sends
 * 8192-byte buffers through the doorbell path (+0x81c4, +0x81c8 alternating,
 * trackload-41 t=298.7 -- before any PLAY press, so not playback, as
 * trackload-36's coincidence with one had suggested).  Nothing is consumed:
 * playback is not modelled, the levels only grow.
 *
 * +0x81c8 is not a level: the DSP's buffer taker 0x80045fe4 reads it as the
 * half of the staging area the data sits in, 0x100381e0 + index * 0x3cc0 --
 * +0x81e0 or +0xbea0, the two buffers the PCM sender fills.  With format 3 or
 * 4 in force the doorbell buffers go to 0x80047280 or 0x80046728 (picked by
 * 0x800463c8 on the kind), which hand them to a stream object; neither calls
 * the level report 0x80045010 directly.  So booking every transfer to the
 * last class-1 buffer, as cdj_dsp_model_firmware does, is right for a class-1
 * stream and unverified for a format-3 one.
 */

/*
 * CDJ_DSP_SLOT_REPORT -- an experiment on the slot table below and the event
 * line.  trackload-50/51b measured that a bare event (any code 1..13, no
 * parameters, the table empty) makes MAIN's player task issue the player
 * commands 1 and 2 within 100 ms and, five times in run 50, report an error
 * stop ("ｴﾗｰ停止通知をﾌﾟﾚｰﾔｰﾀｽｸから受理した", GUI: E-8302 CANNOT PLAY TRACK
 * (C611)); the DspTASK's record poster 0x1c7c62 was never called, so the
 * event is handled by the player task itself, which is also what copies the
 * slot entry (0x1b39cc: +96.. and state +128 into its deck record).  So the
 * event presumably says "read the table".  With CDJ_DSP_SLOT_REPORT=<state>
 * the model, once MAIN has closed the load (command 4 then 2 in +0x7ba0),
 * writes that state and position 0 into the first four entries and raises
 * one event; PLAY (3) makes it state 3 and another event.  What MAIN then
 * shows -- time fields, or E-8302 again -- is the measurement.
 * trackload-52-slotreport, state 2: MAIN answered the event with the player
 * commands 1, 2, 1 and reported an error stop ("ｴﾗｰ停止通知", E-8302) --
 * the same as a bare event during a load, and unlike a bare event after
 * one (trackload-51b: commands 1 and 2, no error).  trackload-53, state 1:
 * the same error stop.  So the entry is read, a non-zero state with position
 * 0 is not what a loaded deck looks like, and the next step is the reader
 * (0x1b39cc..0x1b3a60 and what it does with its deck record), not a fourth
 * guess.
 */
#define DSP_REPORT_ID_WORD      0x7cd4  /* read at 0x1b36c6, 0x1bd330; never written by MAIN */
#define DSP_SLOT_TABLE          0x7ce0
#define DSP_SLOT_SIZE           (33 * 4)
#define DSP_SLOT_ENTRIES        4
#define DSP_SLOT_POS_FINE       96      /* /294 -> sectors (0x1a1174) */
#define DSP_SLOT_POS_COARSE     100     /* *2, 0x1be946 */
#define DSP_SLOT_POS_THIRD      104
#define DSP_SLOT_STATE          128     /* 2 or 3 = running (0x1b3a02) */

/* Post an event, the report ID bumped first when CDJ_DSP_REPORT_ID is set. */
static void cdj_dsp_model_post(CdjDspModel *model, uint8_t *window,
                               size_t length, unsigned code, int64_t now)
{
    if (model->report_id && length >= DSP_REPORT_ID_WORD + 4) {
        model->report_id++;
        stl_le_p(window + DSP_REPORT_ID_WORD, model->report_id);
        fprintf(stderr, "cdj2000-dsp: report ID +0x%x = 0x%x t=%.3f\n",
                DSP_REPORT_ID_WORD, model->report_id, now / 1e9);
    }
    cdj_dsp_event(code);
}

/*
 * The DSP's word about a slot.  Its status publisher (0x800352bc) posts
 * +0x7c00 = (slot + 1) << 4 | 1 for slots 0..3 and (slot - 4) << 4 | 2 for
 * 5..8 (0x80045104; 4 and 9 get none) when a slot operation is through, and
 * MAIN's status reader (0x0419e6aa..0x0419e7bc) takes it, clears the word and,
 * for slots 0..3 of either kind, clears two words of its deck record,
 * X+0x1fc and X+0x188.  X+0x1fc is 1 from the moment MAIN records a hot cue
 * (0x11) and nothing else clears it (runs/cosim/sc-4): without this word MAIN
 * waits for the slot for good, and a CUE after a hot cue jump is not sent at
 * all (stock sc-3, port r75).  The DSP also posts event 1 with the slot once
 * the slot's buffer holds more than 40 frames (0x8002eb14); that one is
 * behind CDJ_DSP_SLOT_READY_EVENT until a run shows MAIN needs it.
 */
static void cdj_dsp_model_slot_msg(CdjDspModel *model, uint8_t *window,
                                   size_t length, unsigned slot, int64_t now)
{
    uint32_t msg;

    if (!model->slot_msg || length < 0x7c04 || slot == 4 || slot >= 9) {
        return;
    }
    msg = slot < 4 ? ((slot + 1) << 4) | 1 : ((slot - 4) << 4) | 2;
    stl_le_p(window + 0x7c00, msg);
    if (model->slot_ready_event) {
        cdj_dsp_event(0x0100 | slot);
    }
    fprintf(stderr, "cdj2000-dsp: slot %u done: +0x7c00 = 0x%02x%s t=%.3f\n", slot, msg,
            model->slot_ready_event ? ", event 1" : "", now / 1e9);
}

/*
 * The position units are a guess to be measured against the time display:
 * +96 is divided by 294 at 0x1a1174 and 294 * 75 = 22050, so it is taken as
 * 22050ths of a second; +100 is doubled at 0x1be946 and is written as CD
 * sectors (75 a second); +104 stays 0.
 */
/*
 * The slot entry the DSP publishes when a slot operation completes
 * (0x80035370, called from the slot pass 0x8002d9ec and 0x8002af4c): slots
 * 0..3 at +0x7ce0 + 132 * slot, 5..8 at +0x7ef0 + 132 * (slot - 5), 4 and 9
 * none.  Words +0..+95 are three 32-byte copies (0x8002b67c of the record
 * ids at 0x10025b48 + 16 * slot); +96 = byte +15 of that descriptor * 294
 * plus word +20 of the slot record 0x10025038 + 48 * slot, the samples into
 * the CD frame (0x800356cc..0x8003570c); +100 and +104 are the descriptor's
 * halfwords +8 and +10 (0x80034818, 0x80034834); +108..+124 words 0, 4, 8,
 * 12 and 32 of the slot record; +128 is set to 1 or 3 by 0x800357f4 right
 * after.  MAIN's DSP task copies +100, +104 and +96 into its deck record
 * (0x041a0048: X + 456 + 12 * slot) and 0x041be974 reads +100 * 2 +
 * +96 / 294 as half frames.  The point MAIN keeps for a recorded slot, and
 * saves as the hot cue's time, is +32 * 2 + +96 / 294 (the record path
 * 0x041a1304, 0x041a1332..0x041a136a, into the deck record +708..+720): +32
 * is the first word of the second 32-byte copy, the slot's record state
 * (0x8002b67c: 0x80061260 + 4800 * record).  Presets proved it: +96 = 294
 * saved 1 (fk-3), +32 = 1500 saved 3000 (fk-11); +100 and +104 moved nothing
 * (fk-5).  MAIN forms the position report's half frames the same way
 * (+0x7c10 * 2 + (+0x7bf4 >= 294)), so +32 is in CD frames and +96 in
 * samples into the frame.  Whether the save converts half frames to ms is
 * open, so this stays off by default.
 */
static void cdj_dsp_model_slot_entry(CdjDspModel *model, uint8_t *window,
                                     size_t length, unsigned slot, int64_t ms,
                                     int64_t now)
{
    unsigned base;
    uint32_t samples;

    if (!model->slot_entry || slot == 4 || slot >= 9 || ms < 0) {
        return;
    }
    base = slot < 4 ? DSP_SLOT_TABLE + slot * DSP_SLOT_SIZE
                    : 0x7ef0 + (slot - 5) * DSP_SLOT_SIZE;
    if (length < base + DSP_SLOT_SIZE) {
        return;
    }
    samples = (uint32_t)(ms * 44100 / 1000);
    stl_le_p(window + base + 32, samples / 588);
    stl_le_p(window + base + DSP_SLOT_POS_FINE, samples % 588);
    stl_le_p(window + base + DSP_SLOT_POS_COARSE, samples / 588);
    fprintf(stderr, "cdj2000-dsp: slot %u entry +0x%x: frame %u + %u samples (%" PRId64
            " ms) t=%.3f\n", slot, base, samples / 588, samples % 588, ms, now / 1e9);
}

static void cdj_dsp_model_slot_report(CdjDspModel *model, uint8_t *window,
                                      size_t length, unsigned state,
                                      int64_t now)
{
    unsigned i;
    unsigned event = (state == 3 && model->slot_state == 3)
                     ? model->play_event : model->slot_event;
    uint32_t fine = (uint32_t)(model->slot_pos_ms * 22050 / 1000);
    uint32_t coarse = (uint32_t)(model->slot_pos_ms * 75 / 1000);

    if (length < DSP_SLOT_TABLE + DSP_SLOT_ENTRIES * DSP_SLOT_SIZE) {
        return;
    }
    for (i = 0; i < DSP_SLOT_ENTRIES; i++) {
        uint8_t *entry = window + DSP_SLOT_TABLE + i * DSP_SLOT_SIZE;

        stl_le_p(entry + DSP_SLOT_POS_FINE, fine);
        stl_le_p(entry + DSP_SLOT_POS_COARSE, coarse);
        stl_le_p(entry + DSP_SLOT_POS_THIRD, 0);
        stl_le_p(entry + DSP_SLOT_STATE, state);
    }
    if (state == 3 && model->slot_state != 3) {
        model->slot_last_ns = now;
    }
    model->slot_state = state;
    if (model->slot_pos_ms == 0 || (model->slot_pos_ms / 1000) % 10 == 0) {
        fprintf(stderr, "cdj2000-dsp: slot entries 0..%u: state %u, position "
                "%" PRId64 " ms (+96 %u, +100 %u); event 0x%x (0 = none) t=%.3f\n",
                DSP_SLOT_ENTRIES - 1, state, model->slot_pos_ms, fine, coarse,
                event, now / 1e9);
    }
    if (event) {
        cdj_dsp_model_post(model, window, length, event, now);
    }
}

/*
 * The slot table at +0x7ce0 (132-byte entries, index * 33 * 4: 0x19fffe..,
 * 0x1a0048.., 0x1b39cc.., 0x1be974..) is where MAIN reads the DSP's position:
 * 0x1be946 combines word +100 * 2 and word +96 / 294 into half-sector units
 * (a CD sector is 588 stereo frames) and compares them with its own count,
 * 0x1b3a02 treats word +128 == 2 or 3 as "running".  Writing sector 0, frame
 * 0 and state 1 there every tick (trackload-38) did not fill the time fields
 * -- the status record's words 5..8 stayed 0xbbbb, the builder's "blank"
 * (0x216802).  (That run's stream also crawled, but so did trackload-39
 * without the report: the cause was the level bookkeeping, see the note in
 * cdj_dsp_model_firmware.)  Nothing is reported there until the entry's
 * layout is measured; the time display stays blank.
 */

/* A data header (class 1) names the buffer the following transfers fill, in
   byte 2; a drop (class 2) takes its count out of the level of the buffer in
   byte 1 (see "The stream header" above for both).  The data itself is booked
   when it arrives, in cdj_dsp_model_firmware. */
static void cdj_dsp_model_stream_header(CdjDspModel *model, uint8_t *window,
                                        uint32_t header, int64_t now)
{
    unsigned class = header >> 24;
    /*
     * Byte 1 is the buffer for every class: the class-2 drops, the class-3
     * headers, and class 1 too (bit 9 at 0x800413c4 tells buffer 2).  Byte 2
     * of a class-1 header is what it does: 1 opens a stream, 2 carries data.
     * Reading the buffer from byte 2 booked all of buffer 1's data (0x0102
     * 0100) to +0x7ccc, the level behind the position, and only each open's
     * first transfer to +0x7cd0, the level ahead -- so a jump to a cue, which
     * moves the position by the level ahead (the jobs, 0x041b6ad2), stopped
     * a few half frames short of the cue once the first 34 were used up.
     */
    unsigned buffer = (header >> 8) & 0xff;
    unsigned level = buffer == 1 ? DSP_LEVEL_BUFFER1
                   : buffer == 2 ? DSP_LEVEL_BUFFER2 : 0;
    int32_t count = (int32_t)ldl_le_p(window + DSP_HEADER_COUNT);
    int32_t was, is;

    if (!level) {
        return;
    }
    if (class == 1) {
        if (count >= 0) {
            model->stream_buffer = buffer;
        }
        return;
    }
    if (class != 2) {
        return;
    }
    if (count < 0) {
        count = 20;
    }
    was = (int32_t)ldl_le_p(window + level);
    is = was > count ? was - count : 0;
    stl_le_p(window + level, is);
    fprintf(stderr, "cdj2000-dsp: buffer %u dropped %d -> level %d (+0x%04x) t=%.3f\n",
            buffer, count, is, level, now / 1e9);
}

/*
 * Which record the position report names, for streams of any format.
 *
 * The DSP takes a record id in one place only: the class-1 header handler
 * reads parameter 13 -- +0x8120 of the last +0x8100 command, copied by
 * 0x80044ab4 for format 2 and for 3/4 alike -- at 0x80041aec and registers
 * it with 0x8002a3a4 in a table of ten records kept in order (head, tail and
 * count bytes at b14+799/800/801).  Stock 4.33 loading an MP3 (cosim
 * scenario-8): +0x7ba0 = 1, +0x8100 = 3 naming record 1, class-1 0x01010100,
 * then 0x01020100 with 40 in +0x8144 for every transfer; 15 s later +0x8100
 * = 3 naming record 2 -- the next track, preloaded -- and another 0x01010100,
 * and so on to record 5.  Both kinds of header reach the registration, at
 * the end of the handler's stream loop, and an id is registered once; the
 * NXS port's first header is a 0x01020100, which names its record too.  The record played is the first one opened after
 * the load began, the rest wait behind it: naming the latest (trackload-87)
 * switched the deck to the next track, and naming none left MAIN without a
 * track length, so the time display read "--".  The "Only a 2 sets it" rule
 * below stays for format 2, which this has not been measured on.
 */
static void cdj_dsp_model_stream_open(CdjDspModel *model, uint8_t *window,
                                      size_t length, uint32_t header, int64_t now)
{
    unsigned i;

    /* Any class-1 header: 0x0101.. (a new stream) and 0x0102.. (the next
       part) meet at 0x800413c4 and end at the registration (0x80041aec),
       which skips an id already in the table -- so a record counts from the
       first class-1 header after its format command, whichever kind. */
    if ((header >> 24) != 1 || !model->pos_report || !model->fmt_seen) {
        return;
    }
    /*
     * Byte 1 is the stream buffer (bit 9 in the DSP's test at 0x800413c4,
     * as byte 1 of the class-2 drops and of the class-3 headers).  The
     * registration is the same for both buffers (0x80041aec, the id of the
     * last format command), and so is event 5 after a new id joins the table
     * (0x8003079c).  Only buffer 1 carries what the deck plays: every load
     * and every jump to a cue streams there, so only a buffer-1 record can be
     * the one played.  Buffer 2 got the last second of the PREVIOUS track
     * (aconcert-1g, loading track 2: record 0xff, track number 1 in +0x8124,
     * from frame 11136 of track 1's file) -- it names no position.
     */
    unsigned buffer = (header >> 8) & 0xff;
    bool known = false;

    if (model->status_record && ((header >> 16) & 0xff) == 1
        && (buffer == 1 || buffer == 2)) {
        if (length >= 0x81c0 && model->status_current) {
            /* A buffer-1 open with no record played yet names it (below). */
            unsigned current = buffer == 1
                && (model->pos_record == 0 || model->pos_state == 0)
                ? model->fmt_record : model->pos_record;

            if (!current && model->status_first) {
                current = model->first_record;
            }

            if (current) {
                window[0x81a4] = current;
                window[0x8184] = current;
            }
        } else if (length >= 0x81c0) {
            window[buffer == 1 ? 0x81a4 : 0x8184] = model->fmt_record;
        }
    }

    for (i = 0; i < model->rec_count; i++) {
        if (model->rec_queue[i] == model->fmt_record) {
            known = true;
        }
    }
    if (!known && model->rec_count < ARRAY_SIZE(model->rec_queue)) {
        model->rec_queue[model->rec_count++] = model->fmt_record;
        if (model->events) {
            cdj_dsp_event(0x0500);      /* 0x8003079c: a record joined the table */
        }
    }
    if (buffer != 1 || known) {
        if (!known) {
            fprintf(stderr, "cdj2000-dsp: stream open 0x%08x registers record %u "
                    "(buffer %u) t=%.3f\n", header, model->fmt_record, buffer, now / 1e9);
        }
        return;
    }
    if (model->pos_record == 0 || model->pos_state == 0) {
        memset(model->seek_applied, 0, sizeof(model->seek_applied));
        model->seek_armed = true;
        model->pos_record = model->fmt_record;
        model->pos_ms = 0;
        model->cue_ms = 0;
        model->pos_state = 2;
        fprintf(stderr, "cdj2000-dsp: stream open 0x%08x names record %u, the "
                "one to play, position 0 t=%.3f\n", header, model->pos_record,
                now / 1e9);
    } else {
        fprintf(stderr, "cdj2000-dsp: stream open 0x%08x queues record %u "
                "behind record %u t=%.3f\n", header, model->fmt_record,
                model->pos_record, now / 1e9);
    }
}

/*
 * A stream that starts from a frame, not from the file's start.  The class-1
 * header copy (0x80044c90) takes +0x814c, +0x8154, +0x8158 and +0x815c as
 * parameters 24, 26, 27 and 28: the byte to start at, a count of frames the
 * decoder then runs and discards (g688, 0x80046ac4), the frame counter's
 * start (g696 = p28 - p26, +1 per decoded frame, 0x80046abc) -- so the first
 * frame heard is number p28 -- and p27, which 0x80042b08 keeps in b14+28 for
 * the decode loop to subtract (0x80042f8c): samples into that first frame,
 * below 1152 in every run (900, 1008).  Stock 4.33 sends them with the first
 * buffer-1 data header after a flush when it jumps to a memory cue (r55:
 * frame 532 + 1008 = 13.92 s, the cue), and with the buffer-2 open of the
 * previous track's last second (frame 11136, load2-3 and r55-4), which is
 * not the deck's position; the NXS port on a load from the cue.  The model
 * has no decoder, so a buffer-1 start is the position: p28 MPEG-1 layer III
 * frames of 1152 samples at 44.1 kHz plus p27 samples, for the record being
 * played.  Taken once per set of values.
 */
static void cdj_dsp_model_stream_seek(CdjDspModel *model, uint8_t *window,
                                      uint32_t header, int64_t now)
{
    uint32_t p26 = ldl_le_p(window + 0x8154);
    uint32_t p27 = ldl_le_p(window + 0x8158);
    uint32_t p28 = ldl_le_p(window + 0x815c);

    if ((header >> 24) != 1 || !model->pos_report || !(p26 | p27 | p28)
        || ((header >> 8) & 0xff) != 1) {
        return;         /* buffer 2's start is not the deck's (see above) */
    }
    if (p26 == model->seek_applied[0] && p27 == model->seek_applied[1]
        && p28 == model->seek_applied[2]) {
        return;
    }
    if (!model->seek_armed) {
        /* reg-3: after the jump had landed on the cue, MAIN opened buffer 1
           of the same record again from 2.05 s; the deck did not move (the
           real one stands on the cue).  A start is the position only for
           the stream that named the record, before any job or state. */
        fprintf(stderr, "cdj2000-dsp: 0x%08x starts record %u at frame %u, but the deck "
                "is already placed: position untouched t=%.3f\n", header,
                model->fmt_record, p28, now / 1e9);
        return;
    }
    if (!model->fmt_seen || model->fmt_record != model->pos_record) {
        fprintf(stderr, "cdj2000-dsp: 0x%08x starts record %u at frame %u, not the "
                "one played (%u): position untouched t=%.3f\n", header,
                model->fmt_record, p28, model->pos_record, now / 1e9);
        return;
    }
    model->seek_armed = false;
    model->seek_applied[0] = p26;
    model->seek_applied[1] = p27;
    model->seek_applied[2] = p28;
    model->pos_ms = ((int64_t)p28 * 1152 + p27) * 1000 / 44100;
    model->cue_ms = model->pos_ms;
    model->pos_last_ns = now;
    fprintf(stderr, "cdj2000-dsp: 0x%08x starts record %u at frame %u + %u samples "
            "(%u discarded, byte 0x%x) -> position %" PRId64 " ms t=%.3f\n", header,
            model->pos_record, p28, p27, p26, ldl_le_p(window + 0x814c),
            model->pos_ms, now / 1e9);
}

#define DSP_POS_STATUS      0x7bf0
#define DSP_POS_SUBFRAME    0x7bf4
#define DSP_POS_FRAMES      0x7c10
#define DSP_POS_VALID       0x7c14

static void cdj_dsp_model_position_report(CdjDspModel *model, uint8_t *window,
                                          size_t length, int64_t now)
{
    uint32_t frames, samples;

    if (length < 0x7c60 || !model->control_cleared) {
        return;
    }
    /*
     * +0x7bc4 is not a transport word: MAIN's DSP task (0x19fca2, the
     * writer at 0x1a0c68/0x1a0d6a) rebuilds it every pass from the deck's
     * flag bytes for the state it is in -- bit 31 comes from byte 0x690 in
     * state 4 only, bits 30..24 from 0x691/0x692/0x69b/0x69c../0x69f.  It
     * looked like a play toggle in trackload-104..113 because those bytes
     * change with the keys; the state requests below are the transport.
     */
    if (model->pos_state == 3) {
        /*
         * +0x7bc0 is the playback rate MAIN sets from the tempo slider,
         * fixed point with 2^20 = 1.0 (trackload-100: 0x00100000 at rest,
         * 0x0010020c = +0.05 % when the record's tempo word said 5).
         * Elapsed guest time times that rate is the audio time played.
         */
        int64_t rate = ldl_le_p(window + 0x7bc0) & 0xffffff;
        int64_t played_ns;

        /*
         * Before MAIN has written a rate the word is 0: play at 1.0.  After
         * that, anything from 0 up is the tempo: WIDE (+/-100 %) reaches 0
         * at -100 % (MAIN's own BPM reads 0 there, px-1), and a deck at 0
         * stands still.
         */
        if (rate >= 0x20000 && rate <= 0x300000) {
            model->rate_valid_seen = true;
        }
        if (rate > 0x300000 || (rate < 0x20000 && !model->rate_valid_seen)) {
            rate = 0x100000;            /* nothing sensible there: nominal */
        }
        /*
         * In nanoseconds, with the part below a millisecond carried over: a
         * whole-ms step lost the fraction on every tick, so -10 % (0x0e6666)
         * ran at 0.8 instead of 0.9 (NEW FIRMWARE cs-135).
         */
        played_ns = (now - model->pos_last_ns) * rate >> 20;
        if (model->reverse && (ldl_le_p(window + 0x7bc4) & 0x80000000u)) {
            int64_t total = model->pos_ms * SCALE_MS + model->pos_rem_ns - played_ns;

            if (total < 0) {
                total = 0;
            }
            model->pos_ms = total / SCALE_MS;
            model->pos_rem_ns = total % SCALE_MS;
        } else {
            played_ns += model->pos_rem_ns;
            model->pos_ms += played_ns / SCALE_MS;
            model->pos_rem_ns = played_ns % SCALE_MS;
        }
        model->pos_last_ns = now;
        if (model->loop_on && model->loop_out_ms > model->loop_in_ms
            && model->pos_ms >= model->loop_out_ms) {
            model->pos_ms = model->loop_in_ms
                + (model->pos_ms - model->loop_in_ms)
                % (model->loop_out_ms - model->loop_in_ms);
        }
    }
    frames = (uint32_t)(model->pos_ms * 75 / 1000);
    samples = (uint32_t)((model->pos_ms * 44100 / 1000) % 588);
    stl_le_p(window + DSP_POS_STATUS, 0);
    stl_le_p(window + DSP_POS_SUBFRAME, samples);
    /* +0x7bfc bits 1/2 reach MAIN's reader as [X-68] (0x19e6b0): a guess at
       "running" / "standing" so a CUE while standing can set its point */
    stl_le_p(window + 0x7bfc, model->pos_state == 3 ? 2 : model->pos_state == 2 ? 4 : 0);
    stl_le_p(window + DSP_POS_FRAMES, frames);
    /*
     * CDJ_DSP_STATUS_FIRST also covers the report: after a quantized hot cue
     * call MAIN re-streams with a buffer-2 open only and jumps (0x21), so no
     * buffer-1 open names the record again.  The DSP's current record is the
     * first one registered after the flush; with +0x7c14 = 0 MAIN's DSP task
     * found no ring entry (0x0419ef0c -> 0x041b23e4, 0x04836394+0x7c = 0),
     * and the next LOOP IN read the track entry through it (0x041b5524 ->
     * 0x041b55ac: 0 + 0x120) and died on an address error (EXPEVT 0xe0 at
     * 0x041c050a), which the GUI shows as E-8709.
     */
    stl_le_p(window + DSP_POS_VALID, !model->pos_state ? 0
             : model->pos_record ? model->pos_record
             : model->status_first ? model->first_record : 0);
    if (model->transport_log
        && (model->pos_state == 3 || model->pos_state != model->transport_last_state
            || model->pos_ms != model->transport_last_ms)) {
        fprintf(model->transport_log, "%" PRId64 " %" PRId64 " %u 0x%06x %u\n",
                now, model->pos_ms, model->pos_state,
                ldl_le_p(window + 0x7bc0) & 0xffffff, model->pos_record);
        fflush(model->transport_log);
        model->transport_last_ms = model->pos_ms;
        model->transport_last_state = model->pos_state;
    }
    if (model->pos_state == 3 && now - model->pos_print_ns >= 5 * 1000000000LL) {
        model->pos_print_ns = now;
        fprintf(stderr, "cdj2000-dsp: position %" PRId64 " ms = frame %u + %u "
                "samples, state %u, rate 0x%06x t=%.3f\n", model->pos_ms, frames, samples,
                model->pos_state, ldl_le_p(window + 0x7bc0) & 0xffffff, now / 1e9);
    }
}

/*
 * The playback rate, one line per change.  +0x7bc0 is 2^20 = 1.0; MAIN's DSP
 * task (0x0419fca2) writes it every pass from the deck word at +0x694 of its
 * deck block: 0x041a0d64 in state 4, 0x041a0e2a and 0x041a0fd0 in the other
 * play states (0x0419ff00/0x0419ff24 on the way through).  So the writer is
 * always the DSP task and the PC says nothing about why; what changed the
 * rate is the input before it, which the line names from the input channel
 * (press, analogue field, medium), with its guest time.  Polled on the
 * model's 10 ms tick: changes inside one tick are one line.
 */
static void cdj_dsp_model_rate_log(CdjDspModel *model, uint8_t *window,
                                   size_t length, int64_t now)
{
    uint32_t rate, flags, hold;
    int64_t input_ns = 0;
    const char *input;

    if (!model->rate_log || !window || length < 0x7bd0) {
        return;
    }
    rate = ldl_le_p(window + 0x7bc0);
    /* The DSP's command pass unpacks the whole block: +0x7bc4's bits into
     * separate fields and +0x7bcc (0x800439ec..0x80043aa4).  MAIN zeroes
     * +0x7bcc while the jog top is held (lm-5); both are logged with the
     * rate, so a change of either shows next to the input that made it. */
    flags = ldl_le_p(window + 0x7bc4);
    hold = ldl_le_p(window + 0x7bcc);
    if (model->rate_seen && rate == model->rate_last
        && flags == model->rate_flags_last && hold == model->rate_hold_last) {
        return;
    }
    input = cdj_input_last_event(&input_ns);
    fprintf(stderr, "cdj2000-dsp: rate 0x%08x = %.6f (%+.3f %%) was 0x%08x"
            " (%.6f), +0x7bc4 0x%08x +0x7bcc 0x%x t=%.3f state %u%s; last input: %s",
            rate, (rate & 0xffffff) / 1048576.0,
            ((rate & 0xffffff) / 1048576.0 - 1.0) * 100.0, model->rate_last,
            (model->rate_last & 0xffffff) / 1048576.0, flags, hold, now / 1e9,
            model->pos_state,
            (rate & 0xffffff) > 0x300000
            || ((rate & 0xffffff) < 0x20000 && !model->rate_valid_seen)
                ? " (the model plays such a rate at 1.0)" : "",
            input ? input : "none");
    if (input) {
        fprintf(stderr, " at %.3f", input_ns / 1e9);
    }
    fprintf(stderr, "\n");
    model->rate_last = rate;
    model->rate_flags_last = flags;
    model->rate_hold_last = hold;
    model->rate_seen = true;
}

void cdj_dsp_model_position_tick(CdjDspModel *model, uint8_t *window, size_t length)
{
    if (model && model->pos_report && model->running && !model->absent && window
        && model->pos_state == 3) {
        cdj_dsp_model_position_report(model, window, length,
                                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

void cdj_dsp_model_tick(CdjDspModel *model, uint8_t *window, size_t length)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed_ms = (now - model->last_tick_ns) / SCALE_MS;

    model->last_tick_ns = now;
    cdj_dsp_model_rate_log(model, window, length, now);
    cdj_dsp_model_loop_select(model, window, length, now);
    if (model->ack_control && model->running && !model->absent && window) {
        unsigned i;

        for (i = 0; i < DSP_ACK_COMMAND_WORDS; i++) {
            const DspAckWord *req = &dsp_ack_words[i];
            int32_t word;

            if (req->offset + 8 > length) {
                continue;
            }
            word = (int32_t)ldl_le_p(window + req->offset);
            if (word <= 0 || word >= req->limit) {
                continue;
            }
            model->acknowledged++;
            if (req->offset == 0x8140 && (word >> 24) == 1) {
                /* A stream open: every one, with its time -- the DSP takes
                   the record id of the last format command here
                   (0x80041aec: parameter 13, +0x8120), so which stream got
                   which id is only readable from the sequence.  +0x8150..
                   +0x815c are the seek fields the DSP copies with +0x814c
                   (0x80044c90): stock 4.33 leaves them 0. */
                fprintf(stderr, "cdj2000-dsp: stream open +0x8140 = 0x%08x "
                        "(+0x8144 %08x +0x8148 %08x +0x814c %08x +0x8150 %08x "
                        "+0x8154 %08x +0x8158 %08x +0x815c %08x +0x8168 %08x) "
                        "#%" PRIu64 " t=%.3f\n", (uint32_t)word,
                        ldl_le_p(window + 0x8144), ldl_le_p(window + 0x8148),
                        ldl_le_p(window + 0x814c), ldl_le_p(window + 0x8150),
                        ldl_le_p(window + 0x8154), ldl_le_p(window + 0x8158),
                        ldl_le_p(window + 0x815c), ldl_le_p(window + 0x8168),
                        model->acknowledged, now / 1e9);
            } else if (req->offset == 0x8100 && length >= 0x8128) {
                /* the format command: its whole parameter block, which is
                   where the stream's kind and record id travel */
                fprintf(stderr, "cdj2000-dsp: control +0x8100 command 0x%08x "
                        "(+4.. %08x %08x %08x %08x %08x %08x %08x %08x %08x) "
                        "acknowledged, #%" PRIu64 " t=%.3f\n", (uint32_t)word,
                        ldl_le_p(window + 0x8104), ldl_le_p(window + 0x8108),
                        ldl_le_p(window + 0x810c), ldl_le_p(window + 0x8110),
                        ldl_le_p(window + 0x8114), ldl_le_p(window + 0x8118),
                        ldl_le_p(window + 0x811c), ldl_le_p(window + 0x8120),
                        ldl_le_p(window + 0x8124), model->acknowledged, now / 1e9);
            } else
            fprintf(stderr, "cdj2000-dsp: control +0x%04x command 0x%08x "
                    "(+4.. %08x %08x %08x %08x) acknowledged, #%" PRIu64
                    " t=%.3f\n",
                    req->offset, (uint32_t)word,
                    ldl_le_p(window + req->offset + 4),
                    ldl_le_p(window + req->offset + 8),
                    ldl_le_p(window + req->offset + 12),
                    ldl_le_p(window + req->offset + 16),
                    model->acknowledged, now / 1e9);
            if (req->offset == 0x8140) {
                cdj_dsp_model_stream_header(model, window, (uint32_t)word, now);
                cdj_dsp_model_stream_open(model, window, length, (uint32_t)word, now);
                cdj_dsp_model_stream_seek(model, window, (uint32_t)word, now);
            }
            if (req->offset == 0x8100 && word >= 2 && word <= 4
                && length >= 0x8128) {
                model->fmt_record = ldl_le_p(window + 0x8120);
                model->fmt_seen = true;
            }
            if (req->offset == 0x7ba0 && (word == 1 || word == 5)) {
                /* a load begins, or the deck is unloaded: no stream open */
                model->rec_count = 0;
            }
            if (req->offset == 0x7cb0 && word == 1) {
                /*
                 * The DSP hands the word to 0x800429e0: 1 resets all ten
                 * slot records (0x8002abc4: held flags 0x10024f70 cleared,
                 * positions -1), 2 only the stream (0x8002ad84).  So a new
                 * track forgets the hot cues and a jump or a hot cue call
                 * (flush 2) keeps them -- also with CDJ_DSP_FLUSH=0, which
                 * only switches off the window resets below.
                 */
                for (unsigned i = 0; i < DSP_HOT_SLOTS; i++) {
                    model->hot_ms[i] = -1;
                }
                cdj_dsp_model_loops_clear(model);
            }
            if (req->offset == 0x7cb0 && (word == 1 || word == 2)
                && model->flush_reset && length >= 0x81c8) {
                memset(window + 0x7c10, 0, 0x20);       /* +0x7c10..+0x7c2c */
                memset(window + 0x8180, 0, 0x40);       /* +0x8180..+0x81bf */
                stl_le_p(window + DSP_LEVEL_BUFFER1, 0);
                stl_le_p(window + DSP_LEVEL_BUFFER2, 0);
                stl_le_p(window + 0x81c4, 0);
                stl_le_p(window + 0x7cd4, 0xff);
                model->rec_count = 0;
                model->pos_record = 0;
                model->first_record = 0;
                model->pos_ms = 0;
                model->pos_state = 0;
                model->loop_on = false;
                fprintf(stderr, "cdj2000-dsp: +0x7cb0 = %d flushes the DSP: record table, "
                        "levels and position report cleared, +0x7cd4 = 0xff%s t=%.3f\n",
                        word, word == 1 ? ", hot cue slots emptied" : "", now / 1e9);
            }
            stl_le_p(window + req->offset, 0);
            if (req->clear_result) {
                stl_le_p(window + req->offset + 4, 0);
            }
            if (req->offset == 0x7c80 && model->pos_report && length >= 0x7c88
                && (word == 0x11 || word == 0x12 || word == 0x21 || word == 0x22)) {
                /*
                 * MAIN's DSP task 0x19fca2 (Ghidra, trackload-118) writes
                 * +0x7c80 = msg[0xb] with msg[0xc] in +0x7c84 for the codes
                 * 0x11/0x12/0x21/0x22.  A CUE while playing sends 0x11, then
                 * the state request 4, then 0x21; the PLAY after it sends
                 * state 2 and 0x21 (trackload-104, 113, 117).  The stop there
                 * is the 4's doing, not the 0x11's.
                 */
                /*
                 * +0x7c84 is not a position but a SLOT: the DSP's handlers
                 * (0x80043ea8 for 0x11/0x12, 0x80043f20 for 0x21/0x22) take
                 * +0x7c84 + 5 * bit 1 of the code as the slot.  Slot 0 is the
                 * cue point: at the track's start after a load, and where a
                 * jump to a cue landed (the stream-open start plus the jobs,
                 * see cdj_dsp_model_stream_seek) -- after the jobs converged
                 * on a memory cue MAIN sends 0x11 with slot 0 (cue-12).
                 */
                /*
                 * hc-1 (stock 4.33, track 836, live): slot 1 is hot cue A,
                 * 2 is B.  0x11/0x12 RECORD a slot: 0x80043ea8 fills it from
                 * the running play state (0x8002cfb0 -> 0x8002ca88) only if
                 * it is not held yet (0x10024f70[slot]) and leaves the play
                 * state alone -- REC MODE (a short press) + A while playing
                 * sends 0x11 slot 1 and MAIN goes on as playing (its next
                 * PLAY is a pause, +0x7ba0 = 2).  0x21/0x22 JUMP to a held
                 * slot (0x80043f20 queues it in 0x100255e0): A outside REC
                 * sends +0x7c9c = 0x2c, +0x7ba0 = 2, 0x21 slot 1 and plays
                 * on from A.  Slot 0 keeps its own bookkeeping: the cue is
                 * where the load's seek and the jobs put it (cue_ms).
                 */
                uint32_t slot = ldl_le_p(window + 0x7c84) + ((word & 2) ? 5 : 0);
                bool jump = (word & 0xf0) == 0x20;
                const char *what;

                if (slot == 0 && !jump) {
                    /*
                     * Recording the cue slot, as for any slot: LOOP IN sends
                     * 0x1c, 0x11 slot 0 while playing and the IN is where the
                     * deck is (runs/cosim/loop-nibble).  Jumping to the old
                     * cue here put every IN at 0:00 (NEW FIRMWARE B17) and
                     * gave MAIN a 0 to store for a recorded hot cue (B18).
                     * After a jump to a memory cue the jobs have already put
                     * the deck on the cue, so the two agree there.
                     */
                    model->cue_ms = model->pos_ms;
                    what = " (the cue) recorded here";
                    if (model->loop_in_slot0) {
                        /*
                         * Segment slot 1 is the cue slot's segment (its
                         * points at 0x8002f364 belong to slot n - 1 = 0).
                         * LOOP IN while playing sends 0x3c, 0x11 slot 0 and
                         * +0x7ba0 = 2, and OUT then sends only command 2
                         * (0x12, fk-3/fk-4): no command 1, so the IN is the
                         * point this record keeps.  RELOOP is 0x21 slot 0.
                         */
                        model->loop_in_ms = model->pos_ms;
                        model->loop_on = false;
                    }
                } else if (slot == 0) {
                    model->pos_ms = model->cue_ms;
                    what = " (the cue)";
                } else if (slot >= DSP_HOT_SLOTS) {
                    what = " (no such slot, ignored)";
                } else if (model->loop_slots && slot >= DSP_LOOP_OUT_SLOT) {
                    /*
                     * Slots 5..9 are the OUT points of the loops on slots
                     * 0..4.  When slot 5 + n is made ready, 0x8002d044
                     * builds loop n's entry (0x10024fe8 + 16 * n, flag 1)
                     * with the length OUT - IN + 1 from slot n's point
                     * (0x8002d0a8..0x8002d104); the play pass takes a loop
                     * from slot n and slot n + 5 (0x80017a88..0x80017ae0)
                     * and turns back at its end only while both belong to
                     * the loaded track (0x8002bfd4, 0x8002c0bc).  Stock 4.33
                     * with the right states (runs/cosim/s25-v1): LOOP IN
                     * sends 0x1c and 0x11 slot 0, LOOP OUT 0x12 = record slot
                     * 5 -- recorded where the deck is, so it is already at
                     * the end and turns back to the IN at once -- EXIT sends
                     * 0x1c and RELOOP then 0x22 = slot 5.  Not traced in the
                     * DSP yet, so taken from what those keys do on a deck:
                     * that command 12 (0x8002feb0 clears segment entry n,
                     * 0x10024f98 + 16 * n) is what ends the loop -- the model
                     * leaves it on 0xc, below -- and that the jump queued for
                     * slot 5 + n (0x80043f94) lands on the loop's IN.
                     * 0x8002d044 also clears the slot's held flag
                     * (0x10024f70[slot] = 0), so a new OUT records again.
                     */
                    unsigned n = slot - DSP_LOOP_OUT_SLOT;
                    int64_t in = n == 0 ? model->cue_ms : model->hot_ms[n];

                    if (!jump) {
                        model->hot_ms[slot] = model->pos_ms;
                        model->loop_built[n] = true;        /* 0x8002d044 */
                    }
                    if (in >= 0 && model->hot_ms[slot] > in) {
                        model->loop_in_ms = in;
                        model->loop_out_ms = model->hot_ms[slot];
                        model->loop_on = true;
                        if (jump) {
                            model->pos_ms = in;
                        }
                        what = jump ? " (loop again: from its IN)"
                                    : " (a loop's OUT) recorded here, looping";
                    } else {
                        what = jump ? " (a loop without an IN before its OUT): position kept"
                                    : " (a loop's OUT) recorded here, no IN before it";
                    }
                } else if (!jump) {
                    if (model->hot_ms[slot] < 0) {
                        model->hot_ms[slot] = model->pos_ms;
                        what = " recorded here";
                    } else {
                        what = " already held, kept";
                    }
                } else if (model->hot_ms[slot] >= 0) {
                    model->pos_ms = model->hot_ms[slot];
                    what = " (a hot cue)";
                } else {
                    what = " is empty: position kept";
                }
                model->pos_last_ns = now;
                /* With CDJ_DSP_STATES a jump moves the position only: run or
                   stand is the last +0x7ba0 request's (0x80043f20 queues the
                   slot and leaves the state alone). */
                if (jump && slot < DSP_HOT_SLOTS && !model->states433
                    && (slot == 0 || model->hot_ms[slot] >= 0)) {
                    model->pos_state = model->pos_standby ? 2 : 3;
                }
                fprintf(stderr, "cdj2000-dsp: +0x7c80 = 0x%x slot %u%s -> position state %u "
                        "at %" PRId64 " ms%s t=%.3f\n", word, slot, what,
                        model->pos_state, model->pos_ms,
                        model->pos_standby ? " (standby)" : "", now / 1e9);
                if (slot < DSP_HOT_SLOTS && (!jump || slot == 0 || model->hot_ms[slot] >= 0)) {
                    cdj_dsp_model_slot_msg(model, window, length, slot, now);
                    cdj_dsp_model_slot_entry(model, window, length, slot,
                                             slot == 0 ? model->cue_ms : model->hot_ms[slot],
                                             now);
                }
            }
            if (req->offset == 0x7c80 && model->pos_report && model->slot_copy
                && length >= 0x7c8c && (word == 0x31 || word == 0x33)) {
                /*
                 * A slot copy.  The dispatcher sends 0x31 and 0x33 to
                 * 0x80043fcc, which calls 0x8002feec(code, +0x7c84 = source,
                 * +0x7c88 = destination) and writes its result back into
                 * +0x7c80: 0 done (also for source == destination), -1 when
                 * a source slot is empty (0x80034834: its descriptor's
                 * halfword +10 is 0).  It copies the source's 48-byte slot
                 * record into the destination's (0x10025038 + 48 slot,
                 * 0x80038040) and publishes the destination's entry
                 * (0x80035370, 0x800357f4).  0x33 does the same for slot
                 * source + 5 -> destination + 5, the loops' OUT points, and
                 * copies loop source's 16-byte entry to loop destination's
                 * (0x10024fe8 + 16 n).  Stock MAIN sends 0x33 with 0 -> 2
                 * when REC MODE + B stores a running loop on B (hl-1), and
                 * calling B then selects that loop with +0x7bc8 = 3.
                 */
                uint32_t src = ldl_le_p(window + 0x7c84);
                uint32_t dst = ldl_le_p(window + 0x7c88);
                bool pair = word == 0x33;
                bool failed = false;
                const char *what = "copied";

                if (src >= DSP_LOOP_OUT_SLOT || dst >= DSP_LOOP_OUT_SLOT) {
                    what = "out of the model's range, left alone";
                } else if (src == dst) {
                    what = "onto itself, nothing to do";
                } else if (cdj_dsp_model_loop_in(model, src) < 0
                           || (pair && model->hot_ms[DSP_LOOP_OUT_SLOT + src] < 0)) {
                    failed = true;
                    what = "source empty: -1";
                } else {
                    int64_t point = cdj_dsp_model_loop_in(model, src);

                    if (dst == 0) {
                        model->cue_ms = point;
                    } else {
                        model->hot_ms[dst] = point;
                    }
                    cdj_dsp_model_slot_entry(model, window, length, dst, point, now);
                    if (pair) {
                        model->hot_ms[DSP_LOOP_OUT_SLOT + dst] =
                            model->hot_ms[DSP_LOOP_OUT_SLOT + src];
                        model->loop_built[dst] = model->loop_built[src];
                        model->seg_end_ms[dst] = model->seg_end_ms[src];
                        model->seg_end_valid[dst] = model->seg_end_valid[src];
                        cdj_dsp_model_slot_entry(model, window, length,
                                                 DSP_LOOP_OUT_SLOT + dst,
                                                 model->hot_ms[DSP_LOOP_OUT_SLOT + dst],
                                                 now);
                    }
                }
                if (failed) {
                    stl_le_p(window + 0x7c80, (uint32_t)-1);
                }
                fprintf(stderr, "cdj2000-dsp: +0x7c80 = 0x%x slot copy %u -> %u%s: %s",
                        word, src, dst, pair ? " with its loop" : "", what);
                if (!failed && src < DSP_LOOP_OUT_SLOT && dst < DSP_LOOP_OUT_SLOT && src != dst) {
                    fprintf(stderr, " (point %" PRId64 " ms", cdj_dsp_model_loop_in(model, dst));
                    if (pair) {
                        fprintf(stderr, ", OUT %" PRId64 " ms",
                                model->hot_ms[DSP_LOOP_OUT_SLOT + dst]);
                    }
                    fprintf(stderr, ")");
                }
                fprintf(stderr, " t=%.3f\n", now / 1e9);
            }
            if (req->offset == 0x7c9c && model->pos_report && length >= 0x7cb0) {
                /*
                 * +0x7c9c = msg[0x15] * 16 + msg[0x13] (the same task): the
                 * segment slot in the high nibble and a slot command in the
                 * low one, parameters msg[0x16..0x19] in +0x7ca0..+0x7cac.
                 * 0xc arrived at the load, at a CUE and at loop IN with all
                 * parameters 0 (trackload-113/117/118); command 1 followed
                 * the IN with (1, 0x2c, 0, 1) and marks the slot's table
                 * entry queued (state 2 at deck+0x2c0+0x54*slot).  Neither
                 * moves the position -- an earlier reading of 0xc as a seek
                 * sent the IN back to the start (trackload-117).
                 */
                /*
                 * trackload-120: IN sent command 1 with (1, 264, 1170, 1) at
                 * 7.8 s and OUT command 2 with (1, 102, 1756, 1) at 11.7 s --
                 * word 3 is the position in half frames (150 a second, the
                 * position reader's own unit), word 2 the samples into the
                 * frame.  The DSP is expected to play the segment between
                 * the two points on its own; the model wraps the position.
                 */
                uint32_t sub = ldl_le_p(window + 0x7ca4);
                uint32_t half = ldl_le_p(window + 0x7ca8);
                int64_t point_ms = (int64_t)half * 1000 / 150 + sub * 1000 / 44100;
                const char *effect = "position untouched";

                /*
                 * The nibble is the slot + 1 (0x80044054: n - 1).  Stock's
                 * loop IN/OUT use n = 1, the cue slot (runs/cosim/loop-nibble:
                 * OUT = 0x12); calling hot cue B from the card sends 0x31
                 * command 1 with B's point (hc-1: (1, 102, 2218, 1) =
                 * 14788 ms, B = 14789) and then jumps to slot 2 -- so for
                 * n >= 2 command 1 is where that hot cue's slot starts.
                 */
                unsigned seg_slot = ((word >> 4) & 0xf) ? ((word >> 4) & 0xf) - 1 : 0;

                if (model->loop_select && seg_slot < DSP_LOOP_OUT_SLOT
                    && ((word & 0xf) == 1 || (word & 0xf) == 2 || (word & 0xf) == 9
                        || (word & 0xf) == 0xc || (word & 0xf) == 0xd)) {
                    /*
                     * The loop table (cdj_dsp_model_loop_select): command 1
                     * is loop n's IN, slot n's own point (the cue for n =
                     * 0), and drops its OUT; command 2 its OUT, slot 5 + n's
                     * point; command 9 its cached END; 12 / 13 drop the
                     * cached END of one loop / all five.  That command 2
                     * builds the loop as a 0x12 record does (0x8002d044) is
                     * inferred: stock loops right after a quantized OUT,
                     * which sends command 2 and no 0x12.
                     */
                    unsigned n = seg_slot;

                    switch (word & 0xf) {
                    case 1:
                        if (n == 0) {
                            model->cue_ms = point_ms;
                            model->loop_in_ms = point_ms;
                        } else {
                            model->hot_ms[n] = point_ms;
                        }
                        model->hot_ms[DSP_LOOP_OUT_SLOT + n] = -1;
                        model->loop_built[n] = false;
                        effect = "loop IN (slot's point), its OUT dropped";
                        break;
                    case 2:
                        model->hot_ms[DSP_LOOP_OUT_SLOT + n] = point_ms;
                        model->loop_built[n] = true;
                        effect = "loop OUT (slot 5 + n's point)";
                        break;
                    case 9:
                        model->seg_end_ms[n] = point_ms;
                        model->seg_end_valid[n] = true;
                        effect = "loop END cached";
                        break;
                    case 0xc:
                        model->seg_end_valid[n] = false;
                        effect = "cached END dropped";
                        break;
                    default:
                        for (unsigned i = 0; i < DSP_LOOP_OUT_SLOT; i++) {
                            model->seg_end_valid[i] = false;
                        }
                        effect = "all cached ENDs dropped";
                        break;
                    }
                } else if ((word & 0xf) == 1 && seg_slot >= 1 && seg_slot < DSP_HOT_SLOTS) {
                    model->hot_ms[seg_slot] = point_ms;
                    effect = "hot cue slot point";
                } else if ((word & 0xf) == 2 && seg_slot >= 1) {
                    effect = "hot cue slot end, not modelled";
                } else if ((word & 0xf) == 1) {
                    model->loop_in_ms = point_ms;
                    model->loop_on = false;
                    effect = "loop IN";
                } else if ((word & 0xf) == 2) {
                    model->loop_out_ms = point_ms;
                    model->loop_on = model->loop_out_ms > model->loop_in_ms;
                    effect = model->loop_on ? "loop OUT, looping" : "loop OUT before IN, ignored";
                } else if ((word & 0xf) == 0xc && model->loop_on) {
                    model->loop_on = false;
                    effect = "slot flushed, loop off";
                }
                fprintf(stderr, "cdj2000-dsp: +0x7c9c = 0x%x slot %u command %u (%u %u %u %u) "
                        "= %" PRId64 " ms: %s t=%.3f\n", word, (word >> 4) & 0xf, word & 0xf,
                        ldl_le_p(window + 0x7ca0), sub, half, ldl_le_p(window + 0x7cac),
                        point_ms, effect, now / 1e9);
            }
            if (req->offset == 0x8100 && word == 3 && model->first_record == 0
                && length >= 0x8128) {
                model->first_record = ldl_le_p(window + 0x8120);
            }
            if (req->offset == 0x8100 && word == 2 && model->pos_report
                && length >= 0x8128) {
                /*
                 * trackload-86/87: the PCM-channel command +0x8100 carries
                 * the load queue's record id in +0x8120/+0x8124 -- 2 at the
                 * load names record 1, and the 3 that follows some 40 s
                 * later names record 2, the NEXT track MAIN preloads into
                 * buffer 2.  MAIN's reader looks the record up by the word
                 * the DSP reports at +0x7c14 (0x1b23e4 over the ring at
                 * 0x4836908, 0x794 bytes each) and takes the track length
                 * X+620 from it, so the report must name the record being
                 * played: trackload-87 named record 2 and the deck switched
                 * to track 2 (5:31).  Only a 2 sets it.
                 */
                model->pos_record = ldl_le_p(window + 0x8120);
                model->pos_ms = 0;
                model->pos_state = 2;
                fprintf(stderr, "cdj2000-dsp: +0x8100 = %d names record %u, position 0 t=%.3f\n",
                        word, model->pos_record, now / 1e9);
            }
            if (req->offset == 0x7ba0 && model->pos_report && model->states433
                && word >= 1 && word <= 8) {
                /*
                 * The DSP takes the request at 0x80043e10 into 0x10025588
                 * (get(18)); its main pass adopts it as the running state
                 * b14+732 whenever no slot jump is queued (0x80048aa0: state
                 * 1 or 0x100255e0 == -1) and publishes it in +0x7bf8
                 * (0x80035940).  The audio pass 0x80014320 switches on that
                 * state through the table at 0x10006904 (0x80014354):
                 * 2 and 5 -> 0x800147b4, the decoder: the deck runs;
                 * 3 -> 0x80015724 and 4 -> 0x800152ec: no decoding, the
                 * position moves only by the jog count (4 also resets the
                 * decoder: b14+518 = 0, b14+526 = 1); 1 -> 0x80015bd8, output
                 * zeroed; 6 -> nothing; 7 -> 0x80015b48, 8 -> 0x80014398,
                 * the DSP's own end states.  Stock MAIN pairs them with its
                 * deck state 0x04fdc27b (fk-1): 3 PLAYING sends 2, 5 PAUSED
                 * sends 3, 6 CUED sends 4.  A 1 (a load or a re-stream
                 * begins) stops a running position and leaves the rest to
                 * the flush and +0x8100.
                 */
                if (word >= 2) {
                    model->pos_standby = word == 4;
                    if (word != 4) {
                        model->seek_armed = false;
                    }
                }
                if (word == 2 || word == 5) {
                    if (model->pos_state != 3) {
                        model->pos_last_ns = now;
                    }
                    model->pos_state = 3;
                } else if (word != 1 || model->pos_state == 3) {
                    model->pos_state = 2;
                }
                if (length >= 0x7bfc) {
                    stl_le_p(window + 0x7bf8, word);
                }
                if (word == 7 && model->auto_cue && length >= 0x7bfc) {
                    /*
                     * 7 is the AUTO CUE search: MAIN sends 1 and then 7 at a
                     * load when its AUTO CUE byte 0x04fdc1cf is 1.  The DSP
                     * searches in state 7 (0x80019608 sets it while b14+134
                     * is 0) and, when a slot completes in its slot pass with
                     * the state still 7, publishes the slot entry
                     * (0x80035370) and state 8 in +0x7bf8 (0x8002dc90 and
                     * 0x8002df6c for slot 0, 0x8002de68).  MAIN's DSP task
                     * keeps the 7 pending while +0x7bf8 reads 7 and takes
                     * any other value as the answer (0x041a0bec..0x041a0bfc).
                     * With no answer it gave up after 10 s and re-streamed
                     * (acue-1).  With 8 it sent 4 at once and the deck ended
                     * CUED, 0x04fdc27b = 6 (fk-8, 8 written by hand).  The
                     * search for the first sound above the AUTO CUE level is
                     * not modelled: the cue is where the stream starts.
                     */
                    model->cue_ms = model->pos_ms;
                    stl_le_p(window + 0x7bf8, 8);
                    cdj_dsp_model_slot_entry(model, window, length, 0, model->cue_ms, now);
                    fprintf(stderr, "cdj2000-dsp: +0x7ba0 = 7 (AUTO CUE search) answered: "
                            "+0x7bf8 = 8, cue at %" PRId64 " ms t=%.3f\n", model->cue_ms,
                            now / 1e9);
                }
                fprintf(stderr, "cdj2000-dsp: +0x7ba0 = %d -> DSP state %d (%s), position state %u "
                        "at %" PRId64 " ms%s t=%.3f\n", word, word,
                        word == 2 || word == 5 ? "runs" : "stands", model->pos_state,
                        model->pos_ms, model->pos_standby ? " (standby)" : "", now / 1e9);
            } else if (req->offset == 0x7ba0 && model->pos_report && word >= 2 && word <= 6) {
                /*
                 * +0x7ba0 = msg[0] is a state request (the task copies it,
                 * 5 and 6 as 5, and follows the DSP's answer in +0x7bf8):
                 * 3 at PLAY from the load or from a pause, 2 at a pause
                 * (PLAY while playing) and with 0x21 at the PLAY after a cue
                 * return, 4 at the load's end and after a cue return, 5 at
                 * an unload (trackload-104, 113, 117).
                 */
                model->pos_standby = word == 4;
                if (word != 4) {
                    model->seek_armed = false;
                }
                if (word == 3) {
                    model->pos_state = 3;
                    model->pos_last_ns = now;
                } else {
                    model->pos_state = 2;
                }
                fprintf(stderr, "cdj2000-dsp: +0x7ba0 = %d -> position state %u "
                        "at %" PRId64 " ms%s t=%.3f\n", word, model->pos_state,
                        model->pos_ms, model->pos_standby ? " (standby)" : "", now / 1e9);
            }
            if (req->offset == 0x7ba0 && model->slot_report) {
                if (word == 4) {
                    model->saw_load_end = true;
                } else if (word == 2 && model->saw_load_end
                           && model->slot_state == 0) {
                    cdj_dsp_model_slot_report(model, window, length,
                                              model->slot_loaded_state, now);
                } else if (word == 3 && model->slot_state != 0
                           && model->slot_state != 3) {
                    cdj_dsp_model_slot_report(model, window, length, 3, now);
                }
            }
        }
    }
    if (model->job_answer && model->running && !model->absent && window
        && length >= 0x7bb0 && model->control_cleared
        && (int32_t)ldl_le_p(window + 0x7ba4) > 0 && ldl_le_p(window + 0x7ba0) == 0) {
        model->jobs++;
        model->seek_armed = false;
        fprintf(stderr, "cdj2000-dsp: job +0x7ba4 = %d (count +0x7ba8 = %d, +0x7bac = %d) "
                "answered done, #%" PRIu64 " t=%.3f\n", (int32_t)ldl_le_p(window + 0x7ba4),
                (int32_t)ldl_le_p(window + 0x7ba8), (int32_t)ldl_le_p(window + 0x7bac),
                model->jobs, now / 1e9);
        stl_le_p(window + 0x7ba4, 0);
        if (model->job_consume) {
            /* +0x7cd0 is the audio ahead of the position and +0x7ccc the
               audio behind it (0x041b6a0e: a job backwards takes its count
               from +0x7ccc, forwards from +0x7cd0), both in CD frames; a
               job's count is in half frames. */
            int32_t count = (int32_t)ldl_le_p(window + 0x7ba8);
            int32_t take = (count < 0 ? -count + 1 : count + 1) / 2;
            unsigned from = count < 0 ? DSP_LEVEL_BUFFER2 : DSP_LEVEL_BUFFER1;
            unsigned to = count < 0 ? DSP_LEVEL_BUFFER1 : DSP_LEVEL_BUFFER2;
            int32_t have = (int32_t)ldl_le_p(window + from);

            take = take < have ? take : have;
            stl_le_p(window + from, have - take);
            stl_le_p(window + to, (int32_t)ldl_le_p(window + to) + take);
            /* The buffer's status word counts what the DSP has used of it
               (trackload-69/70: the worker streams from +0x81a0 on), so
               the stream worker sees room for more. */
            if (length >= 0x81c0) {
                unsigned used = count < 0 ? 0x8180 : 0x81a0;

                stl_le_p(window + used, ldl_le_p(window + used) + take);
            }
        }
        if (model->job_advance) {
            int32_t count = (int32_t)ldl_le_p(window + 0x7ba8);
            int32_t param = (int32_t)ldl_le_p(window + 0x7bac);

            if (param == 0 && count != 0) {
                /* In whole half frames, as MAIN compares them: the first ms
                   of half frame h is ceil(h * 1000 / 150), and the report
                   (+0x7c10 * 2 + (+0x7bf4 >= 294)) reads it back as h. */
                int64_t half = model->pos_ms * 150 / 1000 + count;

                if (half < 0) {
                    half = 0;
                }
                model->pos_ms = (half * 1000 + 149) / 150;
                model->cue_ms = model->pos_ms;      /* a jump's landing is the new cue */
                model->pos_last_ns = now;
                fprintf(stderr, "cdj2000-dsp: job moved the position %d half frames "
                        "to %" PRId64 " ms t=%.3f\n", count, model->pos_ms, now / 1e9);
            } else {
                fprintf(stderr, "cdj2000-dsp: job with +0x7bac = %d, count %d: position "
                        "left at %" PRId64 " ms t=%.3f\n", param, count, model->pos_ms,
                        now / 1e9);
            }
        }
        if (model->events) {
            fprintf(stderr, "cdj2000-dsp: event 5 after the job t=%.3f\n", now / 1e9);
            cdj_dsp_model_post(model, window, length, 0x0500, now);
        }
    }
    if (model->pos_report && model->running && !model->absent && window) {
        cdj_dsp_model_position_report(model, window, length, now);
    }
    cdj_dsp_model_census(model, window, length, now);
    if (model->probe_interval_ns && model->running && !model->absent
        && model->probe_next < model->probe_count
        && now >= model->probe_start_ns
        && now - model->probe_last_ns >= model->probe_interval_ns) {
        model->probe_last_ns = now;
        cdj_dsp_model_post(model, window, length,
                           model->probe_list[model->probe_next++], now);
    }
    if (model->slot_state == 3 && model->slot_period_ns && model->running
        && now - model->slot_last_ns >= model->slot_period_ns) {
        model->slot_pos_ms += (now - model->slot_last_ns) / 1000000;
        model->slot_last_ns = now;
        cdj_dsp_model_slot_report(model, window, length, 3, now);
    }
    if (model->status_flags && window && length >= 0x81b0) {
        bool playing = model->slot_state == 3;

        if (playing != model->status_flags_set) {
            uint32_t b1 = ldl_le_p(window + 0x81ac), b2 = ldl_le_p(window + 0x818c);

            stl_le_p(window + 0x81ac, playing ? (b1 | (1u << 24)) : (b1 & ~(1u << 24)));
            stl_le_p(window + 0x818c, playing ? (b2 | (1u << 25)) : (b2 & ~(1u << 25)));
            model->status_flags_set = playing;
            fprintf(stderr, "cdj2000-dsp: buffer status flags %s (+0x81ac bit 24, "
                    "+0x818c bit 25) t=%.3f\n", playing ? "set" : "cleared", now / 1e9);
        }
    }
    if (model->consume_rate > 0 && model->slot_state == 3 && elapsed_ms > 0
        && window && length >= 0x81a8) {
        int64_t units;

        model->consume_carry_ms += elapsed_ms;
        units = model->consume_carry_ms * model->consume_rate / 1000;
        if (units > 0) {
            int32_t level = (int32_t)ldl_le_p(window + DSP_LEVEL_BUFFER1);

            model->consume_carry_ms -= units * 1000 / model->consume_rate;
            level = level > units ? level - units : 0;
            stl_le_p(window + DSP_LEVEL_BUFFER1, level);
            stl_le_p(window + 0x81a0, ldl_le_p(window + 0x81a0) + units);
            model->consumed += units;
            if (model->refill_event && level < model->refill_low
                && !model->refill_asked) {
                model->refill_asked = true;
                fprintf(stderr, "cdj2000-dsp: buffer 1 at %d, asking with event "
                        "0x%x t=%.3f\n", level, model->refill_event, now / 1e9);
                cdj_dsp_model_post(model, window, length, model->refill_event, now);
            } else if (level >= model->refill_low) {
                model->refill_asked = false;
            }
            if (now - model->consume_report_ns >= 5 * 1000000000LL) {
                model->consume_report_ns = now;
                fprintf(stderr, "cdj2000-dsp: consumed %" PRId64 " units so far; "
                        "levels +0x7cd0=%u +0x7ccc=%u, status +0x81a0=%u t=%.3f\n",
                        model->consumed, ldl_le_p(window + DSP_LEVEL_BUFFER1),
                        ldl_le_p(window + DSP_LEVEL_BUFFER2),
                        ldl_le_p(window + 0x81a0), now / 1e9);
            }
        }
    }
    if (model->transport != CDJ_DSP_PLAYING || elapsed_ms <= 0) {
        return;
    }
    /* Tempo is a parts-per-million offset from nominal speed. */
    model->position_ms += elapsed_ms
        + (elapsed_ms * model->tempo_ppm) / 1000000;
}
