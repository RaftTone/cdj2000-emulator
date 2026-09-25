/*
 * Pioneer CDJ-2000: the MAIN board and the GUI board in one guest time.
 *
 * The two boards are emulated by two programs, this QEMU (MAIN, SH-4) and the
 * GNU sim for the Blackfin (GUI), and until now each ran on the wall clock.
 * That made the GUI simulator's speed part of the protocol: it executes about
 * thirty million instructions a second where the chip executes four hundred,
 * MAIN gives the GUI 3 ms to answer a record ("GU送:bAnsReceive=...3msec..."
 * on MAIN's console), and a GUI that is late on the wall clock is late in
 * MAIN's time too -- forced sends (強制送信), and after ~7 s of that a reset of
 * the GUI board and E-8709 on the screen.  No interpreter can be made fast
 * enough to fix that; the times have to be the boards' own.
 *
 * With CDJ_COSIM=<port> both boards keep guest time of their own -- MAIN under
 * -icount (a fixed number of ns per instruction), the GUI under
 * BFIN_TIME_BASE=virtual (one tick per core cycle, 2.5 ns at 400 MHz) -- and
 * this file keeps the two together, conservatively, the way co-simulations of
 * boards are usually kept (SystemC's quantum, Xilinx's remote-port):
 *
 *  - the link has a latency, CDJ_COSIM_QUANTUM_US (100 us): a frame sent at
 *    guest time t reaches the other board at t + quantum, never earlier;
 *  - neither board runs past the other's reported time + quantum, so nothing
 *    can arrive in its past;
 *  - each reports its time as it goes (every half quantum), with every frame,
 *    and whenever it has to wait.
 *
 * So a GUI that needs a millisecond of its own time for an answer answers
 * after a millisecond of MAIN's time, however long the host takes over it:
 * the boards see each other as the hardware does, and a run takes as long as
 * the slower program needs.  Both skip their idle time, so that is often less
 * than real time.
 *
 * The wire is one TCP connection on 127.0.0.1:<port>, the GUI connecting.  A
 * message is a 20-byte header -- "CDJC", its type, three zero bytes, the guest
 * time in ns since the connection (u64), the payload length (u32), all little
 * endian -- and the payload: TIME (1, empty), RECORD (2, a frame of MAIN's,
 * the body the record socket carried after "CDJL" + length), REQUEST (3, bytes
 * the GUI sent, what the request socket carried) and HELLO (4, empty, the
 * GUI's time 0).  The two chardev sockets stay open and carry nothing.
 *
 * MAIN should start paused (-S): it is started when the GUI connects, so both
 * begin at guest time 0.  Without -S it waits for the GUI at its first step.
 * If the GUI goes away the co-simulation ends and MAIN runs on alone.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"

#include "cdj2000_cosim.h"

#ifdef _WIN32
/*
 * The wire is plain BSD sockets and poll(), which MinGW does not have.  A
 * Windows build keeps the entry points so MAIN links, and refuses CDJ_COSIM
 * rather than running a co-simulation that cannot work.
 */
bool cdj_cosim_active(void)
{
    return false;
}

void cdj_cosim_send_record(const uint8_t *frame, unsigned len)
{
}

bool cdj_cosim_init(CdjCosimRequestFn sink, CdjCosimPollFn poll, void *opaque)
{
    const char *spec = getenv("CDJ_COSIM");

    if (spec && *spec) {
        error_report("cdj2000-cosim: CDJ_COSIM is not supported on Windows");
        exit(1);
    }
    return false;
}
#else

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

enum { COSIM_TIME = 1, COSIM_RECORD = 2, COSIM_REQUEST = 3, COSIM_HELLO = 4 };
#define COSIM_HEADER        20
#define COSIM_PAYLOAD_MAX   8192
#define COSIM_WAIT_NOTE_S   10          /* say so when the GUI is this slow */

typedef struct CosimDue {
    int64_t due;                        /* guest ns it reaches MAIN */
    unsigned len;
    uint8_t data[];
} CosimDue;

static struct {
    bool active;
    bool connected;
    int listen_fd, fd;
    unsigned port;
    int64_t quantum;                    /* ns: the latency and the lookahead */
    int64_t epoch;                      /* the virtual ns that is guest time 0 */
    int64_t peer;                       /* the GUI's time, as far as we know */
    int64_t peer_promise;               /* it sends nothing before this */
    int64_t reported;                   /* ours, as far as it knows */
    int64_t promised;                   /* the promise we last sent */
    QEMUTimer *timer;
    GQueue due;                         /* CosimDue *, in time order */
    CdjCosimRequestFn sink;
    CdjCosimPollFn poll;
    void *sink_opaque;
    uint8_t in[COSIM_HEADER + COSIM_PAYLOAD_MAX + 65536];
    size_t in_len;
    /* the census, CDJ_COSIM_CENSUS=<guest seconds> (10) */
    uint64_t steps, waits, records, requests;
    int64_t wait_wall_ns, census_every, census_next;
} cs = { .listen_fd = -1, .fd = -1 };

