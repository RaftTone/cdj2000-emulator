/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * From hw/cdj/bfin/bf531.c of Stijn Jacobs' cdj-nxs2-qemu,
 * https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, commit 08d5cb1.
 * Changed 2026-10-05 for cdj-gui-run: bf531_boot_ldr split out of
 * bf531_load_update, bf531_flash, the PF ready toggle on flag-register reads
 * (bf531_set_ready_toggle), the async bank 3 latch at 0x20300000, and GP
 * timers without TIMER_IRQ_ENA no longer end a step (next_event/advance).
 * Changed 2026-10-05 for the MAIN link (bfin-link): SPORT1 RX/TX DMA as
 * byte pumps to the host (sport1_rx_pump, sport1_tx_send) with live
 * CURR_ADDR/CURR_X_COUNT, partial bursts and retries, descriptor flows on
 * every channel, SPORT1 registers modelled, the real SIC mask (no forced
 * DMA3 bit), PF straps, ELF boot, a configurable frame period, and the
 * flash's AMD program/erase commands (the GUI saves settings at run time).
 * Changed 2026-10-07: sector erase follows the MX29LV160DT top-boot layout.
 */
/*
 * ADSP-BF531 SoC model (see bf531.h). System MMRs sit at 0xFFC00000; the
 * register offsets below are relative to that base and follow the BF533
 * Hardware Reference. What the display firmware does not use is a plain
 * register file that logs its first access, so the next block to model shows
 * up in the log.
 */
#include "bf531.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SYS_BASE    0xFFC00000u
#define SYS_SIZE    0x2000u

#define ASYNC_BASE  0x20000000u
#define FLASH_SIZE  (2u << 20)
/* The update section (after its title) sits at flash +0x10000: the firmware
 * reads its resources at 0x200EBBA0, which is file offset 0xDBBC0 there.
 * The first 64 KB hold the loader the boot ROM runs, which no update
 * carries; it stays erased, since the model boots the stream directly. */
#define FLASH_APP   0x10000u
/* Async bank 3: a write-only board latch, not memory. The GUI writes 0 and 2
 * there during boot (what it drives is not identified); our gdb sim leaves
 * the bank empty, so the value is only kept to read back. */
#define LATCH_BASE  0x20300000u
#define LATCH_SIZE  0x100000u

#define L1_DATA_A   0xFF800000u
#define L1_DATA_B   0xFF900000u
#define L1_CODE     0xFFA00000u
#define L1_SCRATCH  0xFFB00000u

/* The BF531/532 boot ROM enters the loaded program at the start of their
 * only L1 instruction bank. */
#define BOOT_ENTRY  0xFFA08000u

enum {
    PLL_CTL = 0x000, PLL_DIV = 0x004, PLL_STAT = 0x00C, CHIPID = 0x014,
    SIC_IMASK = 0x10C, SIC_IAR0 = 0x110, SIC_ISR = 0x120, SIC_IWR = 0x124,
    SPI_CTL = 0x500, SPI_STAT = 0x508, SPI_TDBR = 0x50C, SPI_RDBR = 0x510,
    SPORT1_TCR1 = 0x900, SPORT1_END = 0x960,
    TIMER0 = 0x600, TIMER_ENABLE = 0x640, TIMER_DISABLE = 0x644,
    TIMER_STATUS = 0x648,
    FIO_FLAG_D = 0x700, FIO_FLAG_C = 0x704, FIO_FLAG_S = 0x708,
    FIO_FLAG_T = 0x70C, FIO_DIR = 0x730,
    EBIU_SDSTAT = 0xA1C,
    DMA0 = 0xC00, DMA_END = 0xE00,
    PPI_CONTROL = 0x1000,
};

/* DMA channel registers, offsets within a channel's 0x40 bytes. */
enum {
    D_NEXT = 0x00, D_START = 0x04, D_CONFIG = 0x08, D_XCOUNT = 0x10,
    D_XMOD = 0x14, D_YCOUNT = 0x18, D_YMOD = 0x1C, D_CURR_DESC = 0x20,
    D_CURR_ADDR = 0x24, D_IRQ_STATUS = 0x28, D_PMAP = 0x2C,
    D_CURR_X = 0x30, D_CURR_Y = 0x38,
};

#define DMAEN       0x0001
#define TSPEN       0x0001
#define WNR         0x0002
#define DMA2D       0x0010
#define DI_EN       0x0080
#define DMA_DONE    0x0001
#define DMA_RUN     0x0008

/* SIC peripheral interrupt numbers. */
enum { IRQ_PPI_DMA = 8, IRQ_TIMER0 = 16 };

#define TIMER_PWM_OUT   0x0001
#define TIMER_IRQ_ENA   0x0010

#define NEVER UINT64_MAX

