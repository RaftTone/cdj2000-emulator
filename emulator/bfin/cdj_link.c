/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cdj_link: the SPORT1 link to MAIN for cdj-gui-run (see cdj_link.h). The
 * rules are bin/cdj-run's (patches 02, 05, 10, 11, 13, 14, 32, 33), whose
 * comments in dv-bfin_ppi.c and interp.c carry the measurements behind each;
 * only the knobs nxs_vm, cosim and the mods launcher set are carried over.
 */
#define _GNU_SOURCE     /* memmem */
#include "cdj_link.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The firmware validates an announced payload against 1..2048 halfwords. */
#define FRAME_MAX   4096
/* Distinct record lengths kept at once (list answers vary per page). */
#define SIZES       16
#define DEPTH_MAX   256
#define STATUS_LEN  64
/* Where the GUI's receive descriptor puts a 64-byte status record; an
 * announced payload of the same length goes to 0x00f00040. */
#define STATUS_BUF  0x00f00000u
#define SPORT1_BASE 0xFFC00900u

enum { COSIM_TIME = 1, COSIM_RECORD = 2, COSIM_REQUEST = 3, COSIM_HELLO = 4 };
#define COSIM_HEADER      20
#define COSIM_PAYLOAD_MAX 8192

typedef struct due {
    struct due *next;
    int64_t due;
    unsigned len;
    uint8_t data[];
} due;

struct cdj_link {
    /* configuration */
    char host[64];
    unsigned port;
    int link, fresh_only, native, zero200, no_zero200, peer;
    unsigned depth;
    const char *rx_dump_path, *tx_dump_path;
    FILE *rx_dump, *tx_dump;

    /* the two sockets of BFIN_MAIN_LINK */
    int opened, req_fd, rec_fd;
    uint8_t in[16384];
    size_t in_len;
    int framed;

    /* per-length rings of MAIN's records */
    unsigned len[SIZES], put[SIZES], get[SIZES];
    uint8_t *frame;             /* [SIZES][depth][FRAME_MAX] */
    uint8_t *fresh;             /* [SIZES][depth] */
    uint64_t *order, next_order;/* arrival order, for native bursts */
    int yield_budget;
    uint8_t last[STATUS_LEN];
    int have_last;

    /* canned bootstrap status records (BFIN_MAIN_PEER_STATUS) */
    uint8_t *boot;
    unsigned boot_n, boot_i, boot_reads, boot_hold;

    /* co-simulation */
    int cosim, cosim_fd;
    int64_t quantum, peer_t, peer_promise, reported, promised;
    due *head, *tail;
    uint8_t cin[COSIM_HEADER + COSIM_PAYLOAD_MAX + 65536];
    size_t cin_len;

    /* counters */
    unsigned long rx_bytes, resync, flush, noslot, arrived, delivered,
                  repeated, sent, records, requests, late, waits;
};

static unsigned le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static int split_hostport(const char *spec, char *host, size_t size, unsigned *port)
{
    const char *colon = strrchr(spec, ':');

    snprintf(host, size, "127.0.0.1");
    if (colon) {
        size_t n = (size_t)(colon - spec);

        if (n >= size) {
            return -1;
        }
        if (n) {
            memcpy(host, spec, n);
            host[n] = 0;
        }
        spec = colon + 1;
    }
    *port = (unsigned)strtoul(spec, NULL, 0);
    return 0;
}

static int env_is(const char *name, const char *value)
{
    const char *v = getenv(name);

    return v && !strcmp(v, value);
}

