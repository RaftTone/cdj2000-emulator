/*
 * Pioneer CDJ-2000 USB controller -- what it says.
 *
 * The bus surface is cdj2000_usb.c; this file is the whole of the controller's
 * behaviour.
 *
 * It began as a register file that remembers what was written, which is all
 * the firmware's bring-up needs (every access during bring-up is a
 * read-modify-write of bits it set itself; cdj2000_usb.h lists each one with the
 * address that proves it).  That derivation, the two inputs the guest cannot
 * produce (the enable bit and VBUS) and the "absent" control arm are
 * LycheeAPPF's and are kept exactly as they were:
 *
 *   - the enable bit.  0x2399c8 writes 1 to W[0x00] and then polls bit 0 until
 *     it reads back, 255 attempts.  A register file satisfies that by itself,
 *     but the bit is named and watched here because it is the single condition
 *     that decides between caution 61 and a working port.
 *   - VBUS.  W[0x18] bit 7 is only ever read, never written, and the strings
 *     next to its reader say "USBF Init Vbus = %d" / "USBF Vbus ON" / "USBF Vbus
 *     OFF".  It is the one input that comes from outside the machine: whether a
 *     computer is plugged into the type-B socket.  It defaults to **off**,
 *     because a player switched on with nothing in that socket is the normal
 *     case and must not be an error.  CDJ_USB_VBUS=1 says a host is attached.
 *
 * What follows is RaftTone's, written from the Renesas M66291GP/HP data sheet
 * (RJJ03F0074-0111Z, Rev.1.11, sections 2 and 3): the chip itself rather than a
 * register file.  Interrupt status and enable with the INT0/INT1 pins, the
 * device state machine (Powered, Default, Address, Configured, Suspended), EP0
 * with its setup stage, data stage and status stage and the SET_ADDRESS and
 * SET_CONFIGURATION automatic responses, the FIFO ports, and endpoints 1..6 with
 * their buffers, IVAL, BRDY/NRDY/BEMP.  On the other side of the chip is a
 * *host interface* (cdj_usb_model_host_*) that carries one USB transaction at
 * a time; cdj2000_usb.c puts a socket in front of it.
 *
 * What is deliberately not modelled: SOF and isochronous transfers (no frame
 * counter runs), the DMA handshake of D0/D1 (the ports exist and are
 * CPU-readable, nothing asserts *Dreq), DATA0/DATA1 toggling (a transaction
 * either happens or it does not, the host never sees a toggle error), and the
 * timing of any of it: a transaction is carried out at once.
 *
 * Copyright (C) 2026 LycheeAPPF
 * Copyright (C) 2026 RaftTone
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "cdj2000_usb_model.h"

#define EP_BUF 1024

typedef struct {
    uint8_t cpu[EP_BUF];        /* the CPU side buffer */
    unsigned cpu_len, cpu_pos;
    uint8_t sie[EP_BUF];        /* the SIE side buffer */
    unsigned sie_len, sie_pos;
    bool sie_full;              /* SIE side holds a complete transfer */
    bool ival;                  /* IVAL: IN write complete / OUT readable */
} Ep;

struct CdjUsbModel {
    /*
     * The register file, 16 bits per entry.  Only offsets below 0x80 are ever
     * touched by the image; the rest of the mapped page reads as zero and is
     * reported by the bus surface instead of being stored.  Registers with a
     * behaviour of their own are computed on access; the rest read back what
     * was written.
     */
    uint16_t reg[0x40];
    bool absent;                /* CDJ_USB_ABSENT: answer like unmapped memory */
    bool vbus;                  /* CDJ_USB_VBUS or the host: a computer is on USB-B */
    bool enabled;               /* the guest has set bit 0 of 0x00 */

    /* Interrupt status. */
    uint16_t sts0;              /* the latched bits of INTSTS0 (15..11) */
    uint8_t rdy, nrdy, emp;     /* INTSTS1..3, bits 6..0 = EP6..EP0 */
    bool valid;                 /* VALID */
    bool ctrt_hold;             /* a sequence error is holding CTRT */
    bool irq[2];

    /* Device and control transfer state. */
    unsigned dvsq;              /* DVSQ, bits 6:4 of INTSTS0 */
    unsigned ctsq;              /* CTSQ, bits 2:0 of INTSTS0 */
    unsigned address;
    uint8_t setup[8];
    unsigned ep0_pid;           /* EP0_PID, 0 NAK, 1 BUF, 2/3 STALL */
    bool ccpl;
    bool ep0_data_sent;         /* control read: a data packet went out */
    bool ep0_data_acked;        /* control write: a data packet was taken */

    Ep ep[CDJ_USB_EPS];

    CdjUsbIrqFn irq_fn;
    void *irq_opaque;
    uint32_t resets, setups, ins, outs, naks;
};

#define CDJ_USB_REG_SPAN  (sizeof(((CdjUsbModel *)0)->reg) / sizeof(uint16_t) * 2)

/* ------------------------------------------------------------- the registers */

static inline uint16_t r(const CdjUsbModel *m, unsigned offset)
{
    return m->reg[offset / 2];
}

static inline void w(CdjUsbModel *m, unsigned offset, uint16_t value)
{
    m->reg[offset / 2] = value;
}

/* EPi_0CONFIG / EPi_1CONFIG fields, i = 1..6. */
static inline unsigned ep_typ(const CdjUsbModel *m, unsigned i)
{
    return r(m, M66291_EP_CONFIG0(i)) >> 14 & 3;
}
static inline bool ep_dbl(const CdjUsbModel *m, unsigned i)
{
    return i && (r(m, M66291_EP_CONFIG0(i)) & 0x80);
}
static inline bool ep_rwmd(const CdjUsbModel *m, unsigned i)
{
    return i && (r(m, M66291_EP_CONFIG0(i)) & 0x40);
}
static inline unsigned ep_bufsiz(const CdjUsbModel *m, unsigned i)
{
    return ((r(m, M66291_EP_CONFIG0(i)) >> 8 & 15) + 1) * 64;
}

