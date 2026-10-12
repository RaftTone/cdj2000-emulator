/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cdj-gui-run's MAIN link (emulator/bfin/cdj_link.c) and SPORT1 DMA
 * (bf531.c) against a fake MAIN on loopback sockets: framing and resync,
 * per-length slots, fresh-only (patch 05), no canned record once MAIN has
 * spoken (13), zero-200 housekeeping, native partial bursts (32/33), the
 * captures (10/11), the co-simulation wire (14), and the receive DMA's
 * partial/retry/IRQ behaviour and the untruncated transmit.
 *
 *   cc -I emulator/bfin tests/cstub/cdj-gui-link.c emulator/bfin/{cdj_link,
 *      bf531,bfin_core,bfin_exec,bfin_dsp}.c && ./a.out SCRATCH_DIR
 */
#include "bf531.h"
#include "cdj_link.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *dir;
static int req_l, rec_l, req_c, rec_c;  /* fake MAIN: listeners, accepted */
static unsigned base_port;

static int listen_on(unsigned port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 4)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void fake_main(void)
{
    char spec[32];

    for (base_port = 20000 + getpid() % 20000;; base_port += 3) {
        if ((req_l = listen_on(base_port)) < 0) {
            continue;
        }
        if ((rec_l = listen_on(base_port + 2)) >= 0) {
            break;
        }
        close(req_l);
    }
    snprintf(spec, sizeof(spec), "127.0.0.1:%u", base_port);
    setenv("BFIN_MAIN_LINK", spec, 1);
}

static void accept_both(void)
{
    req_c = accept(req_l, NULL, NULL);
    rec_c = accept(rec_l, NULL, NULL);
    assert(req_c >= 0 && rec_c >= 0);
}

static void drop_main(void)
{
    close(req_c);
    close(rec_c);
    close(req_l);
    close(rec_l);
}

static void wire(const void *p, size_t n)
{
    assert(send(rec_c, p, n, 0) == (ssize_t)n);
    usleep(20000);
}

static void frame(const uint8_t *body, unsigned n)
{
    uint8_t h[8] = { 'C', 'D', 'J', 'L', n, n >> 8, n >> 16, n >> 24 };

    wire(h, 8);
    wire(body, n);
}

static void filled(uint8_t *b, unsigned n, uint8_t v, const char *magic)
{
    memset(b, v, n);
    if (magic) {
        memcpy(b, magic, 4);
    }
}

/* A fresh link on a fresh fake MAIN with these switches. */
static cdj_link *open_link(const char *fresh, const char *native)
{
    uint8_t probe[64];
    cdj_link *l;

    fake_main();
    setenv("BFIN_LINK_FRESH_ONLY", fresh, 1);
    setenv("BFIN_LINK_NATIVE_PARTIAL_DMA", native, 1);
    l = cdj_link_new();
    cdj_link_rx(l, probe, 64, 0x00f00000);                 /* connects lazily */
    accept_both();
    return l;
}

static void test_framing_slots_fresh_only(void)
{
    uint8_t st[64], pay[224], got[4096];
    cdj_link *l;

    for (int fresh = 0; fresh < 2; fresh++) {
        l = open_link(fresh ? "1" : "0", "0");
        filled(st, 64, 0x11, NULL);
        filled(pay, 224, 0x22, NULL);
        frame(pay, 224);
        frame(st, 64);
        assert(cdj_link_rx(l, got, 48, 0x00f00040) == 0);      /* no such length */
        assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[5] == 0x11);
        assert(cdj_link_rx(l, got, 224, 0x00f00040) == 224 && got[5] == 0x22);
        /* Nothing new: the status record repeats unless fresh-only (05);
         * a payload never repeats. */
        assert(cdj_link_rx(l, got, 64, 0x00f00000) == (fresh ? 0 : 64));
        assert(cdj_link_rx(l, got, 224, 0x00f00040) == 0);

        /* Resync: junk between frames, then an unusable length. */
        wire("junkjunk", 8);
        filled(st, 64, 0x33, NULL);
        frame(st, 64);
        assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[0] == 0x33);
        wire("CDJL\x88\x13\0\0", 8);                          /* 5000 > 4096 */
        filled(st, 64, 0x44, NULL);
        frame(st, 64);
        assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[0] == 0x44);
        cdj_link_free(l);
        drop_main();
    }
}

