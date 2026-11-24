/*
 * ps3recomp - Timer and time syscalls (implementation)
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* tm_gmtoff on glibc */
#endif
#include "sys_timer.h"
#include <time.h>
#include "sys_event.h"
#include "../memory/vm.h"
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include <stdlib.h>
#include "ps3emu/log_first.h"
#include "ps3emu/port_hitch.h"   /* [hitch]: guest wait call sites of the frame loop */

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_timer_info g_sys_timers[SYS_TIMER_MAX];

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    return now.QuadPart +
        (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}
#endif

int ps3_perf_on(const char* name, int def)
{
    const char* a = getenv("PS3_PERF_ALL");
    if (a && a[0] == '0') return 0;
    a = getenv("RSX_PERF_ALL");
    if (a && a[0] == '0') return 0;
    const char* e = name ? getenv(name) : NULL;
    if (e && *e) return atoi(e) != 0;
    return def;
}

#if defined(_MSC_VER)
#define PS3_TLS __declspec(thread)
#else
#define PS3_TLS _Thread_local
#endif

/* ---------------------------------------------------------------------------
 * Guest-memory write notification (see sys_timer.h)
 * -----------------------------------------------------------------------*/
volatile long g_guest_mem_seq = 0;
volatile long g_ps3_stall_seq = 0;
static volatile long s_mem_waiters = 0;   /* threads in a notify-aware short usleep */
#ifdef _WIN32
/* PS3_USLEEP_BLOCK: short usleeps block on a per-thread event (woken by the
 * notify below) plus a high-resolution timer instead of spinning, so a poll
 * loop no longer holds a core the SPU worker it waits for needs. */
#define USLEEP_BLOCK_SLOTS 64
static HANDLE s_blk_ev[USLEEP_BLOCK_SLOTS];
static volatile long s_blk_armed[USLEEP_BLOCK_SLOTS];
static volatile long s_blk_used = 0;
static volatile long s_blk_waiters = 0;
#endif
/* Resistance port: SPU_DMA_FAST (default 1; SPU_DMA_FAST=0 = the old path)
 * coalesces these notifications. A waiter only ever needs ONE bump of
 * g_guest_mem_seq after it took its snapshot, but every SPU PUT and atomic
 * did an InterlockedIncrement while anyone waited. With R3's ADS depth of
 * field the main thread sits 11-17 ms a frame in short usleeps at 0x0082C340
 * (the WWS job-list wait) while job 419 issues ~31k PUTs a frame over 2-4
 * lanes, so every PUT read a line the waiting PPU rewrote on each usleep and
 * then fought the other lanes for it with a locked add. Now the waiter arms
 * s_mem_armed after its snapshot; the first notify after that disarms it and
 * bumps the sequence, later ones see it clear and only read. Each waiter is
 * still woken by the first write after its snapshot: a notify either finds
 * the flag armed (and bumps) or an earlier notify cleared it after the
 * snapshot was taken (and bumps after that). Kept on its own cache line. */
#if defined(_MSC_VER)
typedef __declspec(align(64)) struct { volatile long v; char pad[60]; } ps3_mem_armed_line;
#else
typedef struct { volatile long v; char pad[60]; } __attribute__((aligned(64))) ps3_mem_armed_line;
#endif
static ps3_mem_armed_line s_mem_armed_line;
#define s_mem_armed (s_mem_armed_line.v)
static volatile int s_mem_coalesce = -1;
static int ps3_mem_notify_coalesce(void)
{
    int c = s_mem_coalesce;
    if (__builtin_expect(c < 0, 0)) {
        const char* e = getenv("SPU_DMA_FAST");
        c = !(e && *e == '0');
        s_mem_coalesce = c;
    }
    return c;
}
/* PS3_USLEEP_WATCH: writer-woken short usleeps for known guest poll loops.
 * The game installs g_ps3_usleep_watch (R3: the WWS job-list wait at
 * 0x0082C340, project\stubs.cpp). For a usleep it recognises, it reports the
 * guest range the loop polls ([lo,hi)) and whether the loop's condition is
 * already met. The waiter then blocks on its own event until an SPU store
 * (PUT or atomic) lands in that range, with a high-resolution timer as the
 * fallback: the guest's own usleep time (as in R2), so a store the watch cannot
 * see costs no more than the old poll; PS3_USLEEP_WATCH_MAX_US=<us> (>= 50)
 * sets a longer floor (the old default was 1000). Stores elsewhere do not
 * wake it, unlike the global notify, which the ~31k DOF PUTs a frame keep
 * firing. PS3_USLEEP_WATCH=0 = the old spin. */
int (*volatile g_ps3_usleep_watch)(void* ctx, uint32_t* lo, uint32_t* hi) = 0;
#ifdef _WIN32
#define USLEEP_WATCH_SLOTS 8
typedef struct { volatile uint32_t lo, hi; volatile long armed; HANDLE ev; volatile long who; } ps3_watch_slot;
static ps3_watch_slot s_watch[USLEEP_WATCH_SLOTS];
static volatile long s_watch_used = 0, s_watch_n = 0;
/* R3_JOBWAIT_WHO=1 (project\stubs.cpp, off by default): the waking writer's
 * SPU job image (-1 = the job kernel, outside a timed job) per watched wait,
 * with the blocked time, appended to the [usleep-watch] line. */
int (*volatile g_ps3_watch_who)(void) = 0;
/* Union of every range ever watched, on its own read-mostly line: a store
 * outside it costs two loads. */
