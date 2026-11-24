// R3 port: native fast paths for hot lifted PPU functions.
//
// ---- occlusion-query report wait (func_0074EAB0) ----
// Same engine routine as R2's func_006483A8 (project/r2_hot.cpp in the R2 port): at the start of
// frame N the main thread reads the visibility results of the occlusion queries it issued in
// frame N-1. It polls the 16-byte RSX report of the LAST query in the set until its timestamp is
// non-zero (sys_timer_usleep(30) per poll, 1 s timeout), then turns every report into a visible
// flag. On timeout it returns without touching the flags or marking the set as read. While it
// sleeps, the main thread and rsx::thread take turns instead of overlapping.
//   R3_OCC_SKIP=<n>      when the result is not in yet, do not wait: write the set visible at once (the
//                        timeout path below), at most n frames in a row per set (the n+1-th waits as
//                        before). Default 1 when R3_OCC_BLOCKING=0 (R2 airfield measured 36 -> 45 fps); the
//                        default R3_OCC_BLOCKING=1 forces 0.
//                        0 = the wait every frame. The default is 0 when R3_OCC_TIMEOUT_VISIBLE=0 or
//                        R3_OCC_TIMEOUT_PARTIAL=1 is set (those can keep a hidden value: never skip by default).
//                        A skipped set whose layout fails validation is not skipped (it waits).
//   R3_OCC_BLOCKING=1    DEFAULT (since 2026-11-12): the old blocking behaviour exactly (0.2.3): the game's own
//                        lifted wait, no native code (R3_OCC_SKIP=0 R3_OCC_WAIT_US=-1). =2: blocking, but the
//                        native wait capped at R3_OCC_WAIT_US (R3_OCC_SKIP=0). =0: the non-blocking skip (the
//                        previous default). Unset = 1, or 0 when R3_OCC_SKIP or R3_OCC_WAIT_US is set explicitly,
//                        so those switches keep working on their own.
//   R3_OCC_WAIT_US=<us>  wait natively at most <us>, then the timeout path (default 50000 = 50 ms, as R2:
//                        cuts only true hangs; RSX lags ~20 ms in heavy scenes. -1: the lifted wait)
//   R3_OCC_TIMEOUT_VISIBLE=0  on timeout/skip leave the previous flags (lifted). Default 1: every report
//                        of the set is made VISIBLE, never occluded/stale.
//   R3_OCC_TIMEOUT_PARTIAL=1  (opt-in) on timeout/skip evaluate reports whose timestamp is non-zero exactly
//                        and force only the others visible. Not the default: the lifted reader only ever
//                        polls the LAST report of a set (RSX writes them in order), so a non-zero timestamp
//                        on an earlier one may be last use's, and its stale count could hide an object.
//   R3_OCC_FENCE_FLIPS=<n>  after a skip or a visible timeout the object is "unproven" (all sets of an object share
//                        one report index space): the RSX may still write reports issued before then, late, into
//                        slots that later frames reuse (indices restart every frame, the game zeroes only the
//                        last report of a set), so a non-zero last-report timestamp proves nothing. Its sets keep
//                        being written all-visible -- also when the report "arrives" -- until rsx::thread has
//                        executed n more flip commands than at the mark (r2rsx_flips_executed; 2n guest frames
//                        when the RPCS3 renderer is not up), n guest frames passed, AND a wait then completes
//                        normally. Default 3. 0 = never mark (the build before this fence). Marks are never made
//                        with R3_OCC_TIMEOUT_VISIBLE=0 or R3_OCC_TIMEOUT_PARTIAL=1 (their old behaviour exactly)
//                        nor with R3_OCC_BLOCKING=1 / R3_OCC_WAIT_US=-1 and R3_OCC_SKIP=0 (the lifted wait, untouched).
// [occ] lines (every ~5 s): sets read, ready at once, skipped, waited, timeouts, unproven.
//   R3_OCC_LATE_LOG=1    DEFAULT: under R3_OCC_BLOCKING=1 the hook still counts (log only, then the lifted body):
//                        sets whose last report was ready when the game asked vs late (the game's wait blocks),
//                        and for ready sets how many reports cull (count <= the object's threshold). 0 = off.
//   RSX_ZCULL_GUEST_WAIT=1  DEFAULT: when the set is late, name its last report to rsx::thread (r2rsx_occ_guest_wait),
//                        which submits the command buffer holding that report's query at once instead of waiting for
//                        RSX_ZCULL_CRIT_FLUSH_US / a natural submit. Report values are untouched (exact). 0 = off.
//                        [occ] gw: late sets named, found claimed / claimed while waited / written unseen, submits,
//                        mean late->written latency per class, max, mean claim->written.
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include "ppu_recomp.h"

extern "C" uint64_t r2rsx_flips_executed(void);   /* rpcs3_bridge: UINT64_MAX when the RPCS3 renderer is not up */
extern "C" void r2rsx_occ_guest_wait(uint32_t ea);            /* rpcs3_bridge: RSX_ZCULL_GUEST_WAIT */
extern "C" int r2rsx_occ_guest_wait_stats(uint64_t out[11]);  /* rpcs3_bridge: 0 when off */
extern "C" int r2rsx_occ_spec_arm(uint32_t ea);               /* rpcs3_bridge: RSX_OCC_SPEC_VISIBLE report fence */
extern "C" uint32_t r2rsx_occ_spec_pending(void);
extern "C" uint64_t r2rsx_occ_nw_arm(void);                   /* rpcs3_bridge: R3_OCC_NOWAIT catch-up fence */
extern "C" int r2rsx_occ_nw_passed(uint64_t seq0);
extern "C" void r2rsx_occ_nw_disarm(void);