typedef struct bf531_timer {
    uint32_t config, counter, period, width;
    uint64_t due;
} bf531_timer;

typedef struct bf531_dma {
    uint32_t reg[0x40 / 4];
    uint64_t due;
} bf531_dma;

struct bf531 {
    bfin_core *core;
    bf531_host host;
    FILE *log;

    uint8_t *sdram;
    uint32_t sdram_size;
    uint8_t  l1_data_a[0x8000], l1_data_b[0x8000], l1_code[0x14000];
    uint8_t  l1_scratch[0x1000];
    uint8_t  flash[FLASH_SIZE];

    uint32_t mmr[SYS_SIZE / 4];
    uint8_t  logged[SYS_SIZE / 4];
    uint32_t sic_isr;

    bf531_timer timer[3];
    uint32_t timer_status;
    bf531_dma dma[8];
    uint16_t *fb;
    uint64_t frames;

    /* Core cycles of one display frame and of a SPORT receive retry. */
    uint64_t ppi_frame, sport_retry;

    /* CDJ board additions (see bf531.h). */
    uint16_t strap_mask, strap_value;
    uint16_t ready_toggle;
    unsigned ready_phase;
    unsigned flash_cycle, flash_program;
    uint32_t latch;
};

static void __attribute__((format(printf, 2, 3))) slog(bf531 *s, const char *fmt, ...)
{
    va_list ap;

    if (s->log) {
        va_start(ap, fmt);
        fprintf(s->log, "bf531: ");
        vfprintf(s->log, fmt, ap);
        fputc('\n', s->log);
        va_end(ap);
    }
}

/* SCLK cycles to core cycles: CCLK = VCO >> CSEL, SCLK = VCO / SSEL. */
static uint64_t sclk_to_cclk(bf531 *s, uint64_t n)
{
    uint32_t div = s->mmr[PLL_DIV / 4];
    unsigned ssel = div & 0xF ? div & 0xF : 5, csel = (div >> 4) & 3;

    return (n * ssel) >> csel;
}

/* ---- SIC ---------------------------------------------------------------- */

static void sic_update(bf531 *s)
{
    uint32_t pend = s->sic_isr & s->mmr[SIC_IMASK / 4];
    uint32_t level = 0;

    for (int n = 0; n < 24; n++) {
        if (pend & (1u << n)) {
            level |= 1u << (((s->mmr[SIC_IAR0 / 4 + n / 8] >> (4 * (n % 8))) & 0xF) + 7);
        }
    }
    for (int ivg = 7; ivg < 16; ivg++) {
        bfin_set_ivg(s->core, ivg, (level >> ivg) & 1);
    }
}

static void sic_set(bf531 *s, int irq, int on)
{
    if (on) {
        s->sic_isr |= 1u << irq;
    } else {
        s->sic_isr &= ~(1u << irq);
    }
    sic_update(s);
}

/* ---- GP timers ---------------------------------------------------------- */

static void timer_arm(bf531 *s, int n)
{
    bf531_timer *t = &s->timer[n];

    t->due = (t->config & 3) == TIMER_PWM_OUT && t->period ?
        bfin_cycles(s->core) + sclk_to_cclk(s, t->period) : NEVER;
    bfin_yield(s->core);
}

static void timer_fire(bf531 *s, int n)
{
    bf531_timer *t = &s->timer[n];

    if (t->config & TIMER_IRQ_ENA) {
        s->timer_status |= 1u << n;
        sic_set(s, IRQ_TIMER0 + n, 1);
    }
    t->due = t->period ? t->due + sclk_to_cclk(s, t->period) : NEVER;
}

static uint32_t timer_read(bf531 *s, uint32_t off)
{
    if (off >= TIMER_ENABLE) {
        switch (off) {
        case TIMER_ENABLE:
        case TIMER_DISABLE: {
            uint32_t on = 0;

            for (int n = 0; n < 3; n++) {
                on |= (s->timer[n].due != NEVER) << n;
            }
            return on;
        }
        case TIMER_STATUS: {
            uint32_t st = s->timer_status;

            for (int n = 0; n < 3; n++) {
                st |= (uint32_t)(s->timer[n].due != NEVER) << (12 + n);
            }
            return st;
        }
        }
        return 0;
    }
    bf531_timer *t = &s->timer[(off - TIMER0) / 0x10];

    switch (off & 0xF) {
    case 0x0: return t->config;
    case 0x4: return t->counter;
    case 0x8: return t->period;
    default:  return t->width;
    }
}

