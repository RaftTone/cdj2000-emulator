/*
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/*
 * Runtime panel input -- a control channel into the running machine.
 *
 * Everything that decides *what* is pressed lives here rather than in
 * cdj2000_main.c: the board's job is to emit a well-formed panel reply, and it
 * should not also grow a control protocol.  cdj_panel_frame() calls
 * cdj_input_apply() once per panel exchange, after the CDJ_PANEL_KEYS schedule
 * and before the checksum, so anything written here still validates.
 *
 * Two things this has to get right, both learned the expensive way:
 *
 *  - A click is a pulse, not a state.  The handlers are rising-edge detectors:
 *    0x28ddc8 compares each status byte against the copy 44 bytes further on
 *    and acts only on a bit that is set now and was clear before.  A bit held
 *    down for ever is the same as a bit never pressed.  So a press is queued,
 *    driven down for a while, driven back up for at least as long, and only
 *    then is the next one started -- two clicks in quick succession must not
 *    merge into one edge.
 *
 *  - A run with this file compiled in must be indistinguishable from a run
 *    without it unless somebody asked for a control channel.  The socket is
 *    therefore opened only when CDJ_INPUT_PORT names a port; with the variable
 *    unset nothing binds, nothing is polled beyond one getenv, and the panel
 *    reports no buttons down.  Control runs stay control runs.
 *
 * The protocol is one ASCII command per line over TCP on 127.0.0.1, answered
 * with "ok ..." or "err ...".  Line-oriented on purpose: it can be driven from
 * panel_control.py, from a test, or by hand.
 *
 *   ping                      -> ok pong
 *   press <byte> <mask> [ms]  queue one down/up pulse, mask is hex
 *   down <byte> <mask>        hold bits down until "up" (for chords and holds)
 *   up <byte> <mask>          release them
 *   level <byte> <mask> <0|1> persistently force literal contact levels
 *   analog <field> <value>    set analogue field 0..7 outright (7 = the encoder)
 *   rotary <field> <delta>    move it by <delta>, one step per panel frame
 *   step <n>                  steps per frame for the rotary ramp (default 1)
 *   hold <ms> / gap <ms>      default press hold and the quiet time after it
 *   clear                     release bits, level overrides and analogue fields
 *   state                     report held bits, analogue fields, queue depth
 *   sd-lid open|closed|toggle|state  persistent NXS lid contact, not a key
 *
 * CDJ_NXS_SD_LID=open|closed opts into this contact even without a socket.
 * Unset preserves legacy raw frames. Once enabled it owns byte17 bit2;
 * raw button pulses/holds and clear cannot change the physical lid state.
 *
 * Numbers are decimal unless prefixed 0x, except <mask>, which is always hex
 * because that is how CDJ_PANEL_KEYS spells it and how the manifest lists it.
 *
 * See INPUT_MANIFEST.md for the full list of decoded inputs.
 */
#include "qemu/osdep.h"

#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"

#include "cdj2000_input.h"

/* The payload the panel checksums: 22 bytes, without checksum or marker. */
#define CDJ_INPUT_PAYLOAD_MAX 22

/*
 * A press has to survive at least this many panel exchanges in each half, on
 * top of its wall-clock hold.  Virtual time alone is not enough: if the guest
 * stops polling the panel for a while, a hold measured only in nanoseconds can
 * expire between two exchanges and the bit is never once transmitted.  Two
 * frames also means the status byte and its copy 44 bytes on have both been
 * refreshed, which is what the edge detector compares.
 */
#define CDJ_INPUT_MIN_FRAMES 2

#define CDJ_INPUT_QUEUE 32
#define CDJ_INPUT_LINE 512

typedef struct CdjInputPress {
    unsigned byte;
    uint8_t mask;
    int64_t hold_ns;
} CdjInputPress;

typedef enum CdjInputPhase {
    CDJ_INPUT_IDLE,
    CDJ_INPUT_DOWN,
    CDJ_INPUT_UP,
} CdjInputPhase;

