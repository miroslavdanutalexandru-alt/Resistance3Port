/*
 * ps3recomp - frame hitch logger (PORT_HITCH_LOG, default off; PORT_HITCH_LOG=1 turns it on).
 *
 * Hot paths (game, RSX, render, mixer threads) add the time they spent in a few
 * known-blocking places to per-category accumulators: one rdtsc pair and one
 * interlocked add, no lock, no I/O (off: one load and branch). Once per game frame the frame limiter
 * (project/stubs.cpp, r2_frame_period) calls port_hitch_frame(), which takes the
 * accumulators and prints a single [hitch] line for a frame over PORT_HITCH_MS
 * (default 25 ms, raised to 1.5x the last 5 s p50 unless PORT_HITCH_ADAPT=0; at most
 * PORT_HITCH_LINES=10 lines per 5 s) and a [hitch] 5 s summary (p50 / p99 / max frame time).
 *
 * Header-only (selectany state) so C runtime libs, the RPCS3 RSX library and the
 * port all share one copy without a link dependency. The reporting half is
 * compiled where PORT_HITCH_IMPLEMENTATION is defined (stubs.cpp).
 */
#ifndef PORT_HITCH_H
#define PORT_HITCH_H
#include <stdint.h>
#include <stdlib.h>
#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#endif

enum port_hitch_cat {
    PH_LIMIT = 0,   /* main thread: frame limiter wait (stubs.cpp)                       */
    PH_TICK,        /* main thread: per-frame PC extras (overlay, benchmark, Reflex sleep) */
    PH_SYNC,        /* main thread: blocking lv2 waits (event queue/flag, semaphore, cond) */
    PH_PAD,         /* game thread: cellPadGetData                                         */
    PH_RSX,         /* rsx::thread: longest flip-to-flip interval (max, not a sum)         */
    PH_PRESENT,     /* rsx::thread: present_frame (DLSS, overlays, submit, vkQueuePresent) */
    PH_ACQUIRE,     /* rsx::thread: vkAcquireNextImageKHR                                  */
    PH_GPUWAIT,     /* rsx::thread: waits for an older frame's command buffer (GPU bound)  */
    PH_DLSS,        /* rsx::thread: r2aa_on_flip (upscaler dispatch)                       */
    PH_PIPE,        /* render thread: pipeline / shader compile stalls (VKR2Pipe)          */
    PH_TEX,         /* rsx::thread: texture uploads (incl. texture-pack hashing)           */
    PH_AUDIO,       /* mixer thread: late guest block wait                                 */
    PH_PRESENT_IV,  /* rsx::thread: longest present-to-present interval (max, not a sum)   */
    PH_FIFOIDLE,    /* rsx::thread: FIFO empty, waiting for the PPU to put commands         */
    PH_LABEL,       /* rsx::thread: NV406E semaphore acquire waits (label not yet released) */
    PH_ZCULL,       /* rsx::thread: ZCULL / occlusion report reads waiting for the GPU      */
    PH_HARDSYNC,    /* rsx::thread: flush_command_queue(hard_sync) (submit + GPU idle)      */
    PH_SUBMIT,      /* rsx::thread: command buffer submits (vkQueueSubmit)                  */
    PH_RSXLIMIT,    /* rsx::thread: RPCS3 flip frame limiter sleep                          */
    PH_RING,        /* PPU (any thread): command ring wrap, waiting for the RSX to drain it */
    PH_AVFLUSH,     /* PPU / SPU (any thread): fault on a GPU-written surface, RSX readback */
    PH_MAINRSX,     /* frame-loop thread's share of PH_RING + PH_AVFLUSH (not "game" time)  */
    PH_COUNT
};

#define PORT_HITCH_LR_SLOTS 6

typedef struct port_hitch_state {
    volatile int64_t acc[PH_COUNT];   /* TSC ticks since the last frame                 */
    volatile int64_t mx[PH_COUNT];    /* longest single event since the last frame      */
    volatile long    cnt[PH_COUNT];   /* events since the last frame                    */
    volatile double  tsc_per_ns;      /* calibrated by port_hitch_frame (0: not yet)    */
    volatile unsigned long main_tid;  /* host thread id of the game's frame loop        */
    volatile int     off;             /* 1 unless PORT_HITCH_LOG is set (non-zero)      */
    /* main thread only: guest waits (usleep / lwmutex / lwcond) by guest call site, QPC ticks */
    uint32_t         lr[PORT_HITCH_LR_SLOTS];
    int64_t          lr_qpc[PORT_HITCH_LR_SLOTS];
} port_hitch_state;

#ifdef __cplusplus
__declspec(selectany) port_hitch_state g_port_hitch = { {}, {}, {}, 0.0, 0, 1 };
#else
__declspec(selectany) port_hitch_state g_port_hitch = { { 0 }, { 0 }, { 0 }, 0.0, 0, 1 };
#endif

/* PORT_HITCH_RSX_SPLIT (default off; 1 = rsx::thread flip intervals over 25 ms, <ms> >= 5 = that threshold): a
 * "[hitch] rsx flip" line per long flip-to-flip interval of rsx::thread (the [hitch] line only has its length,
 * rsx-iv): the rsx-cpu split of that interval (PORT_RSX_CPU_PROF, forced to every frame unless set), the thread's
 * CPU time against the wall time (off-cpu = blocked in the kernel / driver, sleeping in a wait, or preempted) and
 * the timed waits of the interval. While on, the timed categories are also added to the never-reset totals below;
 * off, one load and branch per event. At most PORT_HITCH_LINES (10) lines per 5 s. fn is set by port_hitch_frame
 * (stubs.cpp) and called on rsx::thread by port_rcpu_flip. */
typedef struct port_hitch_split_state {
    volatile int     on;
    double           thr_ms;
    volatile int64_t acc[PH_COUNT];
    volatile long    cnt[PH_COUNT];
    void (*volatile fn)(int sampled);
} port_hitch_split_state;

#ifdef __cplusplus
__declspec(selectany) port_hitch_split_state g_port_hsplit = {};
#else
__declspec(selectany) port_hitch_split_state g_port_hsplit = { 0 };
#endif

static __inline uint64_t port_hitch_t0(void) { return __rdtsc(); }

static __inline void port_hitch_add_ticks(int cat, int64_t d)
{
    if (d <= 0 || g_port_hitch.off) return;
    _InterlockedExchangeAdd64((volatile long long*)&g_port_hitch.acc[cat], d);
    _InterlockedIncrement(&g_port_hitch.cnt[cat]);
    if (g_port_hsplit.on) {
        _InterlockedExchangeAdd64((volatile long long*)&g_port_hsplit.acc[cat], d);
        _InterlockedIncrement(&g_port_hsplit.cnt[cat]);
    }
    if (d > g_port_hitch.mx[cat]) g_port_hitch.mx[cat] = d;   /* racy max: diagnostics only */
}

/* Time since port_hitch_t0() into a category. */
static __inline void port_hitch_end(int cat, uint64_t t0)
{
    port_hitch_add_ticks(cat, (int64_t)(__rdtsc() - t0));
}

/* A wait polled in steps (e.g. the RSX semaphore acquire loop): call once per
 * poll iteration; the gaps between calls under 0.5 ms are summed, and a longer
 * gap starts a new wait (the event count). */