static inline bool ep0_isel(const CdjUsbModel *m)
{
    return r(m, M66291_EP0_SELECT) & 1;
}

/* True when endpoint i moves data to the host. */
static bool ep_is_in(const CdjUsbModel *m, unsigned i)
{
    if (i == 0) {
        return ep0_isel(m);
    }
    return r(m, M66291_EP_CONFIG0(i)) & 0x2000;
}

static unsigned ep_mxps(const CdjUsbModel *m, unsigned i)
{
    if (i == 0) {
        unsigned size = r(m, M66291_EP0_SIZE) & 0x7f;
        return size ? size : 8;
    }
    return r(m, M66291_EP_CONFIG1(i)) & 0x3ff;
}

static unsigned ep_pid(const CdjUsbModel *m, unsigned i)
{
    return i == 0 ? m->ep0_pid : r(m, M66291_EP_CONFIG1(i)) >> 14 & 3;
}

static void ep_set_pid(CdjUsbModel *m, unsigned i, unsigned pid)
{
    if (i == 0) {
        m->ep0_pid = pid & 3;
    } else {
        w(m, M66291_EP_CONFIG1(i),
          (r(m, M66291_EP_CONFIG1(i)) & 0x3fff) | (pid & 3) << 14);
    }
}

/* The size at which a write to an IN buffer counts as complete. */
static unsigned ep_write_complete(const CdjUsbModel *m, unsigned i)
{
    if (i == 0) {
        if (r(m, M66291_CONTROL) & 0x8000) {            /* CTRR */
            unsigned len = r(m, M66291_EP0_SENDLEN) & 0x1ff;
            return len ? len : ep_mxps(m, 0);
        }
        return ep_mxps(m, 0);
    }
    return ep_rwmd(m, i) ? ep_bufsiz(m, i) : ep_mxps(m, i);
}

static bool rdym(const CdjUsbModel *m)
{
    return r(m, M66291_POLARITY) & 4;
}

/* ------------------------------------------------------------ the interrupts */

/* INTSTS0 as the CPU sees it. */
static uint16_t sts0_value(const CdjUsbModel *m)
{
    uint16_t value = m->sts0;
    uint16_t enb1 = r(m, M66291_INTENB1), enb2 = r(m, M66291_INTENB2),
             enb3 = r(m, M66291_INTENB3);

    if (m->emp & enb3 & 0x7f) {
        value |= M66291_BEMP;
    }
    if (m->nrdy & enb2 & 0x7f) {
        value |= M66291_INTN;
    }
    if (m->rdy & enb1 & 0x7f) {
        value |= M66291_INTR;
    }
    if (m->vbus) {
        value |= M66291_VBUS_PIN;
    }
    value |= (m->dvsq & 7) << 4 | (m->valid ? M66291_VALID : 0) | (m->ctsq & 7);
    return value;
}

/* The assignment bit of each source in POLARITY_CNT (0 = INT0, 1 = INT1). */
static unsigned pin_of(const CdjUsbModel *m, uint16_t source)
{
    uint16_t pol = r(m, M66291_POLARITY);

    switch (source) {
    case M66291_VBUS: return pol >> 15 & 1;
    case M66291_RESM: return pol >> 14 & 1;
    case M66291_SOFR: return pol >> 13 & 1;
    case M66291_DVST: return pol >> 12 & 1;
    case M66291_CTRT: return pol >> 11 & 1;
    case M66291_BEMP: return pol >> 10 & 1;
    case M66291_INTN: return pol >> 9 & 1;
    default:          return pol >> 8 & 1;      /* INTR */
    }
}

static void irq_update(CdjUsbModel *m)
{
    static const uint16_t sources[] = {
        M66291_VBUS, M66291_RESM, M66291_SOFR, M66291_DVST, M66291_CTRT,
        M66291_BEMP, M66291_INTN, M66291_INTR,
    };
    uint16_t pending = sts0_value(m) & r(m, M66291_INTENB0) & 0xff00;
    bool level[2] = { false, false };

    for (size_t k = 0; k < sizeof sources / sizeof sources[0]; k++) {
        if (pending & sources[k]) {
            level[pin_of(m, sources[k])] = true;
        }
    }
    for (unsigned pin = 0; pin < 2; pin++) {
        if (level[pin] != m->irq[pin]) {
            m->irq[pin] = level[pin];
            if (m->irq_fn) {
                m->irq_fn(m->irq_opaque, pin, level[pin]);
            }
        }
    }
}

static void set_rdy(CdjUsbModel *m, unsigned i)
{
    /* Control read (EP0 as an IN buffer) never raises BRDY. */
    if (i == 0 && ep_is_in(m, 0)) {
        return;
    }
    m->rdy |= 1u << i;
}

static void ctrt_raise(CdjUsbModel *m, uint16_t enable_bit)
{
    /* Only the setup stage ignores the enable bits (data sheet 2.8). */
    if (enable_bit == 0 || (r(m, M66291_INTENB0) & enable_bit)) {
        m->sts0 |= M66291_CTRT;
    }
}

static void dvst_raise(CdjUsbModel *m, uint16_t enable_bit)
{
    if (r(m, M66291_INTENB0) & enable_bit) {
        m->sts0 |= M66291_DVST;
    }
}

/* IVAL changes, and BRDY follows (data sheet 2.13). */
static void ival_set(CdjUsbModel *m, unsigned i, bool value)
{
    Ep *ep = &m->ep[i];
    bool in = ep_is_in(m, i);

    if (ep->ival == value) {
        return;
    }
    ep->ival = value;
    if (in == !value) {
        set_rdy(m, i);                  /* OUT: 0 -> 1.  IN: 1 -> 0. */
    } else if (!rdym(m)) {
        m->rdy &= ~(1u << i);
    }
}

