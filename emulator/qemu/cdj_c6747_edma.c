/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_edma.h"

#include <stdlib.h>
#include <string.h>

#define OPT_ITCCHEN (1u << 23)
#define OPT_TCCHEN  (1u << 22)
#define OPT_ITCINTEN (1u << 21)
#define OPT_TCINTEN (1u << 20)
#define OPT_STATIC (1u << 3)
#define OPT_SYNCDIM (1u << 2)
#define OPT_DAM (1u << 1)
#define OPT_SAM (1u << 0)
#define OPT_ALLOWED 0x00f3ff0fu
#define SERVICE_LIMIT 1048576u

typedef enum { EVENT_DMA, EVENT_QDMA } EventKind;
typedef struct { EventKind kind; unsigned channel; } PendingEvent;
typedef struct {
    PendingEvent *items;
    unsigned head, tail, capacity;
} PendingQueue;

static bool locate(uint32_t address, uint32_t *offset)
{
    if (address < CDJ_C6747_EDMA_BASE ||
        address >= CDJ_C6747_EDMA_BASE + 0x8000u || (address & 3u))
        return false;
    *offset = address - CDJ_C6747_EDMA_BASE;
    return true;
}

bool cdj_c6747_edma_write_mapped(uint32_t address, unsigned size)
{
    uint32_t offset;
    return (size == 4 || size == 8) && locate(address, &offset);
}

static bool legal_opt(uint32_t value)
{
    unsigned tcc = (value >> 12) & 0x3fu;
    unsigned fwid = (value >> 8) & 7u;
    return !(value & ~OPT_ALLOWED) && tcc < CDJ_C6747_EDMA_CHANNELS &&
           fwid <= 5;
}

static bool legal_queue_map(uint32_t value)
{
    /* C6747 implements two event queues; each channel field is 3 bits with
     * only values zero and one defined. */
    return !(value & ~0x11111111u);
}

static bool legal_qchmap(uint32_t value)
{
    return !(value & ~0x0ffcu) && ((value >> 5) & 0x1ffu) < 128;
}

static bool valid_param(const uint32_t p[8])
{
    if (!legal_opt(p[0]) || (p[0] & (OPT_DAM | OPT_SAM)) ||
        !(p[2] & 0xffffu) || !(p[2] >> 16) || !(p[7] & 0xffffu) ||
        (p[7] & 0xffff0000u))
        return false;
    uint16_t link = p[5];
    if (link != 0xffffu) {
        uint32_t offset = link & 0x3fffu;
        if ((offset & 31u) || offset >= CDJ_C6747_EDMA_PARAMS * 32u)
            return false;
    }
    return true;
}

static bool transfer_bytes(const uint32_t p[8], const CdjC6747EdmaBus *bus,
                           bool commit)
{
    size_t count = p[2] & 0xffffu;
    if (!bus || !bus->read || !bus->write || !valid_param(p)) return false;
    uint8_t *bytes = malloc(count);
    if (!bytes) return false;
    /* SPRUH91D 16.2.2.2: AB synchronization transfers a whole frame
     * per event. B indices separate arrays; C indices separate frames. */
    unsigned arrays = (p[0] & OPT_SYNCDIM) ? p[2] >> 16 : 1;
    uint32_t src = p[1], dst = p[3];
    bool ok = true;
    for (unsigned i = 0; i < arrays && ok; ++i) {
        ok = bus->read(bus->opaque, src, bytes, count) &&
             bus->write(bus->opaque, dst, bytes, count, false);
        if (ok && commit)
            ok = bus->write(bus->opaque, dst, bytes, count, true);
        src += (int32_t)(int16_t)p[4];
        dst += (int32_t)(int16_t)(p[4] >> 16);
    }
    free(bytes);
    return ok;
}