static void timer_write(bf531 *s, uint32_t off, uint32_t v)
{
    if (off >= TIMER_ENABLE) {
        for (int n = 0; n < 3; n++) {
            if (!(v & (1u << n))) {
                continue;
            }
            if (off == TIMER_ENABLE) {
                timer_arm(s, n);
            } else if (off == TIMER_DISABLE) {
                s->timer[n].due = NEVER;
            }
        }
        if (off == TIMER_STATUS) {
            s->timer_status &= ~(v & 7);
            for (int n = 0; n < 3; n++) {
                sic_set(s, IRQ_TIMER0 + n, (s->timer_status >> n) & 1);
            }
        }
        return;
    }
    bf531_timer *t = &s->timer[(off - TIMER0) / 0x10];

    switch (off & 0xF) {
    case 0x0: t->config = v; break;
    case 0x4: t->counter = v; break;
    case 0x8: t->period = v; break;
    default:  t->width = v; break;
    }
}

/* ---- DMA and the PPI ---------------------------------------------------- */

static uint8_t *load_target(bf531 *s, uint32_t addr, uint32_t len)
{
    if (addr < s->sdram_size && len <= s->sdram_size - addr) {
        return s->sdram + addr;
    }
    if (addr - L1_CODE < sizeof(s->l1_code) && len <= sizeof(s->l1_code) - (addr - L1_CODE)) {
        return s->l1_code + (addr - L1_CODE);
    }
    if (addr - L1_DATA_A < sizeof(s->l1_data_a) && len <= sizeof(s->l1_data_a) - (addr - L1_DATA_A)) {
        return s->l1_data_a + (addr - L1_DATA_A);
    }
    if (addr - L1_DATA_B < sizeof(s->l1_data_b) && len <= sizeof(s->l1_data_b) - (addr - L1_DATA_B)) {
        return s->l1_data_b + (addr - L1_DATA_B);
    }
    return NULL;
}

static uint32_t mem_read(bf531 *s, uint32_t addr, unsigned size);

static uint16_t desc_word(bf531 *s, uint32_t *p)
{
    uint16_t v = mem_read(s, *p, 2);

    *p += 2;
    return v;
}

/* Loads the next descriptor for the large-list, small-list and array flows:
 * NDSIZE 16-bit words in the order NEXT_DESC (large list only), START,
 * CONFIG, X_COUNT, X_MODIFY, Y_COUNT, Y_MODIFY. */
static void dma_fetch(bf531 *s, bf531_dma *d)
{
    uint32_t *r = d->reg;
    unsigned flow = (r[D_CONFIG / 4] >> 12) & 7;
    unsigned nd = (r[D_CONFIG / 4] >> 8) & 0xF;
    uint32_t p = flow == 4 ? r[D_CURR_DESC / 4] : r[D_NEXT / 4];
    static const uint8_t order[] = { D_START, D_CONFIG, D_XCOUNT, D_XMOD,
                                     D_YCOUNT, D_YMOD };
    unsigned k = 0;

    if (flow == 6) {
        r[D_NEXT / 4] = (r[D_NEXT / 4] & 0xFFFF0000) | desc_word(s, &p);
        k = 1;
    } else if (flow == 7) {
        r[D_NEXT / 4] = desc_word(s, &p);
        r[D_NEXT / 4] |= (uint32_t)desc_word(s, &p) << 16;
        k = 2;
    }
    for (unsigned i = 0; k < nd && i < sizeof(order); i++) {
        uint32_t v;

        if (order[i] == D_START) {
            v = desc_word(s, &p);
            v |= k + 1 < nd ? (uint32_t)desc_word(s, &p) << 16 : 0;
            k += 2;
        } else {
            v = desc_word(s, &p);
            k++;
        }
        r[order[i] / 4] = v;
    }
    r[D_CURR_DESC / 4] = flow == 4 ? p : r[D_NEXT / 4];
}

static unsigned dma_rows(const bf531_dma *d)
{
    return d->reg[D_CONFIG / 4] & DMA2D ? (d->reg[D_YCOUNT / 4] & 0xFFFF) : 1;
}

/* Starts a work unit. The PPI clocks one 16-bit word per pixel. Its clock
 * is not modelled: a work unit takes the time a 60 Hz panel (ppi_frame)
 * spends on its share of a 525-line frame, which is all the firmware can
 * observe of it. SPORT1 receive (DMA3) pumps at once; SPORT1 transmit (DMA4)
 * goes out once TSPEN is also set: the firmware enables DMA4, fills the
 * buffer, then sets TSPEN. */
static void dma_start_unit(bf531 *s, int ch)
{
    bf531_dma *d = &s->dma[ch];
    uint64_t now = bfin_cycles(s->core);

    d->reg[D_CURR_ADDR / 4] = d->reg[D_START / 4];
    d->reg[D_CURR_X / 4] = d->reg[D_XCOUNT / 4] & 0xFFFF;
    d->reg[D_CURR_Y / 4] = d->reg[D_YCOUNT / 4] & 0xFFFF;
    d->reg[D_IRQ_STATUS / 4] |= DMA_RUN;
    switch (ch) {
    case 0:
        d->due = now + dma_rows(d) * (s->ppi_frame / 262);
        break;
    case 3:
        d->due = now;
        break;
    case 4:
        d->due = s->mmr[SPORT1_TCR1 / 4] & TSPEN ? now : NEVER;
        break;
    default:
        d->due = NEVER;
        break;
    }
    bfin_yield(s->core);
}

