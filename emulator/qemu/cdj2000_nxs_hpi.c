/* SPDX-License-Identifier: GPL-2.0-or-later
 * NXS MAIN-facing TI UHPI transport with a partial C674x execution core.
 * Matches the independently verified NXS upload path: byte-addressed global
 * L2, HWOB=1, HPID auto-increment and fixed-address accesses.
 * The optional DSP thread (CDJ_NXS_DSP_THREAD) follows the lockstep design of
 * Stijn Jacobs' cdj-nxs2-qemu (GPL-2.0-or-later); see THIRD_PARTY.md.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "system/runstate.h"
#include "cdj2000_nxs_hpi.h"
#include "cdj_c674x.h"
#include "cdj_c6747_syscfg.h"
#include "cdj_c6747_psc.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_gpio.h"
#include "cdj_c6747_i2c.h"
#include "cdj_c6747_intc.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_timer.h"
#include "cdj_c6747_spi.h"
#include "cdj_c6747_spi_clock.h"
#include "cdj_c6747_cache.h"
#include "cdj_c6747_edma.h"
#include "cdj_c6747_hpi.h"
#include "cdj_c6747_emifb.h"
#include "cdj_dsp_checkpoint.h"
#include "cdj_dsp_budget.h"
#include "cdj_dsp_scheduler.h"

#define HPI_BASE 0x0c000000u
#define L2_BASE 0x11800000u
#define L2_SIZE 0x40000u
#define SHARED_RAM_BASE 0x80000000u
#define SHARED_RAM_SIZE 0x20000u
#define SDRAM_BASE 0xc0000000u
#define SDRAM_SIZE 0x02000000u
#define DSP_FAULT_HISTORY_COUNT 4096u
#define MCASP_VIRTUAL_BATCH_NS 1000000
#define MCASP_VIRTUAL_MAX_SLOTS 256u
#define MCASP_VIRTUAL_DSP_QUOTA 4096u
#define MCASP_VIRTUAL_REPORT_NS 256000000
#define NXS_AUDIO_RATE 44100u
#define NXS_AUDIO_RING_FRAMES (NXS_AUDIO_RATE * 2u)
#define NXS_AUDIO_PREFILL_FRAMES (NXS_AUDIO_RATE / 20u)
/* Cooperative QEMU scheduling quantum, not a C6747 timing property. HINT
 * still yields immediately. One million packets lets initialization reach
 * its genuine wait loop after the final MAIN event instead of stranding the
 * DSP merely because no later host transition happens to resume it. */

typedef struct {
    uint64_t packets, cycles;
    uint32_t pc, a8, b5, b15, b3, csr, irp, ilc, tsr, itsr;
    uint8_t phase, loop_active;
} DspFaultHistory;

typedef struct {
    MemoryRegion registers;
    uint8_t l2[L2_SIZE];
    uint8_t l1d[CDJ_DSP_L1D_SIZE];
    uint32_t address;
    CdjC6747Hpi hpi;
    bool reset_released, dsp_started, dsp_halted, dsp_running;
    bool functional_audio, virtual_audio_clock, cycle_audio_clock;
    uint32_t legacy_budget;
    CdjDspScheduler scheduler;
    QEMUTimer *dsp_timer;
    QEMUTimer *mcasp_timer;
    int64_t mcasp_last_ns;
    uint64_t mcasp_phase, mcasp_debt;
    uint64_t mcasp_next_cycle, mcasp_cycle_period;
    int64_t mcasp_last_report_ns;
    bool mcasp_have_report;
    AudioBackend *audio_backend;
    SWVoiceOut *audio_voice;
    Notifier audio_shutdown;
    QemuMutex audio_lock;
    int16_t *audio_ring;
    uint32_t audio_rd, audio_wr, audio_fill;
    int16_t audio_left;
    bool audio_have_left, audio_priming;
    uint64_t audio_in, audio_out, audio_underruns, audio_dropped;
    FILE *pcm_wav;
    Notifier pcm_shutdown;
    uint64_t pcm_frames, pcm_nonzero_frames;
    int16_t pcm_left;
    bool pcm_have_left, pcm_failed;
    unsigned boot_phase;
    uint64_t words;
    uint64_t event_sequence, checkpoint_sequence;
    int64_t checkpoint_check_ns; /* Host-only diagnostic polling throttle. */
    CdjC674x cpu;
    CdjC6747Syscfg syscfg;
    CdjC6747Psc psc;
    CdjC6747Mcasp mcasp;
    CdjC6747McaspControl mcasp_control;
    CdjC6747Gpio gpio;
    CdjC6747I2c i2c;
    CdjC6747Intc intc;
    CdjC6747IntcDelivery intc_delivery;
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT];
    CdjWm8740 wm8740;
    CdjC6747SpiTransfer spi_transfer;
    CdjC6747Cache cache;
    CdjC6747Edma edma;
    CdjC6747SyscfgPriority syscfg_priority;
    CdjC6747Pll pll;
    CdjC6747Emifb emifb;
    uint8_t *shared_ram;
    uint8_t *sdram;
    void (*hint)(void *, bool);
    void *opaque;
    FILE *event_log;
    FILE *tx_capture;
    char *tx_capture_path;
    uint64_t tx_capture_sequence;
    uint64_t tx_capture_limit;
    bool tx_capture_nonzero_only;
    bool tx_capture_failed;
    DspFaultHistory fault_history[DSP_FAULT_HISTORY_COUNT];
    uint32_t fault_history_next;
    char *fault_history_path;
    /* Opt-in idle-loop skip (CDJ_NXS_DSP_IDLE_SKIP=1); see dsp_idle_repeat.
     * Host-side bookkeeping only: never checkpointed. */
    /* CDJ_NXS_DSP_MODEL=1: answer MAIN without executing the C674x. */
    bool model;
    int64_t model_ns;           /* virtual time the position last advanced */
    uint64_t model_audio_ns;    /* audio time played since the stream started */
    uint64_t model_samples;     /* the same in 44.1 kHz samples */
    uint32_t model_received[2]; /* frames announced per stream buffer */
    uint32_t model_total[2];    /* +0x1c frames of each buffer's stream */
    uint32_t model_record, model_length; /* the last command, until its first header */
    bool model_pending;
    bool idle_skip, idle_dirty, idle_anchor_valid;
    unsigned idle_anchor_step;
    uint32_t idle_anchor_pc;
    uint32_t idle_anchor_r[2][32], idle_anchor_control[32];
    uint64_t idle_anchor_ready[32], idle_anchor_cycles, idle_anchor_packets;
    uint64_t idle_skipped_packets;

    /* RAM words written since the anchor, with their anchor-time values. */
    unsigned idle_log_count;
    uint32_t idle_log_address[64], idle_log_value[64];
} NxsHpi;
static NxsHpi *nxs_hpi;

/*
 * CDJ_NXS_DSP_THREAD=1: the C674x runs on its own host thread instead of
 * synchronously inside MAIN's HPI MMIO write.  Its clock is packets at
 * CDJ_NXS_DSP_THREAD_MPPS million per virtual second, and it never runs more
 * than a quantum ahead of QEMU_CLOCK_VIRTUAL (an idle-skipping DSP waits for
 * MAIN; so does one in a paused VM).  A DSP behind virtual time gives the lag up
 * ("slip"): the interpreter is several times slower than the C6747, and making
 * MAIN wait for real-time parity starves it worse than the synchronous mode
 * (measured: no Not Loaded within 280 s).  Instead MAIN, whenever it touches
 * the DSP (HPI access, reset, boot phase), first waits with the BQL released
 * until the DSP has executed CDJ_NXS_DSP_THREAD_ACCESS_PACKETS packets since
 * MAIN's previous access, so a MAIN poll loop cannot outrun the DSP it polls
 * (without it MAIN's 3,000-read ready poll saw 13 DSP packets per read and
 * raised E-7010).  Pattern after
 * Stijn Jacobs' cdj-nxs2-qemu (github.com/Stijn-Jacobs/cdj-nxs2-qemu @ 08d5cb1,
 * dsp_host.c and nxs2 dsp_c6x.c CDJ_C6X_THREAD=2), with the author's
 * permission.
 *
 * Synchronisation.  Every NxsHpi field is owned by @lock: the DSP thread holds
 * it for a whole chunk (packet boundaries only), and MAIN's entry points
 * (hpi_read/hpi_write/reset_line/boot_phase) hold it for their whole body.  So
 * MAIN never sees a half-executed packet, a half-committed EDMA transaction or
 * a torn L2/mailbox word, and the DSP sees each HPI access, DSPINT (INTC event
 * 34) and GPIO boot-phase change atomically between two packets; the mutex's
 * acquire/release orders all plain accesses.  Lock order is "never wait for
 * @lock while holding the BQL": MAIN tries @lock, and only on contention drops
 * the BQL, waits, and retakes the BQL with @lock held.  The DSP thread never
 * takes the BQL and nothing else holding the BQL blocks on @lock, so there is
 * no cycle.  MAIN's catch-up wait is a cond wait on @progress with the BQL
 * dropped; the DSP stops its chunk exactly at MAIN's target (@main_target) and
 * yields @lock until MAIN has retaken it.  @host_waiting is only a hint that ends the DSP's chunk at the next
 * packet.  HINT reaches MAIN's DSP-event latch, a BQL-owned board field: from
 * MAIN (BQL held) at once, from the DSP thread through @hint_bh, which reads
 * the latest level from @hint_high (written under @lock; qemu_bh_schedule's
 * atomic xchg orders it before the BH runs).  Level semantics: a later BH can
 * only deliver a newer level, never resurrect an older one.
 *
 * Checkpoints, deferred-v1 and the virtual-time McASP timer are refused: the
 * packet at which a MAIN event lands now depends on host scheduling, so the
 * run is not replay evidence.  The synchronous default is unchanged.
 */
typedef struct {
    bool on, quit;
    QemuMutex lock;
    QemuCond wake;      /* to the DSP: an event, or a pacing wait to cut */
    QemuCond progress;  /* to MAIN: the DSP finished a chunk */
    QemuThread thread;
    unsigned host_waiting;
    uint64_t packets_per_us;
    int64_t quantum_ns;
    int64_t epoch_ns, report_ns;    /* DSP clock: epoch + packets / rate */
    uint64_t epoch_packets;
    uint64_t main_target;           /* packets MAIN waits for; 0: none */
    uint64_t main_last;             /* packets when MAIN last let go */
    uint64_t access_packets;        /* DSP progress owed per MAIN access */
    uint64_t chunks, host_breaks, waits, main_waits;
    int64_t lag_max_ns, slipped_ns, main_wait_ns;
    QEMUBH *hint_bh;
    bool hint_high;
    void (*real_hint)(void *, bool);
    void *real_opaque;
    Notifier shutdown;
} NxsDspThread;
static NxsDspThread dsp_thread;

static void run_dsp(NxsHpi *s);
static void main_lock(void);
static void main_unlock(void);
static void model_boot_phase(NxsHpi *s, unsigned phase);
static void model_service(NxsHpi *s);
static void virtual_audio_tick(void *opaque);

static void record_event(NxsHpi *s, const char *type, uint64_t offset,
                         uint64_t address, uint64_t value, unsigned size)
{
    const char *path = getenv("CDJ_NXS_DSP_EVENTS");
    if (!path || !*path) return;
    if (!s->event_log) {
        s->event_log = fopen(path, "a");
        if (!s->event_log) {
            error_report("nxs-hpi: cannot open DSP event transcript %s", path);
            return;
        }
    }
    ++s->event_sequence;
    if (fprintf(s->event_log,
                "{\"sequence\":%" PRIu64 ",\"event\":\"%s\","
                "\"offset\":%" PRIu64 ",\"address\":%" PRIu64 ","
                "\"value\":%" PRIu64 ",\"size\":%u,\"boot_phase\":%u,"
                "\"hint\":%s,\"dspint\":%s,\"packets\":%" PRIu64 ","
                "\"cycles\":%" PRIu64 "}\n",
                s->event_sequence, type, offset, address, value, size,
                s->boot_phase, s->hpi.hint ? "true" : "false",
                s->hpi.dspint ? "true" : "false", s->cpu.packets,
                s->cpu.cycles) < 0 || fflush(s->event_log))
        error_report("nxs-hpi: DSP event transcript write failed");
}

static void nxs_audio_callback(void *opaque, int avail)
{
    NxsHpi *s = opaque;
    int16_t out[512 * 2];
    while (avail >= 4) {
        qemu_mutex_lock(&s->audio_lock);
        if (s->audio_priming && s->audio_fill >= NXS_AUDIO_PREFILL_FRAMES)
            s->audio_priming = false;
        if (!s->audio_fill && !s->audio_priming) {
            ++s->audio_underruns;
            s->audio_priming = true;
        }
        unsigned frames = MIN((unsigned)avail / 4, 512u);
        bool silence = s->audio_priming;
        if (!silence) frames = MIN(frames, s->audio_fill);
        for (unsigned i = 0; i < frames; ++i) {
            unsigned at = (s->audio_rd + i) % NXS_AUDIO_RING_FRAMES;
            out[2 * i] = silence ? 0 : s->audio_ring[2 * at];
            out[2 * i + 1] = silence ? 0 : s->audio_ring[2 * at + 1];
        }
        size_t written = audio_be_write(s->audio_backend, s->audio_voice,
                                        out, frames * 4);
        unsigned emitted = written / 4;
        if (!silence) {
            s->audio_rd = (s->audio_rd + emitted) % NXS_AUDIO_RING_FRAMES;
            s->audio_fill -= emitted;
            s->audio_out += emitted;
        }
        qemu_mutex_unlock(&s->audio_lock);
        if (!written) break;
        avail -= written;
    }
}