#if defined(_MSC_VER)
typedef __declspec(align(64)) struct { volatile uint32_t lo, hi; char pad[56]; } ps3_watch_span;
#else
typedef struct { volatile uint32_t lo, hi; char pad[56]; } __attribute__((aligned(64))) ps3_watch_span;
#endif
static ps3_watch_span s_watch_span = { 0xFFFFFFFFu, 0u, {0} };
static void ps3_watch_notify_slow(uint32_t ea, uint32_t span)
{
    /* Our data store must be visible before we read the armed flags (the
     * waiter arms, then re-reads the data): a store-load fence. */
    MemoryBarrier();
    if (!s_watch_n) return;
    const uint64_t e0 = ea, e1 = (uint64_t)ea + span;
    long n = s_watch_used; if (n > USLEEP_WATCH_SLOTS) n = USLEEP_WATCH_SLOTS;
    for (long i = 0; i < n; i++) {
        ps3_watch_slot* s = &s_watch[i];
        if (s->armed && e0 < s->hi && e1 > s->lo) {
            int (*who)(void) = g_ps3_watch_who;
            const long w = who ? who() : 0;
            if (InterlockedExchange(&s->armed, 0)) { if (who) s->who = w; SetEvent(s->ev); }
        }
    }
}
static inline void ps3_watch_notify(uint32_t ea, uint32_t span)
{
    if ((uint64_t)ea + span <= s_watch_span.lo || ea >= s_watch_span.hi) return;
    ps3_watch_notify_slow(ea, span);
}
#endif
void ps3_guest_mem_notify(uint32_t ea)
{
    (void)ea;
#ifdef _WIN32
    ps3_watch_notify(ea & ~127u, 128u);
    if (ps3_mem_notify_coalesce()) {
        if (!s_mem_armed || !InterlockedExchange(&s_mem_armed, 0)) return;
        InterlockedIncrement(&g_guest_mem_seq);
        if (s_blk_waiters) {
            long n = s_blk_used; if (n > USLEEP_BLOCK_SLOTS) n = USLEEP_BLOCK_SLOTS;
            for (long i = 0; i < n; i++)
                if (s_blk_armed[i] && InterlockedExchange(&s_blk_armed[i], 0)) SetEvent(s_blk_ev[i]);
        }
        return;
    }
#endif
    if (s_mem_waiters) {
#ifdef _WIN32
        InterlockedIncrement(&g_guest_mem_seq);
        if (s_blk_waiters) {
            long n = s_blk_used; if (n > USLEEP_BLOCK_SLOTS) n = USLEEP_BLOCK_SLOTS;
            for (long i = 0; i < n; i++)
                if (s_blk_armed[i] && InterlockedExchange(&s_blk_armed[i], 0)) SetEvent(s_blk_ev[i]);
        }
#else
        __atomic_add_fetch(&g_guest_mem_seq, 1, __ATOMIC_SEQ_CST);
#endif
    }
}
void (*volatile g_guest_mem_notify)(uint32_t ea) = ps3_guest_mem_notify;
/* Plain SPU DMA PUTs (spu_dma.h). PS3_USLEEP_NOTIFY covered only SPU atomics
 * (PUTLLC/PUTLLUC); a job kernel that reports completion with a plain PUT left
 * the PPU poll (R2's job wait at 0x006B8D3C: usleep(128)) sleeping out its
 * full interval. PS3_USLEEP_NOTIFY_PUT=0 off. Free when nobody waits. */
static volatile int s_notify_put = -1;
void ps3_guest_mem_notify_put(uint32_t ea)
{
#ifdef _WIN32
    ps3_watch_notify(ea, 0x4000u);   /* a PUT is at most 16 KB */
    if (s_mem_coalesce > 0) { if (!s_mem_armed) return; }
    else
#endif
    if (!s_mem_waiters) return;
    if (__builtin_expect(s_notify_put < 0, 0))
        s_notify_put = ps3_perf_on("PS3_USLEEP_NOTIFY", 1) && ps3_perf_on("PS3_USLEEP_NOTIFY_PUT", 1);
    if (s_notify_put) ps3_guest_mem_notify(ea);
}

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static volatile long s_watch_on = -1, s_watch_max_us = 0;   /* 0 = the guest's usec */
static volatile long long s_wst_wait, s_wst_woken, s_wst_tmo, s_wst_ready, s_wst_next;
/* R3_JOBWAIT_SPU_WAKE (default 1; 0/off = per-lap fallback): the watched wait trusts the SPU-store wake.
 * The completion word of a job list is written by an SPU PUT/PUTLLC, which
 * already signals the slot (ps3_guest_mem_notify[_put]); the timeouts in the
 * [usleep-watch] line are the intermediate laps of the guest's usleep(30),
 * ~0.5 ms each at HR-timer granularity. With the switch the fallback timer is
 * R3_JOBWAIT_SPU_WAKE_US (default 2000, >= 50) instead of the guest's usec, so
 * a list costs one block and one writer wake instead of ~3 laps. A timeout that
 * finds the list complete is a wake the watch did not see ("missed"): the next
 * 256 waits fall back to the guest's usec, so an unseen writer costs at most
 * one long block. PS3_USLEEP_WATCH_MAX_US, when set, still wins. */
static volatile long s_spu_wake = -1, s_spu_wake_us = 2000, s_spu_wake_backoff = 0;
static volatile long long s_wst_missed;
/* R3_JOBWAIT_WHO bins: waker image id (+2; 1 = timeout), wakes, blocked QPC ticks */
enum { WATCH_WHO_N = 8 };
static volatile long s_who_id[WATCH_WHO_N];
static volatile long long s_who_n[WATCH_WHO_N], s_who_q[WATCH_WHO_N];
static void ps3_watch_who_note(long id, long long q)
{
    for (int i = 0; i < WATCH_WHO_N; i++) {
        long cur = s_who_id[i];
        if (!cur) cur = InterlockedCompareExchange(&s_who_id[i], id, 0);
        if (!cur || cur == id) {
            InterlockedIncrement64(&s_who_n[i]);
            InterlockedExchangeAdd64(&s_who_q[i], q);
            return;
        }
    }
}
static void ps3_watch_widen(uint32_t lo, uint32_t hi)
{
    for (;;) {
        const uint32_t c = s_watch_span.lo;
        if (lo >= c || (uint32_t)InterlockedCompareExchange((volatile long*)&s_watch_span.lo, (long)lo, (long)c) == c) break;
    }
    for (;;) {
        const uint32_t c = s_watch_span.hi;
        if (hi <= c || (uint32_t)InterlockedCompareExchange((volatile long*)&s_watch_span.hi, (long)hi, (long)c) == c) break;
    }
}
/* PS3_USLEEP_WATCH (above): serve a short usleep of a watched poll loop.
 * Returns 0 when this is not one (the caller sleeps as before). */