static void ppi_frame(bf531 *s, bf531_dma *d)
{
    uint32_t *r = d->reg;
    unsigned w = r[D_XCOUNT / 4] & 0xFFFF, h = dma_rows(d);
    int32_t xmod = (int16_t)r[D_XMOD / 4], ymod = (int16_t)r[D_YMOD / 4];
    uint32_t a = r[D_START / 4];

    if (w < 64 || h < 64 || w > 1024 || h > 1024) {
        return;
    }
    s->fb = realloc(s->fb, (size_t)w * h * 2);
    for (unsigned y = 0; y < h; y++) {
        uint8_t *row = xmod == 2 ? load_target(s, a, w * 2) : NULL;

        if (row) {
            memcpy(&s->fb[y * w], row, w * 2);
            a += (w - 1) * 2 + ymod;
            continue;
        }
        for (unsigned x = 0; x < w; x++) {
            s->fb[y * w + x] = mem_read(s, a, 2);
            a += x + 1 < w ? xmod : ymod;
        }
    }
    s->frames++;
    if (s->host.frame) {
        s->host.frame(s->host.opaque, s->fb, w, h);
    }
}

static void dma_unit_done(bf531 *s, int ch)
{
    bf531_dma *d = &s->dma[ch];
    uint32_t cfg = d->reg[D_CONFIG / 4];
    unsigned flow = (cfg >> 12) & 7;

    if (ch == 0) {
        ppi_frame(s, d);
    }
    if (cfg & DI_EN) {
        d->reg[D_IRQ_STATUS / 4] |= DMA_DONE;
        sic_set(s, IRQ_PPI_DMA + ch, 1);
    }
    switch (flow) {
    case 0:
        d->reg[D_IRQ_STATUS / 4] &= ~DMA_RUN;
        d->due = NEVER;
        return;
    case 1:
        break;
    default:
        dma_fetch(s, d);
        break;
    }
    dma_start_unit(s, ch);
}

static unsigned dma_elem(const bf531_dma *d)
{
    return 1u << ((d->reg[D_CONFIG / 4] >> 2) & 3);
}

/* One pump of DMA3, SPORT1 receive, as bin/cdj-run's DMA engine pumps its
 * peer: the host hands over what MAIN has sent, at most what is left of the
 * unit (and 4 KB, the longest record). Nothing: try again after the SPORT
 * retry time. A short burst lands and the channel keeps running for the
 * rest, CURR_ADDR and CURR_X_COUNT live, as a native partial DMA does. */
static void sport1_rx_pump(bf531 *s)
{
    bf531_dma *d = &s->dma[3];
    unsigned esize = dma_elem(d);
    unsigned cap = (d->reg[D_CURR_X / 4] & 0xFFFF) * esize;
    uint32_t addr = d->reg[D_CURR_ADDR / 4];
    uint8_t buf[4096];
    unsigned n;
    uint8_t *to;

    cap = cap < sizeof(buf) ? cap : sizeof(buf);
    n = s->host.sport1_rx ? s->host.sport1_rx(s->host.opaque, buf, cap, addr) : 0;
    n -= n % esize;
    if (!n || n > cap) {
        d->due = bfin_cycles(s->core) + s->sport_retry;
        return;
    }
    if ((to = load_target(s, addr, n))) {
        memcpy(to, buf, n);
    } else {
        slog(s, "SPORT1 RX of %u bytes to 0x%08x has no target", n, addr);
    }
    d->reg[D_CURR_ADDR / 4] = addr + n;
    d->reg[D_CURR_X / 4] -= n / esize;
    if (d->reg[D_CURR_X / 4] & 0xFFFF) {
        d->due = bfin_cycles(s->core) + 1;
    } else {
        dma_unit_done(s, 3);
    }
}

/* Hands DMA4's whole unit (SPORT1 TX, a request to MAIN) to the host and
 * completes it, once both the channel and the transmitter are on. */
static void sport1_tx_send(bf531 *s)
{
    bf531_dma *d = &s->dma[4];
    size_t len = (d->reg[D_XCOUNT / 4] & 0xFFFF) * dma_elem(d);
    static uint8_t pkt[0x10000 * 4];     /* X_COUNT's ceiling of words */

    for (size_t i = 0; i < len; i++) {
        pkt[i] = mem_read(s, d->reg[D_START / 4] + i, 1);
    }
    if (s->host.sport1_tx) {
        s->host.sport1_tx(s->host.opaque, pkt, len);
    }
    /* The unit has gone out: firmware polls CURR_X_COUNT reaching 0. */
    d->reg[D_CURR_ADDR / 4] = d->reg[D_START / 4] + len;
    d->reg[D_CURR_X / 4] = 0;
    dma_unit_done(s, 4);
}

