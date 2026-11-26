/*
 * R3_JOBWATCH -- who corrupts Resistance 3 job 403's object state?
 *
 * Job 403 (igjob_00D7A480) works through a queue of items for one object per
 * pass. F488 GETs the object (0xA0 bytes, EA = queue header word 3) into LS
 * 0x14C60 and dispatches on the halfword state at object+0x18 (LS 0x14C78):
 * handler = LS[0x10900 + 16*state], then callback = LS[0x14C2C + 4*state].
 * At the end of the pass F1B8 sets state |= 0x8000 and PUTs object+0x10..+0x1F
 * back. A pass that starts on a state that still has 0x8000 set reads its
 * callback from LS 0x34C2C + 4x -- the job's buffers -- and calls garbage: the
 * tunnels-ladder freeze (LS 0x32800), Haven #42 (0x8D) and St. Louis (0x27D7)
 * are all that call (bisl at 0xF8F0, lr 0xF8F4, r3 0x14C60). The handler table
 * wraps in the 256 KB LS (16*0x8000 = 0x80000), so the pass runs normally up
 * to that call.
 *
 * So the question is who leaves 0x800x in memory when an SPU claims new work:
 * a second SPU still processing the same object (stale PUT over the PPU's new
 * state), the PPU writing the line out of order, or a writer our coherence
 * does not see at all (stvx, HLE memcpy). This records, per object EA:
 *   - every job 403 object GET (state seen), item GET, state PUT (state
 *     written vs. memory at that moment), and its GETLLAR/PUTLLC on the queue
 *     header line;
 *   - every store to those lines that goes through the coherent path, with the
 *     writing PPU thread and guest lr (the lines are marked in the SPU
 *     coherence bitmap so the lifted inline stores take that path);
 *   - optionally a poller that timestamps every change of each state halfword,
 *     which also sees writers that bypass the store hooks.
 * Anomalies (BAD-STATE at GET, two SPUs on one object, the state changing
 * under a pass before its PUT, the bad call itself) are printed with the
 * object's recent history.
 *
 * R3_JOBWATCH=1        on (anomalies + their history); 2 = also every event
 * R3_JOBWATCH_POLL=0   no state poller thread (default on; it spins one core)
 * R3_JOBWATCH_MARK=0   do not mark the object lines (PPU stores then stay on
 *                      the inline path and are seen by the poller only)
 * R3_JOBWATCH_STRESS=n n per mille of passes sleep 0-2 ms after the object GET
 *                      and before the state PUT, to widen any race window
 * Output: logs\jobwatch.txt (flushed every 20 ms, and at each anomaly),
 * [jobwatch] lines on stderr for the first anomalies, and the shared memory
 * block Local\R3JobWatch_<pid> (tools\r3jobwatch.py reads it live).
 */
#include "spu_coherency.h"
#include "ps3emu/log_first.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

volatile int g_spu_jw_on = -1;   /* -1 = not read yet */

#if defined(_WIN32)

extern uint8_t* vm_base;

/* ppu_loader.cpp, when linked: the PPU thread running on this host thread. */
int ppu_jw_who(uint32_t* lr, uint32_t* tid);
int ppu_jw_who_absent(uint32_t* lr, uint32_t* tid) { (void)lr; (void)tid; return 0; }
#pragma comment(linker, "/alternatename:ppu_jw_who=ppu_jw_who_absent")

enum {
    JW_GET = 1, JW_ITEM, JW_PUT, JW_LLAR, JW_PUTLLC, JW_WRITE, JW_POLL, JW_STRESS,
    JW_ANOM = 16, JW_BADSTATE = 16, JW_CONCUR, JW_MIDPASS, JW_BADCALL
};
enum {
    C_GET, C_ITEM, C_PUT, C_LLAR, C_LLC_OK, C_LLC_FAIL, C_WRITE, C_POLL,
    C_BADSTATE, C_CONCUR, C_MIDPASS, C_BADCALL, C_LOST, C_STRESS, C_N = 24
};

