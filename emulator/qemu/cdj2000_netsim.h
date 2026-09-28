/*
 * Pioneer CDJ-2000: two or more players on one Pro DJ Link segment, in one
 * guest time (see cdj2000_netsim.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CDJ2000_NETSIM_H
#define CDJ2000_NETSIM_H

/* An Ethernet frame from the segment, handed over at the guest time it
   reaches this player. */
typedef void (*CdjNetsimFrameFn)(void *opaque, const uint8_t *frame,
                                 unsigned len);

/* CDJ_NETSIM=HOST:PORT (a tools.cdj_main.link_hub --sync) turns it on; false,
   and nothing else happens, without it. */
bool cdj_netsim_init(CdjNetsimFrameFn sink, void *opaque);
bool cdj_netsim_active(void);

/* A frame this player sent, stamped with the guest time it left. */
void cdj_netsim_send(const uint8_t *frame, unsigned len);

#endif