static void nxs_audio_word(NxsHpi *s, unsigned slot, uint32_t word)
{
    int16_t sample = (int16_t)((int32_t)word >> 16);
    if (slot == 0) {
        s->audio_left = sample;
        s->audio_have_left = true;
        return;
    }
    if (slot != 1 || !s->audio_have_left) return;
    s->audio_have_left = false;
    qemu_mutex_lock(&s->audio_lock);
    if (s->audio_fill == NXS_AUDIO_RING_FRAMES) {
        s->audio_rd = (s->audio_rd + 1) % NXS_AUDIO_RING_FRAMES;
        --s->audio_fill;
        ++s->audio_dropped;
    }
    s->audio_ring[2 * s->audio_wr] = s->audio_left;
    s->audio_ring[2 * s->audio_wr + 1] = sample;
    s->audio_wr = (s->audio_wr + 1) % NXS_AUDIO_RING_FRAMES;
    ++s->audio_fill;
    ++s->audio_in;
    qemu_mutex_unlock(&s->audio_lock);
}

static void nxs_audio_shutdown(Notifier *notifier, void *opaque)
{
    NxsHpi *s = container_of(notifier, NxsHpi, audio_shutdown);
    info_report("nxs-c674x-audio: frames-in=%" PRIu64 " frames-out=%" PRIu64
                " underruns=%" PRIu64 " dropped=%" PRIu64 " fill=%u",
                s->audio_in, s->audio_out, s->audio_underruns,
                s->audio_dropped, s->audio_fill);
    audio_be_set_active_out(s->audio_backend, s->audio_voice, false);
    audio_be_close_out(s->audio_backend, s->audio_voice);
    s->audio_voice = NULL;
    object_unparent(OBJECT(s->audio_backend));
    s->audio_backend = NULL;
}

static bool nxs_pcm_header(FILE *file, uint32_t frames)
{
    uint32_t data_size = frames * 4u;
    uint8_t header[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0,
        'd','a','t','a', 0,0,0,0,
    };
    stl_le_p(header + 4, data_size + 36u);
    stl_le_p(header + 24, NXS_AUDIO_RATE);
    stl_le_p(header + 28, NXS_AUDIO_RATE * 4u);
    stl_le_p(header + 40, data_size);
    return fseek(file, 0, SEEK_SET) == 0 &&
           fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

/* Write one frame per McASP1 serializer-0 slot pair. The file records DSP
 * progression at the firmware's nominal 44.1 kHz format; its wall-clock
 * playback duration is not a measurement of the coarse packet scheduler. */
static bool nxs_pcm_word(NxsHpi *s, unsigned slot, uint32_t word)
{
    int16_t sample = (int16_t)((int32_t)word >> 16);
    if (slot == 0) {
        s->pcm_left = sample;
        s->pcm_have_left = true;
        return true;
    }
    if (slot != 1 || !s->pcm_have_left) return true;
    s->pcm_have_left = false;
    uint64_t numerator;
    uint32_t denominator;
    if ((s->mcasp_control.afsxctl[1] >> 7) != 2 ||
        !cdj_c6747_mcasp_tx_clock_hz(&s->mcasp_control, 1,
            cdj_c6747_pll_auxclk_hz(), CDJ_C6747_MCASP_AFSX,
            &numerator, &denominator) ||
        numerator != (uint64_t)NXS_AUDIO_RATE * denominator)
        return false;
    if (s->pcm_frames >= (UINT32_MAX - 36u) / 4u) return false;
    uint8_t frame[4];
    stw_le_p(frame, (uint16_t)s->pcm_left);
    stw_le_p(frame + 2, (uint16_t)sample);
    if (fwrite(frame, 1, sizeof(frame), s->pcm_wav) != sizeof(frame))
        return false;
    ++s->pcm_frames;
    s->pcm_nonzero_frames += (s->pcm_left != 0 || sample != 0);
    return true;
}

static void nxs_pcm_shutdown(Notifier *notifier, void *opaque)
{
    NxsHpi *s = container_of(notifier, NxsHpi, pcm_shutdown);
    if (!s->pcm_wav) return;
    bool ok = !s->pcm_failed &&
              nxs_pcm_header(s->pcm_wav, (uint32_t)s->pcm_frames);
    if (fclose(s->pcm_wav)) ok = false;
    if (!ok)
        error_report("nxs-c674x: DSP-paced WAV incomplete");
    else
        info_report("nxs-c674x: DSP-paced WAV frames=%" PRIu64
                    " nonzero=%" PRIu64,
                    s->pcm_frames, s->pcm_nonzero_frames);
    s->pcm_wav = NULL;
}

static bool capture_checkpoint(NxsHpi *s, const char *reason)
{
    if (s->model) return false;  /* no interpreter state to capture */
    if (dsp_thread.on) return false;  /* not replay evidence; see dsp_thread */
    const char *policy = getenv("CDJ_NXS_DSP_CHECKPOINT_POLICY");
    if (policy && !strcmp(policy, "fault") && !s->dsp_halted &&
        strcmp(reason, "debug request")) return false;
    const char *directory = getenv("CDJ_NXS_DSP_CHECKPOINT_DIR");
    if (!directory || !*directory || !s->shared_ram || !s->sdram) return false;
    if (g_mkdir_with_parents(directory, 0700)) {
        error_report("nxs-hpi: cannot create DSP checkpoint directory %s", directory);
        return false;
    }
    CdjDspCheckpointState state = {0};
    state.hpi_address = s->address;
    state.boot_phase = s->boot_phase;
    state.words = s->words;
    state.event_sequence = s->event_sequence;
    state.checkpoint_sequence = ++s->checkpoint_sequence;
    state.reset_released = s->reset_released;
    state.dsp_started = s->dsp_started;
    state.dsp_halted = s->dsp_halted;
    state.cpu = s->cpu;
    state.syscfg = s->syscfg;
    state.psc = s->psc;
    state.mcasp = s->mcasp;
    state.mcasp_control = s->mcasp_control;
    state.gpio = s->gpio;
    state.i2c = s->i2c;
    state.pll = s->pll;
    state.hpi = s->hpi;
    state.emifb = s->emifb;
    state.intc = s->intc;
    state.intc_delivery = s->intc_delivery;
    memcpy(state.timers, s->timers, sizeof(state.timers));
    memcpy(state.spis, s->spis, sizeof(state.spis));
    state.spi_transfer = s->spi_transfer;
    state.scheduler = s->scheduler;
    state.wm8740 = s->wm8740;
    state.cache = s->cache;
    state.edma = s->edma;
    state.syscfg_priority = s->syscfg_priority;
    cdj_dsp_checkpoint_prepare(&state, reason);
    g_autofree char *name = g_strdup_printf("%020" PRIu64 ".cdjdsp",
                                             state.checkpoint_sequence);
    g_autofree char *path = g_build_filename(directory, name, NULL);
    char error[160] = {0};
    if (!cdj_dsp_checkpoint_write_with_l1d(
            path, &state, s->l2, sizeof(s->l2),
            s->shared_ram, SHARED_RAM_SIZE, s->l1d, sizeof(s->l1d),
            s->sdram, SDRAM_SIZE, error, sizeof(error))) {
        error_report("nxs-hpi: checkpoint failed: %s", error);
        return false;
    }
    info_report("nxs-hpi: checkpoint=%s reason=%s event-sequence=%" PRIu64,
                path, reason, state.event_sequence);
    return true;
}

static void capture_requested_checkpoint(NxsHpi *s)
{
    const char *request = getenv("CDJ_NXS_DSP_CHECKPOINT_REQUEST");
    if (!request || !*request || s->dsp_running) return;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now < s->checkpoint_check_ns) return;
    s->checkpoint_check_ns = now + 100000000;
    if (!g_file_test(request, G_FILE_TEST_EXISTS)) return;
    bool ok = capture_checkpoint(s, "debug request");
    g_autofree char *response = ok ?
        g_strdup_printf("{\"ok\":true,\"file\":\"%020" PRIu64 ".cdjdsp\"}\n",
                        s->checkpoint_sequence) :
        g_strdup("{\"ok\":false,\"error\":\"DSP checkpoint capture failed; inspect main-stderr.log\"}\n");
    g_autofree char *done = g_strconcat(request, ".done", NULL);
    if (!g_file_set_contents(done, response, -1, NULL))
        error_report("nxs-hpi: cannot acknowledge DSP checkpoint request");
    if (remove(request))
        error_report("nxs-hpi: cannot remove DSP checkpoint request");
}

bool cdj_nxs_hpi_port(hwaddr address)
{
    return nxs_hpi && (address == HPI_BASE + 0x80000 || address == HPI_BASE + 0xc0000);
}

static void reset_line(NxsHpi *s, bool released)
{
    if (released == s->reset_released) return;
    s->reset_released = released;
    if (!released) {
        if (s->dsp_timer) timer_del(s->dsp_timer);
        if (s->mcasp_timer) timer_del(s->mcasp_timer);
        s->mcasp_last_ns = 0;
        s->mcasp_phase = s->mcasp_debt = 0;
        s->mcasp_next_cycle = s->mcasp_cycle_period = 0;
        s->mcasp_last_report_ns = 0;
        s->mcasp_have_report = false;
        s->pcm_have_left = false;
        if (s->audio_voice) {
            qemu_mutex_lock(&s->audio_lock);
            s->audio_rd = s->audio_wr = s->audio_fill = 0;
            s->audio_have_left = false;
            s->audio_priming = true;
            qemu_mutex_unlock(&s->audio_lock);
        }
        uint8_t scheduler_mode = s->scheduler.mode;
        cdj_dsp_scheduler_reset(&s->scheduler);
        s->scheduler.mode = scheduler_mode;
        if (!scheduler_mode) memset(&s->scheduler, 0, sizeof(s->scheduler));
        /* External DSP reset covers the HPI boot contract, C674x megamodule
         * INTC, Timer64P/SPI blocks, and interpreter lifecycle. Other device
         * peripheral reset domains remain explicit models. */
        cdj_c6747_hpi_reset(&s->hpi);
        cdj_c6747_intc_reset(&s->intc);
        cdj_c6747_intc_delivery_reset(&s->intc_delivery);
        cdj_c6747_timers_reset(s->timers);
        cdj_c6747_spis_reset(s->spis);
        cdj_c6747_spi_transfer_reset(&s->spi_transfer);
        cdj_wm8740_reset(&s->wm8740);
        cdj_c6747_cache_reset(&s->cache);
        /* SRAM contents are undefined across external reset.  Clear the
         * functional backing store so a later SRAM partition cannot expose
         * bytes retained from the preceding DSP lifetime. */
        memset(s->l1d, 0, sizeof(s->l1d));
        cdj_c6747_edma_reset(&s->edma);
        cdj_c6747_mcasp_reset(&s->mcasp);
        cdj_c6747_mcasp_control_reset(&s->mcasp_control);
        s->dsp_started = s->dsp_halted = s->dsp_running = false;
        if (s->hint) s->hint(s->opaque, true);
        record_event(s, "reset_assert", 0, 0, 0, 0);
        info_report("nxs-hpi: DSP reset asserted; HPI boot state reset");
        return;
    }

    /* The on-chip ROM itself is not executed. TI SPRABB1C section 4.1 says
     * its HPI boot path sets HINT when ready for the host download. This is
     * the sole ROM handoff abstraction; uploaded firmware remains genuine. */
    cdj_c6747_hpi_rom_boot_ready(&s->hpi);
    if (s->hint) s->hint(s->opaque, false);
    record_event(s, "rom_hpi_ready", 0, 0, 1, 0);
    info_report("nxs-hpi: DSP reset released; ROM HPI-ready HINT asserted");
}

static void boot_phase(NxsHpi *s, unsigned phase)
{
    if (phase > 7) return;
    bool changed = phase != s->boot_phase;
    s->boot_phase = phase;
    /* Schematic-confirmed CPU_PH0/1/2 reach GP4[5]/GP4[2]/GP4[3]. */
    cdj_c6747_gpio_set_input(&s->gpio, 4, 5, phase & 1);
    cdj_c6747_gpio_set_input(&s->gpio, 4, 2, phase & 2);
    cdj_c6747_gpio_set_input(&s->gpio, 4, 3, phase & 4);
    info_report("nxs-hpi: MAIN boot phase=%u -> DSP GP4 inputs=%#x",
                phase, s->gpio.input[2] & 0x2c);
    record_event(s, "boot_phase", 0, 0, phase, 0);
    /* A phase-budget yield is a cooperative scheduling boundary, not a DSP
     * halt.  Resume when the genuine MAIN firmware changes the sideband that
     * the DSP is polling. */
    if (changed && s->model) {
        model_boot_phase(s, phase);
    } else if (changed) {
        if (s->dsp_started && !s->dsp_halted)
            capture_checkpoint(s, "boot-phase boundary");
        run_dsp(s);
    }
}

static uint8_t *host_memory(NxsHpi *s, uint32_t address)
{
    uint32_t l1d_offset;
    if (address >= 0x11f00000u &&
        cdj_c6747_l1d_sram_span(&s->cache, address, 4, &l1d_offset))
        return s->l1d + l1d_offset;
    if (address >= L2_BASE && address <= L2_BASE + L2_SIZE - 4)
        return s->l2 + address - L2_BASE;
    if (address >= SHARED_RAM_BASE &&
        address <= SHARED_RAM_BASE + SHARED_RAM_SIZE - 4)
        return s->shared_ram + address - SHARED_RAM_BASE;
    uint32_t sdram_offset;
    if (cdj_c6747_emifb_sdram_offset(&s->emifb, address, 4, SDRAM_SIZE,
                                    &sdram_offset))
        return s->sdram + sdram_offset;
    return NULL;
}

static bool valid_data(NxsHpi *s)
{
    return s->hpi.hwob && !s->hpi.hpirst && !(s->address & 3) &&
           host_memory(s, s->address) != NULL;
}