typedef struct {
    volatile uint32_t seq;      /* index + 1 once the record is complete */
    uint16_t kind, spu;         /* spu: SPU id, 0xFFFF = PPU, 0xFFFE = other */
    uint32_t tid;               /* host thread id (PPU: guest thread id) */
    uint32_t ea;
    uint32_t v[6];
    uint64_t t_us;
} jw_ev;

#define JW_MAGIC  0x574A3352u   /* "R3JW" */
#define JW_RING   8192u
#define JW_OBJS   64
#define JW_LINES  256
#define JW_HDR    1024u
typedef struct {
    uint32_t magic, version, pid, ring_n;
    volatile LONG head;
    uint32_t ev_size;
    int64_t  qpc_freq, qpc_start;
    volatile LONG cnt[C_N];
    volatile LONG nobj;
    uint32_t obj_ea[JW_OBJS];
} jw_hdr;

typedef struct {
    volatile uint32_t ea;
    void* volatile active;      /* ctx between its GET and its PUT */
    volatile uint32_t active_tid;
    volatile uint64_t active_t;
    volatile uint32_t poll_last;
} jw_obj;

static jw_hdr* s_hdr;
static jw_ev*  s_ring;
static jw_obj  s_obj[JW_OBJS];
static volatile LONG s_nobj;
static volatile uint32_t s_lines[JW_LINES];
static volatile LONG s_nlines;
static volatile LONG s_tab_lock;
static int s_level, s_mark = 1, s_poll = 1, s_stress;
static FILE* s_out;
static volatile LONG s_out_lock;
static LARGE_INTEGER s_qf, s_q0;

/* Per-context pass state (an SPU context's object between its GET and PUT). */
typedef struct { void* volatile ctx; uint32_t obj, got, snap[4], item; uint64_t t; } jw_pass;
static jw_pass s_pass[64];
static jw_pass* pass_of(const void* ctx);
static __declspec(thread) uint32_t t_llc[4];
static __declspec(thread) uint32_t t_rng;

static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t ls32(const spu_context* c, uint32_t a) { return be32(c->ls + (a & SPU_LS_MASK & ~3u)); }
static inline uint32_t mem32(uint32_t ea) { return be32(vm_base + (ea & ~3u)); }
static inline uint32_t mem16(uint32_t ea) { return be16(vm_base + (ea & ~1u)); }
static uint64_t now_us(void)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (uint64_t)((q.QuadPart - s_q0.QuadPart) * 1000000.0 / (double)s_qf.QuadPart);
}
static void tab_lock(void)   { while (_InterlockedExchange(&s_tab_lock, 1)) _mm_pause(); }
static void tab_unlock(void) { _InterlockedExchange(&s_tab_lock, 0); }

static jw_pass* pass_of(const void* ctx)
{
    for (int i = 0; i < 64; i++) if (s_pass[i].ctx == ctx) return &s_pass[i];
    tab_lock();
    jw_pass* p = NULL;
    for (int i = 0; i < 64 && !p; i++) if (s_pass[i].ctx == ctx) p = &s_pass[i];
    for (int i = 0; i < 64 && !p; i++) if (!s_pass[i].ctx) { p = &s_pass[i]; memset(p, 0, sizeof *p); p->ctx = (void*)ctx; }
    if (!p) { static int rr; p = &s_pass[rr++ & 63]; memset(p, 0, sizeof *p); p->ctx = (void*)ctx; }
    tab_unlock();
    return p;
}

static int is_job403(const spu_context* c)
{
    return c && c->ls && ls32(c, 0xF8F0) == 0x35202D00u && ls32(c, 0xF1B8) == 0x40C0000Du &&
           ls32(c, 0x10280) == 0x4200000Bu;
}

static void push(int kind, uint16_t spu, uint32_t tid, uint32_t ea,
                 uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    const LONG i = _InterlockedIncrement(&s_hdr->head) - 1;
    jw_ev* ev = &s_ring[(uint32_t)i % JW_RING];
    ev->seq = 0;
    _ReadWriteBarrier();
    ev->kind = (uint16_t)kind; ev->spu = spu; ev->tid = tid; ev->ea = ea;
    ev->v[0] = a; ev->v[1] = b; ev->v[2] = c; ev->v[3] = d; ev->v[4] = e; ev->v[5] = f;
    ev->t_us = now_us();
    MemoryBarrier();
    ev->seq = (uint32_t)i + 1u;
}