static bool enqueue(PendingQueue *queue, EventKind kind, unsigned channel)
{
    if (queue->tail >= SERVICE_LIMIT) return false;
    if (queue->tail == queue->capacity) {
        unsigned capacity = queue->capacity ? queue->capacity * 2u : 16u;
        if (capacity > SERVICE_LIMIT) capacity = SERVICE_LIMIT;
        PendingEvent *items = realloc(queue->items,
                                      sizeof(*items) * capacity);
        if (!items) return false;
        queue->items = items; queue->capacity = capacity;
    }
    queue->items[queue->tail++] = (PendingEvent){kind, channel};
    return true;
}

static bool complete_one(CdjC6747Edma *s, EventKind kind, unsigned channel,
                         const CdjC6747EdmaBus *bus, bool commit,
                         PendingQueue *queue)
{
    unsigned set = kind == EVENT_DMA ? channel :
                   ((s->qchmap[channel] >> 5) & 0x1ffu);
    if (set >= CDJ_C6747_EDMA_PARAMS) return false;
    uint32_t original[8];
    memcpy(original, s->param[set], sizeof(original));
    if (!transfer_bytes(original, bus, commit)) return false;

    uint32_t opt = original[0];
    unsigned bcnt = original[2] >> 16;
    unsigned ccnt = original[7] & 0xffffu;
    bool ab_sync = (opt & OPT_SYNCDIM) != 0;
    bool final = (ab_sync || bcnt == 1) && ccnt == 1;
    unsigned tcc = (opt >> 12) & 0x3fu;

    if (kind == EVENT_DMA) {
        uint32_t bit = 1u << channel;
        s->er &= ~bit; s->esr &= ~bit; s->cer &= ~bit; s->ser &= ~bit;
    } else {
        s->qer &= ~(1u << channel); s->qser &= ~(1u << channel);
    }

    /* SPRUH91D Table 16-3 footnote: STATIC suppresses every PaRAM update,
     * including ordinary B/C updates as well as the final link reload. */
    if (!(opt & OPT_STATIC)) {
        if (!final) {
            int16_t src_index, dst_index;
            if (ab_sync) {
                src_index = (int16_t)(original[6] & 0xffffu);
                dst_index = (int16_t)(original[6] >> 16);
                s->param[set][7] = ccnt - 1u;
            } else if (bcnt > 1) {
                src_index = (int16_t)(original[4] & 0xffffu);
                dst_index = (int16_t)(original[4] >> 16);
                s->param[set][2] = (original[2] & 0xffffu) |
                                   ((bcnt - 1u) << 16);
            } else {
                src_index = (int16_t)(original[6] & 0xffffu);
                dst_index = (int16_t)(original[6] >> 16);
                s->param[set][2] = (original[2] & 0xffffu) |
                                   (original[5] & 0xffff0000u);
                s->param[set][7] = ccnt - 1u;
            }
            s->param[set][1] = original[1] + (int32_t)src_index;
            s->param[set][3] = original[3] + (int32_t)dst_index;
        } else {
            uint16_t link = original[5];
            if (link == 0xffffu) {
                memset(s->param[set], 0, sizeof(s->param[set]));
                s->param[set][5] = 0xffffu;
            } else {
                unsigned linked_set = (link & 0x3fffu) / 32u;
                uint32_t linked[8];
                memcpy(linked, s->param[linked_set], sizeof(linked));
                memcpy(s->param[set], linked, sizeof(linked));
                uint8_t qmatches[CDJ_C6747_EDMA_QCHANNELS];
                /* A link writes the complete set.  Test each enabled QDMA's
                 * own trigger word, rather than assuming QDMA0's TRWORD. */
                unsigned n = 0;
                for (unsigned q = 0; q < CDJ_C6747_EDMA_QCHANNELS; ++q)
                    if ((s->qeer & (1u << q)) &&
                        ((s->qchmap[q] >> 5) & 0x1ffu) == set)
                        qmatches[n++] = q;
                for (unsigned i = 0; i < n; ++i) {
                    unsigned q = qmatches[i];
                    uint32_t bit = 1u << q;
                    if (s->qer & bit) s->qemr |= bit;
                    else s->qer |= bit;
                    if (!enqueue(queue, EVENT_QDMA, q)) return false;
                }
            }
        }
    }

    bool interrupt = final ? (opt & OPT_TCINTEN) : (opt & OPT_ITCINTEN);
    bool chain = final ? (opt & OPT_TCCHEN) : (opt & OPT_ITCCHEN);
    if (interrupt) {
        bool pending[CDJ_C6747_EDMA_REGIONS];
        for (unsigned r = 0; r < CDJ_C6747_EDMA_REGIONS; ++r)
            pending[r] = (s->ipr & s->ier & s->drae[r]) != 0;
        s->ipr |= 1u << tcc;
        /* The region output is an interrupt pulse, not a level to be sampled
         * after arbitrary register writes.  A completion notifies a region
         * only when it makes that region newly pending.  Firmware can request
         * another pulse for uncleared IPR state through IEVAL. */
        for (unsigned r = 0; r < CDJ_C6747_EDMA_REGIONS; ++r)
            if (!pending[r] && (s->ipr & s->ier & s->drae[r]))
                s->irq_notifications |= 1u << r;
    }
    if (chain) {
        uint32_t bit = 1u << tcc;
        if (s->cer & bit) s->emr |= bit;
        else s->cer |= bit;
        if (!enqueue(queue, EVENT_DMA, tcc)) return false;
    }
    ++s->transfer_requests;
    s->bytes_transferred += (uint64_t)(original[2] & 0xffffu) *
                            (ab_sync ? bcnt : 1u);
    return true;
}

