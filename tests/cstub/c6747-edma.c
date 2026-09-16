/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_edma.h"

#define EDMA 0x01c00000u
#define PARAM(n) (0x01c04000u + (n) * 32u)
#define MPORT 0x01d06000u

typedef struct {
    uint8_t memory[0x4000];
    uint32_t port_words[256];
    unsigned port_count;
} TestBus;

static bool bus_read(void *opaque, uint32_t address, uint8_t *bytes,
                     size_t size)
{
    TestBus *bus = opaque;
    if (address < 0x1000u || size > sizeof(bus->memory) ||
        address - 0x1000u > sizeof(bus->memory) - size) return false;
    memcpy(bytes, bus->memory + (address - 0x1000u), size);
    return true;
}

static bool bus_write(void *opaque, uint32_t address, const uint8_t *bytes,
                      size_t size, bool commit)
{
    TestBus *bus = opaque;
    if (address == MPORT && size == 4) {
        if (commit) {
            assert(bus->port_count < 256);
            memcpy(&bus->port_words[bus->port_count++], bytes, 4);
        }
        return true;
    }
    if (address < 0x1000u || size > sizeof(bus->memory) ||
        address - 0x1000u > sizeof(bus->memory) - size) return false;
    if (commit) memcpy(bus->memory + (address - 0x1000u), bytes, size);
    return true;
}

static bool wr(CdjC6747Edma *s, const CdjC6747EdmaBus *bus,
               uint32_t address, uint32_t value)
{
    assert(cdj_c6747_edma_write(s, address, value, 4, false, bus));
    return cdj_c6747_edma_write(s, address, value, 4, true, bus);
}

static void param(CdjC6747Edma *s, const CdjC6747EdmaBus *bus, unsigned set,
                  uint32_t opt, uint32_t src, uint32_t abcnt, uint32_t dst,
                  uint32_t bidx, uint32_t link_reload, uint32_t cidx,
                  uint32_t ccnt)
{
    const uint32_t words[] = {opt, src, abcnt, dst, bidx, link_reload,
                              cidx, ccnt};
    for (unsigned i = 0; i < 8; i += 2) {
        uint64_t pair = words[i] | ((uint64_t)words[i + 1] << 32);
        assert(cdj_c6747_edma_write(s, PARAM(set) + i * 4,
                                    pair, 8, false, bus));
        assert(cdj_c6747_edma_write(s, PARAM(set) + i * 4,
                                    pair, 8, true, bus));
    }
}