static void add_line(uint32_t line)
{
    for (LONG i = 0; i < s_nlines; i++) if (s_lines[i] == line) return;
    tab_lock();
    LONG i = 0;
    for (; i < s_nlines; i++) if (s_lines[i] == line) break;
    if (i == s_nlines && s_nlines < JW_LINES) { s_lines[s_nlines] = line; MemoryBarrier(); s_nlines++; }
    tab_unlock();
    /* Route the PPU's inline stores to this line through the coherent path
     * (vm_st_fast checks the bitmap), where spu_jw_note_write sees them. */
    if (s_mark) { spu_lockline_lock(); spu_coh_reserve(NULL, line); spu_lockline_unlock(); }
}

static jw_obj* find_obj(uint32_t ea, int add)
{
    for (LONG i = 0; i < s_nobj; i++) if (s_obj[i].ea == ea) return &s_obj[i];
    if (!add) return NULL;
    jw_obj* o = NULL;
    tab_lock();
    for (LONG i = 0; i < s_nobj; i++) if (s_obj[i].ea == ea) { o = &s_obj[i]; break; }
    if (!o && s_nobj < JW_OBJS) {
        o = &s_obj[s_nobj];
        o->ea = ea; o->active = NULL; o->poll_last = mem16(ea + 0x18);
        s_hdr->obj_ea[s_nobj] = ea;
        MemoryBarrier();
        s_nobj++; s_hdr->nobj = s_nobj;
    }
    tab_unlock();
    if (o) { add_line(ea & ~127u); add_line((ea + 0x9Fu) & ~127u); }
    return o;
}

static int obj_of(uint32_t ea, uint32_t* obj)
{
    for (LONG i = 0; i < s_nobj; i++) {
        const uint32_t o = s_obj[i].ea;
        if (ea >= o && ea < o + 0xA0u) { *obj = o; return 1; }
    }
    return 0;
}

static void stress(spu_context* ctx, uint32_t obj, int where)
{
    if (!s_stress) return;
    if (!t_rng) t_rng = GetCurrentThreadId() * 2654435761u | 1u;
    t_rng = t_rng * 1103515245u + 12345u;
    if (((t_rng >> 8) % 1000u) < (uint32_t)s_stress) {
        const uint32_t ms = (t_rng >> 20) % 3u;
        _InterlockedIncrement(&s_hdr->cnt[C_STRESS]);
        push(JW_STRESS, (uint16_t)(ctx->spu_id & 0xFFu), GetCurrentThreadId(), obj, ms, (uint32_t)where, 0, 0, 0, 0);
        Sleep(ms);
    }
}

/* ---- output ------------------------------------------------------------- */

static const char* kname(int k)
{
    switch (k) {
    case JW_GET: return "GET"; case JW_ITEM: return "ITEM"; case JW_PUT: return "PUT";
    case JW_LLAR: return "LLAR"; case JW_PUTLLC: return "PUTLLC"; case JW_WRITE: return "WRITE";
    case JW_POLL: return "POLL"; case JW_STRESS: return "STRESS"; case JW_BADSTATE: return "!!BAD-STATE"; case JW_CONCUR: return "!!CONCURRENT";
    case JW_MIDPASS: return "!!MIDPASS"; case JW_BADCALL: return "!!BADCALL";
    }
    return "?";
}