bool cdj_cosim_active(void)
{
    return cs.active;
}

static int64_t cosim_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - cs.epoch;
}

static int64_t cosim_wall_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static void cosim_lost(const char *why)
{
    CosimDue *due;

    if (!cs.active) {
        return;
    }
    error_report("cdj2000-cosim: %s at guest t=%.6f; MAIN runs on alone",
                 why, cosim_now() / 1e9);
    cs.active = false;
    cs.connected = false;
    timer_del(cs.timer);
    while ((due = g_queue_pop_head(&cs.due))) {
        g_free(due);
    }
    if (cs.fd >= 0) {
        close(cs.fd);
        cs.fd = -1;
    }
    if (cs.listen_fd >= 0) {
        qemu_set_fd_handler(cs.listen_fd, NULL, NULL, NULL);
        close(cs.listen_fd);
        cs.listen_fd = -1;
    }
    if (!runstate_is_running()) {
        vm_start();
    }
}

static void cosim_write(const uint8_t *data, size_t len)
{
    while (len && cs.active) {
        ssize_t put = send(cs.fd, data, len, 0);

        if (put > 0) {
            data += put;
            len -= put;
        } else if (put < 0 && errno == EINTR) {
            continue;
        } else {
            cosim_lost(put < 0 ? strerror(errno) : "the link closed");
        }
    }
}

static void cosim_send(unsigned type, int64_t t, const uint8_t *payload,
                       unsigned len)
{
    uint8_t header[COSIM_HEADER] = { 'C', 'D', 'J', 'C', type };

    if (!cs.active || !cs.connected) {
        return;
    }
    stq_le_p(header + 8, t);
    stl_le_p(header + 16, len);
    cosim_write(header, sizeof(header));
    if (len) {
        cosim_write(payload, len);
    }
    if (t > cs.reported) {
        cs.reported = t;
    }
}

/* Whole messages out of the input buffer: times, and requests to deliver. */
static void cosim_parse(void)
{
    size_t at = 0;

    while (cs.active && cs.in_len - at >= COSIM_HEADER) {
        const uint8_t *head = cs.in + at;
        unsigned type = head[4], len = ldl_le_p(head + 16);
        int64_t t = ldq_le_p(head + 8);

        if (memcmp(head, "CDJC", 4) || len > COSIM_PAYLOAD_MAX) {
            cosim_lost("a malformed message from the GUI");
            return;
        }
        if (cs.in_len - at < COSIM_HEADER + len) {
            break;
        }
        if (type == COSIM_REQUEST && len) {
            CosimDue *due = g_malloc(sizeof(*due) + len);

            due->due = t + cs.quantum;
            due->len = len;
            memcpy(due->data, head + COSIM_HEADER, len);
            g_queue_push_tail(&cs.due, due);
            cs.requests++;
        }
        /* Every message says how far the GUI has got; a TIME with a payload
           also says it will send nothing before that time. */
        if (t > cs.peer) {
            cs.peer = t;
        }
        {
            int64_t promise = type == COSIM_TIME && len == 8
                ? (int64_t)ldq_le_p(head + COSIM_HEADER) : t;

            if (promise > cs.peer_promise) {
                cs.peer_promise = promise;
            }
        }
        at += COSIM_HEADER + len;
    }
    memmove(cs.in, cs.in + at, cs.in_len - at);
    cs.in_len -= at;
}

/* Everything the GUI has sent so far, without waiting.  False if it is gone. */
static bool cosim_drain(void)
{
    while (cs.active) {
        ssize_t got;

        if (cs.in_len == sizeof(cs.in)) {
            cosim_parse();
            if (cs.in_len == sizeof(cs.in)) {
                cosim_lost("a message larger than the input buffer");
                break;
            }
        }
        got = recv(cs.fd, cs.in + cs.in_len, sizeof(cs.in) - cs.in_len,
                   MSG_DONTWAIT);
        if (got > 0) {
            cs.in_len += got;
        } else if (got == 0) {
            cosim_lost("the GUI closed the link");
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            cosim_lost(strerror(errno));
        }
    }
    cosim_parse();
    return cs.active;
}