static __inline void port_hitch_poll_step(int cat, uint64_t* last)
{
    if (g_port_hitch.off) return;
    const uint64_t now = __rdtsc();
    const double k = g_port_hitch.tsc_per_ns;
    const int64_t d = (int64_t)(now - *last);
    *last = now;
    if (k > 0.0 && d > 0 && (double)d < k * 5e5) {
        _InterlockedExchangeAdd64((volatile long long*)&g_port_hitch.acc[cat], d);
        if (d > g_port_hitch.mx[cat]) g_port_hitch.mx[cat] = d;
        if (g_port_hsplit.on) _InterlockedExchangeAdd64((volatile long long*)&g_port_hsplit.acc[cat], d);
    } else {
        _InterlockedIncrement(&g_port_hitch.cnt[cat]);
        if (g_port_hsplit.on) _InterlockedIncrement(&g_port_hsplit.cnt[cat]);
    }
}

static __inline void port_hitch_add_ns(int cat, uint64_t ns)
{
    const double k = g_port_hitch.tsc_per_ns;
    port_hitch_add_ticks(cat, (int64_t)((double)ns * (k > 0.0 ? k : 3.0)));
}

/* Longest-interval categories (PH_RSX): keeps the max only. */
static __inline void port_hitch_max_ticks(int cat, int64_t d)
{
    if (g_port_hitch.off) return;
    if (d > g_port_hitch.mx[cat]) g_port_hitch.mx[cat] = d;
    _InterlockedIncrement(&g_port_hitch.cnt[cat]);
}

/* 1 on the game's frame-loop thread (set by port_hitch_frame). */
static __inline int port_hitch_on_main(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    return g_port_hitch.main_tid && __readgsdword(0x48) == g_port_hitch.main_tid;   /* TEB ClientId.UniqueThread */
#else
    return 0;
#endif
}

/* A guest thread blocked inside an HLE call until the RSX catches up (ring wrap,
 * surface readback). These are not guest usleeps, so without this the frame
 * loop's share read as "game" (PPU) time in the [hitch] line. */
static __inline void port_hitch_end_ppu_rsx(int cat, uint64_t t0)
{
    const int64_t d = (int64_t)(__rdtsc() - t0);
    port_hitch_add_ticks(cat, d);
    if (port_hitch_on_main()) port_hitch_add_ticks(PH_MAINRSX, d);
}

/* A guest wait on the frame-loop thread (sys_timer usleep, lwmutex, lwcond): its
 * guest call site and QPC ticks, so a [hitch] line names what the game waited on.
 * Written and read on the main thread only (no atomics). */
static __inline void port_hitch_guest_wait(uint32_t lr, int64_t qpc)
{
    int i, lo = 0;
    if (g_port_hitch.off || !port_hitch_on_main()) return;
    for (i = 0; i < PORT_HITCH_LR_SLOTS; i++) {
        if (g_port_hitch.lr[i] == lr) { g_port_hitch.lr_qpc[i] += qpc; return; }
        if (g_port_hitch.lr_qpc[i] < g_port_hitch.lr_qpc[lo]) lo = i;
    }
    if (g_port_hitch.lr_qpc[lo] <= qpc) { g_port_hitch.lr[lo] = lr; g_port_hitch.lr_qpc[lo] = qpc; }
}

/* ------------------------------------------------------------------------- */
/* rsx::thread CPU time by category (PORT_RSX_CPU_PROF, default off; on with PORT_HITCH_LOG=1
 * (1 frame in 8) or PORT_RSX_CPU_PROF=1 (every frame) / N (1 in N)).
 * Exclusive time: the thread is always in exactly one category. Scopes (port_rcpu_scope) switch the current one
 * and restore the previous on exit; everything outside a scope is RC_FIFO (command decode / method handlers,
 * which also holds the FIFO-empty and semaphore-acquire waits: the summary subtracts PH_FIFOIDLE and PH_LABEL
 * from it). One rdtsc and two plain adds per switch, written by rsx::thread only (other threads fail the thread
 * id check and return at once); the frame loop reads the 64-bit accumulators without a lock every 5 s. */
enum port_rcpu_cat {
    RC_FIFO = 0,  /* decode, method handlers, idle and label waits (outside every scope)            */
    RC_STATE,     /* draw begin/end bookkeeping, program env + constants, descriptors, dynamic state  */
    RC_SHADER,    /* VP/FP resolve + pipeline lookup                                                 */
    RC_VERTEX,    /* vertex / index upload and vertex layout (vertex_cache)                          */
    RC_TEXTURE,   /* texture cache lookups, uploads, sampler setup, view binding                     */
    RC_SURFACE,   /* surface cache: framebuffer bind / memory locks                                  */
    RC_DRAW,      /* draw command recording                                                          */
    RC_ZCULL,     /* ZCULL / occlusion report handling (incl. waits for results)                     */
    RC_SUBMIT,    /* flush_command_queue: close + submit the command buffer (incl. hard-sync waits)   */
    RC_READBACK,  /* texture-cache flush / ads-rb readback waits                                     */
    RC_PRESENT,   /* flip: present, DLSS/AA, overlays, frame limiter                                 */
    RC_BLIT,      /* blit engine                                                                     */
    /* PORT_RSX_CPU_DETAIL=1 (default off): finer splits of state and decode. Only entered through
     * port_rcpu_enter_d / port_rcpu_dscope; the 5 s summary folds each into its parent (port_rcpu_parent), so the
     * main line keeps its meaning, and prints them on an extra "[hitch] rsx-cpu detail" line. */
    RC_S_DECODE,  /* state: decode_draw_state (register -> draw state)                                */
    RC_S_AA,      /* state: r2aa_on_draw (upscaler scene tracking, camera-vote capture, objmv)        */
    RC_S_ENV,     /* state: rest of load_program_env (gx/fg hooks, vertex/fragment env + constants)    */
    RC_S_BARRIER, /* state: surface write barriers                                                    */
    RC_S_QUERY,   /* state: r3fx_pre_draw + occlusion query allocate/begin                             */
    RC_S_BIND,    /* state: vertex descriptor bind_uniform + renderpass_op + m_program->bind          */
    RC_S_RPASS,   /* state: update_draw_state + begin_render_pass                                      */
    RC_S_SCISSOR, /* state: backend begin_draw (set_scissor)                                           */
    RC_S_XFORM,   /* state: transform-constant barrier replay inside a draw (execute_pipeline_dependencies) */
    RC_D_XFORM,   /* decode: transform constant / transform program loads                              */
    RC_D_CLEAR,   /* decode: NV4097_CLEAR_SURFACE (clear_surface)                                      */
    RC_D_POST,    /* decode: post_draw_hook and the nop-draw path                                      */
    RC_COUNT
};

static __inline int port_rcpu_parent(int c)
{
    return c < RC_S_DECODE ? c : c <= RC_S_XFORM ? RC_STATE : RC_FIFO;
}

typedef struct port_rcpu_state {
    volatile unsigned long tid;       /* rsx::thread id; 0 = profiler off / not bound */
    volatile unsigned long atid;      /* tid while the current frame is sampled, else 0 (port_rcpu_flip) */
    uint32_t         every;           /* sample 1 frame in every (PORT_RSX_CPU_PROF, default 8)          */
    volatile uint32_t flips, flips_sampled; /* flip_command calls; flip intervals that were sampled  */
    int              cur;
    int              detail;          /* PORT_RSX_CPU_DETAIL=1 */
    uint64_t         last;
    volatile int64_t acc[RC_COUNT];   /* TSC ticks, monotonic (the reader takes differences) */
    volatile uint64_t draws;          /* detail only: core end() calls */
    volatile uint32_t methods;        /* detail only: last FIFO method count seen at a draw (g_r2_fifo_methods) */
} port_rcpu_state;