namespace {

constexpr uint64_t kLo = 0x10000u, kHi = 0xDFFFFFF8u;
constexpr uint64_t kPollLo = 0x20002000u, kPollHi = 0x20002010u;

inline bool span_ok(uint32_t ea, uint64_t n)
{
    const uint64_t a = ea & ~7ull, b = ((uint64_t)ea + n + 7) & ~7ull;
    return a >= kLo && b <= kHi && !(a < kPollHi && b > kPollLo);
}
inline uint32_t ld_be32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return _byteswap_ulong(v); }
inline uint16_t ld_be16(uint32_t ea) { uint16_t v; memcpy(&v, vm_base + ea, 2); return _byteswap_ushort(v); }

int env_int(const char* name, int def)
{
    const char* e = getenv(name);
    return (e && *e) ? atoi(e) : def;
}

int g_occ_init = 0;
long long g_occ_us = -1;
int g_occ_skip = 0;
int g_occ_block = 0;      /* R3_OCC_BLOCKING */
int g_occ_vis = 1;
int g_occ_partial = 0;
int g_occ_fence = 3;      /* R3_OCC_FENCE_FLIPS */
int g_occ_markon = 0;     /* marks are made: visible timeout/skip writes, fence on */
std::atomic<long long> g_occ_calls{0}, g_occ_ready{0}, g_occ_skipped{0}, g_occ_waited{0}, g_occ_timeouts{0}, g_occ_wait_ns{0};
std::atomic<long long> g_occ_unproven{0}, g_occ_marked{0}, g_occ_cleared{0};
std::atomic<long long> g_occ_rep_vis{0}, g_occ_rep_hid{0};   /* R3_OCC_LATE_LOG: reports of ready sets, visible / culled */
int g_occ_latelog = 1;    /* R3_OCC_LATE_LOG (default 1): count sets also under R3_OCC_BLOCKING=1 */
int g_occ_gw = 1;         /* RSX_ZCULL_GUEST_WAIT (default 1): name the awaited report to rsx::thread */
int g_sp_on = 0, g_sp_need = 2;   /* RSX_OCC_SPEC_VISIBLE, RSX_OCC_SPEC_NEED (see sp_update_mark) */
int g_sp_mark = 0;                /* global unproven period: every set read is written visible */
uint32_t g_sp_mark_frame = 0, g_sp_settle_frame = 0;
int g_sp_settling = 0;
std::atomic<long long> g_sp_avoided{0}, g_sp_forced{0}, g_sp_refused{0}, g_sp_mwait{0}, g_sp_episodes{0}, g_sp_mframes{0};
int g_nw_on = 0;                    /* R3_OCC_NOWAIT (see occ_nw_drain) */
long long g_nw_us = 2000;           /* R3_OCC_NOWAIT_US */
long long g_nw_drain_us = 100000;   /* R3_OCC_NOWAIT_DRAIN_MS */
int g_nw_pend = 0, g_nw_fail = 0;   /* main thread only */
uint32_t g_nw_frame = 0, g_nw_try_frame = 0;
int g_nw_tail = 0, g_nw_tail_settle = 0;   /* after giving up: in-flight reports not drained yet (main thread only) */
uint64_t g_nw_tail_seq = 0;
uint32_t g_nw_tail_frame = 0, g_nw_tail_settle_frame = 0;
std::atomic<long long> g_nw_skips{0}, g_nw_budget_ok{0}, g_nw_periods{0}, g_nw_drains{0}, g_nw_drain_ns{0}, g_nw_drain_max_ns{0},
    g_nw_drain_to{0}, g_nw_forced{0};
std::atomic<unsigned long long> g_occ_next{0};
struct OccSet { uint32_t obj; int32_t b; int run; };
OccSet g_occ_sets[16];   /* main thread only */
/* Unproven objects (main thread only). A full table marks every object (never evicts a mark). */
/* clearing: the fence passed and a wait completed in guest frame clear_frame. The mark stays in force for the rest of
   that frame (the game reads every set of an object in the same frame -- the caller at 0x00274E64 calls the wait per
   set b -- and a later set of this frame may still hold a late pre-fence report); freed by the first call in a later
   frame. */
struct OccMark { uint32_t obj; uint32_t frame; uint64_t flip; int clearing; uint32_t clear_frame; };
OccMark g_occ_marks[16];
int g_occ_mark_all = 0;
OccMark g_occ_all;

void occ_gw_report()   /* RSX_ZCULL_GUEST_WAIT stats since the last [occ] line */
{
    uint64_t s[11];
    static uint64_t l[11];
    if (!g_occ_gw || !r2rsx_occ_guest_wait_stats(s)) return;
    uint64_t d[11];
    for (int i = 0; i < 11; i++) d[i] = s[i] - l[i];
    d[9] = s[9];
    auto mean = [](uint64_t sum, uint64_t n) { return n ? (double)sum / 1000.0 / (double)n : 0.0; };
    const uint64_t named = d[2] + d[3];
    fprintf(stderr, "[occ] gw 5 s: late sets named %llu (raced %llu); found claimed %llu (late->written %.2f ms), claimed while "
                    "waited %llu (%.2f ms), written unseen %llu (%.2f ms); early submits %llu; max %.2f ms; claim->written "
                    "%.2f ms (RSX_ZCULL_GUEST_WAIT=0 off)\n",
            (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], mean(d[6], d[2]),
            (unsigned long long)d[3], mean(d[7], d[3]), (unsigned long long)d[4], mean(d[8], d[4]), (unsigned long long)d[5],
            (double)d[9] / 1000.0, mean(d[10], named));
    for (int i = 0; i < 11; i++) l[i] = s[i];
}

