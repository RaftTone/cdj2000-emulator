/*
 * Pioneer CDJ-2000 -- the SH7764 USB module in host mode.
 *
 * See cdj2000_usbh.h for what this is and why.  The register map and the bit
 * names are the SH7764 hardware manual's (section 21, tables 21.2 onward);
 * the behaviour is the subset the Cente USBH driver in the MAIN loader and
 * application exercises, each rule below citing the driver code that needs
 * it.  Where the manual describes timing (tokens every frame, double
 * buffering) the model is simpler: a transaction is issued as soon as a pipe
 * is ready, one packet at a time, from a timer so that the register write
 * that arms it has returned before its interrupt arrives.
 *
 * Copyright (C) 2026 LycheeAPPF
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/sh4/sh.h"
#include "hw/usb/usb.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#include "cdj2000_usbh.h"

/* Register offsets, manual table 21.2. */
#define R_SYSCFG        0x00
#define R_BUSWAIT       0x02
#define R_SYSSTS        0x04
#define R_DVSTCTR       0x08
#define R_TESTMODE      0x0c
#define R_D0FBCFG       0x10
#define R_D1FBCFG       0x12
#define R_CFIFO         0x14
#define R_D0FIFO        0x18
#define R_D1FIFO        0x1c
#define R_CFIFOSEL      0x20
#define R_CFIFOCTR      0x22
#define R_D0FIFOSEL     0x28
#define R_D0FIFOCTR     0x2a
#define R_D1FIFOSEL     0x2c
#define R_D1FIFOCTR     0x2e
#define R_INTENB0       0x30
#define R_INTENB1       0x32
#define R_BRDYENB       0x36
#define R_NRDYENB       0x38
#define R_BEMPENB       0x3a
#define R_SOFCFG        0x3c
#define R_INTSTS0       0x40
#define R_INTSTS1       0x42
#define R_BRDYSTS       0x46
#define R_NRDYSTS       0x48
#define R_BEMPSTS       0x4a
#define R_FRMNUM        0x4c
#define R_UFRMNUM       0x4e
#define R_USBADDR       0x50
#define R_USBREQ        0x54
#define R_USBVAL        0x56
#define R_USBINDX       0x58
#define R_USBLENG       0x5a
#define R_DCPCFG        0x5c
#define R_DCPMAXP       0x5e
#define R_DCPCTR        0x60
#define R_PIPESEL       0x64
#define R_PIPECFG       0x68
#define R_PIPEBUF       0x6a
#define R_PIPEMAXP      0x6c
#define R_PIPEPERI      0x6e
#define R_PIPE1CTR      0x70        /* ..0x80 = PIPE9CTR */
#define R_PIPE1TRE      0x90        /* TRE/TRN pairs to 0xa2, pipes 1..5 */
#define R_DEVADD0       0xd0        /* ..0xe4 = DEVADDA */

#define SYSCFG_USBE     0x0001
#define SYSCFG_DPRPU    0x0010
#define SYSCFG_DRPD     0x0020
#define SYSCFG_DCFM     0x0040
#define SYSCFG_HSE      0x0080
#define SYSCFG_SCKE     0x0400

#define DVSTCTR_UACT    0x0010
#define DVSTCTR_RESUME  0x0020
#define DVSTCTR_USBRST  0x0040
#define DVSTCTR_RWUPE   0x0080
#define DVSTCTR_WKUP    0x0100
#define DVSTCTR_RHST    0x0007
#define RHST_LOW        1
#define RHST_FULL       2
#define RHST_HIGH       3
#define RHST_RESETTING  4

#define FIFOSEL_CURPIPE 0x000f
#define FIFOSEL_ISEL    0x0020
#define FIFOSEL_DREQE   0x1000

#define FIFOCTR_DTLN    0x0fff
#define FIFOCTR_FRDY    0x2000
#define FIFOCTR_BCLR    0x4000
#define FIFOCTR_BVAL    0x8000

#define INTSTS0_BRDY    0x0100
#define INTSTS0_NRDY    0x0200
#define INTSTS0_BEMP    0x0400
#define INTSTS0_STICKY  0xf800      /* VBINT RESM SOFR DVST CTRT: never set here */

#define INTSTS1_SACK    0x0010
#define INTSTS1_SIGN    0x0020
#define INTSTS1_EOFERR  0x0040
#define INTSTS1_ATTCH   0x0800
#define INTSTS1_DTCH    0x1000
#define INTSTS1_BCHG    0x4000
#define INTSTS1_ALL     (INTSTS1_SACK | INTSTS1_SIGN | INTSTS1_EOFERR | \
                         INTSTS1_ATTCH | INTSTS1_DTCH | INTSTS1_BCHG)

#define DCPCTR_SUREQ    0x4000
#define DCPCTR_SUREQCLR 0x0800
#define DCPCFG_DIR      0x0010
#define DCPMAXP_MXPS    0x007f

#define PIPECTR_BSTS    0x8000
#define PIPECTR_INBUFM  0x4000
#define PIPECTR_ACLRM   0x0200
#define PIPECTR_SQCLR   0x0100
#define PIPECTR_SQSET   0x0080
#define PIPECTR_SQMON   0x0040
#define PIPECTR_PBUSY   0x0020
#define PIPECTR_PID     0x0003
#define PID_NAK         0
#define PID_BUF         1
#define PID_STALL       3

#define PIPECFG_SHTNAK  0x0080
#define PIPECFG_DIR     0x0010
#define PIPECFG_EPNUM   0x000f
#define PIPEMAXP_MXPS   0x07ff
#define MAXP_DEVSEL     0xf000

#define TRE_TRENB       0x0200
#define TRE_TRCLR       0x0100

#define NR_PIPES        10
#define NR_FIFOS        3           /* CFIFO, D0FIFO, D1FIFO */
#define PKT_BUF_SIZE    1024        /* a high-speed bulk packet is 512 */
#define NAK_RETRY_NS    1000000     /* a frame */
#define KICK_NS         20000       /* long enough for the arming ISR to return */
#define MICROFRAME_NS   125000      /* a high-speed microframe */

typedef struct CdjUsbhPipe {
    unsigned nr;
    uint16_t cfg, buf, maxp, peri;  /* the PIPESEL window (pipes 1..9) */
    uint16_t ctr;                   /* PIPEnCTR / DCPCTR's writable bits */
    uint16_t tre, trn;
    unsigned trn_count;             /* packets received since TRCLR */
    GByteArray *rx;                 /* received, not yet read out */
    unsigned rx_pos;
    bool rx_zlp;                    /* a zero-length packet awaits its BRDY read */
    GByteArray *tx;                 /* written by the CPU or the DMAC */
    unsigned tx_pos;
    bool tx_valid;                  /* BVAL: the buffer may go on the wire */
    int64_t tx_hold_until;          /* a DMA-filled buffer waits for the schedule */
    bool stopped;                   /* no more IN tokens until PID is rewritten */
    uint32_t in_total;              /* DCP: data-stage bytes since the SETUP */
    unsigned retries;
    USBPacket packet;
    bool inflight;
    uint8_t pkt_buf[PKT_BUF_SIZE];
} CdjUsbhPipe;