static bool dsp_read(void *opaque, uint32_t address, uint32_t *value)
{
    NxsHpi *s = opaque;
    /* RAM and its local L2 alias do not overlap any peripheral window.
     * Instruction fetches dominate reads: avoid probing every MMIO device.
     * Keep SDRAM's dynamic enable gate and all alignment checks. */
    /* The windows below are disjoint, so their order is free: L2 (and its
     * local alias) goes first because the firmware executes from it and it
     * needs no call to decide. */
    uint32_t local_address = address;
    if (address >= 0x00800000 && address < 0x00840000) local_address += 0x11000000;
    if (!(local_address & 3) && local_address >= L2_BASE &&
        local_address <= L2_BASE + L2_SIZE - 4) {
        *value = ldl_le_p(s->l2 + local_address - L2_BASE);
        return true;
    }
    uint32_t l1d_offset;
    if (!(address & 3) && cdj_c6747_l1d_sram_span(
            &s->cache, address, 4, &l1d_offset)) {
        *value = ldl_le_p(s->l1d + l1d_offset);
        return true;
    }
    if (!(address & 3) && address >= SHARED_RAM_BASE &&
        address <= SHARED_RAM_BASE + SHARED_RAM_SIZE - 4) {
        *value = ldl_le_p(s->shared_ram + address - SHARED_RAM_BASE);
        return true;
    }
    uint32_t sdram_offset;
    if (!(address & 3) && address >= SDRAM_BASE && address < 0xe0000000u &&
        cdj_c6747_emifb_sdram_offset(&s->emifb, address, 4, SDRAM_SIZE,
                                    &sdram_offset)) {
        *value = ldl_le_p(s->sdram + sdram_offset);
        return true;
    }
    /* Past RAM, a device read voids an idle proof.  GPIO is exempt: its reads
     * are pure (a const model) and its inputs change only when MAIN writes
     * the boot phase, which cannot happen while the DSP runs.  The NXS idle
     * loop polls those boot-phase inputs. */
    if (address < 0x01e26000u || address >= 0x01e27000u) s->idle_dirty = true;
    if (cdj_c6747_syscfg_read(&s->syscfg, address, value)) return true;
    if (cdj_c6747_syscfg_priority_read(&s->syscfg_priority, address, value))
        return true;
    if (cdj_c6747_psc_read(&s->psc, address, value)) return true;
    if (cdj_c6747_mcasp_read(&s->mcasp, address, value)) return true;
    if (cdj_c6747_mcasp_control_read(&s->mcasp_control, address, value)) return true;
    if (cdj_c6747_gpio_read(&s->gpio, address, value)) return true;
    if (cdj_c6747_i2c_read(&s->i2c, address, value)) return true;
    if (cdj_c6747_intc_read(&s->intc, address, value)) return true;
    if (cdj_c6747_timers_read(s->timers, address, value)) return true;
    if (!cdj_c674x_loop_functional_timing() &&
        cdj_c6747_spi_wm8740_timed_mapped(address))
        return cdj_c6747_spi_wm8740_read_timed(s->spis, &s->spi_transfer,
                                               address, value);
    if (cdj_c6747_spis_read(s->spis, address, value)) return true;
    if (cdj_c6747_cache_read(&s->cache, address, value)) return true;
    if (cdj_c6747_edma_read(&s->edma, address, value)) return true;
    if (cdj_c6747_pll_read(&s->pll, address, value)) return true;
    if (cdj_c6747_emifb_read(&s->emifb, address, value)) return true;
    if ((s->syscfg.cfgchip[1] & 0x8000) &&
        cdj_c6747_hpi_cpu_read(&s->hpi, address, value)) return true;
    return false;
}

static uint8_t *dsp_memory_span(NxsHpi *s, uint32_t address, size_t size)
{
    uint64_t end = (uint64_t)address + size;
    if (!size || end > UINT64_C(0x100000000)) return NULL;
    uint32_t l1d_offset;
    if (cdj_c6747_l1d_sram_span(&s->cache, address, size, &l1d_offset))
        return s->l1d + l1d_offset;
    if (address >= L2_BASE && end <= (uint64_t)L2_BASE + L2_SIZE)
        return s->l2 + address - L2_BASE;
    if (address >= 0x00800000u &&
        end <= UINT64_C(0x00800000) + L2_SIZE)
        return s->l2 + address - 0x00800000u;
    if (address >= SHARED_RAM_BASE &&
        end <= (uint64_t)SHARED_RAM_BASE + SHARED_RAM_SIZE)
        return s->shared_ram + address - SHARED_RAM_BASE;
    uint32_t sdram_offset;
    if (cdj_c6747_emifb_sdram_offset(&s->emifb, address, size, SDRAM_SIZE,
                                    &sdram_offset))
        return s->sdram + sdram_offset;
    return NULL;
}

static bool dsp_l1d_write(NxsHpi *s, uint32_t address, uint64_t value,
                          unsigned size, bool commit)
{
    uint32_t offset;
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        !cdj_c6747_l1d_sram_span(&s->cache, address, size, &offset))
        return false;
    if (commit) {
        uint8_t *target = s->l1d + offset;
        if (size == 8) stq_le_p(target, value);
        else if (size == 1) *target = value;
        else if (size == 2) stw_le_p(target, value);
        else stl_le_p(target, value);
    }
    return true;
}

typedef struct {
    uint8_t *target;
    uint8_t *bytes;
    size_t size;
} EdmaStagedWrite;

typedef struct {
    NxsHpi *owner;
    CdjC6747McaspControl *mcasp;
    EdmaStagedWrite *writes;
    size_t write_count, write_capacity;
    /* Set when an EDMA read touched a RAM word the DSP has rewritten since
     * its idle anchor (its value mid-period is not the anchor value). */
    bool idle_log_hit;
} EdmaBusContext;

static bool edma_stage_write(EdmaBusContext *context, uint8_t *target,
                             const uint8_t *bytes, size_t size)
{
    if (context->write_count == context->write_capacity) {
        size_t capacity = context->write_capacity ?
                          context->write_capacity * 2u : 8u;
        if (capacity < context->write_capacity) return false;
        EdmaStagedWrite *writes = g_try_renew(
            EdmaStagedWrite, context->writes, capacity);
        if (!writes) return false;
        context->writes = writes;
        context->write_capacity = capacity;
    }
    uint8_t *copy = g_try_malloc(size);
    if (!copy) return false;
    memcpy(copy, bytes, size);
    context->writes[context->write_count++] =
        (EdmaStagedWrite){target, copy, size};
    return true;
}

static void edma_free_staged_writes(EdmaBusContext *context)
{
    for (size_t i = 0; i < context->write_count; ++i)
        g_free(context->writes[i].bytes);
    g_free(context->writes);
}

static bool edma_read_bytes(void *opaque, uint32_t address, uint8_t *bytes,
                            size_t size)
{
    EdmaBusContext *context = opaque;
    uint8_t *source = dsp_memory_span(context->owner, address, size);
    if (!source) return false;
    memcpy(bytes, source, size);
    const NxsHpi *owner = context->owner;
    if (owner->idle_skip && owner->idle_anchor_valid)
        for (unsigned i = 0; i < owner->idle_log_count; ++i)
            if (owner->idle_log_address[i] < address + size &&
                address < owner->idle_log_address[i] + 4)
                context->idle_log_hit = true;
    uintptr_t read_start = (uintptr_t)source;
    uintptr_t read_end = read_start + size;
    for (size_t i = 0; i < context->write_count; ++i) {
        EdmaStagedWrite *write = &context->writes[i];
        uintptr_t write_start = (uintptr_t)write->target;
        uintptr_t write_end = write_start + write->size;
        if (write_start < read_end && read_start < write_end) {
            uintptr_t start = write_start > read_start ? write_start : read_start;
            uintptr_t end = write_end < read_end ? write_end : read_end;
            memcpy(bytes + start - read_start,
                   write->bytes + start - write_start, end - start);
        }
    }
    return true;
}

static bool edma_write_bytes(void *opaque, uint32_t address,
                             const uint8_t *bytes, size_t size, bool commit)
{
    EdmaBusContext *context = opaque;
    uint8_t *target = dsp_memory_span(context->owner, address, size);
    if (target) {
        return !commit || edma_stage_write(context, target, bytes, size);
    }
    if (size == 4) {
        uint32_t value = ldl_le_p(bytes);
        return cdj_c6747_mcasp_control_write(context->mcasp, address,
                                             value, 4, commit);
    }
    return false;
}

static bool service_mcasp_axevt(CdjC6747Edma *edma,
                                CdjC6747McaspControl *mcasp,
                                EdmaBusContext *context)
{
    const CdjC6747EdmaBus bus = {edma_read_bytes, edma_write_bytes, context};
    for (unsigned instance = 1; instance <= 2; ++instance) {
        unsigned channel = instance == 1 ? 3 : 5;
        for (unsigned serializer = 0;
             serializer < 16 &&
             cdj_c6747_mcasp_axevt_ready(mcasp, instance);
             ++serializer) {
            uint64_t before = mcasp->xbuf_writes[instance];
            if (!cdj_c6747_edma_event(edma, channel, &bus)) return false;
            /* A disabled EDMA channel latches ER without servicing XBUF.
             * Stop until EESR is programmed rather than spinning. */
            if (mcasp->xbuf_writes[instance] == before) break;
        }
    }
    return true;
}

static void deliver_edma_notifications(NxsHpi *s)
{
    /* C6747 system event 8 is the EDMA3CC region-1 completion pulse. */
    if (cdj_c6747_edma_take_irq_notification(&s->edma, 1)) {
        cdj_c6747_intc_deliver_event(&s->intc, &s->intc_delivery, 8);
        s->idle_dirty = true;
    }
}

static bool edma_mcasp_transaction(NxsHpi *s, bool edma_access,
                                   uint32_t address, uint64_t value,
                                   unsigned size, bool commit)
{
    if (!(edma_access ? cdj_c6747_edma_write_mapped(address, size) :
                       cdj_c6747_mcasp_control_write_mapped(address, size)))
        return false;
    CdjC6747Edma trial_edma = s->edma;
    CdjC6747McaspControl trial_mcasp = s->mcasp_control;
    EdmaBusContext trial_context = {.owner = s, .mcasp = &trial_mcasp};
    const CdjC6747EdmaBus trial_bus = {
        edma_read_bytes, edma_write_bytes, &trial_context,
    };
    bool ok = edma_access ?
        cdj_c6747_edma_write(&trial_edma, address, value, size, true,
                             &trial_bus) :
        cdj_c6747_mcasp_control_write(&trial_mcasp, address, value, size, true);
    if (ok) ok = service_mcasp_axevt(&trial_edma, &trial_mcasp,
                                     &trial_context);
    if (ok && commit) {
        for (size_t i = 0; i < trial_context.write_count; ++i) {
            EdmaStagedWrite *write = &trial_context.writes[i];
            memcpy(write->target, write->bytes, write->size);
        }
        s->edma = trial_edma;
        s->mcasp_control = trial_mcasp;
        deliver_edma_notifications(s);
    }
    edma_free_staged_writes(&trial_context);
    return ok;
}

static bool advance_functional_mcasp_slots(NxsHpi *s)
{
    CdjC6747McaspControl original_mcasp = s->mcasp_control;
    CdjC6747Edma trial_edma = s->edma;
    CdjC6747McaspControl trial_mcasp = s->mcasp_control;
    EdmaBusContext trial_context = {.owner = s, .mcasp = &trial_mcasp};
    bool advanced = false, ok = true;

    for (unsigned instance = 1; instance <= 2; ++instance) {
        if ((trial_mcasp.gblctl[instance] & 0x1f00u) != 0x1f00u)
            continue;
        bool axevt;
        if (!cdj_c6747_mcasp_tx_slot(&trial_mcasp, instance, &axevt)) {
            ok = false;
            break;
        }
        advanced = true;
    }
    if (ok && advanced)
        ok = service_mcasp_axevt(&trial_edma, &trial_mcasp, &trial_context);
    if (ok && advanced) {
        for (size_t i = 0; i < trial_context.write_count; ++i) {
            EdmaStagedWrite *write = &trial_context.writes[i];
            memcpy(write->target, write->bytes, write->size);
        }
        s->edma = trial_edma;
        s->mcasp_control = trial_mcasp;
        /* A slot the DSP cannot observe keeps an idle proof: it staged no
         * RAM write and read no transiently rewritten word (an EDMA
         * completion dirties it in deliver_edma_notifications). */
        if (trial_context.write_count || trial_context.idle_log_hit)
            s->idle_dirty = true;
        deliver_edma_notifications(s);
        if (s->tx_capture || s->audio_voice || s->pcm_wav) {
            for (unsigned instance = 1; instance <= 2; ++instance) {
                for (unsigned serializer = 0; serializer < 16; ++serializer) {
                    if ((s->audio_voice || s->pcm_wav) &&
                        instance == 1 && serializer == 0 &&
                        (trial_mcasp.gblctl[1] & 0x1f00u) == 0x1f00u &&
                        (trial_mcasp.srctl[1][0] & 3u) == 1u) {
                        if (s->audio_voice)
                            nxs_audio_word(s, trial_mcasp.xslot[1],
                                           trial_mcasp.xrsr[1][0]);
                        if (s->pcm_wav &&
                            !nxs_pcm_word(s, trial_mcasp.xslot[1],
                                          trial_mcasp.xrsr[1][0])) {
                            s->pcm_failed = true;
                            ok = false;
                            break;
                        }
                    }
                    uint64_t sequence = trial_mcasp.xrsr_source_sequence[instance][serializer];
                    if (!sequence || sequence ==
                        original_mcasp.xrsr_source_sequence[instance][serializer])
                        continue;
                    if (!s->tx_capture) continue;
                    if (s->tx_capture_nonzero_only &&
                        !trial_mcasp.xrsr[instance][serializer])
                        continue;
                    if (fprintf(s->tx_capture,
                            "{\"sequence\":%" PRIu64 ",\"instance\":%u,"
                            "\"slot\":%u,\"serializer\":%u,\"word\":%u,"
                            "\"xbuf_sequence\":%" PRIu64 ",\"packets\":%" PRIu64 ","
                            "\"cycles\":%" PRIu64 ",\"virtual_ns\":%" PRIi64 ","
                            "\"source\":\"genuine_xbuf\","
                            "\"clock\":\"%s\"}\n",
                            ++s->tx_capture_sequence, instance,
                            trial_mcasp.xslot[instance], serializer,
                            trial_mcasp.xrsr[instance][serializer], sequence,
                            s->cpu.packets, s->cpu.cycles,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                            s->virtual_audio_clock ? "virtual-clock-batch" :
                            s->cycle_audio_clock ? "dsp-sysclk1-cycle" :
                                                   "functional-coarse-packet-slot") < 0 ||
                        fflush(s->tx_capture)) {
                        s->tx_capture_failed = true;
                        fclose(s->tx_capture);
                        s->tx_capture = NULL;
                        if (s->tx_capture_path)
                            remove(s->tx_capture_path);
                        ok = false;
                        break;
                    }
                    if (s->tx_capture_sequence >= s->tx_capture_limit) {
                        info_report("nxs-hpi: DSP transmit capture reached bounded limit of %" PRIu64 " records",
                                    s->tx_capture_limit);
                        fclose(s->tx_capture);
                        s->tx_capture = NULL;
                        break;
                    }
                }
                if (!ok || !s->tx_capture) break;
            }
        }
    }
    edma_free_staged_writes(&trial_context);
    return ok;
}