/* ------------------------------------------------------------- the endpoints */

/* The CPU may touch an endpoint's FIFO. */
static bool clocked(const CdjUsbModel *m)
{
    return m->enabled && (r(m, M66291_USB_ENABLE) & M66291_SCKE);
}

static void ep_clear(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    ep->cpu_len = ep->cpu_pos = 0;
    ep->sie_len = ep->sie_pos = 0;
    ep->sie_full = false;
    ep->ival = false;
    m->rdy &= ~(1u << i);
}

/* An OUT transfer has arrived whole on the SIE side: hand it to the CPU. */
static void out_toggle(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    if (ep->sie_full && !ep->ival) {
        memcpy(ep->cpu, ep->sie, ep->sie_len);
        ep->cpu_len = ep->sie_len;
        ep->cpu_pos = 0;
        ep->sie_len = 0;
        ep->sie_full = false;
        ival_set(m, i, true);
    }
}

/* The CPU has written a whole IN transfer (or forced it short). */
static void in_commit(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    if (!ep->sie_full) {
        memcpy(ep->sie, ep->cpu, ep->cpu_len);
        ep->sie_len = ep->cpu_len;
        ep->sie_pos = 0;
        ep->sie_full = true;
        ep->cpu_len = 0;
        if (ep_dbl(m, i)) {
            set_rdy(m, i);              /* the CPU side is free again */
        } else {
            ival_set(m, i, true);
        }
    } else {
        ival_set(m, i, true);           /* waits for the SIE side to drain */
    }
}

/* The SIE side has sent everything. */
static void in_drained(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    ep->sie_full = false;
    ep->sie_len = ep->sie_pos = 0;
    if (ep_dbl(m, i) && ep->ival) {
        memcpy(ep->sie, ep->cpu, ep->cpu_len);
        ep->sie_len = ep->cpu_len;
        ep->sie_pos = 0;
        ep->sie_full = true;
        ep->cpu_len = 0;
        ival_set(m, i, false);          /* BRDY: the CPU side is free */
        return;
    }
    if (!ep_dbl(m, i)) {
        ival_set(m, i, false);
        ep->cpu_len = 0;
    }
    m->emp |= 1u << i;                  /* BEMP: nothing left to send */
}

static void port_push(CdjUsbModel *m, unsigned i, uint8_t byte)
{
    Ep *ep = &m->ep[i];

    if (!ep_is_in(m, i) || ep->ival || ep->cpu_len >= EP_BUF) {
        return;
    }
    ep->cpu[ep->cpu_len++] = byte;
    if (ep->cpu_len >= ep_write_complete(m, i)) {
        in_commit(m, i);
    }
}

static int port_pop(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    if (ep_is_in(m, i) || !ep->ival || ep->cpu_pos >= ep->cpu_len) {
        return -1;
    }
    int byte = ep->cpu[ep->cpu_pos++];
    if (ep->cpu_pos >= ep->cpu_len) {
        ep->cpu_len = ep->cpu_pos = 0;
        ival_set(m, i, false);
        out_toggle(m, i);
    }
    return byte;
}

static unsigned port_dtln(const CdjUsbModel *m, unsigned i, bool rcnt)
{
    const Ep *ep = &m->ep[i];

    if (ep_is_in(m, i) || !ep->ival) {
        return 0;
    }
    return rcnt ? ep->cpu_len - ep->cpu_pos : ep->cpu_len;
}

/* The CPU's BCLR, IVAL=1 and RWND, shared by EP0 and the three ports. */
static void ep_bclr(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    if (ep_is_in(m, i)) {
        if (!ep->ival) {
            ep->cpu_len = 0;
        } else if (i == 0) {
            /* EP0 alone can also drop what the SIE side holds. */
            ep->sie_full = false;
            ep->sie_len = ep->sie_pos = 0;
            ep->cpu_len = 0;
            ival_set(m, 0, false);
        }
    } else if (ep->ival) {
        ep->cpu_len = ep->cpu_pos = 0;
        ival_set(m, i, false);
        out_toggle(m, i);
    }
}

static void ep_force_ival(CdjUsbModel *m, unsigned i, bool bclr)
{
    Ep *ep = &m->ep[i];

    if (!ep_is_in(m, i) || ep->ival) {
        return;
    }
    if (bclr) {
        ep->cpu_len = 0;                /* IVAL with BCLR: a zero-length packet */
    }
    in_commit(m, i);
}

static void ep_rwnd(CdjUsbModel *m, unsigned i)
{
    Ep *ep = &m->ep[i];

    if (ep_is_in(m, i)) {
        if (!ep->ival) {
            ep->cpu_len = 0;
        }
    } else if (ep->ival) {
        ep->cpu_pos = 0;
    }
}

/* ------------------------------------------------------------- FIFO ports */

enum { PORT_EP0, PORT_CPU, PORT_D0, PORT_D1 };

static unsigned port_select_offset(unsigned port)
{
    static const unsigned off[] = { M66291_EP0_SELECT, M66291_CPU_SELECT,
                                    M66291_D0_SELECT, M66291_D1_SELECT };
    return off[port];
}

/* The endpoint a port is pointed at, or 0xff for none. */
static unsigned port_ep(const CdjUsbModel *m, unsigned port)
{
    unsigned ep;

    if (port == PORT_EP0) {
        return 0;
    }
    ep = r(m, port_select_offset(port)) & 15;
    return ep >= 1 && ep <= 6 ? ep : 0xff;
}

static bool port_octl(const CdjUsbModel *m, unsigned port)
{
    unsigned ep = port_ep(m, port);

    if (port == PORT_EP0) {
        return r(m, M66291_EP0_SELECT) & 0x400;
    }
    if (r(m, port_select_offset(port)) & 0x40) {
        return true;
    }
    return ep != 0xff && (r(m, M66291_EP_CONFIG1(ep)) & 0x400);
}