typedef struct CdjUsbhDma {
    bool active;
    bool to_fifo;
    hwaddr memory;
    uint32_t remaining;
    uint32_t moved;
    unsigned channel;
} CdjUsbhDma;

#define TYPE_CDJ_USBH "cdj2000-usbh"
OBJECT_DECLARE_SIMPLE_TYPE(CdjUsbhState, CDJ_USBH)

struct CdjUsbhState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion iomem_p4;
    USBBus bus;
    /*
     * Two ports, though the SoC has one: QEMU slips a full-speed hub in
     * front of a device plugged into a bus with a single free port
     * (bus.c usb_claim_port), and the driver then enumerates the hub.  The
     * second port stays empty; whichever holds the device is `active`.
     */
    USBPort ports[2];
    USBPort *active;
    qemu_irq irq;
    bool irq_level;

    uint16_t reg[CDJ_USBH_SIZE / 2];
    CdjUsbhPipe pipe[NR_PIPES];
    CdjUsbhDma dma[NR_FIFOS];       /* index 1 = D0FIFO, 2 = D1FIFO */

    QEMUTimer *timer;
    int64_t timer_at;
    bool attached;
    bool reset_done;                /* USBRST has been through 1 and back */
    bool setup_pending;
    uint64_t packet_id;
    bool trace;
    bool trace_scsi;                    /* CDJ_USBH_TRACE=scsi */
    bool trace_on_write;                /* =scsi+write: full trace after a WRITE */
    unsigned trace_left;                /* lines of that full trace still to go */

    CdjUsbhDmaDone dma_done;
    void *dma_opaque;
};

static CdjUsbhState *cdj_usbh_singleton;

static inline uint16_t rd(CdjUsbhState *s, unsigned off)
{
    return s->reg[off / 2];
}

static inline void wr(CdjUsbhState *s, unsigned off, uint16_t value)
{
    s->reg[off / 2] = value;
}

