"""Compile the NXS board's DSP-thread handoff (CDJ_NXS_DSP_THREAD=1) against a
pthread shim and check its lockstep, lock and HINT rules with a stub core."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]

SHIM = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cdj_dsp_audio_clock.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NANOSECONDS_PER_SECOND 1000000000LL
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define qatomic_set(p, v) __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_inc(p) __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_dec(p) __atomic_fetch_sub(p, 1, __ATOMIC_SEQ_CST)
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
static void dsp_thread_prefer_performance(void) {}
typedef pthread_mutex_t QemuMutex;
typedef pthread_cond_t QemuCond;
typedef pthread_t QemuThread;
typedef int QEMUBH;
typedef struct Notifier { void (*notify)(struct Notifier *, void *); } Notifier;
enum { QEMU_CLOCK_VIRTUAL };
static int64_t virtual_ns;
static int64_t qemu_clock_get_ns(int clock) { (void)clock; return qatomic_read(&virtual_ns); }
static int64_t get_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NANOSECONDS_PER_SECOND + ts.tv_nsec;
}
#define qemu_mutex_lock pthread_mutex_lock
#define qemu_mutex_unlock pthread_mutex_unlock
#define qemu_mutex_trylock pthread_mutex_trylock
#define qemu_cond_wait pthread_cond_wait
#define qemu_cond_signal pthread_cond_signal
#define qemu_cond_broadcast pthread_cond_broadcast
static bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return pthread_cond_timedwait(c, m, &ts) == 0;
}
static void qemu_thread_join(QemuThread *t) { pthread_join(*t, NULL); }
static pthread_mutex_t bql = PTHREAD_MUTEX_INITIALIZER;
static __thread bool bql_held;
static void bql_lock(void) { pthread_mutex_lock(&bql); bql_held = true; }
static void bql_unlock(void) { bql_held = false; pthread_mutex_unlock(&bql); }
static bool bql_locked(void) { return bql_held; }
static unsigned bh_scheduled;
static void qemu_bh_schedule(QEMUBH *bh) { (void)bh; qatomic_inc(&bh_scheduled); }
static void info_report(const char *format, ...) { (void)format; }
static void dsp_jit_report(void) {}
/* QEMU_CLOCK_VIRTUAL's run state: held while MAIN waits for the DSP. */
static bool ticks_enabled = true, vm_running = true;
static unsigned ticks_held;
static void cpu_disable_ticks(void) { if (ticks_enabled) ++ticks_held; ticks_enabled = false; }
static void cpu_enable_ticks(void) { assert(bql_locked()); ticks_enabled = true; }
static bool runstate_is_running(void) { return vm_running; }

typedef struct {
    bool dsp_started, dsp_halted;
    struct { uint64_t packets; uint32_t pc; } cpu;
    uint64_t idle_skipped_packets;
    uint64_t mailbox[2];   /* written non-atomically inside a chunk */
    bool thread_audio_clock;   /* off: tests/test_dsp_audio_clock.py */
    CdjDspAudioClock audio_clock;
    uint64_t audio_clock_next_packets;
    struct { uint64_t until; } horizon;    /* the board horizon's bound */
} NxsHpi;
static void dsp_horizon_close(NxsHpi *s) { qatomic_set(&s->horizon.until, 0); }
static uint64_t audio_clock_packets(int64_t ns) { (void)ns; return 0; }
static NxsHpi *nxs_hpi;
static void execute_dsp(NxsHpi *s, unsigned quota);
'''

STUBS = r'''
static pthread_t dsp_tid;
static unsigned hints_direct, hints_wrong_thread;
static void real_hint(void *opaque, bool high)
{
    (void)opaque; (void)high;
    if (!bql_locked()) ++hints_wrong_thread;
    ++hints_direct;
}

/* The real execute_dsp's threaded exits, with a core that tears a two-word
 * mailbox across packets and checks it never passes its pacing limit. */
static void execute_dsp(NxsHpi *s, unsigned quota)
{
    NxsDspThread *t = &dsp_thread;
    assert(pthread_equal(pthread_self(), dsp_tid));
    assert(pthread_mutex_trylock(&t->lock) != 0);   /* held by us */
    assert(quota > 0);
    uint64_t limit = MAX(dsp_thread_packets_at(qemu_clock_get_ns(0) + t->quantum_ns),
                         t->main_target);
    for (unsigned steps = 0; steps < quota; ++steps) {
        s->mailbox[0] = s->cpu.packets + 1;
        if (!(s->cpu.packets & 63)) sched_yield();
        s->mailbox[1] = ++s->cpu.packets;
        if (s->cpu.packets == 1000) dsp_thread_hint(NULL, false);
        if (qatomic_read(&t->quit) || dsp_thread_main_due(s)) {
            ++t->host_breaks;
            break;
        }
    }
    /* At most one quantum ahead of the clock it saw (which only grows),
     * unless serving a waiting MAIN. */
    assert(s->cpu.packets <= limit);
}

static volatile bool probe_bql_free, probe_ticks;
static void *bql_probe(void *arg)
{
    (void)arg;
    while (!qatomic_read(&probe_bql_free)) {
        if (pthread_mutex_trylock(&bql) == 0) {
            probe_ticks = ticks_enabled;    /* MAIN's wait: clock held */
            qatomic_set(&probe_bql_free, true);
            pthread_mutex_unlock(&bql);
        }
        sched_yield();
    }
    return NULL;
}

int main(void)
{
    static NxsHpi hpi;
    NxsDspThread *t = &dsp_thread;
    nxs_hpi = &hpi;
    t->on = true;
    t->packets_per_us = 1000;          /* one packet per virtual ns */
    t->quantum_ns = 100000;            /* 100K packets */
    t->access_packets = 50000;
    t->real_hint = real_hint;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->wake, NULL);
    pthread_cond_init(&t->progress, NULL);
    bql_lock();
    hpi.dsp_started = true;            /* as start_dsp: epoch now, 0 packets */
    pthread_create(&dsp_tid, NULL, dsp_thread_run, &hpi);
    t->thread = dsp_tid;
    bql_unlock();

    /* 1. The DSP stops exactly one quantum ahead and waits for the clock. */
    while (!qatomic_read(&t->waits)) sched_yield();
    pthread_mutex_lock(&t->lock);
    assert(hpi.cpu.packets == 100000);
    pthread_mutex_unlock(&t->lock);

    /* 2. MAIN waits (BQL released, seen by another thread) for
     * access_packets of DSP progress since its last access, even past the
     * pacing limit with the clock held, and gets the lock at that packet. */
    bql_lock();
    main_lock();
    assert(!t->main_waits);            /* 100000 >= 0 + 50000 */
    main_unlock();
    pthread_t probe;
    pthread_create(&probe, NULL, bql_probe, NULL);
    main_lock();
    assert(bql_locked());
    assert(t->main_waits == 1 && !t->main_target);
    assert(qatomic_read(&probe_bql_free));
    assert(!probe_ticks && ticks_held == 1 && ticks_enabled);
    assert(hpi.cpu.packets == 150000);
    assert(hpi.mailbox[0] == hpi.mailbox[1]);
    /* The 50K packets run for MAIN past the pacing limit moved the DSP
     * clock: the DSP is a quantum ahead again, not 50K packets further. */
    assert(t->credited_packets == 50000);
    assert(dsp_thread_packets_at(100000) == 150000);
    main_unlock();
    bql_unlock();
    pthread_join(probe, NULL);

    /* 2b. A DSP behind virtual time gives the lag up and runs one quantum
     * ahead of the new time, no further. */
    uint64_t pacing = qatomic_read(&t->waits);
    qatomic_set(&virtual_ns, 10000000);
    pthread_mutex_lock(&t->lock);
    pthread_cond_signal(&t->wake);
    pthread_mutex_unlock(&t->lock);
    while (qatomic_read(&t->waits) == pacing) sched_yield();
    pthread_mutex_lock(&t->lock);
    assert(t->slipped_ns == 10000000 - 100000);
    assert(hpi.cpu.packets == dsp_thread_packets_at(10000000 + 100000));
    assert(hpi.cpu.packets == 250000);
    pthread_mutex_unlock(&t->lock);

    /* 3. HINT: from MAIN (BQL held) at once, from the DSP thread via the BH. */
    assert(bh_scheduled == 1 && hints_direct == 0);
    bql_lock();
    main_lock();
    dsp_thread_hint(NULL, true);
    main_unlock();
    bql_unlock();
    assert(hints_direct == 1 && !hints_wrong_thread);
    dsp_thread_deliver_hint(NULL);     /* what the BH does, BQL held */
    assert(hints_direct == 2);

    /* 4. Many short MAIN accesses against a running DSP: never a torn
     * mailbox, and each one owed access_packets of DSP progress. */
    for (int i = 0; i < 300; ++i) {
        qatomic_set(&virtual_ns, virtual_ns + 997);
        bql_lock();
        main_lock();
        assert(hpi.mailbox[0] == hpi.mailbox[1]);
        uint64_t last = hpi.cpu.packets;
        main_unlock();
        bql_unlock();
        bql_lock();
        main_lock();
        assert(hpi.cpu.packets >= last + 50000);
        main_unlock();
        bql_unlock();
    }

    /* 4b. A VM stopped during the wait keeps its clock stopped. */
    bql_lock();
    vm_running = false;
    main_lock();
    assert(!ticks_enabled);
    main_unlock();
    vm_running = true;
    ticks_enabled = true;               /* what vm_start does */
    bql_unlock();

    /* 5. A halted DSP never makes MAIN wait. */
    pthread_mutex_lock(&t->lock);
    hpi.dsp_halted = true;
    pthread_mutex_unlock(&t->lock);
    uint64_t waits = t->main_waits;
    qatomic_set(&virtual_ns, virtual_ns + 50000000);
    bql_lock();
    main_lock();
    assert(t->main_waits == waits);
    main_unlock();

    /* 6. Shutdown stops and joins the thread. */
    dsp_thread_shutdown(NULL, NULL);
    bql_unlock();
    puts("ok");
    return 0;
}
'''


def section(source, start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]


def test_dsp_thread_handoff(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    source = (ROOT / 'emulator/qemu/cdj2000_nxs_hpi.c').read_text()
    code = (SHIM +
            section(source, 'typedef struct {\n    bool on, quit;', 'static void run_dsp(') +
            section(source, '/* Packets the DSP should have executed', 'static void dsp_thread_deliver_hint(') +
            'static void main_lock(void);\nstatic void main_unlock(void);\n' +
            section(source, 'static void dsp_thread_deliver_hint(', '/* Experimental independent McASP') +
            section(source, '/* MAIN side of dsp_thread.lock', 'void cdj_nxs_hpi_reset_line(') +
            STUBS)
    harness = tmp_path / 'dsp_thread.c'
    harness.write_text(code)
    binary = tmp_path / 'dsp_thread'
    subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function',
                    '-I', str(ROOT / 'emulator/qemu'), '-pthread', str(harness), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == 'ok'