static bool port_bswp(const CdjUsbModel *m, unsigned port)
{
    return r(m, port_select_offset(port)) & 0x80;
}

static bool port_rcnt(const CdjUsbModel *m, unsigned port)
{
    return r(m, port_select_offset(port)) & 0x8000;
}

static uint16_t port_data_read(CdjUsbModel *m, unsigned port)
{
    unsigned ep = port_ep(m, port);
    int lo, hi = 0;

    if (ep == 0xff || !clocked(m)) {
        return 0;
    }
    lo = port_pop(m, ep);
    if (lo < 0) {
        return 0;
    }
    if (!port_octl(m, port)) {
        hi = port_pop(m, ep);
        if (hi < 0) {
            hi = 0;
        }
    }
    return port_bswp(m, port) ? (uint16_t)(lo << 8 | hi) : (uint16_t)(hi << 8 | lo);
}

static void port_data_write(CdjUsbModel *m, unsigned port, uint16_t value)
{
    unsigned ep = port_ep(m, port);

    if (ep == 0xff || !clocked(m)) {
        return;
    }
    if (port_octl(m, port)) {
        port_push(m, ep, value & 0xff);
    } else if (port_bswp(m, port)) {
        port_push(m, ep, value >> 8);
        port_push(m, ep, value & 0xff);
    } else {
        port_push(m, ep, value & 0xff);
        port_push(m, ep, value >> 8);
    }
}

static uint16_t port_control_read(const CdjUsbModel *m, unsigned port)
{
    unsigned ep = port_ep(m, port);
    uint16_t value = 0;

    if (port == PORT_EP0) {
        value = m->ep0_pid << 14 | (m->ccpl ? 0x400 : 0);
        if (!clocked(m)) {
            return value | 0x800;                 /* E0req */
        }
        if (m->ep[0].ival) {
            value |= 0x2000;
        }
        return value | (ep_is_in(m, 0) ? 0 : port_dtln(m, 0, port_rcnt(m, 0)));
    }
    if (ep == 0xff || !clocked(m)) {
        return 0x0800;                            /* Creq */
    }
    if (m->ep[ep].ival) {
        value |= 0x2000;
    }
    return value | port_dtln(m, ep, port_rcnt(m, port));
}

static void port_control_write(CdjUsbModel *m, unsigned port, uint16_t value)
{
    unsigned ep = port_ep(m, port);

    if (ep == 0xff) {
        return;
    }
    if (port == PORT_EP0) {
        unsigned pid = value >> 14 & 3;

        /* VALID makes the chip ignore NAK, BUF and CCPL (data sheet 2.12). */
        if (!m->valid || pid >= 2) {
            m->ep0_pid = pid;
        }
        if (!m->valid && (value & 0x400)) {
            m->ccpl = true;
        }
    }
    if ((value & 0x2000) && (value & 0x1000)) {
        ep_force_ival(m, ep, true);
        return;
    }
    if (value & 0x2000) {
        ep_force_ival(m, ep, false);
    }
    if (value & 0x1000) {
        ep_bclr(m, ep);
    }
}

/* ----------------------------------------------------------- device states */

/* Software reset (USBE 1 -> 0): the registers the data sheet marks H'0000. */
static void soft_reset(CdjUsbModel *m)
{
    static const unsigned cleared[] = {
        0x02, 0x04, 0x08, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a,
        0x1c, 0x1e, 0x20, 0x22, 0x24, 0x26, 0x58,
    };

    for (size_t k = 0; k < sizeof cleared / sizeof cleared[0]; k++) {
        w(m, cleared[k], 0);
    }
    m->sts0 = 0;
    m->rdy = m->nrdy = m->emp = 0;
    m->valid = false;
    m->ctrt_hold = false;
    m->dvsq = 0;
    m->ctsq = 0;
    m->address = 0;
    m->ccpl = false;
    m->ep0_pid = 0;
    for (unsigned i = 0; i < CDJ_USB_EPS; i++) {
        ep_clear(m, i);
    }
    irq_update(m);
}

CdjUsbModel *cdj_usb_model_new(bool absent, bool vbus)
{
    CdjUsbModel *model = g_new0(CdjUsbModel, 1);

    model->absent = absent;
    model->vbus = vbus;
    cdj_usb_model_reset(model);
    return model;
}

void cdj_usb_model_reset(CdjUsbModel *model)
{
    CdjUsbIrqFn fn = model->irq_fn;
    void *opaque = model->irq_opaque;
    bool vbus = model->vbus, absent = model->absent;
    bool raised[2] = { model->irq[0], model->irq[1] };

    memset(model->reg, 0, sizeof(model->reg));
    model->enabled = false;
    /* Hardware reset values (data sheet figures 2.1 and 2.2). */
    w(model, M66291_EP0_SIZE, 8);
    for (unsigned i = 1; i <= 6; i++) {
        w(model, M66291_EP_CONFIG1(i), 0x0040);
    }
    soft_reset(model);
    model->vbus = vbus;
    model->absent = absent;
    model->irq_fn = fn;
    model->irq_opaque = opaque;
    for (unsigned pin = 0; pin < 2; pin++) {
        model->irq[pin] = raised[pin];
    }
    irq_update(model);
}

void cdj_usb_model_set_irq(CdjUsbModel *model, CdjUsbIrqFn fn, void *opaque)
{
    model->irq_fn = fn;
    model->irq_opaque = opaque;
    for (unsigned pin = 0; pin < 2; pin++) {
        if (fn && model->irq[pin]) {
            fn(opaque, pin, true);
        }
    }
}

bool cdj_usb_model_enabled(const CdjUsbModel *model)
{
    return model->enabled;
}