static int ps3_usleep_watch_wait(void* ctx, uint64_t usec, int* early)
{
    if (__builtin_expect(s_watch_on < 0, 0)) {
        const char* m = getenv("PS3_USLEEP_WATCH_MAX_US");
        long v = m && *m ? atol(m) : 0;
        s_watch_max_us = v <= 0 ? 0 : v < 50 ? 50 : v;
        s_watch_on = ps3_perf_on("PS3_USLEEP_WATCH", 1);
        fprintf(stderr, "[usleep] PS3_USLEEP_WATCH=%ld (max %ld us, 0 = the guest's usleep time; hook %s)\n", s_watch_on, s_watch_max_us,
                g_ps3_usleep_watch ? "set" : "none");
        const char* sw = getenv("R3_JOBWAIT_SPU_WAKE");
        const char* su = getenv("R3_JOBWAIT_SPU_WAKE_US");
        long swu = su && *su ? atol(su) : 2000;
        s_spu_wake_us = swu < 50 ? 50 : swu;
        s_spu_wake = !(sw && *sw && (*sw == '0' || !_stricmp(sw, "off") || !_stricmp(sw, "false") || !_stricmp(sw, "no")));
        if (s_spu_wake)
            fprintf(stderr, "[usleep] R3_JOBWAIT_SPU_WAKE=1 (writer-woken block up to %ld us%s)\n", s_spu_wake_us,
                    s_watch_max_us ? "; PS3_USLEEP_WATCH_MAX_US overrides" : "");
    }
    int (*hook)(void*, uint32_t*, uint32_t*) = g_ps3_usleep_watch;
    if (!s_watch_on || !hook) return 0;
    uint32_t lo = 0, hi = 0;
    const int w = hook(ctx, &lo, &hi);
    if (!w || (w == 1 && lo >= hi)) return 0;
    static PS3_TLS long t_ws = -1;
    static PS3_TLS HANDLE t_wtimer;
    if (t_ws == -1) {
        t_ws = -2;
        const long i = InterlockedIncrement(&s_watch_used) - 1;
        if (i < USLEEP_WATCH_SLOTS) {
            HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
            t_wtimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!t_wtimer) t_wtimer = CreateWaitableTimerW(NULL, TRUE, NULL);
            if (ev && t_wtimer) { s_watch[i].ev = ev; MemoryBarrier(); t_ws = i; }
        }
    }
    if (t_ws < 0) return 0;
    InterlockedIncrement64(&s_wst_wait);
    if (w == 2) {   /* the condition is met already: the loop re-checks at once */
        InterlockedIncrement64(&s_wst_ready);
        *early = 1;
    } else {
        ps3_watch_widen(lo, hi);
        ps3_watch_slot* s = &s_watch[t_ws];
        s->lo = lo; s->hi = hi;
        InterlockedIncrement(&s_watch_n);
        InterlockedExchange(&s->armed, 1);
        /* armed (full barrier), then re-check: a store that landed before the
         * arm is seen here, one after it sets the event */
        uint32_t l2, h2;
        if (hook(ctx, &l2, &h2) == 2) {
            InterlockedIncrement64(&s_wst_ready);
            *early = 1;
        } else {
            uint64_t floor_us = (uint64_t)s_watch_max_us;
            int spu_long = 0;
            if (!floor_us && s_spu_wake > 0) {
                if (s_spu_wake_backoff > 0) InterlockedDecrement(&s_spu_wake_backoff);
                else { floor_us = (uint64_t)s_spu_wake_us; spu_long = 1; }
            }
            const uint64_t us = !floor_us ? usec : usec > floor_us ? usec : floor_us;
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)us * 10;
            SetWaitableTimer(t_wtimer, &due, 0, NULL, NULL, FALSE);
            HANDLE hs[2] = { s->ev, t_wtimer };
            const int who_on = g_ps3_watch_who != 0;
            LARGE_INTEGER wq0; if (who_on) { s->who = -1; QueryPerformanceCounter(&wq0); }
            if (WaitForMultipleObjects(2, hs, FALSE, (DWORD)(us / 1000 + 20)) == WAIT_OBJECT_0) {
                InterlockedIncrement64(&s_wst_woken);
                *early = 1;
                if (who_on) { LARGE_INTEGER wq1; QueryPerformanceCounter(&wq1); ps3_watch_who_note(s->who + 2, wq1.QuadPart - wq0.QuadPart); }
            } else {
                InterlockedIncrement64(&s_wst_tmo);
                if (who_on) { LARGE_INTEGER wq1; QueryPerformanceCounter(&wq1); ps3_watch_who_note(-1, wq1.QuadPart - wq0.QuadPart); }
                if (spu_long) {
                    uint32_t l3, h3;
                    if (hook(ctx, &l3, &h3) == 2) {   /* done, but nobody woke us: a writer the watch cannot see */
                        InterlockedIncrement64(&s_wst_missed);
                        InterlockedExchange(&s_spu_wake_backoff, 256);
                        *early = 1;
                    }
                }
            }
        }
        /* a notifier that already disarmed the slot may still set the event:
         * drop that so the next wait is not cut short */
        if (!InterlockedExchange(&s->armed, 0)) WaitForSingleObject(s->ev, 0);
        InterlockedDecrement(&s_watch_n);
    }
    /* [usleep-watch] 10 s statistics: opt-in, PS3_USLEEP_WATCH_LOG=1 (or R3_JOBWAIT_WHO=1) */
    static int s_wst_log = -1;
    if (s_wst_log < 0) { const char* e = getenv("PS3_USLEEP_WATCH_LOG"); const char* w = getenv("R3_JOBWAIT_WHO");
                         s_wst_log = (e && *e && *e != '0') || (w && *w && *w != '0'); }
    if (!s_wst_log) return 1;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now);
    if (now.QuadPart >= s_wst_next) {
        QueryPerformanceFrequency(&f);
        const long long nx = now.QuadPart + 10 * f.QuadPart, prev = s_wst_next;
        if (InterlockedCompareExchange64(&s_wst_next, nx, prev) == prev && prev) {
            fprintf(stderr, "[usleep-watch] 10 s: %lld waits, %lld woken by the writer, %lld timed out, %lld already done",
                    (long long)InterlockedExchange64(&s_wst_wait, 0), (long long)InterlockedExchange64(&s_wst_woken, 0),
                    (long long)InterlockedExchange64(&s_wst_tmo, 0), (long long)InterlockedExchange64(&s_wst_ready, 0));
            if (s_spu_wake > 0)   /* R3_JOBWAIT_SPU_WAKE: timeouts that found the list done */
                fprintf(stderr, ", %lld missed wakes", (long long)InterlockedExchange64(&s_wst_missed, 0));
            if (g_ps3_watch_who) {
                const double ms = 1000.0 / (double)f.QuadPart;
                for (int i = 0, k = 0; i < WATCH_WHO_N && s_who_id[i]; i++) {
                    const long long n = InterlockedExchange64(&s_who_n[i], 0), q = InterlockedExchange64(&s_who_q[i], 0);
                    const long id = s_who_id[i];
                    if (!n) continue;
                    if (id == -1) fprintf(stderr, "%s timeout %lld/%.1f ms", k++ ? "," : "; by waker:", n, q * ms);
                    else if (id == 1) fprintf(stderr, "%s no job %lld/%.1f ms", k++ ? "," : "; by waker:", n, q * ms);
                    else fprintf(stderr, "%s img %ld %lld/%.1f ms", k++ ? "," : "; by waker:", id - 2, n, q * ms);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    return 1;
}
#endif
/* ---------------------------------------------------------------------------
 * Per-thread wait accounting (see sys_timer.h)
 * -----------------------------------------------------------------------*/
ps3_waitstat g_ps3_waitstat[PS3_WAITSTAT_MAX];
static volatile long s_waitstat_n = 0;
static PS3_TLS ps3_waitstat* t_waitstat;
static PS3_TLS int t_waitstat_full;
ps3_waitstat* ps3_waitstat_self(void)
{
    if (t_waitstat || t_waitstat_full) return t_waitstat;
#ifdef _WIN32
    const unsigned long tid = GetCurrentThreadId();
    const long i = InterlockedIncrement(&s_waitstat_n) - 1;
#else
    const unsigned long tid = (unsigned long)(uintptr_t)&t_waitstat;
    const long i = __atomic_add_fetch(&s_waitstat_n, 1, __ATOMIC_SEQ_CST) - 1;
#endif
    if (i >= PS3_WAITSTAT_MAX) { t_waitstat_full = 1; return NULL; }
    g_ps3_waitstat[i].tid = tid;
    t_waitstat = &g_ps3_waitstat[i];
    return t_waitstat;
}

/* R3_SEMLOG: once per long RSX label wait, per thread (see ps3_waitstat.h) */
void ps3_stall_chain(const char* tag)
{
    static PS3_TLS long t_seen;
    const long s = g_ps3_stall_seq;
    if (!s || s == t_seen) return;
    t_seen = s;
    extern void ppu_log_host_chain(const char*);
    ppu_log_host_chain(tag);
}

int64_t ps3_qpc_freq(void)
{
#ifdef _WIN32
    ensure_qpc_init();
    return s_qpc_freq.QuadPart;
#else
    return 1000000000LL;
#endif
}

/* ---------------------------------------------------------------------------
 * PPU timebase (mftb/mftbu) -- THE guest clock.
 *
 * One global monotonic counter scaled to the PS3 timebase (79.8 MHz),
 * anchored at first use. The old lifter emission was a per-call-site
 * static that advanced 16667 ticks PER READ -- not a clock at all: every
 * guest timing loop (media pacers, throttles, profilers) computed garbage
 * from it, and each call site had a PRIVATE counter that only moved when
 * polled. Overflow-safe split multiply (rem < qpf, so
 * rem * 79.8e6 < ~8e14 << 2^63).
 * -----------------------------------------------------------------------*/
/* Resistance port: TSC-based timebase. The game reads mftb millions of
 * times a second (profiling scope markers on the main thread); QPC plus two
 * 64-bit divides per read was ~30% of that thread. The invariant TSC is
 * calibrated against QPC once (~20 ms) and scaled with a 128-bit multiply.
 * PPU_TB_QPC=1 restores the QPC path. */
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#include <intrin.h>
static uint64_t s_tsc0 = 0, s_tsc_mul = 0;   /* tb = (tsc - tsc0) * mul >> 32 */
static volatile long s_tsc_state = 0;        /* 0 = not set up, 1 = busy, 2 = TSC, 3 = QPC */
static void tsc_calibrate(void)
{
    if (InterlockedCompareExchange(&s_tsc_state, 1, 0) != 0) {
        while (s_tsc_state == 1) YieldProcessor();
        return;
    }
    const char* e = getenv("PPU_TB_QPC");
    int cpuinfo[4] = {0};
    __cpuid(cpuinfo, 0x80000007);
    int invariant = (cpuinfo[3] >> 8) & 1;
    if ((e && atoi(e)) || !invariant) { s_tsc_state = 3; return; }
    ensure_qpc_init();
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0); uint64_t c0 = __rdtsc();
    do { QueryPerformanceCounter(&q1); } while ((q1.QuadPart - q0.QuadPart) * 1000 < s_qpc_freq.QuadPart * 20);
    uint64_t c1 = __rdtsc();
    double secs = (double)(q1.QuadPart - q0.QuadPart) / (double)s_qpc_freq.QuadPart;
    double tsc_hz = (double)(c1 - c0) / secs;
    s_tsc_mul = (uint64_t)((double)PS3_TIMEBASE_FREQ / tsc_hz * 4294967296.0);
    s_tsc0 = c1;
    fprintf(stderr, "[timebase] TSC %.1f MHz -> timebase via rdtsc%c", tsc_hz / 1e6, 10);
    s_tsc_state = 2;
}
#endif

uint64_t ppu_timebase_now(void)
{
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (s_tsc_state != 2 && s_tsc_state != 3) tsc_calibrate();
    if (s_tsc_state == 2) {
        unsigned __int128 t = (unsigned __int128)(__rdtsc() - s_tsc0) * s_tsc_mul;
        return (uint64_t)(t >> 32) + 1;   /* +1: the guest retries a read of 0 */
    }
#endif
#ifdef _WIN32
    static LONGLONG t0 = 0;
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!t0) t0 = now.QuadPart;   /* benign race: same anchor either way */
    uint64_t d = (uint64_t)(now.QuadPart - t0);
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * PS3_TIMEBASE_FREQ + (d % q) * PS3_TIMEBASE_FREQ / q;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * PS3_TIMEBASE_FREQ +
           (uint64_t)ts.tv_nsec * PS3_TIMEBASE_FREQ / 1000000000ull;
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_timer_usleep
 *
 * r3 = microseconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_usleep(ppu_context* ctx)
{
    uint64_t usec = LV2_ARG_U64(ctx, 0);
    { extern unsigned long long ps3_qpc_us(void);
      static int n=0; if (PS3_LOG_FIRST(n, 60))
        fprintf(stderr, "[WAIT] t=%lluus timer_usleep(%llu us) lr=0x%08llX cia=0x%08llX\n", ps3_qpc_us(),
        (unsigned long long)usec, (unsigned long long)ctx->lr, (unsigned long long)ctx->cia); }
    /* PS3_POLLTOP=<seconds>: name the usleep poll sites.
     *
     * A title that is "hung" is usually not blocked on anything -- every thread
     * is spinning on sys_timer_usleep waiting for a flag, and [BLOCKSUM] only
     * says "syscall 141, 12s". This histograms the callers over a window and
     * prints the top ones, so the answer is a guest address rather than a
     * count. Keyed on ctx->lr: a host-stack backtrace through lifted code has
     * no unwind tables and invents chains (see the Guitar Hero III writeup),
     * and lr is the only guest chain that survives.
     *
     * Deliberately unlocked: the counts are a diagnostic, and a lost increment
     * under a race cannot change which site is at the top of a 30k-per-second
     * poll. */
    { static long s_pt = -1;
      if (s_pt < 0) { const char* e = getenv("PS3_POLLTOP");
                      s_pt = e ? (long)strtoul(e, 0, 10) : 0;
                      if (s_pt < 0) s_pt = 0; }
      if (s_pt > 0) {
        enum { PT_MAX = 128 };
        static uint32_t pt_lr[PT_MAX]; static uint32_t pt_n[PT_MAX];
        static uint64_t pt_us[PT_MAX]; static int pt_used = 0;
        static uint32_t pt_tid[PT_MAX];
        static unsigned long long pt_win = 0;
        uint32_t lr = (uint32_t)ctx->lr;
        int i = 0;
        for (; i < pt_used; i++) if (pt_lr[i] == lr) break;
        if (i == pt_used && pt_used < PT_MAX) {
            pt_lr[pt_used] = lr;
            /* The thread matters as much as the site: "who is waiting" is the
             * half that says whether a poll is the engine idling or the main
             * thread stuck. */
            pt_tid[pt_used] = (uint32_t)ctx->thread_id;
            pt_used++;
        }
        if (i < PT_MAX) { pt_n[i]++; pt_us[i] += usec; }
        { extern unsigned long long ps3_qpc_us(void);
          unsigned long long now = ps3_qpc_us();
          if (!pt_win) pt_win = now;
          else if (now - pt_win >= (unsigned long long)s_pt * 1000000ull) {
            fprintf(stderr, "--- usleep poll sites (last %lds) ---\n", s_pt);
            for (int k = 0; k < 8; k++) {
              int best = -1;
              for (int j = 0; j < pt_used; j++)
                if (pt_n[j] && (best < 0 || pt_n[j] > pt_n[best])) best = j;
              if (best < 0) break;
              fprintf(stderr, "  %8u calls  %6llums slept  tid=%-3u lr=0x%08X\n",
                      pt_n[best], (unsigned long long)(pt_us[best] / 1000),
                      pt_tid[best], pt_lr[best]);
              pt_n[best] = 0;
            }
            fflush(stderr);
            for (int j = 0; j < pt_used; j++) { pt_n[j] = 0; pt_us[j] = 0; }
            pt_win = now;
          } } } }

    /* PS3_WAIT_OBJ=<lr-hex>: when a usleep spin is reached from this return
     * address, dump the registers and the object they point at. A poll loop
     * tells you WHERE it is spinning; this tells you WHAT it is spinning on,
     * which is the part you actually need to find who never releases it. */
    { static long s_wo = -1;
      if (s_wo < 0) { const char* e = getenv("PS3_WAIT_OBJ");
                      s_wo = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_wo && (uint32_t)ctx->lr == (uint32_t)s_wo) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8)) {
          /* Dump the words at EVERY plausible object register, not just r29.
           * Which register holds the object is per-title -- r29 was right for
           * the title this was written for and is a spin COUNTER in Guitar
           * Hero III, where the object is in r30/r31. Printing one guess makes
           * the tool silently report the wrong memory as "the thing it waits
           * on", which is worse than printing nothing. */
          fprintf(stderr, "[wait-obj] lr=0x%08X sleep=%lluus", (uint32_t)ctx->lr,
                  (unsigned long long)usec);
          static const int regs[] = { 28, 29, 30, 31, 3 };
          for (unsigned ri = 0; ri < sizeof regs / sizeof regs[0]; ri++) {
            const int r = regs[ri];
            const uint32_t o = (uint32_t)ctx->gpr[r];
            fprintf(stderr, "  r%d=0x%08X", r, o);
            /* Only deref something that looks like a guest pointer: a small
             * integer is a count, and dumping "memory at 4" is noise. */
            if (vm_base && o >= 0x10000u) {
              fprintf(stderr, "[");
              for (int w = 0; w < 4; w++) {
                const uint8_t* q = vm_base + o + (unsigned)w * 4u;
                uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                             ((uint32_t)q[2] << 8)  | (uint32_t)q[3];
                fprintf(stderr, "%s%08X", w ? " " : "", v);
              }
              fprintf(stderr, "]");
            }
          }
          fprintf(stderr, "\n"); fflush(stderr);
        } } }

    /* POLLSITE: resolve the host chain of the 1ms poller (LBP bringup) to
     * guest functions -- names the stage that is starving. */
    { static int _ps = -1; if (_ps < 0) _ps = getenv("POLLSITE") ? 12 : 0;
      if (_ps > 0 && usec == 1000) { _ps--;
        extern void ppu_log_host_chain(const char*);
        ppu_log_host_chain("usleep1ms"); } }