static void test_native_partial_bursts(void)
{
    uint8_t a[100], b[40], st[64], got[4096];
    cdj_link *l = open_link("1", "1");

    filled(a, 100, 0xA1, "DLNK");
    filled(b, 40, 0xB2, "DLNK");
    filled(st, 64, 0x55, NULL);
    frame(a, 100);
    frame(b, 40);
    frame(st, 64);
    /* Oldest first across lengths, each once, with its own length. */
    assert(cdj_link_rx(l, got, 4096, 0x00f01000) == 100 && got[99] == 0xA1);
    assert(cdj_link_rx(l, got, 4096 - 100, 0x00f01064) == 40 && got[39] == 0xB2);
    /* A plain record is not a burst; it waits for its exact receive. */
    assert(cdj_link_rx(l, got, 4096 - 140, 0x00f0108c) == 0);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[0] == 0x55);
    /* A burst larger than what is left of the receive waits. */
    frame(a, 100);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 0);
    assert(cdj_link_rx(l, got, 200, 0x00f01000) == 100);
    cdj_link_free(l);
    drop_main();
    unsetenv("BFIN_LINK_NATIVE_PARTIAL_DMA");
}

static void test_zero200_and_bootstrap(void)
{
    char path[512];
    uint8_t canned[64], st[64], got[4096];
    FILE *f;
    cdj_link *l;

    snprintf(path, sizeof(path), "%s/canned.bin", dir);
    filled(canned, 64, 0xCA, NULL);
    f = fopen(path, "wb");
    fwrite(canned, 1, 64, f);
    fclose(f);
    setenv("BFIN_SPORT_RX_ZERO_200", "", 1);
    setenv("BFIN_MAIN_PEER", "1", 1);
    setenv("BFIN_MAIN_PEER_STATUS", path, 1);
    l = open_link("1", "0");
    /* Before MAIN speaks: housekeeping reads complete with zeros, status
     * receives get the canned bootstrap record. */
    memset(got, 0xFF, 200);
    assert(cdj_link_rx(l, got, 200, 0x00f00100) == 200 && got[0] == 0 && got[199] == 0);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[0] == 0xCA);
    /* A status record waiting refuses the housekeeping read. */
    filled(st, 64, 0x66, NULL);
    frame(st, 64);
    assert(cdj_link_rx(l, got, 200, 0x00f00100) == 0);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[0] == 0x66);
    assert(cdj_link_rx(l, got, 200, 0x00f00100) == 200);
    /* Patch 13: once MAIN has spoken, a quiet wire is never canned. */
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 0);
    cdj_link_free(l);
    drop_main();
    unsetenv("BFIN_SPORT_RX_ZERO_200");
    unsetenv("BFIN_MAIN_PEER");
    unsetenv("BFIN_MAIN_PEER_STATUS");
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = malloc(1 << 16);

    assert(f);
    *n = fread(b, 1, 1 << 16, f);
    fclose(f);
    return b;
}

static void test_tx_and_captures(void)
{
    char rx_path[512], tx_path[512];
    uint8_t req[300], st[64], got[64], back[300];
    size_t n;
    uint8_t *cap;
    cdj_link *l;

    snprintf(rx_path, sizeof(rx_path), "%s/main-link.bin", dir);
    snprintf(tx_path, sizeof(tx_path), "%s/gui-link-tx.bin", dir);
    unlink(rx_path);
    unlink(tx_path);
    setenv("BFIN_MAIN_LINK_DUMP", rx_path, 1);
    setenv("BFIN_SPORT_TX_OUTPUT", tx_path, 1);
    l = open_link("1", "0");
    for (unsigned i = 0; i < sizeof(req); i++) {
        req[i] = (uint8_t)i;
    }
    cdj_link_tx(l, req, sizeof(req), 0);        /* no 128-byte truncation */
    usleep(20000);
    assert(recv(req_c, back, sizeof(back), MSG_WAITALL) == (ssize_t)sizeof(back));
    assert(!memcmp(back, req, sizeof(req)));
    filled(st, 64, 0x77, NULL);
    frame(st, 64);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64);
    /* Flushed per record: readable while the link is still open. */
    cap = slurp(tx_path, &n);
    assert(n == 12 + sizeof(req) && !memcmp(cap, "SPTX\0\x09\xc0\xff\x2c\x01\0\0", 12));
    assert(!memcmp(cap + 12, req, sizeof(req)));
    free(cap);
    cap = slurp(rx_path, &n);
    assert(n == 8 + 64 && !memcmp(cap, "SPRX\x40\0\0\0", 8) && cap[8] == 0x77);
    free(cap);
    cdj_link_free(l);
    drop_main();
    unsetenv("BFIN_MAIN_LINK_DUMP");
    unsetenv("BFIN_SPORT_TX_OUTPUT");
}