static void fmt_ev(char* b, size_t n, const jw_ev* e)
{
    char who[32];
    if (e->spu == 0xFFFFu) snprintf(who, sizeof who, "PPU t%u", e->tid);
    else if (e->spu == 0xFFFEu) snprintf(who, sizeof who, "host %u", e->tid);
    else snprintf(who, sizeof who, "spu%u h%u", e->spu, e->tid);
    const unsigned long long t = (unsigned long long)e->t_us;
    const char* k = kname(e->kind);
    const uint32_t* v = e->v;
    switch (e->kind) {
    case JW_GET:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X state=%04X item=%08X +10: %08X %08X %08X %08X",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case JW_ITEM:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s ea=%08X lsa=%05X size=%u : %08X %08X %08X %08X",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case JW_PUT:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X writes=%04X mem-before=%04X got=%04X pass=%uus",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2], v[4]); break;
    case JW_LLAR:
    case JW_PUTLLC:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s line=%08X words: %08X %08X %08X %08X%s",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2], v[3],
                 e->kind == JW_LLAR ? "" : (v[4] == 0 ? "  ok" : v[4] == 1 ? "  FAILED" : "  (unconditional)")); break;
    case JW_WRITE:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s ea=%08X word=%08X lr=%08X%s obj=%08X state=%04X",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2] ? " (SPU DMA)" : "", v[4], v[3]); break;
    case JW_POLL:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X state %04X -> %04X",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1]); break;
    case JW_STRESS:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X slept %u ms %s",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1] ? "after the GET" : "before the PUT"); break;
    case JW_BADSTATE:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X state=%04X at GET (valid 0..11; 0x8000 = done bit of a finished pass)",
                 t / 1000, t % 1000, k, who, e->ea, v[0]); break;
    case JW_CONCUR:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X another SPU (h%u) has had it for %uus without its PUT",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1]); break;
    case JW_MIDPASS:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X state got=%04X mem-now=%04X pass-writes=%04X; changed since GET (xor) +10:%08X +14:%08X +18:%08X",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case JW_BADCALL:
        snprintf(b, n, "%10llu.%03llu %-13s %-11s obj=%08X call 0x%05X lr=0x%05X LS state=%04X got=%04X item=%08X",
                 t / 1000, t % 1000, k, who, e->ea, v[0], v[1], v[2], v[3], v[4]); break;
    default:
        snprintf(b, n, "%10llu.%03llu kind=%u ea=%08X", t / 1000, t % 1000, e->kind, e->ea);
    }
}

static int related(const jw_ev* e, uint32_t obj)
{
    if (e->ea >= obj && e->ea < obj + 0xA0u) return 1;
    if ((e->kind == JW_LLAR || e->kind == JW_PUTLLC) && e->v[3] == obj) return 1;
    if (e->kind == JW_WRITE && e->v[4] == obj) return 1;
    return 0;
}

static void dump_history(uint32_t at, uint32_t obj)
{
    uint32_t idx[48]; int n = 0;
    for (uint32_t k = 1; k < JW_RING - 64 && k <= at && n < 48; k++) {
        const jw_ev* e = &s_ring[(at - k) % JW_RING];
        if (e->seq != at - k + 1) break;
        if (related(e, obj)) idx[n++] = at - k;
    }
    fprintf(s_out, "    history of obj %08X (oldest first, %d events):\n", obj, n);
    char b[256];
    for (int j = n - 1; j >= 0; j--) {
        fmt_ev(b, sizeof b, &s_ring[idx[j] % JW_RING]);
        fprintf(s_out, "      %s\n", b);
    }
}

static uint32_t s_next;
static void drain(void)
{
    while (_InterlockedExchange(&s_out_lock, 1)) Sleep(0);
    const uint32_t h = (uint32_t)s_hdr->head;
    int any = 0;
    if (h - s_next > JW_RING - 64) {
        _InterlockedExchangeAdd(&s_hdr->cnt[C_LOST], (LONG)(h - s_next - (JW_RING - 64)));
        fprintf(s_out, "--- %u events lost (ring overrun) ---\n", h - s_next - (JW_RING - 64));
        s_next = h - (JW_RING - 64);
    }
    char b[256];
    for (; s_next != h; s_next++) {
        const jw_ev* e = &s_ring[s_next % JW_RING];
        for (int w = 0; e->seq != s_next + 1 && w < 1000; w++) Sleep(0);
        if (e->seq != s_next + 1) continue;
        if (e->kind < JW_ANOM && s_level < 2) continue;
        fmt_ev(b, sizeof b, e);
        fprintf(s_out, "%s\n", b); any = 1;
        if (e->kind >= JW_ANOM) {
            dump_history(s_next, e->ea);
            static volatile LONG n_err;
            if (_InterlockedIncrement(&n_err) <= 40) { fprintf(stderr, "[jobwatch] %s\n", b); fflush(stderr); }
        }
    }
    if (any) fflush(s_out);
    _InterlockedExchange(&s_out_lock, 0);
}

