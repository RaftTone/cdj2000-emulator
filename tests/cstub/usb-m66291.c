/*
 * The M66291GP model, driven the way a driver drives it and spoken to the way a
 * host speaks to it, with no QEMU and no firmware.  Each check cites the part
 * of the Renesas data sheet (RJJ03F0074-0111Z Rev.1.11) that fixes the answer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 RaftTone
 */
#include "cdj2000_usb_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RD(o)    cdj_usb_model_read(m, (o))
#define WR(o, v) cdj_usb_model_write(m, (o), (v))
#define CLR(bits) ((uint16_t)~(bits))

static bool pin[2];

static void irq(void *opaque, unsigned which, bool level)
{
    (void)opaque;
    pin[which] = level;
}

/* A device descriptor to serve from "the driver". */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 1, 2, 3, 1,
};

/* The firmware's bring-up, in the order the header of cdj2000_usb.h gives. */
static CdjUsbModel *bring_up(void)
{
    CdjUsbModel *m = cdj_usb_model_new(false, false);

    cdj_usb_model_set_irq(m, irq, NULL);
    pin[0] = pin[1] = false;
    WR(0x00, 0);
    WR(0x00, 1);
    assert(RD(0x00) & 1);                       /* 0x2399c8 */
    WR(0x00, RD(0x00) | 0x8000);
    WR(0x00, RD(0x00) | 0x0d01);                /* SCKE USBPC TrON USBE */
    WR(0x0e, RD(0x0e) | 2);                     /* level output */
    WR(0x10, 0x80);
    WR(0x10, RD(0x10) | 0x8000);
    WR(0x10, RD(0x10) | 0x1000);
    WR(0x10, RD(0x10) | 0x080c);
    WR(0x10, RD(0x10) | 0x0700);
    WR(0x12, 0x01);                             /* BRDY: EP0 */
    WR(0x16, 0x03);                             /* BEMP: EP0, EP1 */
    WR(0x2c, 0);
    WR(0x2a, 64);
    return m;
}

static void test_absent_and_vbus(void)
{
    CdjUsbModel *m = cdj_usb_model_new(true, true);

    assert(RD(0x00) == 0 && RD(0x18) == 0);
    WR(0x00, 1);
    assert(!cdj_usb_model_enabled(m) && RD(0x00) == 0);

    m = cdj_usb_model_new(false, false);
    assert(!(RD(0x18) & 0x80));
    cdj_usb_model_set_vbus(m, true);
    assert(RD(0x18) & 0x80);
    m = cdj_usb_model_new(false, true);
    assert(RD(0x18) & 0x80);
    /* Plain registers read back what was written, as the bring-up needs. */
    WR(0x0e, 0x0002);
    WR(0x30, 0x0001);
    assert(RD(0x0e) == 2 && RD(0x30) == 1);
}

static void test_reset_values(void)
{
    CdjUsbModel *m = cdj_usb_model_new(false, false);

    assert(RD(0x2a) == 8);                      /* 2.21 */
    assert(RD(0x32) == 0x0800);                 /* 2.24: E0req before the clock */
    assert(RD(0x42) == 0x0800);                 /* 2.28 */
    assert(RD(0x62) == 0x0040 && RD(0x76) == 0x0040);   /* 2.40 */
}

static void test_vbus_interrupt(void)
{
    CdjUsbModel *m = bring_up();

    cdj_usb_model_set_vbus(m, true);
    assert(RD(0x18) & 0x8000);                  /* 2.12 (1): VBUS changed */
    assert(RD(0x18) & 0x0080);
    assert(pin[0]);                             /* VBSE is set */
    WR(0x18, CLR(0x8000));                      /* write 0 to clear */
    assert(!(RD(0x18) & 0x8000));
    assert(!pin[0]);
    cdj_usb_model_set_vbus(m, false);
    assert(pin[0] && !(RD(0x18) & 0x0080));
    WR(0x18, CLR(0x8000));
    assert(!pin[0]);
}

/* The data stage of a control read, as an interrupt routine would do it. */
static void drv_send(CdjUsbModel *m, const uint8_t *data, unsigned len)
{
    unsigned i;

    WR(0x30, 1);                                /* ISEL: control read */
    WR(0x32, 0x4000);                           /* EP0_PID = BUF */
    for (i = 0; i + 1 < len; i += 2) {
        WR(0x34, data[i] | data[i + 1] << 8);
    }
    if (len & 1) {
        assert(cdj_usb_model_write_byte(m, 0x34, data[len - 1]));
    }
    if (len % 64 != 0 || !len) {
        WR(0x32, 0x4000 | 0x2000);              /* IVAL: force the short packet */
    }
}

