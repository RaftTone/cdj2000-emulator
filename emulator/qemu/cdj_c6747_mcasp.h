/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_MCASP_H
#define CDJ_C6747_MCASP_H
#include <stdbool.h>
#include <stdint.h>
/* SPRUH91D 24.1: configuration and transmit-buffer state. No physical pin
 * routing, external input, serializer clock progression, FIFO or interrupt
 * delivery yet. */
typedef struct {
    uint32_t pfunc[3], pdir[3], pdout[3];
} CdjC6747Mcasp;
/* Keep control state separate from CdjC6747Mcasp: the pin-only type is part
 * of older checkpoint layouts and must remain exactly 36 bytes. */
typedef struct {
    uint32_t undocumented04[3];
    uint32_t gblctl[3], amute[3], dlbctl[3], ditctl[3];
    uint32_t xmask[3], xfmt[3], afsxctl[3];
    uint32_t aclkxctl[3], ahclkxctl[3], xtdm[3];
    uint32_t xintctl[3], xstat[3], xclkchk[3];
    uint32_t dit[3][24];
    uint32_t srctl[3][16], xrdy[3];
    /* XBUF holds only genuine words accepted through one of the two hardware
     * ports.  Sequence numbers retain their order without claiming that a
     * serializer clock has shifted any word to an external audio device. */
    uint32_t xbuf[3][16], xdma_next[3];
    uint64_t xbuf_sequence[3][16], xbuf_writes[3], axevt_generation[3];
    /* Schema-7 state ends above.  These appended fields record only explicit
     * slot-boundary calls and genuine accepted XBUF words; they do not imply
     * a physical serializer clock or an external audio sample. */
    uint32_t xrsr[3][16], xslot[3];
    uint64_t xrsr_source_sequence[3][16], tx_slot_boundaries[3];
} CdjC6747McaspControl;
void cdj_c6747_mcasp_reset(CdjC6747Mcasp *s);
/* Candidate window only; the control write still validates register/value. */
bool cdj_c6747_mcasp_control_write_mapped(uint32_t address, unsigned size);
bool cdj_c6747_mcasp_read(const CdjC6747Mcasp *s, uint32_t address,
                         uint32_t *value);
bool cdj_c6747_mcasp_write(CdjC6747Mcasp *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit);
/* Taps in the transmit clock chain AUXCLK -> AHCLKX -> ACLKX -> AFSX. */
#define CDJ_C6747_MCASP_AHCLKX 0u
#define CDJ_C6747_MCASP_ACLKX  1u
#define CDJ_C6747_MCASP_AFSX   2u

/* One tap of the transmit clock chain for one McASP, as an exact unreduced
 * fraction: *numerator Hz over *denominator, given AUXCLK in Hz from
 * cdj_c6747_pll_auxclk_hz().  SPRUH91D Table 6-2 printed page 104 (PDF page
 * 104) and Table 7-1 printed page 118 put the McASP serial clock on AUXCLK
 * (the McASP's peripheral bus interface is on SYSCLK2 instead, which is a
 * different clock and not what this returns).  Figure 24-15 printed page 996
 * and Tables 24-37 and 24-38 printed pages 1077 and 1078 give
 *   AHCLKX = AUXCLK / (HCLKXDIV + 1)    (/1 ... /4096)
 *   ACLKX  = AHCLKX / (CLKXDIV + 1)     (/1 ... /32)
 * and for the internally generated TDM frame sync SPRUH91D printed page 1011
 * gives XMOD as the TDM slot count (2h..20h), so
 *   AFSX   = ACLKX / (slot bits x slots).
 * False - outputs untouched - wherever the manuals fix no rate: an external
 * AHCLKX/ACLKX pin source, an externally generated frame sync, burst mode,
 * DIT mode, or an illegal slot size.  This is a rate, not a run condition:
 * GBLCTL's clock/serializer resets and the PSC1 LPSC that gates the McASP
 * module (LPSC 7, 8 and 9, Table 8-2 printed page 141) decide whether these
 * clocks run, and none of them change this rate. */
bool cdj_c6747_mcasp_tx_clock_hz(const CdjC6747McaspControl *s,
                                unsigned instance, uint32_t auxclk_hz,
                                unsigned tap, uint64_t *numerator,
                                uint32_t *denominator);

void cdj_c6747_mcasp_control_reset(CdjC6747McaspControl *s);
bool cdj_c6747_mcasp_control_valid(const CdjC6747McaspControl *s);
bool cdj_c6747_mcasp_control_read(const CdjC6747McaspControl *s,
                                 uint32_t address, uint32_t *value);
bool cdj_c6747_mcasp_control_write(CdjC6747McaspControl *s,
                                  uint32_t address, uint64_t value,
                                  unsigned size, bool commit);
/* Level query for the documented XDATA/AXEVT service condition.  A false
 * result for an invalid instance also keeps callers from inventing events. */
bool cdj_c6747_mcasp_axevt_ready(const CdjC6747McaspControl *s,
                                unsigned instance);
/* Advance one caller-timed, architecture-valid transmit slot.  On success,
 * axevt reports whether this active slot rearmed XDATA/AXEVT.  Inactive TDM
 * slots still advance XSLOT but do not consume XBUF or generate an event.
 * Failure is side-effect free and covers stopped resets/clocks and unsupported
 * or inconsistent frame configuration. */
bool cdj_c6747_mcasp_tx_slot(CdjC6747McaspControl *s, unsigned instance,
                            bool *axevt);
#endif