static void dma_write(bf531 *s, uint32_t off, uint32_t v)
{
    int ch = (off - DMA0) / 0x40;
    bf531_dma *d = &s->dma[ch];
    uint32_t reg = off & 0x3F;

    if (reg == D_IRQ_STATUS) {
        d->reg[reg / 4] &= ~(v & 3);
        sic_set(s, IRQ_PPI_DMA + ch, d->reg[reg / 4] & DMA_DONE);
        return;
    }
    d->reg[reg / 4] = v;
    if (reg != D_CONFIG) {
        return;
    }
    if (!(v & DMAEN)) {
        d->due = NEVER;
        d->reg[D_IRQ_STATUS / 4] &= ~DMA_RUN;
        return;
    }
    /* The PPI channel moves frames; DMA3 and DMA4 are SPORT1's receive and
     * transmit, MAIN's link. */
    if (((v >> 12) & 7) >= 4) {
        if (((v >> 12) & 7) == 4) {
            d->reg[D_CURR_DESC / 4] = d->reg[D_NEXT / 4];
        }
        dma_fetch(s, d);
    }
    if (ch != 0 && ch != 3 && ch != 4) {
        slog(s, "DMA%u enabled, config 0x%04x, start 0x%08x, %u words (unmodelled)",
             ch, v, d->reg[D_START / 4], d->reg[D_XCOUNT / 4] & 0xFFFF);
    }
    dma_start_unit(s, ch);
}

static uint32_t dma_read(bf531 *s, uint32_t off)
{
    return s->dma[(off - DMA0) / 0x40].reg[(off & 0x3F) / 4];
}

/* ---- system MMRs -------------------------------------------------------- */

static int mmr_modelled(uint32_t off)
{
    return off < 0x020 || (off >= 0x100 && off < 0x128) ||
           (off >= 0x500 && off < 0x520) || (off >= 0x600 && off < 0x650) ||
           (off >= 0x700 && off < 0x750) || (off >= 0xA00 && off < 0xA20) ||
           (off >= SPORT1_TCR1 && off < SPORT1_END) ||
           (off >= DMA0 && off < DMA_END) || (off >= PPI_CONTROL && off < 0x1014);
}

static uint32_t sys_read(bf531 *s, uint32_t off)
{
    if (!mmr_modelled(off) && !s->logged[off / 4]) {
        s->logged[off / 4] = 1;
        slog(s, "read 0x%08x (unmodelled)", SYS_BASE + off);
    }
    if (off >= DMA0 && off < DMA_END) {
        return dma_read(s, off);
    }
    if (off >= TIMER0 && off < TIMER_STATUS + 4) {
        return timer_read(s, off);
    }
    switch (off) {
    case PLL_STAT:  return 0x20;              /* PLL_LOCKED */
    case CHIPID:    return 0x027A50CB;
    case SIC_ISR:   return s->sic_isr;
    case SPI_STAT:  return 0x21;              /* SPIF, RXS: a transfer is done */
    case SPI_RDBR:  return 0;
    case FIO_FLAG_D: case FIO_FLAG_C: case FIO_FLAG_S: case FIO_FLAG_T: {
        /* The flash READY/BUSY line on PF0 (BFIN_GPIO5_READY_TOGGLE in
         * patches/02): with no flash state machine behind it, every read
         * flips the masked bits, so a wait for either level ends. */
        uint32_t d = s->mmr[FIO_FLAG_D / 4];

        if (s->ready_toggle) {
            d = (d & ~s->ready_toggle) | (s->ready_phase++ & 1 ? s->ready_toggle : 0);
        }
        return (d & ~s->strap_mask) | (s->strap_value & s->strap_mask);
    }
    case EBIU_SDSTAT: return 0x8;             /* SDRS: SDRAM powered up */
    }
    return s->mmr[off / 4];
}