static void test_flat_peer(void)
{
    uint8_t raw[64], got[64];
    cdj_link *l = open_link("1", "0");

    filled(raw, 64, 0x88, NULL);
    wire(raw, 32);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 0);
    wire(raw, 32);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[63] == 0x88);
    cdj_link_free(l);
    drop_main();
}

/* ---- co-simulation ------------------------------------------------------ */

static void cosim_msg(int fd, unsigned type, int64_t t, const void *p, unsigned len)
{
    uint8_t h[20] = { 'C', 'D', 'J', 'C', type };

    for (int i = 0; i < 8; i++) h[8 + i] = (uint8_t)((uint64_t)t >> (8 * i));
    for (int i = 0; i < 4; i++) h[16 + i] = (uint8_t)(len >> (8 * i));
    assert(send(fd, h, 20, 0) == 20);
    if (len) {
        assert(send(fd, p, len, 0) == (ssize_t)len);
    }
    usleep(20000);
}

static int64_t le64(const uint8_t *p)
{
    uint64_t v = 0;

    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return (int64_t)v;
}

static void test_cosim(void)
{
    char spec[32];
    uint8_t h[20], promise[8], st[64], got[64], body[48];
    int lfd, fd;
    cdj_link *l;

    fake_main();                        /* for a free port; only one is used */
    close(rec_l);
    unsetenv("BFIN_MAIN_LINK");
    lfd = req_l;
    snprintf(spec, sizeof(spec), "127.0.0.1:%u", base_port);
    setenv("BFIN_COSIM", spec, 1);
    setenv("BFIN_COSIM_QUANTUM_US", "100", 1);
    l = cdj_link_new();
    assert(cdj_link_cosim(l) && cdj_link_enabled(l));
    cdj_link_cosim_connect(l);
    fd = accept(lfd, NULL, NULL);
    assert(recv(fd, h, 20, MSG_WAITALL) == 20 && !memcmp(h, "CDJC\x04", 5));
    /* MAIN at 50 us promises nothing before 1 ms, with a record at 50 us. */
    for (int i = 0; i < 8; i++) promise[i] = (uint8_t)(1000000 >> (8 * i));
    filled(st, 64, 0x99, NULL);
    cosim_msg(fd, 2, 50000, st, 64);
    cosim_msg(fd, 1, 50000, promise, 8);
    /* Never past MAIN's promise plus the latency; the record is due at its
     * stamp plus the latency, and waits for the firmware to touch the link. */
    assert(cdj_link_cosim_step(l, 0) <= 1100000);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 0);      /* opens; not due */
    assert(cdj_link_cosim_step(l, 149999) == 150000);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 0);
    cdj_link_cosim_step(l, 150000);
    assert(cdj_link_rx(l, got, 64, 0x00f00000) == 64 && got[1] == 0x99);
    /* A request leaves stamped with this board's time. */
    memset(body, 0x5A, sizeof(body));
    cdj_link_tx(l, body, sizeof(body), 160000);
    for (;;) {                          /* skip the TIME reports */
        assert(recv(fd, h, 20, MSG_WAITALL) == 20);
        unsigned len = h[16] | h[17] << 8;
        uint8_t p[64];

        assert(len <= sizeof(p) && recv(fd, p, len, MSG_WAITALL) == (ssize_t)len);
        if (h[4] == 3) {
            assert(le64(h + 8) == 160000 && len == 48 && p[47] == 0x5A);
            break;
        }
        assert(h[4] == 1 && len == 8);
    }
    cdj_link_free(l);
    close(fd);
    close(lfd);
    unsetenv("BFIN_COSIM");
}

/* ---- the SPORT1 DMA in bf531 -------------------------------------------- */

static unsigned script_n, rx_calls, tx_len;
static uint8_t tx_seen[512];
static uint32_t rx_addr;