bool cdj_usb_model_vbus(const CdjUsbModel *model)
{
    return model->vbus;
}

void cdj_usb_model_set_vbus(CdjUsbModel *model, bool vbus)
{
    if (model->vbus == vbus) {
        return;
    }
    model->vbus = vbus;
    model->sts0 |= M66291_VBUS;             /* the pin changed */
    if (!vbus) {
        model->dvsq = 0;                    /* the bus is gone */
        model->ctsq = 0;
        model->address = 0;
        model->valid = false;
        ep_clear(model, 0);
    }
    irq_update(model);
}

bool cdj_usb_model_attached(const CdjUsbModel *model)
{
    uint16_t en = r(model, M66291_USB_ENABLE);

    return !model->absent && model->vbus && model->enabled && (en & M66291_SCKE)
        && (en & M66291_USBPC) && (en & M66291_TRON);
}

uint16_t cdj_usb_model_read(CdjUsbModel *model, uint32_t offset)
{
    uint16_t value;

    /*
     * The control arm.  Unmapped memory on this board reads zero, so answering
     * zero here reproduces the machine before the device existed exactly --
     * including the 255 futile attempts of 0x2399c8 -- while still going through
     * the same code path, so a trace shows what the driver was asking for.
     */
    if (model->absent || offset >= CDJ_USB_REG_SPAN) {
        return 0;
    }

    value = r(model, offset & ~1u);

    switch (offset & ~1u) {
    case M66291_REMOTE_WAKEUP:
    case M66291_SEQUENCE_BIT:
        return 0;
    case M66291_USB_ADDRESS:
        return model->address;
    case M66291_INTSTS0:
        /*
         * VBUS is the model's to answer, not the register file's: nothing in the
         * image ever writes 0x18, and the bit means "a computer is on the other
         * end of the cable".
         */
        return sts0_value(model);
    case M66291_INTSTS1:
        return model->rdy;
    case M66291_INTSTS2:
        return model->nrdy;
    case M66291_INTSTS3:
        return model->emp;
    case M66291_REQUEST:
        return model->setup[0] | model->setup[1] << 8;
    case M66291_VALUE:
        return model->setup[2] | model->setup[3] << 8;
    case M66291_INDEX:
        return model->setup[4] | model->setup[5] << 8;
    case M66291_LENGTH:
        return model->setup[6] | model->setup[7] << 8;
    case M66291_EP0_SELECT:
    case M66291_CPU_SELECT:
    case M66291_D0_SELECT:
    case M66291_D1_SELECT:
        return value & ~0x1000;             /* RWND reads 0 */
    case M66291_EP0_CONTROL:
        value = port_control_read(model, PORT_EP0);
        break;
    case M66291_EP0_DATA:
        value = port_data_read(model, PORT_EP0);
        irq_update(model);
        break;
    case M66291_CPU_CONTROL:
        value = port_control_read(model, PORT_CPU);
        break;
    case M66291_CPU_DATA:
        value = port_data_read(model, PORT_CPU);
        irq_update(model);
        break;
    case M66291_D0_CONTROL:
        value = port_control_read(model, PORT_D0);
        break;
    case M66291_D0_DATA:
        value = port_data_read(model, PORT_D0);
        irq_update(model);
        break;
    case M66291_D1_CONTROL:
        value = port_control_read(model, PORT_D1);
        break;
    case M66291_D1_DATA:
        value = port_data_read(model, PORT_D1);
        irq_update(model);
        break;
    case M66291_SIE_STATUS: {
        unsigned ep = port_ep(model, PORT_CPU);

        return ep == 0xff ? 0 : model->ep[ep].sie_len - model->ep[ep].sie_pos;
    }
    case M66291_FIFO_STATUS: {
        uint16_t status = 0;

        /* The same conditions as INTSTS1, but never cleared by a write. */
        for (unsigned i = 0; i < CDJ_USB_EPS; i++) {
            bool in = ep_is_in(model, i);

            if (i && !ep_typ(model, i)) {
                continue;
            }
            if (i == 0 ? (!in && model->ep[0].ival)
                       : (in != model->ep[i].ival)) {
                status |= 1u << i;
            }
        }
        return status;
    }
    default:
        break;
    }
    return value;
}

bool cdj_usb_model_write_byte(CdjUsbModel *model, uint32_t offset, uint8_t value)
{
    unsigned port;

    if (model->absent) {
        return false;
    }
    switch (offset & ~1u) {
    case M66291_EP0_DATA:  port = PORT_EP0; break;
    case M66291_CPU_DATA:  port = PORT_CPU; break;
    case M66291_D0_DATA:   port = PORT_D0; break;
    case M66291_D1_DATA:   port = PORT_D1; break;
    default:               return false;
    }
    if (port_ep(model, port) != 0xff && clocked(model)) {
        port_push(model, port_ep(model, port), value);
        irq_update(model);
    }
    return true;
}

static void port_select_write(CdjUsbModel *m, unsigned port, uint16_t value)
{
    unsigned offset = port_select_offset(port);
    unsigned ep;

    w(m, offset, value & ~0x1000);
    ep = port_ep(m, port);
    if ((value & 0x1000) && ep != 0xff) {
        ep_rwnd(m, ep);
    }
}

static void ep_config0_write(CdjUsbModel *m, unsigned i, uint16_t value)
{
    uint16_t old = r(m, M66291_EP_CONFIG0(i));

    w(m, M66291_EP_CONFIG0(i), value);
    if (!(old & 0x2000) && (value & 0x2000)) {
        m->rdy |= 1u << i;                  /* DIR 0 -> 1: the buffer is writable */
    }
}