static void sys_write(bf531 *s, uint32_t off, uint32_t v)
{
    if (!mmr_modelled(off) && !s->logged[off / 4]) {
        s->logged[off / 4] = 1;
        slog(s, "write 0x%08x = 0x%x (unmodelled)", SYS_BASE + off, v);
    }
    if (off >= DMA0 && off < DMA_END) {
        dma_write(s, off, v);
        return;
    }
    if (off >= TIMER0 && off < TIMER_STATUS + 4) {
        timer_write(s, off, v);
        return;
    }
    switch (off) {
    case PLL_CTL:
        /* The relock ends the IDLE the firmware issues next. */
        s->mmr[off / 4] = v;
        bfin_wake(s->core);
        return;
    case PLL_STAT: case CHIPID: case SIC_ISR:
        return;
    case SPORT1_TCR1:
        if (v & ~s->mmr[off / 4] & TSPEN &&
            s->dma[4].reg[D_IRQ_STATUS / 4] & DMA_RUN && s->dma[4].due == NEVER) {
            s->dma[4].due = bfin_cycles(s->core);
            bfin_yield(s->core);
        }
        break;
    case FIO_FLAG_C: s->mmr[FIO_FLAG_D / 4] &= ~v; return;
    case FIO_FLAG_S: s->mmr[FIO_FLAG_D / 4] |= v; return;
    case FIO_FLAG_T: s->mmr[FIO_FLAG_D / 4] ^= v; return;
    }
    s->mmr[off / 4] = v;
    if (off == SIC_IMASK || (off >= SIC_IAR0 && off < SIC_IAR0 + 12)) {
        sic_update(s);
    }
}

/* ---- bus ---------------------------------------------------------------- */

static uint32_t mem_read(bf531 *s, uint32_t addr, unsigned size)
{
    uint8_t *h = load_target(s, addr, size);
    uint32_t v = 0;

    if (!h && addr - ASYNC_BASE < FLASH_SIZE) {
        h = s->flash + (addr - ASYNC_BASE);
    }
    if (h) {
        memcpy(&v, h, size);
        return v;
    }
    if (addr - SYS_BASE < SYS_SIZE) {
        return sys_read(s, addr - SYS_BASE);
    }
    if (addr - LATCH_BASE < LATCH_SIZE) {
        return s->latch;
    }
    slog(s, "read 0x%08x size %u (unmapped)", addr, size);
    return 0;
}

/* The flash's AMD command set (cmdset 2, as the gdb board file's CFI
 * device): unlock AA/55 at word 0x555/0x2AA, then A0 programs the next word
 * (bits can only clear), 80 + unlock + 30 erases the sector, + 10 the
 * chip. Reads stay in array mode, so the firmware's DQ7/DQ6 status polls
 * see the operation already done. Only the in-memory copy changes. */
/* The NXS GUI's MX29LV160DT is a top-boot part: 31 64 KB sectors, then
 * 32, 8, 8 and 16 KB. The installer (0x00d09db0) erases 0x1F0000, 0x1F8000
 * and 0x1FA000 separately and leaves 0x1FC000 (the brightness journal)
 * alone, which a uniform 64 KB model would wipe with them. */
static void flash_sector(uint32_t off, uint32_t *at, uint32_t *len)
{
    static const uint32_t top[][2] = {
        { 0x1FC000, 0x4000 }, { 0x1FA000, 0x2000 }, { 0x1F8000, 0x2000 },
        { 0x1F0000, 0x8000 },
    };

    for (unsigned i = 0; i < 4; i++) {
        if (off >= top[i][0]) {
            *at = top[i][0];
            *len = top[i][1];
            return;
        }
    }
    *at = off & ~0xFFFFu;
    *len = 0x10000;
}

static void flash_write(bf531 *s, uint32_t off, uint32_t v, unsigned size)
{
    unsigned word = (off >> 1) & 0x7FF;
    uint8_t cmd = v;

    if (s->flash_program) {
        s->flash_program = 0;
        for (unsigned i = 0; i < size && off + i < FLASH_SIZE; i++) {
            s->flash[off + i] &= v >> (8 * i);
        }
        return;
    }
    switch (s->flash_cycle) {
    case 0: case 3:
        if (word == 0x555 && cmd == 0xAA) {
            s->flash_cycle++;
            return;
        }
        break;
    case 1: case 4:
        if (word == 0x2AA && cmd == 0x55) {
            s->flash_cycle++;
            return;
        }
        break;
    case 2:
        if (word == 0x555 && cmd == 0xA0) {
            s->flash_program = 1;
        } else if (word == 0x555 && cmd == 0x80) {
            s->flash_cycle = 3;
            return;
        }
        break;
    case 5:
        if (cmd == 0x30 || (cmd == 0x10 && word == 0x555)) {
            uint32_t at = 0, len = FLASH_SIZE;

            if (cmd == 0x30) {
                flash_sector(off, &at, &len);
            }
            memset(s->flash + at, 0xFF, len);
            slog(s, "flash erase 0x%08x +0x%x", ASYNC_BASE + at, len);
        }
        break;
    }
    s->flash_cycle = 0;
}

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    return mem_read(opaque, addr, size);
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    bf531 *s = opaque;

    if (addr - SYS_BASE < SYS_SIZE) {
        sys_write(s, addr - SYS_BASE, val);
    } else if (addr - ASYNC_BASE < FLASH_SIZE) {
        flash_write(s, addr - ASYNC_BASE, val, size);
    } else if (addr - LATCH_BASE < LATCH_SIZE) {
        s->latch = val;
    } else {
        slog(s, "write 0x%08x = 0x%x (unmapped)", addr, val);
    }
}