void occ_report()
{
    static long long lc, lr, ls, lw, lt, lns, lu, lm, lcl, lrv, lrh;
    const long long c = g_occ_calls.load(), r = g_occ_ready.load(), s = g_occ_skipped.load(), w = g_occ_waited.load(),
                    t = g_occ_timeouts.load(), ns = g_occ_wait_ns.load(), u = g_occ_unproven.load(), mk = g_occ_marked.load(),
                    cl = g_occ_cleared.load(), rv = g_occ_rep_vis.load(), rh = g_occ_rep_hid.load();
    if (g_occ_us < 0 && !g_occ_skip) {
        /* R3_OCC_BLOCKING=1: the game's own wait reads every result (nothing is visible for being late) */
        fprintf(stderr, "[occ] 5 s: sets %lld, last report ready when the game asked %lld (%.1f%%), late (game's own wait blocks "
                        "on the RSX) %lld; reports of ready sets: visible %lld, culled %lld (%.1f%%) (R3_OCC_LATE_LOG=0 off)\n",
                c - lc, r - lr, c - lc ? 100.0 * (double)(r - lr) / (double)(c - lc) : 0.0, w - lw, rv - lrv, rh - lrh,
                (rv - lrv) + (rh - lrh) ? 100.0 * (double)(rh - lrh) / (double)((rv - lrv) + (rh - lrh)) : 0.0);
        lc = c; lr = r; lw = w; lrv = rv; lrh = rh;
        if (g_sp_on) {
            static long long la, lf, lrf, lmw, le, lmf;
            const long long a = g_sp_avoided.load(), fo = g_sp_forced.load(), rf = g_sp_refused.load(), mw = g_sp_mwait.load(),
                            ep = g_sp_episodes.load(), mf = g_sp_mframes.load();
            fprintf(stderr, "[occ] spec-visible 5 s: waits avoided %lld in %lld periods, refused (fence slots full) %lld; "
                            "unproven frames %lld: sets forced visible %lld, native waits %lld (RSX_OCC_SPEC_VISIBLE=1, need %d)\n",
                    a - la, ep - le, rf - lrf, mf - lmf, fo - lf, mw - lmw, g_sp_need);
            la = a; lf = fo; lrf = rf; lmw = mw; le = ep; lmf = mf;
        }
        if (g_nw_on || g_nw_skips.load()) {
            static long long lsk, lbo, lpe, ldr, ldn, lto, lfo;
            const long long sk = g_nw_skips.load(), bo = g_nw_budget_ok.load(), pe = g_nw_periods.load(), dr = g_nw_drains.load(),
                            dn = g_nw_drain_ns.load(), to = g_nw_drain_to.load(), fo = g_nw_forced.load();
            const long long mx = g_nw_drain_max_ns.exchange(0);
            fprintf(stderr, "[occ] nowait 5 s: late sets drawn visible %lld in %lld frames, in within %lld us %lld; drains %lld "
                            "(%.2f ms avg, max %.2f ms, timeouts %lld), sets visible while undrained %lld (R3_OCC_NOWAIT=%d)\n",
                    sk - lsk, pe - lpe, g_nw_us, bo - lbo, dr - ldr, dr - ldr ? (double)(dn - ldn) / 1e6 / (double)(dr - ldr) : 0.0,
                    (double)mx / 1e6, to - lto, fo - lfo, g_nw_on);
            lsk = sk; lbo = bo; lpe = pe; ldr = dr; ldn = dn; lto = to; lfo = fo;
        }
        occ_gw_report();
        return;
    }
    fprintf(stderr, "[occ] report sets %lld: ready %lld, skipped %lld (R3_OCC_SKIP=%d), waited %lld", c - lc, r - lr, s - ls,
            g_occ_skip, w - lw);
    if (g_occ_us >= 0)
        fprintf(stderr, " (%.2f ms/s, %.0f us each, timeouts %lld, R3_OCC_WAIT_US=%lld)", (double)(ns - lns) / 1e7,
                w - lw ? (double)(ns - lns) / 1e3 / (double)(w - lw) : 0.0, t - lt, g_occ_us);
    if (mk - lm || u - lu || cl - lcl)
        fprintf(stderr, ", unproven: marked %lld, arrived-but-visible %lld, cleared %lld (R3_OCC_FENCE_FLIPS=%d)", mk - lm,
                u - lu, cl - lcl, g_occ_fence);
    fprintf(stderr, "\n");
    lc = c; lr = r; ls = s; lw = w; lt = t; lns = ns; lu = u; lm = mk; lcl = cl;
    occ_gw_report();
}

HANDLE occ_timer()   /* one per calling thread (the reader is the main thread; a shared timer would race) */
{
    static thread_local HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                          TIMER_ALL_ACCESS);
    return h;
}

inline void st_be32(uint32_t ea, uint32_t v) { v = _byteswap_ulong(v); memcpy(vm_base + ea, &v, 4); }

/* Timeout / skip path: instead of leaving last frame's flags (which can keep an object hidden),
 * write a VISIBLE value for every report of the set (the previous value if it was visible, else 1).
 * R3_OCC_TIMEOUT_PARTIAL=1: reports with a non-zero timestamp get what the lifted reader would write
 * (max(count - threshold, 0)) -- only safe if the game clears every report's timestamp per frame.
 * The set is still not marked read, exactly as the lifted timeout path. R3_OCC_TIMEOUT_VISIBLE=0:
 * the lifted behaviour (flags untouched). Returns the number of pending reports forced visible. */
int occ_force_visible(uint32_t obj, int32_t b)
{
    const uint32_t count = ld_be16(obj + 0x84 + 2 * b);
    const uint32_t list = ld_be32(obj + 0x74 + 4 * b);
    const uint32_t base = ld_be32(0x012D6F74);
    if (!span_ok(obj, 0x24)) return 0;
    const uint32_t flags = ld_be32(obj + 0x0), thr = ld_be32(obj + 0x20);
    int forced = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!span_ok(list + 2 * i, 2)) break;
        const uint32_t idx = ld_be16(list + 2 * i);
        const uint32_t rep = base + (idx << 4), fl = flags + (idx << 2);
        if (!span_ok(rep, 16) || !span_ok(fl, 4)) continue;
        uint64_t ts; memcpy(&ts, vm_base + rep, 8);
        if (ts && g_occ_partial) {
            const uint32_t val = ld_be32(rep + 8);
            st_be32(fl, val > thr ? val - thr : 0);
        } else {
            if (!ld_be32(fl)) st_be32(fl, 1);
            forced++;
        }
    }
    return forced;
}