static void ep_config1_write(CdjUsbModel *m, unsigned i, uint16_t value)
{
    uint16_t old = r(m, M66291_EP_CONFIG1(i));

    w(m, M66291_EP_CONFIG1(i), value);
    if ((value & 0x800) && !(old & 0x800)) {
        ep_clear(m, i);                     /* ACLR: every buffer is cleared */
    }
}

void cdj_usb_model_write(CdjUsbModel *model, uint32_t offset, uint16_t value)
{
    CdjUsbModel *m = model;

    if (m->absent || offset >= CDJ_USB_REG_SPAN) {
        return;
    }
    offset &= ~1u;

    switch (offset) {
    case M66291_USB_ENABLE: {
        bool was = m->enabled;

        w(m, offset, value);
        /*
         * 0x2399bc turns the module off and 0x2399c8 turns it on; the poll that
         * follows the second is the one that decides whether MAIN reports the
         * port broken.  Tracking it costs nothing and makes the trace say
         * "the module came up" rather than "a word was written".
         */
        m->enabled = (value & CDJ_USB_ENABLE_BIT) != 0;
        if (was && !m->enabled) {
            soft_reset(m);
        }
        break;
    }
    case M66291_REMOTE_WAKEUP:
    case M66291_SEQUENCE_BIT:
    case M66291_USB_ADDRESS:
    case M66291_REQUEST:
    case M66291_VALUE:
    case M66291_INDEX:
    case M66291_LENGTH:
    case M66291_FIFO_STATUS:
        break;                              /* write-only action or read-only */
    case M66291_INTSTS0: {
        /* VBUS, RESM, SOFR, DVST, CTRT and VALID clear by writing 0. */
        m->sts0 &= value | ~(M66291_VBUS | M66291_RESM | M66291_SOFR
                             | M66291_DVST | M66291_CTRT);
        if (!(value & M66291_CTRT) && m->ctrt_hold) {
            m->ctrt_hold = false;
            if (m->ctsq == 6) {
                m->ctsq = 0;
            }
        }
        if (!(value & M66291_VALID)) {
            m->valid = false;
        }
        break;
    }
    case M66291_INTSTS1:
        if (rdym(m)) {
            m->rdy &= value;
        }
        break;
    case M66291_INTSTS2:
        m->nrdy &= value;
        break;
    case M66291_INTSTS3:
        m->emp &= value;
        break;
    case M66291_EP0_SELECT:
        port_select_write(m, PORT_EP0, value);
        break;
    case M66291_CPU_SELECT:
        port_select_write(m, PORT_CPU, value);
        break;
    case M66291_D0_SELECT:
        port_select_write(m, PORT_D0, value);
        break;
    case M66291_D1_SELECT:
        port_select_write(m, PORT_D1, value);
        break;
    case M66291_EP0_CONTROL:
        port_control_write(m, PORT_EP0, value);
        break;
    case M66291_CPU_CONTROL:
        port_control_write(m, PORT_CPU, value);
        break;
    case M66291_D0_CONTROL:
        port_control_write(m, PORT_D0, value);
        break;
    case M66291_D1_CONTROL:
        port_control_write(m, PORT_D1, value);
        break;
    case M66291_EP0_DATA:
        port_data_write(m, PORT_EP0, value);
        break;
    case M66291_CPU_DATA:
        port_data_write(m, PORT_CPU, value);
        break;
    case M66291_D0_DATA:
        port_data_write(m, PORT_D0, value);
        break;
    case M66291_D1_DATA:
        port_data_write(m, PORT_D1, value);
        break;
    case M66291_SIE_STATUS: {
        unsigned ep = port_ep(m, PORT_CPU);

        if (ep != 0xff && ep_is_in(m, ep) && (value & 0x1000)) {
            /* SCLR: drop what waits for the host. */
            m->ep[ep].sie_full = false;
            m->ep[ep].sie_len = m->ep[ep].sie_pos = 0;
            if (!ep_dbl(m, ep)) {
                ival_set(m, ep, false);
            }
        } else if (ep != 0xff && !ep_is_in(m, ep) && (value & 0x2000)) {
            /* TGL: force the buffer being received into the CPU's hands. */
            Ep *e = &m->ep[ep];

            if (!e->sie_full && e->sie_len) {
                e->sie_full = true;
            }
            out_toggle(m, ep);
        }
        break;
    }
    case M66291_CONTROL:
    case M66291_EP0_SIZE:
    case M66291_AUTO_RESPONSE:
    case M66291_EP0_SENDLEN:
    default:
        if (offset >= M66291_EP_CONFIG0(1) && offset <= M66291_EP_CONFIG1(6)) {
            unsigned i = (offset - 0x60) / 4 + 1;

            if (offset & 2) {
                ep_config1_write(m, i, value);
            } else {
                ep_config0_write(m, i, value);
            }
        } else {
            w(m, offset, value);
        }
        break;
    }
    irq_update(m);
}

/* --------------------------------------------------------------- the host */

static bool bus_active(const CdjUsbModel *m)
{
    return cdj_usb_model_attached(m);
}

static bool addressed(const CdjUsbModel *m, unsigned addr)
{
    return bus_active(m) && m->dvsq != 0 && addr == m->address;
}

bool cdj_usb_model_host_reset(CdjUsbModel *m)
{
    if (!bus_active(m)) {
        return false;
    }
    m->resets++;
    m->dvsq = 1;
    m->ctsq = 0;
    m->address = 0;
    m->valid = false;
    m->ctrt_hold = false;
    m->ccpl = false;
    ep_clear(m, 0);
    dvst_raise(m, 0x80);                    /* URST */
    irq_update(m);
    return true;
}

bool cdj_usb_model_host_suspend(CdjUsbModel *m)
{
    if (!bus_active(m) || m->dvsq == 0 || m->dvsq >= 4) {
        return false;
    }
    m->dvsq |= 4;
    dvst_raise(m, 0x10);                    /* SUSP */
    irq_update(m);
    return true;
}