static void cdj_usbh_trace(CdjUsbhState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

static void cdj_usbh_trace(CdjUsbhState *s, const char *fmt, ...)
{
    va_list ap;

    if (!s->trace && !s->trace_scsi) {
        return;
    }
    if (s->trace_left && !--s->trace_left) {
        s->trace = false;           /* the window after a WRITE is over */
    }
    fprintf(stderr, "cdj2000-usbh %.3f: ",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- interrupts ------------------------------------------------------- */

static uint16_t cdj_usbh_intsts0(CdjUsbhState *s)
{
    uint16_t value = rd(s, R_INTSTS0) & INTSTS0_STICKY;

    /*
     * BRDY/NRDY/BEMP in INTSTS0 are summaries of the per-pipe status ANDed
     * with the per-pipe enable (manual 21.3.16), which is why the driver's
     * ISR (0x0400bc44) can clear INTSTS0 by writing its complement and still
     * find the pipe bits in BRDYSTS afterwards.
     */
    if (rd(s, R_BRDYSTS) & rd(s, R_BRDYENB)) {
        value |= INTSTS0_BRDY;
    }
    if (rd(s, R_NRDYSTS) & rd(s, R_NRDYENB)) {
        value |= INTSTS0_NRDY;
    }
    if (rd(s, R_BEMPSTS) & rd(s, R_BEMPENB)) {
        value |= INTSTS0_BEMP;
    }
    return value;
}

static void cdj_usbh_update_irq(CdjUsbhState *s)
{
    bool level = (cdj_usbh_intsts0(s) & rd(s, R_INTENB0) & 0xff00) != 0
        || (rd(s, R_INTSTS1) & rd(s, R_INTENB1) & INTSTS1_ALL) != 0;

    if (level != s->irq_level) {
        s->irq_level = level;
        if (s->trace) {
            cdj_usbh_trace(s, "irq %d (INTSTS0 %#06x INTENB0 %#06x INTSTS1 %#06x "
                       "INTENB1 %#06x BRDY %#05x NRDY %#05x BEMP %#05x)",
                       level, cdj_usbh_intsts0(s), rd(s, R_INTENB0),
                       rd(s, R_INTSTS1), rd(s, R_INTENB1), rd(s, R_BRDYSTS),
                       rd(s, R_NRDYSTS), rd(s, R_BEMPSTS));
        }
        qemu_set_irq(s->irq, level);
    }
}

static void cdj_usbh_raise1(CdjUsbhState *s, uint16_t bits)
{
    wr(s, R_INTSTS1, rd(s, R_INTSTS1) | bits);
    cdj_usbh_update_irq(s);
}

static void cdj_usbh_pipe_status(CdjUsbhState *s, unsigned reg, unsigned pipe)
{
    wr(s, reg, rd(s, reg) | (1u << pipe));
    cdj_usbh_update_irq(s);
}

/* ---- the timer that issues transactions -------------------------------- */

static void cdj_usbh_kick(CdjUsbhState *s, int64_t delay_ns)
{
    int64_t at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;

    if (!timer_pending(s->timer) || at < s->timer_at) {
        s->timer_at = at;
        timer_mod(s->timer, at);
    }
}

/* ---- pipe geometry ----------------------------------------------------- */

static inline bool pipe_is_dcp(const CdjUsbhPipe *p)
{
    return p->nr == 0;
}

/* Transmitting (host to device) or receiving, as the driver configured it. */
static bool pipe_transmits(CdjUsbhState *s, const CdjUsbhPipe *p)
{
    if (pipe_is_dcp(p)) {
        return (rd(s, R_DCPCFG) & DCPCFG_DIR) != 0;
    }
    return (p->cfg & PIPECFG_DIR) != 0;
}

static unsigned pipe_maxp(CdjUsbhState *s, const CdjUsbhPipe *p)
{
    unsigned maxp = pipe_is_dcp(p) ? rd(s, R_DCPMAXP) & DCPMAXP_MXPS
                                   : p->maxp & PIPEMAXP_MXPS;

    return maxp ? maxp : 64;
}

static unsigned pipe_devsel(CdjUsbhState *s, const CdjUsbhPipe *p)
{
    uint16_t maxp = pipe_is_dcp(p) ? rd(s, R_DCPMAXP) : p->maxp;

    return (maxp & MAXP_DEVSEL) >> 12;
}

static unsigned pipe_epnum(const CdjUsbhPipe *p)
{
    return pipe_is_dcp(p) ? 0 : p->cfg & PIPECFG_EPNUM;
}

static unsigned pipe_rx_avail(const CdjUsbhPipe *p)
{
    return p->rx->len - p->rx_pos;
}

static void pipe_rx_clear(CdjUsbhPipe *p)
{
    g_byte_array_set_size(p->rx, 0);
    p->rx_pos = 0;
    p->rx_zlp = false;
}

static void pipe_tx_clear(CdjUsbhPipe *p)
{
    g_byte_array_set_size(p->tx, 0);
    p->tx_pos = 0;
    p->tx_valid = false;
}

static void pipe_set_pid(CdjUsbhPipe *p, unsigned pid)
{
    p->ctr = (p->ctr & ~PIPECTR_PID) | (pid & PIPECTR_PID);
}

/* The FIFO port's pipe: CURPIPE of that port's select register. */
static CdjUsbhPipe *fifo_pipe(CdjUsbhState *s, unsigned fifo)
{
    static const unsigned sel[NR_FIFOS] = { R_CFIFOSEL, R_D0FIFOSEL,
                                            R_D1FIFOSEL };
    unsigned nr = rd(s, sel[fifo]) & FIFOSEL_CURPIPE;

    /*
     * The DCP is reached through the CFIFO port only; on D0FIFO/D1FIFO
     * CURPIPE = 0 selects no pipe (the Renesas USB modules' convention; the
     * SH7764 section was not re-read for this).  The driver
     * sets DREQE with CURPIPE still 0 (D0FIFOSEL = 0x1800) and picks the
     * pipe only after the DMAC is running (0x1802); a transfer served in
     * between went into the DCP's buffer and left the bulk pipe empty.
     */
    if (fifo != 0 && nr == 0) {
        return NULL;
    }
    return nr < NR_PIPES ? &s->pipe[nr] : NULL;
}

/*
 * Whether an access through this port reaches the pipe's transmit buffer.
 * For the DCP on the CFIFO that is ISEL (manual 21.3.8), which the driver
 * flips per stage (0x0400b79a: ISEL for the OUT data, cleared for the IN
 * status); everywhere else it is the pipe's direction.
 */
static bool fifo_writes(CdjUsbhState *s, unsigned fifo, const CdjUsbhPipe *p)
{
    if (fifo == 0 && pipe_is_dcp(p)) {
        return (rd(s, R_CFIFOSEL) & FIFOSEL_ISEL) != 0;
    }
    return pipe_transmits(s, p);
}

/* ---- DMA (the DMAC's burst port reads and writes) ---------------------- */

static void cdj_usbh_dma_finish(CdjUsbhState *s, unsigned fifo)
{
    CdjUsbhDma *dma = &s->dma[fifo];

    dma->active = false;
    cdj_usbh_trace(s, "dma fifo%u channel %u done, %u bytes", fifo,
                   dma->channel, dma->moved);
    if (s->dma_done) {
        s->dma_done(s->dma_opaque, dma->channel, dma->moved);
    }
}

/*
 * Move what can be moved between the FIFO's pipe and memory.  Only while
 * DREQE is set on that port's select register: the driver programs the DMAC
 * first (0x0400b3a0) and only then points D0FIFOSEL at the pipe with DREQE
 * (0x0400c81a), so a transfer that ran on CHCR.DE would read the wrong pipe.
 */
static void cdj_usbh_dma_service(CdjUsbhState *s, unsigned fifo)
{
    static const unsigned sel[NR_FIFOS] = { R_CFIFOSEL, R_D0FIFOSEL,
                                            R_D1FIFOSEL };
    CdjUsbhDma *dma = &s->dma[fifo];
    CdjUsbhPipe *p;

    if (!dma->active || !(rd(s, sel[fifo]) & FIFOSEL_DREQE)) {
        return;
    }
    p = fifo_pipe(s, fifo);
    if (!p) {
        return;
    }
    if (dma->to_fifo) {
        /*
         * Memory to the transmit buffer, all of it at once: the DMAC's part
         * is over when the last byte has left memory, and the pipe sends the
         * buffer out in maximum-packet pieces from there.
         */
        if (!p->tx_valid && dma->remaining) {
            uint32_t n = dma->remaining;
            unsigned old = p->tx->len;

            g_byte_array_set_size(p->tx, old + n);
            address_space_read(&address_space_memory, dma->memory,
                               MEMTXATTRS_UNSPECIFIED, p->tx->data + old, n);
            dma->memory += n;
            dma->remaining -= n;
            dma->moved += n;
            p->tx_valid = true;
            /*
             * The DMAC is done the moment the last byte has left memory; the
             * host puts the packet on the bus only in its schedule, and the
             * driver's DMA-end path runs in between: it NAKs the pipe, writes
             * BVAL (0x8000 to D0FIFOCTR) and lets it go again.  On the board
             * that BVAL meets the full buffer and adds nothing; sent at once,
             * the buffer was already empty, the BVAL went out as a
             * zero-length packet, the stick STALLed it, and every WRITE(10)
             * was retried every 50 ms for good -- a USB LOAD never started
             * its stream (E-8302 3611).  So the buffer waits a microframe.
             */
            p->tx_hold_until = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MICROFRAME_NS;
            cdj_usbh_kick(s, MICROFRAME_NS);
        }
    } else {
        unsigned avail = pipe_rx_avail(p);
        uint32_t n = MIN(avail, dma->remaining);

        if (n) {
            address_space_write(&address_space_memory, dma->memory,
                                MEMTXATTRS_UNSPECIFIED,
                                p->rx->data + p->rx_pos, n);
            p->rx_pos += n;
            dma->memory += n;
            dma->remaining -= n;
            dma->moved += n;
            if (pipe_rx_avail(p) == 0) {
                pipe_rx_clear(p);
                /* The buffer has room again; the next IN may go out. */
                cdj_usbh_kick(s, KICK_NS);
            }
        }
    }
    if (dma->remaining == 0) {
        cdj_usbh_dma_finish(s, fifo);
    }
}

bool cdj_usbh_dma_start(hwaddr offset, bool to_fifo, hwaddr memory,
                        uint32_t nr_bytes, unsigned channel)
{
    CdjUsbhState *s = cdj_usbh_singleton;
    unsigned fifo;
    CdjUsbhDma *dma;

    if (!s) {
        return false;
    }
    fifo = offset == CDJ_USBH_D1FIFO_BURST ? 2 : 1;
    dma = &s->dma[fifo];
    if (dma->active) {
        warn_report("cdj2000-usbh: DMA on fifo%u restarted with %u bytes "
                    "still outstanding", fifo, dma->remaining);
    }
    dma->active = true;
    dma->to_fifo = to_fifo;
    dma->memory = memory;
    dma->remaining = nr_bytes;
    dma->moved = 0;
    dma->channel = channel;
    cdj_usbh_trace(s, "dma fifo%u channel %u %s memory %#010" HWADDR_PRIx
                   " %u bytes", fifo, channel, to_fifo ? "from" : "to",
                   memory, nr_bytes);
    cdj_usbh_dma_service(s, fifo);
    cdj_usbh_kick(s, KICK_NS);
    return true;
}

static void cdj_usbh_dma_service_all(CdjUsbhState *s)
{
    unsigned fifo;

    for (fifo = 1; fifo < NR_FIFOS; fifo++) {
        cdj_usbh_dma_service(s, fifo);
    }
}

/* ---- transactions ------------------------------------------------------ */

static USBDevice *cdj_usbh_device(CdjUsbhState *s, const CdjUsbhPipe *p)
{
    if (!s->attached || !s->active || !s->active->dev) {
        return NULL;
    }
    return usb_find_device(s->active, pipe_devsel(s, p));
}

static void cdj_usbh_packet_done(CdjUsbhState *s, CdjUsbhPipe *p);

static void cdj_usbh_issue(CdjUsbhState *s, CdjUsbhPipe *p, int pid,
                           unsigned len)
{
    USBDevice *dev = cdj_usbh_device(s, p);
    USBEndpoint *ep;

    if (!dev) {
        cdj_usbh_trace(s, "pipe%u: no device at address %u", p->nr,
                       pipe_devsel(s, p));
        p->inflight = false;
        p->packet.status = USB_RET_NODEV;
        cdj_usbh_packet_done(s, p);
        return;
    }
    ep = usb_ep_get(dev, pid, pipe_epnum(p));
    usb_packet_setup(&p->packet, pid, ep, 0, ++s->packet_id, false, false);
    usb_packet_addbuf(&p->packet, p->pkt_buf, len);
    p->inflight = true;
    if (s->trace) {
        cdj_usbh_trace(s, "pipe%u: %s addr %u ep %u len %u", p->nr,
                       pid == USB_TOKEN_SETUP ? "SETUP"
                       : pid == USB_TOKEN_IN ? "IN" : "OUT",
                       pipe_devsel(s, p), pipe_epnum(p), len);
    }
    /*
     * Firmware may write BVAL again after the DMA-filled bulk OUT packet
     * has completed.  The microframe hold handles BVAL during DMA; when
     * BVAL arrives after an exact-size packet, it must not put a second,
     * empty packet into an already completed usb-storage BOT data stage.
     * Control-pipe zero-length packets are real status stages.
     */
    if (pid == USB_TOKEN_OUT && len == 0 && !pipe_is_dcp(p)) {
        p->packet.status = USB_RET_SUCCESS;
        p->packet.actual_length = 0;
        cdj_usbh_packet_done(s, p);
        return;
    }
    usb_handle_packet(dev, &p->packet);
    if (p->packet.status == USB_RET_ASYNC) {
        return;
    }
    cdj_usbh_packet_done(s, p);
}

static void cdj_usbh_setup(CdjUsbhState *s)
{
    CdjUsbhPipe *p = &s->pipe[0];
    uint16_t req = rd(s, R_USBREQ), val = rd(s, R_USBVAL);
    uint16_t indx = rd(s, R_USBINDX), leng = rd(s, R_USBLENG);

    s->setup_pending = false;
    if (p->inflight) {
        return;
    }
    /*
     * The eight bytes are the four request registers in little-endian
     * order; the driver fills them straight from its request block
     * (0x0400b99e), so USBREQ's low byte is bmRequestType.
     */
    p->pkt_buf[0] = req;
    p->pkt_buf[1] = req >> 8;
    p->pkt_buf[2] = val;
    p->pkt_buf[3] = val >> 8;
    p->pkt_buf[4] = indx;
    p->pkt_buf[5] = indx >> 8;
    p->pkt_buf[6] = leng;
    p->pkt_buf[7] = leng >> 8;
    p->in_total = 0;
    p->stopped = false;
    pipe_rx_clear(p);
    cdj_usbh_issue(s, p, USB_TOKEN_SETUP, 8);
}

/* Failure of a data transaction; the driver's NRDY handler is 0x0400c25e. */
static void cdj_usbh_fail(CdjUsbhState *s, CdjUsbhPipe *p, int status)
{
    switch (status) {
    case USB_RET_NAK:
        /* Retried silently every frame, as the hardware does. */
        cdj_usbh_kick(s, NAK_RETRY_NS);
        return;
    case USB_RET_STALL:
        cdj_usbh_trace(s, "pipe%u: STALL", p->nr);
        pipe_set_pid(p, PID_STALL);
        cdj_usbh_pipe_status(s, R_NRDYSTS, p->nr);
        return;
    default:
        /*
         * No response or a bad packet: three in a row set NRDY and put the
         * pipe to NAK (manual 21.4.2 (2)(a)).
         */
        if (++p->retries < 3) {
            cdj_usbh_kick(s, NAK_RETRY_NS);
            return;
        }
        cdj_usbh_trace(s, "pipe%u: error %d, giving up", p->nr, status);
        pipe_set_pid(p, PID_NAK);
        cdj_usbh_pipe_status(s, R_NRDYSTS, p->nr);
        return;
    }
}

static void cdj_usbh_packet_done(CdjUsbhState *s, CdjUsbhPipe *p)
{
    USBPacket *pkt = &p->packet;
    int pid = pkt->pid;
    unsigned actual = pkt->actual_length;

    p->inflight = false;
    if (pid == USB_TOKEN_SETUP) {
        p->ctr &= ~DCPCTR_SUREQ;
        cdj_usbh_trace(s, "pipe0: SETUP -> %d", pkt->status);
        cdj_usbh_raise1(s, pkt->status == USB_RET_SUCCESS ? INTSTS1_SACK
                                                          : INTSTS1_SIGN);
        return;
    }
    if (pkt->status != USB_RET_SUCCESS) {
        cdj_usbh_fail(s, p, pkt->status);
        return;
    }
    p->retries = 0;
    if (pid == USB_TOKEN_IN) {
        unsigned maxp = pipe_maxp(s, p);

        if (s->trace_scsi && actual == 13 && !memcmp(p->pkt_buf, "USBS", 4)) {
            cdj_usbh_trace(s, "pipe%u: CSW tag %08x residue %u status %u",
                           p->nr, ldl_le_p(p->pkt_buf + 4),
                           ldl_le_p(p->pkt_buf + 8), p->pkt_buf[12]);
        }
        if (s->trace) {
            char hex[3 * 32 + 1];
            unsigned i, n = MIN(actual, 32);

            for (i = 0; i < n; i++) {
                snprintf(hex + 3 * i, 4, "%02x ", p->pkt_buf[i]);
            }
            hex[3 * n] = 0;
            cdj_usbh_trace(s, "pipe%u: IN -> %u bytes: %s%s", p->nr, actual,
                           hex, actual > n ? "..." : "");
        }
        g_byte_array_append(p->rx, p->pkt_buf, actual);
        if (actual == 0) {
            p->rx_zlp = true;
        }
        p->trn_count++;
        p->in_total += actual;
        /*
         * A short packet ends the transfer: with SHTNAK the pipe goes to
         * NAK by itself (the driver sets it on every receiving bulk pipe,
         * 0x0400c32c), and the DCP stops when the request's wLength is in.
         */
        if (actual < maxp) {
            p->stopped = true;
            if (!pipe_is_dcp(p) && (p->cfg & PIPECFG_SHTNAK)) {
                pipe_set_pid(p, PID_NAK);
            }
        }
        /*
         * The transaction counter: on the counted packet the pipe goes to
         * NAK by itself while SHTNAK is set (manual 21.3.37), which is what
         * lets the driver clear and reload the counter for the CSW read
         * (0x0400d1aa) before it writes BUF again.  Without the NAK the
         * next IN went out on TRCLR and the CSW arrived before the driver
         * was waiting for it (update-15).
         */
        if (!pipe_is_dcp(p) && (p->tre & TRE_TRENB)
            && p->trn_count >= p->trn) {
            p->stopped = true;
            if (p->cfg & PIPECFG_SHTNAK) {
                pipe_set_pid(p, PID_NAK);
            }
        }
        if (pipe_is_dcp(p) && p->in_total >= rd(s, R_USBLENG)) {
            p->stopped = true;
        }
        cdj_usbh_pipe_status(s, R_BRDYSTS, p->nr);
        cdj_usbh_dma_service_all(s);
    } else {
        if (s->trace) {
            cdj_usbh_trace(s, "pipe%u: OUT %u bytes done", p->nr, actual);
        }
        p->tx_pos += actual;
        if (p->tx_pos >= p->tx->len) {
            pipe_tx_clear(p);
            /*
             * BEMP: the buffer emptied on completion (manual 21.4.2 (3));
             * the driver's write paths wait for it (0x0400cd98 via the BEMP
             * handler 0x0400c1b4).  BRDY too for every pipe but the DCP,
             * because the buffer is writable again (21.4.2 (1)(a)(i)).
             */
            cdj_usbh_pipe_status(s, R_BEMPSTS, p->nr);
            if (!pipe_is_dcp(p)) {
                cdj_usbh_pipe_status(s, R_BRDYSTS, p->nr);
            }
            cdj_usbh_dma_service_all(s);
        }
    }
    cdj_usbh_kick(s, KICK_NS);
}

static bool cdj_usbh_running(CdjUsbhState *s)
{
    uint16_t syscfg = rd(s, R_SYSCFG);

    return s->attached && (syscfg & SYSCFG_USBE) && (syscfg & SYSCFG_DCFM)
        && (syscfg & SYSCFG_SCKE) && !(rd(s, R_DVSTCTR) & DVSTCTR_USBRST);
}

static void cdj_usbh_run_pipe(CdjUsbhState *s, CdjUsbhPipe *p)
{
    if (p->inflight || (p->ctr & PIPECTR_PID) != PID_BUF) {
        return;
    }
    if (pipe_transmits(s, p)) {
        unsigned len;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (!p->tx_valid) {
            return;
        }
        if (now < p->tx_hold_until) {
            if (s->trace) {
                cdj_usbh_trace(s, "pipe%u: %u bytes held %" PRId64 " ns for the schedule",
                               p->nr, p->tx->len - p->tx_pos, p->tx_hold_until - now);
            }
            cdj_usbh_kick(s, p->tx_hold_until - now);
            return;
        }
        len = MIN(p->tx->len - p->tx_pos, pipe_maxp(s, p));
        len = MIN(len, PKT_BUF_SIZE);
        memcpy(p->pkt_buf, p->tx->data + p->tx_pos, len);
        if (s->trace_scsi && len == 31 && !memcmp(p->pkt_buf, "USBC", 4)) {
            const uint8_t *cb = p->pkt_buf + 15;

            /* READ/WRITE(10): LBA and blocks; anything else by opcode. */
            cdj_usbh_trace(s, "pipe%u: CBW tag %08x %u bytes %s op %02x lba %u "
                           "blocks %u", p->nr, ldl_le_p(p->pkt_buf + 4),
                           ldl_le_p(p->pkt_buf + 8),
                           p->pkt_buf[12] & 0x80 ? "in" : "out", cb[0],
                           ldl_be_p(cb + 2), lduw_be_p(cb + 7));
            if (s->trace_on_write && cb[0] == 0x2a && !s->trace_left) {
                s->trace = true;
                s->trace_left = 1500;
                s->trace_on_write = false;      /* the first WRITE only */
            }
        }
        if (s->trace && len) {
            char hex[3 * 32 + 1];
            unsigned i, n = MIN(len, 32);

            for (i = 0; i < n; i++) {
                snprintf(hex + 3 * i, 4, "%02x ", p->pkt_buf[i]);
            }
            hex[3 * n] = 0;
            cdj_usbh_trace(s, "pipe%u: OUT %u bytes: %s%s", p->nr, len, hex,
                           len > n ? "..." : "");
        }
        cdj_usbh_issue(s, p, USB_TOKEN_OUT, len);
    } else {
        if (p->stopped || pipe_rx_avail(p) || p->rx_zlp) {
            return;
        }
        cdj_usbh_issue(s, p, USB_TOKEN_IN, MIN(pipe_maxp(s, p), PKT_BUF_SIZE));
    }
}

static void cdj_usbh_timer(void *opaque)
{
    CdjUsbhState *s = opaque;
    unsigned nr;

    if (!cdj_usbh_running(s)) {
        return;
    }
    if (s->setup_pending) {
        cdj_usbh_setup(s);
    }
    cdj_usbh_dma_service_all(s);
    for (nr = 0; nr < NR_PIPES; nr++) {
        cdj_usbh_run_pipe(s, &s->pipe[nr]);
    }
}

/* ---- the port ---------------------------------------------------------- */

static void cdj_usbh_attach(USBPort *port)
{
    CdjUsbhState *s = port->opaque;

    s->attached = true;
    s->active = port;
    s->reset_done = false;
    cdj_usbh_trace(s, "device attached on port %d (speed %d)", port->index,
                   port->dev->speed);
    if (rd(s, R_SYSCFG) & SYSCFG_DRPD) {
        cdj_usbh_raise1(s, INTSTS1_ATTCH | INTSTS1_BCHG);
    }
}

static void cdj_usbh_child_detach(USBPort *port, USBDevice *dev)
{
    CdjUsbhState *s = port->opaque;
    unsigned nr;

    for (nr = 0; nr < NR_PIPES; nr++) {
        CdjUsbhPipe *p = &s->pipe[nr];

        if (p->inflight && usb_packet_is_inflight(&p->packet)
            && p->packet.ep->dev == dev) {
            usb_cancel_packet(&p->packet);
            p->inflight = false;
        }
    }
}

static void cdj_usbh_detach(USBPort *port)
{
    CdjUsbhState *s = port->opaque;

    cdj_usbh_child_detach(port, port->dev);
    s->attached = false;
    s->active = NULL;
    s->reset_done = false;
    wr(s, R_DVSTCTR, rd(s, R_DVSTCTR) & ~DVSTCTR_UACT);
    cdj_usbh_trace(s, "device detached");
    cdj_usbh_raise1(s, INTSTS1_DTCH | INTSTS1_BCHG);
}

static void cdj_usbh_wakeup(USBPort *port)
{
    CdjUsbhState *s = port->opaque;

    /*
     * In host mode RWUPE controls whether a downstream device's resume
     * signal is observed.  The SH7764 reports an accepted bus-state change
     * through BCHG; the firmware then finishes the resume sequence by
     * clearing/rewriting DVSTCTR.  QEMU calls this hook only after the USB
     * device has requested remote wakeup, so no synthetic packet is needed.
     */
    if (!s->attached || !(rd(s, R_DVSTCTR) & DVSTCTR_RWUPE)) {
        cdj_usbh_trace(s, "remote wakeup ignored (attached=%d RWUPE=%d)",
                       s->attached, !!(rd(s, R_DVSTCTR) & DVSTCTR_RWUPE));
        return;
    }

    cdj_usbh_trace(s, "remote wakeup accepted");
    cdj_usbh_raise1(s, INTSTS1_BCHG);
    cdj_usbh_kick(s, KICK_NS);
}

static void cdj_usbh_async_complete(USBPort *port, USBPacket *packet)
{
    CdjUsbhState *s = port->opaque;
    CdjUsbhPipe *p = container_of(packet, CdjUsbhPipe, packet);

    cdj_usbh_packet_done(s, p);
}

static USBPortOps cdj_usbh_port_ops = {
    .attach = cdj_usbh_attach,
    .detach = cdj_usbh_detach,
    .child_detach = cdj_usbh_child_detach,
    .wakeup = cdj_usbh_wakeup,
    .complete = cdj_usbh_async_complete,
};

static USBBusOps cdj_usbh_bus_ops = {
};

/* ---- registers --------------------------------------------------------- */

static unsigned cdj_usbh_rhst(CdjUsbhState *s)
{
    if (rd(s, R_DVSTCTR) & DVSTCTR_USBRST) {
        return RHST_RESETTING;
    }
    if (!s->attached || !s->reset_done || !s->active || !s->active->dev) {
        return 0;
    }
    switch (s->active->dev->speed) {
    case USB_SPEED_LOW:
        return RHST_LOW;
    case USB_SPEED_HIGH:
        return (rd(s, R_SYSCFG) & SYSCFG_HSE) ? RHST_HIGH : RHST_FULL;
    default:
        return RHST_FULL;
    }
}

static uint16_t cdj_usbh_pipectr_read(CdjUsbhState *s, CdjUsbhPipe *p)
{
    uint16_t value = p->ctr & (PIPECTR_PID | PIPECTR_SQMON | PIPECTR_INBUFM);

    if (pipe_is_dcp(p)) {
        value = p->ctr & (PIPECTR_PID | PIPECTR_SQMON | DCPCTR_SUREQ);
    }
    if (pipe_transmits(s, p) ? !p->tx_valid : (pipe_rx_avail(p) || p->rx_zlp)) {
        value |= PIPECTR_BSTS;
    }
    if (p->inflight) {
        value |= PIPECTR_PBUSY;
    }
    return value;
}

static void cdj_usbh_pipectr_write(CdjUsbhState *s, CdjUsbhPipe *p,
                                   uint16_t value)
{
    unsigned old_pid = p->ctr & PIPECTR_PID;
    unsigned new_pid = value & PIPECTR_PID;

    if (pipe_is_dcp(p)) {
        if (value & DCPCTR_SUREQ) {
            p->ctr |= DCPCTR_SUREQ;
            s->setup_pending = true;
        }
        if (value & DCPCTR_SUREQCLR) {
            p->ctr &= ~DCPCTR_SUREQ;
            s->setup_pending = false;
        }
    } else if (value & PIPECTR_ACLRM) {
        pipe_rx_clear(p);
        pipe_tx_clear(p);
    }
    /*
     * Writing BUF re-arms the pipe even if it already read BUF: after the
     * transaction counter has run out the driver clears the counter, loads
     * it again and writes BUF again (0x0400d1aa, the CSW after a DMA read)
     * without ever having written NAK in between.
     */
    if (new_pid == PID_BUF) {
        p->stopped = false;
        p->retries = 0;
    }
    (void)old_pid;
    p->ctr = (p->ctr & ~(PIPECTR_PID | PIPECTR_SQMON))
        | new_pid | (value & PIPECTR_SQMON);
    cdj_usbh_kick(s, KICK_NS);
}

static uint64_t cdj_usbh_fifo_read(CdjUsbhState *s, unsigned fifo,
                                   unsigned size)
{
    CdjUsbhPipe *p = fifo_pipe(s, fifo);
    uint64_t value = 0;
    unsigned i;

    if (!p || fifo_writes(s, fifo, p)) {
        return 0;
    }
    for (i = 0; i < size && pipe_rx_avail(p); i++) {
        value |= (uint64_t)p->rx->data[p->rx_pos++] << (8 * i);
    }
    if (pipe_rx_avail(p) == 0) {
        g_byte_array_set_size(p->rx, 0);
        p->rx_pos = 0;
        cdj_usbh_kick(s, KICK_NS);
    }
    return value;
}

static void cdj_usbh_fifo_write(CdjUsbhState *s, unsigned fifo,
                                uint64_t value, unsigned size)
{
    CdjUsbhPipe *p = fifo_pipe(s, fifo);
    uint8_t bytes[8];
    unsigned i;

    if (!p || !fifo_writes(s, fifo, p)) {
        return;
    }
    for (i = 0; i < size; i++) {
        bytes[i] = value >> (8 * i);
    }
    g_byte_array_append(p->tx, bytes, size);
}

static uint16_t cdj_usbh_fifoctr_read(CdjUsbhState *s, unsigned fifo)
{
    CdjUsbhPipe *p = fifo_pipe(s, fifo);
    uint16_t value = FIFOCTR_FRDY;

    if (p && !fifo_writes(s, fifo, p)) {
        value |= MIN(pipe_rx_avail(p), FIFOCTR_DTLN);
    }
    return value;
}

static void cdj_usbh_fifoctr_write(CdjUsbhState *s, unsigned fifo,
                                   uint16_t value)
{
    CdjUsbhPipe *p = fifo_pipe(s, fifo);

    if (!p) {
        return;
    }
    if (fifo_writes(s, fifo, p)) {
        if (value & FIFOCTR_BCLR) {
            pipe_tx_clear(p);
        }
        if (value & FIFOCTR_BVAL) {
            /* Including a zero-length buffer: that is the status stage. */
            p->tx_valid = true;
            cdj_usbh_kick(s, KICK_NS);
        }
    } else if (value & FIFOCTR_BCLR) {
        pipe_rx_clear(p);
        cdj_usbh_kick(s, KICK_NS);
    }
}

static CdjUsbhPipe *cdj_usbh_window_pipe(CdjUsbhState *s)
{
    unsigned nr = rd(s, R_PIPESEL) & FIFOSEL_CURPIPE;

    return nr >= 1 && nr < NR_PIPES ? &s->pipe[nr] : NULL;
}

static uint64_t cdj_usbh_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjUsbhState *s = opaque;
    uint64_t value = 0;
    CdjUsbhPipe *p;

    if (offset >= R_CFIFO && offset < R_CFIFO + 4) {
        return cdj_usbh_fifo_read(s, 0, size);
    }
    if (offset >= R_D0FIFO && offset < R_D0FIFO + 4) {
        return cdj_usbh_fifo_read(s, 1, size);
    }
    if (offset >= R_D1FIFO && offset < R_D1FIFO + 4) {
        return cdj_usbh_fifo_read(s, 2, size);
    }
    if (offset >= CDJ_USBH_D0FIFO_BURST && offset < CDJ_USBH_D0FIFO_BURST + 4) {
        return cdj_usbh_fifo_read(s, 1, size);
    }
    if (offset >= CDJ_USBH_D1FIFO_BURST && offset < CDJ_USBH_D1FIFO_BURST + 4) {
        return cdj_usbh_fifo_read(s, 2, size);
    }

    switch (offset & ~1) {
    case R_SYSSTS:
        /* Bit 10 reads 1; LNST is J (D+ high) with a device pulled down. */
        value = 0x0400;
        if (s->attached && (rd(s, R_SYSCFG) & SYSCFG_DRPD)) {
            value |= s->active && s->active->dev
                && s->active->dev->speed == USB_SPEED_LOW ? 2 : 1;
        }
        break;
    case R_DVSTCTR:
        value = (rd(s, R_DVSTCTR) & ~DVSTCTR_RHST) | cdj_usbh_rhst(s);
        break;
    case R_CFIFOCTR:
        value = cdj_usbh_fifoctr_read(s, 0);
        break;
    case R_D0FIFOCTR:
        value = cdj_usbh_fifoctr_read(s, 1);
        break;
    case R_D1FIFOCTR:
        value = cdj_usbh_fifoctr_read(s, 2);
        break;
    case R_INTSTS0:
        value = cdj_usbh_intsts0(s);
        break;
    case R_FRMNUM:
        value = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000) & 0x7ff;
        break;
    case R_UFRMNUM:
        value = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 125000) & 7;
        break;
    case R_USBADDR:
        value = 0;
        break;
    case R_DCPCTR:
        value = cdj_usbh_pipectr_read(s, &s->pipe[0]);
        break;
    case R_PIPECFG:
        p = cdj_usbh_window_pipe(s);
        value = p ? p->cfg : 0;
        break;
    case R_PIPEBUF:
        p = cdj_usbh_window_pipe(s);
        value = p ? p->buf : 0;
        break;
    case R_PIPEMAXP:
        p = cdj_usbh_window_pipe(s);
        value = p ? p->maxp : 0;
        break;
    case R_PIPEPERI:
        p = cdj_usbh_window_pipe(s);
        value = p ? p->peri : 0;
        break;
    default:
        if (offset >= R_PIPE1CTR && offset < R_PIPE1CTR + 2 * (NR_PIPES - 1)) {
            value = cdj_usbh_pipectr_read(s, &s->pipe[1 + (offset - R_PIPE1CTR) / 2]);
        } else if (offset >= R_PIPE1TRE && offset < R_PIPE1TRE + 4 * 5) {
            p = &s->pipe[1 + (offset - R_PIPE1TRE) / 4];
            value = (offset & 2) ? p->trn : p->tre;
        } else {
            value = rd(s, offset & ~1);
        }
        break;
    }
    if (size == 1 && (offset & 1)) {
        value >>= 8;
    }
    value &= size == 1 ? 0xff : 0xffff;
    if (s->trace && (offset & ~1) != R_INTSTS0 && (offset & ~1) != R_INTSTS1
        && (offset & ~1) != R_BRDYSTS && (offset & ~1) != R_NRDYSTS
        && (offset & ~1) != R_BEMPSTS && (offset & ~1) != R_SYSSTS
        && (offset & ~1) != R_DVSTCTR) {
        cdj_usbh_trace(s, "read  %#05" HWADDR_PRIx " (%u) = %#06" PRIx64,
                       offset, size, value);
    }
    return value;
}

