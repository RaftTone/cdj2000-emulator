"""Exercise physical SDRAM aliases through actual replay master dispatchers."""
from pathlib import Path
import shutil
import subprocess

import pytest
from tools.cdj_dsp.replay import SOURCES

ROOT = Path(__file__).resolve().parents[1]


def test_sdram_alias_reads_writes_hpi_edma_and_boundaries(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    harness = tmp_path / 'alias.c'
    harness.write_text('#define main replay_main\n#include "' +
                       str(SOURCES[0]) + '"\n#undef main\n' + r'''
#include <assert.h>
int main(void)
{
    cdj_c6747_emifb_reset(&emifb);
    uint32_t offset, value;
    const uint32_t base = 0xc0000000, alias = 0xd2000000;
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, alias, 4, 0, &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, alias, 4, 123, &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, alias, 0, sizeof(sdram), &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, alias, SIZE_MAX, sizeof(sdram), &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, 0xe0000000, 4, sizeof(sdram), &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, 0xbfffffff, 1, sizeof(sdram), &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, 0xdfffffff, 2, sizeof(sdram), &offset));
    assert(!cdj_c6747_emifb_sdram_offset(&emifb, 0xc1ffffff, 2, sizeof(sdram), &offset));
    assert(!memory_span(0xc1fffffc, 8));
    assert(!memory_span(0xdffffffc, 8));
    assert(!read_bus(NULL, alias + 1, &value));
    assert(!write_bus(NULL, alias, 0, 3, true));

    /* Genuine checkpoint fault address maps to populated bytes, not zero. */
    assert(write_bus(NULL, base + 0xccff9c, 0x89abcdef, 4, true));
    assert(read_bus(NULL, 0xd2ccff9c, &value) && value == 0x89abcdef);
    assert(write_bus(NULL, 0xd2ccff9c, 0x12345678, 4, false));
    assert(read_bus(NULL, base + 0xccff9c, &value) && value == 0x89abcdef);
    assert(write_bus(NULL, 0xd2ccff9c, 0x12345678, 4, true));
    assert(read_bus(NULL, base + 0xccff9c, &value) && value == 0x12345678);
    assert(host_memory(0xd2ccff9c) == host_memory(base + 0xccff9c));
    assert(write_bus(NULL, alias + 0x100, UINT64_C(0x1122334455667788), 8, true));
    assert(read_bus(NULL, base + 0x104, &value) && value == 0x11223344);
    assert(write_bus(NULL, alias + 0x101, 0xa5, 1, true));
    assert(write_bus(NULL, alias + 0x102, 0xb6c7, 2, true));
    assert(read_bus(NULL, base + 0x100, &value) && value == 0xb6c7a588);

    /* EDMA's staged-write forwarding compares physical pointers, so reads
     * through different aliases still see pending writes in the transaction. */
    EdmaBusContext context = {.mcasp = &mcasp_control};
    uint8_t bytes[4] = {9, 8, 7, 6}, observed[4];
    assert(edma_write_bytes(&context, alias + 0x200, bytes, 4, false));
    assert(context.write_count == 0);
    assert(edma_write_bytes(&context, alias + 0x200, bytes, 4, true));
    assert(context.write_count == 1 && context.writes[0].target == sdram + 0x200);
    assert(edma_read_bytes(&context, base + 0x200, observed, 4));
    assert(!memcmp(bytes, observed, 4));
    memcpy(context.writes[0].target, context.writes[0].bytes, 4);
    assert(read_bus(NULL, alias + 0x200, &value) && value == 0x06070809);
    edma_free_staged_writes(&context);

    assert(write_bus(NULL, 0xdffffffc, 0x76543210, 4, true));
    assert(read_bus(NULL, 0xc1fffffc, &value) && value == 0x76543210);
    assert(!host_memory(0xe0000000));
    emifb.sdcfg &= ~(1u << 16);
    assert(!read_bus(NULL, alias, &value));
    assert(!read_bus(NULL, base, &value));
    assert(!write_bus(NULL, alias, 0, 4, true));
    assert(!host_memory(alias));
    assert(!memory_span(alias, 4));
    return 0;
}
''')
    binary = tmp_path / 'alias'
    subprocess.run([cc, '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'emulator/qemu'), str(harness),
                    *map(str, SOURCES[1:]), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
