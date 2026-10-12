/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cdj-gui-run: the CDJ-2000NXS GUI board (BF531) on the vendored Blackfin
 * core, alone or linked to MAIN (cdj_link.c) in place of bin/cdj-run.
 *
 * Based on hw/cdj/bfin/bfinrun.c from Stijn Jacobs' cdj-nxs2-qemu
 * (https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, commit 08d5cb1).
 * Changed 2026-10-05: our inputs (UPD, update body, bare LDR or the 2 MiB
 * flash image bin/cdj-run's board file uses, plus an optional separate
 * flash), 64 MiB SDRAM, frames published as bin/cdj-run publishes them
 * (P6, the PPI's 480x255 capture, rgb555le expanded (v << 3) | (v >> 2),
 * written through a temporary file and a rename, unchanged frames skipped),
 * the PF0 ready toggle, a virtual-seconds budget and a stats line.
 * Changed 2026-10-05 (bfin-link): the MAIN link and co-simulation
 * (cdj_link.c), an ELF BOOT (gui-boot-memory.elf, as bin/cdj-run loads it),
 * wall-clock pacing, and bin/cdj-run's environment (below).
 *
 *   cdj-gui-run [-s seconds | -n cycles] [-o screen.ppm] [-f flash.bin]
 *               [-g mask] [-x lo:hi] [-q] BOOT
 *
 * The environment nxs_vm gives bin/cdj-run is read the same way:
 * BFIN_GUI_OUTPUT (-o), BFIN_GPIO5_READY_TOGGLE (-g 1, the default),
 * BFIN_GPIO_STRAP=mask:value, BFIN_STATS=<wall s> (a STATS line that often),
 * BFIN_EXIT_AFTER_WALL=<s> (exit 0 then; the budget is unbounded unless -s/-n
 * is given), cdj_link.h's link knobs, and the time base: BFIN_CCLK_HZ (core
 * cycles per second of guest time, 400 MHz), BFIN_PPI_FPS (60),
 * BFIN_SPORT_RETRY_US (1000), BFIN_WALL_LAG_MS (50). With a MAIN link the run
 * is paced to the wall clock at BFIN_CCLK_HZ, never ahead of it, unless
 * BFIN_TIME_BASE=virtual; with BFIN_COSIM it is virtual and kept within
 * MAIN's promise; alone it is unpaced unless BFIN_TIME_BASE=wall.
 *
 * -x reports how many 256-byte code lines in [lo, hi) ran (see
 * bfin_code_lines_run), e.g. a mod's extension range.
 *
 * BOOT is an ELF (with -f for the flash), a C2KGUI.UPD (0x20-byte title),
 * its body, a bare LDR stream, or a 2 MiB flash image: firmware/nxs/gui-flash-image.bin (stream at 0, the
 * image bin/cdj-run's board file maps) or a dump with the body at 0x10000.
 * Without -f the flash holds BOOT itself -- a UPD, body or LDR at 0x10000,
 * as the GUI's updater programs it.
 *
 * Exit status: 0 the budget ran out, 1 an unimplemented instruction (PC and
 * words on stderr) or an idle with nothing left to wake it, 2 bad input.
 */
#include "bf531.h"
#include "cdj_link.h"
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FLASH_APP 0x10000u

typedef struct runner {
    const char *out;
    char *tmp;
    uint8_t *rgb, *published;
    uint16_t *raw;              /* the last frame's pixels, as scanned */
    size_t rgb_size;
    unsigned w, h;
    uint64_t published_n;
    bf531 *s;
    cdj_link *link;
    double cclk;                /* core cycles per second of guest time */
    uint64_t epoch;             /* the cycle that is guest time 0 */
} runner;

static int64_t guest_ns(const runner *r)
{
    return (int64_t)((double)(bf531_cycles(r->s) - r->epoch) * 1e9 / r->cclk);
}

static void on_tx(void *opaque, const uint8_t *data, size_t len)
{
    runner *r = opaque;

    cdj_link_tx(r->link, data, (unsigned)len, guest_ns(r));
}

static unsigned on_rx(void *opaque, uint8_t *dst, unsigned cap, uint32_t addr)
{
    return cdj_link_rx(((runner *)opaque)->link, dst, cap, addr);
}

static void on_frame(void *opaque, const uint16_t *px, unsigned w, unsigned h)
{
    runner *r = opaque;
    size_t n = (size_t)w * h, size = n * 3;
    FILE *f;

    if (size != r->rgb_size) {
        r->rgb = realloc(r->rgb, size);
        r->published = realloc(r->published, size);
        r->raw = realloc(r->raw, n * 2);
        r->rgb_size = size;
        r->published_n = 0;
    } else if (!memcmp(r->raw, px, n * 2)) {
        return;                 /* the PPI rescans an unchanged picture */
    }
    memcpy(r->raw, px, n * 2);
    for (size_t k = 0; k < n; k++) {
        unsigned p = px[k], c[3] = { (p >> 10) & 31, (p >> 5) & 31, p & 31 };

        for (int j = 0; j < 3; j++) {
            r->rgb[k * 3 + j] = (c[j] << 3) | (c[j] >> 2);
        }
    }
    if (r->published_n && w == r->w && h == r->h &&
        !memcmp(r->rgb, r->published, size)) {
        return;
    }
    memcpy(r->published, r->rgb, size);
    r->w = w;
    r->h = h;
    r->published_n++;
    if (!(f = fopen(r->tmp, "wb"))) {
        perror(r->tmp);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(r->rgb, 1, size, f);
    if (fclose(f) == 0 && rename(r->tmp, r->out) != 0) {
        perror(r->out);
    }
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long size;

    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = malloc(size)) &&
        fread(buf, 1, size, f) == (size_t)size) {
        *len = size;
    } else {
        free(buf);
        buf = NULL;
        fprintf(stderr, "%s: cannot read\n", path);
    }
    fclose(f);
    return buf;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double env_num(const char *name, double fallback)
{
    const char *v = getenv(name);

    return v && *v ? strtod(v, NULL) : fallback;
}

/* bin/cdj-run's probes and diagnostics this runner does not have: say so
 * rather than run a qualification that silently lacks them. */
static void warn_unsupported(void)
{
    static const char *known[] = {
        "BFIN_GUI_OUTPUT", "BFIN_GUI_COLOR", "BFIN_GPIO5_READY_TOGGLE", "BFIN_GPIO_STRAP",
        "BFIN_STATS", "BFIN_EXIT_AFTER_WALL", "BFIN_CCLK_HZ", "BFIN_PPI_FPS",
        "BFIN_SPORT_RETRY_US", "BFIN_WALL_LAG_MS", "BFIN_TIME_BASE", "BFIN_MAIN_LINK",
        "BFIN_MAIN_LINK_DUMP", "BFIN_SPORT_TX_OUTPUT", "BFIN_LINK_FRESH_ONLY",
        "BFIN_LINK_NATIVE_PARTIAL_DMA", "BFIN_LINK_DEPTH", "BFIN_SPORT_RX_ZERO_200",
        "BFIN_LINK_NO_ZERO200", "BFIN_MAIN_PEER", "BFIN_MAIN_PEER_STATUS",
        "BFIN_MAIN_PEER_STATUS_HOLD", "BFIN_COSIM", "BFIN_COSIM_QUANTUM_US",
        /* gdb-only switches with nothing to do here */
        "BFIN_PARALLEL_WRITEBACK", "BFIN_EXCEPTION_TRACE",
    };
    extern char **environ;

    for (char **e = environ; *e; e++) {
        size_t n = strcspn(*e, "=");
        int ok = strncmp(*e, "BFIN_", 5) != 0;

        for (size_t k = 0; !ok && k < sizeof(known) / sizeof(*known); k++) {
            ok = strlen(known[k]) == n && !strncmp(*e, known[k], n);
        }
        if (!ok) {
            fprintf(stderr, "cdj-gui-run: %.*s is not supported here; ignored\n", (int)n, *e);
        }
    }
}

static void stats(runner *r, double wall)
{
    uint64_t c = bf531_cycles(r->s);
    double virt = (double)(c - r->epoch) / r->cclk;

    fprintf(stderr, "STATS cycles=%" PRIu64 " virtual_s=%.3f wall=%.3f speed=%.2fx "
            "frames=%" PRIu64 " published=%" PRIu64 " pc=0x%08x\n", c, virt, wall,
            virt / (wall > 0 ? wall : 1e-9), bf531_frames(r->s), r->published_n,
            bfin_get_pc(bf531_core(r->s)));
    if (cdj_link_enabled(r->link)) {
        cdj_link_stats(r->link, stderr);
    }
    fflush(stderr);
}

int main(int argc, char **argv)
{
    runner r = { .out = getenv("BFIN_GUI_OUTPUT") ? getenv("BFIN_GUI_OUTPUT") : "screen.ppm" };
    bf531_host host = { .opaque = &r, .frame = on_frame, .sport1_tx = on_tx,
                        .sport1_rx = on_rx };
    double exit_after = env_num("BFIN_EXIT_AFTER_WALL", 0);
    uint64_t cycles = exit_after > 0 ? UINT64_MAX : 60 * BF531_CCLK_HZ;
    const char *flash_path = NULL;
    uint32_t xlo = 0, xhi = 0;
    char *end;
    uint16_t toggle = 1;
    FILE *log = stderr;
    uint8_t *boot, *flash, *fimg = NULL;
    const uint8_t *ldr;
    size_t boot_len, ldr_len, fimg_len = 0;
    uint32_t flash_size;
    bfin_stop stop;
    double t0;
    bf531 *s;
    int opt;

    while ((opt = getopt(argc, argv, "s:n:o:f:g:x:q")) != -1) {
        switch (opt) {
        case 's': cycles = (uint64_t)(strtod(optarg, NULL) * BF531_CCLK_HZ); break;
        case 'n': cycles = strtoull(optarg, NULL, 0); break;
        case 'o': r.out = optarg; break;
        case 'f': flash_path = optarg; break;
        case 'g': toggle = strtoul(optarg, NULL, 0); break;
        case 'x':
            xlo = strtoul(optarg, &end, 0);
            xhi = *end == ':' ? strtoul(end + 1, NULL, 0) : xlo + 1;
            break;
        case 'q': log = NULL; break;
        default:
            fprintf(stderr, "usage: %s [-s seconds | -n cycles] [-o screen.ppm] "
                    "[-f flash.bin] [-g mask] [-x lo:hi] [-q] BOOT\n", argv[0]);
            return 2;
        }
    }
    if (optind + 1 != argc || !(boot = read_file(argv[optind], &boot_len))) {
        fprintf(stderr, "%s: one boot image (UPD, body, LDR or flash) needed\n", argv[0]);
        return 2;
    }
    if (flash_path && !(fimg = read_file(flash_path, &fimg_len))) {
        return 2;
    }
    r.tmp = malloc(strlen(r.out) + 5);
    strcpy(r.tmp, r.out);
    strcat(r.tmp, ".tmp");

    signal(SIGPIPE, SIG_IGN);
    warn_unsupported();
    r.link = cdj_link_new();
    r.cclk = env_num("BFIN_CCLK_HZ", BF531_CCLK_HZ);
    s = r.s = bf531_new(64u << 20, &host, log);
    bf531_set_timing(s, (uint64_t)(r.cclk / env_num("BFIN_PPI_FPS", 60)),
                     (uint64_t)(r.cclk * env_num("BFIN_SPORT_RETRY_US", 1000) / 1e6));
    if (getenv("BFIN_GPIO_STRAP") && *getenv("BFIN_GPIO_STRAP")) {
        uint16_t mask = strtoul(getenv("BFIN_GPIO_STRAP"), &end, 0);
        uint16_t value = *end == ':' ? strtoul(end + 1, NULL, 0) : mask;

        bf531_set_strap(s, mask, value);
        fprintf(stderr, "gpio: PF straps 0x%04x read as 0x%04x\n", mask, value & mask);
    }
    flash = bf531_flash(s, &flash_size);
    ldr = boot;
    ldr_len = boot_len;
    if (boot_len == flash_size) {
        /* A flash image: firmware.py's physical_flash_image puts the stream
         * at 0, the GUI's updater (a BFIN_CFI_DUMP) at 0x10000. */
        uint32_t first;

        memcpy(&first, boot, 4);
        if (first == 0xFFFFFFFFu) {
            ldr += FLASH_APP;
            ldr_len -= FLASH_APP;
        }
    } else if (boot_len > 0x20 && !memcmp(boot, "CDJ-", 4)) {   /* a UPD */
        ldr += 0x20;
        ldr_len -= 0x20;
    }
    if (fimg) {
        if (fimg_len > flash_size) {
            fprintf(stderr, "%s: larger than the 2 MiB flash\n", flash_path);
            return 2;
        }
        memcpy(flash, fimg, fimg_len);
    } else if (boot_len == flash_size) {
        memcpy(flash, boot, flash_size);
    } else if (ldr_len <= flash_size - FLASH_APP) {
        memcpy(flash + FLASH_APP, ldr, ldr_len);
    }
    if (boot_len >= 4 && !memcmp(boot, "\177ELF", 4) ? bf531_boot_elf(s, boot, boot_len)
                                                      : bf531_boot_ldr(s, ldr, ldr_len)) {
        fprintf(stderr, "%s: not an ELF or LDR boot stream\n", argv[optind]);
        return 2;
    }
    bf531_set_ready_toggle(s, toggle);

    /* The time base (see the top of this file). */
    const char *base = getenv("BFIN_TIME_BASE");
    int cosim = cdj_link_cosim(r.link);
    int paced = !cosim && (base ? !strcmp(base, "wall") : cdj_link_enabled(r.link));
    double lag = env_num("BFIN_WALL_LAG_MS", 50) / 1e3, stats_every = env_num("BFIN_STATS", 0);
    double next_stats = stats_every;
    uint64_t end_at = cycles == UINT64_MAX ? UINT64_MAX : bf531_cycles(s) + cycles;

    if (cosim) {
        cdj_link_cosim_connect(r.link);
    }
    r.epoch = bf531_cycles(s);
    fprintf(stderr, "cdj-gui-run: time base %s, cclk %.0f MHz, ppi %.0f fps, sport retry %.0f us\n",
            cosim ? "cosim" : paced ? "wall" : "virtual", r.cclk / 1e6,
            env_num("BFIN_PPI_FPS", 60), env_num("BFIN_SPORT_RETRY_US", 1000));
    double start = t0 = now();
    stop = BFIN_STOP_BUDGET;
    while (stop == BFIN_STOP_BUDGET && bf531_cycles(s) < end_at) {
        uint64_t c = bf531_cycles(s);
        /* Half a millisecond of guest time between looks at the wall clock,
         * MAIN and the stats; unpaced and alone, a whole second. */
        uint64_t slice = (uint64_t)(r.cclk / (paced || cosim ? 2000 : 1));
        double wall;

        if (cosim) {
            int64_t to = cdj_link_cosim_step(r.link, guest_ns(&r));
            uint64_t at = r.epoch + (uint64_t)((double)to * r.cclk / 1e9);

            slice = at > c ? at - c : 1;
        }
        slice = slice < end_at - c ? slice : end_at - c;
        stop = bf531_run(s, slice ? slice : 1);
        wall = now() - t0;
        if (paced) {
            double ahead = (double)(bf531_cycles(s) - r.epoch) / r.cclk - wall;

            if (ahead > 0.0003) {
                double sleep_s = ahead - 0.0002;
                struct timespec ts = { (time_t)sleep_s,
                                       (long)((sleep_s - (time_t)sleep_s) * 1e9) };

                nanosleep(&ts, NULL);
            } else if (-ahead > lag) {
                t0 += -ahead - lag;     /* forgive the excess once */
            }
        }
        if (stats_every > 0 && wall >= next_stats) {
            stats(&r, wall);
            next_stats += stats_every;
        }
        if (exit_after > 0 && now() - start >= exit_after) {
            break;
        }
    }
    stats(&r, now() - start);
    bfin_core *c = bf531_core(s);
    if (xhi > xlo) {
        fprintf(stderr, "RANGE 0x%08x:0x%08x lines_run=%u\n", xlo, xhi,
                bfin_code_lines_run(c, xlo, xhi));
    }
    if (stop == BFIN_STOP_UNDEF) {
        /* bfin_trap_insn: iw0 << 16 | iw1, and a 64-bit bundle's two more
         * words below those. (bfinrun printed a 16-bit form's padding word
         * instead of the instruction.) */
        uint64_t insn = bfin_trap_insn(c);
        unsigned top = insn >> 32 ? 48 : 16;
        unsigned len = bfin_insn_len(insn >> top);

        fprintf(stderr, "unimplemented at 0x%08x:", bfin_trap_pc(c));
        for (unsigned n = 0; n < len / 2; n++) {
            fprintf(stderr, " %04x", (unsigned)(insn >> (top - 16 * n)) & 0xFFFF);
        }
        fputc('\n', stderr);
    } else if (stop == BFIN_STOP_IDLE) {
        fprintf(stderr, "idle with no event pending\n");
    } else if (stop == BFIN_STOP_BREAK) {
        fprintf(stderr, "break at 0x%08x\n", bfin_get_pc(c));
    }
    bf531_free(s);
    cdj_link_free(r.link);
    free(boot);
    free(fimg);
    return stop != BFIN_STOP_BUDGET;
}