int main(void)
{
    CdjC6747Edma s, before;
    TestBus memory = {0};
    CdjC6747EdmaBus bus = {bus_read, bus_write, &memory};
    uint32_t v;

    cdj_c6747_edma_reset(&s);
    before = (CdjC6747Edma){0};
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(cdj_c6747_edma_valid(&s));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x000, &v) &&
           v == 0x40015300u);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x004, &v) &&
           v == 0x00213344u);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x600, &v) && v == 0);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x604, &v) && v == 0);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x640, &v) && v == 0);

    /* Exact firmware global setup at C004DE04-C004DFD4: regions, queue
     * selection, queue watermark and all eight QDMA maps. */
    assert(wr(&s, &bus, EDMA + 0x340, 0));
    assert(wr(&s, &bus, EDMA + 0x348, 0x28));
    assert(wr(&s, &bus, EDMA + 0x350, 0));
    assert(wr(&s, &bus, EDMA + 0x358, 0));
    assert(wr(&s, &bus, EDMA + 0x380, 0));
    assert(wr(&s, &bus, EDMA + 0x384, 0xff));
    assert(wr(&s, &bus, EDMA + 0x388, 0));
    assert(wr(&s, &bus, EDMA + 0x38c, 0));
    assert(wr(&s, &bus, EDMA + 0x240, 0x11010111));
    for (unsigned i = 1; i < 4; ++i)
        assert(wr(&s, &bus, EDMA + 0x240 + i * 4, 0x11111111));
    assert(wr(&s, &bus, EDMA + 0x260, 0x11111111));
    assert(wr(&s, &bus, EDMA + 0x620, 0x1010));
    for (unsigned q = 0; q < 8; ++q)
        assert(wr(&s, &bus, EDMA + 0x200 + q * 4,
                  (40u + q) * 32u + 7u * 4u));

    /* Exact controller/region clears from C004E284 onward.  Shadow writes
     * are filtered through DRAE1/QRAE1; all-one clear masks are legal. */
    const uint32_t clear32[] = {
        0x1028, 0x2228, 0x1058, 0x2258, 0x0308, 0x2208,
        0x1040, 0x2240, 0x1070, 0x2270,
    };
    for (unsigned i = 0; i < sizeof(clear32) / sizeof(clear32[0]); ++i)
        assert(wr(&s, &bus, EDMA + clear32[i], 0xffffffffu));
    assert(wr(&s, &bus, EDMA + 0x1088, 0xff));
    assert(wr(&s, &bus, EDMA + 0x2288, 0xff));
    assert(wr(&s, &bus, EDMA + 0x0314, 0xff));
    assert(wr(&s, &bus, EDMA + 0x031c, 0x00030003));
    assert(wr(&s, &bus, EDMA + 0x1094, 0xff));
    assert(wr(&s, &bus, EDMA + 0x2294, 0xff));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x308, &v) && v == s.emr);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x314, &v) && v == s.qemr);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x31c, &v) && v == s.ccerr);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x240, &v) &&
           v == 0x11010111);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x348, &v) && v == 0x28);
    assert(cdj_c6747_edma_valid(&s));

    /* Genuine PaRAM constructors use STNDW.  Paired little-endian words are
     * accepted atomically, including the word 6/7 pair at a set boundary. */
    assert(cdj_c6747_edma_write(&s, PARAM(127) + 24,
                                 UINT64_C(0x00000010fff40040), 8, false,
                                 &bus));
    assert(cdj_c6747_edma_write(&s, PARAM(127) + 24,
                                 UINT64_C(0x00000010fff40040), 8, true,
                                 &bus));
    assert(cdj_c6747_edma_read(&s, PARAM(127) + 24, &v) &&
           v == 0xfff40040u);
    assert(cdj_c6747_edma_read(&s, PARAM(127) + 28, &v) && v == 16);

    /* Shadow region 1 owns only channels/TCCs 3 and 5. */
    assert(wr(&s, &bus, EDMA + 0x2230, 0xffffffffu));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x2220, &v) && v == 0x28);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x2230, &v) && v == 0x28);
    assert(wr(&s, &bus, EDMA + 0x2260, 0xffffffffu));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x2250, &v) && v == 0x28);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x2258, &v) && v == 0x28);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x2260, &v) && v == 0x28);

    /* Firmware's channel-3 ping/pong A-sync geometry.  Each event moves one
     * exact 32-bit sample, alternating planar left/right blocks. */
    for (unsigned frame = 0; frame < 16; ++frame) {
        uint32_t left = 0x10000000u + frame;
        uint32_t right = 0x20000000u + frame;
        memcpy(memory.memory + frame * 4, &left, 4);
        memcpy(memory.memory + 0x40 + frame * 4, &right, 4);
    }
    param(&s, &bus, 3, 0x00103200, 0x1000, 0x00020004, MPORT,
          0x00000040, 0x00024400, 0x0000ffc4, 16);
    param(&s, &bus, 32, 0x00103200, 0x1080, 0x00020004, MPORT,
          0x00000040, 0x00024420, 0x0000ffc4, 16);
    param(&s, &bus, 33, 0x00103200, 0x1000, 0x00020004, MPORT,
          0x00000040, 0x00024400, 0x0000ffc4, 16);
    before = s;
    TestBus memory_before = memory;
    assert(cdj_c6747_edma_event(&s, 3, &bus));
    assert(memory.port_count == 1 && memory.port_words[0] == 0x10000000u);
    assert(s.param[3][1] == 0x1040 && (s.param[3][2] >> 16) == 1);
    for (unsigned event = 1; event < 32; ++event)
        assert(cdj_c6747_edma_event(&s, 3, &bus));
    for (unsigned frame = 0; frame < 16; ++frame) {
        assert(memory.port_words[frame * 2] == 0x10000000u + frame);
        assert(memory.port_words[frame * 2 + 1] == 0x20000000u + frame);
    }
    assert(!memcmp(s.param[3], s.param[32], sizeof(s.param[3])));
    assert(s.ipr & (1u << 3));
    assert(cdj_c6747_edma_irq_pending(&s, 1));
    assert(!cdj_c6747_edma_irq_pending(&s, 0));
    assert(cdj_c6747_edma_take_irq_notification(&s, 1));
    assert(!cdj_c6747_edma_take_irq_notification(&s, 1));
    /* An unrelated write cannot turn the still-pending IPR into another
     * interrupt pulse.  IEVAL=1 explicitly requests exactly one repulse. */
    assert(wr(&s, &bus, EDMA + 0x620, 0x1010));
    assert(!cdj_c6747_edma_take_irq_notification(&s, 1));
    assert(wr(&s, &bus, EDMA + 0x2278, 0));
    assert(!cdj_c6747_edma_take_irq_notification(&s, 1));
    assert(wr(&s, &bus, EDMA + 0x2278, 1));
    assert(cdj_c6747_edma_take_irq_notification(&s, 1));
    assert(!cdj_c6747_edma_take_irq_notification(&s, 1));
    assert(wr(&s, &bus, EDMA + 0x2270, 1u << 3));
    assert(!cdj_c6747_edma_irq_pending(&s, 1));
    assert(s.transfer_requests == 32 && s.bytes_transferred == 128);
    (void)before; (void)memory_before;

    /* Final chaining services the target DMA channel immediately. */
    cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
    memory.memory[0] = 0xa5; memory.memory[1] = 0x5a;
    param(&s, &bus, 0, (1u << 22) | (1u << 12), 0x1000,
          0x00010001, 0x1200, 0, 0xffff, 0, 1);
    param(&s, &bus, 1, (1u << 20) | (1u << 12), 0x1001,
          0x00010001, 0x1201, 0, 0xffff, 0, 1);
    before = s; memory_before = memory;
    assert(cdj_c6747_edma_write(&s, EDMA + 0x1010, 1, 4, false, &bus));
    assert(!memcmp(&s, &before, sizeof(s)));

    assert(!memcmp(&memory, &memory_before, sizeof(memory)));
    assert(cdj_c6747_edma_write(&s, EDMA + 0x1010, 1, 4, true, &bus));
    assert(memory.memory[0x200] == 0xa5 && memory.memory[0x201] == 0x5a);
    assert(s.transfer_requests == 2 && (s.ipr & 2));

    /* STATIC suppresses every PaRAM update while preserving the movement. */
    cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
    memory.memory[0] = 0xcc;
    param(&s, &bus, 2, (1u << 3), 0x1000, 0x00020001, 0x1300,
          0x00010001, 0xffff, 0, 1);
    uint32_t static_param[8]; memcpy(static_param, s.param[2], sizeof(static_param));
    assert(wr(&s, &bus, EDMA + 0x1010, 1u << 2));
    assert(!memcmp(static_param, s.param[2], sizeof(static_param)));

    /* QDMA0 maps to PaRAM40 and triggers on word 7 only while enabled. */
    cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
    memory.memory[0] = 0x77;
    assert(wr(&s, &bus, EDMA + 0x200, 40u * 32u + 7u * 4u));
    const uint32_t qwords[] = {(1u << 20) | (7u << 12), 0x1000,
        0x00010001, 0x1400, 0, 0xffff, 0};
    for (unsigned i = 0; i < 7; ++i)
        assert(wr(&s, &bus, PARAM(40) + i * 4, qwords[i]));
    assert(wr(&s, &bus, EDMA + 0x108c, 1));
    assert(wr(&s, &bus, PARAM(40) + 28, 1));
    assert(memory.memory[0x400] == 0x77 && (s.ipr & (1u << 7)));
    assert(s.transfer_requests == 1 && s.qer == 0);

    /* Unsupported modes, reserved fields/offsets and malformed accesses all
     * fail closed without changing controller state. */
    cdj_c6747_edma_reset(&s); before = s;
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x240, 2, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x200, 128u << 5, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, PARAM(0), 1u << 31, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, PARAM(0) + 28, 1u << 16, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, PARAM(0) + 28, 0, 8, true, &bus));
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x31c, 4, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x284, 0, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x1000, 0, 4, true, &bus));
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x1010, 1, 1, true, &bus));
    assert(!cdj_c6747_edma_write(&s, EDMA + 0x1010,
                                  UINT64_C(1) << 32, 4, true, &bus));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x1008, &v) && v == 0);
    assert(!cdj_c6747_edma_read(&s, EDMA + 0x3000, &v));
    assert(!cdj_c6747_edma_event(&s, 32, &bus));
    assert(!memcmp(&s, &before, sizeof(s)));
    CdjC6747Edma invalid = s;
    invalid.param[0][0] = 1u << 31;
    assert(!cdj_c6747_edma_valid(&invalid));
    invalid = s; invalid.param[0][7] = 1u << 16;
    assert(!cdj_c6747_edma_valid(&invalid));
    invalid = s; invalid.irq_notifications = 1u << CDJ_C6747_EDMA_REGIONS;
    assert(!cdj_c6747_edma_valid(&invalid));
    assert(!cdj_c6747_edma_take_irq_notification(
        &s, CDJ_C6747_EDMA_REGIONS));

    /* AB-sync: three strided arrays per event, two frames. C indices
     * start at the first array, BCNT remains unchanged, BCNTRLD ignored. */
    cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
    for (unsigned i = 0; i < 128; ++i) memory.memory[i] = i + 1;
    param(&s, &bus, 0, 4, 0x1020, 0x00030004, 0x1100,
          0x0008fff8, 0x0009ffff, 0x00200020, 2);
    assert(wr(&s, &bus, EDMA + 0x1010, 1));
    for (unsigned i = 0; i < 3; ++i)
        assert(!memcmp(memory.memory + 0x100 + i * 8,
                       memory.memory + 0x20 - i * 8, 4));
    assert(s.param[0][1] == 0x1040 && s.param[0][3] == 0x1120);
    assert(s.param[0][2] == 0x00030004 && s.param[0][7] == 1);
    assert(s.bytes_transferred == 12 && s.transfer_requests == 1);
    assert(wr(&s, &bus, EDMA + 0x1010, 1));
    for (unsigned i = 0; i < 3; ++i)
        assert(!memcmp(memory.memory + 0x120 + i * 8,
                       memory.memory + 0x40 - i * 8, 4));
    assert(s.bytes_transferred == 24 && s.transfer_requests == 2);
    assert(s.param[0][7] == 0);

    /* Native LPCM QDMA geometry: 588 four-byte arrays spread across
     * eight-byte destination slots, triggered by writing CCNT. */
    cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
    for (unsigned i = 0; i < 2352; ++i) memory.memory[i] = i * 17u + 3u;
    param(&s, &bus, 64, 0x8204, 0x1000, 0x024c0004, 0x2000,
          0x00080004, 0xffff, 0, 1);
    assert(wr(&s, &bus, EDMA + 0x200, 64 * 32 + 7 * 4));
    assert(wr(&s, &bus, EDMA + 0x108c, 1));
    before = s; memory_before = memory;
    assert(cdj_c6747_edma_write(&s, PARAM(64) + 28, 1, 4, false, &bus));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(!memcmp(&memory, &memory_before, sizeof(memory)));
    assert(wr(&s, &bus, PARAM(64) + 28, 1));
    for (unsigned i = 0; i < 588; ++i) {
        assert(!memcmp(memory.memory + 0x1000 + i * 8,
                       memory.memory + i * 4, 4));
        assert(memory.memory[0x1004 + i * 8] == 0);
    }
    assert(s.bytes_transferred == 2352 && s.transfer_requests == 1);

    /* Firmware C004E1C0 polls one TCC, then reads/ORs/writes ICR. Two
     * completed transfers must remain independently acknowledgeable. */
    cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
    memory.memory[0] = 0x39; memory.memory[1] = 0xa7;
    param(&s, &bus, 0, 0x00108000, 0x1000, 0x00010001, 0x1100,
          0, 0xffff, 0, 1);
    param(&s, &bus, 1, 0x00109000, 0x1001, 0x00010001, 0x1101,
          0, 0xffff, 0, 1);
    assert(wr(&s, &bus, EDMA + 0x1010, 3));
    assert(memory.memory[0x100] == 0x39 && memory.memory[0x101] == 0xa7);
    assert(s.ipr == 0x300 && s.transfer_requests == 2);
    assert(cdj_c6747_edma_read(&s, EDMA + 0x1070, &v) && v == 0);
    assert(s.ipr == 0x300); /* Reading the clear alias has no side effects. */
    assert(wr(&s, &bus, EDMA + 0x1070, v | 0x100));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x1068, &v) && v == 0x200);
    assert(wr(&s, &bus, EDMA + 0x348, 0x300));
    assert(cdj_c6747_edma_read(&s, EDMA + 0x2270, &v) && v == 0);
    assert(wr(&s, &bus, EDMA + 0x2270, v | 0x100));
    assert(s.ipr == 0x200);
    assert(wr(&s, &bus, EDMA + 0x2270, 0x200));
    assert(s.ipr == 0);

    /* Constant-address modes
     * is rejected before bus or architectural state can change. */
    for (uint32_t unsupported = 1; unsupported <= 2; unsupported <<= 1) {
        cdj_c6747_edma_reset(&s); memset(&memory, 0, sizeof(memory));
        param(&s, &bus, 0, unsupported, 0x1000, 0x00010001, 0x1100,
              0, 0xffff, 0, 1);
        before = s; memory_before = memory;
        assert(!cdj_c6747_edma_write(&s, EDMA + 0x1010, 1, 4, false, &bus));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(!memcmp(&memory, &memory_before, sizeof(memory)));
    }
    return 0;
}