static void cdj_usbh_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CdjUsbhState *s = opaque;
    uint16_t old, v = value;
    CdjUsbhPipe *p;

    if (offset >= R_CFIFO && offset < R_CFIFO + 4) {
        cdj_usbh_fifo_write(s, 0, value, size);
        return;
    }
    if (offset >= R_D0FIFO && offset < R_D0FIFO + 4) {
        cdj_usbh_fifo_write(s, 1, value, size);
        return;
    }
    if (offset >= R_D1FIFO && offset < R_D1FIFO + 4) {
        cdj_usbh_fifo_write(s, 2, value, size);
        return;
    }
    if (offset >= CDJ_USBH_D0FIFO_BURST && offset < CDJ_USBH_D0FIFO_BURST + 4) {
        cdj_usbh_fifo_write(s, 1, value, size);
        return;
    }
    if (offset >= CDJ_USBH_D1FIFO_BURST && offset < CDJ_USBH_D1FIFO_BURST + 4) {
        cdj_usbh_fifo_write(s, 2, value, size);
        return;
    }
    if (size == 1) {
        old = rd(s, offset & ~1);
        v = (offset & 1) ? (old & 0x00ff) | (v << 8) : (old & 0xff00) | (v & 0xff);
    }
    offset &= ~1;
    if (s->trace) {
        cdj_usbh_trace(s, "write %#05" HWADDR_PRIx " (%u) = %#06x", offset, size, v);
    }

    switch (offset) {
    case R_SYSCFG:
        old = rd(s, R_SYSCFG);
        wr(s, R_SYSCFG, v);
        /*
         * Pulling the lines down with a device on them is how the module
         * first sees it: ATTCH and BCHG (manual 21.3.17).  The driver's
         * ISR (0x0400bc44) reports BCHG and DTCH to its task, which then
         * reads LNST.
         */
        if ((v & SYSCFG_DRPD) && !(old & SYSCFG_DRPD) && s->attached) {
            cdj_usbh_raise1(s, INTSTS1_ATTCH | INTSTS1_BCHG);
        }
        cdj_usbh_kick(s, KICK_NS);
        break;
    case R_DVSTCTR:
        old = rd(s, R_DVSTCTR);
        wr(s, R_DVSTCTR, v & (DVSTCTR_UACT | DVSTCTR_RESUME | DVSTCTR_USBRST
                              | DVSTCTR_RWUPE | DVSTCTR_WKUP));
        if ((v & DVSTCTR_USBRST) && !(old & DVSTCTR_USBRST)) {
            cdj_usbh_trace(s, "bus reset");
            if (s->attached && s->active && s->active->dev) {
                usb_device_reset(s->active->dev);
            }
        }
        if (!(v & DVSTCTR_USBRST) && (old & DVSTCTR_USBRST)) {
            s->reset_done = true;
            cdj_usbh_trace(s, "bus reset over, RHST %u", cdj_usbh_rhst(s));
        }
        cdj_usbh_kick(s, KICK_NS);
        break;
    case R_CFIFOSEL:
    case R_D0FIFOSEL:
    case R_D1FIFOSEL:
        wr(s, offset, v);
        cdj_usbh_kick(s, KICK_NS);
        break;
    case R_CFIFOCTR:
        cdj_usbh_fifoctr_write(s, 0, v);
        break;
    case R_D0FIFOCTR:
        cdj_usbh_fifoctr_write(s, 1, v);
        break;
    case R_D1FIFOCTR:
        cdj_usbh_fifoctr_write(s, 2, v);
        break;
    case R_INTENB0:
    case R_INTENB1:
    case R_BRDYENB:
    case R_NRDYENB:
    case R_BEMPENB:
        wr(s, offset, v);
        cdj_usbh_update_irq(s);
        break;
    case R_INTSTS0:
        /* Writing 0 clears; the summary bits are computed and unaffected. */
        wr(s, R_INTSTS0, rd(s, R_INTSTS0) & v & INTSTS0_STICKY);
        cdj_usbh_update_irq(s);
        break;
    case R_INTSTS1:
        wr(s, R_INTSTS1, rd(s, R_INTSTS1) & v & INTSTS1_ALL);
        cdj_usbh_update_irq(s);
        break;
    case R_BRDYSTS:
    case R_NRDYSTS:
    case R_BEMPSTS:
        wr(s, offset, rd(s, offset) & v);
        cdj_usbh_update_irq(s);
        break;
    case R_DCPCTR:
        cdj_usbh_pipectr_write(s, &s->pipe[0], v);
        break;
    case R_PIPECFG:
        p = cdj_usbh_window_pipe(s);
        if (p) {
            p->cfg = v;
        }
        break;
    case R_PIPEBUF:
        p = cdj_usbh_window_pipe(s);
        if (p) {
            p->buf = v;
        }
        break;
    case R_PIPEMAXP:
        p = cdj_usbh_window_pipe(s);
        if (p) {
            p->maxp = v;
        }
        break;
    case R_PIPEPERI:
        p = cdj_usbh_window_pipe(s);
        if (p) {
            p->peri = v;
        }
        break;
    case R_SYSSTS:
        break;
    default:
        if (offset >= R_PIPE1CTR && offset < R_PIPE1CTR + 2 * (NR_PIPES - 1)) {
            cdj_usbh_pipectr_write(s, &s->pipe[1 + (offset - R_PIPE1CTR) / 2], v);
        } else if (offset >= R_PIPE1TRE && offset < R_PIPE1TRE + 4 * 5) {
            p = &s->pipe[1 + (offset - R_PIPE1TRE) / 4];
            if (offset & 2) {
                p->trn = v;
            } else {
                if (v & TRE_TRCLR) {
                    p->trn_count = 0;
                }
                p->tre = v & TRE_TRENB;
            }
        } else {
            wr(s, offset, v);
        }
        break;
    }
}

