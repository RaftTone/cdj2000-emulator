/*
 * Pioneer CDJ-2000 USB controller -- the chip model's interface.
 *
 * The chip on the type-B socket is a Renesas M66291GP, a Full Speed USB device
 * controller with a 16-bit register file, 3 KiB of FIFO and EP0 plus up to six
 * more endpoints.  This header is the whole of the model's surface and it does
 * not depend on QEMU, so the model can be exercised on its own
 * (tests/cstub/usb-m66291.c) as well as behind the bus window in
 * cdj2000_usb.c.
 *
 * Authorship:
 *   - The register window, the VBUS bit, the enable-and-poll that decides
 *     E-7020, and the whole derivation from the firmware behind them are
 *     LycheeAPPF's (cdj2000_usb.h, cdj2000_usb.c and the original
 *     cdj2000_usb_model.c).  Nothing of that is changed in meaning here.
 *   - The register behaviour, the endpoint buffers, the EP0 control transfer
 *     machine and the host-side transaction interface below are RaftTone's,
 *     written from the Renesas M66291GP/HP data sheet (RJJ03F0074-0111Z,
 *     Rev.1.11).
 *
 * Copyright (C) 2026 LycheeAPPF
 * Copyright (C) 2026 RaftTone
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CDJ2000_USB_MODEL_H
#define CDJ2000_USB_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The register window.  The base is not inferred: 0x2399c8, 0x2399bc, 0x2397cc
 * and 0x2df2d4 all build 0xA1000000 with the same `#-95 << 8 << 16`, and P2
 * strips to physical 0x01000000.  The highest offset any code in the image
 * touches is 0x60 (0x23987x and the usbh block at 0x238000..0x240000); a page is
 * mapped so that a stray access lands on the device and is logged rather than
 * disappearing into unassigned memory.
 */
#define CDJ_USB_BASE          0x01000000
#define CDJ_USB_SIZE          0x1000

/* The offsets the firmware proves.  Everything else is a plain register. */
#define CDJ_USB_ENABLE        0x00   /* bit 0 is what 0x2399c8 polls */
#define CDJ_USB_ENABLE_BIT    0x0001
#define CDJ_USB_STATUS        0x18   /* bit 7 = VBUS, read at 0x2deb4e */
#define CDJ_USB_VBUS_BIT      0x0080

/* The rest of the M66291GP register map (data sheet figures 2.1 and 2.2). */
#define M66291_USB_ENABLE     0x00
#define M66291_REMOTE_WAKEUP  0x02
#define M66291_SEQUENCE_BIT   0x04
#define M66291_USB_ADDRESS    0x08
#define M66291_ISO_STATUS     0x0a
#define M66291_SOF_CNT        0x0c
#define M66291_POLARITY       0x0e
#define M66291_INTENB0        0x10
#define M66291_INTENB1        0x12
#define M66291_INTENB2        0x14
#define M66291_INTENB3        0x16
#define M66291_INTSTS0        0x18
#define M66291_INTSTS1        0x1a
#define M66291_INTSTS2        0x1c
#define M66291_INTSTS3        0x1e
#define M66291_REQUEST        0x20
#define M66291_VALUE          0x22
#define M66291_INDEX          0x24
#define M66291_LENGTH         0x26
#define M66291_CONTROL        0x28
#define M66291_EP0_SIZE       0x2a
#define M66291_AUTO_RESPONSE  0x2c
#define M66291_EP0_SELECT     0x30
#define M66291_EP0_CONTROL    0x32
#define M66291_EP0_DATA       0x34
#define M66291_EP0_SENDLEN    0x36
#define M66291_CPU_SELECT     0x40
#define M66291_CPU_CONTROL    0x42
#define M66291_CPU_DATA       0x44
#define M66291_SIE_STATUS     0x46
#define M66291_D0_SELECT      0x48
#define M66291_D0_CONTROL     0x4a
#define M66291_D0_DATA        0x4c
#define M66291_D0_COUNT       0x4e
#define M66291_D1_SELECT      0x50
#define M66291_D1_CONTROL     0x52
#define M66291_D1_DATA        0x54
#define M66291_D1_COUNT       0x56
#define M66291_FIFO_STATUS    0x58
#define M66291_PORT_CONTROL   0x5a
#define M66291_PORT_DATA      0x5c
#define M66291_DRIVE          0x5e
#define M66291_EP_CONFIG0(i)  (0x60 + ((i) - 1) * 4)    /* i = 1..6 */
#define M66291_EP_CONFIG1(i)  (0x62 + ((i) - 1) * 4)

/* INTSTS0 / INTENB0 bits. */
#define M66291_VBUS   0x8000
#define M66291_RESM   0x4000
#define M66291_SOFR   0x2000
#define M66291_DVST   0x1000
#define M66291_CTRT   0x0800
#define M66291_BEMP   0x0400
#define M66291_INTN   0x0200
#define M66291_INTR   0x0100
#define M66291_VBUS_PIN 0x0080
#define M66291_VALID  0x0008

/* USB_ENABLE bits. */
#define M66291_USBE   0x0001
#define M66291_SCKE   0x0800
#define M66291_USBPC  0x0400
#define M66291_TRON   0x0300

