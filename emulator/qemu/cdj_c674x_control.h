/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_CONTROL_H
#define CDJ_C674X_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
#include "cdj_c674x.h"
/* The architectural read view of a control register: reserved fields that
 * SPRUFE8B marks "always read as 0" are masked off here rather than being
 * kept out of storage, and the few derived fields (ISTP.HPEINT, TSR/ITSR GIE)
 * are computed from the registers they alias. TSCL additionally derives its
 * value from cycles and its reset-disabled enable origin. */
uint32_t cdj_c674x_control_read(const CdjC674x *cpu, unsigned id);
/* Architectural 64-bit timestamp value. Architectural IDs are TSCL=10 and
 * TSCH=11 (TI dis6x: 0x022803e2 MVC TSCL,B4; 0x002c03e2 MVC TSCH,B0).
 * The otherwise-unused storage slot control_ready[16] stores the first cycle
 * at which the enabled counter reads zero; zero means reset-disabled, while
 * control[16] stores the TSCH snapshot captured by a TSCL read. */
uint64_t cdj_c674x_timestamp(const CdjC674x *cpu);
/* Whether MVC can name this control register id as a source / destination.
 * Ids this core does not model stay fail-closed: the execute packet rejects
 * them rather than reading or storing an invented value. */
bool cdj_c674x_control_read_supported(unsigned id);
bool cdj_c674x_control_write_supported(unsigned id);
#endif