static DWORD WINAPI writer_thread(void* p)
{
    (void)p;
    uint64_t last = 0;
    for (;;) {
        Sleep(20);
        drain();
        const uint64_t t = now_us();
        if (t - last >= 10000000ull) {
            last = t;
            volatile LONG* c = s_hdr->cnt;
            fprintf(s_out, "--- %llus: objs=%ld get=%ld item=%ld put=%ld llar=%ld putllc ok/fail=%ld/%ld writes=%ld polls=%ld"
                           " | BAD-STATE=%ld CONCURRENT=%ld MIDPASS=%ld BADCALL=%ld lost=%ld stress=%ld\n",
                    (unsigned long long)(t / 1000000), s_nobj, c[C_GET], c[C_ITEM], c[C_PUT], c[C_LLAR], c[C_LLC_OK],
                    c[C_LLC_FAIL], c[C_WRITE], c[C_POLL], c[C_BADSTATE], c[C_CONCUR], c[C_MIDPASS], c[C_BADCALL],
                    c[C_LOST], c[C_STRESS]);
            fflush(s_out);
        }
    }
    return 0;
}

static DWORD WINAPI poll_thread(void* p)
{
    (void)p;
    for (unsigned scans = 0;; scans++) {
        const LONG n = s_nobj;
        for (LONG i = 0; i < n; i++) {
            jw_obj* o = &s_obj[i];
            const uint32_t s = mem16(o->ea + 0x18);
            if (s != o->poll_last) {
                push(JW_POLL, 0xFFFEu, 0, o->ea, o->poll_last, s, 0, 0, 0, 0);
                _InterlockedIncrement(&s_hdr->cnt[C_POLL]);
                o->poll_last = s;
            }
        }
        if (!n) { Sleep(50); continue; }
        for (int k = 0; k < 16; k++) _mm_pause();
        if ((scans & 1023u) == 0) SwitchToThread();
    }
    return 0;
}

static int jw_init(void)
{
    static volatile LONG once;
    if (_InterlockedCompareExchange(&once, 1, 0) != 0) {
        while (g_spu_jw_on < 0) Sleep(0);
        return g_spu_jw_on;
    }
    const char* e = getenv("R3_JOBWATCH");
    s_level = e ? atoi(e) : 0;
    if (s_level <= 0) { g_spu_jw_on = 0; return 0; }
    { const char* m = getenv("R3_JOBWATCH_MARK");   if (m && *m == '0') s_mark = 0; }
    { const char* m = getenv("R3_JOBWATCH_POLL");   if (m && *m == '0') s_poll = 0; }
    { const char* m = getenv("R3_JOBWATCH_STRESS"); if (m) s_stress = atoi(m); }
    QueryPerformanceFrequency(&s_qf); QueryPerformanceCounter(&s_q0);
    const DWORD size = JW_HDR + JW_RING * (DWORD)sizeof(jw_ev);
    char name[64]; snprintf(name, sizeof name, "Local\\R3JobWatch_%lu", GetCurrentProcessId());
    HANDLE hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, name);
    uint8_t* base = hm ? (uint8_t*)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, size) : NULL;
    if (!base) { base = (uint8_t*)calloc(1, size); name[0] = 0; }
    s_hdr = (jw_hdr*)base;
    s_ring = (jw_ev*)(base + JW_HDR);
    s_hdr->version = 1; s_hdr->pid = GetCurrentProcessId(); s_hdr->ring_n = JW_RING;
    s_hdr->ev_size = sizeof(jw_ev); s_hdr->qpc_freq = s_qf.QuadPart; s_hdr->qpc_start = s_q0.QuadPart;
    MemoryBarrier();
    s_hdr->magic = JW_MAGIC;
    CreateDirectoryA("logs", NULL);
    s_out = fopen("logs\\jobwatch.txt", "w");
    if (!s_out) s_out = stderr;
    fprintf(s_out, "R3_JOBWATCH=%d poll=%d mark=%d stress=%d shm=%s\n", s_level, s_poll, s_mark, s_stress,
            name[0] ? name : "(none)");
    fflush(s_out);
    fprintf(stderr, "[jobwatch] on (level %d, poll %d, mark %d, stress %d) -> logs\\jobwatch.txt, %s\n",
            s_level, s_poll, s_mark, s_stress, name[0] ? name : "no shared memory");
    fflush(stderr);
    CloseHandle(CreateThread(NULL, 0, writer_thread, NULL, 0, NULL));
    if (s_poll) CloseHandle(CreateThread(NULL, 0, poll_thread, NULL, 0, NULL));
    MemoryBarrier();
    g_spu_jw_on = 1;
    return 1;
}