/* advance_functional_mcasp_slots on caller-owned EDMA/McASP copies, with no
 * commit and no output: 1 if the slot would be visible to the DSP (a staged
 * RAM write, an EDMA completion, or a read of a word rewritten since the idle
 * anchor), 0 if not, -1 if it would fail. */
static int functional_slot_probe(NxsHpi *s, CdjC6747Edma *edma,
                                 CdjC6747McaspControl *mcasp)
{
    EdmaBusContext context = {.owner = s, .mcasp = mcasp};
    uint32_t notifications = edma->irq_notifications;
    bool advanced = false;
    int result = 0;
    for (unsigned instance = 1; instance <= 2 && result >= 0; ++instance) {
        if ((mcasp->gblctl[instance] & 0x1f00u) != 0x1f00u) continue;
        bool axevt;
        if (!cdj_c6747_mcasp_tx_slot(mcasp, instance, &axevt)) result = -1;
        advanced = true;
    }
    if (result >= 0 && advanced && !service_mcasp_axevt(edma, mcasp, &context))
        result = -1;
    if (result >= 0 && (context.write_count || context.idle_log_hit ||
                        (edma->irq_notifications & ~notifications & 2u)))
        result = 1;
    edma_free_staged_writes(&context);
    return result;
}

static bool functional_audio_tick(NxsHpi *s)
{
    if (!s->functional_audio || s->virtual_audio_clock)
        return true;
    if (s->cycle_audio_clock) {
        if ((s->mcasp_control.gblctl[1] & 0x1f00u) != 0x1f00u) {
            s->mcasp_next_cycle = s->mcasp_cycle_period = 0;
            return true;
        }
        if (s->mcasp_next_cycle && s->cpu.cycles < s->mcasp_next_cycle)
            return true;
        uint64_t core_num, frame_num;
        uint32_t core_den, frame_den;
        unsigned slots = s->mcasp_control.afsxctl[1] >> 7;
        if (slots != 2 ||
            !cdj_c6747_pll_sysclk_hz(&s->pll, 1, &core_num, &core_den) ||
            !cdj_c6747_mcasp_tx_clock_hz(&s->mcasp_control, 1,
                cdj_c6747_pll_auxclk_hz(), CDJ_C6747_MCASP_AFSX,
                &frame_num, &frame_den) ||
            !core_den || !frame_den ||
            frame_num > UINT64_MAX / (slots * (uint64_t)core_den) ||
            core_num > UINT64_MAX / frame_den) {
            s->cpu.fault = "unsupported DSP-cycle McASP clock";
            s->cpu.fault_pc = s->cpu.pc;
            return false;
        }
        uint64_t slot_num = frame_num * slots * core_den;
        uint64_t cycle_num = core_num * frame_den;
        if (!slot_num || cycle_num < slot_num || cycle_num % slot_num) {
            s->cpu.fault = "nonintegral DSP cycles per McASP slot";
            s->cpu.fault_pc = s->cpu.pc;
            return false;
        }
        uint64_t period = cycle_num / slot_num;
        if (!s->mcasp_next_cycle || s->mcasp_cycle_period != period) {
            info_report("nxs-c674x-audio-clock: SYSCLK1 cycles per McASP1 slot=%" PRIu64,
                        period);
            s->mcasp_cycle_period = period;
            s->mcasp_next_cycle = s->cpu.cycles + period;
            return true;
        }
        do {
            if (!advance_functional_mcasp_slots(s)) goto slot_fault;
            s->mcasp_next_cycle += period;
        } while (s->cpu.cycles >= s->mcasp_next_cycle);
        return true;
    }
    if (s->cpu.packets % CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL)
        return true;
    if (advance_functional_mcasp_slots(s)) return true;
slot_fault:
    s->cpu.fault = s->tx_capture_failed ? "DSP transmit capture write failed" :
                   s->pcm_failed ? "DSP-paced WAV output failed or format changed" :
                   "unsupported functional McASP transmit slot";
    s->cpu.fault_pc = s->cpu.pc;
    s->cpu.fault_word = 0;
    return false;
}

/*
 * Idle-loop skip.  In the legacy scheduler a DSP activation runs until HINT or
 * its packet budget, synchronously inside the SH-4's MMIO write, so while the
 * DSP firmware sits in its polling loop MAIN is frozen for the rest of the
 * budget.  Nothing outside the DSP can run during that time.  So if the DSP
 * returns to an earlier PC with the same registers and the same memory (every
 * RAM word it wrote since then holds its earlier value again), having made no
 * device access other than pure GPIO reads, no McASP slot tick or EDMA
 * completion happened, and no tick-driven peripheral can raise an event, then
 * the whole system state has repeated with a period of P packets and C cycles
 * and will keep repeating.  Running k more periods is then exactly: packets
 * += kP, cycles += kC, the PLL's input-period counter advanced by kC edges,
 * and each delayed-control cycle written once per period moved by kC.  The
 * load/store queues are empty at the anchor, but their dead slots still hold
 * the last retired entries and checkpoints store them verbatim.  A skip
 * therefore always leaves at least one whole period to execute before the
 * activation budget ends or the next visible McASP slot: that period repeats
 * every append of the loop at the same phase, so every dead slot the loop
 * uses is rewritten with exactly the bytes full execution leaves, and only a
 * checkpoint (at the end of an activation, or between activations) ever
 * reads dead slots.
 * dsp_idle_skip applies that for as many whole periods as fit before the end
 * of the activation budget and before the next functional McASP slot edge,
 * so every checkpoint, event record and report stays what full execution
 * would have produced.
 */
#define DSP_IDLE_WINDOW 65536u

static bool dsp_idle_clean(const NxsHpi *s)
{
    const CdjC674x *c = &s->cpu;
    return !c->fault && !c->store_count && !c->load_count && !c->branch_due &&
           !c->branch_count && !c->loop_active && !c->idle_cycles;
}

/* Nothing clocked by DSP cycles or steps can raise an event or change state
 * that firmware could observe. */
static bool dsp_idle_quiescent(const NxsHpi *s)
{
    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i)
        if ((s->timers[i].tgcr & 3u) && (s->timers[i].tcr & 0x00c000c0u))
            return false;
    if (s->pll.go_remaining || s->pll.lock_wait_remaining ||
        ((s->pll.config[0] & 0x12b) == 0x100 && s->pll.reset_age < 17))
        return false;
    if (!cdj_c674x_loop_functional_timing() &&
        (s->spi_transfer.phase || s->spi_transfer.queued_valid ||
         s->spi_transfer.tx_full || s->spi_transfer.fault))
        return false;
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned d = 0; d < 2; ++d)
            if (s->psc.remaining[b][d]) return false;
    return true;
}

static void dsp_idle_anchor(NxsHpi *s, unsigned step)
{
    s->idle_anchor_valid = true;
    s->idle_dirty = false;
    s->idle_anchor_step = step;
    s->idle_anchor_pc = s->cpu.pc;
    s->idle_anchor_cycles = s->cpu.cycles;
    s->idle_anchor_packets = s->cpu.packets;
    memcpy(s->idle_anchor_r, s->cpu.r, sizeof(s->idle_anchor_r));
    memcpy(s->idle_anchor_control, s->cpu.control,
           sizeof(s->idle_anchor_control));
    memcpy(s->idle_anchor_ready, s->cpu.control_ready,
           sizeof(s->idle_anchor_ready));
    s->idle_log_count = 0;
}

/* Same architectural state as the anchor.  A control_ready entry is a cycle
 * at which a delayed control value becomes visible: it must be unchanged or,
 * if rewritten once per period, have moved by exactly the period's cycles
 * (and be already passed, so it is not pending).  Entry 31 holds loop
 * context, not a cycle. */
static bool dsp_idle_repeat(const NxsHpi *s)
{
    if (s->cpu.pc != s->idle_anchor_pc || !dsp_idle_clean(s) ||
        memcmp(s->cpu.r, s->idle_anchor_r, sizeof(s->idle_anchor_r)) ||
        memcmp(s->cpu.control, s->idle_anchor_control,
               sizeof(s->idle_anchor_control)))
        return false;
    uint64_t period = s->cpu.cycles - s->idle_anchor_cycles;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t a = s->idle_anchor_ready[i], b = s->cpu.control_ready[i];
        if (a != b && (i == 31 || b - a != period || b > s->cpu.cycles))
            return false;
    }
    for (unsigned i = 0; i < s->idle_log_count; ++i) {
        const uint8_t *p = dsp_memory_span((NxsHpi *)s, s->idle_log_address[i], 4);
        if (!p || ldl_le_p(p) != s->idle_log_value[i]) return false;
    }
    return true;
}

/* Whole repeat periods that can be skipped with `remaining` activation steps
 * left: none may cross a functional McASP slot edge while a transmitter runs
 * (packet-interval mode fires when packets reach a multiple of the interval,
 * the cycle clock when cycles reach mcasp_next_cycle). */
static uint64_t dsp_idle_periods(const NxsHpi *s, uint64_t remaining)
{
    uint64_t packets = s->cpu.packets - s->idle_anchor_packets;
    uint64_t cycles = s->cpu.cycles - s->idle_anchor_cycles;
    if (!packets || !cycles) return 0;
    uint64_t k = remaining / packets;
    if (s->functional_audio && s->cycle_audio_clock) {
        if ((s->mcasp_control.gblctl[1] & 0x1f00u) == 0x1f00u) {
            if (!s->mcasp_next_cycle || s->mcasp_next_cycle <= s->cpu.cycles)
                return 0;
            k = MIN(k, (s->mcasp_next_cycle - 1 - s->cpu.cycles) / cycles);
        }
    } else if (s->functional_audio &&
               ((s->mcasp_control.gblctl[1] & 0x1f00u) == 0x1f00u ||
                (s->mcasp_control.gblctl[2] & 0x1f00u) == 0x1f00u)) {
        /* Slots the DSP cannot observe may fall inside the skip (they are
         * then applied in order by dsp_idle_skip_slots); stop before the
         * first visible one.  TX capture records carry per-slot DSP
         * counters, so with it every slot is treated as visible. */
        const uint64_t interval = CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL;
        uint64_t limit = s->cpu.packets + k * packets;
        uint64_t edge = (s->cpu.packets / interval + 1) * interval;
        if (!s->tx_capture) {
            CdjC6747Edma edma = s->edma;
            CdjC6747McaspControl mcasp = s->mcasp_control;
            while (edge <= limit &&
                   functional_slot_probe((NxsHpi *)s, &edma, &mcasp) == 0)
                edge += interval;
        }
        if (edge <= limit) k = (edge - 1 - s->cpu.packets) / packets;
    }
    /* Keep one whole period to execute afterwards (see "Idle-loop skip"). */
    return k ? k - 1 : 0;
}

/* Advance the proven-repeating system by k periods; see "Idle-loop skip". */
static void dsp_idle_skip(NxsHpi *s, uint64_t k)
{
    uint64_t packets = s->cpu.packets - s->idle_anchor_packets;
    uint64_t cycles = s->cpu.cycles - s->idle_anchor_cycles;
    for (unsigned i = 0; i < 31; ++i)
        if (s->cpu.control_ready[i] != s->idle_anchor_ready[i])
            s->cpu.control_ready[i] += k * cycles;
    s->cpu.packets += k * packets;
    s->cpu.cycles += k * cycles;
    cdj_c6747_pll_ticks(&s->pll, k * cycles);
    s->idle_skipped_packets += k * packets;
}

/* Run the functional McASP slots whose edges (packet counts that are
 * multiples of the interval) a skip from `from` to cpu.packets passed over,
 * in order, exactly as functional_audio_tick would have after each of those
 * steps.  dsp_idle_periods proved each of them invisible to the DSP. */
static bool dsp_idle_skip_slots(NxsHpi *s, uint64_t from)
{
    if (!s->functional_audio || s->cycle_audio_clock) return true;
    const uint64_t interval = CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL;
    uint64_t now = s->cpu.packets;
    bool ok = true;
    for (uint64_t edge = (from / interval + 1) * interval; ok && edge <= now;
         edge += interval) {
        s->cpu.packets = edge;
        ok = functional_audio_tick(s);
    }
    s->cpu.packets = now;
    return ok;
}

/* Called before a committed DSP write lands.  A device write dirties the idle
 * proof; a RAM write records each touched word's value on first write since
 * the anchor, so dsp_idle_repeat can require memory to match again.  Too many
 * distinct words also dirties it. */