static bool service(CdjC6747Edma *s, const PendingEvent *initial,
                    unsigned initial_count, const CdjC6747EdmaBus *bus,
                    bool commit)
{
    PendingQueue queue = {0};
    for (unsigned i = 0; i < initial_count; ++i)
        if (!enqueue(&queue, initial[i].kind, initial[i].channel)) {
            free(queue.items); return false;
        }
    bool ok = true;
    while (queue.head < queue.tail) {
        PendingEvent event = queue.items[queue.head++];
        if (!complete_one(s, event.kind, event.channel, bus, commit,
                          &queue)) {
            ok = false;
            break;
        }
    }
    free(queue.items);
    return ok;
}

void cdj_c6747_edma_reset(CdjC6747Edma *s)
{
    *s = (CdjC6747Edma){0};
}

bool cdj_c6747_edma_valid(const CdjC6747Edma *s)
{
    for (unsigned q = 0; q < CDJ_C6747_EDMA_QCHANNELS; ++q)
        if (!legal_qchmap(s->qchmap[q])) return false;
    for (unsigned n = 0; n < 4; ++n)
        if (!legal_queue_map(s->dmaqnum[n])) return false;
    if (!legal_queue_map(s->qdmaqnum) || (s->qwmthra & ~0x1f1fu) ||
        (s->qer & ~0xffu) || (s->qeer & ~0xffu) ||
        (s->qser & ~0xffu) || (s->qemr & ~0xffu) ||
        (s->irq_notifications & ~((1u << CDJ_C6747_EDMA_REGIONS) - 1u)) ||
        (s->ccerr & ~0x00030003u)) return false;
    for (unsigned r = 0; r < CDJ_C6747_EDMA_REGIONS; ++r)
        if (s->qrae[r] & ~0xffu) return false;
    for (unsigned p = 0; p < CDJ_C6747_EDMA_PARAMS; ++p)
        if (!legal_opt(s->param[p][0]) ||
            (s->param[p][7] & 0xffff0000u)) return false;
    return true;
}

