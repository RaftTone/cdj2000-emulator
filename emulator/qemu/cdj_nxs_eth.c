/* SPDX-License-Identifier: GPL-2.0-or-later
 * SH7764 EtherC/E-DMAC + RTL8201FL (RRV4356 p98 IC704).
 * Functional, bounded DMA; no analog PHY, wire-time or PTP-lock shortcut.
 * Only guest SDRAM may be addressed by DMA. No host-memory passthrough.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "cdj_nxs_eth.h"
#include "cdj_sh7764_eth.h"
#include "cdj_rtl8201fl.h"

#define TYPE_NXS_ETH "cdj-nxs-ethernet"
#define ETH_MASK (1u << 16)
OBJECT_DECLARE_SIMPLE_TYPE(NxsEth, NXS_ETH)
struct NxsEth {
    SysBusDevice parent_obj;
    MemoryRegion io, alias, masks, masks_alias, status, status_alias;
    NICConf conf;
    NICState *nic;
    qemu_irq irq;
    QEMUTimer *timer;
    CdjSh7764Eth core;
    CdjRtl8201fl phy;
    uint32_t mask;
    uint64_t tx, rx;
    bool peer_present, last_link;
};

static void fatal(NxsEth *s, const char *operation, uint32_t offset)
{
    error_report("nxs-ethernet: %s offset=%#x: %s", operation, offset,
                 s->core.error ? s->core.error : "unsupported PHY/access");
    exit(1);
}
static void update_irq(NxsEth *s)
{
    qemu_set_irq(s->irq, !(s->mask & ETH_MASK) && cdj_sh7764_eth_irq(&s->core));
}
static bool ram_range(uint32_t addr, size_t len)
{
    return addr >= 0x04000000 && addr < 0x0c000000 && len <= 0x0c000000 - addr;
}
static bool dma_read(void *opaque, uint32_t addr, void *data, size_t len)
{
    return ram_range(addr, len) && address_space_read(&address_space_memory,
            addr, MEMTXATTRS_UNSPECIFIED, data, len) == MEMTX_OK;
}
static bool dma_write(void *opaque, uint32_t addr, const void *data, size_t len)
{
    return ram_range(addr, len) && address_space_write(&address_space_memory,
            addr, MEMTXATTRS_UNSPECIFIED, data, len) == MEMTX_OK;
}
/* Descriptor fields are bus longwords, independent of EDMR.DE. SH7764 in
 * this board's little-endian CPU mode stores these numeric words LE in RAM.
 * Stock 04217cf4/04217d10 stores 0x30000000 directly, without a byte swap. */