static unsigned host_rx(void *o, uint8_t *dst, unsigned cap, uint32_t addr)
{
    rx_calls++;
    rx_addr = addr;
    if (!script_n || script_n > cap) {
        return 0;
    }
    memset(dst, 0xD7, script_n);
    unsigned n = script_n;

    script_n = 0;
    return n;
}

static void host_tx(void *o, const uint8_t *d, size_t n)
{
    tx_len = (unsigned)n;
    memcpy(tx_seen, d, n < sizeof(tx_seen) ? n : sizeof(tx_seen));
}

#define MMR(o) (0xFFC00000u + (o))
#define DMA3(r) MMR(0xCC0 + (r))
#define DMA4(r) MMR(0xD00 + (r))

static void test_dma(void)
{
    /* JUMP.S 0 at the boot entry: the core spins, the DMA runs as events. */
    static const uint8_t spin[] = { 0x00, 0x80, 0xA0, 0xFF, 2, 0, 0, 0, 0x00, 0x80, 0x00, 0x20 };
    bf531_host host = { .sport1_rx = host_rx, .sport1_tx = host_tx };
    bf531 *s = bf531_new(1 << 20, &host, NULL);
    const uint8_t *ram;
    uint32_t size;

    assert(bf531_boot_ldr(s, spin, sizeof(spin)) == 0);
    bf531_set_timing(s, BF531_CCLK_HZ / 60, 400000);       /* 1 ms retry */

    /* A 64-byte receive: nothing for a while (retried), then one record. */
    bf531_write(s, DMA3(0x04), 0x1000, 4);
    bf531_write(s, DMA3(0x10), 32, 2);
    bf531_write(s, DMA3(0x14), 2, 2);
    bf531_write(s, DMA3(0x08), 0x0001 | 0x0002 | 0x0004 | 0x0080, 2);
    bf531_run(s, 4000000);                                  /* 10 ms */
    assert(rx_calls >= 9 && rx_calls <= 12);
    assert(bf531_read(s, DMA3(0x28), 2) & 0x8);            /* DMA_RUN */
    script_n = 64;
    bf531_run(s, 800000);
    ram = bf531_sdram(s, &size);
    assert(ram[0x1000] == 0xD7 && ram[0x103F] == 0xD7 && ram[0x1040] == 0);
    assert((bf531_read(s, DMA3(0x28), 2) & 0x9) == 0x1);   /* DONE, not RUN */
    assert(bf531_read(s, DMA3(0x30), 2) == 0);
    /* The SIC bit is pending but stays masked: the real SIC_IMASK. */
    assert(bf531_read(s, MMR(0x120), 4) & (1u << 11));
    assert(bf531_read(s, MMR(0x10C), 4) == 0);
    bf531_write(s, DMA3(0x28), 1, 2);
    assert(!(bf531_read(s, MMR(0x120), 4) & (1u << 11)));

    /* An oversized receive takes a 100-byte burst and keeps running. */
    bf531_write(s, DMA3(0x04), 0x2000, 4);
    bf531_write(s, DMA3(0x10), 2048, 2);
    bf531_write(s, DMA3(0x08), 0x0001 | 0x0002 | 0x0004 | 0x0080, 2);
    script_n = 100;
    bf531_run(s, 800000);
    assert(ram[0x2000] == 0xD7 && ram[0x2063] == 0xD7 && ram[0x2064] == 0);
    assert(bf531_read(s, DMA3(0x30), 2) == 2048 - 50);
    assert(bf531_read(s, DMA3(0x24), 4) == 0x2064 && rx_addr == 0x2064);
    assert((bf531_read(s, DMA3(0x28), 2) & 0x9) == 0x8);   /* RUN, not DONE */
    bf531_write(s, DMA3(0x08), 0, 2);                      /* the firmware stops it */

    /* Transmit: the whole 200-byte unit, once TSPEN is set. */
    for (int i = 0; i < 200; i++) {
        bf531_write(s, 0x3000 + i, (uint8_t)(i + 1), 1);
    }
    bf531_write(s, DMA4(0x04), 0x3000, 4);
    bf531_write(s, DMA4(0x10), 100, 2);
    bf531_write(s, DMA4(0x14), 2, 2);
    bf531_write(s, DMA4(0x08), 0x0001 | 0x0004 | 0x0080, 2);
    bf531_run(s, 400000);
    assert(tx_len == 0);
    bf531_write(s, MMR(0x900), 1, 2);                       /* TSPEN */
    bf531_run(s, 400000);
    assert(tx_len == 200 && tx_seen[0] == 1 && tx_seen[199] == 200);
    assert(bf531_read(s, DMA4(0x28), 2) == 1);              /* DONE, RUN clear */
    assert(bf531_read(s, DMA4(0x30), 2) == 0);              /* CURR_X_COUNT */

    /* The flash's AMD program and sector erase. */
    assert(bf531_read(s, 0x201fc000, 2) == 0xFFFF);
    bf531_write(s, 0x20000aaa, 0xAA, 2);
    bf531_write(s, 0x20000554, 0x55, 2);
    bf531_write(s, 0x20000aaa, 0xA0, 2);
    bf531_write(s, 0x201fc000, 0x0300, 2);
    assert(bf531_read(s, 0x201fc000, 2) == 0x0300);
    bf531_write(s, 0x201fc002, 0x1234, 2);              /* not a command */
    assert(bf531_read(s, 0x201fc002, 2) == 0xFFFF);
    static const uint16_t erase[][2] = { { 0xaaa, 0xAA }, { 0x554, 0x55 }, { 0xaaa, 0x80 },
                                         { 0xaaa, 0xAA }, { 0x554, 0x55 } };
    for (int i = 0; i < 5; i++) {
        bf531_write(s, 0x20000000 + erase[i][0], erase[i][1], 2);
    }
    bf531_write(s, 0x201f0000, 0x30, 2);                /* the 32 KB sector only */
    assert(bf531_read(s, 0x201fc000, 2) == 0x0300);
    for (int i = 0; i < 5; i++) {
        bf531_write(s, 0x20000000 + erase[i][0], erase[i][1], 2);
    }
    bf531_write(s, 0x201fc000, 0x30, 2);                /* the 16 KB top boot block */
    assert(bf531_read(s, 0x201fc000, 2) == 0xFFFF);

    /* Straps on the PF port. */
    bf531_set_strap(s, 0x8, 0x8);
    assert(bf531_read(s, MMR(0x700), 2) & 0x8);
    bf531_free(s);
}

