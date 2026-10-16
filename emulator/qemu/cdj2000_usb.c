/*
 * Pioneer CDJ-2000 USB controller -- the bus surface.
 *
 * MAIN reports `E-7020: USB-B DEVICE ERROR` because the USB function task
 * cannot bring its controller up, and it cannot bring it up because the chip is
 * not on the bus.  cdj2000_usb.h has the whole derivation: the device-status
 * array at 0x04c0875c, the switch in 0x24fd20 that turns device 3 into caution
 * 61, and the nine-instruction enable-and-poll at 0x2399c8 that fails.
 *
 * This file is the window.  The controller is a 16-bit register file at
 * physical 0x01000000 -- the firmware builds that address four separate times
 * with the same `#-95 << 8 << 16`, so it is not inferred -- and every register
 * access is 16 bits wide.  What the registers answer is
 * cdj2000_usb_model.c's business.  The window, the trace and the "absent"
 * control arm are LycheeAPPF's.
 *
 * Added by RaftTone: the chip's INT0/INT1 pins, and a *host* on the other side
 * of the cable.  CDJ_USBB_HOST=<port> makes the emulator listen on UDP
 * 127.0.0.1:<port>, and a program there plays the computer plugged into the
 * type-B socket: it attaches and resets the bus and then speaks one USB
 * transaction per datagram (SETUP, IN, OUT), which the model carries out against
 * the chip exactly as a bus would.  tools/cdj_main/usbb_host.py is such a host;
 * it enumerates the deck and reads its HID reports.
 *
 * The idea of a loopback UDP host on the rear port, and the datagram-per-packet
 * style, follow Stijn Jacobs' CDJ-2000NXS model (usbf_host.c, CDJ_USBF_HOST)
 * and P2GR's HID relay on it (cdj-nxs2-qemu PR #15).  The framing here is
 * different, because the host is outside the emulator and the model is the
 * device: a request carries a token and a reply carries the chip's answer to it.
 *
 *   request  byte 0   op: 0 status, 1 attach, 2 detach, 3 bus reset, 4 SETUP,
 *                     5 IN, 6 OUT, 7 suspend, 8 resume
 *            byte 1   sequence number, echoed
 *            byte 2   device address
 *            byte 3   endpoint number
 *            bytes 4-5  payload length (IN: the most the host will take),
 *                     little-endian
 *            then the payload (SETUP: the 8 byte request; OUT: the data)
 *   reply    byte 0   op | 0x80
 *            byte 1   the sequence number
 *            byte 2   0 ACK, 1 NAK, 2 STALL, 3 no response, 4 malformed
 *            byte 3   0
 *            bytes 4-5  payload length
 *            then the payload (IN: the packet; status: see below)
 *
 * The status payload is 56 bytes, little-endian: vbus, attached, enabled,
 * TrON, DVSQ, CTSQ, address, pins (bit 0 INT0, bit 1 INT1); then the counters
 * resets, setups, ins, outs, naks as 32-bit words; then for EP0..EP6 four bytes
 * each: type, direction (1 = IN), maximum packet size (16-bit).
 *
 * Copyright (C) 2026 LycheeAPPF
 * Copyright (C) 2026 RaftTone
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "system/memory.h"

#include "cdj2000_usb.h"

#define HOST_MAX_PAYLOAD 1100

typedef struct CdjUsbState {
    MemoryRegion iomem;
    CdjUsbModel *model;
    bool trace;
    bool announced;             /* the "module came up" line is worth once */
    qemu_irq int0;
    CdjUsbLevelFn level;
    void *level_opaque;
    int host_fd;                /* the UDP socket, or -1 */
    bool host_trace;
    bool host_trace_all;        /* CDJ_USBB_TRACE=all: NAKed polls too */
} CdjUsbState;

static void cdj_usb_pin(void *opaque, unsigned pin, bool asserted)
{
    CdjUsbState *usb = opaque;

    if (usb->trace) {
        qemu_log("cdj2000-usb: INT%u %s\n", pin, asserted ? "asserted" : "released");
    }
    if (pin != 0) {
        return;                 /* INT1 goes nowhere on this board */
    }
    if (usb->level) {
        usb->level(usb->level_opaque, asserted);
    }
    if (usb->int0) {
        qemu_set_irq(usb->int0, asserted);
    }
}

