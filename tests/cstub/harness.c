/*
 * Drive emulator/qemu/cdj2000_input.c on the host and print what it does to the
 * panel payload, one line per panel exchange.
 *
 * This is the real file, compiled against the stub headers next to it, talking
 * over a real loopback socket to a client in this same process.  Only the
 * virtual clock is faked, because a test has to be able to step it.  So the
 * pulse machine, the rotary ramp and the command parser are *measured* here,
 * without an emulator and without spending one of strand A's run slots.
 *
 * Output, on stdout:
 *
 *     f <index> <virtual ns> <22 payload bytes as hex>
 *
 * plus lines beginning '#' for what the harness itself did.  The scenario is
 * argv[1]; tests/test_input_channel.py runs each one and reads the trace.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include <assert.h>

#include "cdj2000_input.c"

#define HARNESS_PAYLOAD 22
#define FRAME_NS (100 * 1000000LL)   /* 100 ms of virtual time per exchange */

int64_t cdj_stub_clock_ns;

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    (void)type;
    return cdj_stub_clock_ns;
}

void info_report(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    fputs("# info: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

void warn_report(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    fputs("# warn: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

bool qemu_set_blocking(int fd, bool block, void *errp)
{
    (void)errp;
#ifdef _WIN32
    unsigned long option = block ? 0 : 1;

    return ioctlsocket((SOCKET)fd, FIONBIO, &option) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return false;
    }
    flags = block ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

int socket_set_fast_reuse(int fd)
{
    int one = 1;

    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
                      sizeof(one));
}

int socket_set_nodelay(int fd)
{
    int one = 1;

    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                      sizeof(one));
}

/* ------------------------------------------------------------------------ */

static int harness_frame;
static int harness_client = -1;
static unsigned harness_replies;
static char harness_reply_line[4096];
static size_t harness_reply_fill;
static bool harness_defer_frame_replies;
static uint8_t harness_base_payload[HARNESS_PAYLOAD];

static void harness_sleep_ms(int milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec wait = { milliseconds / 1000,
                             (milliseconds % 1000) * 1000000L };
    nanosleep(&wait, NULL);
#endif
}

/*
 * Print whatever the board has answered so far, one '# reply' line each.
 *
 * The trace used to carry only what the payload did, which answers "did this
 * command take effect" but not "was this command understood".  For a verb that
 * moves no payload byte -- ping, state, clear -- those are the same trace, and
 * so are a refused command and a well-formed one that does nothing.  The reply
 * separates them, and it is the only way to check a line the harness did not
 * write itself.
 */
static void harness_reply_bytes(const char *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        char ch = bytes[i];

        if (ch == '\r' || ch == '\n') {
            if (harness_reply_fill) {
                harness_reply_line[harness_reply_fill] = 0;
                printf("# reply %s\n", harness_reply_line);
                if (strcmp(harness_reply_line, "ok cdj2000-input")) {
                    ++harness_replies;
                }
                harness_reply_fill = 0;
            }
        } else {
            if (harness_reply_fill == sizeof(harness_reply_line) - 1) {
                fprintf(stderr, "# harness: reply line too long\n");
                exit(2);
            }
            harness_reply_line[harness_reply_fill++] = ch;
        }
    }
}

static void harness_drain(void)
{
    char buffer[512];
    ssize_t got;

    if (harness_client < 0) {
        return;
    }
    while ((got = recv(harness_client, buffer, sizeof(buffer) - 1, 0)) > 0) {
        harness_reply_bytes(buffer, got);
    }
}

/* A simulated frame is not a TCP delivery deadline. Keep the next command's
 * marker behind this command's complete reply, without adding guest frames
 * or advancing virtual time. Greeting lines are not command acknowledgments. */
static void harness_wait_reply(unsigned expected)
{
    gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;

    while (harness_replies < expected) {
        cdj_input_poll();
        harness_drain();
        if (harness_replies >= expected) {
            return;
        }
        if (g_get_monotonic_time() >= deadline) {
            fprintf(stderr, "# harness: timed out waiting for reply %u\n",
                    expected);
            exit(2);
        }
        harness_sleep_ms(1);
    }
}

/*
 * One panel exchange, exactly as cdj_panel_frame() does it: a zeroed payload,
 * then the seam.  The payload starts at zero every time because the board
 * rebuilds the frame from scratch, which is what makes a held bit a decision
 * this file has to keep making.
 */
static void frame(void)
{
    uint8_t payload[HARNESS_PAYLOAD];
    int i;

    memcpy(payload, harness_base_payload, sizeof(payload));
    cdj_input_apply(payload, sizeof(payload));
    printf("f %d %" PRId64 " ", harness_frame++, cdj_stub_clock_ns);
    for (i = 0; i < HARNESS_PAYLOAD; i++) {
        printf("%02x", payload[i]);
    }
    printf("\n");
    if (!harness_defer_frame_replies) {
        harness_drain();
    }
    fflush(stdout);
    cdj_stub_clock_ns += FRAME_NS;
}

static void frames(int count)
{
    while (count-- > 0) {
        frame();
    }
}

static int harness_connect(int port)
{
    struct sockaddr_in address;
    int fd = socket(PF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    /* So harness_drain() can read what is there without waiting for what is
     * not.  The board never asks the client anything, so nothing here depends
     * on a blocking read. */
    qemu_set_blocking(fd, false, NULL);
    return fd;
}

/*
 * Send a command, allowing loopback delivery before the caller's panel frames.
 * This delay is not a reply deadline: script segments additionally wait for a
 * complete acknowledgment without advancing the simulated clock.
 */
static void command(const char *line)
{
    char buffer[256];

    printf("# command %s\n", line);
    snprintf(buffer, sizeof(buffer), "%s\n", line);
    send(harness_client, buffer, strlen(buffer), 0);
    harness_sleep_ms(20);
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "press";
    const char *port = getenv("CDJ_INPUT_PORT");
    const char *base = getenv("CDJ_TEST_PANEL_BASE");

    if (base) {
        assert(strlen(base) == 2 * HARNESS_PAYLOAD);
        for (unsigned i = 0; i < HARNESS_PAYLOAD; i++) {
            unsigned value;
            assert(sscanf(base + 2 * i, "%2x", &value) == 1);
            harness_base_payload[i] = value;
        }
    }

#ifdef _WIN32
    WSADATA started;
    WSAStartup(MAKEWORD(2, 2), &started);
#endif

    /* The first exchange is what opens the listening socket. */
    frame();

    if (port && *port) {
        harness_client = harness_connect(atoi(port));
        if (harness_client < 0) {
            fprintf(stderr, "# harness: cannot reach 127.0.0.1:%s\n", port);
            return 2;
        }
        printf("# connected\n");
        frame();               /* accept, and send the greeting */
    }

    if (!strcmp(scenario, "silent")) {
        /* CDJ_INPUT_PORT unset: nothing binds, nothing is merged. */
        frames(8);
    } else if (!strcmp(scenario, "press")) {
        command("press 19 02");
        frames(12);
    } else if (!strcmp(scenario, "press-short")) {
        /* A hold shorter than one exchange still has to survive two of them. */
        command("press 19 02 1");
        frames(8);
    } else if (!strcmp(scenario, "two-presses")) {
        command("press 19 02");
        command("press 18 01");
        frames(24);
    } else if (!strcmp(scenario, "hold")) {
        command("down 21 01");
        frames(4);
        command("up 21 01");
        frames(4);
    } else if (!strcmp(scenario, "analog")) {
        command("analog 0 90");
        command("analog 2 4660");
        frames(4);
    } else if (!strcmp(scenario, "rotary")) {
        command("analog 4 0");
        command("rotary 4 5");
        frames(10);
    } else if (!strcmp(scenario, "rotary-back")) {
        command("analog 2 3");
        command("rotary 2 -5");
        frames(10);
    } else if (!strcmp(scenario, "rotary-step")) {
        command("step 4");
        command("analog 4 0");
        command("rotary 4 12");
        frames(8);
    } else if (!strcmp(scenario, "clear")) {
        command("down 21 01");
        frames(2);
        command("analog 0 90");
        frames(2);
        command("clear");
        frames(3);
    } else if (!strcmp(scenario, "idle-after-connect")) {
        /*
         * r094's shape, and the one 'quiet' does not have.
         *
         * 'quiet' sends a command first and goes silent afterwards -- which is
         * r091, and r091 survived.  Here the silence comes *immediately after
         * connecting*, which is the only difference between the run that
         * worked and the run that lost every command.
         *
         * And it idles in **wall-clock seconds**, not in exchanges.  600
         * exchanges pass in milliseconds here, so anything that counts in
         * seconds -- a receive timeout, a keepalive, an idle limit anywhere in
         * the stack -- is invisible to a test that only counts iterations.
         * That is exactly the gap r094 fell through.
         *
         *     harness idle-after-connect [seconds]      (default 10)
         */
        int seconds = argc > 2 ? atoi(argv[2]) : 10;
        int64_t deadline = (int64_t)seconds * 1000;
        int64_t waited = 0;

        printf("# idling %d real seconds before the first command\n", seconds);
        fflush(stdout);
        while (waited < deadline) {
            /* The guest polls the panel throughout; ~300 exchanges a second. */
            int burst = 300;

            while (burst-- > 0) {
                frame();
            }
            harness_sleep_ms(1000);
            waited += 1000;
        }
        command("press 19 08");
        frames(8);
    } else if (!strcmp(scenario, "quiet")) {
        /*
         * The failure r089 measured: two commands are accepted, then nothing is
         * sent for a long time, and the third never arrives because the guest
         * has closed the connection.  Hundreds of exchanges with an idle socket
         * is the condition; if the connection does not survive them here, the
         * cause is in this file rather than in QEMU.
         */
        command("analog 0 90");
        frames(600);
        command("press 19 08");
        frames(8);
    } else if (!strcmp(scenario, "reconnect")) {
        /* A second client must be able to take over from a wedged first one. */
        command("analog 0 90");
        frames(2);
        close(harness_client);
        harness_client = harness_connect(atoi(port));
        printf("# reconnected\n");
        frames(2);
        command("press 19 08");
        frames(8);
    } else if (!strcmp(scenario, "abandoned")) {
        /*
         * r089: the guest closed the channel between two commands and the
         * second never arrived.  It does not reproduce here, so this is the
         * situation that has to be survivable whatever the cause -- the old
         * connection is still open as far as the server is concerned, and a
         * client that reconnects has to be able to take over anyway.  Without
         * that, one lost connection ends input for the rest of the run.
         */
        int abandoned;

        command("analog 0 90");
        frames(2);
        abandoned = harness_client;          /* left open, never used again */
        harness_client = harness_connect(atoi(port));
        printf("# second client\n");
        frames(2);
        command("press 19 08");
        frames(8);
        close(abandoned);
    } else if (!strcmp(scenario, "reply-timeout")) {
        harness_wait_reply(1);  /* no command was sent; must fail, not pass */
    } else if (!strcmp(scenario, "reply-fragments")) {
        harness_reply_bytes("ok po", 5);
        assert(harness_replies == 0);
        harness_reply_bytes("ng\nok cl", 8);
        assert(harness_replies == 1);
        harness_reply_bytes("ear\r\nok cdj2000-input\n", 22);
        assert(harness_replies == 2);
        assert(harness_reply_fill == 0);
    } else if (!strcmp(scenario, "script")
               || !strcmp(scenario, "script-deferred-replies")) {
        /*
         * Replay a file of commands, so that a caller can drive lines it did
         * not write into this harness -- specifically the ones the operator
         * window would put on the wire.  "Every control emits a line the board
         * accepts" is otherwise a claim about two files that never meet.
         *
         *     <command>     one protocol line, sent verbatim
         *     frames <n>    run n panel exchanges
         *     # ...         a comment, echoed so the reader can segment
         *
         * The output is the ordinary trace; the '# command' lines mark where
         * one segment ends and the next begins.
         */
        FILE *script = fopen(argc > 2 ? argv[2] : "", "r");
        char line[CDJ_INPUT_LINE];
        unsigned expected = harness_replies;

        harness_defer_frame_replies =
            !strcmp(scenario, "script-deferred-replies");

        if (!script) {
            fprintf(stderr, "# harness: cannot open the script\n");
            return 2;
        }
        while (fgets(line, sizeof(line), script)) {
            size_t length = strlen(line);

            while (length && (line[length - 1] == '\n'
                              || line[length - 1] == '\r')) {
                line[--length] = 0;
            }
            if (!length || line[0] == '#') {
                continue;
            }
            if (!strncmp(line, "frames ", 7)) {
                frames(atoi(line + 7));
                harness_wait_reply(expected);
                continue;
            }
            harness_wait_reply(expected);
            command(line);
            ++expected;
        }
        harness_wait_reply(expected);
        fclose(script);
        frames(4);
    } else if (!strcmp(scenario, "bad")) {
        command("nonsense");
        command("press 99 02");
        command("press 19 zz");
        command("analog 9 1");
        frames(4);
    } else {
        fprintf(stderr, "# harness: unknown scenario %s\n", scenario);
        return 2;
    }

    if (harness_client >= 0) {
        close(harness_client);
    }
    return 0;
}