/*
 * Payload bytes 2..13, as the decoder at 0x28e1d6 splits them: two 8-bit
 * fields, then five 16-bit big-endian pairs -- and byte 14, which the same
 * decoder reads as a level too and which MAIN's own panel simulator steps by
 * +1 and -1 from two adjacent arms.  See cdj2000_input.h for the disassembly.
 */
static const struct {
    uint8_t byte;
    uint8_t width;
} cdj_input_analog[CDJ_INPUT_ANALOG_FIELDS] = {
    { 2, 1 }, { 3, 1 }, { 4, 2 }, { 6, 2 }, { 8, 2 }, { 10, 2 }, { 12, 2 },
    { 14, 1 },
};

static int cdj_input_listen_fd = -1;
static int cdj_input_client_fd = -1;
static bool cdj_input_opened;

static char cdj_input_line[CDJ_INPUT_LINE];
static unsigned cdj_input_fill;
static bool cdj_input_overlong;

static CdjInputPress cdj_input_queue[CDJ_INPUT_QUEUE];
static unsigned cdj_input_queue_head;
static unsigned cdj_input_queue_len;

static CdjInputPhase cdj_input_phase = CDJ_INPUT_IDLE;
static CdjInputPress cdj_input_active;
static int64_t cdj_input_phase_since;
static unsigned cdj_input_phase_frames;

static uint8_t cdj_input_held[CDJ_INPUT_PAYLOAD_MAX];
/* Literal levels override the base frame and other runtime inputs. */
static uint8_t cdj_input_level_mask[CDJ_INPUT_PAYLOAD_MAX];
static uint8_t cdj_input_level_value[CDJ_INPUT_PAYLOAD_MAX];
/* NXS lid is a level contact, not a key. -1 preserves legacy raw input. */
static int cdj_input_sd_lid = -1; /* 0=open, 1=closed */
static bool cdj_input_lid_initialized;

static bool cdj_input_analog_driven[CDJ_INPUT_ANALOG_FIELDS];
static int32_t cdj_input_analog_value[CDJ_INPUT_ANALOG_FIELDS];
static int32_t cdj_input_analog_target[CDJ_INPUT_ANALOG_FIELDS];
static int32_t cdj_input_analog_step = 1;

static int64_t cdj_input_hold_ns = 300 * 1000000LL;
static int64_t cdj_input_gap_ns = 300 * 1000000LL;

static uint64_t cdj_input_frames;
static uint64_t cdj_input_commands;

unsigned cdj_input_analog_byte(unsigned field)
{
    return field < CDJ_INPUT_ANALOG_FIELDS ? cdj_input_analog[field].byte : 0;
}

unsigned cdj_input_analog_width(unsigned field)
{
    return field < CDJ_INPUT_ANALOG_FIELDS ? cdj_input_analog[field].width : 0;
}

/* ------------------------------------------------------------------ socket */

static void cdj_input_reply(const char *text)
{
    size_t left;
    size_t done = 0;

    if (cdj_input_client_fd < 0) {
        return;
    }
    left = strlen(text);
    while (done < left) {
        ssize_t wrote = send(cdj_input_client_fd, text + done,
                             (int)(left - done), 0);

        if (wrote > 0) {
            done += wrote;
            continue;
        }
        /*
         * A client that does not read is not worth stalling the machine for --
         * the answers are a convenience, the presses are the point.
         */
        break;
    }
}

/*
 * Say why, every time.  r089 lost the channel between a command at t=20 and one
 * at t=150, and the run could not tell whether the key did nothing or never
 * arrived -- because this function used to be silent.  A dropped connection is
 * rare enough that logging every one costs nothing and turns the next
 * occurrence into a measurement instead of a guess.
 */