/* ---- the chip ----------------------------------------------------------- */

bf531 *bf531_new(uint32_t sdram_size, const bf531_host *host, FILE *log)
{
    bf531 *s = calloc(1, sizeof(*s));
    bfin_bus bus = { s, bus_read, bus_write };
    static const uint32_t iar[3] = { 0x10000000, 0x33322221, 0x66655444 };

    s->host = *host;
    s->log = log;
    s->sdram_size = sdram_size;
    s->sdram = calloc(1, sdram_size);
    memset(s->flash, 0xFF, sizeof(s->flash));
    s->core = bfin_new(&bus);
    bfin_map_ram(s->core, 0, sdram_size, s->sdram);
    bfin_map_ram(s->core, L1_CODE, sizeof(s->l1_code), s->l1_code);
    bfin_map_ram(s->core, L1_DATA_A, sizeof(s->l1_data_a), s->l1_data_a);
    bfin_map_ram(s->core, L1_DATA_B, sizeof(s->l1_data_b), s->l1_data_b);
    bfin_map_ram(s->core, L1_SCRATCH, sizeof(s->l1_scratch), s->l1_scratch);
    bfin_map_rom(s->core, ASYNC_BASE, FLASH_SIZE, s->flash);

    /* Reset values: PLL_DIV SSEL 5, the default SIC assignment (PPI DMA
     * IVG8, SPORT DMA IVG9, the timers IVG11). */
    s->mmr[PLL_DIV / 4] = 0x0005;
    for (int n = 0; n < 3; n++) {
        s->mmr[SIC_IAR0 / 4 + n] = iar[n];
        s->timer[n].due = NEVER;
    }
    for (int n = 0; n < 8; n++) {
        s->dma[n].due = NEVER;
    }
    s->ppi_frame = BF531_CCLK_HZ / 60;
    s->sport_retry = BF531_CCLK_HZ / 1000;
    return s;
}

void bf531_free(bf531 *s)
{
    bfin_free(s->core);
    free(s->sdram);
    free(s->fb);
    free(s);
}

void bf531_set_strap(bf531 *s, uint16_t mask, uint16_t value)
{
    s->strap_mask = mask;
    s->strap_value = value;
}

void bf531_set_timing(bf531 *s, uint64_t ppi_frame, uint64_t sport_retry)
{
    s->ppi_frame = ppi_frame ? ppi_frame : 1;
    s->sport_retry = sport_retry ? sport_retry : 1;
}

uint32_t bf531_read(bf531 *s, uint32_t addr, unsigned size)
{
    return mem_read(s, addr, size);
}

void bf531_write(bf531 *s, uint32_t addr, uint32_t val, unsigned size)
{
    uint8_t *h = load_target(s, addr, size);

    if (h) {
        memcpy(h, &val, size);
    } else {
        bus_write(s, addr, val, size);
    }
}

uint16_t bf531_flags(const bf531 *s)
{
    return s->mmr[FIO_FLAG_D / 4] & s->mmr[FIO_DIR / 4];
}

/* LDR block: u32 destination, u32 byte count, u16 flags; bit 0 zero-fill
 * (no payload), bit 4 ignore (payload skipped), bit 15 the last block. */
int bf531_boot_ldr(bf531 *s, const uint8_t *img, size_t len)
{
    size_t off = 0;

    while (off + 10 <= len) {
        uint32_t dest, count;
        uint16_t flags;
        uint8_t *to;

        memcpy(&dest, img + off, 4);
        memcpy(&count, img + off + 4, 4);
        memcpy(&flags, img + off + 8, 2);
        off += 10;
        if (!(flags & 1) && count > len - off) {
            return -1;
        }
        if (!(flags & 0x10)) {
            to = load_target(s, dest, count);
            if (!to) {
                slog(s, "LDR block to 0x%08x (0x%x bytes) has no target", dest, count);
                return -1;
            }
            if (flags & 1) {
                memset(to, 0, count);
            } else {
                memcpy(to, img + off, count);
            }
        }
        if (!(flags & 1)) {
            off += count;
        }
        if (flags & 0x8000) {
            bfin_reset(s->core, BOOT_ENTRY);
            return 0;
        }
    }
    return -1;
}