/* The skip path writes every report of the set visible; only skip when all of them can be written (else a report
   would keep its previous, possibly hidden, value): the same address checks as occ_force_visible, all up front. */
bool occ_set_writable(uint32_t obj, int32_t b)
{
    if (!span_ok(obj, 0x24) || !span_ok(0x012D6F74, 4)) return false;
    const uint32_t count = ld_be16(obj + 0x84 + 2 * b);
    const uint32_t list = ld_be32(obj + 0x74 + 4 * b);
    if (!span_ok(list, 2 * (uint64_t)count)) return false;
    const uint32_t base = ld_be32(0x012D6F74), flags = ld_be32(obj + 0x0);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t idx = ld_be16(list + 2 * i);
        if (!span_ok(base + (idx << 4), 16) || !span_ok(flags + (idx << 2), 4)) return false;
    }
    return true;
}

/* Mark obj unproven at guest frame `frame`. Fence: flip commands rsx::thread has executed. Every report issued so far
   precedes, in the FIFO, the flip of the frame being built, so once the RSX has executed R3_OCC_FENCE_FLIPS more
   flips (and as many guest frames passed) those reports have been processed. */
void occ_mark(uint32_t obj, uint32_t frame)
{
    const uint64_t f = r2rsx_flips_executed();
    g_occ_marked.fetch_add(1, std::memory_order_relaxed);
    OccMark* free_e = nullptr;
    for (OccMark& m : g_occ_marks) {
        if (m.obj == obj) { m.frame = frame; m.flip = f; m.clearing = 0; return; }
        if (!m.obj && !free_e) free_e = &m;
    }
    if (free_e) { free_e->obj = obj; free_e->frame = frame; free_e->flip = f; free_e->clearing = 0; return; }
    g_occ_mark_all = 1; g_occ_all.obj = 1; g_occ_all.frame = frame; g_occ_all.flip = f; g_occ_all.clearing = 0;
}

bool occ_fence_passed(const OccMark& m, uint32_t frame)
{
    const uint32_t k = (uint32_t)g_occ_fence, df = frame - m.frame;
    const uint64_t f = r2rsx_flips_executed();
    if (f == UINT64_MAX || m.flip == UINT64_MAX) return df >= 2 * k;   /* no RSX flip count: frames only, doubled */
    return f - m.flip >= k && df >= k;
}

/* Frees marks whose clearing frame is over, then tells whether obj is (still) unproven in guest frame `frame`. */
bool occ_is_marked(uint32_t obj, uint32_t frame)
{
    for (OccMark& m : g_occ_marks)
        if (m.obj && m.clearing && m.clear_frame != frame) { m.obj = 0; m.clearing = 0; }
    if (g_occ_mark_all && g_occ_all.clearing && g_occ_all.clear_frame != frame) { g_occ_mark_all = 0; g_occ_all.clearing = 0; }
    if (g_occ_mark_all) return true;
    for (const OccMark& m : g_occ_marks) if (m.obj == obj) return true;
    return false;
}

/* Only after a normal completion: obj's mark (and the all-mark) whose fence passed starts clearing -- in force until
   this guest frame ends. The caller still writes this frame visible -- this frame's reset may predate the fence. */
void occ_try_clear(uint32_t obj, uint32_t frame)
{
    for (OccMark& m : g_occ_marks)
        if (m.obj == obj && !m.clearing && occ_fence_passed(m, frame)) {
            m.clearing = 1; m.clear_frame = frame; g_occ_cleared.fetch_add(1, std::memory_order_relaxed);
        }
    if (g_occ_mark_all && !g_occ_all.clearing && occ_fence_passed(g_occ_all, frame)) {
        g_occ_all.clearing = 1; g_occ_all.clear_frame = frame; g_occ_cleared.fetch_add(1, std::memory_order_relaxed);
    }
}

OccSet* occ_set(uint32_t obj, int32_t b)
{
    OccSet* lru = &g_occ_sets[0];
    for (OccSet& e : g_occ_sets) {
        if (e.obj == obj && e.b == b) return &e;
        if (!e.obj) { e.obj = obj; e.b = b; e.run = 0; return &e; }
    }
    lru->obj = obj; lru->b = b; lru->run = 0;
    return lru;
}

