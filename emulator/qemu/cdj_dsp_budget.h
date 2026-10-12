/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_BUDGET_H
#define CDJ_DSP_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

#define CDJ_DSP_LEGACY_BUDGET_DEFAULT 1000000u
#define CDJ_DSP_LEGACY_BUDGET_FAST 65536u
#define CDJ_DSP_LEGACY_BUDGET_MIN 4096u

bool cdj_dsp_legacy_budget_parse(const char *text, uint32_t *budget);

#endif