static void cdj_input_drop_client(const char *why)
{
    if (cdj_input_client_fd >= 0) {
        info_report("cdj2000-input: client dropped after %" PRIu64
                    " commands: %s", cdj_input_commands, why);
        close(cdj_input_client_fd);
        cdj_input_client_fd = -1;
    }
    cdj_input_fill = 0;
    cdj_input_overlong = false;
}

static void cdj_input_open(void)
{
    const char *spec = getenv("CDJ_INPUT_PORT");
    struct sockaddr_in address;
    long port;
    char *end;
    int fd;

    cdj_input_opened = true;
    if (!spec || !*spec) {
        return;
    }
    port = strtol(spec, &end, 0);
    if (end == spec || port <= 0 || port > 65535) {
        warn_report("cdj2000-input: CDJ_INPUT_PORT=%s is not a port", spec);
        return;
    }

    fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        warn_report("cdj2000-input: socket: %s", strerror(errno));
        return;
    }
    socket_set_fast_reuse(fd);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0
        || listen(fd, 1) < 0) {
        warn_report("cdj2000-input: cannot listen on 127.0.0.1:%ld: %s",
                    port, strerror(errno));
        close(fd);
        return;
    }
    qemu_set_blocking(fd, false, NULL);
    cdj_input_listen_fd = fd;
    info_report("cdj2000-input: control channel on 127.0.0.1:%ld", port);
}

/* ---------------------------------------------------------------- commands */

/*
 * Split a line into at most `max` words in place.  strtok_r is a portability
 * question on the Windows toolchain this is built with and the job is four
 * fields long, so it is not worth asking.
 */
static void cdj_input_split(char *line, char **word, unsigned max)
{
    unsigned found = 0;
    char *cursor = line;

    while (found < max) {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (!*cursor) {
            return;
        }
        word[found++] = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        if (*cursor) {
            *cursor++ = 0;
        }
    }
}

static bool cdj_input_number(const char *text, long *out)
{
    char *end;
    long value;

    if (!text || !*text) {
        return false;
    }
    value = strtol(text, &end, 0);
    if (end == text || *end) {
        return false;
    }
    *out = value;
    return true;
}

static bool cdj_input_hex(const char *text, long *out)
{
    char *end;
    long value;

    if (!text || !*text) {
        return false;
    }
    value = strtol(text, &end, 16);
    if (end == text || *end || value < 0 || value > 0xff) {
        return false;
    }
    *out = value;
    return true;
}

static void cdj_input_report_state(void)
{
    GString *text = g_string_new(NULL);
    unsigned i;

    g_string_append_printf(text, "ok state frames=%" PRIu64 " commands=%"
                           PRIu64 " queue=%u phase=%s held=", cdj_input_frames,
                           cdj_input_commands, cdj_input_queue_len,
                           cdj_input_phase == CDJ_INPUT_DOWN ? "down"
                           : cdj_input_phase == CDJ_INPUT_UP ? "up" : "idle");
    for (i = 0; i < CDJ_INPUT_PAYLOAD_MAX; i++) {
        g_string_append_printf(text, "%02x", cdj_input_held[i]);
    }
    g_string_append_printf(text, " level_mask=");
    for (i = 0; i < CDJ_INPUT_PAYLOAD_MAX; i++) {
        g_string_append_printf(text, "%02x", cdj_input_level_mask[i]);
    }
    g_string_append_printf(text, " level_value=");
    for (i = 0; i < CDJ_INPUT_PAYLOAD_MAX; i++) {
        g_string_append_printf(text, "%02x", cdj_input_level_value[i]);
    }
    for (i = 0; i < CDJ_INPUT_ANALOG_FIELDS; i++) {
        g_string_append_printf(text, " a%u=%s%d/%d", i,
                               cdj_input_analog_driven[i] ? "" : "-",
                               cdj_input_analog_value[i],
                               cdj_input_analog_target[i]);
    }
    g_string_append_printf(text, " sd_lid=%s\n", cdj_input_sd_lid < 0 ? "raw" :
                           cdj_input_sd_lid ? "closed" : "open");
    cdj_input_reply(text->str);
    g_string_free(text, TRUE);
}