static bool channel_view(const CdjC6747Edma *s, uint32_t offset,
                         uint32_t *local, uint32_t *dma_mask,
                         uint32_t *qdma_mask, int *region)
{
    if (offset >= 0x1000u && offset <= 0x1094u) {
        *local = offset - 0x1000u; *dma_mask = UINT32_MAX;
        *qdma_mask = 0xffu; *region = -1; return true;
    }
    if (offset >= 0x2000u && offset < 0x2800u) {
        unsigned r = (offset - 0x2000u) / 0x200u;
        uint32_t within = (offset - 0x2000u) % 0x200u;
        if (r >= CDJ_C6747_EDMA_REGIONS || within > 0x94u) return false;
        *local = within; *dma_mask = s->drae[r];
        *qdma_mask = s->qrae[r] & 0xffu; *region = (int)r; return true;
    }
    return false;
}

bool cdj_c6747_edma_read(const CdjC6747Edma *s, uint32_t address,
                         uint32_t *value)
{
    uint32_t o;
    if (!locate(address, &o)) return false;
    if (o >= 0x4000u && o < 0x5000u) {
        unsigned index = (o - 0x4000u) / 32u;
        unsigned word = ((o - 0x4000u) % 32u) / 4u;
        *value = s->param[index][word]; return true;
    }
    if (o == 0x000u) { *value = 0x40015300u; return true; }
    if (o == 0x004u) { *value = 0x00213344u; return true; }
    if (o >= 0x200u && o <= 0x21cu) {
        *value = s->qchmap[(o - 0x200u) / 4u]; return true;
    }
    if (o >= 0x240u && o <= 0x24cu) {
        *value = s->dmaqnum[(o - 0x240u) / 4u]; return true;
    }
    if (o == 0x260u) { *value = s->qdmaqnum; return true; }
    /* The recovered firmware's generic register helpers read the write-only
     * clear aliases before ORing in their clear mask.  Return the underlying
     * status for those aliases; this is a deliberate software-compatibility
     * readback, not a claim that physical silicon exposes them. */
    if (o == 0x300u || o == 0x308u) { *value = s->emr; return true; }
    if (o == 0x310u || o == 0x314u) { *value = s->qemr; return true; }
    if (o == 0x318u || o == 0x31cu) { *value = s->ccerr; return true; }
    if (o >= 0x340u && o <= 0x358u && !((o - 0x340u) & 7u)) {
        *value = s->drae[(o - 0x340u) / 8u]; return true;
    }
    if (o >= 0x380u && o <= 0x38cu) {
        *value = s->qrae[(o - 0x380u) / 4u]; return true;
    }
    if (o == 0x600u || o == 0x604u || o == 0x640u) {
        *value = 0; return true; /* all work completes synchronously */
    }
    if (o == 0x620u) { *value = s->qwmthra; return true; }

    uint32_t local, dmask, qmask; int region;
    if (!channel_view(s, o, &local, &dmask, &qmask, &region)) return false;
    (void)region;
    switch (local) {
    case 0x00: *value = s->er & dmask; return true;
    case 0x08: *value = s->er & dmask; return true;
    case 0x10: *value = s->esr & dmask; return true;
    case 0x18: *value = s->cer & dmask; return true;
    case 0x20: case 0x28: case 0x30: *value = s->eer & dmask; return true;
    case 0x38: *value = s->ser & dmask; return true;
    case 0x40: *value = s->ser & dmask; return true;
    case 0x50: case 0x58: case 0x60: *value = s->ier & dmask; return true;
    case 0x68: *value = s->ipr & dmask; return true;
    /* SPRUH91D 16.4.2.6.5 defines ICR as write-only (W-0), not an
     * alias of IPR.  Use zero for the firmware's read/OR/write helper:
     * reflecting IPR here would acknowledge unrelated completions too.
     * Read-zero is our compatibility choice for this write-only register;
     * the manual does not specify its read value. Applies to shadow ICRs. */
    case 0x70: *value = 0; return true;
    case 0x80: *value = s->qer & qmask; return true;
    case 0x84: case 0x88: case 0x8c: *value = s->qeer & qmask; return true;
    case 0x90: *value = s->qser & qmask; return true;
    case 0x94: *value = s->qser & qmask; return true;
    default: return false; /* IEVAL is write-only */
    }
}