cdj_link *cdj_link_new(void)
{
    cdj_link *l = calloc(1, sizeof(*l));
    const char *v;

    l->req_fd = l->rec_fd = l->cosim_fd = -1;
    v = getenv("BFIN_COSIM");
    if (v && *v) {
        l->cosim = 1;
        split_hostport(v, l->host, sizeof(l->host), &l->port);
        v = getenv("BFIN_COSIM_QUANTUM_US");
        l->quantum = (v && *v ? strtoll(v, NULL, 0) : 100) * 1000;
    } else if ((v = getenv("BFIN_MAIN_LINK")) && *v &&
               !split_hostport(v, l->host, sizeof(l->host), &l->port)) {
        l->link = 1;
    }
    l->fresh_only = env_is("BFIN_LINK_FRESH_ONLY", "1");
    l->native = env_is("BFIN_LINK_NATIVE_PARTIAL_DMA", "1");
    l->zero200 = getenv("BFIN_SPORT_RX_ZERO_200") != NULL;
    l->no_zero200 = getenv("BFIN_LINK_NO_ZERO200") != NULL;
    v = getenv("BFIN_LINK_DEPTH");
    l->depth = v && *v ? (unsigned)strtoul(v, NULL, 0) : 1;
    l->depth = l->depth < 1 ? 1 : l->depth > DEPTH_MAX ? DEPTH_MAX : l->depth;
    l->frame = malloc((size_t)SIZES * l->depth * FRAME_MAX);
    l->fresh = calloc((size_t)SIZES * l->depth, 1);
    l->order = calloc((size_t)SIZES * l->depth, sizeof(*l->order));
    l->rx_dump_path = getenv("BFIN_MAIN_LINK_DUMP");
    l->tx_dump_path = getenv("BFIN_SPORT_TX_OUTPUT");

    /* ponytail: the canned peer serves BFIN_MAIN_PEER_STATUS only; patch 02's
     * per-type payload files (BFIN_MAIN_PEER_PAYLOAD_*) are for GUI-only
     * scenes, add them if one has to run here. */
    l->peer = getenv("BFIN_MAIN_PEER") != NULL;
    if (l->peer && (v = getenv("BFIN_MAIN_PEER_STATUS")) && *v) {
        FILE *f = fopen(v, "rb");

        if (f) {
            l->boot = malloc(16 * STATUS_LEN);
            l->boot_n = (unsigned)fread(l->boot, STATUS_LEN, 16, f);
            fclose(f);
        }
        v = getenv("BFIN_MAIN_PEER_STATUS_HOLD");
        l->boot_hold = v && *v ? (unsigned)strtoul(v, NULL, 0) : 1;
    }
    if (l->fresh_only) {
        fprintf(stderr, "link: fresh-only live delivery\n");
    }
    return l;
}

static void dump_close(FILE **f)
{
    if (*f) {
        fclose(*f);
        *f = NULL;
    }
}

void cdj_link_free(cdj_link *l)
{
    if (l->req_fd >= 0) close(l->req_fd);
    if (l->rec_fd >= 0) close(l->rec_fd);
    if (l->cosim_fd >= 0) close(l->cosim_fd);
    dump_close(&l->rx_dump);
    dump_close(&l->tx_dump);
    while (l->head) {
        due *d = l->head;

        l->head = d->next;
        free(d);
    }
    free(l->frame);
    free(l->fresh);
    free(l->order);
    free(l->boot);
    free(l);
}

int cdj_link_enabled(const cdj_link *l)
{
    return l->link || l->cosim;
}

int cdj_link_cosim(const cdj_link *l)
{
    return l->cosim;
}

/* ---- captures (patches 10, 11): kept open, flushed per record ----------- */

static void capture(FILE **f, const char *path, const uint8_t *head, size_t head_len,
                    const uint8_t *data, unsigned len)
{
    if (!path || !*path) {
        return;
    }
    if (!*f && !(*f = fopen(path, "ab"))) {
        return;
    }
    if (fwrite(head, 1, head_len, *f) != head_len || fwrite(data, 1, len, *f) != len ||
        fflush(*f) != 0) {
        fprintf(stderr, "link: capture write to %s failed\n", path);
        dump_close(f);
    }
}

static void rx_dump(cdj_link *l, const uint8_t *data, unsigned len)
{
    uint8_t h[8] = { 'S', 'P', 'R', 'X' };

    put_le(h + 4, len, 4);
    capture(&l->rx_dump, l->rx_dump_path, h, sizeof(h), data, len);
}

static void tx_dump(cdj_link *l, const uint8_t *data, unsigned len)
{
    uint8_t h[12] = { 'S', 'P', 'T', 'X' };

    put_le(h + 4, SPORT1_BASE, 4);
    put_le(h + 8, len, 4);
    capture(&l->tx_dump, l->tx_dump_path, h, sizeof(h), data, len);
}

/* ---- the record rings --------------------------------------------------- */