static void cdj_input_clear(void)
{
    unsigned i;

    memset(cdj_input_held, 0, sizeof(cdj_input_held));
    memset(cdj_input_level_mask, 0, sizeof(cdj_input_level_mask));
    memset(cdj_input_level_value, 0, sizeof(cdj_input_level_value));
    cdj_input_queue_head = 0;
    cdj_input_queue_len = 0;
    for (i = 0; i < CDJ_INPUT_ANALOG_FIELDS; i++) {
        cdj_input_analog_driven[i] = false;
        cdj_input_analog_value[i] = 0;
        cdj_input_analog_target[i] = 0;
    }
}

/*
 * One command line, already stripped of its terminator.  Returns nothing: the
 * answer goes straight back down the socket, because the caller has no use for
 * it and a queued reply is one more thing to get wrong.
 */
static void cdj_input_command(char *line)
{
    char *word[5] = { NULL, NULL, NULL, NULL, NULL };
    const char *verb;
    const char *arg1;
    const char *arg2;
    const char *arg3;
    long first = 0;
    long second = 0;
    long third = 0;

    cdj_input_split(line, word, 5);
    verb = word[0];
    arg1 = word[1];
    arg2 = word[2];
    arg3 = word[3];
    if (!verb) {
        return;
    }
    cdj_input_commands++;

    if (!strcmp(verb, "ping")) {
        cdj_input_reply("ok pong\n");
        return;
    }
    if (!strcmp(verb, "state")) {
        cdj_input_report_state();
        return;
    }
    if (!strcmp(verb, "sd-lid")) {
        if (!arg1 || arg2 ||
            (strcmp(arg1, "open") && strcmp(arg1, "closed") &&
             strcmp(arg1, "toggle") && strcmp(arg1, "state"))) {
            cdj_input_reply("err sd-lid <open|closed|toggle|state>\n");
            return;
        }
        if (!strcmp(arg1, "open")) cdj_input_sd_lid = 0;
        if (!strcmp(arg1, "closed")) cdj_input_sd_lid = 1;
        if (!strcmp(arg1, "toggle")) {
            if (cdj_input_sd_lid < 0) {
                cdj_input_reply("err SD lid not configured\n");
                return;
            }
            cdj_input_sd_lid ^= 1;
        }
        cdj_input_reply(cdj_input_sd_lid < 0 ? "ok sd-lid raw\n" :
                        cdj_input_sd_lid ? "ok sd-lid closed\n" :
                                           "ok sd-lid open\n");
        return;
    }
    if (!strcmp(verb, "clear")) {
        cdj_input_clear();
        info_report("cdj2000-input: cleared");
        cdj_input_reply("ok clear\n");
        return;
    }
    if (!strcmp(verb, "level")) {
        if (!cdj_input_number(arg1, &first)
            || first < 0 || first >= CDJ_INPUT_PAYLOAD_MAX
            || !cdj_input_hex(arg2, &second) || second == 0
            || !arg3 || (strcmp(arg3, "0") && strcmp(arg3, "1"))
            || word[4]) {
            cdj_input_reply("err level <byte 0..21> <mask hex 01..ff> <0|1>\n");
            return;
        }
        cdj_input_level_mask[first] |= (uint8_t)second;
        if (arg3[0] == '1') {
            cdj_input_level_value[first] |= (uint8_t)second;
        } else {
            cdj_input_level_value[first] &= (uint8_t)~second;
        }
        info_report("cdj2000-input: byte %ld mask %#lx level %s", first,
                    second, arg3);
        cdj_input_reply("ok level\n");
        return;
    }
    if (!strcmp(verb, "step")) {
        if (!cdj_input_number(arg1, &first) || first < 1 || first > 4096) {
            cdj_input_reply("err step <1..4096>\n");
            return;
        }
        cdj_input_analog_step = (int32_t)first;
        cdj_input_reply("ok step\n");
        return;
    }
    if (!strcmp(verb, "hold") || !strcmp(verb, "gap")) {
        if (!cdj_input_number(arg1, &first) || first < 0 || first > 60000) {
            cdj_input_reply("err <0..60000> ms\n");
            return;
        }
        if (verb[0] == 'h') {
            cdj_input_hold_ns = first * 1000000LL;
        } else {
            cdj_input_gap_ns = first * 1000000LL;
        }
        cdj_input_reply("ok\n");
        return;
    }
    if (!strcmp(verb, "press") || !strcmp(verb, "down")
        || !strcmp(verb, "up")) {
        if (!cdj_input_number(arg1, &first)
            || first < 0 || first >= CDJ_INPUT_PAYLOAD_MAX
            || !cdj_input_hex(arg2, &second) || second == 0) {
            cdj_input_reply("err <byte 0..21> <mask hex 01..ff>\n");
            return;
        }
        if (!strcmp(verb, "down")) {
            cdj_input_held[first] |= (uint8_t)second;
            info_report("cdj2000-input: byte %ld mask %#lx down", first, second);
            cdj_input_reply("ok down\n");
            return;
        }
        if (!strcmp(verb, "up")) {
            cdj_input_held[first] &= (uint8_t)~second;
            info_report("cdj2000-input: byte %ld mask %#lx up", first, second);
            cdj_input_reply("ok up\n");
            return;
        }
        if (arg3 && (!cdj_input_number(arg3, &third) || third < 0
                     || third > 60000)) {
            cdj_input_reply("err press <byte> <mask> [ms 0..60000]\n");
            return;
        }
        if (cdj_input_queue_len >= CDJ_INPUT_QUEUE) {
            cdj_input_reply("err queue full\n");
            return;
        }
        {
            unsigned slot = (cdj_input_queue_head + cdj_input_queue_len)
                            % CDJ_INPUT_QUEUE;

            cdj_input_queue[slot].byte = (unsigned)first;
            cdj_input_queue[slot].mask = (uint8_t)second;
            cdj_input_queue[slot].hold_ns = arg3 ? third * 1000000LL
                                                 : cdj_input_hold_ns;
            cdj_input_queue_len++;
        }
        info_report("cdj2000-input: queued byte %ld mask %#lx (%u waiting)",
                    first, second, cdj_input_queue_len);
        cdj_input_reply("ok press\n");
        return;
    }
    if (!strcmp(verb, "analog") || !strcmp(verb, "rotary")) {
        if (!cdj_input_number(arg1, &first)
            || first < 0 || first >= CDJ_INPUT_ANALOG_FIELDS
            || !cdj_input_number(arg2, &second)) {
            cdj_input_reply("err <field 0..7> <value>\n");
            return;
        }
        if (verb[0] == 'a') {
            cdj_input_analog_value[first] = (int32_t)second;
            cdj_input_analog_target[first] = (int32_t)second;
        } else {
            cdj_input_analog_target[first] += (int32_t)second;
        }
        cdj_input_analog_driven[first] = true;
        info_report("cdj2000-input: analogue field %ld -> %d (now %d)", first,
                    cdj_input_analog_target[first],
                    cdj_input_analog_value[first]);
        cdj_input_reply("ok analog\n");
        return;
    }
    cdj_input_reply("err unknown command\n");
}

