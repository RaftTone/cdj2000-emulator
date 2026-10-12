/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU TCG plugin: count the guest instructions that run inside one address
 * range, and report the running count at marker addresses.
 *
 *   -plugin rangecount.so,lo=0xADDR,hi=0xADDR[,mark=0xADDR...][,prof=FILE]
 *
 * lo and hi bound the range (lo inclusive, hi exclusive).  Each time the
 * instruction at a mark address runs, the plugin prints
 * "rangecount MARK COUNT" to stderr, where COUNT is the total so far.  The
 * difference between two marks is the cost of the code between them.  At exit
 * it prints "rangecount total COUNT".
 *
 * prof=FILE also counts the instructions per 16-byte block of the range and
 * writes "ADDRESS COUNT" lines to FILE at exit.  A translation block is
 * charged in full, on entry, to the 16-byte block of its first instruction.
 * So a block that leaves early counts too much, and a block that starts
 * outside the range is not in the profile.  The totals above are exact.
 *
 * All addresses are compared with the top three bits cleared, so the SH-4 P0,
 * P1 and P2 aliases of one physical address (0x0..., 0x8..., 0xa...) match.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_MARKS 16
#define ADDR_MASK 0x1fffffffu

static uint64_t lo, hi;
static int have_lo, have_hi;
static uint64_t marks[MAX_MARKS];
static int n_marks;
static struct qemu_plugin_scoreboard *board;
static qemu_plugin_u64 count;
static uint64_t *prof;          /* one counter per 16-byte block, or NULL */
static char prof_path[256];

static void on_tb_exec(unsigned int vcpu, void *udata)
{
    uint64_t v = (uint64_t)(uintptr_t)udata;    /* block index << 8 | insns */
    prof[v >> 8] += v & 0xff;
}

static void on_mark(unsigned int vcpu, void *udata)
{
    fprintf(stderr, "rangecount %#" PRIx64 " %" PRIu64 "\n",
            (uint64_t)(uintptr_t)udata, qemu_plugin_u64_sum(count));
}

static void on_tb(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    if (prof != NULL && n > 0 && n < 256) {
        uint64_t pc0 = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, 0))
                       & ADDR_MASK;
        if (pc0 >= lo && pc0 < hi) {
            qemu_plugin_register_vcpu_tb_exec_cb(
                tb, on_tb_exec, QEMU_PLUGIN_CB_NO_REGS,
                (void *)(uintptr_t)((((pc0 - lo) >> 4) << 8) | n));
        }
    }
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn) & ADDR_MASK;

        if (pc < lo || pc >= hi) {
            continue;
        }
        qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
            insn, QEMU_PLUGIN_INLINE_ADD_U64, count, 1);
        for (int m = 0; m < n_marks; m++) {
            if (pc == marks[m]) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, on_mark, QEMU_PLUGIN_CB_NO_REGS,
                    (void *)(uintptr_t)pc);
            }
        }
    }
}

static void at_exit(void *userdata)
{
    if (prof != NULL) {
        FILE *f = fopen(prof_path, "w");

        if (f == NULL) {
            fprintf(stderr, "rangecount: cannot write %s\n", prof_path);
        }
        for (uint64_t i = 0; f != NULL && i <= (hi - lo) / 16; i++) {
            if (prof[i]) {
                fprintf(f, "%#" PRIx64 " %" PRIu64 "\n", lo + i * 16, prof[i]);
            }
        }
        if (f != NULL) {
            fclose(f);
        }
    }
    fprintf(stderr, "rangecount total %" PRIu64 "\n",
            qemu_plugin_u64_sum(count));
}

static int bad_argument(const char *arg, const char *why)
{
    fprintf(stderr, "rangecount: %s: %s\n", arg, why);
    return -1;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        const char *v = strchr(arg, '=');
        char *end;
        uint64_t x;

        if (v == NULL) {
            return bad_argument(arg, "expected NAME=VALUE");
        }
        v++;
        if (strncmp(arg, "prof=", 5) == 0) {
            if (strlen(v) >= sizeof prof_path) {
                return bad_argument(arg, "path too long");
            }
            snprintf(prof_path, sizeof prof_path, "%s", v);
            continue;
        }
        if (strncmp(arg, "lo=", 3) && strncmp(arg, "hi=", 3) &&
            strncmp(arg, "mark=", 5)) {
            return bad_argument(arg, "unknown argument");
        }
        x = strtoull(v, &end, 0) & ADDR_MASK;
        if (*v == '\0' || *end != '\0') {
            return bad_argument(arg, "not a number");
        }
        if (arg[0] == 'l') {
            lo = x;
            have_lo = 1;
        } else if (arg[0] == 'h') {
            hi = x;
            have_hi = 1;
        } else if (n_marks == MAX_MARKS) {
            return bad_argument(arg, "too many marks");
        } else {
            marks[n_marks++] = x;
        }
    }
    if (!have_lo || !have_hi || hi <= lo) {
        return bad_argument("lo/hi", "both are required, and hi must be above lo");
    }
    if (prof_path[0]) {
        prof = calloc((hi - lo) / 16 + 1, sizeof *prof);
        if (prof == NULL) {
            return bad_argument("prof", "range too large");
        }
    }
    board = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    count = qemu_plugin_scoreboard_u64(board);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