#define FRAME(l, s, e) ((l)->frame + ((size_t)(s) * (l)->depth + (e)) * FRAME_MAX)
#define FRESH(l, s, e) ((l)->fresh[(size_t)(s) * (l)->depth + (e)])
#define ORDER(l, s, e) ((l)->order[(size_t)(s) * (l)->depth + (e)])

static int slot_unread(const cdj_link *l, unsigned s)
{
    for (unsigned e = 0; e < l->depth; e++) {
        if (FRESH(l, s, e)) {
            return 1;
        }
    }
    return 0;
}

/* One whole record from MAIN into the ring of its length. A full ring drops
 * its oldest: the socket is always drained, so MAIN never stalls on us. */
static void store(cdj_link *l, const uint8_t *body, unsigned n)
{
    unsigned s, free_slot = SIZES, put;

    for (s = 0; s < SIZES && l->len[s] != n; s++) {
        if (!l->len[s] && free_slot == SIZES) {
            free_slot = s;
        }
    }
    if (s == SIZES) {
        s = free_slot;
    }
    if (s == SIZES) {
        /* Recycle a read-out slot, never the status slot that is repeated. */
        for (s = 0; s < SIZES; s++) {
            if (l->len[s] != STATUS_LEN && !slot_unread(l, s)) {
                break;
            }
        }
        if (s == SIZES) {
            if (!l->noslot++) {
                fprintf(stderr, "link: no slot for a %u-byte frame -- all %d lengths "
                        "hold unread frames; dropped\n", n, SIZES);
            }
            return;
        }
        l->put[s] = l->get[s] = 0;
        memset(&FRESH(l, s, 0), 0, l->depth);
    }
    put = l->put[s] % l->depth;
    if (FRESH(l, s, put) && l->get[s] % l->depth == put) {
        l->get[s] = (put + 1) % l->depth;
    }
    memcpy(FRAME(l, s, put), body, n);
    l->len[s] = n;
    FRESH(l, s, put) = 1;
    ORDER(l, s, put) = ++l->next_order;
    l->put[s] = (put + 1) % l->depth;
    l->arrived++;
    if (n == STATUS_LEN) {
        l->yield_budget = 3;
    }
}

static void consume(cdj_link *l, size_t n)
{
    l->in_len -= n;
    memmove(l->in, l->in + n, l->in_len);
}

/* "CDJL" + LE32 length + body. A bad magic on a framed stream or a bad
 * length resynchronises on the next magic; a peer that never framed is a
 * flat byte stream, left for cdj_link_rx to read as such. */
static void split(cdj_link *l)
{
    while (l->in_len >= 8) {
        const uint8_t *h = l->in;
        unsigned n;

        if (memcmp(h, "CDJL", 4)) {
            const uint8_t *m;

            if (!l->framed) {
                return;
            }
            m = memmem(h + 1, l->in_len - 1, "CDJL", 4);
            l->resync++;
            consume(l, m ? (size_t)(m - h) : l->in_len - 3);
            continue;
        }
        l->framed = 1;
        n = le32(h + 4);
        if (n == 0 || n > FRAME_MAX) {
            if (!l->flush++) {
                fprintf(stderr, "link: unusable frame length %u (max %d) -- resync\n",
                        n, FRAME_MAX);
            }
            consume(l, 4);
            continue;
        }
        if (l->in_len < 8 + (size_t)n) {
            return;
        }
        store(l, h + 8, n);
        consume(l, 8 + n);
    }
}

void cdj_link_inject(cdj_link *l, const uint8_t *frame, unsigned len)
{
    if (len == 0 || len > FRAME_MAX) {
        l->flush++;
        return;
    }
    l->framed = 1;
    store(l, frame, len);
}

/* Native packets are byte bursts (patches 32/33): the oldest fresh DLNK
 * record of any length that fits, delivered once with its own length. */
static unsigned take_native(cdj_link *l, uint8_t *dst, unsigned cap)
{
    unsigned best = SIZES, best_e = 0;
    uint64_t oldest = UINT64_MAX;

    for (unsigned s = 0; s < SIZES; s++) {
        unsigned n = l->len[s];

        if (n < 4 || n > cap || n % 2) {
            continue;
        }
        for (unsigned e = 0; e < l->depth; e++) {
            if (FRESH(l, s, e) && !memcmp(FRAME(l, s, e), "DLNK", 4) &&
                ORDER(l, s, e) < oldest) {
                oldest = ORDER(l, s, e);
                best = s;
                best_e = e;
            }
        }
    }
    if (best == SIZES) {
        return 0;
    }
    memcpy(dst, FRAME(l, best, best_e), l->len[best]);
    FRESH(l, best, best_e) = 0;
    l->get[best] = (best_e + 1) % l->depth;
    return l->len[best];
}