/* ------------------------------------------------------------------- poll */

static void cdj_input_consume(void)
{
    unsigned start = 0;
    unsigned i;

    for (i = 0; i < cdj_input_fill; i++) {
        char c = cdj_input_line[i];

        if (c != '\n' && c != '\r') {
            continue;
        }
        cdj_input_line[i] = 0;
        if (cdj_input_overlong) {
            /* The tail of a line already refused; do not act on half of it. */
            cdj_input_overlong = false;
        } else if (i > start) {
            cdj_input_command(cdj_input_line + start);
        }
        start = i + 1;
    }
    if (start) {
        memmove(cdj_input_line, cdj_input_line + start, cdj_input_fill - start);
        cdj_input_fill -= start;
    }
    if (cdj_input_fill + 1 >= sizeof(cdj_input_line)) {
        /*
         * No terminator in a full buffer: refuse the line rather than execute
         * an arbitrary prefix of it, and swallow the rest until the newline.
         */
        cdj_input_fill = 0;
        cdj_input_overlong = true;
        cdj_input_reply("err line too long\n");
    }
}

/*
 * Take whoever is knocking, even when a client is already attached.
 *
 * This is the lesson of r089.  The channel died between a command at t=20 and
 * one at t=150 -- from the guest, with a cause that does not reproduce on the
 * host over the same number of exchanges -- and because accept() was only
 * reached while no client was attached, nothing could ever get back in.  One
 * lost connection ended input for the rest of the run, and every queued
 * measurement after it would have read as "the key does nothing".
 *
 * Newest wins, and the old one is closed rather than left dangling.  A control
 * channel that cannot be reconnected to is a design defect on its own, whatever
 * closed it.
 */
