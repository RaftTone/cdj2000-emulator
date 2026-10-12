/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cdj_link: the GUI board's SPORT1 link to MAIN for cdj-gui-run -- the
 * bfin_sport_link_* model of patches/02 (dv-bfin_ppi.c) with the delivery
 * rules of patches 05, 13, 32 and 33, the captures of patches 10 and 11 and
 * the co-simulation client of patch 14 (emulator/qemu/cdj2000_cosim.c has the
 * wire format). No Blackfin dependency: bf531.c's DMA asks it for bytes.
 *
 * Configured from the same environment bin/cdj-run reads:
 *   BFIN_MAIN_LINK=host:port   requests out on port, records in on port+2
 *   BFIN_COSIM=host:port       both over one co-simulation connection
 *   BFIN_COSIM_QUANTUM_US      link latency and lookahead (100)
 *   BFIN_LINK_FRESH_ONLY=1     a record is delivered once, never repeated
 *   BFIN_LINK_NATIVE_PARTIAL_DMA=1  a fresh DLNK burst lands once, oldest
 *                              first, in an oversized receive, with its length
 *   BFIN_LINK_DEPTH=n          unread records kept per length (1)
 *   BFIN_SPORT_RX_ZERO_200     200-byte housekeeping reads complete with zeros
 *                              (BFIN_LINK_NO_ZERO200 refuses them on a live link)
 *   BFIN_MAIN_PEER + BFIN_MAIN_PEER_STATUS[_HOLD]  canned 64-byte bootstrap
 *                              records, only until MAIN has spoken
 *   BFIN_MAIN_LINK_DUMP        "SPRX" + LE32 length + body per delivery
 *   BFIN_SPORT_TX_OUTPUT       "SPTX" + LE32 SPORT base + LE32 length + body
 */
#ifndef CDJ_LINK_H
#define CDJ_LINK_H

#include <stdint.h>
#include <stdio.h>

typedef struct cdj_link cdj_link;

cdj_link *cdj_link_new(void);
void      cdj_link_free(cdj_link *l);

/* Whether MAIN is attached at all (BFIN_MAIN_LINK or BFIN_COSIM). */
int cdj_link_enabled(const cdj_link *l);

/* One pump of the receive DMA: up to `cap` bytes for the buffer at `addr`.
 * Returns how many landed in dst; 0 means nothing yet, retry later. */
unsigned cdj_link_rx(cdj_link *l, uint8_t *dst, unsigned cap, uint32_t addr);

/* One transmit DMA unit, at this board's guest time now_ns. */
void cdj_link_tx(cdj_link *l, const uint8_t *data, unsigned len, int64_t now_ns);

/* Co-simulation (BFIN_COSIM): connect (exits 2 when nothing listens). */
int  cdj_link_cosim(const cdj_link *l);
void cdj_link_cosim_connect(cdj_link *l);
/* Takes MAIN's messages, hands over records due by now_ns, reports this
 * board's time, and waits while now_ns is a whole latency past MAIN's
 * promise. Returns the guest ns the board may run to before the next step.
 * Exits 0 when MAIN goes away, as bin/cdj-run does. */
int64_t cdj_link_cosim_step(cdj_link *l, int64_t now_ns);

/* Hands a framed record body to the slots, as the record socket would. */
void cdj_link_inject(cdj_link *l, const uint8_t *frame, unsigned len);

/* One line of counters for the STATS output. */
void cdj_link_stats(const cdj_link *l, FILE *f);

#endif