static void host_reset_and_address(CdjUsbModel *m, unsigned *addr, bool auto_resp)
{
    uint8_t setup_set_address[8] = { 0x00, 0x05, 0x07, 0, 0, 0, 0, 0 };
    uint8_t buf[8];
    size_t n;

    cdj_usb_model_set_vbus(m, true);
    WR(0x18, CLR(0x8000));
    assert(cdj_usb_model_attached(m));
    assert(cdj_usb_model_host_reset(m));
    assert((RD(0x18) >> 4 & 7) == 1);           /* Default */
    assert((RD(0x18) & 0x1000) && pin[0]);      /* DVST, URST enabled */
    WR(0x18, CLR(0x1000));
    assert(!pin[0]);

    /* SET_ADDRESS, optionally answered by the chip itself (2.22). */
    WR(0x2c, auto_resp ? 1 : 0);
    assert(cdj_usb_model_host_setup(m, 0, setup_set_address) == CDJ_USB_ACK);
    if (auto_resp) {
        assert(!(RD(0x18) & 0x0800));           /* no CTRT */
        assert((RD(0x18) & 7) == 5);
    } else {
        assert(RD(0x18) & 0x0800);              /* setup stage complete */
        assert(RD(0x20) == 0x0500 && RD(0x22) == 7);
        WR(0x18, CLR(0x0800 | 0x0008));         /* clear CTRT, VALID */
        assert(!(RD(0x18) & 8));
        WR(0x32, 0x4400);                       /* BUF + CCPL */
    }
    assert(cdj_usb_model_host_in(m, 0, 0, buf, 8, &n) == CDJ_USB_ACK && n == 0);
    assert(RD(0x08) == 7);
    assert((RD(0x18) >> 4 & 7) == 2);           /* Address */
    assert(!(RD(0x18) & 0x1000));               /* SADR not enabled: no DVST */
    *addr = 7;
}

static void test_enumeration(bool auto_resp)
{
    CdjUsbModel *m = bring_up();
    unsigned addr;
    uint8_t buf[64];
    size_t n;
    uint8_t get_dev[8] = { 0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0 };
    uint8_t get_dev8[8] = { 0x80, 0x06, 0x00, 0x01, 0, 0, 8, 0 };
    uint8_t set_cfg[8] = { 0x00, 0x09, 0x01, 0, 0, 0, 0, 0 };

    /* A device not yet reset is not on the bus (2.12 DVSQ = Powered). */
    cdj_usb_model_set_vbus(m, true);
    assert(cdj_usb_model_host_setup(m, 0, get_dev) == CDJ_USB_NORESP);

    /* Default state: GET_DESCRIPTOR(device, 8) at address 0, 8-byte packets. */
    WR(0x2a, 8);
    assert(cdj_usb_model_host_reset(m));
    WR(0x18, CLR(0x1000));
    assert(cdj_usb_model_host_setup(m, 0, get_dev8) == CDJ_USB_ACK);
    assert((RD(0x18) & 0x0800) && (RD(0x18) & 7) == 1);   /* read data stage */
    assert(RD(0x26) == 8 && RD(0x22) == 0x0100);
    /* Before the firmware answers, the host's IN is NAKed (2.14, fig 2.10). */
    assert(cdj_usb_model_host_in(m, 0, 0, buf, 8, &n) == CDJ_USB_NAK);
    WR(0x18, CLR(0x0800 | 0x0008));
    drv_send(m, dev_desc, 8);
    assert(cdj_usb_model_host_in(m, 0, 0, buf, 8, &n) == CDJ_USB_ACK && n == 8);
    assert(!memcmp(buf, dev_desc, 8));
    assert(RD(0x1e) & 1);                       /* BEMP: sent, nothing queued */
    WR(0x1e, CLR(1));
    /* Status stage: OUT zero-length; the chip raises RDST, the driver CCPL. */
    assert(cdj_usb_model_host_out(m, 0, 0, NULL, 0) == CDJ_USB_NAK);
    assert((RD(0x18) & 7) == 2 && (RD(0x18) & 0x0800));
    WR(0x18, CLR(0x0800));
    WR(0x32, 0x4400);
    assert(cdj_usb_model_host_out(m, 0, 0, NULL, 0) == CDJ_USB_ACK);
    assert((RD(0x18) & 7) == 0);

    host_reset_and_address(m, &addr, auto_resp);

    /* The full device descriptor now at address 7, with 64-byte packets. */
    WR(0x2a, 64);
    assert(cdj_usb_model_host_in(m, 3, 0, buf, 64, &n) == CDJ_USB_NORESP);
    assert(cdj_usb_model_host_setup(m, addr, get_dev) == CDJ_USB_ACK);
    WR(0x18, CLR(0x0800 | 0x0008));
    drv_send(m, dev_desc, 18);
    assert(cdj_usb_model_host_in(m, addr, 0, buf, 64, &n) == CDJ_USB_ACK);
    assert(n == 18 && !memcmp(buf, dev_desc, 18));
    assert(RD(0x1e) & 1);                       /* BEMP: EP0 buffer empty */
    WR(0x1e, CLR(1));
    assert(cdj_usb_model_host_out(m, addr, 0, NULL, 0) == CDJ_USB_NAK);
    WR(0x18, CLR(0x0800));
    WR(0x32, 0x4400);
    assert(cdj_usb_model_host_out(m, addr, 0, NULL, 0) == CDJ_USB_ACK);

    /* SET_CONFIGURATION(1) */
    WR(0x2c, auto_resp ? 3 : 0);
    assert(cdj_usb_model_host_setup(m, addr, set_cfg) == CDJ_USB_ACK);
    if (!auto_resp) {
        assert((RD(0x18) & 0x0800) && (RD(0x18) & 7) == 5);
        WR(0x18, CLR(0x0800 | 0x0008));
        WR(0x32, 0x4400);
    } else {
        assert(!(RD(0x18) & 0x0800));
    }
    assert(cdj_usb_model_host_in(m, addr, 0, buf, 64, &n) == CDJ_USB_ACK && !n);
    assert((RD(0x18) >> 4 & 7) == 3);           /* Configured */
}

