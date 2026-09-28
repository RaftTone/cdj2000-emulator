#ifndef CDJ_NXS_ETH_H
#define CDJ_NXS_ETH_H
#include "hw/core/irq.h"
void cdj_nxs_eth_init(qemu_irq irq);
/* The CDJ-2000 board's EtherC, created only when a -nic names
 * model=cdj2000-ethernet or CDJ_NETSIM names a synchronising hub; false when
 * there is neither. */
bool cdj2000_eth_init(qemu_irq irq);
#endif
