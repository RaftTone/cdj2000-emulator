/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cdj_dsp_budget.h"

bool cdj_dsp_legacy_budget_parse(const char *text, uint32_t *budget)
{
    char *end = NULL;
    uint64_t value;

    if (!budget) return false;
    if (!text || !*text) {
        *budget = CDJ_DSP_LEGACY_BUDGET_DEFAULT;
        return true;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || !end || *end || value < CDJ_DSP_LEGACY_BUDGET_MIN ||
        value > CDJ_DSP_LEGACY_BUDGET_DEFAULT)
        return false;
    *budget = value;
    return true;
}