static void test_control_write(void)
{
    CdjUsbModel *m = bring_up();
    uint8_t set_report[8] = { 0x21, 0x09, 0x00, 0x02, 0x00, 0, 4, 0 };
    uint8_t data[4] = { 1, 2, 3, 4 }, buf[8];
    size_t n;

    cdj_usb_model_set_vbus(m, true);
    cdj_usb_model_host_reset(m);
    assert(cdj_usb_model_host_setup(m, 0, set_report) == CDJ_USB_ACK);
    assert((RD(0x18) & 7) == 3);                /* write data stage */
    WR(0x18, CLR(0x0800 | 0x0008));
    WR(0x30, 0);                                /* ISEL = 0 */
    /* PID is still NAK: the data stage is refused until the driver says BUF. */
    assert(cdj_usb_model_host_out(m, 0, 0, data, 4) == CDJ_USB_NAK);
    WR(0x32, 0x4000);
    assert(cdj_usb_model_host_out(m, 0, 0, data, 4) == CDJ_USB_ACK);
    assert(RD(0x1a) & 1);                       /* BRDY EP0 */
    assert(pin[0]);
    assert((RD(0x32) & 0x01ff) == 4 && (RD(0x32) & 0x2000));
    assert(RD(0x34) == 0x0201);
    assert(RD(0x34) == 0x0403);
    assert(!(RD(0x32) & 0x2000));               /* all read: IVAL 0 */
    assert(!(RD(0x1a) & 1));                    /* RDYM 0: cleared with IVAL */
    /* Status stage: IN token moves to the write status stage. */
    assert(cdj_usb_model_host_in(m, 0, 0, buf, 8, &n) == CDJ_USB_NAK);
    assert((RD(0x18) & 7) == 4 && (RD(0x18) & 0x0800));
    WR(0x18, CLR(0x0800));
    WR(0x32, 0x4400);
    assert(cdj_usb_model_host_in(m, 0, 0, buf, 8, &n) == CDJ_USB_ACK && !n);
    assert((RD(0x18) & 7) == 0);
}