static uint64_t cdj_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjUsbState *usb = opaque;
    uint16_t value = cdj_usb_model_read(usb->model, offset);

    if (size == 1) {
        value = (offset & 1) ? value >> 8 : value & 0xff;
    }
    if (usb->trace) {
        qemu_log("cdj2000-usb: read  %#04" HWADDR_PRIx " (%u) = %#06x\n",
                 offset, size, value);
    }
    return value;
}

static void cdj_usb_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    CdjUsbState *usb = opaque;

    if (size == 1) {
        /*
         * The chip recognises a byte write to a FIFO data register on its
         * 16-bit bus (data sheet 2.29).  Any other byte access is merged into
         * the register.
         */
        if (!cdj_usb_model_write_byte(usb->model, offset, (uint8_t)value)) {
            uint16_t word = cdj_usb_model_read(usb->model, offset & ~1ull);

            if (offset & 1) {
                word = (word & 0x00ff) | (uint16_t)(value << 8);
            } else {
                word = (word & 0xff00) | (uint8_t)value;
            }
            cdj_usb_model_write(usb->model, offset & ~1ull, word);
        }
    } else {
        cdj_usb_model_write(usb->model, offset, (uint16_t)value);
    }

    if (usb->trace) {
        qemu_log("cdj2000-usb: write %#04" HWADDR_PRIx " (%u) = %#06x\n",
                 offset, size, (uint16_t)value);
    }
    if (!usb->announced && cdj_usb_model_enabled(usb->model)) {
        usb->announced = true;
        qemu_log("cdj2000-usb: the module is enabled; 0x2399c8 will see bit 0\n");
    }
}

/*
 * Mostly 16 bits.  Every instruction in the image that touches the registers
 * is a `mov.w`; the FIFO data registers also take a byte, which is how an odd
 * length reaches the chip.  A 32-bit access would be a bug worth seeing rather
 * than something to accommodate silently.
 */
