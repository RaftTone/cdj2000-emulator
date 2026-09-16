/* Deterministic host/device backend for extracted Blackfin wake code.
 * No proprietary firmware. The real simulator/CEC/SPORT and host OS are not
 * emulated here; live PLL/IDLE coverage is in test_bfin_wake_guest.py. */
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

typedef uint32_t bu32;
typedef struct {
    int64_t time_of_event, time_from_event;
    int work_pending, nr_ticks_to_process;
} sim_events;
struct sim_desc { sim_events events; };
typedef struct sim_desc *SIM_DESC;
typedef struct { bu32 pc, lc[2]; int did_jump; } SIM_CPU;
#define PCREG (cpu->pc)
#define LCREG(n) (cpu->lc[n])
#define BFIN_CPU_STATE (*cpu)
#define STATE_EVENTS(sd) (&(sd)->events)
#define SIM_ASSERT(condition) check((condition), "production event invariant")
static void check(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static struct { unsigned long events; } bfin_stats;
static double clock_now, link_at, requested_wait;
static int link_fd, wait_calls, work_calls, irq_unmasked, irq_pending;
static SIM_CPU *current_cpu;
static double bfin_wall_seconds(void) { return clock_now; }
static int bfin_sport_link_wait_fd(void) { return link_fd; }
static int fake_select(int nfds, fd_set *readfds, fd_set *writefds,
                       fd_set *exceptfds, struct timeval *timeout)
{
    check(!writefds && !exceptfds, "read-only link wait");
    check(timeout->tv_sec >= 0 && timeout->tv_usec >= 0 && timeout->tv_usec < 1000000,
          "valid select timeout");
    requested_wait = timeout->tv_sec + timeout->tv_usec / 1e6;
    ++wait_calls;
    if (link_fd >= 0 && nfds > link_fd && readfds && FD_ISSET(link_fd, readfds)
        && link_at >= clock_now && link_at <= clock_now + requested_wait) {
        clock_now = link_at;
        return 1;
    }
    clock_now += requested_wait;
    return 0;
}
#define select fake_select
static void sim_events_process(SIM_DESC sd);
#include "wake-production.inc"

/* Minimal backend honoring GNU sim's tick/process contract: callbacks occur
 * for deadlines < current_time + pending_ticks, then pending ticks advance
 * time. The time/tick/tickn implementations above are production source. */
struct event { int64_t when; int kind; };
static struct event queue[16];
static int queued, delivered;
static int64_t delivered_at[16];
static int delivered_kind[16];
static void set_head(SIM_DESC sd, int64_t current)
{
    check(queued > 0, "perpetual poll remains queued");
    sd->events.time_of_event = queue[0].when;
    sd->events.time_from_event = queue[0].when - current;
}
static void add_event(SIM_DESC sd, int64_t when, int kind)
{
    int64_t current = sim_events_time(sd);
    check(queued < 16 && when >= current, "bounded future event");
    int i = queued++;
    while (i && queue[i-1].when > when) { queue[i] = queue[i-1]; --i; }
    queue[i] = (struct event){when, kind};
    set_head(sd, current);
}
static void sim_events_process(SIM_DESC sd)
{
    int64_t now = sim_events_time(sd);
    int ticks = sd->events.nr_ticks_to_process;
    if (sd->events.work_pending) ++work_calls;
    sd->events.work_pending = 0;
    while (queue[0].when < now + ticks) {
        struct event event = queue[0];
        check(event.kind != 99 && delivered < 16, "bounded callback backend");
        memmove(queue, queue+1, (size_t)(--queued) * sizeof(*queue));
        set_head(sd, now);
        delivered_at[delivered] = event.when;
        delivered_kind[delivered++] = event.kind;
        if (event.kind == 1) {
            irq_pending = 1;
            if (irq_unmasked) current_cpu->pc = 0x800;
        } else if (event.kind == 2) {
            /* Model a non-interrupting PLL-style wake callback. */
            bfin_idle_wake_pending = 1;
        }
    }
    set_head(sd, now + ticks);
    sd->events.nr_ticks_to_process = 0;
}
static void reset(SIM_DESC sd, SIM_CPU *cpu)
{
    memset(sd, 0, sizeof(*sd)); memset(cpu, 0, sizeof(*cpu));
    memset(&bfin_wall, 0, sizeof(bfin_wall));
    bfin_wall.active = bfin_wall.init = 1;
    bfin_wall.cclk_hz = 1024; bfin_wall.lag_cap = 16;
    bfin_stats.events = 0; bfin_idle_hint = bfin_idle_wake_pending = 0;
    clock_now = requested_wait = 0; link_at = 100000; link_fd = -1;
    wait_calls = work_calls = irq_pending = 0; irq_unmasked = 1;
    current_cpu = cpu; cpu->pc = 0x100;
    queued = 1; queue[0] = (struct event){1000000, 99}; delivered = 0;
    set_head(sd, 0);
}
static void exercise(const char *name)
{
    struct sim_desc state; SIM_DESC sd = &state; SIM_CPU object, *cpu = &object;
    reset(sd, cpu);
    if (!strcmp(name, "deadline")) {
        add_event(sd, 20, 2);
        bfin_wall_deliver(sd, cpu, 19);
        check(sim_events_time(sd) == 19 && !delivered, "no event before deadline");
        bfin_wall_deliver(sd, cpu, 1);
        check(sim_events_time(sd) == 20 && !delivered && !bfin_stats.events,
              "deadline boundary not yet processed");
        bfin_wall_deliver(sd, cpu, 1);
        check(sim_events_time(sd) == 21 && delivered == 1 && delivered_at[0] == 20,
              "event dispatched at its deadline before next tick");
    } else if (!strcmp(name, "interrupt")) {
        add_event(sd, 3, 1); add_event(sd, 5, 2);
        bfin_wall_deliver(sd, cpu, 12);
        check(sim_events_time(sd) == 4 && delivered == 1 && cpu->pc == 0x800,
              "interrupt stops catch-up");
        check(delivered_at[0] == 3 && irq_pending, "interrupt callback ordering");
        bfin_wall_deliver(sd, cpu, 2);
        check(sim_events_time(sd) == 6 && delivered == 2 && delivered_kind[1] == 2
              && delivered_at[1] == 5, "later callback waits for resumed catch-up");
    } else if (!strcmp(name, "pending")) {
        add_event(sd, 50, 2); sd->events.work_pending = 1;
        bfin_wall_deliver(sd, cpu, 1);
        check(work_calls == 1 && !sd->events.work_pending && !delivered
              && sim_events_time(sd) == 1, "pending work does not fire future event");
    } else if (!strcmp(name, "same_pc")) {
        irq_unmasked = 0; add_event(sd, 3, 1); add_event(sd, 5, 2);
        clock_now = 10.0 / 1024;
        bfin_wall_sync(sd, cpu, 1);
        check(cpu->pc == 0x100 && delivered == 2 && irq_pending && bfin_idle_wake_pending,
              "masked callback and PLL-style wake preserve PC");
        check(!wait_calls && sim_events_time(sd) == 10,
              "same-PC event returns before distant wait");
        irq_unmasked = 1; add_event(sd, 10, 1); clock_now = 12.0 / 1024;
        bfin_wall_sync(sd, cpu, 1);
        check(cpu->pc == 0x800 && sim_events_time(sd) == 11,
              "unmasked callback stops sync before remaining ticks");
    } else if (!strcmp(name, "prepaid")) {
        add_event(sd, 1024, 2); bfin_wall_sync(sd, cpu, 1);
        check(wait_calls == 1 && !delivered && sim_events_time(sd) == 0
              && bfin_wall.prepaid >= 1023, "wait credits time without advancing guest");
        /* Small host wake overrun, substantially less than the ordinary cap. */
        clock_now = 1.001; bfin_wall_sync(sd, cpu, 1);
        check(!bfin_wall.dropped && delivered == 1 && delivered_at[0] == 1024
              && sim_events_time(sd) == 1025 && bfin_wall.prepaid == 0,
              "prepaid protects deliberate wait");
    } else if (!strcmp(name, "unpaid_lag")) {
        clock_now = 2; bfin_wall_sync(sd, cpu, 0);
        check(bfin_wall.dropped == 2032 && sim_events_time(sd) == 16
              && !wait_calls, "unpaid lag capped without waiting");
    } else if (!strcmp(name, "link")) {
        link_fd = 7; link_at = .125; add_event(sd, 1024, 2);
        bfin_wall_sync(sd, cpu, 1);
        check(wait_calls == 1 && clock_now == .125 && !delivered
              && sim_events_time(sd) == 0 && bfin_wall.prepaid == 128,
              "link wakes host wait");
        bfin_wall_sync(sd, cpu, 0);
        check(sim_events_time(sd) == 128 && !delivered && !bfin_wall.dropped,
              "link wake catch-up does not fabricate device event");
        reset(sd, cpu); link_at = .125; bfin_wall_wait(.5);
        check(clock_now == .5, "no descriptor means timeout-only wait");
    } else if (!strcmp(name, "spin")) {
        add_event(sd, 1, 2); clock_now = .0008; bfin_wall_sync(sd, cpu, 1);
        check(!wait_calls && bfin_wall.spins == 1 && !delivered
              && sim_events_time(sd) == 0, "short spin does not deliver future tick");
        bfin_wall_wait(0); bfin_wall_wait(-1);
        check(!wait_calls, "nonpositive wait never selects");
    } else if (!strcmp(name, "hardware_loop")) {
        cpu->did_jump = 1;
        check(classify_parked(cpu, 0x100), "ordinary self-jump parks");
        cpu->lc[0] = 1;
        check(!classify_parked(cpu, 0x100), "LC0 excludes self-jump parking");
        cpu->lc[0] = 0; cpu->lc[1] = 1;
        check(!classify_parked(cpu, 0x100), "LC1 excludes self-jump parking");
        cpu->lc[1] = 0; cpu->did_jump = 0;
        check(!classify_parked(cpu, 0x100), "unchanged PC without jump does not park");
        cpu->did_jump = 1;
        check(!classify_parked(cpu, 0x102), "non-self branch does not park");
        bfin_idle_hint = 1;
        check(classify_parked(cpu, 0x102), "explicit IDLE hint parks");
    } else {
        check(0, "unknown scenario");
    }
}
int main(int argc, char **argv)
{
    check(argc == 2, "one scenario required");
    exercise(argv[1]);
    printf("PASS %s\n", argv[1]);
    return 0;
}