bool cdj_usb_model_host_resume(CdjUsbModel *m)
{
    if (!bus_active(m) || m->dvsq < 4) {
        return false;
    }
    m->dvsq &= 3;
    m->sts0 |= M66291_RESM;
    irq_update(m);
    return true;
}

static CdjUsbResult nak(CdjUsbModel *m)
{
    m->naks++;
    return CDJ_USB_NAK;
}

static CdjUsbResult nak_not_ready(CdjUsbModel *m, unsigned ep)
{
    m->nrdy |= 1u << ep;
    return nak(m);
}

/* Control transfer sequence error: EP0 stalls until the next setup. */
static CdjUsbResult ctrl_seq_error(CdjUsbModel *m)
{
    m->ctsq = 6;
    m->ep0_pid = 2;
    if (r(m, M66291_INTENB0) & 1) {         /* SERR */
        m->sts0 |= M66291_CTRT;
        m->ctrt_hold = true;
    }
    return CDJ_USB_STALL;
}

/* The status stage has completed with an ACK. */
static void ctrl_complete(CdjUsbModel *m)
{
    unsigned value = m->setup[2] | m->setup[3] << 8;

    if (m->setup[0] == 0x00 && m->setup[1] == 5 && m->dvsq == 1) {
        m->address = value & 0x7f;
        if (m->address != 0) {
            m->dvsq = 2;
            dvst_raise(m, 0x40);            /* SADR */
        }
    } else if (m->setup[0] == 0x00 && m->setup[1] == 9) {
        if (m->dvsq == 2 && (value & 0xff)) {
            m->dvsq = 3;
            dvst_raise(m, 0x20);            /* SCFG */
        } else if (m->dvsq == 3 && !(value & 0xff)) {
            m->dvsq = 2;
            dvst_raise(m, 0x20);
        }
    }
    m->ctsq = 0;
    ctrt_raise(m, 0x02);                    /* CMPL */
}

/* A status-stage token has arrived (IN for a write, OUT for a read). */
static CdjUsbResult ctrl_status(CdjUsbModel *m)
{
    if (m->ep0_pid >= 2) {
        return CDJ_USB_STALL;
    }
    if (m->ep0_pid == 1 && m->ccpl) {
        ctrl_complete(m);
        return CDJ_USB_ACK;
    }
    return nak(m);
}

CdjUsbResult cdj_usb_model_host_setup(CdjUsbModel *m, unsigned addr,
                                      const uint8_t setup[8])
{
    unsigned length = setup[6] | setup[7] << 8;
    unsigned value = setup[2] | setup[3] << 8;
    bool dir_in = setup[0] & 0x80;
    bool auto_reply = false;

    if (!addressed(m, addr)) {
        return CDJ_USB_NORESP;
    }
    if (m->dvsq >= 4) {
        return CDJ_USB_NORESP;              /* suspended */
    }
    m->setups++;

    memcpy(m->setup, setup, 8);
    ep_clear(m, 0);
    m->ep0_pid = 0;
    m->ccpl = false;
    m->valid = true;
    m->ep0_data_sent = m->ep0_data_acked = false;
    m->nrdy &= ~1u;
    m->emp &= ~1u;

    if (length == 0) {
        m->ctsq = 5;
    } else {
        m->ctsq = dir_in ? 1 : 3;
    }

    /* SET_ADDRESS and SET_CONFIGURATION may be answered by the chip itself. */
    if (setup[0] == 0x00 && setup[1] == 5 && m->dvsq == 1
        && (r(m, M66291_AUTO_RESPONSE) & 1)) {
        auto_reply = true;
    } else if (setup[0] == 0x00 && setup[1] == 9
               && (r(m, M66291_AUTO_RESPONSE) & 2)
               && ((m->dvsq == 2 && (value & 0xff))
                   || (m->dvsq == 3 && !(value & 0xff)))) {
        auto_reply = true;
    }
    if (auto_reply) {
        m->ep0_pid = 1;
        m->ccpl = true;
    }

    if (m->ctrt_hold) {
        /* A sequence error holds CTRT until it is cleared (data sheet fig 2.7). */
    } else if (!auto_reply) {
        ctrt_raise(m, 0);                   /* setup stage complete */
    }
    irq_update(m);
    return CDJ_USB_ACK;
}

/* One packet from a SIE side buffer to the host. */
static CdjUsbResult ep_send(CdjUsbModel *m, unsigned i, uint8_t *buf,
                            size_t max, size_t *len)
{
    Ep *ep = &m->ep[i];
    size_t n = ep->sie_len - ep->sie_pos;
    size_t mxps = ep_mxps(m, i);

    if (n > mxps) {
        n = mxps;
    }
    if (n > max) {
        n = max;
    }
    memcpy(buf, ep->sie + ep->sie_pos, n);
    ep->sie_pos += n;
    *len = n;
    if (ep->sie_pos >= ep->sie_len) {
        in_drained(m, i);
    }
    return CDJ_USB_ACK;
}

static CdjUsbResult ep0_in(CdjUsbModel *m, uint8_t *buf, size_t max, size_t *len)
{
    *len = 0;
    switch (m->ctsq) {
    case 1: {                               /* control read data stage */
        Ep *ep = &m->ep[0];
        CdjUsbResult res;

        if (m->ep0_pid >= 2) {
            m->nrdy |= 1;
            return CDJ_USB_STALL;
        }
        if (m->ep0_pid == 0) {
            return nak(m);
        }
        if (!ep->sie_full || !ep0_isel(m)) {
            return nak_not_ready(m, 0);
        }
        res = ep_send(m, 0, buf, max, len);
        m->ep0_data_sent = true;
        return res;
    }
    case 3:                                 /* control write: status begins */
        if (!m->ep0_data_acked) {
            return ctrl_seq_error(m);
        }
        m->ctsq = 4;
        ctrt_raise(m, 0x08);                /* WDST */
        irq_update(m);
        return ctrl_status(m);
    case 4:
    case 5:
        return ctrl_status(m);
    case 6:
        return CDJ_USB_STALL;
    case 2:
        return ctrl_seq_error(m);
    default:
        return nak(m);
    }
}