#ifdef __cplusplus
__declspec(selectany) port_rcpu_state g_port_rcpu = {};
#else
__declspec(selectany) port_rcpu_state g_port_rcpu = { 0 };
#endif

/* PORT_HITCH_RSX_SPLIT method split (on with the split, PORT_HITCH_RSX_METH=0 off): rsx::thread decode self time
 * (RC_FIFO and its detail sub-buckets; nested scopes such as state/texture/submit excluded) per FIFO method register
 * and per housekeeping pass, reset at every flip. A long flip's "[hitch] rsx flip" line is followed by a
 * "[hitch] rsx decode" line naming the top methods. Two rdtsc per method while on; off, one load and branch.
 * Written and read on rsx::thread only. */
#define PORT_RMETH_HK_LIGHT 0x4000
#define PORT_RMETH_HK_FULL  0x4001
#define PORT_RMETH_HK_IDLE  0x4002   /* both passes while the FIFO is idle: inside fifo-idle, not decode */
#define PORT_RMETH_KEYS     0x4003
typedef struct port_rcpu_meth_state {
    int      on;
    uint32_t nt;
    int64_t  t[PORT_RMETH_KEYS];
    uint32_t n[PORT_RMETH_KEYS];
    uint16_t touched[PORT_RMETH_KEYS];
} port_rcpu_meth_state;

#ifdef __cplusplus
__declspec(selectany) port_rcpu_meth_state g_port_rmeth = {};
#else
__declspec(selectany) port_rcpu_meth_state g_port_rmeth = { 0 };
#endif

/* PORT_HITCH_RSX_METH: NV0039_BUFFER_NOTIFY (0x2328, the memory-to-memory copy) by phase (nv0039.cpp), in rsx::thread wall
 * ticks (nested profiler scopes such as a zcull wait or a fault's GPU readback included). Printed after the method line
 * of a long flip and reset with it. rsx::thread only. */
enum { PORT_NV39_PRE = 0, PORT_NV39_INVAL, PORT_NV39_MM, PORT_NV39_COPY, PORT_NV39_WBAR, PORT_NV39_PHASES };
typedef struct port_rnv39_state {
    int64_t  t[PORT_NV39_PHASES];
    int64_t  copy_max;      /* slowest single copy */
    int64_t  fault_t;       /* rsx::thread time in the access-violation hook during the copies */
    uint64_t bytes;         /* bytes written */
    uint32_t n, n_local, n_overlap, n_faulted, faults, inval_pages;
} port_rnv39_state;

#ifdef __cplusplus
__declspec(selectany) port_rnv39_state g_port_rnv39 = {};
#else
__declspec(selectany) port_rnv39_state g_port_rnv39 = { 0 };
#endif

/* Called once on rsx::thread before its FIFO loop. */
static __inline void port_rcpu_bind(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    const char* e = getenv("PORT_RSX_CPU_PROF");
    const char* h = getenv("PORT_HITCH_LOG");
    if (!(e && *e && *e != '0') && !(h && *h && *h != '0')) return;   /* opt-in */
    const char* d = getenv("PORT_RSX_CPU_DETAIL");
    g_port_rcpu.detail = d && *d && *d != '0';
    /* unset: 1 frame in 8 (two rdtsc per scope cost ~0.4 ms per sampled frame at 2400 draws); 1: every frame;
     * N >= 2: 1 in N. PORT_RSX_CPU_DETAIL forces every frame; so does PORT_HITCH_RSX_SPLIT unless this is set. */
    const char* sp = getenv("PORT_HITCH_RSX_SPLIT");
    const int split = sp && *sp && *sp != '0';
    if (split) {
        const double v = atof(sp);
        g_port_hsplit.thr_ms = v >= 5.0 ? v : 25.0;
        g_port_hsplit.on = 1;
        const char* m = getenv("PORT_HITCH_RSX_METH");
        g_port_rmeth.on = !(m && *m == '0');
    }
    const int ev = e && *e >= '1' && *e <= '9' ? atoi(e) : split ? 1 : 8;
    g_port_rcpu.every = g_port_rcpu.detail || ev < 1 ? 1u : (uint32_t)ev;
    g_port_rcpu.cur = RC_FIFO;
    g_port_rcpu.last = __rdtsc();
    g_port_rcpu.tid = __readgsdword(0x48);
    g_port_rcpu.atid = g_port_rcpu.every <= 1 ? g_port_rcpu.tid : 0;
#endif
}

/* Switch to cat; returns the previous category for port_rcpu_leave, or -1 (not rsx::thread / off). */
static __inline int port_rcpu_enter(int cat)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (__readgsdword(0x48) != g_port_rcpu.atid) return -1;
    const uint64_t now = __rdtsc();
    const int prev = g_port_rcpu.cur;
    g_port_rcpu.acc[prev] += (int64_t)(now - g_port_rcpu.last);
    g_port_rcpu.last = now;
    g_port_rcpu.cur = cat;
    return prev;
#else
    (void)cat; return -1;
#endif
}

static __inline void port_rcpu_leave(int prev)
{
    if (prev < 0 || !g_port_rcpu.atid) return;   /* sampling stopped inside the scope: drop it */
    const uint64_t now = __rdtsc();
    g_port_rcpu.acc[g_port_rcpu.cur] += (int64_t)(now - g_port_rcpu.last);
    g_port_rcpu.last = now;
    g_port_rcpu.cur = prev;
}

/* PORT_RSX_CPU_DETAIL sub-category: a single load and branch when detail is off */
static __inline int port_rcpu_enter_d(int cat)
{
    return g_port_rcpu.detail ? port_rcpu_enter(cat) : -1;
}

/* Detail scope for code reached from both a FIFO handler and draw execution: the sub-bucket follows the parent
 * category the thread is in now (decode -> d_cat, state -> s_cat, anything else stays where it is). */
static __inline int port_rcpu_enter_d2(int d_cat, int s_cat)
{
    if (!g_port_rcpu.detail) return -1;
    const int p = port_rcpu_parent(g_port_rcpu.cur);
    return p == RC_FIFO ? port_rcpu_enter(d_cat) : p == RC_STATE ? port_rcpu_enter(s_cat) : -1;
}

/* Detail counters, once per draw (core end) */
static __inline void port_rcpu_count_draw(uint32_t fifo_methods)
{
    if (!g_port_rcpu.detail) return;
    g_port_rcpu.draws++;
    g_port_rcpu.methods = fifo_methods;
}

/* Accrue the open interval now (idle loops on rsx::thread): a long wait (pause, alt-tab) is counted in the frames it
 * spans, so the [hitch] summary drops it with the discarded frames instead of adding it to the first frame after it. */
static __inline void port_rcpu_tick(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (__readgsdword(0x48) != g_port_rcpu.atid) return;
    const uint64_t now = __rdtsc();
    g_port_rcpu.acc[g_port_rcpu.cur] += (int64_t)(now - g_port_rcpu.last);
    g_port_rcpu.last = now;
#endif
}