static bool desc_read(void *opaque, uint32_t addr, uint32_t words[3])
{
    uint8_t bytes[12];
    if (!dma_read(opaque, addr, bytes, sizeof bytes)) return false;
    for (unsigned i = 0; i < 3; i++) words[i] = ldl_le_p(bytes + i * 4);
    return true;
}
static bool desc_write(void *opaque, uint32_t addr, const uint32_t *words, size_t count)
{
    uint8_t bytes[12];
    if (count > 3) return false;
    for (size_t i = 0; i < count; i++) stl_le_p(bytes + i * 4, words[i]);
    return dma_write(opaque, addr, bytes, count * 4);
}
static bool send_frame(void *opaque, const uint8_t *data, size_t len)
{
    NxsEth *s = opaque;
    NetClientState *nc = qemu_get_queue(s->nic);
    if (!nc->peer || nc->link_down || nc->peer->link_down) return false;
    ssize_t result = qemu_send_packet(nc, data, len);
    if (result < 0) return false;
    s->tx++;
    qemu_log("nxs-ethernet: tx=%" PRIu64 " bytes=%zu virtual_ns=%" PRId64 "\n",
             s->tx, len, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return true;
}
static const CdjSh7764EthOps dma_ops = {
    .dma_read = dma_read, .dma_write = dma_write, .send = send_frame,
    .desc_read = desc_read, .desc_write = desc_write,
};
static void synchronize(NxsEth *s)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    NetClientState *nc = qemu_get_queue(s->nic);
    bool present = nc->peer && !nc->link_down && !nc->peer->link_down;
    if (present != s->peer_present) {
        /* Explicit virtual cable partner: 100Base-TX full duplex, no EEE.
         * This is the local Ethernet endpoint's capability, not Dante state. */
        if (!cdj_rtl8201fl_set_peer(&s->phy, present, 0x0101, now))
            fatal(s, "peer transition", 0x120);
        s->peer_present = present;
    }
    if (!cdj_rtl8201fl_advance(&s->phy, now)) fatal(s, "PHY time", 0x120);
    bool link = cdj_rtl8201fl_link(&s->phy);
    bool lmon;
    /* RRV4356 p98 routes LED0 (pin34), not INTB, to CPU_LNKSTA.
     * Firmware selects custom Link10/Link100 via page7 registers17/19. */
    if (!cdj_rtl8201fl_led0(&s->phy, &lmon)) fatal(s, "LED0/LNKSTA mode", 0x128);
    cdj_sh7764_eth_set_link(&s->core, link, lmon);
    if (link != s->last_link) {
        qemu_log("nxs-ethernet: link=%d virtual_ns=%" PRIu64 "\n", link, now);
        s->last_link = link;
    }
    update_irq(s);
    /* Socket backend disconnects have no NIC callback in this QEMU version.
     * Poll its cable state every 10ms virtual time; not PHY/wire timing. */
    uint64_t next = MIN(now + 10000000, cdj_rtl8201fl_deadline(&s->phy));
    timer_mod(s->timer, next);
}
static void tick(void *opaque) { synchronize(opaque); }
static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    NxsEth *s = opaque; uint32_t value;
    synchronize(s);
    if (size != 4 || (off & 3)) fatal(s, "read width/alignment", off);
    if (off == 0x120) return cdj_rtl8201fl_pir_read(&s->phy);
    if (!cdj_sh7764_eth_read(&s->core, off, &value)) fatal(s, "read", off);
    return value;
}
static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    NxsEth *s = opaque;
    synchronize(s);
    if (size != 4 || (off & 3)) fatal(s, "write width/alignment", off);
    if (off == 0x120) {
        if (!cdj_rtl8201fl_pir_write(&s->phy, value)) {
            error_report("nxs-ethernet: MDIO header=%#x bits=%u data=%#x page=%u bmcr=%#x pir=%#" PRIx64,
                         s->phy.header, s->phy.bits, s->phy.data, s->phy.page,
                         s->phy.bmcr, value);
            fatal(s, "MDIO write", off);
        }
    } else {
        qemu_log("nxs-ethernet: write offset=%#" HWADDR_PRIx " value=%#" PRIx64 "\n", off, value);
        if (!cdj_sh7764_eth_write(&s->core, off, value)) fatal(s, "write", off);
        /* EDMR.SWR resets EtherC's PIR output latch, not external IC704. */
        if (off == 0 && value == 1 && !cdj_rtl8201fl_pir_write(&s->phy, 0))
            fatal(s, "PIR software reset", off);
    }
    synchronize(s);
}
static const MemoryRegionOps eth_ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1, .impl.max_access_size = 4,
};
static uint64_t mask_read(void *opaque, hwaddr off, unsigned size)
{
    NxsEth *s = opaque;
    if (size != 4 || (off != 0 && off != 4)) fatal(s, "INTC mask read", off);
    return off == 0 ? s->mask : 0;
}
static void mask_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    NxsEth *s = opaque;
    if (size != 4 || (off != 0 && off != 4)) fatal(s, "INTC mask write", off);
    /* Only EtherC's gate is newly implemented. Other sources retain the
     * existing board priority-only policy; their mask readback stays one. */
    if (off == 0) s->mask |= value & ETH_MASK;
    else s->mask &= ~(value & ETH_MASK);
    update_irq(s);
}
static const MemoryRegionOps mask_ops = {
    .read = mask_read, .write = mask_write, .endianness = DEVICE_LITTLE_ENDIAN,
};
static uint64_t status_read(void *opaque, hwaddr off, unsigned size)
{
    NxsEth *s = opaque;
    if (size != 4 || (off != 0 && off != 4)) fatal(s, "INTC source read", off);
    return cdj_sh7764_eth_irq(&s->core) && (!off || !(s->mask & ETH_MASK)) ? ETH_MASK : 0;
}
static const MemoryRegionOps status_ops = {
    .read = status_read, .endianness = DEVICE_LITTLE_ENDIAN,
};
static ssize_t receive_frame(NetClientState *nc, const uint8_t *buf, size_t len)
{
    NxsEth *s = qemu_get_nic_opaque(nc);
    synchronize(s);
    if (!cdj_sh7764_eth_receive(&s->core, buf, len)) fatal(s, "receive", 0);
    qemu_log("nxs-ethernet: rx-input=%" PRIu64 " bytes=%zu virtual_ns=%" PRId64 "\n",
             ++s->rx, len, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    update_irq(s);
    return len;
}
static void link_changed(NetClientState *nc) { synchronize(qemu_get_nic_opaque(nc)); }
static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC, .size = sizeof(NICState),
    .receive = receive_frame, .link_status_changed = link_changed,
};
static void reset(DeviceState *dev)
{
    NxsEth *s = NXS_ETH(dev);
    timer_del(s->timer);
    cdj_sh7764_eth_init(&s->core, &dma_ops, s);
    cdj_rtl8201fl_reset(&s->phy, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->mask = UINT32_MAX; s->peer_present = false; s->last_link = false;
    s->tx = s->rx = 0;
    synchronize(s);
}
static void realize(DeviceState *dev, Error **errp)
{
    NxsEth *s = NXS_ETH(dev); SysBusDevice *bus = SYS_BUS_DEVICE(dev);
    memory_region_init_io(&s->io, OBJECT(s), &eth_ops, s, TYPE_NXS_ETH, 0x200);
    memory_region_init_alias(&s->alias, OBJECT(s), TYPE_NXS_ETH "-a7", &s->io, 0, 0x200);
    memory_region_init_io(&s->masks, OBJECT(s), &mask_ops, s, TYPE_NXS_ETH "-mask", 8);
    memory_region_init_alias(&s->masks_alias, OBJECT(s), TYPE_NXS_ETH "-mask-a7", &s->masks, 0, 8);
    memory_region_init_io(&s->status, OBJECT(s), &status_ops, s, TYPE_NXS_ETH "-source", 8);
    memory_region_init_alias(&s->status_alias, OBJECT(s), TYPE_NXS_ETH "-source-a7", &s->status, 0, 8);
    sysbus_init_mmio(bus, &s->io); sysbus_init_mmio(bus, &s->alias);
    sysbus_init_mmio(bus, &s->masks); sysbus_init_mmio(bus, &s->masks_alias);
    sysbus_init_mmio(bus, &s->status); sysbus_init_mmio(bus, &s->status_alias);
    sysbus_init_irq(bus, &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_info, &s->conf, TYPE_NXS_ETH, dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}
static const Property properties[] = { DEFINE_NIC_PROPERTIES(NxsEth, conf) };
static const VMStateDescription no_migration = {
    .name = TYPE_NXS_ETH, .unmigratable = true,
};
static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize; dc->vmsd = &no_migration;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, properties);
}
static const TypeInfo info = {
    .name = TYPE_NXS_ETH, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NxsEth), .class_init = class_init,
};
static void register_type(void) { type_register_static(&info); }
type_init(register_type)
void cdj_nxs_eth_init(qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_NXS_ETH);
    qemu_configure_nic_device(dev, false, NULL);
    SysBusDevice *bus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(bus, &error_fatal);
    sysbus_mmio_map(bus, 0, 0xfef00000); sysbus_mmio_map(bus, 1, 0x1ef00000);
    sysbus_mmio_map(bus, 2, 0xffd400d0); sysbus_mmio_map(bus, 3, 0x1fd400d0);
    sysbus_mmio_map(bus, 4, 0xffd400c0); sysbus_mmio_map(bus, 5, 0x1fd400c0);
    sysbus_connect_irq(bus, 0, irq);
}