/* A receive of exactly n bytes: the oldest unread record of that length.
 * With nothing unread a 64-byte status record is repeated (the wire carries
 * MAIN's last one continuously) unless BFIN_LINK_FRESH_ONLY (patch 05). */
static unsigned take(cdj_link *l, uint8_t *dst, unsigned n)
{
    unsigned got = l->native ? take_native(l, dst, n) : 0;

    if (got) {
        return got;
    }
    for (unsigned s = 0; s < SIZES; s++) {
        unsigned e = l->depth, get;

        if (l->len[s] != n) {
            continue;
        }
        get = l->get[s] % l->depth;
        for (unsigned k = 0; k < l->depth; k++) {
            if (FRESH(l, s, (get + k) % l->depth)) {
                e = (get + k) % l->depth;
                break;
            }
        }
        if (e == l->depth) {
            if (l->fresh_only || n != STATUS_LEN) {
                return 0;
            }
            e = (l->put[s] + l->depth - 1) % l->depth;
            l->repeated++;
        }
        memcpy(dst, FRAME(l, s, e), n);
        FRESH(l, s, e) = 0;
        l->get[s] = (e + 1) % l->depth;
        return n;
    }
    return 0;
}

/* ---- the sockets -------------------------------------------------------- */

static int connect_to(const char *host, unsigned port, int nonblocking)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    if (fd < 0) {
        return -1;
    }
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (nonblocking) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    return fd;
}

/* Lazily, and retried on the next transfer: MAIN may not listen yet. */
static void link_open(cdj_link *l)
{
    if (l->opened) {
        return;
    }
    if (l->cosim) {
        l->opened = 1;      /* the run loop owns the connection */
        return;
    }
    if (!l->link) {
        return;
    }
    l->req_fd = connect_to(l->host, l->port, 1);
    l->rec_fd = l->req_fd >= 0 ? connect_to(l->host, l->port + 2, 1) : -1;
    if (l->rec_fd >= 0) {
        l->opened = 1;
        fprintf(stderr, "link: open to %s:%u/%u\n", l->host, l->port, l->port + 2);
        return;
    }
    if (l->req_fd >= 0) {
        close(l->req_fd);
    }
    l->req_fd = -1;
}

static void link_poll(cdj_link *l)
{
    while (!l->cosim && l->rec_fd >= 0 && l->in_len < sizeof(l->in)) {
        ssize_t got = recv(l->rec_fd, l->in + l->in_len, sizeof(l->in) - l->in_len, 0);

        if (got <= 0) {
            break;
        }
        l->in_len += (size_t)got;
        l->rx_bytes += (unsigned long)got;
    }
    split(l);
}

/* Whether a status record is waiting, which refuses the 200-byte
 * housekeeping read so its poll loop gives DMA3 back (a few per record). */
static int record_waiting(cdj_link *l)
{
    if (!l->opened) {
        return 0;
    }
    link_poll(l);
    if (l->yield_budget <= 0) {
        return 0;
    }
    for (unsigned s = 0; s < SIZES; s++) {
        if (l->len[s] == STATUS_LEN && slot_unread(l, s)) {
            l->yield_budget--;
            return 1;
        }
    }
    l->yield_budget = 0;
    return 0;
}