/*
 * QEMU's Windows socket wrappers have one path that returns -1 **without
 * touching errno**: qemu_accept_wrap and qemu_recv_wrap both begin
 *
 *     SOCKET s = _get_osfhandle(sockfd);
 *     if (s == INVALID_SOCKET) {
 *         return -1;
 *     }
 *
 * so when the descriptor is no longer a socket the caller sees a failure
 * carrying whatever errno was left behind by something else -- and the most
 * recent value here is almost always EAGAIN, which reads as "nothing pending".
 * That is a blind spot rather than an explanation: it is how this file could
 * lose its channel and log nothing, which is exactly what r094 showed (the host
 * saw a reset, the guest never recorded a drop).
 *
 * Clearing errno first makes the two cases distinguishable at no cost: a
 * negative return with errno still 0 is the wrapper giving up before the
 * syscall.  This does not say what closed anything; it says the code will
 * notice next time instead of going quiet.
 */
static bool cdj_input_would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void cdj_input_reopen(const char *why)
{
    warn_report("cdj2000-input: %s -- reopening the control channel", why);
    if (cdj_input_client_fd >= 0) {
        close(cdj_input_client_fd);
        cdj_input_client_fd = -1;
    }
    if (cdj_input_listen_fd >= 0) {
        close(cdj_input_listen_fd);
        cdj_input_listen_fd = -1;
    }
    cdj_input_fill = 0;
    cdj_input_overlong = false;
    /* cdj_input_open() binds again on the next exchange; the listening socket
     * is opened with SO_REUSEADDR, so the port is not held against us. */
    cdj_input_opened = false;
}

static void cdj_input_accept(void)
{
    int fd;

    errno = 0;
    fd = accept(cdj_input_listen_fd, NULL, NULL);
    if (fd < 0) {
        if (cdj_input_would_block() || errno == ECONNABORTED) {
            return;             /* nobody knocking, which is the normal case */
        }
        if (errno == 0) {
            cdj_input_reopen("the listening socket is no longer a socket");
        } else {
            warn_report("cdj2000-input: accept failed, errno %d", errno);
        }
        return;
    }
    if (cdj_input_client_fd >= 0) {
        cdj_input_drop_client("replaced by a new connection");
    }
    qemu_set_blocking(fd, false, NULL);
    socket_set_nodelay(fd);
    cdj_input_client_fd = fd;
    cdj_input_fill = 0;
    cdj_input_overlong = false;
    cdj_input_reply("ok cdj2000-input\n");
}