/* ---- hooks -------------------------------------------------------------- */

int spu_jw_cmd(spu_context* ctx, uint32_t cmd, int post)
{
    if (g_spu_jw_on < 0 && !jw_init()) return 0;
    if (g_spu_jw_on <= 0 || !vm_base || !is_job403(ctx)) return 0;
    const uint32_t c = cmd & 0xFFu, lsa = ctx->mfc_lsa & SPU_LS_MASK, ea = ctx->mfc_eal, size = ctx->mfc_size;
    const uint16_t spu = (uint16_t)(ctx->spu_id & 0xFFu);
    const uint32_t tid = GetCurrentThreadId();
    const int is_get = c >= 0x40u && c <= 0x42u, is_put = c >= 0x20u && c <= 0x22u;
    const int is_llc = c == 0xB4u || c == 0xB0u || c == 0xB8u;
    jw_pass* P = pass_of(ctx);
    if (!post) {
        if (is_put && lsa == 0x14C70u && size == 0x10u) {
            stress(ctx, ea - 0x10u, 0);
            const uint32_t obj = ea - 0x10u;
            const uint32_t wr = be16(ctx->ls + 0x14C78u), mem = mem16(ea + 8u);
            uint32_t now[4]; for (int k = 0; k < 4; k++) now[k] = mem32(ea + 4u * (uint32_t)k);
            const uint32_t pass = P->obj == obj ? (uint32_t)(now_us() - P->t) : 0xFFFFFFFFu;
            push(JW_PUT, spu, tid, obj, wr, mem, P->obj == obj ? P->got : 0xFFFFu, now[0], pass, 0);
            _InterlockedIncrement(&s_hdr->cnt[C_PUT]);
            if (P->obj == obj && memcmp(now, P->snap, sizeof now) != 0) {
                /* Something wrote object+0x10..+0x1F while this pass ran; the
                 * PUT about to happen overwrites it with this pass's copy. */
                push(JW_MIDPASS, spu, tid, obj, P->got, mem, wr, now[0] ^ P->snap[0], now[1] ^ P->snap[1], now[2] ^ P->snap[2]);
                _InterlockedIncrement(&s_hdr->cnt[C_MIDPASS]);
            }
            jw_obj* o = find_obj(obj, 0);
            if (o && o->active == ctx) o->active = NULL;
            P->obj = 0;
        } else if (is_llc) {
            for (int k = 0; k < 4; k++) t_llc[k] = ls32(ctx, lsa + 4u * (uint32_t)k);
        }
        return 1;
    }
    if (is_get && lsa == 0x14C60u && size == 0xA0u) {
        const uint32_t st = be16(ctx->ls + 0x14C78u);
        uint32_t w[4]; for (int k = 0; k < 4; k++) w[k] = ls32(ctx, 0x14C70u + 4u * (uint32_t)k);
        jw_obj* o = find_obj(ea, 1);
        if (o) {
            void* other = o->active;
            if (other && other != ctx) {
                push(JW_CONCUR, spu, tid, ea, o->active_tid, (uint32_t)(now_us() - o->active_t), 0, 0, 0, 0);
                _InterlockedIncrement(&s_hdr->cnt[C_CONCUR]);
            }
            o->active = ctx; o->active_tid = tid; o->active_t = now_us();
        }
        P->obj = ea; P->got = st; memcpy(P->snap, w, sizeof w); P->t = now_us();
        push(JW_GET, spu, tid, ea, st, P->item, w[0], w[1], w[2], w[3]);
        _InterlockedIncrement(&s_hdr->cnt[C_GET]);
        if ((st & 0x8000u) || st >= 12u) {
            push(JW_BADSTATE, spu, tid, ea, st, P->item, 0, 0, 0, 0);
            _InterlockedIncrement(&s_hdr->cnt[C_BADSTATE]);
        }
        stress(ctx, ea, 1);
    } else if (is_get && size <= 0x20u) {
        P->item = ea;
        push(JW_ITEM, spu, tid, ea, lsa, size, ls32(ctx, lsa), ls32(ctx, lsa + 4), ls32(ctx, lsa + 8), ls32(ctx, lsa + 12));
        _InterlockedIncrement(&s_hdr->cnt[C_ITEM]);
    } else if (c == 0xD0u) {
        static __declspec(thread) uint32_t last[5];
        uint32_t w[4]; for (int k = 0; k < 4; k++) w[k] = ls32(ctx, lsa + 4u * (uint32_t)k);
        _InterlockedIncrement(&s_hdr->cnt[C_LLAR]);
        add_line(ea & ~127u);
        if (last[0] != (ea & ~127u) || memcmp(last + 1, w, sizeof w) != 0) {
            last[0] = ea & ~127u; memcpy(last + 1, w, sizeof w);
            push(JW_LLAR, spu, tid, ea & ~127u, w[0], w[1], w[2], w[3], lsa, 0);
        }
    } else if (is_llc) {
        const uint32_t st = ctx->atomic_stat;
        _InterlockedIncrement(&s_hdr->cnt[st == 1 ? C_LLC_FAIL : C_LLC_OK]);
        push(JW_PUTLLC, spu, tid, ea & ~127u, t_llc[0], t_llc[1], t_llc[2], t_llc[3], c == 0xB4u ? st : 2u, c);
    }
    return 1;
}

