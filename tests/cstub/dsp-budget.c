/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "cdj_dsp_budget.h"

int main(void)
{
    uint32_t budget = 7;
    assert(cdj_dsp_legacy_budget_parse(NULL, &budget));
    assert(budget == CDJ_DSP_LEGACY_BUDGET_DEFAULT);
    assert(cdj_dsp_legacy_budget_parse("", &budget));
    assert(budget == CDJ_DSP_LEGACY_BUDGET_DEFAULT);
    assert(cdj_dsp_legacy_budget_parse("65536", &budget));
    assert(budget == CDJ_DSP_LEGACY_BUDGET_FAST);
    assert(cdj_dsp_legacy_budget_parse("4096", &budget));
    assert(budget == CDJ_DSP_LEGACY_BUDGET_MIN);
    assert(cdj_dsp_legacy_budget_parse("1000000", &budget));
    assert(budget == CDJ_DSP_LEGACY_BUDGET_DEFAULT);

    budget = 123;
    assert(!cdj_dsp_legacy_budget_parse("4095", &budget));
    assert(!cdj_dsp_legacy_budget_parse("1000001", &budget));
    assert(!cdj_dsp_legacy_budget_parse("65536x", &budget));
    assert(!cdj_dsp_legacy_budget_parse("-1", &budget));
    assert(!cdj_dsp_legacy_budget_parse("18446744073709551616", &budget));
    assert(!cdj_dsp_legacy_budget_parse("65536", NULL));
    assert(budget == 123);
    return 0;
}