static void cosim_deliver(int64_t now)
{
    CosimDue *due;

    while (cs.active && (due = g_queue_peek_head(&cs.due)) && due->due <= now) {
        g_queue_pop_head(&cs.due);
        cs.sink(cs.sink_opaque, due->data, due->len);
        g_free(due);
    }
}

static void cosim_census(int64_t now)
{
    if (!cs.census_every || now < cs.census_next) {
        return;
    }
    cs.census_next = now + cs.census_every;
    fprintf(stderr, "cdj2000-cosim: t=%.3f steps %" PRIu64 " waits %" PRIu64
            " (%.1f s of wall clock) records %" PRIu64 " requests %" PRIu64
            " gui at t=%.6f\n", now / 1e9, cs.steps, cs.waits,
            cs.wait_wall_ns / 1e9, cs.records, cs.requests, cs.peer / 1e9);
}

static void cosim_connected(int fd)
{
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    qemu_set_fd_handler(cs.listen_fd, NULL, NULL, NULL);
    close(cs.listen_fd);
    cs.listen_fd = -1;
    cs.fd = fd;
    cs.connected = true;
    cs.epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    cs.peer = cs.reported = cs.peer_promise = cs.promised = 0;
    cs.census_next = cs.census_every;
    cosim_send(COSIM_HELLO, 0, NULL, 0);
    timer_mod(cs.timer, cs.epoch + cs.quantum / 2);
    fprintf(stderr, "cdj2000-cosim: the GUI is on, guest time 0 = virtual "
            "%.6f s, quantum %" PRId64 " us\n", cs.epoch / 1e9,
            cs.quantum / 1000);
}

/* -S: the GUI connecting is what starts MAIN. */
static void cosim_accept_ready(void *opaque)
{
    int fd = accept(cs.listen_fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    cosim_connected(fd);
    if (!runstate_is_running()) {
        vm_start();
    }
}

/* Not started paused: MAIN reached its first step before the GUI connected. */
static bool cosim_accept_blocking(void)
{
    struct pollfd wait = { .fd = cs.listen_fd, .events = POLLIN };
    int fd;

    fprintf(stderr, "cdj2000-cosim: MAIN waits for the GUI on port %u "
            "(start it paused with -S to begin together)\n", cs.port);
    if (poll(&wait, 1, 120 * 1000) <= 0
        || (fd = accept(cs.listen_fd, NULL, NULL)) < 0) {
        cosim_lost("no GUI connected within 120 s");
        return false;
    }
    cosim_connected(fd);
    return true;
}

/*
 * The earliest guest time MAIN could send anything.  While it runs, now.
 * While every CPU sleeps with nothing to do, not before its next timer (a
 * device's or the RTOS tick), a request from the GUI due to arrive, or the
 * earliest a message from the GUI could arrive -- whatever wakes it first.
 * That promise lets the GUI run on to it plus the link latency in one go
 * instead of in half-latency steps while both boards idle: the classic
 * lookahead of a conservative co-simulation.
 */
static int64_t cosim_promise(int64_t now)
{
    CPUState *cpu;
    CosimDue *due;
    int64_t promise, deadline;

    CPU_FOREACH(cpu) {
        if (!cpu->halted || cpu_has_work(cpu)) {
            return now;
        }
    }
    timer_del(cs.timer);                /* our own step is not MAIN's work */
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          QEMU_TIMER_ATTR_ALL);
    promise = deadline < 0 ? INT64_MAX : now + deadline;
    due = g_queue_peek_head(&cs.due);
    if (due && due->due < promise) {
        promise = due->due;
    }
    if (cs.peer_promise + cs.quantum < promise) {
        promise = cs.peer_promise + cs.quantum;
    }
    return promise > now ? promise : now;
}

static void cosim_report(int64_t now, int64_t promise)
{
    uint8_t payload[8];

    stq_le_p(payload, promise);
    cosim_send(COSIM_TIME, now, payload, sizeof(payload));
    cs.promised = promise;
}

/*
 * The step: take what the GUI sent, hand over the requests that are due,
 * tell the GUI where MAIN is, and wait while MAIN is a whole latency ahead.
 * It runs as a timer on MAIN's virtual clock, so under -icount MAIN stops at
 * exactly the step's time; the wait holds the main loop, which is the point.
 */