/* Decode time so far (RC_FIFO plus the RC_D_* detail buckets folded into it), open interval accrued. */
static __inline int64_t port_rmeth_fifo(void)
{
    port_rcpu_tick();
    int64_t s = 0;
    for (int i = 0; i < RC_COUNT; i++)
        if (port_rcpu_parent(i) == RC_FIFO) s += g_port_rcpu.acc[i];
    return s;
}

/* Around a FIFO method handler / housekeeping pass: begin returns a mark, or -1 (off / not a sampled rsx::thread frame). */
static __inline int64_t port_rmeth_begin(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (!g_port_rmeth.on || __readgsdword(0x48) != g_port_rcpu.atid) return -1;
    return port_rmeth_fifo();
#else
    return -1;
#endif
}

static __inline void port_rmeth_end(uint32_t key, int64_t mark)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (mark < 0 || key >= PORT_RMETH_KEYS || __readgsdword(0x48) != g_port_rcpu.atid) return;
    const int64_t d = port_rmeth_fifo() - mark;
    if (g_port_rmeth.n[key]++ == 0 && g_port_rmeth.nt < PORT_RMETH_KEYS) g_port_rmeth.touched[g_port_rmeth.nt++] = (uint16_t)key;
    if (d > 0) g_port_rmeth.t[key] += d;
#else
    (void)key; (void)mark;
#endif
}

/* rsx::thread, at each flip_command before its scope opens (only RC_FIFO is current): starts or stops sampling, so
 * a sampled interval is a whole flip-to-flip frame. The summary scales sampled time by flips / flips_sampled. */
static __inline void port_rcpu_flip(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (!g_port_rcpu.tid || __readgsdword(0x48) != g_port_rcpu.tid) return;
    const int was_on = g_port_rcpu.atid != 0;
    if (was_on) g_port_rcpu.flips_sampled = g_port_rcpu.flips_sampled + 1;
    const uint32_t n = g_port_rcpu.flips + 1;
    g_port_rcpu.flips = n;
    if (g_port_hsplit.on) {   /* PORT_HITCH_RSX_SPLIT: report the interval that ends here */
        void (*const f)(int) = g_port_hsplit.fn;
        if (f) {
            if (was_on) {
                const uint64_t t = __rdtsc();
                g_port_rcpu.acc[g_port_rcpu.cur] += (int64_t)(t - g_port_rcpu.last);
                g_port_rcpu.last = t;
            }
            f(was_on);
        }
    }
    const int want = g_port_rcpu.every <= 1 || n % g_port_rcpu.every == 0;
    if (want == was_on) return;
    const uint64_t now = __rdtsc();
    if (was_on) g_port_rcpu.acc[g_port_rcpu.cur] += (int64_t)(now - g_port_rcpu.last);
    g_port_rcpu.cur = RC_FIFO;
    g_port_rcpu.last = now;
    g_port_rcpu.atid = want ? g_port_rcpu.tid : 0;
#endif
}

/* Once per game frame, on the frame-loop thread. target_fps <= 0: uncapped.
 * discard = 1 after a pause / long wait that is not a hitch. */
void port_hitch_frame(double target_fps, int discard);

#ifdef __cplusplus
}

/* rsx::thread CPU category scope (PORT_RSX_CPU_PROF) */
struct port_rcpu_scope
{
    int prev;
    explicit port_rcpu_scope(int cat) : prev(port_rcpu_enter(cat)) {}
    ~port_rcpu_scope() { port_rcpu_leave(prev); }
    port_rcpu_scope(const port_rcpu_scope&) = delete;
    port_rcpu_scope& operator=(const port_rcpu_scope&) = delete;
};

/* PORT_RSX_CPU_DETAIL scope (no-op unless detail is on) */
struct port_rcpu_dscope
{
    int prev;
    explicit port_rcpu_dscope(int cat) : prev(port_rcpu_enter_d(cat)) {}
    ~port_rcpu_dscope() { port_rcpu_leave(prev); }
    port_rcpu_dscope(const port_rcpu_dscope&) = delete;
    port_rcpu_dscope& operator=(const port_rcpu_dscope&) = delete;
};

/* port_rcpu_enter_d2 scope: decode or state sub-bucket by the current parent */
struct port_rcpu_dscope2
{
    int prev;
    port_rcpu_dscope2(int d_cat, int s_cat) : prev(port_rcpu_enter_d2(d_cat, s_cat)) {}
    ~port_rcpu_dscope2() { port_rcpu_leave(prev); }
    port_rcpu_dscope2(const port_rcpu_dscope2&) = delete;
    port_rcpu_dscope2& operator=(const port_rcpu_dscope2&) = delete;
};
#endif

/* ------------------------------------------------------------------------- */
/* Asynchronous log lines for hot threads (render / RSX / frame loop). A printf
 * + fflush there blocks on the CRT stream lock and the file write: the [pipe]
 * line every 5 s on the render thread cost a ~21 ms present (one [hitch] per 5 s
 * in every run). port_log_async formats into a buffer under a short lock; a
 * writer thread flushes it every 100 ms. PORT_LOG_SYNC=1: write at once. */