static const MemoryRegionOps cdj_usbh_ops = {
    .read = cdj_usbh_read,
    .write = cdj_usbh_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- the device -------------------------------------------------------- */

static void cdj_usbh_reset(DeviceState *dev)
{
    CdjUsbhState *s = CDJ_USBH(dev);
    unsigned nr;

    memset(s->reg, 0, sizeof(s->reg));
    for (nr = 0; nr < NR_PIPES; nr++) {
        CdjUsbhPipe *p = &s->pipe[nr];

        p->cfg = p->buf = p->maxp = p->peri = 0;
        p->ctr = PIPECTR_SQMON;
        p->tre = p->trn = 0;
        p->trn_count = 0;
        p->stopped = false;
        p->in_total = 0;
        p->retries = 0;
        pipe_rx_clear(p);
        pipe_tx_clear(p);
    }
    memset(s->dma, 0, sizeof(s->dma));
    s->reset_done = false;
    s->setup_pending = false;
    s->irq_level = false;
    qemu_set_irq(s->irq, 0);
}

static void cdj_usbh_realize(DeviceState *dev, Error **errp)
{
    CdjUsbhState *s = CDJ_USBH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    unsigned nr;

    /* CDJ_USBH_TRACE=scsi: only the mass-storage commands and their
       statuses, failures and DMA -- the full trace prints every register
       read, hundreds of megabytes over a stick's library. */
    s->trace_scsi = !g_strcmp0(getenv("CDJ_USBH_TRACE"), "scsi")
        || !g_strcmp0(getenv("CDJ_USBH_TRACE"), "scsi+write");
    s->trace_on_write = !g_strcmp0(getenv("CDJ_USBH_TRACE"), "scsi+write");
    s->trace = getenv("CDJ_USBH_TRACE") != NULL && !s->trace_scsi;
    for (nr = 0; nr < NR_PIPES; nr++) {
        CdjUsbhPipe *p = &s->pipe[nr];

        p->nr = nr;
        p->rx = g_byte_array_new();
        p->tx = g_byte_array_new();
        usb_packet_init(&p->packet);
    }
    usb_bus_new(&s->bus, sizeof(s->bus), &cdj_usbh_bus_ops, dev);
    for (nr = 0; nr < ARRAY_SIZE(s->ports); nr++) {
        usb_register_port(&s->bus, &s->ports[nr], s, nr, &cdj_usbh_port_ops,
                          USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL
                          | USB_SPEED_MASK_HIGH);
        usb_port_location(&s->ports[nr], NULL, nr + 1);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_usbh_timer, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &cdj_usbh_ops, s,
                          "cdj2000.usbh", CDJ_USBH_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void cdj_usbh_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cdj_usbh_realize;
    dc->desc = "SH7764 USB 2.0 host/function module (host)";
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    device_class_set_legacy_reset(dc, cdj_usbh_reset);
}

static const TypeInfo cdj_usbh_info = {
    .name = TYPE_CDJ_USBH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CdjUsbhState),
    .class_init = cdj_usbh_class_init,
};

static void cdj_usbh_register_types(void)
{
    type_register_static(&cdj_usbh_info);
}

type_init(cdj_usbh_register_types)

void cdj_usbh_init(MemoryRegion *system, qemu_irq irq,
                   CdjUsbhDmaDone dma_done, void *dma_opaque)
{
    DeviceState *dev = qdev_new(TYPE_CDJ_USBH);
    CdjUsbhState *s = CDJ_USBH(dev);

    s->irq = irq;
    s->dma_done = dma_done;
    s->dma_opaque = dma_opaque;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    cdj_usbh_singleton = s;

    /* Both the P4 window the firmware uses and the area-7 physical one. */
    memory_region_add_subregion_overlap(system, CDJ_USBH_BASE, &s->iomem, 1);
    memory_region_init_alias(&s->iomem_p4, OBJECT(dev), "cdj2000.usbh-p4",
                             &s->iomem, 0, CDJ_USBH_SIZE);
    memory_region_add_subregion_overlap(system, P4ADDR(CDJ_USBH_BASE),
                                        &s->iomem_p4, 1);
}