static CdjUsbResult ep0_out(CdjUsbModel *m, const uint8_t *data, size_t len)
{
    Ep *ep = &m->ep[0];

    switch (m->ctsq) {
    case 3: {                               /* control write data stage */
        if (m->ep0_pid >= 2) {
            return CDJ_USB_STALL;
        }
        if (len > ep_mxps(m, 0)) {
            m->emp |= 1;
            return ctrl_seq_error(m);
        }
        if (m->ep0_pid == 0 || ep->ival || ep0_isel(m)) {
            return m->ep0_pid ? nak_not_ready(m, 0) : nak(m);
        }
        memcpy(ep->sie, data, len);
        ep->sie_len = len;
        ep->sie_full = true;
        m->ep0_data_acked = true;
        out_toggle(m, 0);
        return CDJ_USB_ACK;
    }
    case 1:                                 /* control read: status begins */
        if (!m->ep0_data_sent || len) {
            return ctrl_seq_error(m);
        }
        ep->sie_full = false;
        ep->sie_len = ep->sie_pos = 0;
        m->ctsq = 2;
        ctrt_raise(m, 0x04);                /* RDST */
        irq_update(m);
        return ctrl_status(m);
    case 2:
        if (len) {
            return ctrl_seq_error(m);
        }
        return ctrl_status(m);
    case 4:
    case 5:
        return ctrl_seq_error(m);
    case 6:
        return CDJ_USB_STALL;
    default:
        return nak(m);
    }
}

CdjUsbResult cdj_usb_model_host_in(CdjUsbModel *m, unsigned addr, unsigned ep,
                                   uint8_t *buf, size_t max, size_t *len)
{
    CdjUsbResult res;

    *len = 0;
    if (!addressed(m, addr) || m->dvsq >= 4 || ep >= CDJ_USB_EPS) {
        return CDJ_USB_NORESP;
    }
    m->ins++;
    if (ep == 0) {
        res = ep0_in(m, buf, max, len);
        irq_update(m);
        return res;
    }
    if (!ep_typ(m, ep) || !ep_is_in(m, ep)) {
        return CDJ_USB_NORESP;
    }
    switch (ep_pid(m, ep)) {
    case 0:
        res = nak(m);
        break;
    case 1:
        if (m->ep[ep].sie_full) {
            res = ep_send(m, ep, buf, max, len);
        } else {
            res = nak_not_ready(m, ep);
        }
        break;
    default:
        m->nrdy |= 1u << ep;
        res = CDJ_USB_STALL;
        break;
    }
    irq_update(m);
    return res;
}

CdjUsbResult cdj_usb_model_host_out(CdjUsbModel *m, unsigned addr, unsigned ep,
                                    const uint8_t *data, size_t len)
{
    CdjUsbResult res;
    Ep *e;

    if (!addressed(m, addr) || m->dvsq >= 4 || ep >= CDJ_USB_EPS) {
        return CDJ_USB_NORESP;
    }
    m->outs++;
    if (ep == 0) {
        res = ep0_out(m, data, len);
        irq_update(m);
        return res;
    }
    if (!ep_typ(m, ep) || ep_is_in(m, ep)) {
        return CDJ_USB_NORESP;
    }
    e = &m->ep[ep];
    switch (ep_pid(m, ep)) {
    case 0:
        return nak(m);
    case 1:
        break;
    default:
        m->nrdy |= 1u << ep;
        irq_update(m);
        return CDJ_USB_STALL;
    }
    if (len > ep_mxps(m, ep) || e->sie_len + len > EP_BUF) {
        /* Size over: the endpoint stalls itself (data sheet 2.15). */
        ep_set_pid(m, ep, 2);
        m->emp |= 1u << ep;
        irq_update(m);
        return CDJ_USB_STALL;
    }
    if (e->sie_full || (!ep_dbl(m, ep) && e->ival)) {
        res = nak_not_ready(m, ep);
    } else if (r(m, M66291_EP_CONFIG1(ep)) & 0x800) {
        res = CDJ_USB_ACK;                  /* ACLR: acknowledged and dropped */
    } else {
        memcpy(e->sie + e->sie_len, data, len);
        e->sie_len += len;
        if (!ep_rwmd(m, ep) || len < ep_mxps(m, ep)
            || e->sie_len >= ep_bufsiz(m, ep)) {
            e->sie_full = true;
            out_toggle(m, ep);
        }
        res = CDJ_USB_ACK;
    }
    irq_update(m);
    return res;
}

void cdj_usb_model_status(const CdjUsbModel *m, CdjUsbModelStatus *s)
{
    uint16_t en = r(m, M66291_USB_ENABLE);

    memset(s, 0, sizeof(*s));
    s->vbus = m->vbus;
    s->attached = cdj_usb_model_attached(m);
    s->enabled = m->enabled;
    s->tron = en >> 8 & 3;
    s->dvsq = m->dvsq;
    s->ctsq = m->ctsq;
    s->address = m->address;
    s->irq[0] = m->irq[0];
    s->irq[1] = m->irq[1];
    for (unsigned i = 1; i < CDJ_USB_EPS; i++) {
        s->ep_type[i] = ep_typ(m, i);
        s->ep_dir[i] = ep_is_in(m, i);
        s->ep_mxps[i] = ep_mxps(m, i);
    }
    s->ep_mxps[0] = ep_mxps(m, 0);
    s->resets = m->resets;
    s->setups = m->setups;
    s->ins = m->ins;
    s->outs = m->outs;
    s->naks = m->naks;
}