#ifdef _WIN32
    /* Switches, read once: PS3_USLEEP_TIMER_CACHE (one high-resolution timer
     * per thread instead of create+close per sleep) and PS3_USLEEP_NOTIFY
     * (a short spin ends when ps3_guest_mem_notify reports a guest write).
     * Both default on; =0, PS3_PERF_ALL=0 or RSX_PERF_ALL=0 turn them off. */
    static volatile int s_tcache = -1, s_notify = -1;
    if (s_tcache < 0) {
        s_notify = ps3_perf_on("PS3_USLEEP_NOTIFY", 1);
        s_tcache = ps3_perf_on("PS3_USLEEP_TIMER_CACHE", 1);
        fprintf(stderr, "[usleep] PS3_USLEEP_NOTIFY=%d PS3_USLEEP_NOTIFY_PUT=%d PS3_USLEEP_TIMER_CACHE=%d\n",
                s_notify, s_notify && ps3_perf_on("PS3_USLEEP_NOTIFY_PUT", 1), s_tcache);
    }
    PS3_STALL_CHAIN("semstall-usleep");
    ps3_waitstat* ws = ps3_waitstat_self();
    LARGE_INTEGER ws_t0; ws_t0.QuadPart = 0;
    if (ws) QueryPerformanceCounter(&ws_t0);
    /* Use high-resolution sleep via waitable timer for better precision */
    if (usec >= 1000) {
        /* A plain waitable timer fires on the next 1 ms system tick, so a
         * 16667 us sleep took ~17-18 ms. Resistance 2's syn_tick refills the
         * audio ring once per 1/60 s sleep and that overshoot ate its slack.
         * Use a high-resolution timer (Win10 1803+), else fall back. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        static PS3_TLS HANDLE t_timer;
        HANDLE timer = s_tcache ? t_timer : NULL;
        if (!timer) {
            timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
            if (s_tcache) t_timer = timer;
        }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10); /* 100ns units, negative = relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
            if (!s_tcache) CloseHandle(timer);
        } else {
            Sleep((DWORD)(usec / 1000));
        }
        if (ws) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
                  ws->long_n++; ws->long_qpc += (unsigned long long)(t1.QuadPart - ws_t0.QuadPart);
                  port_hitch_guest_wait((uint32_t)ctx->lr, t1.QuadPart - ws_t0.QuadPart); }
    } else if (usec > 0) {
        /* Sub-millisecond sleep. Sleep()/waitable timers round up to the
         * ~0.5-1 ms scheduler quantum, far too coarse for a guest that relies
         * on accurate microsecond pacing -- a short usleep() spin waiting on
         * another thread to make progress otherwise collapses to a single yield
         * and busy-spins at full speed (so the wait is effectively a no-op).
         * Busy-wait to a precise QPC deadline, yielding inside the spin
         * (SwitchToThread) so a sibling host thread the guest may be waiting on
         * still gets the core. Mirrors RPCS3's "Usleep Only" TSC busy-tail.
         *
         * These short sleeps are the guest's poll loops (R2: the occlusion
         * report poll usleep(30) and the frame-label wait usleep(50) were 76%
         * of the main thread). With PS3_USLEEP_NOTIFY the spin also ends as
         * soon as a guest write it may be waiting for is reported, so the
         * guest re-checks immediately; an early return from usleep is
         * harmless to a poll loop, which simply checks and sleeps again. */
        int wearly = 0;
        if (ps3_usleep_watch_wait(ctx, usec, &wearly)) {
            if (ws && wearly) ws->early_n++;
        } else {
        ensure_qpc_init();
        LARGE_INTEGER qpc_start, qpc_now;
        QueryPerformanceCounter(&qpc_start);
        const int64_t qpc_deadline = qpc_start.QuadPart +
            (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
        if (s_notify) {
            InterlockedIncrement(&s_mem_waiters);
            const long seq0 = g_guest_mem_seq;
            if (ps3_mem_notify_coalesce()) InterlockedExchange(&s_mem_armed, 1);   /* after the snapshot */
            int early = 0;
            static volatile int s_block = -1, s_block_min = 0;
            if (s_block < 0) {
                const char* m = getenv("PS3_USLEEP_BLOCK_MIN");
                s_block_min = m && *m ? atoi(m) : 10;
                s_block = ps3_perf_on("PS3_USLEEP_BLOCK", 0);
                fprintf(stderr, "[usleep] PS3_USLEEP_BLOCK=%d (min %d us)\n", s_block, s_block_min);
            }
            /* PS3_USLEEP_PARK (low-core no-spin profile; default = PORT_NOSPIN,
             * on by default; PORT_NOSPIN=auto = below 12 CPUs only): a poll loop that has
             * been spinning here for PS3_USLEEP_PARK_AFTER_US (default 200)
             * from the same guest site stops spinning and parks on the block
             * event below (woken by the next guest-memory notify -- SPU
             * PUT/atomic, RSX label/semaphore writes -- with the guest's own
             * usleep time on a high-resolution timer as the fallback). Short
             * polls (< the threshold) still spin exactly as before, so a
             * quick answer sees no added latency; only long waits give the
             * core back to the SPU lanes they are waiting on. */
            static volatile int s_park = -1, s_park_after = 200;
            if (s_park < 0) {
                extern int ps3_host_nospin(void);
                const char* m = getenv("PS3_USLEEP_PARK_AFTER_US");
                s_park_after = m && *m ? atoi(m) : 200;
                if (s_park_after < 0) s_park_after = 0;
                s_park = ps3_perf_on("PS3_USLEEP_PARK", ps3_host_nospin());
                fprintf(stderr, "[usleep] PS3_USLEEP_PARK=%d (after %d us of polling one site)\n", s_park, s_park_after);
            }
            int park = 0;
            if (s_park) {
                static PS3_TLS uint32_t t_pk_lr;
                static PS3_TLS long long t_pk_t0, t_pk_last;
                const long long now = qpc_start.QuadPart;
                /* same site, and the previous usleep began < 2 ms (plus this
                 * one's length) ago: still the same wait */
                if ((uint32_t)ctx->lr != t_pk_lr || now - t_pk_last > s_qpc_freq.QuadPart / 500 + (long long)(qpc_deadline - qpc_start.QuadPart))
                    { t_pk_lr = (uint32_t)ctx->lr; t_pk_t0 = now; }
                t_pk_last = now;
                park = (now - t_pk_t0) * 1000000 >= (long long)s_park_after * s_qpc_freq.QuadPart;
            }
            static PS3_TLS long t_slot = -1;
            static PS3_TLS HANDLE t_btimer;
            if ((s_block || park) && t_slot == -1 && (park || usec >= (uint64_t)s_block_min)) {
                t_slot = -2;
                /* claim a slot only while one is left: s_blk_used never passes
                 * USLEEP_BLOCK_SLOTS (the notify loops index s_blk_armed/_ev by it) */
                long i;
                for (;;) {
                    i = s_blk_used;
                    if (i >= USLEEP_BLOCK_SLOTS || InterlockedCompareExchange(&s_blk_used, i + 1, i) == i) break;
                }
                if (i < USLEEP_BLOCK_SLOTS) {
                    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
                    t_btimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_MANUAL_RESET |
                                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                    if (ev && t_btimer) { s_blk_ev[i] = ev; MemoryBarrier(); t_slot = i; }
                }
            }
            if (t_slot >= 0 && (park || (s_block && usec >= (uint64_t)s_block_min))) {
                InterlockedExchange(&s_blk_armed[t_slot], 1);
                InterlockedIncrement(&s_blk_waiters);
                if (g_guest_mem_seq == seq0) {
                    LARGE_INTEGER due; due.QuadPart = -((LONGLONG)usec * 10);
                    SetWaitableTimer(t_btimer, &due, 0, NULL, NULL, FALSE);
                    HANDLE hs[2] = { s_blk_ev[t_slot], t_btimer };
                    if (WaitForMultipleObjects(2, hs, FALSE, 20) == WAIT_OBJECT_0) early = 1;
                } else early = 1;
                InterlockedDecrement(&s_blk_waiters);
                /* a notifier that already disarmed the slot may still set the
                 * event: drop that so the next sleep is not cut short */
                if (!InterlockedExchange(&s_blk_armed[t_slot], 0)) WaitForSingleObject(s_blk_ev[t_slot], 0);
            } else
            do {
                if (g_guest_mem_seq != seq0) { early = 1; break; }
                SwitchToThread();
                QueryPerformanceCounter(&qpc_now);
            } while (qpc_now.QuadPart < qpc_deadline);
            InterlockedDecrement(&s_mem_waiters);
            if (ws && early) ws->early_n++;
        } else {
            do {
                SwitchToThread();
                QueryPerformanceCounter(&qpc_now);
            } while (qpc_now.QuadPart < qpc_deadline);
        }
        }
        if (ws) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
                  ws->short_n++; ws->short_qpc += (unsigned long long)(t1.QuadPart - ws_t0.QuadPart);
                  port_hitch_guest_wait((uint32_t)ctx->lr, t1.QuadPart - ws_t0.QuadPart); }
    }