void spu_jw_note_write(uint32_t ea, const void* self_spu)
{
    if (g_spu_jw_on <= 0) return;
    const uint32_t line = ea & ~127u;
    const LONG n = s_nlines;
    LONG i = 0;
    for (; i < n; i++) if (s_lines[i] == line) break;
    if (i == n) return;
    uint32_t lr = 0, tid = 0, obj = 0, st = 0;
    const int ppu = ppu_jw_who(&lr, &tid);
    if (!ppu) tid = GetCurrentThreadId();
    if (obj_of(ea, &obj)) st = mem16(obj + 0x18);
    push(JW_WRITE, ppu ? 0xFFFFu : 0xFFFEu, tid, ea, mem32(ea), lr, self_spu ? 1u : 0u, st, obj, 0);
    _InterlockedIncrement(&s_hdr->cnt[C_WRITE]);
}

void spu_jw_badcall(spu_context* ctx, uint32_t target, uint32_t lr)
{
    if (g_spu_jw_on <= 0 || !is_job403(ctx)) return;
    const uint32_t st = be16(ctx->ls + 0x14C78u);
    jw_pass* P = pass_of(ctx);
    push(JW_BADCALL, (uint16_t)(ctx->spu_id & 0xFFu), GetCurrentThreadId(), P->obj,
         target, lr, st, P->got, P->item, 0);
    _InterlockedIncrement(&s_hdr->cnt[C_BADCALL]);
    drain();
}

#else  /* !_WIN32: the hooks exist and stay off */

int spu_jw_cmd(spu_context* ctx, uint32_t cmd, int post) { (void)ctx; (void)cmd; (void)post; g_spu_jw_on = 0; return 0; }
void spu_jw_note_write(uint32_t ea, const void* self_spu) { (void)ea; (void)self_spu; }
void spu_jw_badcall(spu_context* ctx, uint32_t target, uint32_t lr) { (void)ctx; (void)target; (void)lr; }

#endif

#ifdef __cplusplus
}
#endif