static void test_endpoints(void)
{
    CdjUsbModel *m = bring_up();
    unsigned addr;
    uint8_t buf[64], report[20], big[65] = { 0 };
    size_t n;
    unsigned k;

    host_reset_and_address(m, &addr, true);
    /* EP1: interrupt IN, 20 byte packets.  EP2: bulk OUT, 64, double buffered. */
    WR(0x60, 0x8000 | 0x2000 | 1);
    WR(0x62, 20);                               /* PID NAK while configuring */
    WR(0x64, 0x4000 | 0x0080 | 8);
    WR(0x66, 64);
    WR(0x12, 0x07);
    /* A cleared IN buffer is ready at once, with no token (2.13; the driver
     * enables BRDY and writes its first packet on that interrupt). */
    WR(0x62, 20 | 0x0800);
    WR(0x62, 20);
    assert(RD(0x1a) & 2);
    assert(pin[0]);
    for (k = 0; k < 20; k++) {
        report[k] = (uint8_t)(0xa0 + k);
    }

    /* Not yet armed: PID NAK, no NRDY (2.40 EPi_PID). */
    assert(cdj_usb_model_host_in(m, addr, 1, buf, 20, &n) == CDJ_USB_NAK);
    assert(!(RD(0x1c) & 2));
    WR(0x1a, 0);                                /* RDYM 0: a write does not clear it */
    assert(RD(0x1a) & 2);
    WR(0x62, 0x4000 | 20);                      /* BUF */
    assert(cdj_usb_model_host_in(m, addr, 1, buf, 20, &n) == CDJ_USB_NAK);
    assert(RD(0x1c) & 2);                       /* NRDY now */
    WR(0x1c, CLR(2));

    /* The driver writes the report through CPU_FIFO; MXPS bytes complete it. */
    WR(0x40, 1);
    assert(!(RD(0x42) & 0x0800));               /* Creq 0 */
    for (k = 0; k < 20; k += 2) {
        WR(0x44, report[k] | report[k + 1] << 8);
    }
    assert(RD(0x42) & 0x2000);                  /* IVAL: write complete */
    assert(cdj_usb_model_host_in(m, addr, 1, buf, 64, &n) == CDJ_USB_ACK);
    assert(n == 20 && !memcmp(buf, report, 20));
    assert(!(RD(0x42) & 0x2000));
    assert(RD(0x1a) & 2);                       /* BRDY: writable again */
    assert(RD(0x1e) & 2);                       /* BEMP: sent, nothing queued */
    assert(pin[0]);
    WR(0x1e, CLR(2));
    assert(cdj_usb_model_host_in(m, addr, 1, buf, 64, &n) == CDJ_USB_NAK);

    /* A forced short packet and a zero-length packet. */
    WR(0x40, 1);
    cdj_usb_model_write_byte(m, 0x44, 0x55);
    WR(0x42, 0x2000);
    assert(cdj_usb_model_host_in(m, addr, 1, buf, 64, &n) == CDJ_USB_ACK && n == 1);
    WR(0x42, 0x2000 | 0x1000);
    assert(cdj_usb_model_host_in(m, addr, 1, buf, 64, &n) == CDJ_USB_ACK && !n);

    /* EP2: bulk OUT.  PID BUF, 64 bytes, then a 5 byte packet. */
    WR(0x66, 0x4000 | 64);
    for (k = 0; k < 64; k++) {
        buf[k] = (uint8_t)k;
    }
    assert(cdj_usb_model_host_out(m, addr, 2, buf, 64) == CDJ_USB_ACK);
    assert(RD(0x1a) & 4);
    WR(0x40, 0x0002);
    assert((RD(0x42) & 0x7ff) == 64 && (RD(0x42) & 0x2000));
    /* Double buffered: a second packet is taken while the first is unread. */
    assert(cdj_usb_model_host_out(m, addr, 2, buf, 5) == CDJ_USB_ACK);
    assert(cdj_usb_model_host_out(m, addr, 2, buf, 5) == CDJ_USB_NAK);
    assert(RD(0x1c) & 4);
    for (k = 0; k < 64; k += 2) {
        assert(RD(0x44) == (uint16_t)(k | (k + 1) << 8));
    }
    assert((RD(0x42) & 0x7ff) == 5 && (RD(0x42) & 0x2000));   /* toggled */
    for (k = 0; k < 3; k++) {
        (void)RD(0x44);
    }
    assert(!(RD(0x42) & 0x2000));
    /* Size over: the endpoint stalls itself (2.15, 2.40). */
    assert(cdj_usb_model_host_out(m, addr, 2, big, 65) == CDJ_USB_STALL);
    assert((RD(0x1e) & 4) && (RD(0x66) >> 14) == 2);
    /* An endpoint that was never configured does not answer. */
    assert(cdj_usb_model_host_in(m, addr, 5, buf, 64, &n) == CDJ_USB_NORESP);
}

static void test_unplug(void)
{
    CdjUsbModel *m = bring_up();
    unsigned addr;
    uint8_t buf[8];
    size_t n;

    host_reset_and_address(m, &addr, true);
    cdj_usb_model_set_vbus(m, false);
    assert((RD(0x18) >> 4 & 7) == 0 && RD(0x08) == 0);
    assert(cdj_usb_model_host_in(m, addr, 0, buf, 8, &n) == CDJ_USB_NORESP);
    /* USBE off: software reset clears the enables (2.1, fig 2.1). */
    WR(0x00, 0);
    assert(RD(0x10) == 0 && !cdj_usb_model_enabled(m));
}

int main(void)
{
    test_absent_and_vbus();
    test_reset_values();
    test_vbus_interrupt();
    test_enumeration(true);
    test_enumeration(false);
    test_control_write();
    test_endpoints();
    test_unplug();
    puts("usb-m66291: ok");
    return 0;
}