static void cosim_step(void *opaque)
{
    int64_t now, next, promise, wall = 0, noted = 0;
    CosimDue *due;

    if (!cs.active) {
        return;
    }
    if (!cs.connected && !cosim_accept_blocking()) {
        return;
    }
    now = cosim_now();
    cs.steps++;
    if (!cosim_drain()) {
        return;
    }
    cosim_deliver(now);
    if (cs.poll) {
        cs.poll(cs.sink_opaque);
    }
    promise = cosim_promise(now);
    if (now - cs.reported >= cs.quantum / 2 || promise > cs.promised) {
        cosim_report(now, promise);
    }
    while (cs.active && now >= cs.peer_promise + cs.quantum) {
        struct pollfd wait = { .fd = cs.fd, .events = POLLIN };

        promise = cosim_promise(now);
        if (cs.reported != now || promise != cs.promised) {
            cosim_report(now, promise);
        }
        if (!wall) {
            wall = cosim_wall_ns();
            cs.waits++;
        }
        if (poll(&wait, 1, 1000) == 0) {
            int64_t waited = cosim_wall_ns() - wall;

            if (waited / NANOSECONDS_PER_SECOND >= noted + COSIM_WAIT_NOTE_S) {
                noted = waited / NANOSECONDS_PER_SECOND;
                fprintf(stderr, "cdj2000-cosim: MAIN has waited %" PRId64
                        " s for the GUI at t=%.6f (GUI at t=%.6f, promised "
                        "%.6f)\n", noted, now / 1e9, cs.peer / 1e9,
                        cs.peer_promise / 1e9);
            }
            continue;
        }
        if (!cosim_drain()) {
            return;
        }
        cosim_deliver(now);
    }
    if (wall) {
        cs.wait_wall_ns += cosim_wall_ns() - wall;
    }
    if (!cs.active) {
        return;
    }
    cosim_census(now);
    next = cs.peer_promise + cs.quantum;
    /* running: report every half latency; sleeping: at the promise */
    promise = cosim_promise(now);
    if (promise > cs.promised) {
        cosim_report(now, promise);
    }
    if (promise == now) {
        if (cs.reported + cs.quantum / 2 < next) {
            next = cs.reported + cs.quantum / 2;
        }
    } else if (promise < next) {
        next = promise;
    }
    due = g_queue_peek_head(&cs.due);
    if (due && due->due < next) {
        next = due->due;
    }
    if (next <= now) {
        next = now + 1;
    }
    timer_mod(cs.timer, cs.epoch + next);
}

void cdj_cosim_send_record(const uint8_t *frame, unsigned len)
{
    if (!cs.active || !cs.connected) {
        return;
    }
    cs.records++;
    cosim_send(COSIM_RECORD, cosim_now(), frame, len);
}

bool cdj_cosim_init(CdjCosimRequestFn sink, CdjCosimPollFn poll, void *opaque)
{
    const char *spec = getenv("CDJ_COSIM");
    const char *quantum = getenv("CDJ_COSIM_QUANTUM_US");
    const char *census = getenv("CDJ_COSIM_CENSUS");
    struct sockaddr_in address = { .sin_family = AF_INET };
    int one = 1;

    if (!spec || !*spec) {
        return false;
    }
    cs.port = strtoul(spec, NULL, 0);
    cs.quantum = (quantum && *quantum ? strtoll(quantum, NULL, 0) : 100)
                 * SCALE_US;
    if (cs.quantum < 2 * SCALE_US) {
        cs.quantum = 2 * SCALE_US;
    }
    cs.census_every = (int64_t)((census && *census ? strtod(census, NULL)
                                 : 10.0) * NANOSECONDS_PER_SECOND);
    cs.sink = sink;
    cs.poll = poll;
    cs.sink_opaque = opaque;
    g_queue_init(&cs.due);

    address.sin_port = htons(cs.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cs.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cs.listen_fd < 0
        || setsockopt(cs.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                      sizeof(one)) < 0
        || bind(cs.listen_fd, (struct sockaddr *)&address,
                sizeof(address)) < 0
        || listen(cs.listen_fd, 1) < 0) {
        error_report("cdj2000-cosim: cannot listen on 127.0.0.1:%u: %s",
                     cs.port, strerror(errno));
        exit(1);
    }
    cs.active = true;
    qemu_set_fd_handler(cs.listen_fd, cosim_accept_ready, NULL, NULL);
    cs.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cosim_step, NULL);
    timer_mod(cs.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                        + cs.quantum / 2);
    if (!icount_enabled()) {
        warn_report("cdj2000-cosim: without -icount MAIN's time is the wall "
                    "clock's, and the GUI's speed still decides what it sees");
    }
    fprintf(stderr, "cdj2000-cosim: listening on 127.0.0.1:%u, link latency "
            "%" PRId64 " us\n", cs.port, cs.quantum / 1000);
    return true;
}
#endif