static const MemoryRegionOps cdj_usb_ops = {
    .read = cdj_usb_read,
    .write = cdj_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

/* ----------------------------------------------------------------- the host */

static void cdj_usb_host_reply(CdjUsbState *usb, const struct sockaddr_in *to,
                               socklen_t to_len, uint8_t op, uint8_t seq,
                               uint8_t result, const uint8_t *payload,
                               size_t len)
{
    uint8_t out[6 + HOST_MAX_PAYLOAD];

    out[0] = op | 0x80;
    out[1] = seq;
    out[2] = result;
    out[3] = 0;
    stw_le_p(out + 4, len);
    if (len) {
        memcpy(out + 6, payload, len);
    }
    if (sendto(usb->host_fd, (void *)out, 6 + len, 0,
               (const struct sockaddr *)to, to_len) < 0 && usb->host_trace) {
        qemu_log("cdj2000-usb: host reply failed: %s\n", strerror(errno));
    }
}

static size_t cdj_usb_host_status(const CdjUsbModel *model, uint8_t *out)
{
    CdjUsbModelStatus s;
    size_t n = 0;

    cdj_usb_model_status(model, &s);
    out[n++] = s.vbus;
    out[n++] = s.attached;
    out[n++] = s.enabled;
    out[n++] = s.tron;
    out[n++] = s.dvsq;
    out[n++] = s.ctsq;
    out[n++] = s.address;
    out[n++] = (s.irq[0] ? 1 : 0) | (s.irq[1] ? 2 : 0);
    stl_le_p(out + n, s.resets);  n += 4;
    stl_le_p(out + n, s.setups);  n += 4;
    stl_le_p(out + n, s.ins);     n += 4;
    stl_le_p(out + n, s.outs);    n += 4;
    stl_le_p(out + n, s.naks);    n += 4;
    for (unsigned i = 0; i < CDJ_USB_EPS; i++) {
        out[n++] = s.ep_type[i];
        out[n++] = s.ep_dir[i];
        stw_le_p(out + n, s.ep_mxps[i]);
        n += 2;
    }
    return n;
}

static void cdj_usb_host_read(void *opaque)
{
    CdjUsbState *usb = opaque;
    CdjUsbModel *model = usb->model;
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    uint8_t in[6 + HOST_MAX_PAYLOAD];
    uint8_t out[HOST_MAX_PAYLOAD];
    ssize_t got = recvfrom(usb->host_fd, (void *)in, sizeof(in), 0,
                           (struct sockaddr *)&from, &from_len);
    uint8_t op, seq, addr, ep;
    unsigned len;
    size_t out_len = 0;
    CdjUsbResult result = CDJ_USB_ACK;

    if (got < 6) {
        return;
    }
    op = in[0];
    seq = in[1];
    addr = in[2];
    ep = in[3];
    len = lduw_le_p(in + 4);
    if (len > HOST_MAX_PAYLOAD || (op != 5 && 6 + len > (size_t)got)) {
        cdj_usb_host_reply(usb, &from, from_len, op, seq, 4, NULL, 0);
        return;
    }

    switch (op) {
    case 0:
        out_len = cdj_usb_host_status(model, out);
        break;
    case 1:
        cdj_usb_model_set_vbus(model, true);
        break;
    case 2:
        cdj_usb_model_set_vbus(model, false);
        break;
    case 3:
        result = cdj_usb_model_host_reset(model) ? CDJ_USB_ACK : CDJ_USB_NORESP;
        break;
    case 4:
        result = len == 8
            ? cdj_usb_model_host_setup(model, addr, in + 6) : CDJ_USB_BADREQ;
        break;
    case 5: {
        size_t n = 0;

        result = cdj_usb_model_host_in(model, addr, ep, out, len, &n);
        out_len = n;
        break;
    }
    case 6:
        result = cdj_usb_model_host_out(model, addr, ep, in + 6, len);
        break;
    case 7:
        result = cdj_usb_model_host_suspend(model) ? CDJ_USB_ACK : CDJ_USB_NORESP;
        break;
    case 8:
        result = cdj_usb_model_host_resume(model) ? CDJ_USB_ACK : CDJ_USB_NORESP;
        break;
    default:
        result = CDJ_USB_BADREQ;
        break;
    }
    if (usb->host_trace && op >= 3
        && (usb->host_trace_all || result != CDJ_USB_NAK)) {
        qemu_log("cdj2000-usb: host op %u addr %u ep %u len %u -> %d (%zu)\n",
                 op, addr, ep, len, (int)result, out_len);
    }
    cdj_usb_host_reply(usb, &from, from_len, op, seq, result, out, out_len);
}

static void cdj_usb_host_open(CdjUsbState *usb, const char *spec)
{
    struct sockaddr_in address = { .sin_family = AF_INET };
    char *end = NULL;
    unsigned long port = strtoul(spec, &end, 10);
    int fd;

    if (!*spec || *end || port == 0 || port > 65535) {
        warn_report("cdj2000-usb: CDJ_USBB_HOST=%s is not a UDP port", spec);
        return;
    }
    fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        warn_report("cdj2000-usb: socket: %s", strerror(errno));
        return;
    }
    socket_set_fast_reuse(fd);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        warn_report("cdj2000-usb: cannot listen on udp 127.0.0.1:%lu: %s",
                    port, strerror(errno));
        close(fd);
        return;
    }
    qemu_set_blocking(fd, false, NULL);
    usb->host_fd = fd;
    qemu_set_fd_handler(fd, cdj_usb_host_read, NULL, usb);
    info_report("cdj2000-usb: a host may be plugged into the type-B socket "
                "through udp 127.0.0.1:%lu", port);
}

void cdj_usb_init(MemoryRegion *system, qemu_irq int0, CdjUsbLevelFn level,
                  void *opaque)
{
    CdjUsbState *usb = g_new0(CdjUsbState, 1);
    bool absent = getenv("CDJ_USB_ABSENT") != NULL;
    bool vbus = getenv("CDJ_USB_VBUS") != NULL;
    const char *host = getenv("CDJ_USBB_HOST");

    usb->trace = getenv("CDJ_USB_TRACE") != NULL;
    usb->host_trace = getenv("CDJ_USBB_TRACE") != NULL;
    usb->host_trace_all = usb->host_trace && !strcmp(getenv("CDJ_USBB_TRACE"), "all");
    usb->host_fd = -1;
    usb->int0 = int0;
    usb->level = level;
    usb->level_opaque = opaque;
    usb->model = cdj_usb_model_new(absent, vbus);
    cdj_usb_model_set_irq(usb->model, cdj_usb_pin, usb);

    memory_region_init_io(&usb->iomem, NULL, &cdj_usb_ops, usb,
                          "cdj2000.usb", CDJ_USB_SIZE);
    memory_region_add_subregion(system, CDJ_USB_BASE, &usb->iomem);

    if (absent) {
        qemu_log("cdj2000-usb: CDJ_USB_ABSENT -- the window answers zero, "
                 "which is the machine before this device existed\n");
    } else if (host && *host) {
        cdj_usb_host_open(usb, host);
    }
}
