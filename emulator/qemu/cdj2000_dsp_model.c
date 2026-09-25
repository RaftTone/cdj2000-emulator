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

/* Where MAIN's second firmware record lands, i.e. the shared control block. */
#define DSP_CONTROL_OFFSET   0x7800
/* The two fill levels MAIN reads and the count word of a stream header. */
#define DSP_LEVEL_BUFFER1   0x7cd0
#define DSP_LEVEL_BUFFER2   0x7ccc
#define DSP_HEADER_COUNT    0x8144
#define DSP_HOT_SLOTS       10      /* +0x7c80 slots: 0 the cue, 1.. hot cue A.., +5 for 0x12/0x22 */

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
    int64_t pos_print_ns;
    bool pos_standby;                   /* +0x7ba0 = 4 (cue standby): 0x21 does not run */
    int64_t loop_in_ms;                 /* segment slot 1: command 1 (IN) and 2 (OUT) */
    int64_t loop_out_ms;
    bool loop_on;                       /* both points set; 0xc (flush) clears */
    uint32_t pos_record;                /* +0x8120 of the last +0x8100 command: the record id the report names */
    uint32_t fmt_record;                /* +0x8120 of the last format command, any kind */
    bool fmt_seen;
    bool flush_reset;                   /* +0x7cb0 = 1/2 resets as 0x80034c08 does */
    bool job_answer;                    /* +0x7ba4 = 1 (a job) answered with 0 */
    bool job_consume;                   /* CDJ_DSP_JOB_CONSUME: the job empties buffer 1's level */
    bool job_advance;                   /* CDJ_DSP_JOB_ADVANCE: ... and moves the position by it */
    bool events;                        /* CDJ_DSP_EVENTS: post event 5 as the DSP does */
    bool status_record;                 /* CDJ_DSP_STATUS_RECORD: the buffers' record bytes */
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
    for (unsigned i = 0; i < DSP_HOT_SLOTS; i++) {
        model->hot_ms[i] = -1;
    }
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
 * The position units are a guess to be measured against the time display:
 * +96 is divided by 294 at 0x1a1174 and 294 * 75 = 22050, so it is taken as
 * 22050ths of a second; +100 is doubled at 0x1be946 and is written as CD
 * sectors (75 a second); +104 stays 0.
 */
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
        if (length >= 0x81c0) {
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
        int64_t elapsed = (now - model->pos_last_ns) / SCALE_MS;

        if (rate < 0x20000 || rate > 0x300000) {
            rate = 0x100000;            /* nothing sensible there: nominal */
        }
        model->pos_ms += elapsed * rate >> 20;
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
    stl_le_p(window + DSP_POS_VALID, model->pos_state ? model->pos_record : 0);
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

void cdj_dsp_model_tick(CdjDspModel *model, uint8_t *window, size_t length)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed_ms = (now - model->last_tick_ns) / SCALE_MS;

    model->last_tick_ns = now;
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

                if (slot == 0) {
                    model->pos_ms = model->cue_ms;
                    what = " (the cue)";
                } else if (slot >= DSP_HOT_SLOTS) {
                    what = " (no such slot, ignored)";
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
                if (jump && slot < DSP_HOT_SLOTS
                    && (slot == 0 || model->hot_ms[slot] >= 0)) {
                    model->pos_state = model->pos_standby ? 2 : 3;
                }
                fprintf(stderr, "cdj2000-dsp: +0x7c80 = 0x%x slot %u%s -> position state %u "
                        "at %" PRId64 " ms%s t=%.3f\n", word, slot, what,
                        model->pos_state, model->pos_ms,
                        model->pos_standby ? " (standby)" : "", now / 1e9);
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

                if ((word & 0xf) == 1 && seg_slot >= 1 && seg_slot < DSP_HOT_SLOTS) {
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
            if (req->offset == 0x7ba0 && model->pos_report && word >= 2 && word <= 6) {
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