unsigned cdj_link_rx(cdj_link *l, uint8_t *dst, unsigned cap, uint32_t addr)
{
    unsigned got = 0;

    if (cap == 200 && l->zero200 && !record_waiting(l) &&
        !(l->no_zero200 && l->opened && cdj_link_enabled(l))) {
        memset(dst, 0, cap);
        return cap;
    }
    if (cdj_link_enabled(l)) {
        link_open(l);
        if (l->opened) {
            link_poll(l);
            if (l->framed) {
                got = take(l, dst, cap);
            } else if (l->in_len >= cap) {
                memcpy(dst, l->in, cap);
                consume(l, cap);
                got = cap;
            }
            if (got) {
                if (got == STATUS_LEN && addr == STATUS_BUF) {
                    memcpy(l->last, dst, STATUS_LEN);
                    l->have_last = 1;
                }
                l->delivered++;
                rx_dump(l, dst, got);
                return got;
            }
            if (!l->fresh_only && cap == STATUS_LEN && addr == STATUS_BUF && l->have_last) {
                memcpy(dst, l->last, cap);
                l->repeated++;
                return cap;
            }
        }
        /* Patch 13: the canned bootstrap only until MAIN has spoken. */
        if (!l->peer || (l->opened && l->have_last)) {
            return 0;
        }
    }
    if (l->peer && l->boot_n && cap == STATUS_LEN) {
        memcpy(dst, l->boot + (size_t)l->boot_i * STATUS_LEN, cap);
        if (++l->boot_reads >= l->boot_hold && l->boot_i + 1 < l->boot_n) {
            l->boot_i++;
            l->boot_reads = 0;
        }
        return cap;
    }
    return 0;
}

/* ---- co-simulation (patch 14) ------------------------------------------- */

static void cosim_lost(cdj_link *l, const char *why)
{
    fprintf(stderr, "bfin-cosim: %s; exiting\n", why);
    cdj_link_stats(l, stderr);
    fflush(stderr);
    exit(0);
}

