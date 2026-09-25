/*
 * Pioneer CDJ-2000: MAIN and GUI boards in one guest time (see cdj2000_cosim.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CDJ2000_COSIM_H
#define CDJ2000_COSIM_H

/* Bytes of a GUI request, handed over at the guest time they reach MAIN. */
typedef void (*CdjCosimRequestFn)(void *opaque, const uint8_t *data,
                                  unsigned len);

/* Called at every step: hand waiting request bytes over if MAIN can take
   them now -- what a chardev's can_read does on every main-loop turn. */
typedef void (*CdjCosimPollFn)(void *opaque);

/* CDJ_COSIM=<port> turns it on; false (and nothing else happens) otherwise. */
bool cdj_cosim_init(CdjCosimRequestFn sink, CdjCosimPollFn poll, void *opaque);
bool cdj_cosim_active(void);

/* A frame MAIN put on the link, stamped with the guest time it left. */
void cdj_cosim_send_record(const uint8_t *frame, unsigned len);

#endif
