#ifndef CDJ_RTL8201FL_H
#define CDJ_RTL8201FL_H
#include <stdbool.h>
#include <stdint.h>

/* Functional virtual link, not analog PHY timing or PTP synchronization.
 * All times are absolute monotonically increasing virtual nanoseconds.
 * Initial MDIO transaction requires 32 preamble ones; later suppression works.
 */
#define CDJ_PHY_NO_DEADLINE UINT64_MAX
#define CDJ_PHY_RESET_NS UINT64_C(1000000)
#define CDJ_PHY_NEGOTIATE_NS UINT64_C(100000000)
typedef struct CdjRtl8201fl {
    uint16_t bmcr, anar, partner, anlpar, aner, data;
    uint16_t page, led_config, led_control, eee_config, mmd_control, mmd_address, eee_advert;
    uint64_t now, deadline;
    uint32_t header;
    uint8_t pir, mdi, preamble, bits;
    bool peer, link, latch_low, reset_pending, synchronized, frame, idle_needed;
} CdjRtl8201fl;
void cdj_rtl8201fl_reset(CdjRtl8201fl *s, uint64_t now);
/* Peer advertisement is an explicit local virtual-peer input, not invented
 * network discovery. Supports base page CSMA/CD 10/100 abilities only. */
bool cdj_rtl8201fl_set_peer(CdjRtl8201fl *s, bool present,
                          uint16_t advertisement, uint64_t now);
bool cdj_rtl8201fl_advance(CdjRtl8201fl *s, uint64_t now);
uint64_t cdj_rtl8201fl_deadline(const CdjRtl8201fl *s);
bool cdj_rtl8201fl_link(const CdjRtl8201fl *s);
/* NXS LED0/PHYAD0 is active low; false means unsupported LED mode. */
bool cdj_rtl8201fl_led0(const CdjRtl8201fl *s, bool *high);
/* PIR: MDC bit0, MMD bit1 (MAC drives), MDO bit2, MDI bit3.
 * Unsupported operation returns false with entire state unchanged. */
bool cdj_rtl8201fl_pir_write(CdjRtl8201fl *s, uint32_t value);
uint32_t cdj_rtl8201fl_pir_read(const CdjRtl8201fl *s);
bool cdj_rtl8201fl_read(CdjRtl8201fl *s, unsigned reg, uint16_t *value);
bool cdj_rtl8201fl_write(CdjRtl8201fl *s, unsigned reg, uint16_t value);
#endif