static void cosim_send(cdj_link *l, unsigned type, int64_t t, const uint8_t *p, unsigned len)
{
    uint8_t h[COSIM_HEADER] = { 'C', 'D', 'J', 'C', (uint8_t)type };
    const uint8_t *parts[2] = { h, p };
    size_t sizes[2] = { sizeof(h), len };

    put_le(h + 8, (uint64_t)t, 8);
    put_le(h + 16, len, 4);
    for (int i = 0; i < 2; i++) {
        while (sizes[i]) {
            ssize_t n = send(l->cosim_fd, parts[i], sizes[i], 0);

            if (n > 0) {
                parts[i] += n;
                sizes[i] -= (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                cosim_lost(l, n < 0 ? strerror(errno) : "the link closed");
            }
        }
    }
    if (t > l->reported) {
        l->reported = t;
    }
}

void cdj_link_cosim_connect(cdj_link *l)
{
    for (int tries = 0; tries < 600 && l->cosim_fd < 0; tries++) {
        if ((l->cosim_fd = connect_to(l->host, l->port, 0)) < 0) {
            usleep(100000);
        }
    }
    if (l->cosim_fd < 0) {
        fprintf(stderr, "bfin-cosim: nothing listens on %s:%u\n", l->host, l->port);
        exit(2);
    }
    cosim_send(l, COSIM_HELLO, 0, NULL, 0);
    fprintf(stderr, "bfin-cosim: on with MAIN at %s:%u, link latency %lld us\n",
            l->host, l->port, (long long)(l->quantum / 1000));
}

static void cosim_parse(cdj_link *l, int64_t now)
{
    size_t at = 0;

    while (l->cin_len - at >= COSIM_HEADER) {
        const uint8_t *h = l->cin + at;
        unsigned type = h[4], len = le32(h + 16);
        int64_t t = 0, promise;

        for (int i = 7; i >= 0; i--) {
            t = (int64_t)((uint64_t)t << 8 | h[8 + i]);
        }
        if (memcmp(h, "CDJC", 4) || len > COSIM_PAYLOAD_MAX) {
            /* bin/cdj-run runs on alone for a post-mortem; nothing here to see */
            cosim_lost(l, "a malformed message from MAIN");
        }
        if (l->cin_len - at < COSIM_HEADER + (size_t)len) {
            break;
        }
        if (type == COSIM_RECORD && len) {
            due *d = malloc(sizeof(*d) + len);

            d->next = NULL;
            d->due = t + l->quantum;
            d->len = len;
            memcpy(d->data, h + COSIM_HEADER, len);
            l->late += d->due < now;
            if (l->tail) {
                l->tail->next = d;
            } else {
                l->head = d;
            }
            l->tail = d;
            l->records++;
        }
        if (t > l->peer_t) {
            l->peer_t = t;
        }
        promise = t;
        if (type == COSIM_TIME && len == 8) {
            promise = 0;
            for (int i = 7; i >= 0; i--) {
                promise = (int64_t)((uint64_t)promise << 8 | h[COSIM_HEADER + i]);
            }
        }
        if (promise > l->peer_promise) {
            l->peer_promise = promise;
        }
        at += COSIM_HEADER + len;
    }
    l->cin_len -= at;
    memmove(l->cin, l->cin + at, l->cin_len);
}

static void cosim_drain(cdj_link *l, int64_t now)
{
    for (;;) {
        ssize_t got;

        if (l->cin_len == sizeof(l->cin)) {
            cosim_parse(l, now);
            if (l->cin_len == sizeof(l->cin)) {
                cosim_lost(l, "a message larger than the input buffer");
            }
        }
        got = recv(l->cosim_fd, l->cin + l->cin_len, sizeof(l->cin) - l->cin_len, MSG_DONTWAIT);
        if (got > 0) {
            l->cin_len += (size_t)got;
        } else if (got == 0) {
            cosim_lost(l, "MAIN closed the link");
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            cosim_lost(l, strerror(errno));
        }
    }
    cosim_parse(l, now);
}

/* Records due by now, once the firmware has touched the link. */
static void cosim_deliver(cdj_link *l, int64_t now)
{
    while (l->opened && l->head && l->head->due <= now) {
        due *d = l->head;

        l->head = d->next;
        if (!l->head) {
            l->tail = NULL;
        }
        cdj_link_inject(l, d->data, d->len);
        free(d);
    }
}

static void cosim_report(cdj_link *l, int64_t now, int64_t promise)
{
    uint8_t p[8];

    put_le(p, (uint64_t)promise, 8);
    cosim_send(l, COSIM_TIME, now, p, sizeof(p));
    l->promised = promise;
}

/* ponytail: the promise is always "now" -- this board never claims the
 * parked lookahead bin/cdj-run computes from its next event, so MAIN runs at
 * most one quantum ahead of it. Correct, only slower while both idle; add
 * the lookahead (bf531's next event) if co-simulated idle time matters. */
int64_t cdj_link_cosim_step(cdj_link *l, int64_t now)
{
    int64_t next;

    cosim_drain(l, now);
    cosim_deliver(l, now);
    if (now - l->reported >= l->quantum / 2 || now > l->promised) {
        cosim_report(l, now, now);
    }
    if (now >= l->peer_promise + l->quantum) {
        l->waits++;
    }
    while (now >= l->peer_promise + l->quantum) {
        struct pollfd w = { .fd = l->cosim_fd, .events = POLLIN };

        if (l->reported != now) {
            cosim_report(l, now, now);
        }
        if (poll(&w, 1, 1000) > 0) {
            cosim_drain(l, now);
            cosim_deliver(l, now);
        }
    }
    next = l->peer_promise + l->quantum;
    if (l->reported + l->quantum / 2 < next) {
        next = l->reported + l->quantum / 2;
    }
    if (l->opened && l->head && l->head->due < next) {
        next = l->head->due;
    }
    return next > now ? next : now + 1;
}

void cdj_link_tx(cdj_link *l, const uint8_t *data, unsigned len, int64_t now_ns)
{
    if (cdj_link_enabled(l)) {
        link_open(l);
        if (l->cosim) {
            l->requests++;
            cosim_send(l, COSIM_REQUEST, now_ns, data, len);
        } else if (l->opened) {
            ssize_t n = send(l->req_fd, data, len, 0);

            l->sent += n == (ssize_t)len;
        }
    }
    tx_dump(l, data, len);
}

void cdj_link_stats(const cdj_link *l, FILE *f)
{
    fprintf(f, "STATS link open=%d framed=%d rx_bytes=%lu arrived=%lu delivered=%lu "
            "repeated=%lu sent=%lu resync=%lu flush=%lu noslot=%lu", l->opened, l->framed,
            l->rx_bytes, l->arrived, l->delivered, l->repeated, l->sent, l->resync,
            l->flush, l->noslot);
    if (l->cosim) {
        fprintf(f, " cosim records=%lu requests=%lu late=%lu waits=%lu main_t=%.6f "
                "main_promise=%.6f", l->records, l->requests, l->late, l->waits,
                l->peer_t / 1e9, l->peer_promise / 1e9);
    }
    fputc('\n', f);
}