/* The dsp32alu forms bfin-link added (BYTEUNPACK, BYTEPACK, A0 += A1),
 * values worked from GNU sim's decode_dsp32alu_0. */
static void test_dsp_alu(void)
{
    static const uint16_t prog[] = {
        0xE100, 0x1100, 0xE140, 0x3322,         /* R0 = 0x33221100 */
        0xE101, 0x5544, 0xE141, 0x7766,         /* R1 = 0x77665544 */
        0xE110, 0x0001, 0xE150, 0x0000,         /* I0 = 1 */
        0xC418, 0x4040,                         /* (R1, R0) = BYTEUNPACK R1:0 */
        0xC418, 0x0401,                         /* R2 = BYTEPACK (R0, R1) */
        0xC409, 0x2010,                         /* A0 = R2 */
        0xC409, 0xA010,                         /* A1 = R2 */
        0xC40B, 0x0800,                         /* R4 = (A0 += A1) */
        0x2000,                                 /* JUMP.S 0 */
    };
    bf531_host host = { 0 };
    bf531 *s = bf531_new(1 << 20, &host, NULL);
    uint8_t ldr[10 + sizeof(prog)] = { 0x00, 0x80, 0xA0, 0xFF, sizeof(prog), 0, 0, 0, 0x00, 0x80 };
    bfin_core *c;

    memcpy(ldr + 10, prog, sizeof(prog));
    assert(bf531_boot_ldr(s, ldr, sizeof(ldr)) == 0);
    c = bf531_core(s);
    assert(bf531_run(s, 2000) == BFIN_STOP_BUDGET);
    assert(bfin_get_reg(c, 0, 0) == 0x00220011 && bfin_get_reg(c, 0, 1) == 0x00440033);
    assert(bfin_get_reg(c, 0, 2) == 0x44332211);
    assert(bfin_get_reg(c, 0, 4) == 0x7FFFFFFF);           /* saturated */
    bf531_free(s);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    dir = argv[1];
    unsetenv("BFIN_COSIM");
    test_framing_slots_fresh_only();
    test_native_partial_bursts();
    test_zero200_and_bootstrap();
    test_tx_and_captures();
    test_flat_peer();
    test_cosim();
    test_dma();
    test_dsp_alu();
    puts("ok");
    return 0;
}