#ifdef __cplusplus
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>
struct port_alog_queue
{
    std::mutex mtx;
    std::condition_variable cv;
    std::string out, err;
    bool started = false;
    int sync = -1;
};
inline port_alog_queue& port_alog_q()
{
    static port_alog_queue* q = new port_alog_queue();   /* never destroyed: the writer outlives static teardown */
    return *q;
}
inline void port_alog_drain()
{
    auto& q = port_alog_q();
    std::string o, e;
    { std::lock_guard<std::mutex> lk(q.mtx); o.swap(q.out); e.swap(q.err); }
    if (!o.empty()) { fwrite(o.data(), 1, o.size(), stdout); fflush(stdout); }
    if (!e.empty()) { fwrite(e.data(), 1, e.size(), stderr); fflush(stderr); }
}
inline void port_log_async(int to_stderr, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    auto& q = port_alog_q();
    if (q.sync < 0) { const char* s = getenv("PORT_LOG_SYNC"); q.sync = (s && *s && *s != '0') ? 1 : 0; }
    if (q.sync) { FILE* f = to_stderr ? stderr : stdout; fputs(buf, f); fflush(f); return; }
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(q.mtx);
        (to_stderr ? q.err : q.out) += buf;
        if (!q.started) { q.started = true; start = true; }
    }
    if (start) {
        atexit([] { port_alog_drain(); });
        std::thread([] {
            auto& q = port_alog_q();
            for (;;) {
                { std::unique_lock<std::mutex> lk(q.mtx);
                  q.cv.wait_for(lk, std::chrono::milliseconds(100)); }
                port_alog_drain();
            }
        }).detach();
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* Present-to-present intervals: what the display is actually handed, as opposed
 * to the game's CPU frame time above. Called by the RSX display engine
 * (RSXDisplayEngine.cpp, PORT_HITCH_PRESENT_IMPL) right after each successful
 * present_frame -- with FIFO the present / acquire blocks on the swap chain, so
 * this follows scan-out closely; an even 60 fps reads p50 16.7 / p99 ~17. Every
 * 5 s on that thread:
 *   [hitch] present 5 s: N presents, interval p50/p99/max, sd, uneven M
 *   (|interval - median| > 4 ms), and at a fixed PORT_DISPLAY_HZ refresh how many
 *   frames would be held 1 / 2 / 3 / 4+ refreshes ("if no VRR": with G-Sync the
 *   display follows the interval instead). A mix of 2 and 3 = judder.
 * Per frame, the longest interval goes into the [hitch] line as present-iv. */
#if defined(__cplusplus) && defined(PORT_HITCH_PRESENT_IMPL)
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <vector>
static inline void port_hitch_present(void)
{
    static uint64_t last = 0, t_sum = 0;
    static std::vector<float> iv;
    if (g_port_hitch.off) return;
    const uint64_t now = __rdtsc();
    const double k = g_port_hitch.tsc_per_ns;
    if (!last || k <= 0.0) { last = now; t_sum = now; return; }
    const int64_t d = (int64_t)(now - last);
    last = now;
    port_hitch_max_ticks(PH_PRESENT_IV, d);
    const double ms = (double)d / (k * 1e6);
    if (ms < 1000.0) iv.push_back((float)ms);   /* a load / pause gap is not pacing */
    if ((double)(now - t_sum) < k * 5e9) return;
    t_sum = now;
    if (iv.size() < 8) { iv.clear(); return; }
    std::vector<float> s(iv);
    std::sort(s.begin(), s.end());
    const size_t n = s.size();
    const float p50 = s[n / 2], p99 = s[(std::min)(n - 1, (n * 99) / 100)], mx = s[n - 1];
    double mean = 0.0, sd = 0.0;
    for (float v : s) mean += v;
    mean /= (double)n;
    unsigned uneven = 0;
    for (float v : iv) { sd += (v - mean) * (v - mean); if (fabs(v - p50) > 4.0) uneven++; }
    double hz = 0.0;
    if (const char* h = getenv("PORT_DISPLAY_HZ")) hz = atof(h);
    char held[112] = "";
    if (hz >= 20.0) {
        unsigned r[4] = { 0, 0, 0, 0 };
        const double per = 1000.0 / hz;
        for (float v : iv) { long c = lround(v / per); c = c < 1 ? 1 : c > 4 ? 4 : c; r[c - 1]++; }
        snprintf(held, sizeof held, " | at %.0f Hz if no VRR, refreshes held 1/2/3/4+: %u/%u/%u/%u", hz, r[0], r[1], r[2], r[3]);
    }
    port_log_async(1, "[hitch] present 5 s: %zu presents (%.1f/s), interval p50 %.1f p99 %.1f max %.1f ms (sd %.2f), uneven %u%s\n",
            n, 1000.0 / mean, p50, p99, mx, sqrt(sd / (double)n), uneven, held);
    iv.clear();
}
#endif

/* ------------------------------------------------------------------------- */
#ifdef PORT_HITCH_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include "../../runtime/syscalls/ps3_waitstat.h"

/* PORT_HITCH_RSX_METH: the decode self time of the flip interval by method register (top 8) and housekeeping pass.
 * dec = the line's decode ms (RC_FIFO minus fifo-idle and label); label = the semaphore-acquire wait, which runs inside
 * the NV406E acquire handler (0x0068) and so is in that method's time; f = ms per TSC tick. */
static void port_hitch_rsx_meth_line(double dec, double label, double f)
{
    enum { TOP = 8 };
    uint32_t top[TOP]; int nt = 0;
    double sum = 0.0;
    for (uint32_t j = 0; j < g_port_rmeth.nt; j++) {
        const uint32_t key = g_port_rmeth.touched[j];
        if (key == PORT_RMETH_HK_IDLE) continue;
        sum += (double)g_port_rmeth.t[key] * f;
        if (key >= 0x4000) continue;
        int p = nt < TOP ? nt++ : TOP;
        if (p == TOP) { if (g_port_rmeth.t[key] <= g_port_rmeth.t[top[TOP - 1]]) continue; p = TOP - 1; }
        while (p > 0 && g_port_rmeth.t[top[p - 1]] < g_port_rmeth.t[key]) { top[p] = top[p - 1]; p--; }
        top[p] = key;
    }
    char ms[512]; int o = 0;
    ms[0] = 0;
    for (int i = 0; i < nt && o < (int)sizeof ms - 48; i++)
        o += snprintf(ms + o, sizeof ms - o, " 0x%04x %.2f/%u", top[i] << 2, (double)g_port_rmeth.t[top[i]] * f, g_port_rmeth.n[top[i]]);
    port_log_async(1, "[hitch] rsx decode %.1f ms by method (offset ms/calls, self time):%s | housekeeping light %.2f/%u full %.2f/%u (fifo idle %.2f/%u)"
                      " | outside handlers (FIFO fetch, flow control, loop) %.1f (0x0068 includes the label wait)\n",
                   dec, ms, (double)g_port_rmeth.t[PORT_RMETH_HK_LIGHT] * f, g_port_rmeth.n[PORT_RMETH_HK_LIGHT],
                   (double)g_port_rmeth.t[PORT_RMETH_HK_FULL] * f, g_port_rmeth.n[PORT_RMETH_HK_FULL],
                   (double)g_port_rmeth.t[PORT_RMETH_HK_IDLE] * f, g_port_rmeth.n[PORT_RMETH_HK_IDLE], (std::max)(0.0, dec + label - sum));
    const port_rnv39_state* v = &g_port_rnv39;
    if (v->n)
        port_log_async(1, "[hitch] rsx nv0039 (0x2328 wall ms): %u copies %.2f MB (dst local mem %u, overlapping %u) | barriers %.2f"
                          " | preinval %.2f (%u pages) | lock + mm_flush %.2f | copy %.2f (max %.3f; %u copies faulted, %u faults,"
                          " %.2f in the fault hook) | unlock + write barrier %.2f\n",
                       v->n, (double)v->bytes / 1048576.0, v->n_local, v->n_overlap, (double)v->t[PORT_NV39_PRE] * f,
                       (double)v->t[PORT_NV39_INVAL] * f, v->inval_pages, (double)v->t[PORT_NV39_MM] * f,
                       (double)v->t[PORT_NV39_COPY] * f, (double)v->copy_max * f, v->n_faulted, v->faults, (double)v->fault_t * f,
                       (double)v->t[PORT_NV39_WBAR] * f);
}

static void port_hitch_rsx_meth_reset(void)
{
    for (uint32_t j = 0; j < g_port_rmeth.nt; j++) {
        const uint32_t key = g_port_rmeth.touched[j];
        g_port_rmeth.t[key] = 0;
        g_port_rmeth.n[key] = 0;
    }
    g_port_rmeth.nt = 0;
    memset(&g_port_rnv39, 0, sizeof g_port_rnv39);
}

/* PORT_HITCH_RSX_SPLIT: rsx::thread, at every flip (port_rcpu_flip). sampled = the rsx-cpu split covers the interval. */
static void port_hitch_rsx_split(int sampled)
{
    static uint64_t t_prev = 0, win_t0 = 0;
    static int64_t rc_prev[RC_COUNT], ph_prev[PH_COUNT];
    static long cn_prev[PH_COUNT];
    static ULONG64 cyc_prev = 0;
    static unsigned lines = 0, skipped = 0, cap = 10;
    const uint64_t now = __rdtsc();
    const double k = g_port_hitch.tsc_per_ns;
    ULONG64 cyc = 0;
    const int have_cyc = QueryThreadCycleTime(GetCurrentThread(), &cyc) != 0;
    int64_t rc[RC_COUNT], ph[PH_COUNT];
    long cn[PH_COUNT];
    for (int i = 0; i < RC_COUNT; i++) rc[i] = g_port_rcpu.acc[i];
    for (int i = 0; i < PH_COUNT; i++) { ph[i] = g_port_hsplit.acc[i]; cn[i] = g_port_hsplit.cnt[i]; }
    if (!t_prev) {
        if (const char* m = getenv("PORT_HITCH_LINES")) { const int v = atoi(m); cap = v < 0 ? 0u : v > 1000 ? 1000u : (unsigned)v; }
        port_log_async(1, "[hitch] rsx flip split on (PORT_HITCH_RSX_SPLIT): rsx::thread flip intervals over %.0f ms are listed with "
                          "their rsx-cpu split (every frame sampled: PORT_RSX_CPU_PROF=%u), thread cpu vs wall and the timed waits\n",
                       g_port_hsplit.thr_ms, g_port_rcpu.every);
        win_t0 = now;
    } else if (k > 0.0) {
        if ((double)(now - win_t0) >= k * 5e9) {
            if (skipped) port_log_async(1, "[hitch] rsx flip: %u more long flips in the last 5 s not listed\n", skipped);
            lines = skipped = 0;
            win_t0 = now;
        }
        const double f = 1.0 / (k * 1e6);
        const double iv = (double)(now - t_prev) * f;
        if (iv > g_port_hsplit.thr_ms && iv < 2000.0) {
            if (lines >= cap) skipped++;
            else {
                lines++;
                double ms[PH_COUNT]; long c[PH_COUNT];
                for (int i = 0; i < PH_COUNT; i++) { ms[i] = (double)(ph[i] - ph_prev[i]) * f; c[i] = cn[i] - cn_prev[i]; }
                const double cpu = have_cyc && cyc_prev ? (double)(cyc - cyc_prev) * f : -1.0;
                const double waits = ms[PH_FIFOIDLE] + ms[PH_LABEL] + ms[PH_ZCULL] + ms[PH_HARDSYNC] + ms[PH_GPUWAIT] + ms[PH_ACQUIRE] + ms[PH_RSXLIMIT];
                const double off = cpu >= 0.0 ? (std::max)(0.0, iv - cpu) : -1.0;
                char rs[512] = " | rsx-cpu: not sampled (PORT_RSX_CPU_PROF > 1)";
                double dec = -1.0, dec_label = 0.0;
                if (sampled) {
                    double r[RC_COUNT] = {};
                    for (int i = 0; i < RC_COUNT; i++) r[port_rcpu_parent(i)] += (double)(rc[i] - rc_prev[i]) * f;
                    const double idle = (std::min)(r[RC_FIFO], ms[PH_FIFOIDLE]);
                    const double label = (std::min)(r[RC_FIFO] - idle, ms[PH_LABEL]);
                    dec = r[RC_FIFO] - idle - label;
                    dec_label = label;
                    snprintf(rs, sizeof rs, " | rsx-cpu: decode %.1f state %.1f shader %.1f vertex %.1f tex %.1f surface %.1f draw %.1f"
                                            " zcull %.1f submit %.1f readback %.1f present %.1f blit %.1f (+ fifo-idle %.1f label %.1f)",
                             r[RC_FIFO] - idle - label, r[RC_STATE], r[RC_SHADER], r[RC_VERTEX], r[RC_TEXTURE], r[RC_SURFACE], r[RC_DRAW],
                             r[RC_ZCULL], r[RC_SUBMIT], r[RC_READBACK], r[RC_PRESENT], r[RC_BLIT], idle, label);
                }
                port_log_async(1, "[hitch] rsx flip %.1f ms: thread cpu %.1f off-cpu %.1f (outside the timed waits >= %.1f)%s"
                                  " | waits fifo-idle %.1f label %.1f/%ld zcull %.1f/%ld hard-sync %.1f/%ld gpu-wait %.1f acquire %.1f rsx-limit %.1f"
                                  " | present %.1f dlss %.1f submits %ld tex %.1f/%ld pipe %.1f/%ld | guest threads on rsx: ring %.1f/%ld av-flush %.1f/%ld\n",
                               iv, cpu, off, off >= 0.0 ? (std::max)(0.0, off - waits) : -1.0, rs,
                               ms[PH_FIFOIDLE], ms[PH_LABEL], c[PH_LABEL], ms[PH_ZCULL], c[PH_ZCULL], ms[PH_HARDSYNC], c[PH_HARDSYNC],
                               ms[PH_GPUWAIT], ms[PH_ACQUIRE], ms[PH_RSXLIMIT], ms[PH_PRESENT], ms[PH_DLSS], c[PH_SUBMIT],
                               ms[PH_TEX], c[PH_TEX], ms[PH_PIPE], c[PH_PIPE], ms[PH_RING], c[PH_RING], ms[PH_AVFLUSH], c[PH_AVFLUSH]);
                if (dec >= 0.0 && g_port_rmeth.on) port_hitch_rsx_meth_line(dec, dec_label, f);
            }
        }
    }
    port_hitch_rsx_meth_reset();
    t_prev = now;
    cyc_prev = have_cyc ? cyc : 0;
    for (int i = 0; i < RC_COUNT; i++) rc_prev[i] = rc[i];
    for (int i = 0; i < PH_COUNT; i++) { ph_prev[i] = ph[i]; cn_prev[i] = cn[i]; }
}

extern "C" void port_hitch_frame(double target_fps, int discard)
{
    static int init = 0, on = 1;
    static double thr_ms = 25.0;
    static LARGE_INTEGER qf, q0;
    static uint64_t tsc0 = 0, last = 0, sum_t0 = 0;
    static std::vector<float> win;
    static unsigned n_over = 0, n_2x = 0, lines_s = 0, dropped = 0, n_disc = 0;
    static unsigned lines_cap = 10;     /* PORT_HITCH_LINES: per-frame lines per 5 s window */
    static int adapt = 1;               /* PORT_HITCH_ADAPT=0: fixed threshold */
    static double adapt_p50 = 0.0;      /* previous window's p50 */
    static ps3_waitstat* ws = NULL;
    static unsigned long long ws_prev4[4] = { 0, 0, 0, 0 };
    static ULONG64 cyc_prev = 0;
    static double sm_game = 0, sm_gw = 0, sm_cpu = 0, sm_fifo = 0, sm_label = 0, sm_zcull = 0, sm_hard = 0, sm_rlim = 0, sm_sub = 0, sm_lim = 0;
    static double sm_mrsx = 0, sm_ring = 0, sm_av = 0;
    static double prev_ft = 0.0;
    if (!init) {
        init = 1;
        const char* e = getenv("PORT_HITCH_LOG");
        on = e && *e && *e != '0';
        g_port_hitch.off = !on;
        if (const char* m = getenv("PORT_HITCH_MS")) { const double v = atof(m); if (v >= 5.0) thr_ms = v; }
        if (const char* m = getenv("PORT_HITCH_LINES")) { const int v = atoi(m); lines_cap = v < 0 ? 0u : v > 1000 ? 1000u : (unsigned)v; }
        { const char* a = getenv("PORT_HITCH_ADAPT"); adapt = !(a && *a == '0'); }
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        tsc0 = __rdtsc();
        last = tsc0; sum_t0 = tsc0;
        g_port_hitch.main_tid = GetCurrentThreadId();
        win.reserve(1024);
        if (on) fprintf(stderr, "[hitch] logger on: frames over %.0f ms (or 2x the %.1f fps target, or 1.5x the last 5 s p50) are listed "
                                "with a breakdown, at most %u per 5 s; 5 s summary p50/p99/max (PORT_HITCH_LOG=1 on, PORT_HITCH_MS=<ms>, "
                                "PORT_HITCH_LINES=<n>, PORT_HITCH_ADAPT=0)\n", thr_ms, target_fps, lines_cap);
        ws = ps3_waitstat_self();
        g_port_hsplit.fn = on ? port_hitch_rsx_split : NULL;
    }
    if (!on) return;
    const uint64_t now = __rdtsc();
    {   /* TSC rate against QPC over the whole run (stable after the first second) */
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        const double ns = (double)(q.QuadPart - q0.QuadPart) * 1e9 / (double)qf.QuadPart;
        if (ns > 2e8) g_port_hitch.tsc_per_ns = (double)(now - tsc0) / ns;
    }
    const double k = g_port_hitch.tsc_per_ns;
    double ms[PH_COUNT], mxms[PH_COUNT]; long cnt[PH_COUNT];
    for (int i = 0; i < PH_COUNT; i++) {
        const int64_t a = _InterlockedExchange64((volatile long long*)&g_port_hitch.acc[i], 0);
        const int64_t m = _InterlockedExchange64((volatile long long*)&g_port_hitch.mx[i], 0);
        cnt[i] = _InterlockedExchange(&g_port_hitch.cnt[i], 0);
        ms[i] = k > 0.0 ? (double)a / (k * 1e6) : 0.0;
        mxms[i] = k > 0.0 ? (double)m / (k * 1e6) : 0.0;
    }
    /* the main thread's own guest waits (usleep polls, lwmutex / lwcond) from the [thrcpu] accounting:
     * spin = usleep < 1 ms (poll loops), sleep = usleep >= 1 ms */
    double ws_ms = 0.0, ws_spin = 0.0, ws_sleep = 0.0, ws_lwm = 0.0, ws_lwc = 0.0;
    if (ws) {
        const double f = 1000.0 / (double)qf.QuadPart;
        const unsigned long long s0 = ws->short_qpc, s1 = ws->long_qpc, s2 = ws->lwm_block_qpc, s3 = ws->lwc_wait_qpc;
        ws_spin = (double)(s0 - ws_prev4[0]) * f; ws_sleep = (double)(s1 - ws_prev4[1]) * f;
        ws_lwm = (double)(s2 - ws_prev4[2]) * f;  ws_lwc = (double)(s3 - ws_prev4[3]) * f;
        ws_prev4[0] = s0; ws_prev4[1] = s1; ws_prev4[2] = s2; ws_prev4[3] = s3;
        ws_ms = ws_spin + ws_sleep + ws_lwm + ws_lwc;
    }
    /* the guest call site the frame loop waited at longest */
    uint32_t top_lr = 0; double top_ms = 0.0;
    for (int i = 0; i < PORT_HITCH_LR_SLOTS; i++) {
        const double v = (double)g_port_hitch.lr_qpc[i] * 1000.0 / (double)qf.QuadPart;
        if (v > top_ms) { top_ms = v; top_lr = g_port_hitch.lr[i]; }
        g_port_hitch.lr[i] = 0; g_port_hitch.lr_qpc[i] = 0;
    }
    /* CPU time the frame-loop thread actually ran: "game" minus this = preempted / not scheduled */
    double cpu_ms = -1.0;
    {
        ULONG64 cyc = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &cyc) && k > 0.0) {
            if (cyc_prev) cpu_ms = (double)(cyc - cyc_prev) / (k * 1e6);
            cyc_prev = cyc;
        }
    }
    const double ft = k > 0.0 ? (double)(now - last) / (k * 1e6) : 0.0;
    last = now;
    /* rsx::thread CPU split: per-frame deltas, so a discarded frame's interval (pause, alt-tab) is dropped and not
     * counted as decode work in the 5 s summary */
    static int64_t rc_last[RC_COUNT] = {};
    static int64_t rc_win[RC_COUNT] = {};
    for (int i = 0; i < RC_COUNT; i++) {
        const int64_t a = g_port_rcpu.acc[i];
        if (!(k <= 0.0 || discard)) rc_win[i] += a - rc_last[i];
        rc_last[i] = a;
    }
    static uint32_t rc_fl_last = 0, rc_fs_last = 0;
    static uint64_t rc_fl_win = 0, rc_fs_win = 0;   /* flips and sampled flips in the window (PORT_RSX_CPU_PROF=N) */
    {
        const uint32_t fl = g_port_rcpu.flips, fs = g_port_rcpu.flips_sampled;
        if (!(k <= 0.0 || discard)) { rc_fl_win += (uint32_t)(fl - rc_fl_last); rc_fs_win += (uint32_t)(fs - rc_fs_last); }
        rc_fl_last = fl; rc_fs_last = fs;
    }
    if (k <= 0.0 || discard) { n_disc += discard ? 1 : 0; return; }

    win.push_back((float)ft);
    const double waits = ms[PH_LIMIT] + ms[PH_TICK] + ms[PH_SYNC] + ws_ms + ms[PH_PAD] + ms[PH_MAINRSX];
    const double game = (std::max)(0.0, ft - waits);
    {   /* 5 s means, so the steady state (not only the hitches) shows where the frame goes */
        sm_game += game; sm_gw += ws_ms; sm_cpu += cpu_ms > 0.0 ? cpu_ms : 0.0;
        sm_fifo += ms[PH_FIFOIDLE]; sm_label += ms[PH_LABEL]; sm_zcull += ms[PH_ZCULL]; sm_hard += ms[PH_HARDSYNC];
        sm_rlim += ms[PH_RSXLIMIT]; sm_sub += cnt[PH_SUBMIT]; sm_lim += ms[PH_LIMIT];
        sm_mrsx += ms[PH_MAINRSX]; sm_ring += ms[PH_RING]; sm_av += ms[PH_AVFLUSH];
    }
    const double period = target_fps > 0.0 ? 1000.0 / target_fps : 0.0;
    /* adaptive: a game steadily over the fixed threshold (30-40 fps under 25 ms) lists only frames well above its
     * own p50, not every frame; the list is capped per 5 s window (the summary counts the rest) */
    const double thr = (std::max)((std::max)(thr_ms, period * 1.5), adapt ? adapt_p50 * 1.5 : 0.0);
    const int over2x = period > 0.0 && ft > 2.0 * period;
    if (ft > thr || over2x) {
        n_over += ft > thr; n_2x += over2x;
        if (!(ft > thr) && adapt) { /* over 2x a target the game does not reach: counted, not listed */ }
        else if (lines_s++ < lines_cap) {
            char cpu[32] = "";
            if (cpu_ms >= 0.0) snprintf(cpu, sizeof cpu, " (cpu %.1f)", cpu_ms);
            port_log_async(1, "[hitch] %.1f ms%s (prev %.1f) | game %.1f%s limiter %.1f extras %.1f sync %.1f guestwait %.1f"
                            " (spin %.1f sleep %.1f lwm %.1f lwc %.1f, top 0x%08X %.1f) pad %.2f on-rsx %.1f"
                            " | rsx-iv %.1f present %.1f (acquire %.1f gpu-wait %.1f dlss %.1f) fifo-idle %.1f label %.1f/%ld zcull %.1f/%ld"
                            " hard-sync %.1f/%ld submits %ld rsx-limit %.1f pipe %.1f/%ld tex %.1f/%ld (max %.1f)"
                            " | guest threads on rsx: ring %.1f/%ld av-flush %.1f/%ld | audio-wait %.1f | present-iv %.1f/%ld\n",
                    ft, over2x ? " >2x" : "", prev_ft, game, cpu, ms[PH_LIMIT], ms[PH_TICK], ms[PH_SYNC], ws_ms,
                    ws_spin, ws_sleep, ws_lwm, ws_lwc, top_lr, top_ms, ms[PH_PAD], ms[PH_MAINRSX],
                    mxms[PH_RSX], ms[PH_PRESENT], ms[PH_ACQUIRE], ms[PH_GPUWAIT], ms[PH_DLSS], ms[PH_FIFOIDLE],
                    ms[PH_LABEL], cnt[PH_LABEL], ms[PH_ZCULL], cnt[PH_ZCULL], ms[PH_HARDSYNC], cnt[PH_HARDSYNC], cnt[PH_SUBMIT],
                    ms[PH_RSXLIMIT], ms[PH_PIPE], cnt[PH_PIPE],
                    ms[PH_TEX], cnt[PH_TEX], mxms[PH_TEX], ms[PH_RING], cnt[PH_RING], ms[PH_AVFLUSH], cnt[PH_AVFLUSH],
                    ms[PH_AUDIO], mxms[PH_PRESENT_IV], cnt[PH_PRESENT_IV]);
        } else dropped++;
    }
    prev_ft = ft;
    if ((double)(now - sum_t0) >= k * 5e9 && !win.empty()) {
        std::vector<float> s(win);
        std::sort(s.begin(), s.end());
        const size_t n = s.size();
        const float p50 = s[n / 2], p99 = s[(std::min)(n - 1, (n * 99) / 100)], mx = s[n - 1];
        adapt_p50 = p50;
        double sd = 0.0, mean = 0.0;
        for (float v : s) mean += v;
        mean /= (double)n;
        for (float v : win) sd += (v - mean) * (v - mean);
        const double dn = (double)n;
        char extra[64] = "";
        if (dropped) snprintf(extra, sizeof extra, " (%u lines not shown)", dropped);
        /* rsx::thread CPU split (PORT_RSX_CPU_PROF): decode = RC_FIFO minus the FIFO-empty and label waits it holds */
        char rcpu_s[384] = "";
        char rcpu_d[640] = "";
        if (g_port_rcpu.tid && rc_fs_win) {
            const double rc_scale = (double)rc_fl_win / (double)rc_fs_win;   /* 1 when every frame is sampled */
            double r[RC_COUNT], sub[RC_COUNT], tot = 0.0;
            for (int i = 0; i < RC_COUNT; i++) {
                sub[i] = (double)rc_win[i] / (k * 1e6) / dn * rc_scale;
                r[i] = 0.0;
                tot += sub[i];
            }
            for (int i = 0; i < RC_COUNT; i++) r[port_rcpu_parent(i)] += sub[i];   /* detail folded into its parent */
            const double idle = (std::min)(r[RC_FIFO], sm_fifo / dn);
            const double label = (std::min)(r[RC_FIFO] - idle, sm_label / dn);
            const double decode = r[RC_FIFO] - idle - label;
            const double busy = tot - idle - label - r[RC_READBACK] - r[RC_PRESENT];
            snprintf(rcpu_s, sizeof rcpu_s,
                     " | rsx-cpu per frame: work %.1f = decode %.2f state %.2f shader %.2f vertex %.2f tex %.2f surface %.2f"
                     " draw %.2f zcull %.2f submit %.2f blit %.2f; present %.2f readback %.2f label %.2f idle %.2f",
                     busy, decode, r[RC_STATE], r[RC_SHADER], r[RC_VERTEX], r[RC_TEXTURE],
                     r[RC_SURFACE], r[RC_DRAW], r[RC_ZCULL], r[RC_SUBMIT], r[RC_BLIT], r[RC_PRESENT], r[RC_READBACK], label, idle);
            if (g_port_rcpu.detail) {
                /* per-draw / per-method costs: draws and methods are this window's deltas */
                static uint64_t d_last = 0;
                static uint32_t m_last = 0;
                const uint64_t dr = g_port_rcpu.draws;
                const uint32_t mt = g_port_rcpu.methods;
                const double draws = (double)(dr - d_last) / dn, meths = (double)(uint32_t)(mt - m_last) / dn;
                d_last = dr; m_last = mt;
                const double dec_other = (std::max)(0.0, sub[RC_FIFO] - idle - label);
                snprintf(rcpu_d, sizeof rcpu_d,
                         "[hitch] rsx-cpu detail per frame (PORT_RSX_CPU_DETAIL=1; adds ~2 rdtsc per scope): draws %.0f methods %.0f"
                         " | state %.2f (%.2f us/draw) = decode-state %.2f aa %.2f env %.2f barrier %.2f query %.2f bind %.2f rpass %.2f scissor %.2f xform-replay %.2f other %.2f"
                         " | decode %.2f = xform %.2f clear %.2f post-draw %.2f fifo+handlers %.2f (%.1f ns/method)\n",
                         draws, meths, r[RC_STATE], draws > 0.0 ? r[RC_STATE] * 1000.0 / draws : 0.0,
                         sub[RC_S_DECODE], sub[RC_S_AA], sub[RC_S_ENV], sub[RC_S_BARRIER], sub[RC_S_QUERY], sub[RC_S_BIND],
                         sub[RC_S_RPASS], sub[RC_S_SCISSOR], sub[RC_S_XFORM], sub[RC_STATE],
                         decode, sub[RC_D_XFORM], sub[RC_D_CLEAR], sub[RC_D_POST], dec_other,
                         meths > 0.0 ? dec_other * 1e6 / meths : 0.0);
            }
        }
        port_log_async(1, "[hitch] 5 s: %zu frames, frame time p50 %.1f p99 %.1f max %.1f ms (sd %.2f) | over %.0f ms: %u, over 2x target: %u%s"
                          " | per frame: game %.1f (cpu %.1f) guestwait %.1f on-rsx %.2f limiter %.1f | rsx fifo-idle %.1f label %.1f zcull %.2f hard-sync %.2f"
                          " rsx-limit %.1f submits %.1f | guest threads on rsx: ring %.2f av-flush %.2f%s\n",
                n, p50, p99, mx, sqrt(sd / dn), thr, n_over, n_2x, extra,
                sm_game / dn, sm_cpu / dn, sm_gw / dn, sm_mrsx / dn, sm_lim / dn, sm_fifo / dn, sm_label / dn, sm_zcull / dn, sm_hard / dn,
                sm_rlim / dn, sm_sub / dn, sm_ring / dn, sm_av / dn, rcpu_s);
        if (rcpu_d[0]) port_log_async(1, "%s", rcpu_d);
        sm_game = sm_gw = sm_cpu = sm_fifo = sm_label = sm_zcull = sm_hard = sm_rlim = sm_sub = sm_lim = 0.0;
        sm_mrsx = sm_ring = sm_av = 0.0;
        for (int i = 0; i < RC_COUNT; i++) rc_win[i] = 0;
        rc_fl_win = rc_fs_win = 0;
        win.clear(); n_over = n_2x = dropped = n_disc = lines_s = 0;
        sum_t0 = now;
    }
}
#endif /* PORT_HITCH_IMPLEMENTATION */

#endif /* PORT_HITCH_H */