int bf531_boot_elf(bf531 *s, const uint8_t *img, size_t len)
{
    uint32_t entry, phoff;
    uint16_t phentsize, phnum;

    if (len < 52 || memcmp(img, "\177ELF\1\1", 6)) {
        return -1;
    }
    memcpy(&entry, img + 24, 4);
    memcpy(&phoff, img + 28, 4);
    memcpy(&phentsize, img + 42, 2);
    memcpy(&phnum, img + 44, 2);
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + (size_t)i * phentsize;
        uint32_t type, off, paddr, filesz, memsz;
        uint8_t *to;

        if (phoff + (size_t)(i + 1) * phentsize > len) {
            return -1;
        }
        memcpy(&type, ph, 4);
        memcpy(&off, ph + 4, 4);
        memcpy(&paddr, ph + 12, 4);
        memcpy(&filesz, ph + 16, 4);
        memcpy(&memsz, ph + 20, 4);
        if (type != 1 || !memsz) {
            continue;
        }
        if (filesz > memsz || off > len || filesz > len - off ||
            !(to = load_target(s, paddr, memsz))) {
            slog(s, "ELF segment at 0x%08x (0x%x bytes) has no target", paddr, memsz);
            return -1;
        }
        memcpy(to, img + off, filesz);
        memset(to + filesz, 0, memsz - filesz);
    }
    bfin_reset(s->core, entry);
    return 0;
}

int bf531_load_update(bf531 *s, const uint8_t *img, size_t len)
{
    if (len < 0x20 || len - 0x20 > FLASH_SIZE - FLASH_APP) {
        return -1;
    }
    memcpy(s->flash + FLASH_APP, img + 0x20, len - 0x20);
    return bf531_boot_ldr(s, img + 0x20, len - 0x20);
}

uint8_t *bf531_flash(bf531 *s, uint32_t *size)
{
    *size = FLASH_SIZE;
    return s->flash;
}

void bf531_set_ready_toggle(bf531 *s, uint16_t mask)
{
    s->ready_toggle = mask;
}

static uint64_t next_event(bf531 *s)
{
    uint64_t due = NEVER, core = bfin_timer_due(s->core);

    /* A timer without TIMER_IRQ_ENA has nothing the firmware can observe
     * (the counter is not live), so it does not end a step: the board's
     * PWM timer would otherwise cut every step to a few dozen
     * instructions. advance() moves its period on. */
    for (int n = 0; n < 3; n++) {
        if (s->timer[n].config & TIMER_IRQ_ENA) {
            due = s->timer[n].due < due ? s->timer[n].due : due;
        }
    }
    for (int n = 0; n < 8; n++) {
        due = s->dma[n].due < due ? s->dma[n].due : due;
    }
    if (core != NEVER && bfin_cycles(s->core) + core < due) {
        due = bfin_cycles(s->core) + core;
    }
    return due;
}

static void advance(bf531 *s)
{
    uint64_t now = bfin_cycles(s->core);

    for (int n = 0; n < 3; n++) {
        bf531_timer *t = &s->timer[n];

        if (!(t->config & TIMER_IRQ_ENA) && t->due <= now) {
            uint64_t p = sclk_to_cclk(s, t->period);

            t->due += p ? ((now - t->due) / p + 1) * p : NEVER - t->due;
        }
        while (t->due <= now) {
            timer_fire(s, n);
        }
    }
    for (int n = 0; n < 8; n++) {
        if (s->dma[n].due > now) {
            continue;
        }
        if (n == 3) {
            sport1_rx_pump(s);
        } else if (n == 4) {
            sport1_tx_send(s);
        } else {
            dma_unit_done(s, n);
        }
    }
}

bfin_stop bf531_run(bf531 *s, uint64_t n)
{
    uint64_t end = bfin_cycles(s->core) + n;

    while (bfin_cycles(s->core) < end) {
        uint64_t now = bfin_cycles(s->core), due = next_event(s);
        uint64_t stop_at = due < end ? due : end;
        bfin_stop stop;

        /* At least one cycle: an expired core timer is serviced by the step. */
        stop = bfin_step(s->core, stop_at > now ? stop_at - now : 1, NULL);
        if (stop == BFIN_STOP_UNDEF || stop == BFIN_STOP_BREAK) {
            return stop;
        }
        if (stop == BFIN_STOP_IDLE) {
            if (due == NEVER) {
                return stop;
            }
            if (stop_at > bfin_cycles(s->core)) {
                bfin_skip_cycles(s->core, stop_at - bfin_cycles(s->core));
            }
        }
        advance(s);
    }
    return BFIN_STOP_BUDGET;
}

bfin_core *bf531_core(bf531 *s)
{
    return s->core;
}

uint64_t bf531_cycles(const bf531 *s)
{
    return bfin_cycles(s->core);
}

uint64_t bf531_frames(const bf531 *s)
{
    return s->frames;
}

const uint8_t *bf531_sdram(const bf531 *s, uint32_t *size)
{
    *size = s->sdram_size;
    return s->sdram;
}