static void cdj_input_poll(void)
{
    if (!cdj_input_opened) {
        cdj_input_open();
    }
    if (cdj_input_listen_fd < 0) {
        return;
    }
    cdj_input_accept();
    while (cdj_input_client_fd >= 0) {
        size_t space = sizeof(cdj_input_line) - cdj_input_fill - 1;
        ssize_t got;

        if (space == 0) {
            /*
             * recv() with a zero-length buffer returns 0, which is also how a
             * closed peer reports itself -- so asking for nothing would look
             * exactly like a hang-up and take the connection down.
             */
            cdj_input_fill = 0;
            cdj_input_overlong = true;
            cdj_input_reply("err line too long\n");
            continue;
        }
        errno = 0;
        got = recv(cdj_input_client_fd, cdj_input_line + cdj_input_fill,
                   (int)space, 0);
        if (got > 0) {
            cdj_input_fill += got;
            cdj_input_consume();
            continue;
        }
        if (got == 0) {
            cdj_input_drop_client("peer closed the connection");
            return;
        }
        if (cdj_input_would_block()) {
            return;         /* nothing pending, which is the normal case */
        }
        if (errno == 0) {
            /* See cdj_input_accept: the wrapper gave up before the syscall,
             * so this descriptor is not a socket any more. */
            cdj_input_drop_client("the client socket is no longer a socket");
            return;
        }
        /*
         * Anything else is a real socket error.  Drop it -- but the accept
         * above means a client can always come back, so this is no longer the
         * end of input for the run.
         */
        {
            char reason[64];

            snprintf(reason, sizeof(reason), "recv failed, errno %d", errno);
            cdj_input_drop_client(reason);
        }
        return;
    }
}

/* ------------------------------------------------------------------ frames */

static void cdj_input_run_press(uint8_t *payload, unsigned len, int64_t now)
{
    if (cdj_input_phase == CDJ_INPUT_IDLE && cdj_input_queue_len) {
        cdj_input_active = cdj_input_queue[cdj_input_queue_head];
        cdj_input_queue_head = (cdj_input_queue_head + 1) % CDJ_INPUT_QUEUE;
        cdj_input_queue_len--;
        cdj_input_phase = CDJ_INPUT_DOWN;
        cdj_input_phase_since = now;
        cdj_input_phase_frames = 0;
        info_report("cdj2000-input: byte %u mask %#x down at %.3f s "
                    "(frame %" PRIu64 ")", cdj_input_active.byte,
                    (unsigned)cdj_input_active.mask, now / 1e9,
                    cdj_input_frames);
    }

    if (cdj_input_phase == CDJ_INPUT_DOWN) {
        if (cdj_input_active.byte < len) {
            payload[cdj_input_active.byte] |= cdj_input_active.mask;
        }
        cdj_input_phase_frames++;
        if (cdj_input_phase_frames >= CDJ_INPUT_MIN_FRAMES
            && now - cdj_input_phase_since >= cdj_input_active.hold_ns) {
            cdj_input_phase = CDJ_INPUT_UP;
            cdj_input_phase_since = now;
            cdj_input_phase_frames = 0;
            info_report("cdj2000-input: byte %u mask %#x up at %.3f s",
                        cdj_input_active.byte,
                        (unsigned)cdj_input_active.mask, now / 1e9);
        }
    } else if (cdj_input_phase == CDJ_INPUT_UP) {
        cdj_input_phase_frames++;
        if (cdj_input_phase_frames >= CDJ_INPUT_MIN_FRAMES
            && now - cdj_input_phase_since >= cdj_input_gap_ns) {
            cdj_input_phase = CDJ_INPUT_IDLE;
        }
    }
}

