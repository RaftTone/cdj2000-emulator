/* SPDX-License-Identifier: GPL-2.0-or-later
 * C67x+ lint: find words in a CDJ-2000 DSP image that the C6727 (C67x+) does
 * not have, by the same rule the emulator's core traps on
 * (c66x_insn_in_isa with C66X_ISA_C67XP, emulator/c6x/c66x_decode.c).
 *
 *   c67x-lint [--stock STOCK] [--skip LO:HI]... IMAGE BASE  a raw image at BASE
 *   c67x-lint [--stock STOCK] [--skip LO:HI]... --app APP.BIN
 *             images A and B of a MAIN app.bin (main-unpacked.bin layout)
 *
 * Every 32-bit word is decoded as an instruction. A hit is a word that some
 * C6000 ISA defines but the C67x+ does not: a C64x/C64x+/C674x/C66x opcode, a
 * header-based (compact) fetch packet, an MVC to a control register the chip
 * lacks. Words no ISA defines (data, sentinels) are counted, not reported: an
 * assembler never emits them. With --stock (an image or app.bin of the same
 * kind), only words that differ from the stock one at the same address are
 * checked, so data the stock firmware carries does not show. --skip leaves
 * out the DSP addresses [LO, HI), for data tables in new code (a table of
 * small integers reads as C64x packed ops). Exit status: 0
 * clean, 1 hits, 2 usage or input error.
 *
 * The images in an app.bin are found the way MAIN loads them (4.33): image A's
 * header pointer is the literal at 0x1C79CC (header 16 bytes, load address in
 * its word 2, data after it), its length the literal at 0x2B0D84; image B's
 * data pointer is at 0x1C79FC, its length at 0x2B0D88, loaded at 0x80000000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c66x.h"
#include "c66x_decode.h"

typedef struct image {
    const char *name;
    const uint8_t *data;
    uint32_t base, len;
} image;

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(n > 0 ? n : 1);
    if (!p || fread(p, 1, n, f) != (size_t)n) {
        fprintf(stderr, "%s: read failed\n", path);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = n;
    return p;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fp_read(void *opaque, uint32_t fp_addr, uint8_t fp[32])
{
    const image *im = opaque;
    memset(fp, 0, 32);
    for (unsigned i = 0; i < 32; i++)
        if (fp_addr + i - im->base < im->len)
            fp[i] = im->data[fp_addr + i - im->base];
    return 0;
}

/* Images A and B of an app.bin; 0 on success. */
static int app_images(const char *path, const uint8_t *app, size_t len, image out[2])
{
    if (len < 0x2B0D8C) {
        fprintf(stderr, "%s: too short for a MAIN app.bin\n", path);
        return -1;
    }
    uint32_t a_hdr = le32(app + 0x1C79CC) & 0xFFFFFF, a_len = le32(app + 0x2B0D84);
    uint32_t b_off = le32(app + 0x1C79FC) & 0xFFFFFF, b_len = le32(app + 0x2B0D88);
    if ((uint64_t)a_hdr + 16 + a_len > len || (uint64_t)b_off + b_len > len) {
        fprintf(stderr, "%s: image pointers out of the file (A 0x%x+0x%x, B 0x%x+0x%x)\n",
                path, a_hdr, a_len, b_off, b_len);
        return -1;
    }
    out[0] = (image){ "A", app + a_hdr + 16, le32(app + a_hdr + 8), a_len };
    out[1] = (image){ "B", app + b_off, 0x80000000u, b_len };
    return 0;
}

#define MAX_SKIP 32
static uint32_t skip_lo[MAX_SKIP], skip_hi[MAX_SKIP];
static unsigned nskip;

static int skipped(uint32_t addr)
{
    for (unsigned i = 0; i < nskip; i++)
        if (addr >= skip_lo[i] && addr < skip_hi[i])
            return 1;
    return 0;
}

/* Hits in im (only where it differs from stock, if given). */
static unsigned lint(const image *im, const image *stock)
{
    unsigned hits = 0, undefined = 0, checked = 0;
    for (uint32_t off = 0; off + 4 <= im->len; off += 4) {
        uint32_t addr = im->base + off;
        uint32_t word = le32(im->data + off);
        if (stock && addr - stock->base + 4 <= stock->len
            && le32(stock->data + (addr - stock->base)) == word)
            continue;
        if (skipped(addr))
            continue;
        checked++;
        c66x_insn in;
        char text[96];
        c66x_decode(fp_read, (void *)im, addr, &in, text, sizeof text);
        if (in.opc == -1 && !in.compact) {
            undefined++;
            continue;
        }
        if (c66x_insn_in_isa(&in, C66X_ISA_C67XP))
            continue;
        printf("image %s 0x%08x: 0x%08x %s%s\n", im->name, addr, word,
               in.compact ? "(compact fetch packet) " : "", text);
        hits++;
    }
    printf("image %s: 0x%08x..0x%08x, %u words checked, %u not C67x+, %u undefined (data)\n",
           im->name, im->base, im->base + im->len, checked, hits, undefined);
    return hits;
}

int main(int argc, char **argv)
{
    const char *stock_path = NULL;
    int i = 1;
    for (; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--stock")) {
            stock_path = argv[i + 1];
        } else if (!strcmp(argv[i], "--skip") && nskip < MAX_SKIP) {
            char *end;
            skip_lo[nskip] = strtoul(argv[i + 1], &end, 0);
            if (*end != ':')
                break;
            skip_hi[nskip++] = strtoul(end + 1, NULL, 0);
        } else {
            break;
        }
    }
    int app = i < argc && !strcmp(argv[i], "--app");
    if (argc != i + 2) {
        fprintf(stderr, "usage: c67x-lint [--stock STOCK] [--skip LO:HI]... IMAGE BASE\n"
                        "       c67x-lint [--stock STOCK] [--skip LO:HI]... --app APP.BIN\n");
        return 2;
    }
    const char *path = argv[i + app];
    size_t len, slen = 0;
    uint8_t *data = slurp(path, &len), *sdata = NULL;
    if (!data || (stock_path && !(sdata = slurp(stock_path, &slen))))
        return 2;

    image im[2], st[2];
    unsigned n = app ? 2 : 1;
    if (app) {
        if (app_images(path, data, len, im) || (sdata && app_images(stock_path, sdata, slen, st)))
            return 2;
    } else {
        uint32_t base = strtoul(argv[i + 1], NULL, 0);
        im[0] = (image){ "raw", data, base, (uint32_t)len };
        st[0] = (image){ "raw", sdata, base, (uint32_t)slen };
    }
    unsigned hits = 0;
    for (unsigned k = 0; k < n; k++)
        hits += lint(&im[k], sdata ? &st[k] : NULL);
    return hits ? 1 : 0;
}