static bool write_channel(CdjC6747Edma *s, uint32_t local, uint32_t dmask,
                          uint32_t qmask, int region, uint32_t value, bool commit,
                          const CdjC6747EdmaBus *bus)
{
    uint32_t d = value & dmask, q = value & qmask;
    PendingEvent events[CDJ_C6747_EDMA_CHANNELS];
    unsigned count = 0;
    switch (local) {
    case 0x08: if (commit) s->er &= ~d; return true;
    case 0x10:
        for (unsigned ch = 0; ch < CDJ_C6747_EDMA_CHANNELS; ++ch)
            if (d & (1u << ch)) events[count++] = (PendingEvent){EVENT_DMA, ch};
        {
            CdjC6747Edma preview = *s; preview.esr |= d;
            if (!service(&preview, events, count, bus, false)) return false;
        }
        if (!commit) return true;
        s->esr |= d;
        return service(s, events, count, bus, true);
    case 0x28: if (commit) s->eer &= ~d; return true;
    case 0x30:
        /* A previously latched external event is serviced on enable. */
        d &= s->er;
        count = 0;
        for (unsigned ch = 0; ch < CDJ_C6747_EDMA_CHANNELS; ++ch)
            if (d & (1u << ch)) events[count++] = (PendingEvent){EVENT_DMA, ch};
        if (count) {
            CdjC6747Edma preview = *s; preview.eer |= value & dmask;
            if (!service(&preview, events, count, bus, false)) return false;
        }
        if (!commit) return true;
        s->eer |= value & dmask;
        return count ? service(s, events, count, bus, true) : true;
    case 0x40: if (commit) s->ser &= ~d; return true;
    case 0x58: if (commit) s->ier &= ~d; return true;
    case 0x60: if (commit) s->ier |= d; return true;
    case 0x70: if (commit) s->ipr &= ~d; return true;
    case 0x78:
        if (value > 1u) return false;
        if (commit && value && region >= 0 &&
            (s->ipr & s->ier & s->drae[region]))
            s->irq_notifications |= 1u << region;
        return true;
    case 0x88: if (commit) s->qeer &= ~q; return !(value & ~0xffu);
    case 0x8c: if (commit) s->qeer |= q; return !(value & ~0xffu);
    case 0x94: if (commit) s->qser &= ~q; return !(value & ~0xffu);
    default: return false;
    }
}