/* Native poll of a report timestamp (the lifted wait polls with sys_timer_usleep(30)). Returns 1 on timeout. */
int occ_native_wait(const volatile uint64_t* ts, long long wait_us)
{
    LARGE_INTEGER f, q0, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
    const long long lim = wait_us * f.QuadPart / 1000000;
    int timed_out = 0;
    for (unsigned i = 0;; i++) {
        if (*ts) break;
        QueryPerformanceCounter(&q);
        if (q.QuadPart - q0.QuadPart >= lim) { timed_out = 1; break; }
        if (i < 64) _mm_pause();
        else if (q.QuadPart - q0.QuadPart < f.QuadPart / 5000 || !occ_timer()) SwitchToThread();   /* first 200 us */
        else {   /* then poll every ~30 us like the lifted sys_timer_usleep(30), without burning the core */
            LARGE_INTEGER due; due.QuadPart = -300;
            if (SetWaitableTimer(occ_timer(), &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(occ_timer(), 5);
            else SwitchToThread();
        }
    }
    QueryPerformanceCounter(&q);
    g_occ_wait_ns.fetch_add((q.QuadPart - q0.QuadPart) * 1000000000 / f.QuadPart, std::memory_order_relaxed);
    return timed_out;
}

/* ---- RSX_OCC_SPEC_VISIBLE=1 (default 0; R3_OCC_BLOCKING=1 only) -------------------------------------------------
   A set whose last report is late, and whose reports were all VISIBLE in the last RSX_OCC_SPEC_NEED (default 2)
   real evaluations, is answered at once: every report of the set visible (never hidden; same writes as the skip
   path), the set not marked read. Its reports are still in flight and, under relaxed ZCULL, can land after the game
   has zeroed and reused slots (Resistance 2 port occlusion notes, "Popping root cause" -- why the game's
   own wait is the default). So the answer starts a global unproven period:
   - a report-write fence is armed on the skipped report (r2rsx_occ_spec_arm): rsx::thread frees it when it writes
     that report, after every report claimed before it;
   - until the fence passed AND two more guest frames began (their slots were zeroed after the last stale write),
     every set the game reads is written visible (ready ones too) and a late set waits natively, then visible;
   - new answers only in the guest frame that started the period (more fence slots), none when the slots are full.
   Counts are binary here (approximate ZCULL: visible = 0xFFFF), so no pixel-count (flare) consumer can be fooled
   by "visible"; "visible" only costs drawing an object the GPU would have culled. (state: see g_sp_on) */
struct SpecHist { uint32_t obj; int32_t b; uint32_t rep, count, frame; int real, vis_run; uint64_t used; };
SpecHist g_sp_hist[64];   /* main thread only */
uint64_t g_sp_use = 0;

SpecHist* sp_hist(uint32_t obj, int32_t b)
{
    SpecHist* lru = &g_sp_hist[0];
    for (SpecHist& e : g_sp_hist) {
        if (e.obj == obj && e.b == b) { e.used = ++g_sp_use; return &e; }
        if (e.used < lru->used) lru = &e;
    }
    *lru = SpecHist{ obj, b, 0, 0, 0, 0, 0, ++g_sp_use };
    return lru;
}

bool sp_flags_all_visible(uint32_t obj, uint32_t count, uint32_t list)
{
    if (!span_ok(obj, 4) || !span_ok(list, 2 * (uint64_t)count)) return false;
    const uint32_t flags = ld_be32(obj + 0x0);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t fl = flags + ((uint32_t)ld_be16(list + 2 * i) << 2);
        if (!span_ok(fl, 4) || !ld_be32(fl)) return false;
    }
    return true;
}

/* global unproven period bookkeeping, every call */
void sp_update_mark(uint32_t frame)
{
    if (!g_sp_mark || frame == g_sp_mark_frame) return;
    if (r2rsx_occ_spec_pending() != 0) { g_sp_settling = 0; return; }
    if (!g_sp_settling) { g_sp_settling = 1; g_sp_settle_frame = frame; return; }
    if (frame - g_sp_settle_frame >= 2) { g_sp_mark = 0; g_sp_settling = 0; }
}

/* ---- R3_OCC_NOWAIT (default 1; R3_OCC_BLOCKING=1, RSX_OCC_SPEC_VISIBLE=0, R3_OCC_TIMEOUT_PARTIAL=0 only), ported from R2 -----------------
   Frame S: a set read from the per-object caller (0x00274E64, the only call site of func_0074EAB0) whose last report is
   still not written after R3_OCC_NOWAIT_US (default 2000) is answered at once with occ_nw_visible (every flag
   1 -- the object is drawn and the depth test hides it if it really is occluded; never hidden). Sets that are
   ready in S keep their exact results. The skipped reports can still land after the game zeroed and reused their
   slots, so at the FIRST set read of frame S+1 the main thread drains the RSX once (occ_nw_drain): it waits until
   rsx::thread has executed every command put so far and every report claimed by then is written. Afterwards every
   slot holds its last issued report (writes are in order) and the game's own wait/results are exact again. A drain
   that does not finish in R3_OCC_NOWAIT_DRAIN_MS (default 100) answers that frame visible and retries next frame;
   after 3 failed drains in a row R3_OCC_NOWAIT turns itself off: every set waits the game's way and is drawn visible
   until the RSX was idle on its own once (non-blocking fence) and two more frames began, then the plain lifted wait.
   =0: the old blocking wait, exactly. */
int occ_nw_drain(long long* ns_out)   /* 0 passed, 1 timed out, -1 no RPCS3 renderer */
{
    LARGE_INTEGER f, q0, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
    const uint64_t seq0 = r2rsx_occ_nw_arm();
    if (seq0 == UINT64_MAX) { *ns_out = 0; return -1; }
    const long long lim = g_nw_drain_us * f.QuadPart / 1000000;
    int r;
    for (unsigned i = 0;; i++) {
        const int p = r2rsx_occ_nw_passed(seq0);
        if (p) { r = p > 0 ? 0 : -1; break; }
        QueryPerformanceCounter(&q);
        if (q.QuadPart - q0.QuadPart >= lim) { r = 1; break; }
        if (i < 64) _mm_pause();
        else if (q.QuadPart - q0.QuadPart < f.QuadPart / 5000 || !occ_timer()) SwitchToThread();
        else {
            LARGE_INTEGER due; due.QuadPart = -300;
            if (SetWaitableTimer(occ_timer(), &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(occ_timer(), 5);
            else SwitchToThread();
        }
    }
    r2rsx_occ_nw_disarm();
    QueryPerformanceCounter(&q);
    *ns_out = (q.QuadPart - q0.QuadPart) * 1000000000 / f.QuadPart;
    return r;
}

/* the set written visible (return 1 from func_0074EAB0), or 0 = layout unexpected: the lifted wait */
int occ_nw_visible(uint32_t obj, int32_t b)
{
    if (!occ_set_writable(obj, b)) return 0;
    /* always 1, as R2's r2_occ_write_visible: never a landed report count (a skipped report's slot can be stale) */
    const uint32_t count = ld_be16(obj + 0x84 + 2 * b);
    const uint32_t list = ld_be32(obj + 0x74 + 4 * b);
    const uint32_t flags = ld_be32(obj + 0x0);
    for (uint32_t i = 0; i < count; i++) {
        if (!span_ok(list + 2 * i, 2)) break;
        const uint32_t fl = flags + ((uint32_t)ld_be16(list + 2 * i) << 2);
        if (span_ok(fl, 4)) st_be32(fl, 1);
    }
    return 1;
}

/* R3_OCC_NOWAIT step for a lifted_only set read: 1/0 = r3_occ_wait's answer, -1 = go on with the normal path */
int occ_nw_step(uint32_t site_lr, uint32_t obj, int32_t b, uint32_t frame, const volatile uint64_t* ts, uint32_t rep)
{
    if (g_nw_pend && frame != g_nw_frame) {
        if (g_nw_try_frame != frame) {   /* first read of a later frame: one drain */
            g_nw_try_frame = frame;
            long long dns = 0;
            const int r = occ_nw_drain(&dns);
            g_nw_drain_ns.fetch_add(dns, std::memory_order_relaxed);
            if (dns > g_nw_drain_max_ns.load(std::memory_order_relaxed)) g_nw_drain_max_ns.store(dns, std::memory_order_relaxed);
            if (r == 0) { g_nw_pend = 0; g_nw_fail = 0; g_nw_drains.fetch_add(1, std::memory_order_relaxed); }
            else if (r < 0) {
                g_nw_pend = 0; g_nw_on = 0;
                fprintf(stderr, "[occ] R3_OCC_NOWAIT: no RPCS3 renderer fence, off\n");
            } else {
                g_nw_drain_to.fetch_add(1, std::memory_order_relaxed);
                if (++g_nw_fail >= 3) {
                    g_nw_on = 0; g_nw_pend = 0;
                    g_nw_tail = 1; g_nw_tail_settle = 0;
                    g_nw_tail_seq = r2rsx_occ_nw_arm();
                    if (g_nw_tail_seq == UINT64_MAX) g_nw_tail = 0;
                    fprintf(stderr, "[occ] R3_OCC_NOWAIT: %d drains in a row timed out (%lld ms): off, back to the "
                                    "game's own wait (all-visible until the RSX has caught up once)\n",
                            g_nw_fail, g_nw_drain_us / 1000);
                }
            }
        }
        if (g_nw_pend) {   /* not drained yet: a ready report may be a stale late write */
            if (!occ_nw_visible(obj, b)) return 0;
            g_nw_forced.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
    }
    if (g_nw_tail) {
        if (frame != g_nw_tail_frame) {
            g_nw_tail_frame = frame;
            if (!g_nw_tail_settle) {
                if (r2rsx_occ_nw_passed(g_nw_tail_seq)) { g_nw_tail_settle = 1; g_nw_tail_settle_frame = frame; r2rsx_occ_nw_disarm(); }
            } else if (frame - g_nw_tail_settle_frame >= 2) {
                g_nw_tail = 0;
                fprintf(stderr, "[occ] R3_OCC_NOWAIT: in-flight reports settled, the game's own wait from now on\n");
            }
        }
        if (g_nw_tail) {
            if (!*ts) { if (g_occ_gw) r2rsx_occ_guest_wait(rep); occ_native_wait(ts, 1000000); }
            if (!occ_nw_visible(obj, b)) return 0;
            g_nw_forced.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
    }
    if (!*ts && g_nw_on && site_lr == 0x00274E64u && (!g_nw_pend || frame == g_nw_frame)) {
        g_occ_waited.fetch_add(1, std::memory_order_relaxed);
        if (g_occ_gw) r2rsx_occ_guest_wait(rep);
        if (g_nw_us > 0 && !occ_native_wait(ts, g_nw_us)) {
            g_nw_budget_ok.fetch_add(1, std::memory_order_relaxed);
            return 0;   /* in within the budget: the lifted body reads the exact results */
        }
        if (!occ_nw_visible(obj, b)) return 0;   /* layout unexpected: the lifted wait */
        if (!g_nw_pend) g_nw_periods.fetch_add(1, std::memory_order_relaxed);
        g_nw_pend = 1; g_nw_frame = frame;
        g_nw_skips.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return -1;
}
}  // namespace

/* 1: return from func_0074EAB0 at once (its timeout path: no flags written, set not marked);
   0: run the lifted body. */
extern "C" int r3_occ_wait(ppu_context* ctx)
{
    if (__builtin_expect(!g_occ_init, 0)) {
        const char* e = getenv("R3_OCC_WAIT_US");
        /* default: wait natively at most 50 ms (as R2). RSX normally lags up to ~20 ms in heavy
         * scenes, so this only cuts true hangs (the lifted wait holds the main thread up to 1 s);
         * a few ms made objects pop. R3_OCC_WAIT_US=-1: the lifted wait. */
        g_occ_us = (e && *e) ? atoll(e) : 50000;
        g_occ_vis = env_int("R3_OCC_TIMEOUT_VISIBLE", 1) != 0;
        g_occ_partial = env_int("R3_OCC_TIMEOUT_PARTIAL", 0) != 0;
        if (g_occ_us < 0) g_occ_us = -1;
        g_occ_skip = env_int("R3_OCC_SKIP", (g_occ_vis && !g_occ_partial) ? 1 : 0);
        if (g_occ_skip < 0) g_occ_skip = 0;
        {
            /* Default 1 (user play test 2026-11-12 with relaxed ZCULL: no popping, 52.5 -> 55.1 fps, p10 36 -> 44;
             * in R2 the non-blocking skip + relaxed ZCULL made objects pop). An explicit R3_OCC_SKIP or
             * R3_OCC_WAIT_US selects the native path as before (default 0 then). R3_OCC_BLOCKING=0: the skip. */
            const char* sk = getenv("R3_OCC_SKIP");
            const int native_asked = (sk && *sk) || (e && *e);
            g_occ_block = env_int("R3_OCC_BLOCKING", native_asked ? 0 : 1);
        }
        if (g_occ_block == 1) { g_occ_skip = 0; g_occ_us = -1; }   /* 0.2.3 exactly: the game's own wait, nothing native */
        else if (g_occ_block >= 2) g_occ_skip = 0;                 /* blocking, native wait capped at R3_OCC_WAIT_US */
        g_occ_fence = env_int("R3_OCC_FENCE_FLIPS", 3);
        if (g_occ_fence < 0) g_occ_fence = 0;
        g_occ_markon = g_occ_vis && !g_occ_partial && g_occ_fence > 0;
        g_occ_latelog = env_int("R3_OCC_LATE_LOG", 1) != 0;
        g_occ_gw = env_int("RSX_ZCULL_GUEST_WAIT", 1) != 0;
        g_sp_need = env_int("RSX_OCC_SPEC_NEED", 2);
        if (g_sp_need < 1) g_sp_need = 1;
        /* not with R3_OCC_TIMEOUT_PARTIAL: occ_force_visible then writes landed (possibly stale, culled) values */
        g_sp_on = env_int("RSX_OCC_SPEC_VISIBLE", 0) != 0 && g_occ_block == 1 && !g_occ_partial;
        if (env_int("RSX_OCC_SPEC_VISIBLE", 0) != 0)
            fprintf(stderr, "[occ] RSX_OCC_SPEC_VISIBLE: %s (late sets visible %d real evaluations in a row are answered visible "
                            "at once, report-write fence + 2 frames all-visible after; RSX_OCC_SPEC_NEED)\n",
                    g_sp_on ? "on" : "off (needs R3_OCC_BLOCKING=1 and R3_OCC_TIMEOUT_PARTIAL=0)", g_sp_need);
        {
            const int nw = env_int("R3_OCC_NOWAIT", 1);
            g_nw_on = (g_occ_block == 1 && !g_sp_on && !g_occ_partial && nw > 0) ? 1 : 0;
            const char* u = getenv("R3_OCC_NOWAIT_US");
            g_nw_us = (u && *u) ? atoll(u) : 2000;
            if (g_nw_us < 0) g_nw_us = 0;
            long long dm = env_int("R3_OCC_NOWAIT_DRAIN_MS", 100);
            if (dm < 1) dm = 1;
            g_nw_drain_us = dm * 1000;
            if (g_nw_on)
                fprintf(stderr, "[occ] R3_OCC_NOWAIT=1: an occlusion set still late after %lld us (R3_OCC_NOWAIT_US) is drawn "
                                "visible instead of stalling the frame (caller 0x00274E64); the next frame drains the RSX once "
                                "(<= %lld ms, R3_OCC_NOWAIT_DRAIN_MS) so results stay exact. 0 = the game's own wait\n",
                        g_nw_us, g_nw_drain_us / 1000);
            else if (nw > 0)
                fprintf(stderr, "[occ] R3_OCC_NOWAIT: off (needs R3_OCC_BLOCKING=1, RSX_OCC_SPEC_VISIBLE=0 and R3_OCC_TIMEOUT_PARTIAL=0)\n");
        }
        g_occ_init = 1;
        fprintf(stderr, "[occ] occlusion report wait: skip %d frame(s) in a row, native timeout %lld us (-1 = lifted), "
                        "reports on timeout/skip: %s (R3_OCC_TIMEOUT_VISIBLE, R3_OCC_TIMEOUT_PARTIAL)%s\n",
                g_occ_skip, g_occ_us,
                !g_occ_vis ? "previous flags" : g_occ_partial ? "landed exact, pending visible" : "all visible",
                g_occ_block == 1 ? " (R3_OCC_BLOCKING=1: the game's own blocking wait, as 0.2.3)"
                : g_occ_block >= 2 ? " (R3_OCC_BLOCKING=2: blocking, native capped wait)"
                : g_occ_skip ? " (R3_OCC_BLOCKING=0: non-blocking skip)" : "");
        fprintf(stderr, "[occ] unproven fence after a skip/timeout: %s (R3_OCC_FENCE_FLIPS=%d)\n",
                g_occ_markon ? "on" : "off", g_occ_fence);
        g_occ_next.store(GetTickCount64() + 10000);
    }
    if (g_occ_us < 0 && !g_occ_skip && !g_occ_latelog && !g_occ_gw && !g_sp_on && !g_nw_on && !g_nw_pend && !g_nw_tail) return 0;
    const bool lifted_only = g_occ_us < 0 && !g_occ_skip;   /* R3_OCC_BLOCKING=1: count only, then the lifted body */

    /* same address arithmetic as the lifted entry; anything unexpected runs the lifted body */
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const int32_t b = (int32_t)ctx->gpr[4];
    if (b < 0 || b > 64) return 0;
    if (!span_ok(obj, 0x88 + 4 * (uint64_t)b) || !span_ok(0x012B1F20 + 0xB4, 4) || !span_ok(0x012D6F74, 4)) return 0;
    const uint32_t frame = ld_be32(0x012B1F20 + 0xB4);
    if (ld_be32(obj + 8 + 4 * b) < frame - 1) return 0;   /* not last frame's set: no wait */
    const uint32_t count = ld_be16(obj + 0x84 + 2 * b);
    if (!count) return 0;
    const uint32_t list = ld_be32(obj + 0x74 + 4 * b);
    if (!span_ok(list + 2 * (count - 1), 2)) return 0;
    const uint32_t last = ld_be16(list + 2 * (count - 1));
    const uint32_t rep = ld_be32(0x012D6F74) + (last << 4);
    if (!span_ok(rep, 8)) return 0;

    g_occ_calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned long long now = GetTickCount64();
    unsigned long long nx = g_occ_next.load(std::memory_order_relaxed);
    if (now >= nx && g_occ_next.compare_exchange_strong(nx, now + 5000)) occ_report();

    const volatile uint64_t* ts = (const volatile uint64_t*)(vm_base + rep);
    if (lifted_only) {
        SpecHist* h = nullptr;
        if (g_sp_on) {
            h = sp_hist(obj, b);
            if (h->frame != frame) {   /* first read of this set in this frame: score the previous evaluation */
                const bool cont = h->frame == frame - 1 && h->rep == rep && h->count == count;
                h->vis_run = (cont && h->real && sp_flags_all_visible(obj, count, list)) ? h->vis_run + 1 : 0;
                h->frame = frame; h->rep = rep; h->count = count;
            }
            h->real = 0;
            sp_update_mark(frame);
            static uint32_t last_mframe = 0;
            if (g_sp_mark && last_mframe != frame) { last_mframe = frame; g_sp_mframes.fetch_add(1, std::memory_order_relaxed); }
        }
        if (g_nw_on || g_nw_pend || g_nw_tail) {   /* R3_OCC_NOWAIT (see occ_nw_drain) */
            const int r = occ_nw_step((uint32_t)ctx->lr, obj, b, frame, ts, rep);
            if (r >= 0) return r;
        }
        /* R3_OCC_LATE_LOG: is the last report of the set already written when the game asks (ready), or does the
           game's own wait have to block on the RSX (late)? For ready sets, how many reports cull (count <= thr). */
        if (*ts) {
            g_occ_ready.fetch_add(1, std::memory_order_relaxed);
            if (span_ok(obj, 0x24) && span_ok(list, 2 * (uint64_t)count)) {
                const uint32_t base = ld_be32(0x012D6F74), thr = ld_be32(obj + 0x20);
                long long hid = 0, vis = 0;
                for (uint32_t i = 0; i < count; i++) {
                    const uint32_t r = base + ((uint32_t)ld_be16(list + 2 * i) << 4);
                    if (!span_ok(r, 12)) continue;
                    if (ld_be32(r + 8) > thr) vis++; else hid++;
                }
                g_occ_rep_vis.fetch_add(vis, std::memory_order_relaxed);
                g_occ_rep_hid.fetch_add(hid, std::memory_order_relaxed);
            }
            if (h && g_sp_mark) {   /* unproven period: the value may be a stale pre-fence write */
                occ_force_visible(obj, b);
                g_sp_forced.fetch_add(1, std::memory_order_relaxed);
                return 1;
            }
            if (h) h->real = 1;
        } else {
            g_occ_waited.fetch_add(1, std::memory_order_relaxed);
            if (g_occ_gw) r2rsx_occ_guest_wait(rep);   /* RSX_ZCULL_GUEST_WAIT: submit this report's query now */
            if (h) {
                if (h->vis_run >= g_sp_need && (!g_sp_mark || frame == g_sp_mark_frame) && occ_set_writable(obj, b)) {
                    if (r2rsx_occ_spec_arm(rep)) {
                        occ_force_visible(obj, b);
                        h->vis_run = 0;
                        if (!g_sp_mark) {
                            g_sp_mark = 1; g_sp_mark_frame = frame; g_sp_settling = 0;
                            g_sp_episodes.fetch_add(1, std::memory_order_relaxed);
                        }
                        g_sp_avoided.fetch_add(1, std::memory_order_relaxed);
                        return 1;
                    }
                    g_sp_refused.fetch_add(1, std::memory_order_relaxed);
                }
                if (g_sp_mark) {   /* unproven period: wait natively (lifted 1 s cap), then visible */
                    g_sp_mwait.fetch_add(1, std::memory_order_relaxed);
                    occ_native_wait(ts, 1000000);
                    occ_force_visible(obj, b);
                    g_sp_forced.fetch_add(1, std::memory_order_relaxed);
                    return 1;
                }
                h->real = 1;
            }
        }
        return 0;
    }
    OccSet* st = occ_set(obj, b);
    const bool marked = g_occ_markon && occ_is_marked(obj, frame);
    /* the last report arrived; for an unproven object it may be a late write from before the skip/timeout into a
       reused slot: write the set visible and return like the skip path (set not marked read) */
    auto occ_done = [&]() -> int {
        if (!marked) return 0;
        occ_try_clear(obj, frame);
        occ_force_visible(obj, b);
        g_occ_unproven.fetch_add(1, std::memory_order_relaxed);
        return 1;
    };
    if (*ts) { st->run = 0; g_occ_ready.fetch_add(1, std::memory_order_relaxed); return occ_done(); }
    if (st->run < g_occ_skip && (!g_occ_vis || occ_set_writable(obj, b))) {
        st->run++; g_occ_skipped.fetch_add(1, std::memory_order_relaxed);
        if (g_occ_vis) occ_force_visible(obj, b);
        if (g_occ_markon) occ_mark(obj, frame);
        return 1;
    }
    st->run = 0;
    g_occ_waited.fetch_add(1, std::memory_order_relaxed);
    if (g_occ_gw) r2rsx_occ_guest_wait(rep);
    if (g_occ_us < 0 && !marked) return 0;   /* the lifted wait */

    /* an unproven object under R3_OCC_WAIT_US=-1 waits natively with the lifted 1 s timeout */
    const long long wait_us = g_occ_us < 0 ? 1000000 : g_occ_us;
    const int timed_out = occ_native_wait(ts, wait_us);
    if (!timed_out) return occ_done();   /* ready: the lifted body reads the results (unless unproven) */
    {
        const int forced = g_occ_vis ? occ_force_visible(obj, b) : -1;
        if (g_occ_markon) occ_mark(obj, frame);
        const long long n = g_occ_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5 || (n & (n - 1)) == 0)   /* first 5, then at powers of two */
            fprintf(stderr, "[occ] report wait timed out after %lld us (set %08x/%d, %u reports, %d forced visible, "
                            "timeout #%lld; R3_OCC_WAIT_US=-1: lifted wait)\n",
                    wait_us, obj, b, count, forced, n);
    }
    return 1;
}
