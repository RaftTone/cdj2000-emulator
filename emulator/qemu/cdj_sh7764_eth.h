#ifndef CDJ_SH7764_ETH_H
#define CDJ_SH7764_ETH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CdjSh7764EthOps {
    bool (*dma_read)(void *, uint32_t, void *, size_t);
    bool (*dma_write)(void *, uint32_t, const void *, size_t);
    bool (*send)(void *, const uint8_t *, size_t);
    bool (*desc_read)(void *, uint32_t, uint32_t words[3]);
    bool (*desc_write)(void *, uint32_t, const uint32_t *, size_t word_count);
} CdjSh7764EthOps;
typedef struct CdjSh7764Eth {
    uint32_t regs[128], tx_cursor, rx_cursor;
    bool link, raw_lmon;
    const CdjSh7764EthOps *ops;
    void *opaque;
    const char *error;
} CdjSh7764Eth;
/* Functional, atomic descriptor DMA; no wire timing, cache, PHY or FCS model.
 * Payload callbacks transfer raw bytes. Descriptor callbacks transfer logical
 * bus words, independent of payload DE (manual 20.2.1). Adapter normalizes
 * CPU byte lanes: little-endian SH firmware stores TACT at the high byte of
 * a native longword. Core byte arrays are internal big-endian serialization.
 * Failed DMA/send is terminal; already completed external writes cannot roll
 * back. Frames exclude FCS. Reset latency (64 B-clock cycles) is not modeled.
 * DL16/DE1 only; frame maximum 2048 bytes, per-call descriptor bound 128.
 * Mid-frame empty descriptors fail closed (partial abort DMA is unmodeled).
 * set_link supplies backend link and raw external PHY LMON separately; it
 * does not create a peer or transmit pending data. A DMA request kicks TX.
 * Public reset is hardware reset; EDMR.SWR preserves manual-listed registers.
 * Register access to PIR belongs exclusively to the adapter's PHY model. */
void cdj_sh7764_eth_init(CdjSh7764Eth *, const CdjSh7764EthOps *, void *);
void cdj_sh7764_eth_reset(CdjSh7764Eth *);
bool cdj_sh7764_eth_read(CdjSh7764Eth *, uint32_t, uint32_t *);
bool cdj_sh7764_eth_write(CdjSh7764Eth *, uint32_t, uint32_t);
bool cdj_sh7764_eth_receive(CdjSh7764Eth *, const uint8_t *, size_t);
void cdj_sh7764_eth_set_link(CdjSh7764Eth *, bool, bool);
bool cdj_sh7764_eth_irq(const CdjSh7764Eth *);
#endif