bool cdj_c6747_edma_write(CdjC6747Edma *s, uint32_t address, uint64_t value,
                          unsigned size, bool commit,
                          const CdjC6747EdmaBus *bus)
{
    uint32_t o;
    if ((size != 4 && size != 8) || !locate(address, &o)) return false;
    if (size == 8 && (o < 0x4000u || o >= 0x5000u ||
                      (o & 31u) > 24u)) return false;
    if (size == 4 && value > UINT32_MAX) return false;
    uint32_t v = value;
    if (o >= 0x4000u && o < 0x5000u) {
        unsigned set = (o - 0x4000u) / 32u;
        unsigned word = ((o - 0x4000u) % 32u) / 4u;
        uint32_t words[2] = {(uint32_t)value, (uint32_t)(value >> 32)};
        unsigned word_count = size / 4u;
        for (unsigned i = 0; i < word_count; ++i) {
            unsigned w = word + i;
            if ((w == 0 && !legal_opt(words[i])) ||
                (w == 7 && (words[i] & 0xffff0000u))) return false;
        }
        uint8_t matches[CDJ_C6747_EDMA_QCHANNELS];
        CdjC6747Edma preview = *s;
        for (unsigned i = 0; i < word_count; ++i)
            preview.param[set][word + i] = words[i];
        unsigned n = 0;
        for (unsigned q = 0; q < CDJ_C6747_EDMA_QCHANNELS; ++q) {
            unsigned mapped_set = (preview.qchmap[q] >> 5) & 0x1ffu;
            unsigned trigger = (preview.qchmap[q] >> 2) & 7u;
            if ((preview.qeer & (1u << q)) && mapped_set == set &&
                trigger >= word && trigger < word + word_count)
                matches[n++] = q;
        }
        PendingEvent events[CDJ_C6747_EDMA_QCHANNELS];
        for (unsigned i = 0; i < n; ++i)
            events[i] = (PendingEvent){EVENT_QDMA, matches[i]};
        if (n && !service(&preview, events, n, bus, false)) return false;
        if (!commit) return true;
        for (unsigned i = 0; i < word_count; ++i)
            s->param[set][word + i] = words[i];
        for (unsigned i = 0; i < n; ++i) s->qer |= 1u << matches[i];
        return n ? service(s, events, n, bus, true) : true;
    }
    if (o >= 0x200u && o <= 0x21cu) {
        if (!legal_qchmap(v)) return false;
        if (commit) s->qchmap[(o - 0x200u) / 4u] = v;
        return true;
    }
    if (o >= 0x240u && o <= 0x24cu) {
        if (!legal_queue_map(v)) return false;
        if (commit) s->dmaqnum[(o - 0x240u) / 4u] = v;
        return true;
    }
    if (o == 0x260u) {
        if (!legal_queue_map(v)) return false;
        if (commit) s->qdmaqnum = v; return true;
    }
    if (o == 0x308u) { if (commit) s->emr &= ~v; return true; }
    if (o == 0x314u) {
        if (v & ~0xffu) return false;
        if (commit) s->qemr &= ~v; return true;
    }
    if (o == 0x31cu) {
        if (v & ~0x00030003u) return false;
        if (commit) s->ccerr &= ~v; return true;
    }
    if (o == 0x320u) return v <= 1u;
    if (o >= 0x340u && o <= 0x358u && !((o - 0x340u) & 7u)) {
        if (commit) s->drae[(o - 0x340u) / 8u] = v; return true;
    }
    if (o >= 0x380u && o <= 0x38cu) {
        if (v & ~0xffu) return false;
        if (commit) s->qrae[(o - 0x380u) / 4u] = v; return true;
    }
    if (o == 0x620u) {
        if (v & ~0x1f1fu) return false;
        if (commit) s->qwmthra = v; return true;
    }
    uint32_t local, dmask, qmask; int region;
    if (channel_view(s, o, &local, &dmask, &qmask, &region))
        return write_channel(s, local, dmask, qmask, region, v, commit, bus);
    return false;
}

bool cdj_c6747_edma_event(CdjC6747Edma *s, unsigned channel,
                          const CdjC6747EdmaBus *bus)
{
    if (channel >= CDJ_C6747_EDMA_CHANNELS) return false;
    uint32_t bit = 1u << channel;
    if (!(s->eer & bit)) { s->er |= bit; return true; }
    PendingEvent event = {EVENT_DMA, channel};
    CdjC6747Edma preview = *s;
    if (preview.er & bit) preview.emr |= bit;
    preview.er |= bit;
    if (!service(&preview, &event, 1, bus, false)) return false;
    if (s->er & bit) s->emr |= bit;
    s->er |= bit;
    return service(s, &event, 1, bus, true);
}

bool cdj_c6747_edma_irq_pending(const CdjC6747Edma *s, unsigned region)
{
    return region < CDJ_C6747_EDMA_REGIONS &&
           (s->ipr & s->ier & s->drae[region]) != 0;
}

bool cdj_c6747_edma_take_irq_notification(CdjC6747Edma *s, unsigned region)
{
    if (region >= CDJ_C6747_EDMA_REGIONS ||
        !(s->irq_notifications & (1u << region))) return false;
    s->irq_notifications &= ~(1u << region);
    return true;
}