static void dsp_idle_note_write(NxsHpi *s, uint32_t address, uint64_t value,
                                unsigned size)
{
    (void)value;
    if (!s->idle_anchor_valid || s->idle_dirty) return;
    if (size > 8 || !dsp_memory_span(s, address, size)) {
        s->idle_dirty = true;
        return;
    }
    for (uint32_t word = address & ~3u; word < address + size; word += 4) {
        unsigned i = 0;
        while (i < s->idle_log_count && s->idle_log_address[i] != word) ++i;
        if (i < s->idle_log_count) continue;
        const uint8_t *p = dsp_memory_span(s, word, 4);
        if (!p || i == 64) {
            s->idle_dirty = true;
            return;
        }
        s->idle_log_address[i] = word;
        s->idle_log_value[i] = ldl_le_p(p);
        s->idle_log_count = i + 1;
    }
}

static bool dsp_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    NxsHpi *s = opaque;
    if (commit && s->idle_skip) dsp_idle_note_write(s, address, value, size);
    if (dsp_l1d_write(s, address, value, size, commit)) return true;
    if (cdj_c6747_syscfg_pll_locked(&s->syscfg) &&
        cdj_c6747_pll_write_mapped(address, size)) {
        if (commit) info_report("nxs-pll: locked write ignored address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (!cdj_c674x_loop_functional_timing() &&
        (s->spi_transfer.phase || s->spi_transfer.queued_valid) &&
        cdj_c6747_pll_write_mapped(address, size)) return false;
    if (cdj_c6747_pll_write(&s->pll, address, value, size, commit)) {
        if (commit) info_report("nxs-pll: write address=%#x value=%#x legacy-bit4-assumption=%d",
                                address, (uint32_t)value, s->pll.legacy_bit4_used);
        return true;
    }
    if (cdj_c6747_i2c_write(&s->i2c, address, value, size, commit)) {
        if (commit) info_report("nxs-i2c: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_intc_write_delivery(&s->intc, &s->intc_delivery,
                                      address, value, size, commit)) {
        if (commit) info_report("nxs-intc: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_timers_write(s->timers, address, value, size, commit)) {
        if (commit) info_report("nxs-timer: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (!cdj_c674x_loop_functional_timing() &&
        cdj_c6747_spi_wm8740_timed_mapped(address)) {
        if ((address == CDJ_C6747_SPI1_BASE + 0x38 ||
             address == CDJ_C6747_SPI1_BASE + 0x3c) &&
            (s->spis[1].gcr1 & (1u << 24)) &&
            !cdj_spi_clock_ready(&s->pll)) return false;
        return cdj_c6747_spi_wm8740_write_timed(s->spis, &s->wm8740,
                  &s->spi_transfer, address, value, size, commit);
    }
    if (cdj_c6747_spis_write_wm8740(
            s->spis, &s->wm8740, address, value, size,
            cdj_c674x_loop_functional_timing(), commit)) {
        if (commit)
            info_report("nxs-spi: WM8740 write word=%#x transfers=%" PRIu64,
                        (uint32_t)value & 0xffff, s->wm8740.transfers);
        return true;
    }
    if (cdj_c6747_spis_write(s->spis, address, value, size, commit)) {
        if (commit) info_report("nxs-spi: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_cache_write(&s->cache, address, value, size, commit)) {
        if (commit) info_report("nxs-cache: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (edma_mcasp_transaction(s, true, address, value, size, commit)) {
        if (commit) info_report("nxs-edma: write address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_gpio_write(&s->gpio, address, value, size, commit)) {
        if (commit) info_report("nxs-gpio: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_mcasp_write(&s->mcasp, address, value, size, commit)) {
        if (commit) info_report("nxs-mcasp: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (edma_mcasp_transaction(s, false, address, value, size, commit)) {
        if (commit)
            info_report("nxs-mcasp-control: write address=%#x value=%#x",
                        address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_psc_write(&s->psc, address, value, size, commit)) {
        if (commit) info_report("nxs-psc: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_syscfg_write(&s->syscfg, address, value, size, commit)) {
        if (commit) info_report("nxs-syscfg: write address=%#x value=%#x unlocked=%d",
                                address, (uint32_t)value, s->syscfg.unlocked);
        return true;
    }
    if (cdj_c6747_syscfg_priority_write(&s->syscfg_priority, &s->syscfg,
                                        address, value, size, commit)) {
        if (commit)
            info_report("nxs-syscfg: master priority address=%#x value=%#x",
                        address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_emifb_write(&s->emifb, address, value, size, commit)) {
        if (commit) info_report("nxs-emifb: write address=%#x value=%#x init-sequences=%u",
                                address, (uint32_t)value, s->emifb.init_sequences);
        return true;
    }
    if ((s->syscfg.cfgchip[1] & 0x8000)) {
        bool old_hint = s->hpi.hint;
        if (cdj_c6747_hpi_cpu_write(&s->hpi, address, value, size, commit)) {
            if (commit) {
                info_report("nxs-hpi: DSP HPIC write value=%#x DSPINT=%d HINT=%d",
                            (uint32_t)value, s->hpi.dspint, s->hpi.hint);
                record_event(s, "dsp_hpic_write", 0, address, value, size);
                if (!old_hint && s->hpi.hint && s->hint) s->hint(s->opaque, false);
            }
            return true;
        }
    }
    if ((size == 1 || size == 2 || size == 4 || size == 8) &&
        address >= SHARED_RAM_BASE &&
        address <= SHARED_RAM_BASE + SHARED_RAM_SIZE - size) {
        if (commit) {
            uint8_t *target = s->shared_ram + address - SHARED_RAM_BASE;
            if (size == 8) stq_le_p(target, value);
            else if (size == 1) *target = value;
            else if (size == 2) stw_le_p(target, value);
            else stl_le_p(target, value);
        }
        return true;
    }
    uint32_t sdram_offset;
    if ((size == 1 || size == 2 || size == 4 || size == 8) &&
        cdj_c6747_emifb_sdram_offset(&s->emifb, address, size, SDRAM_SIZE,
                                    &sdram_offset)) {
        if (commit) {
            if (size == 8) stq_le_p(s->sdram + sdram_offset, value);
            else if (size == 1) s->sdram[sdram_offset] = value;
            else if (size == 2) stw_le_p(s->sdram + sdram_offset, value);
            else stl_le_p(s->sdram + sdram_offset, value);
        }
        return true;
    }
    if (address >= 0x00800000 && address < 0x00840000) address += 0x11000000;
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        address < L2_BASE || address > L2_BASE + L2_SIZE - size) return false;
    if (commit) {
        if (size == 8) stq_le_p(s->l2 + address - L2_BASE, value);
        else if (size == 1) s->l2[address - L2_BASE] = value;
        else if (size == 2) stw_le_p(s->l2 + address - L2_BASE, value);
        else stl_le_p(s->l2 + address - L2_BASE, value);
    }
    return true;
}

/* cdj_c674x_fetch fast path: dsp_memory_span covers exactly dsp_read's RAM
 * windows, which are disjoint from every peripheral, and a 32-byte-aligned
 * block lies wholly inside one window or is refused (then fetch uses
 * dsp_read). */
static const uint8_t *dsp_fetch_block(void *opaque, uint32_t block)
{
    return dsp_memory_span(opaque, block, 32);
}

static void dsp_cycle_tick(void *opaque)
{
    NxsHpi *s = opaque;
    uint64_t transfers = s->wm8740.transfers;
    if (!cdj_c674x_loop_functional_timing())
        cdj_spi_core_tick(s->spis, &s->wm8740, &s->spi_transfer, &s->pll);
    if (s->wm8740.transfers != transfers)
        info_report("nxs-spi: timed WM8740 latch word=%#x transfers=%" PRIu64,
                    s->wm8740.last_word, s->wm8740.transfers);
    cdj_c6747_pll_tick(&s->pll);
    /* SPRUH91D chapter 28 counts on the timer input clock; this callback is
     * the only per-cycle hook the core offers, so one call is one input clock
     * period - an approximation of nothing measurable.  It is declared in BOTH
     * provenance artifacts, because both reach this line: tools/cdj_dsp/replay.py
     * for replay runs and tools/cdj_main/nxs_vm.py for the QEMU firmware boots
     * that write runs/<run>/dsp-checkpoints/manifest.json.  This is the board a
     * "firmware delay loop terminated" observation would be made on, so the
     * declaration there is load-bearing, not decorative.  The events themselves
     * are Table 2-1's, mapped by cdj_c6747_timer_event(). */
    uint32_t timer_outputs = cdj_c6747_timers_tick(s->timers);
    for (unsigned bit = 0; timer_outputs >> bit; ++bit)
        if (timer_outputs & (1u << bit))
            cdj_c6747_intc_deliver_event(&s->intc, &s->intc_delivery,
                                         cdj_c6747_timer_event(bit));
}

static bool report_dsp(NxsHpi *s, const char *reason)
{
    capture_requested_checkpoint(s);
    /* Throttle routine reports by virtual time, so a future change in DSP
     * scheduling granularity cannot multiply checkpoint output. */
    if (s->virtual_audio_clock && !s->cpu.fault && !s->hpi.hint) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (s->mcasp_have_report && now >= s->mcasp_last_report_ns &&
            now - s->mcasp_last_report_ns < MCASP_VIRTUAL_REPORT_NS)
            return false;
        s->mcasp_last_report_ns = now;
        s->mcasp_have_report = true;
    }
    if (s->cpu.fault && s->fault_history_path) {
        FILE *history = fopen(s->fault_history_path, "w");
        if (!history) {
            error_report("nxs-c674x: cannot write DSP fault history %s",
                         s->fault_history_path);
        } else {
            uint32_t first = s->fault_history_next > DSP_FAULT_HISTORY_COUNT ?
                s->fault_history_next - DSP_FAULT_HISTORY_COUNT : 0;
            for (uint32_t n = first; n < s->fault_history_next; ++n) {
                const DspFaultHistory *item =
                    &s->fault_history[n % DSP_FAULT_HISTORY_COUNT];
                fprintf(history,
                        "{\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64
                        ",\"phase\":%u,\"pc\":%u,\"a8\":%u,\"b5\":%u"
                        ",\"b15\":%u,\"b3\":%u,\"csr\":%u,\"irp\":%u"
                        ",\"ilc\":%u,\"tsr\":%u,\"itsr\":%u,\"loop_active\":%s}\n",
                        item->packets, item->cycles, item->phase, item->pc,
                        item->a8, item->b5, item->b15, item->b3, item->csr,
                        item->irp, item->ilc, item->tsr, item->itsr,
                        item->loop_active ? "true" : "false");
            }
            if (fclose(history))
                error_report("nxs-c674x: cannot close DSP fault history %s",
                             s->fault_history_path);
        }
    }
    info_report("nxs-c674x: packets=%" PRIu64 " cycles=%" PRIu64
                " pc=%#x word=%#x stop=%s B15=%#x B14=%#x B3=%#x",
                s->cpu.packets, s->cpu.cycles, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                s->cpu.fault_word, reason, s->cpu.r[1][15], s->cpu.r[1][14], s->cpu.r[1][3]);
    record_event(s, "dsp_stop", 0, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                 s->cpu.fault_word, 0);
    if (!s->scheduler.mode || !s->scheduler.pending || s->dsp_halted ||
        !(s->scheduler.slice_id % 256)) capture_checkpoint(s, reason);
    return true;
}

static void execute_dsp(NxsHpi *s, unsigned quota)
{
    if (!s->dsp_started || s->dsp_halted || s->dsp_running) return;
    int64_t entered_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    s->dsp_running = true;
    const char *reason = s->scheduler.mode ? "deferred slice boundary" :
                                           "phase budget exhausted";
    unsigned steps = 0;
    /* The host may have changed memory since the last activation. */
    s->idle_anchor_valid = false;
    bool idle_skip = s->idle_skip && !s->scheduler.mode &&
                     !s->virtual_audio_clock && !s->fault_history_path;
    while (steps < quota) {
        if (s->fault_history_path) {
            DspFaultHistory *item = &s->fault_history[
                s->fault_history_next++ % DSP_FAULT_HISTORY_COUNT];
            *item = (DspFaultHistory){s->cpu.packets, s->cpu.cycles,
                s->cpu.pc, s->cpu.r[0][8], s->cpu.r[1][5],
                s->cpu.r[1][15], s->cpu.r[1][3], s->cpu.control[1],
                s->cpu.control[6], s->cpu.control[13], s->cpu.control[26],
                s->cpu.control[27],
                0, s->cpu.loop_active};
        }
        deliver_edma_notifications(s);
        if (!cdj_c674x_interrupt(
                &s->cpu, cdj_c6747_intc_cpu_pending(&s->intc_delivery))) {
            reason = s->cpu.fault ? s->cpu.fault : "CPU interrupt stopped";
            s->dsp_halted = true;
            break;
        }
        if (s->fault_history_path) {
            DspFaultHistory *item = &s->fault_history[
                s->fault_history_next++ % DSP_FAULT_HISTORY_COUNT];
            *item = (DspFaultHistory){s->cpu.packets, s->cpu.cycles,
                s->cpu.pc, s->cpu.r[0][8], s->cpu.r[1][5],
                s->cpu.r[1][15], s->cpu.r[1][3], s->cpu.control[1],
                s->cpu.control[6], s->cpu.control[13], s->cpu.control[26],
                s->cpu.control[27],
                1, s->cpu.loop_active};
        }
        if (idle_skip) {
            if (s->idle_anchor_valid && !s->idle_dirty &&
                steps != s->idle_anchor_step && dsp_idle_repeat(s) &&
                dsp_idle_quiescent(s)) {
                uint64_t k = dsp_idle_periods(s, quota - steps);
                if (k) {
                    uint64_t period = s->cpu.packets - s->idle_anchor_packets;
                    uint64_t from = s->cpu.packets;
                    dsp_idle_skip(s, k);
                    steps += k * period;
                    if (!dsp_idle_skip_slots(s, from)) {
                        reason = s->cpu.fault;
                        s->dsp_halted = true;
                        break;
                    }
                }
                dsp_idle_anchor(s, steps);
                if (steps >= quota) break;
            }
            if ((s->idle_dirty || !s->idle_anchor_valid ||
                 steps - s->idle_anchor_step > DSP_IDLE_WINDOW) &&
                dsp_idle_clean(s))
                dsp_idle_anchor(s, steps);
        }
        if (!cdj_c674x_step(&s->cpu, dsp_read, dsp_write, s)) {
            reason = s->cpu.fault ? s->cpu.fault : "CPU stopped";
            s->dsp_halted = true;
            break;
        }
        ++steps;
        if (s->spi_transfer.fault) {
            s->cpu.fault = "unsupported SPI transfer clock or state";
            s->cpu.fault_pc = s->cpu.pc;
            reason = s->cpu.fault;
            s->dsp_halted = true;
            break;
        }
        cdj_c6747_psc_tick(&s->psc);
        if (!functional_audio_tick(s)) {
            reason = s->cpu.fault;
            s->dsp_halted = true;
            break;
        }
        if (dsp_thread.on) {
            /* The real DSP keeps running after HINT; MAIN only waits for
             * the chunk to reach a packet boundary. */
            if (qatomic_read(&dsp_thread.host_waiting) ||
                qatomic_read(&dsp_thread.quit) ||
                (dsp_thread.main_target &&
                 s->cpu.packets >= dsp_thread.main_target)) {
                ++dsp_thread.host_breaks;
                break;
            }
            continue;
        }
        if (s->hpi.hint) { reason = "HINT host-event yield"; break; }
    }
    s->dsp_running = false;
    if (s->scheduler.mode) {
        if (!cdj_dsp_scheduler_end(&s->scheduler, steps, s->hpi.hint,
                                   s->dsp_halted)) {
            s->cpu.fault = "invalid deferred DSP slice completion";
            s->cpu.fault_pc = s->cpu.pc;
            s->dsp_halted = true;
            reason = s->cpu.fault;
        }
        record_event(s, "dsp_slice_end", s->scheduler.activation_id,
                     s->scheduler.slice_id, s->scheduler.remaining, steps);
    }
    int64_t executed_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (dsp_thread.on && !s->dsp_halted) return;  /* see dsp_thread_run */
    bool reported = report_dsp(s, reason);
    if (reported)
        info_report("nxs-dsp-host-time: execution-ns=%" PRId64
                    " reporting-ns=%" PRId64,
                    executed_ns - entered_ns,
                    qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - executed_ns);
}

static void deferred_dsp_tick(void *opaque)
{
    NxsHpi *s = opaque;
    if (!s->dsp_started || s->dsp_halted || !s->scheduler.pending) return;
    unsigned quota = cdj_dsp_scheduler_begin(&s->scheduler);
    if (!quota) {
        s->cpu.fault = "invalid deferred DSP scheduler state";
        s->cpu.fault_pc = s->cpu.pc;
        s->dsp_halted = true;
        report_dsp(s, s->cpu.fault);
        return;
    }
    record_event(s, "dsp_slice_begin", s->scheduler.activation_id,
                 s->scheduler.slice_id, s->scheduler.remaining, quota);
    execute_dsp(s, quota);
    /* Future relative to callback completion: never catch up in this timer
     * dispatch pass. This is a host fairness policy, not a DSP clock ratio. */
    if (s->scheduler.pending && !s->dsp_halted)
        timer_mod(s->dsp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
}

static void run_dsp(NxsHpi *s)
{
    if (s->model) return;       /* nothing executes; see model_service */
    if (dsp_thread.on) {        /* it runs anyway; just cut a pacing wait */
        qemu_cond_signal(&dsp_thread.wake);
        return;
    }
    if (!s->dsp_started || s->dsp_halted || s->dsp_running) return;
    if (!s->scheduler.mode) {
        execute_dsp(s, s->virtual_audio_clock ?
                    MIN(s->legacy_budget, MCASP_VIRTUAL_DSP_QUOTA) :
                    s->legacy_budget);
        return;
    }
    if (!cdj_dsp_scheduler_request(&s->scheduler)) {
        s->cpu.fault = "invalid deferred DSP scheduling request";
        s->cpu.fault_pc = s->cpu.pc;
        s->dsp_halted = true;
        report_dsp(s, s->cpu.fault);
        return;
    }
    record_event(s, "dsp_schedule", s->scheduler.activation_id,
                 s->scheduler.slice_id, s->scheduler.remaining,
                 s->scheduler.pending | (s->scheduler.rearm << 1));
    if (!timer_pending(s->dsp_timer))
        timer_mod(s->dsp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
}

/* Packets the DSP should have executed by virtual time @ns. */
static uint64_t dsp_thread_packets_at(int64_t ns)
{
    NxsDspThread *t = &dsp_thread;
    return t->epoch_packets +
           (uint64_t)MAX(ns - t->epoch_ns, 0) * t->packets_per_us / 1000;
}

/* Give up DSP time the DSP is behind virtual time.  Under dsp_thread.lock,
 * with the DSP started. */
static void dsp_thread_slip(NxsHpi *s, int64_t virt)
{
    NxsDspThread *t = &dsp_thread;
    uint64_t due = dsp_thread_packets_at(virt);
    if (due <= s->cpu.packets) return;
    int64_t lag = (int64_t)((due - s->cpu.packets) * 1000 / t->packets_per_us);
    t->lag_max_ns = MAX(t->lag_max_ns, lag);
    t->epoch_ns += lag;
    t->slipped_ns += lag;
}

#define DSP_THREAD_CHUNK 65536u

static void *dsp_thread_run(void *opaque)
{
    NxsHpi *s = opaque;
    NxsDspThread *t = &dsp_thread;
    qemu_mutex_lock(&t->lock);
    while (!t->quit) {
        if (!s->dsp_started || s->dsp_halted) {
            qemu_cond_broadcast(&t->progress);
            qemu_cond_wait(&t->wake, &t->lock);
            continue;
        }
        int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        dsp_thread_slip(s, virt);
        uint64_t limit = dsp_thread_packets_at(virt + t->quantum_ns);
        /* MAIN blocked on the DSP holds virtual time (always under icount):
         * serve it past the pacing limit, or the two would wait for each
         * other. */
        limit = MAX(limit, t->main_target);
        if (s->cpu.packets >= limit) {
            /* A quantum ahead: wait for MAIN's clock (or a paused VM). */
            ++t->waits;
            qemu_cond_timedwait(&t->wake, &t->lock,
                                MAX(t->quantum_ns / 1000000, 1));
            continue;
        }
        uint64_t quota = MIN(limit - s->cpu.packets, DSP_THREAD_CHUNK);
        if (t->main_target > s->cpu.packets)
            quota = MIN(quota, t->main_target - s->cpu.packets);
        /* Idle skip stays within the quota, so this never passes @limit. */
        execute_dsp(s, quota);
        ++t->chunks;
        qemu_cond_broadcast(&t->progress);
        if (virt - t->report_ns >= 10 * NANOSECONDS_PER_SECOND) {
            info_report("nxs-dsp-thread: virtual=%.3fs packets=%" PRIu64
                        " idle-skipped=%" PRIu64 " lag-max=%.3fs slipped=%.3fs"
                        " main-waits=%" PRIu64 " main-wait=%.3fs"
                        " chunks=%" PRIu64 " host-breaks=%" PRIu64
                        " pacing-waits=%" PRIu64 " pc=%#x",
                        virt / 1e9, s->cpu.packets, s->idle_skipped_packets,
                        t->lag_max_ns / 1e9, t->slipped_ns / 1e9,
                        t->main_waits, t->main_wait_ns / 1e9, t->chunks,
                        t->host_breaks, t->waits, s->cpu.pc);
            t->report_ns = virt;
        }
        /* Hand the lock to MAIN before taking the next chunk: one waiting
         * for it, or one woken from @progress that must retake it. */
        if (qatomic_read(&t->host_waiting) ||
            (t->main_target && s->cpu.packets >= t->main_target)) {
            uint64_t packets = s->cpu.packets;
            qemu_mutex_unlock(&t->lock);
            while (!qatomic_read(&t->quit) &&
                   (qatomic_read(&t->host_waiting) ||
                    (qatomic_read(&t->main_target) &&
                     packets >= qatomic_read(&t->main_target))))
                sched_yield();
            qemu_mutex_lock(&t->lock);
        }
    }
    qemu_cond_broadcast(&t->progress);
    qemu_mutex_unlock(&t->lock);
    return NULL;
}

static void dsp_thread_deliver_hint(void *opaque)
{
    dsp_thread.real_hint(dsp_thread.real_opaque,
                         qatomic_read(&dsp_thread.hint_high));
}

/* s->hint in threaded mode; always called with dsp_thread.lock held. */
static void dsp_thread_hint(void *opaque, bool high)
{
    qatomic_set(&dsp_thread.hint_high, high);
    if (bql_locked()) dsp_thread_deliver_hint(opaque);
    else qemu_bh_schedule(dsp_thread.hint_bh);
}

static void dsp_thread_shutdown(Notifier *notifier, void *data)
{
    /* Before the WAV/audio notifiers (registered earlier, so run later). */
    main_lock();
    qatomic_set(&dsp_thread.quit, true);
    qemu_cond_signal(&dsp_thread.wake);
    main_unlock();
    qemu_thread_join(&dsp_thread.thread);
    info_report("nxs-dsp-thread: stopped packets=%" PRIu64
                " lag-max=%.3fs slipped=%.3fs main-waits=%" PRIu64
                " main-wait=%.3fs chunks=%" PRIu64 " host-breaks=%" PRIu64,
                nxs_hpi->cpu.packets, dsp_thread.lag_max_ns / 1e9,
                dsp_thread.slipped_ns / 1e9, dsp_thread.main_waits,
                dsp_thread.main_wait_ns / 1e9, dsp_thread.chunks,
                dsp_thread.host_breaks);
}

/* Experimental independent McASP event source. QEMU virtual time is host
 * paced without icount; batching avoids 88,200 timer callbacks per second.
 * The coupled McASP2 DIT path remains a functional approximation. */
static void virtual_audio_tick(void *opaque)
{
    NxsHpi *s = opaque;
    if (!s->dsp_started || s->dsp_halted) return;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t numerator;
    uint32_t denominator;
    unsigned slots = s->mcasp_control.afsxctl[1] >> 7;
    bool active = (s->mcasp_control.gblctl[1] & 0x1f00u) == 0x1f00u;
    if (!active || slots < 2 || slots > 32 ||
        !cdj_c6747_mcasp_tx_clock_hz(&s->mcasp_control, 1,
            cdj_c6747_pll_auxclk_hz(), CDJ_C6747_MCASP_AFSX,
            &numerator, &denominator)) {
        s->mcasp_last_ns = now;
        s->mcasp_phase = s->mcasp_debt = 0;
    } else if (s->mcasp_last_ns > 0 && now >= s->mcasp_last_ns) {
        uint64_t elapsed = MIN((uint64_t)(now - s->mcasp_last_ns),
                               UINT64_C(1000000000));
        uint64_t divisor = (uint64_t)denominator * 1000000000u;
        uint64_t scaled = s->mcasp_phase + elapsed * numerator * slots;
        uint64_t due = scaled / divisor;
        s->mcasp_phase = scaled % divisor;
        s->mcasp_debt = MIN(s->mcasp_debt + due, UINT64_C(1000000));
        unsigned count = MIN(s->mcasp_debt, MCASP_VIRTUAL_MAX_SLOTS);
        for (unsigned index = 0; index < count; ++index) {
            if (!advance_functional_mcasp_slots(s)) {
                s->cpu.fault = s->tx_capture_failed ?
                    "DSP transmit capture write failed" :
                    s->pcm_failed ?
                    "DSP-paced WAV output failed or format changed" :
                    "unsupported virtual McASP transmit slot";
                s->cpu.fault_pc = s->cpu.pc;
                s->cpu.fault_word = 0;
                s->dsp_halted = true;
                report_dsp(s, s->cpu.fault);
                return;
            }
        }
        s->mcasp_debt -= count;
    }
    s->mcasp_last_ns = now;
    if (!s->hpi.hint) run_dsp(s);
    timer_mod(s->mcasp_timer, now + MCASP_VIRTUAL_BATCH_NS);
}

/*
 * Behavioural DSP (CDJ_NXS_DSP_MODEL=1). The uploaded C674x code never runs;
 * this reproduces only what MAIN can observe of it, measured from a stock
 * real-DSP transcript (runs/fork-stock-obey-1, CDJ_NXS_DSP_EVENTS) and the
 * re-docs main-dsp pages, and the transport from a fixed-binary real-DSP play
 * (runs/dsp-model-ref-3), whose vocabulary matches the upstream CDJ-2000
 * model (cdj2000_dsp_model.c). No audio and no decoder: position follows
 * QEMU virtual time at MAIN's rate word, so a run on this model is fast and
 * keeps MAIN's deadlines, but it is not evidence about audio or DSP timing.
 */
#define MODEL_WINDOW 0x11837ba0u        /* runtime window base (re-docs) */
#define MODEL_WINDOW_END 0x1183fb60u    /* end of the two stream records */
#define MODEL_CMD 0x11838100u           /* stream command; 2..4 accepted */
#define MODEL_REQUEST 0x11837ba0u       /* run request: 1 load, 2 play, 4 cue */
#define MODEL_RATE 0x11837bc0u          /* 12.20 rate, 0x100000 = 1.0 */
#define MODEL_SUBFRAME 0x11837bf4u      /* samples left in the frame, both halves */
#define MODEL_STATE 0x11837bf8u         /* the request last taken */
#define MODEL_FREE 0x11837cc8u          /* frames the stream buffer can take */
#define MODEL_BEHIND 0x11837cccu        /* buffered frames already played */
#define MODEL_AHEAD 0x11837cd0u         /* buffered frames not yet played */
#define MODEL_HEADER 0x11838140u        /* stream header; +4 = its frames */
#define MODEL_FRAME_SAMPLES 588u        /* 44.1 kHz / 75 CD frames */

static uint32_t model_get(NxsHpi *s, uint32_t address)
{
    return ldl_le_p(s->l2 + address - L2_BASE);
}

static void model_put(NxsHpi *s, uint32_t address, uint32_t value)
{
    stl_le_p(s->l2 + address - L2_BASE, value);
}

/* A DSP-side HPIC store, with the same effects and transcript record. */
static void model_hpic(NxsHpi *s, uint32_t value)
{
    bool old_hint = s->hpi.hint;
    cdj_c6747_hpi_cpu_write(&s->hpi, CDJ_C6747_HPIC, value, 4, true);
    record_event(s, "dsp_hpic_write", 0, CDJ_C6747_HPIC, value, 4);
    if (!old_hint && s->hpi.hint && s->hint) s->hint(s->opaque, false);
}

static void model_start(NxsHpi *s)
{
    s->dsp_started = true;
    s->model_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    record_event(s, "dsp_start", 0, ldl_le_p(s->l2), 0, 0);
    /* Stage 1 acks DSPINT (0x14a, then 0x149/0x148 as transcribed), clears
     * the handshake pair and writes ready=1 (0x1180304c); MAIN tests for 1. */
    model_hpic(s, 0x14a);
    model_hpic(s, 0x149);
    model_hpic(s, 0x148);
    model_put(s, 0x1183fff0, 0);
    model_put(s, 0x1183fff4, 1);
    info_report("nxs-hpi: behavioural DSP model; C674x code is not executed");
}

static void model_boot_phase(NxsHpi *s, unsigned phase)
{
    if (!s->dsp_started || !phase) return;
    /* Each nonzero CPU_PH value is answered with HINT (HPIC 0x14c). */
    model_hpic(s, 0x14c);
    if (phase != 3) return;
    /* Phase 3 starts the runtime: by MAIN's next read the DSP has cleared
     * the window MAIN's second record overlapped, set 0x11837bf8=1 (read
     * every service step) and 0x11837cc8=0x1a24. Values as transcribed. */
    memset(s->l2 + MODEL_WINDOW - L2_BASE, 0, MODEL_WINDOW_END - MODEL_WINDOW);
    model_put(s, MODEL_STATE, 1);
    model_put(s, MODEL_FREE, 0x1a24);
}

/* The CD frame reached, as the DSP publishes it (0x11837c10 and its four
 * copies), and the samples left before the next one. */
static void model_publish_position(NxsHpi *s)
{
    static const uint32_t copies[] = {
        0x11837c10, 0x11837c24, 0x11837c30, 0x11837c44, 0x11837c50,
    };
    uint32_t frame = s->model_samples / MODEL_FRAME_SAMPLES;
    uint32_t left = MODEL_FRAME_SAMPLES - s->model_samples % MODEL_FRAME_SAMPLES;
    for (size_t i = 0; i < ARRAY_SIZE(copies); ++i)
        model_put(s, copies[i], frame);
    model_put(s, MODEL_SUBFRAME, left << 16 | left);
}

/* Play the virtual time since the last service at MAIN's rate, moving whole
 * frames from ahead to behind; a dry buffer stops the position as an
 * underrun would. ponytail: the rate word applies over the whole interval;
 * per-write rate changes would need MAIN's write times. */
static void model_advance(NxsHpi *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->model_ns;
    s->model_ns = now;
    if (model_get(s, MODEL_STATE) != 2 || elapsed <= 0) return;
    /* Totals, not per-tick truncation, so the position cannot drift. */
    s->model_audio_ns += (uint64_t)elapsed * model_get(s, MODEL_RATE) >> 20;
    uint32_t frame = s->model_samples / MODEL_FRAME_SAMPLES;
    uint32_t ahead = model_get(s, MODEL_AHEAD);
    uint64_t limit = (uint64_t)(frame + ahead) * MODEL_FRAME_SAMPLES;
    s->model_samples = s->model_audio_ns * 44100u / 1000000000u;
    if (s->model_samples > limit) {
        s->model_samples = limit;
        s->model_audio_ns = limit * 1000000000u / 44100u;
    }
    uint32_t played = s->model_samples / MODEL_FRAME_SAMPLES - frame;
    model_put(s, MODEL_AHEAD, ahead - played);
    model_put(s, MODEL_BEHIND, model_get(s, MODEL_BEHIND) + played);
    model_publish_position(s);
}

/* A new stream's first header (byte 2 = 1) names its buffer (byte 1: 1 the
 * deck's stream, 2 the second one). Its status block takes the record, the
 * transcribed 0x128 (+0xc; meaning not established) and, for the deck's
 * stream, -1 in +0x10..+0x18; the deck's stream also restarts the position
 * blocks, which count from the stream's start (dsp-model-ref-3). */
static void model_bind(NxsHpi *s, unsigned buffer)
{
    uint32_t block = buffer ? 0x11838180 : 0x118381a0;
    s->model_pending = false;
    s->model_received[buffer] = 0;
    s->model_total[buffer] = s->model_length;
    model_put(s, block + 4, s->model_record);
    model_put(s, block + 0xc, 0x128);
    if (buffer) return;
    model_put(s, 0x11838184, s->model_record);   /* until a second stream */
    for (uint32_t at = 0x10; at <= 0x18; at += 4)
        model_put(s, block + at, UINT32_MAX);
    model_put(s, 0x11837c14, s->model_record);
    model_put(s, 0x11837c34, s->model_record);
    model_put(s, 0x11837c1c, 0x128);
    model_put(s, 0x11837c3c, 0x128);
    model_put(s, 0x11837c58, 0x10001);
    s->model_samples = s->model_audio_ns = 0;
    model_publish_position(s);
}

/* One DSPINT: ack it and consume the mailboxes MAIN polls for zero. */
static void model_service(NxsHpi *s)
{
    static const uint32_t cleared[] = {
        0x11837c9c, 0x11837cb0, 0x118381c4,
    };
    model_hpic(s, 0x14a);
    model_advance(s);
    uint32_t request = model_get(s, MODEL_REQUEST);
    if (request && request < 0x100) {
        /* Taken requests become the published state (2 play, 4 cue). */
        model_put(s, MODEL_STATE, request);
        model_put(s, MODEL_REQUEST, 0);
    }
    uint32_t header = model_get(s, MODEL_HEADER);
    if (header) {
        /* A stream header announces +0x8144 frames into buffer byte 1:
         * 1 is the track (level 0x7cd0, status 0x81a0), 2 the second
         * stream (level 0x7ccc, status 0x8180). Status +0 is the last
         * frame received, as transcribed (0x77 after three 40s). */
        uint32_t frames = model_get(s, MODEL_HEADER + 4) & 0xffff;
        unsigned buffer = (header >> 8 & 0xff) == 2;
        if (s->model_pending && (header >> 16 & 0xff) == 1)
            model_bind(s, buffer);
        uint32_t level = buffer ? MODEL_BEHIND : MODEL_AHEAD;
        model_put(s, level, model_get(s, level) + frames);
        model_put(s, MODEL_FREE, model_get(s, MODEL_FREE) - frames);
        s->model_received[buffer] += frames;
        /* The track counts up from its start; the auxiliary stream down
         * from its end (0x6d06 = 0x6d9e - 2 x 0x4c in dsp-model-ref-3). */
        model_put(s, buffer ? 0x11838180 : 0x118381a0, buffer ?
                  s->model_total[1] - s->model_received[1] : s->model_received[0] - 1);
        model_put(s, MODEL_HEADER, 0);
    }
    uint32_t command = model_get(s, MODEL_CMD);
    if (command >= 2 && command <= 4) {
        /* Consumer 0xc004834c: clears 0x11838140/0x118381c4, then the
         * command word. Other values stay put, as on the DSP. */
        /* The stream's record (+0x20, an instance id MAIN counts down
         * from 0xff) and length (+0x1c) belong to the buffer its first
         * header names; see model_bind. */
        s->model_record = model_get(s, 0x11838120);
        s->model_length = model_get(s, 0x1183811c);
        s->model_pending = true;
        model_put(s, 0x11838140, 0);
        model_put(s, 0x118381c4, 0);
        model_put(s, MODEL_CMD, 0);
    }
    for (size_t i = 0; i < ARRAY_SIZE(cleared); ++i)
        if (model_get(s, cleared[i])) model_put(s, cleared[i], 0);
    record_event(s, "dsp_stop", 0, 0, 0, 0);
}

static void start_dsp(NxsHpi *s)
{
    if (s->model) {
        model_start(s);
        return;
    }
    /* Boot-ROM handoff abstraction: the host supplies the entry in L2[0].
     * No claim to execute the unavailable ROM. The uploaded code is decoded. */
    cdj_c674x_reset(&s->cpu, ldl_le_p(s->l2));
    s->cpu.cycle_tick = dsp_cycle_tick;
    s->cpu.cycle_opaque = s;
    s->dsp_started = true;
    if (dsp_thread.on) {
        dsp_thread.epoch_ns = dsp_thread.report_ns =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        dsp_thread.epoch_packets = dsp_thread.main_last = s->cpu.packets;
    }
    if (s->mcasp_timer)
        timer_mod(s->mcasp_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MCASP_VIRTUAL_BATCH_NS);
    record_event(s, "dsp_start", 0, s->cpu.pc, 0, 0);
    capture_checkpoint(s, "DSP start boundary");
    run_dsp(s);
    info_report("nxs-pll: oscin-cycles=%" PRIu64 " reset-age=%u lock-wait-remaining=%u early-enable=%d",
                s->pll.oscin_cycles, s->pll.reset_age, s->pll.lock_wait_remaining,
                s->pll.early_enable);
    info_report("nxs-syscfg: cfgchip=%#x,%#x,%#x,%#x amute-clear-pulses=%#x",
                s->syscfg.cfgchip[0], s->syscfg.cfgchip[1], s->syscfg.cfgchip[2],
                s->syscfg.cfgchip[3], s->syscfg.amute_clear_pulses);
}

static uint64_t hpi_read_locked(void *opaque, hwaddr offset, unsigned size)
{
    NxsHpi *s = opaque;
    capture_requested_checkpoint(s);
    uint32_t result = 0xffffffff;
    uint32_t address = s->address;
    bool data_access = offset == 0x80000 || offset == 0xc0000;
    bool data_valid = data_access && valid_data(s);
    if (offset == 0) result = cdj_c6747_hpi_host_read(&s->hpi);
    else if (offset == 0x40000) result = s->address;
    if (data_valid) {
        result = ldl_le_p(host_memory(s, s->address));
        if (offset == 0x80000) s->address += 4;
    } else if (offset != 0 && offset != 0x40000) {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported read offset=%" HWADDR_PRIx " address=%#x HWOB=%d reset=%d\n",
                      offset, s->address, s->hpi.hwob, s->hpi.hpirst);
    }
    /* HPIC polling is DSP-to-MAIN observation, not an external stimulus, and
     * produced hundreds of thousands of redundant records. Data-port reads do
     * advance HPIA and therefore remain ordered state-changing events. */
    if (data_valid)
        record_event(s, "hpi_host_data_read", offset, address, result, size);
    return result;
}

static void hpi_write_locked(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    NxsHpi *s = opaque;
    uint32_t address = s->address;
    if (offset == 0) {
        bool old_hint = s->hpi.hint, old_dspint = s->hpi.dspint;
        cdj_c6747_hpi_host_write(&s->hpi, value);
        bool dspint_rising = !old_dspint && s->hpi.dspint;
        if (dspint_rising)
            cdj_c6747_intc_deliver_event(&s->intc, &s->intc_delivery, 34);
        record_event(s, "hpi_host_control_write", offset, address, value, size);
        if (old_hint != s->hpi.hint && s->hint) s->hint(s->opaque, !s->hpi.hint);
        if (dspint_rising && !s->dsp_started) {
            info_report("nxs-hpi: DSPINT after %" PRIu64 " written words; starting partial C674x interpreter", s->words);
            const char *path = getenv("CDJ_NXS_HPI_DUMP");
            if (path && *path) {
                FILE *file = fopen(path, "wb");
                if (!file) error_report("nxs-hpi: cannot open L2 dump %s", path);
                else {
                    size_t count = fwrite(s->l2, 1, sizeof(s->l2), file);
                    int status = fclose(file);
                    if (count != sizeof(s->l2) || status) error_report("nxs-hpi: incomplete L2 dump");
                }
            }
            start_dsp(s);
        } else if (old_hint && !s->hpi.hint) {
            run_dsp(s);
        } else if (dspint_rising && s->dsp_started) {
            if (s->model) model_service(s);
            else run_dsp(s);
        }
        return;
    }
    if (offset == 0x40000) {
        s->address = value;
        record_event(s, "hpi_host_address_write", offset, address, value, size);
        return;
    }
    if ((offset == 0x80000 || offset == 0xc0000) && valid_data(s)) {
        stl_le_p(host_memory(s, s->address), value);
        ++s->words;
        if (offset == 0x80000) s->address += 4;
        record_event(s, offset == 0x80000 ? "hpi_host_data_autoincrement_write" :
                     "hpi_host_data_fixed_write", offset, address, value, size);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported write offset=%" HWADDR_PRIx " address=%#x HWOB=%d reset=%d\n",
                      offset, s->address, s->hpi.hwob, s->hpi.hpirst);
    }
}

/* MAIN side of dsp_thread.lock; a no-op in the synchronous default.  Takes the
 * lock, then waits (BQL released) until the running DSP has executed
 * access_packets since MAIN's previous access, and returns with both held. */
static void main_lock(void)
{
    NxsDspThread *t = &dsp_thread;
    NxsHpi *s = nxs_hpi;
    if (!t->on) return;
    bool bql = bql_locked(), dropped = false;
    if (qemu_mutex_trylock(&t->lock)) {
        qatomic_inc(&t->host_waiting);
        if (bql) {
            bql_unlock();
            dropped = true;
        }
        qemu_mutex_lock(&t->lock);
        qatomic_dec(&t->host_waiting);
    }
    if (s->dsp_started && !s->dsp_halted && !t->quit) {
        uint64_t target = t->main_last + t->access_packets;
        if (s->cpu.packets < target) {
            if (bql && !dropped) {
                bql_unlock();
                dropped = true;
            }
            int64_t start = get_clock();
            ++t->main_waits;
            qatomic_set(&t->main_target, target);
            qemu_cond_signal(&t->wake);
            while (s->cpu.packets < target && s->dsp_started &&
                   !s->dsp_halted && !t->quit)
                qemu_cond_wait(&t->progress, &t->lock);
            qatomic_set(&t->main_target, 0);
            t->main_wait_ns += get_clock() - start;
        }
    }
    /* Holding @lock while waiting for the BQL is safe: no BQL holder ever
     * blocks on @lock (it tries, then drops the BQL as above). */
    if (dropped) bql_lock();
}

static void main_unlock(void)
{
    if (!dsp_thread.on) return;
    dsp_thread.main_last = nxs_hpi->cpu.packets;
    qemu_mutex_unlock(&dsp_thread.lock);
}

void cdj_nxs_hpi_reset_line(bool released)
{
    if (!nxs_hpi) return;
    main_lock();
    reset_line(nxs_hpi, released);
    main_unlock();
}

void cdj_nxs_hpi_boot_phase(unsigned phase)
{
    if (!nxs_hpi) return;
    main_lock();
    boot_phase(nxs_hpi, phase);
    main_unlock();
}

static uint64_t hpi_read(void *opaque, hwaddr offset, unsigned size)
{
    main_lock();
    uint64_t value = hpi_read_locked(opaque, offset, size);
    main_unlock();
    return value;
}

static void hpi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    main_lock();
    hpi_write_locked(opaque, offset, value, size);
    main_unlock();
}

static const MemoryRegionOps hpi_ops = {
    .read = hpi_read, .write = hpi_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void cdj_nxs_hpi_init(MemoryRegion *system, void (*hint)(void *, bool), void *opaque)
{
    NxsHpi *s = g_new0(NxsHpi, 1);
    const char *timing = getenv("CDJ_NXS_DSP_FUNCTIONAL_TIMING");
    const char *audio = getenv("CDJ_NXS_DSP_FUNCTIONAL_AUDIO");
    const char *virtual_audio = getenv("CDJ_NXS_DSP_VIRTUAL_MCASP");
    const char *cycle_audio = getenv("CDJ_NXS_DSP_CYCLE_MCASP");
    const char *host_audio = getenv("CDJ_NXS_DSP_HOST_AUDIO");
    const char *pcm_wav = getenv("CDJ_NXS_DSP_PCM_WAV");
    const char *scheduler = getenv("CDJ_NXS_DSP_SCHEDULER");
    const char *fault_history = getenv("CDJ_NXS_DSP_FAULT_HISTORY");
    if (fault_history && *fault_history)
        s->fault_history_path = g_strdup(fault_history);
    const char *legacy_budget = getenv("CDJ_NXS_DSP_LEGACY_BUDGET");
    if (!cdj_dsp_legacy_budget_parse(legacy_budget, &s->legacy_budget)) {
        error_report("nxs-c674x: invalid legacy DSP budget %s; expected %u..%u packets",
                     legacy_budget ? legacy_budget : "(null)",
                     CDJ_DSP_LEGACY_BUDGET_MIN,
                     CDJ_DSP_LEGACY_BUDGET_DEFAULT);
        exit(EXIT_FAILURE);
    }
    if (scheduler && !strcmp(scheduler, "deferred-v1")) {
        cdj_dsp_scheduler_reset(&s->scheduler);
        s->dsp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, deferred_dsp_tick, s);
        warn_report("nxs-c674x: deferred-v1 host scheduling diagnostic; not a hardware clock model");
    } else if (scheduler && strcmp(scheduler, "legacy")) {
        error_report("nxs-c674x: unsupported DSP scheduler %s", scheduler);
        exit(EXIT_FAILURE);
    }
    if (s->legacy_budget != CDJ_DSP_LEGACY_BUDGET_DEFAULT)
        warn_report("nxs-c674x: legacy cooperative budget reduced to %u packets; exploratory host-fairness mode",
                    s->legacy_budget);
    nxs_hpi = s;
    const char *model = getenv("CDJ_NXS_DSP_MODEL");
    s->model = model && !strcmp(model, "1");
    if (s->model)
        warn_report("nxs-c674x: behavioural DSP model; no C674x execution, audio or playback position");
    cdj_c674x_loop_set_functional_timing(timing && !strcmp(timing, "1"));
    cdj_c674x_set_fetch_block(dsp_read, dsp_fetch_block);
    s->functional_audio = audio && !strcmp(audio, "1");
    const char *idle_skip = getenv("CDJ_NXS_DSP_IDLE_SKIP");
    s->idle_skip = idle_skip && !strcmp(idle_skip, "1");
    if (s->idle_skip)
        info_report("nxs-c674x: idle-loop skip enabled; proven DSP spin periods are advanced without execution");
    s->virtual_audio_clock = virtual_audio && !strcmp(virtual_audio, "1");
    s->cycle_audio_clock = cycle_audio && !strcmp(cycle_audio, "1");
    if ((s->virtual_audio_clock || s->cycle_audio_clock) &&
        (!s->functional_audio ||
         (s->virtual_audio_clock && s->cycle_audio_clock))) {
        error_report("nxs-c674x: select one McASP clock with functional audio");
        exit(EXIT_FAILURE);
    }
    if (s->virtual_audio_clock)
        s->mcasp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, virtual_audio_tick, s);
    if (pcm_wav && *pcm_wav) {
        if (!s->functional_audio) {
            error_report("nxs-c674x: DSP-paced WAV requires functional audio");
            exit(EXIT_FAILURE);
        }
        s->pcm_wav = fopen(pcm_wav, "wb+");
        if (!s->pcm_wav || !nxs_pcm_header(s->pcm_wav, 0)) {
            error_report("nxs-c674x: cannot open DSP-paced WAV %s", pcm_wav);
            exit(EXIT_FAILURE);
        }
        s->pcm_shutdown.notify = nxs_pcm_shutdown;
        qemu_register_shutdown_notifier(&s->pcm_shutdown);
        info_report("nxs-c674x: McASP1 serializer 0 -> DSP-paced stereo WAV %s",
                    pcm_wav);
    }
    if (host_audio && !strcmp(host_audio, "1")) {
        if (!s->virtual_audio_clock) {
            error_report("nxs-c674x: host audio requires virtual McASP clock");
            exit(EXIT_FAILURE);
        }
        Error *audio_error = NULL;
        struct audsettings settings = {
            .freq = NXS_AUDIO_RATE, .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16, .big_endian = HOST_BIG_ENDIAN,
        };
        s->audio_backend = audio_be_by_name("cdj-dsp", &audio_error);
        if (!s->audio_backend) {
            error_report_err(audio_error);
            exit(EXIT_FAILURE);
        }
        s->audio_ring = g_new0(int16_t, NXS_AUDIO_RING_FRAMES * 2);
        s->audio_priming = true;
        qemu_mutex_init(&s->audio_lock);
        s->audio_voice = audio_be_open_out(s->audio_backend, NULL,
                                           "cdj-nxs-mcasp1", s,
                                           nxs_audio_callback, &settings);
        if (!s->audio_voice) {
            error_report("nxs-c674x: cannot open 44.1 kHz stereo host voice");
            exit(EXIT_FAILURE);
        }
        audio_be_set_active_out(s->audio_backend, s->audio_voice, true);
        s->audio_shutdown.notify = nxs_audio_shutdown;
        qemu_register_shutdown_notifier(&s->audio_shutdown);
        info_report("nxs-c674x: McASP1 serializer 0 -> 44.1 kHz stereo host voice");
    }
    const char *tx_path = getenv("CDJ_NXS_DSP_TX_CAPTURE");
    if (tx_path && *tx_path) {
        const char *limit_text = getenv("CDJ_NXS_DSP_TX_CAPTURE_LIMIT");
        const char *nonzero_only = getenv("CDJ_NXS_DSP_TX_CAPTURE_NONZERO_ONLY");
        char *limit_end = NULL;
        s->tx_capture_nonzero_only = nonzero_only &&
                                     !strcmp(nonzero_only, "1");
        s->tx_capture_limit = 65536;
        if (limit_text && *limit_text) {
            errno = 0;
            uint64_t limit = g_ascii_strtoull(limit_text, &limit_end, 10);
            if (errno || !limit || !limit_end || *limit_end) {
                error_report("nxs-hpi: invalid DSP transmit capture limit %s",
                             limit_text);
                exit(EXIT_FAILURE);
            }
            s->tx_capture_limit = limit;
        }
        s->tx_capture_path = g_strdup(tx_path);
        s->tx_capture = fopen(s->tx_capture_path, "wb");
        if (!s->tx_capture) {
            s->tx_capture_failed = true;
            error_report("nxs-hpi: cannot open DSP transmit capture %s", tx_path);
        }
    }
    if (cdj_c674x_loop_functional_timing())
        warn_report("nxs-c674x: functional SPLOOPD timing enabled; run is not cycle-validation evidence");
    if (s->virtual_audio_clock)
        warn_report("nxs-c674x: experimental virtual-time McASP batches enabled; not hardware or host-audio validation");
    else if (s->cycle_audio_clock)
        warn_report("nxs-c674x: experimental SYSCLK1-cycle McASP slots enabled; QEMU host time remains independent");
    else if (s->functional_audio)
        warn_report("nxs-c674x: functional McASP slots every %u packets enabled; run is not audio-timing evidence",
                    CDJ_DSP_FUNCTIONAL_AUDIO_PACKET_INTERVAL);
    cdj_c6747_syscfg_reset(&s->syscfg);
    cdj_c6747_syscfg_priority_reset(&s->syscfg_priority);
    cdj_c6747_psc_reset(&s->psc);
    cdj_c6747_mcasp_reset(&s->mcasp);
    cdj_c6747_mcasp_control_reset(&s->mcasp_control);
    cdj_c6747_gpio_reset(&s->gpio);
    cdj_c6747_i2c_reset(&s->i2c);
    cdj_c6747_intc_reset(&s->intc);
    cdj_c6747_intc_delivery_reset(&s->intc_delivery);
    cdj_c6747_timers_reset(s->timers);
    cdj_c6747_spis_reset(s->spis);
    cdj_c6747_spi_transfer_reset(&s->spi_transfer);
    cdj_wm8740_reset(&s->wm8740);
    cdj_c6747_cache_reset(&s->cache);
    cdj_c6747_edma_reset(&s->edma);
    cdj_c6747_pll_reset(&s->pll);
    cdj_c6747_hpi_reset(&s->hpi);
    cdj_c6747_emifb_reset(&s->emifb);
    s->shared_ram = g_malloc0(SHARED_RAM_SIZE);
    s->sdram = g_malloc0(SDRAM_SIZE);
    s->hint = hint;
    s->opaque = opaque;
    const char *threaded = getenv("CDJ_NXS_DSP_THREAD");
    if (threaded && !strcmp(threaded, "1")) {
        NxsDspThread *t = &dsp_thread;
        if (s->model || s->scheduler.mode || s->virtual_audio_clock ||
            s->audio_voice || !hint) {
            error_report("nxs-c674x: the DSP thread needs the real C674x with "
                         "the legacy scheduler and no virtual McASP clock");
            exit(EXIT_FAILURE);
        }
        const char *rate = getenv("CDJ_NXS_DSP_THREAD_MPPS");
        const char *quantum = getenv("CDJ_NXS_DSP_THREAD_QUANTUM_US");
        const char *access = getenv("CDJ_NXS_DSP_THREAD_ACCESS_PACKETS");
        t->access_packets = access && *access ?
                            g_ascii_strtoull(access, NULL, 10) : 256;
        t->packets_per_us = rate && *rate ? g_ascii_strtoull(rate, NULL, 10) : 150;
        t->quantum_ns = (quantum && *quantum ?
                         g_ascii_strtoull(quantum, NULL, 10) : 1000) * 1000;
        if (!t->packets_per_us || t->packets_per_us > 100000 ||
            t->quantum_ns < 10000 || t->quantum_ns > NANOSECONDS_PER_SECOND ||
            t->access_packets > CDJ_DSP_LEGACY_BUDGET_DEFAULT) {
            error_report("nxs-c674x: invalid DSP thread rate, quantum or "
                         "packets per access");
            exit(EXIT_FAILURE);
        }
        t->on = true;
        t->real_hint = hint;
        t->real_opaque = opaque;
        t->hint_bh = qemu_bh_new(dsp_thread_deliver_hint, NULL);
        s->hint = dsp_thread_hint;
        qemu_mutex_init(&t->lock);
        qemu_cond_init(&t->wake);
        qemu_cond_init(&t->progress);
        qemu_thread_create(&t->thread, "cdj-nxs-c674x", dsp_thread_run, s,
                           QEMU_THREAD_JOINABLE);
        t->shutdown.notify = dsp_thread_shutdown;
        qemu_register_shutdown_notifier(&t->shutdown);
        warn_report("nxs-c674x: DSP on its own thread, %" PRIu64 "M packets/s "
                    "of virtual time, quantum %" PRId64 " us, %" PRIu64
                    " packets per MAIN access; no checkpoints, not replay "
                    "evidence", t->packets_per_us, t->quantum_ns / 1000,
                    t->access_packets);
    }
    if (s->hint) s->hint(s->opaque, true); /* UHPI_HINT is active low and idle high. */
    memory_region_init_io(&s->registers, NULL, &hpi_ops, s, "nxs.uhpi", 0x100000);
    memory_region_add_subregion(system, HPI_BASE, &s->registers);
}