#else
    if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(usec / 1000000);
        ts.tv_nsec = (long)((usec % 1000000) * 1000);
        nanosleep(&ts, NULL);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_sleep
 *
 * r3 = seconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_sleep(ppu_context* ctx)
{
    uint32_t sec = LV2_ARG_U32(ctx, 0);

#ifdef _WIN32
    Sleep(sec * 1000);
#else
    sleep(sec);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_current_time
 *
 * r3 = pointer to receive seconds (u64*)
 * r4 = pointer to receive nanoseconds (u64*)
 * -----------------------------------------------------------------------*/
/* LV2 144: r3/r4 point to signed BE32 minute offsets. The first is the
 * standard UTC offset; the second is the current seasonal adjustment.
 * Derive the adjustment instead of assuming every DST shift is 60 minutes. */
int64_t sys_time_get_timezone(ppu_context* ctx)
{
    uint32_t zone_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t dst_addr = LV2_ARG_PTR(ctx, 1);
    if (!zone_addr || !dst_addr || zone_addr > UINT32_MAX - 3 || dst_addr > UINT32_MAX - 3)
        return (int32_t)CELL_EFAULT;

    int32_t standard_minutes, daylight_minutes;
#ifdef _WIN32
    /* Ask the system, not the C runtime: _get_timezone and _get_dstbias do not exist in the msvcrt.dll that a
     * MinGW-w64 toolchain with the msvcrt runtime links against. Bias is minutes west of UTC. */
    TIME_ZONE_INFORMATION tz;
    DWORD zone_id = GetTimeZoneInformation(&tz);
    if (zone_id == TIME_ZONE_ID_INVALID)
        return (int32_t)CELL_EAGAIN;
    standard_minutes = -(int32_t)(tz.Bias + tz.StandardBias);
    daylight_minutes = zone_id == TIME_ZONE_ID_DAYLIGHT ? -(int32_t)(tz.DaylightBias - tz.StandardBias) : 0;
#else
    time_t now = time(NULL);
    struct tm local;
    if (!localtime_r(&now, &local)) return (int32_t)CELL_EAGAIN;
    daylight_minutes = 0;
    if (local.tm_isdst > 0) {
        struct tm standard = local;
        standard.tm_isdst = 0;
        time_t standard_epoch = mktime(&standard);
        if (standard_epoch == (time_t)-1) return (int32_t)CELL_EAGAIN;
        daylight_minutes = (int32_t)(difftime(standard_epoch, now) / 60);
    }
    standard_minutes = (int32_t)(local.tm_gmtoff / 60) - daylight_minutes;
#endif
    write_be32(zone_addr, (uint32_t)standard_minutes);
    write_be32(dst_addr, (uint32_t)daylight_minutes);
    return CELL_OK;
}

int64_t sys_time_get_current_time(ppu_context* ctx)
{
    uint32_t sec_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t nsec_addr = LV2_ARG_PTR(ctx, 1);

    uint64_t sec, nsec;

#ifdef _WIN32
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    /* Convert QPC to seconds + nanoseconds */
    sec  = (uint64_t)(now.QuadPart / s_qpc_freq.QuadPart);
    uint64_t remainder = (uint64_t)(now.QuadPart % s_qpc_freq.QuadPart);
    nsec = (remainder * 1000000000ULL) / (uint64_t)s_qpc_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec  = (uint64_t)ts.tv_sec;
    nsec = (uint64_t)ts.tv_nsec;
#endif

    if (sec_addr != 0) {
        write_be64(sec_addr, sec);
    }
    if (nsec_addr != 0) {
        write_be64(nsec_addr, nsec);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_timebase_frequency
 *
 * Returns the PS3 timebase frequency in r3.
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_timebase_frequency(ppu_context* ctx)
{
    (void)ctx;
    return (int64_t)PS3_TIMEBASE_FREQ;
}

/* ---------------------------------------------------------------------------
 * Periodic timer thread
 * -----------------------------------------------------------------------*/

/* Forward declaration */
static void timer_send_event(sys_timer_info* t);

/* Microseconds on a monotonic host clock: the timer's expiration times. */
static uint64_t timer_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (uint64_t)(n.QuadPart / f.QuadPart) * 1000000ull +
           (uint64_t)(n.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

#ifdef _WIN32
static DWORD WINAPI timer_thread_proc(LPVOID param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    while (t->running) {
        DWORD ms = (DWORD)(t->period_usec / 1000);
        if (ms == 0) ms = 1;

        DWORD result = WaitForSingleObject(t->stop_event, ms);
        if (result == WAIT_OBJECT_0) {
            break; /* stop signaled */
        }

        if (t->running) {
            timer_send_event(t);
        }
    }

    return 0;
}
#else
static void* timer_thread_proc(void* param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    pthread_mutex_lock(&t->mtx);
    while (!t->stop_flag) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(t->period_usec / 1000000);
        ts.tv_nsec += (long)((t->period_usec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        if (t->stop_flag) break;

        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            timer_send_event(t);
            pthread_mutex_lock(&t->mtx);
        }
    }
    pthread_mutex_unlock(&t->mtx);

    return NULL;
}
#endif

static void timer_send_event(sys_timer_info* t)
{
    if (t->event_queue_id <= 0 || t->event_queue_id > SYS_EVENT_QUEUE_MAX)
        return;

    sys_event_queue_info* q = &g_sys_event_queues[t->event_queue_id - 1];
    if (!q->active)
        return;

    sys_event_t evt;
    evt.source = t->source;
    /* lv2 (and RPCS3) deliver the connect's data1/data2 and, in data3, the
     * EXPIRATION TIME -- which advances every period. It was 0 here, so a
     * consumer that paces itself by the delta between ticks saw no time pass:
     * Twisted Metal's BoomRangBuss audio driver wakes on this timer, derives
     * how many blocks to service from data3, and serviced none, ever -- so
     * brb_StartSession timed out and every later BRB request hung. */
    evt.data1  = t->data1;
    evt.data2  = t->data2;
    evt.data3  = t->next_expire;
    t->next_expire += t->period_usec;

    /* Push event (ignore if queue full) */
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count < q->capacity) {
        q->buffer[q->tail] = evt;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
#ifdef _WIN32
        WakeConditionVariable(&q->not_empty);
#else
        pthread_cond_signal(&q->not_empty);
#endif
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* ---------------------------------------------------------------------------
 * sys_timer_create
 *
 * r3 = pointer to receive timer ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_timer_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);

    int slot = -1;
    for (int i = 0; i < SYS_TIMER_MAX; i++) {
        if (!g_sys_timers[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        static int s_full_said;
        if (s_full_said++ < 4)
            fprintf(stderr, "[SYS] sys_timer_create FAILED: all %d timers in use (CELL_EAGAIN)\n", SYS_TIMER_MAX);
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_timer_info* t = &g_sys_timers[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->running = 0;
    t->event_queue_id = 0;

#ifdef _WIN32
    t->stop_event    = NULL;
    t->thread_handle = NULL;
    t->timer_handle  = NULL;
#else
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->stop_flag = 0;
#endif

    uint32_t timer_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, timer_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_destroy
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_destroy(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Stop if running */
    if (t->running) {
        t->running = 0;
#ifdef _WIN32
        if (t->stop_event) SetEvent(t->stop_event);
        if (t->thread_handle) {
            WaitForSingleObject(t->thread_handle, 3000);
            CloseHandle(t->thread_handle);
        }
        if (t->stop_event) CloseHandle(t->stop_event);
#else
        pthread_mutex_lock(&t->mtx);
        t->stop_flag = 1;
        pthread_cond_signal(&t->cv);
        pthread_mutex_unlock(&t->mtx);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
#endif
    }

    t->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_connect_event_queue
 *
 * r3 = timer_id
 * r4 = queue_id
 * r5 = source
 * r6 = data1
 * -----------------------------------------------------------------------*/
int64_t sys_timer_connect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    uint64_t source   = LV2_ARG_U64(ctx, 2);
    uint64_t data1    = LV2_ARG_U64(ctx, 3);
    uint64_t data2    = LV2_ARG_U64(ctx, 4);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = (int32_t)queue_id;
    t->source         = source;
    t->data1          = data1;
    t->data2          = data2;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_disconnect_event_queue
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_start
 *
 * r3 = timer_id
 * r4 = base_time (absolute start time, 0 = now)
 * r5 = period_usec
 * -----------------------------------------------------------------------*/
int64_t sys_timer_start(ppu_context* ctx)
{
    uint32_t timer_id    = LV2_ARG_U32(ctx, 0);
    /* uint64_t base_time = LV2_ARG_U64(ctx, 1); -- ignored for now */
    uint64_t period      = LV2_ARG_U64(ctx, 2);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (t->running)
        return (int64_t)(int32_t)CELL_EBUSY;

    if (period == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    t->period_usec = period;
    t->running     = 1;
    t->next_expire = timer_now_us() + period;

#ifdef _WIN32
    t->stop_event    = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->thread_handle = CreateThread(NULL, 0, timer_thread_proc, t, 0, NULL);
#else
    t->stop_flag = 0;
    pthread_create(&t->thread, NULL, timer_thread_proc, t);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_stop
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_stop(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (!t->running)
        return CELL_OK;

    t->running = 0;

#ifdef _WIN32
    if (t->stop_event) SetEvent(t->stop_event);
    if (t->thread_handle) {
        WaitForSingleObject(t->thread_handle, 3000);
        CloseHandle(t->thread_handle);
        t->thread_handle = NULL;
    }
    if (t->stop_event) {
        CloseHandle(t->stop_event);
        t->stop_event = NULL;
    }
#else
    pthread_mutex_lock(&t->mtx);
    t->stop_flag = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    pthread_join(t->thread, NULL);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * NOTE: There are syscall number collisions in the existing table:
 *   SYS_TIMER_USLEEP (141) == SYS_EVENT_FLAG_WAIT (141)
 *   SYS_TIMER_SLEEP  (142) == SYS_EVENT_FLAG_TRYWAIT (142)
 *   SYS_TIME_GET_CURRENT_TIME (145) == SYS_EVENT_FLAG_CANCEL (145)
 * The event flag handlers are registered separately and will win.
 * Timer sleep/time functions should be called via wrapper functions
 * or fixed syscall numbers should be assigned.
 * For now, we register the timer-specific syscalls that don't collide.
 * -----------------------------------------------------------------------*/
void sys_timer_init(lv2_syscall_table* tbl)
{
    memset(g_sys_timers, 0, sizeof(g_sys_timers));

#ifdef _WIN32
    ensure_qpc_init();
#endif

    lv2_syscall_register(tbl, SYS_TIMER_CREATE,                   sys_timer_create);
    lv2_syscall_register(tbl, SYS_TIMER_DESTROY,                  sys_timer_destroy);
    lv2_syscall_register(tbl, SYS_TIMER_START,                    sys_timer_start);
    lv2_syscall_register(tbl, SYS_TIMER_STOP,                     sys_timer_stop);
    lv2_syscall_register(tbl, SYS_TIMER_CONNECT_EVENT_QUEUE,      sys_timer_connect_event_queue);
    lv2_syscall_register(tbl, SYS_TIMER_DISCONNECT_EVENT_QUEUE,   sys_timer_disconnect_event_queue);

    /* These have conflicting numbers with event flag syscalls.
     * Register them here -- last registration wins. If events are
     * registered after timers, the event handlers will override.
     * The runtime should dispatch timer_usleep/sleep/get_current_time
     * via direct function calls instead. */
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEBASE_FREQUENCY, sys_time_get_timebase_frequency);
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEZONE, sys_time_get_timezone);

    /* Register these but be aware of collisions */
    lv2_syscall_register(tbl, SYS_TIMER_USLEEP,            sys_timer_usleep);
    lv2_syscall_register(tbl, SYS_TIMER_SLEEP,             sys_timer_sleep);
    lv2_syscall_register(tbl, SYS_TIME_GET_CURRENT_TIME,   sys_time_get_current_time);
}