/*
 * The rotary, one step per panel frame.  An encoder that jumps twelve counts
 * between two frames is not what the firmware sees on real hardware, and the
 * whole reason the manifest lists the rotary as undrivable is that
 * CDJ_PANEL_KEYS can only OR button bits -- it cannot ramp a value over time.
 * `rotary` sets a target and this walks towards it; `analog` sets both at once
 * for the cases where a jump is what is wanted.
 *
 * **Every step that moves prints its own timestamp, and that is not decoration.**
 * A press can be proven to have arrived from the GUI's key dispatcher line; an
 * analogue field cannot -- it never reaches that dispatcher -- so its arrival is
 * only visible as a write to its destination in MAIN's status block, which is
 * what CDJ_WATCH reports.  But `cdj2000-watch:` lines carry **no time of their
 * own** (checked against every archived stream on disk), so a watched write can
 * only be placed in a window by the last timestamped line in front of it.  In
 * `plan coverage` the last press is at t1275 and the first analogue window at
 * t1250: a clock carried from presses alone would be up to three minutes stale
 * exactly where it is needed, and every analogue window would be datable only by
 * assumption.  One line per moved step costs 120 lines over the whole coverage
 * plan and makes the window real.
 */
static void cdj_input_run_analog(uint8_t *payload, unsigned len, int64_t now)
{
    unsigned field;

    for (field = 0; field < CDJ_INPUT_ANALOG_FIELDS; field++) {
        unsigned byte = cdj_input_analog[field].byte;
        unsigned width = cdj_input_analog[field].width;
        int32_t value;

        if (!cdj_input_analog_driven[field]) {
            continue;
        }
        value = cdj_input_analog_value[field];
        if (value != cdj_input_analog_target[field]) {
            int32_t delta = cdj_input_analog_target[field] - value;

            if (delta > cdj_input_analog_step) {
                delta = cdj_input_analog_step;
            } else if (delta < -cdj_input_analog_step) {
                delta = -cdj_input_analog_step;
            }
            value += delta;
            cdj_input_analog_value[field] = value;
            info_report("cdj2000-input: analogue field %u = %d at %.3f s "
                        "(frame %" PRIu64 ")", field, value, now / 1e9,
                        cdj_input_frames);
        }
        if (width == 1) {
            if (byte < len) {
                payload[byte] = (uint8_t)(value & 0xff);
            }
        } else if (byte + 1 < len) {
            /* Big-endian, as 0x28e1d6 reassembles the pair. */
            payload[byte] = (uint8_t)((value >> 8) & 0xff);
            payload[byte + 1] = (uint8_t)(value & 0xff);
        }
    }
}

void cdj_input_apply(uint8_t *payload, unsigned len)
{
    int64_t now;
    unsigned i;

    if (!cdj_input_lid_initialized) {
        const char *lid = getenv("CDJ_NXS_SD_LID");
        cdj_input_lid_initialized = true;
        if (lid) {
            if (strcmp(lid, "closed") && strcmp(lid, "open")) {
                info_report("CDJ_NXS_SD_LID must be open or closed");
                exit(1);
            }
            cdj_input_sd_lid = !strcmp(lid, "closed");
        }
    }
    cdj_input_poll();
    /* Applied even without a socket; clear/focus loss cannot open a lid.
     * NXS 042f5810 inverts byte17 bit2 into the SD-open firmware gate. */
    if (len > 17 && cdj_input_sd_lid >= 0) {
        payload[17] = (payload[17] & ~4u) | (cdj_input_sd_lid ? 4u : 0u);
    }
    if (cdj_input_listen_fd < 0 || !len) {
        return;
    }
    cdj_input_frames++;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (i = 0; i < len && i < CDJ_INPUT_PAYLOAD_MAX; i++) {
        payload[i] |= cdj_input_held[i];
    }
    cdj_input_run_press(payload, len, now);
    cdj_input_run_analog(payload, len, now);
    for (i = 0; i < len && i < CDJ_INPUT_PAYLOAD_MAX; i++) {
        payload[i] = (payload[i] & ~cdj_input_level_mask[i])
                     | cdj_input_level_value[i];
    }
    if (len > 17 && cdj_input_sd_lid >= 0) {
        payload[17] = (payload[17] & ~4u) | (cdj_input_sd_lid ? 4u : 0u);
    }
}