/* The seven endpoints, EP0..EP6. */
#define CDJ_USB_EPS   7

typedef struct CdjUsbModel CdjUsbModel;

/* What the chip puts on the bus in answer to a token. */
typedef enum {
    CDJ_USB_ACK = 0,        /* data (or a zero-length packet) was taken or given */
    CDJ_USB_NAK = 1,        /* not ready: the host tries again */
    CDJ_USB_STALL = 2,
    CDJ_USB_NORESP = 3,     /* nothing answers: not powered, not attached, or
                             * the address is not the chip's */
    CDJ_USB_BADREQ = 4,     /* host interface only: a malformed request */
} CdjUsbResult;

/*
 * The chip's two interrupt pins.  POLARITY_CNT assigns each source to INT0 or
 * INT1; the firmware leaves everything on INT0.  `asserted` is true while the
 * pin is requesting service.
 */
typedef void (*CdjUsbIrqFn)(void *opaque, unsigned pin, bool asserted);

/*
 * Create a model.  `absent` keeps the window mapped but makes it answer like
 * unmapped memory -- reads 0, writes dropped -- which is the machine exactly as
 * it was before this device existed, and therefore the control an A/B needs.
 */
CdjUsbModel *cdj_usb_model_new(bool absent, bool vbus);

/* Back to hardware reset: the module is off and nothing has been configured. */
void cdj_usb_model_reset(CdjUsbModel *model);

/* Where the pins go.  Called with a pin's level whenever it changes. */
void cdj_usb_model_set_irq(CdjUsbModel *model, CdjUsbIrqFn fn, void *opaque);

uint16_t cdj_usb_model_read(CdjUsbModel *model, uint32_t offset);
void cdj_usb_model_write(CdjUsbModel *model, uint32_t offset, uint16_t value);

/*
 * A byte access to one of the three FIFO data registers, which the chip
 * recognises on a 16-bit bus (data sheet 2.29).  Returns false when `offset`
 * is not a data register, and the caller then falls back to a word access.
 */
bool cdj_usb_model_write_byte(CdjUsbModel *model, uint32_t offset,
                              uint8_t value);

/* The same for a byte read: one byte leaves the FIFO, not a word's two. */
bool cdj_usb_model_read_byte(CdjUsbModel *model, uint32_t offset,
                             uint8_t *value);

/* True once the guest has enabled the module, i.e. once bit 0 of 0x00 is set. */
bool cdj_usb_model_enabled(const CdjUsbModel *model);

/* ------------------------------------------------------------ the host side */

/* The cable.  VBUS changes raise the chip's VBUS interrupt. */
void cdj_usb_model_set_vbus(CdjUsbModel *model, bool vbus);
bool cdj_usb_model_vbus(const CdjUsbModel *model);

/*
 * True while the chip drives the bus: VBUS is present, the module is out of
 * software reset and has its clocks, and the transceiver and TrON are on.  A
 * host that is not told this has nothing to talk to.
 */
bool cdj_usb_model_attached(const CdjUsbModel *model);

/* USB bus reset (SE0 for more than 2.5 us); the chip becomes Default. */
bool cdj_usb_model_host_reset(CdjUsbModel *model);
bool cdj_usb_model_host_suspend(CdjUsbModel *model);
bool cdj_usb_model_host_resume(CdjUsbModel *model);

/*
 * One transaction each.  `addr` is the device address the token carries, and a
 * token for any address other than the one the chip holds is not answered.
 * IN returns the packet in `buf` (at most `max` bytes, and never more than the
 * endpoint's maximum packet size) with its length in `*len`.
 */
CdjUsbResult cdj_usb_model_host_setup(CdjUsbModel *model, unsigned addr,
                                      const uint8_t setup[8]);
CdjUsbResult cdj_usb_model_host_in(CdjUsbModel *model, unsigned addr,
                                   unsigned ep, uint8_t *buf, size_t max,
                                   size_t *len);
CdjUsbResult cdj_usb_model_host_out(CdjUsbModel *model, unsigned addr,
                                    unsigned ep, const uint8_t *data,
                                    size_t len);

/* What the host protocol reports about the chip. */
typedef struct {
    bool vbus, attached, enabled;
    uint8_t tron;           /* USB_ENABLE bits 9:8 */
    uint8_t dvsq;           /* 0 powered, 1 default, 2 address, 3 configured, +4 suspended */
    uint8_t ctsq;           /* control transfer stage, INTSTS0 bits 2:0 */
    uint8_t address;
    bool irq[2];            /* INT0, INT1 asserted */
    uint8_t ep_type[CDJ_USB_EPS];   /* EPi_TYP */
    uint8_t ep_dir[CDJ_USB_EPS];    /* 1 = IN */
    uint16_t ep_mxps[CDJ_USB_EPS];
    uint32_t resets, setups, ins, outs, naks;
} CdjUsbModelStatus;

void cdj_usb_model_status(const CdjUsbModel *model, CdjUsbModelStatus *status);

#endif /* CDJ2000_USB_MODEL_H */
