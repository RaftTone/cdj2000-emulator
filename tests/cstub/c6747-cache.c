/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>

#include "cdj_c6747_cache.h"

int main(void)
{
    CdjC6747Cache cache;
    uint32_t value;
    uint32_t offset;
    cdj_c6747_cache_reset(&cache);
    assert(cdj_c6747_cache_read(&cache, 0x01840000, &value) && value == 0);
    assert(cdj_c6747_cache_read(&cache, 0x01840020, &value) && value == 7);
    assert(cdj_c6747_cache_read(&cache, 0x01840040, &value) && value == 7);
    assert(!cdj_c6747_l1d_sram_span(&cache, 0x00f00000, 4, &offset));
    assert(cdj_c6747_cache_write(&cache, 0x01840020, 3, 4, false));
    assert(cache.l1pcfg == 7);
    assert(cdj_c6747_cache_write(&cache, 0x01840020, 3, 4, true));
    assert(cdj_c6747_cache_read(&cache, 0x01840020, &value) && value == 3);
    assert(cdj_c6747_cache_write(&cache, 0x01840040, 0xffffffff, 4, true));
    assert(cdj_c6747_cache_read(&cache, 0x01840040, &value) && value == 7);
    assert(cdj_c6747_cache_write(&cache, 0x01840040, 3, 4, true));
    assert(cdj_c6747_l1d_sram_span(
        &cache, 0x00f00000, 0x4000, &offset) && offset == 0);
    assert(cdj_c6747_l1d_sram_span(
        &cache, 0x11f03ffc, 4, &offset) && offset == 0x3ffc);
    assert(!cdj_c6747_l1d_sram_span(
        &cache, 0x00f03ffc, 8, &offset));
    assert(!cdj_c6747_l1d_sram_span(
        &cache, 0x00f04000, 4, &offset));
    assert(cdj_c6747_cache_write(&cache, 0x01840040, 0, 4, true));
    assert(cdj_c6747_l1d_sram_span(
        &cache, 0x00f07ffc, 4, &offset) && offset == 0x7ffc);
    assert(!cdj_c6747_l1d_sram_span(
        &cache, 0x00f00000, SIZE_MAX, &offset));
    assert(!cdj_c6747_l1d_sram_span(
        &cache, 0x00f07ffc, 8, &offset));
    assert(cdj_c6747_cache_write(&cache, 0x01840040, 4, 4, true));
    assert(!cdj_c6747_l1d_sram_span(&cache, 0x00f00000, 4, &offset));

    assert(cdj_c6747_cache_write(&cache, 0x01840024, 1, 4, true));
    assert(cdj_c6747_cache_read(&cache, 0x01840024, &value) && value == 1);
    assert(cdj_c6747_cache_write(&cache, 0x01840024, 0, 4, true));
    assert(cdj_c6747_cache_read(&cache, 0x01840024, &value) && value == 0x10000);

    assert(cdj_c6747_cache_write(&cache, 0x01844000, 0xc0000000, 4, true));
    assert(!cdj_c6747_cache_read(&cache, 0x01844000, &value));
    assert(cdj_c6747_cache_write(&cache, 0x01844004, 0x40, 4, true));
    assert(cdj_c6747_cache_read(&cache, 0x01844004, &value) && value == 0);
    assert(cdj_c6747_cache_write(&cache, 0x01845004, 1, 4, true));
    assert(cdj_c6747_cache_read(&cache, 0x01845004, &value) && value == 0);

    assert(cdj_c6747_cache_write(&cache, 0x01848100, 1, 4, true)); /* MAR64 */
    assert(cdj_c6747_cache_read(&cache, 0x01848100, &value) && value == 1);
    assert(cdj_c6747_cache_write(&cache, 0x01848200, 3, 4, true)); /* MAR128 */
    assert(cdj_c6747_cache_read(&cache, 0x01848200, &value) && value == 1);
    assert(cdj_c6747_cache_write(&cache, 0x01848300, 1, 4, true)); /* MAR192 */
    assert(cdj_c6747_cache_read(&cache, 0x01848300, &value) && value == 1);
    assert(!cdj_c6747_cache_write(&cache, 0x01848000, 1, 4, true)); /* reserved MAR0 */
    assert(!cdj_c6747_cache_write(&cache, 0x01848380, 1, 4, true)); /* reserved MAR224 */
    assert(!cdj_c6747_cache_write(&cache, 0x01841000, 1, 4, true)); /* unmodeled EDMA weight */
    assert(!cdj_c6747_cache_write(&cache, 0x01840020, 1, 2, true));
    return 0;
}
