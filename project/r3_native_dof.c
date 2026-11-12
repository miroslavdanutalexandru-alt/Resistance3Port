/* Resistance 3: ADS depth-of-field job (igjob 419 = igjob_00E06E80) --
 * kernel tracing / capture / native replacement.
 *
 * Job 419 itself is a streaming framework (DMA in/out, double buffering, no
 * float math). The per-block work runs in kernels it calls indirectly
 * (bisl at job image offsets 0x1358 / 0x15D4 / 0x187C -> link 0x145C /
 * 0x16D8 / 0x1980). The runtime calls g_spu_native_ib_hook at the top of
 * spu_indirect_branch (spu_channels.c); this file installs it when one of
 * these switches is set:
 *   R3_DOF_TRACE=1    log every distinct kernel target (+ time spent) called
 *                     from job 419.
 *   R3_DOF_CAPTURE=N  dump regs + LS before/after the first N calls of each
 *                     kernel target to scratch\dof_capture\ (offline verifier
 *                     input, R3DOF001 format below; project\tests\native_dof).
 *   R3_DOF_CAPTURE_EVERY=K  only capture every K-th call.
 *   R3_NATIVE_DOF=1   run the native kernel instead of the lifted one (default,
 *                     R3_NATIVE_DOF_DEFAULT); 0 = lifted SPU code.
 *   R3_NATIVE_DOF_IMG=<id>   native only for that kernel image (debugging).
 *   R3_DOF_VERIFY=1   run the lifted kernel, then the native one on a copy of
 *                     the state, compare LS + registers on every call (slow).
 *   R3_NATIVE_DOF_TOGGLE=<s> alternate native / lifted every s seconds (A/B).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif
#include "spu_context.h"
#include "r3_native_dof.h"
#include "r3_native_dof_kern.h"

extern int (*volatile g_spu_native_ib_hook)(spu_context*);
extern void spu_drain_call(spu_context* ctx, uint32_t return_pc);
extern void spu_indirect_branch(spu_context* ctx);

#define DOF_JOB_IMG 419

static int s_trace, s_cap_max, s_cap_every = 1, s_native, s_verify, s_toggle_last = -1;
static long long s_toggle_ticks;
static volatile long s_vfy_n, s_vfy_bad;
typedef int (*dof_gen_fn)(uint8_t*, u128*, uint32_t, uint32_t, uint32_t, uint32_t*);
static dof_gen_fn dof_gen_for(uint32_t img);
static __declspec(thread) int t_in_hook;

typedef struct {
    uint32_t lr, pc, owner, off;
    volatile long long n, ticks;
    volatile long caps;
} dof_key;
#define DOF_NKEY 64
static dof_key s_keys[DOF_NKEY];
static volatile long s_nkey;
static SRWLOCK s_klock = SRWLOCK_INIT;
static long long s_qf, s_next_print;

static int dof_is_call_site(uint32_t lr)
{
    return lr == 0x145Cu || lr == 0x16D8u || lr == 0x1980u;
}

static void dof_owner(spu_context* ctx, uint32_t pc, uint32_t* owner, uint32_t* off)
{
    *owner = 0; *off = pc;
    for (unsigned s = 0; s < SPU_RESIDENT_CODE_SLOTS; ++s) {
        if (ctx->resident_code[s].image_id && pc >= ctx->resident_code[s].lsa &&
            pc - ctx->resident_code[s].lsa < ctx->resident_code[s].size) {
            *owner = (uint32_t)ctx->resident_code[s].image_id;
            *off = pc - ctx->resident_code[s].lsa;
            return;
        }
    }
}

static dof_key* dof_key_get(uint32_t lr, uint32_t pc, uint32_t owner, uint32_t off)
{
    long n = s_nkey;
    for (long i = 0; i < n; i++)
        if (s_keys[i].lr == lr && s_keys[i].owner == owner && s_keys[i].off == off) return &s_keys[i];
    AcquireSRWLockExclusive(&s_klock);
    n = s_nkey;
    dof_key* k = NULL;
    for (long i = 0; i < n; i++)
        if (s_keys[i].lr == lr && s_keys[i].owner == owner && s_keys[i].off == off) { k = &s_keys[i]; break; }
    if (!k && n < DOF_NKEY) {
        k = &s_keys[n];
        k->lr = lr; k->pc = pc; k->owner = owner; k->off = off;
        MemoryBarrier();
        s_nkey = n + 1;
        fprintf(stderr, "[ndof] new kernel #%ld: lr=0x%05X target=0x%05X owner_img=%u off=0x%X%c",
                n, lr, pc, owner, off, 10);
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&s_klock);
    return k;
}

/* R3DOF001: magic[8], u32 lr, pc, owner, off, ls_bias, image_id, spu_id, seq,
 * then pre {gpr[128] 2048 B, LS 256 KB}, post {gpr, LS}. */
static void dof_write_capture(const dof_key* k, long seq, spu_context* ctx,
                              const void* pre_gpr, const uint8_t* pre_ls)
{
    char fn[260];
    snprintf(fn, sizeof fn, "scratch\\dof_capture\\k%u_%05X_%04ld.bin", k->owner, k->off, seq);
    FILE* f = fopen(fn, "wb");
    if (!f) return;
    uint32_t h[8] = { k->lr, k->pc, k->owner, k->off, ctx->ls_bias, (uint32_t)ctx->image_id,
                      ctx->spu_id, (uint32_t)seq };
    fwrite("R3DOF001", 1, 8, f);
    fwrite(h, 4, 8, f);
    fwrite(pre_gpr, 1, sizeof ctx->gpr, f);
    fwrite(pre_ls, 1, SPU_LS_SIZE, f);
    fwrite(ctx->gpr, 1, sizeof ctx->gpr, f);
    fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
    fclose(f);
}

static void dof_print_stats(void)
{
    long n = s_nkey;
    for (long i = 0; i < n; i++) {
        const dof_key* k = &s_keys[i];
        fprintf(stderr, "[ndof] kernel lr=0x%05X img=%u off=0x%X calls=%lld total=%.1f ms avg=%.1f us%c",
                k->lr, k->owner, k->off, k->n, (double)k->ticks * 1000.0 / (double)s_qf,
                k->n ? (double)k->ticks * 1e6 / (double)s_qf / (double)k->n : 0.0, 10);
    }
    fflush(stderr);
}

static int env_int(const char* n, int d);
static int dof_hook(spu_context* ctx)
{
    const uint32_t lr = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    if (s_trace >= 2 && !t_in_hook && (ctx->image_id == DOF_JOB_IMG || dof_is_call_site(lr))) {
        static volatile long nlog;
        if (InterlockedIncrement(&nlog) <= 200)
            fprintf(stderr, "[ndof] ib img=%d lr=0x%05X pc=0x%05X bias=0x%05X%c", ctx->image_id, lr,
                    (uint32_t)ctx->pc & SPU_LS_MASK, ctx->ls_bias, 10);
    }
    if (!dof_is_call_site(lr) || t_in_hook) return 0;
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK & ~3u;
    uint32_t owner, off;
    dof_owner(ctx, pc, &owner, &off);
    /* plain native mode (the default): no bookkeeping, no log lines */
    if (s_native && !s_trace && !s_verify && !s_toggle_ticks && s_cap_max <= 0) {
        if (!r3_native_dof_kernel(ctx, owner, off)) return 0;
        ctx->pc = lr;
        return 1;
    }
    dof_key* k = dof_key_get(lr, pc, owner, off);
    if (!k) return 0;

    int native = s_native;
    if (s_toggle_ticks) {   /* R3_NATIVE_DOF_TOGGLE: same-scene A/B, native and lifted alternate */
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        native = (int)((t.QuadPart / s_toggle_ticks) & 1);
        if (native != s_toggle_last) {
            s_toggle_last = native;
            fprintf(stderr, "[ndof] toggle: %s kernels from now%c", native ? "NATIVE" : "LIFTED", 10);
            if (s_trace) dof_print_stats();
        }
    }
    if (native && !s_verify) {
        LARGE_INTEGER n0, n1;
        if (s_trace) QueryPerformanceCounter(&n0);
        int r = r3_native_dof_kernel(ctx, owner, off);
        if (r) {
            ctx->pc = lr;
            if (s_trace) {
                QueryPerformanceCounter(&n1);
                InterlockedIncrement64(&k->n);
                InterlockedAdd64(&k->ticks, n1.QuadPart - n0.QuadPart);
                if (n1.QuadPart > s_next_print) { s_next_print = n1.QuadPart + 5 * s_qf; dof_print_stats(); }
            }
            return 1;
        }
        if (!s_trace && s_cap_max <= 0) return 0;
    }

    /* R3_DOF_VERIFY=1: run the lifted kernel on the real state and the native
     * one on a copy, compare LS + registers (in-game bit-exactness check). */
    static __declspec(thread) uint8_t* t_vls;
    static __declspec(thread) u128* t_vgpr;
    int vfy = 0;
    if (s_verify && dof_gen_for(owner)) {
        if (!t_vls) { t_vls = (uint8_t*)_aligned_malloc(SPU_LS_SIZE, 64); t_vgpr = (u128*)_aligned_malloc(sizeof ctx->gpr, 64); }
        if (t_vls && t_vgpr) {
            memcpy(t_vls, ctx->ls, SPU_LS_SIZE);
            memcpy(t_vgpr, ctx->gpr, sizeof ctx->gpr);
            vfy = 1;
        }
    }

    long seq = -1;
    uint8_t* snap = NULL;
    if (s_cap_max > 0 && k->caps < s_cap_max && (k->n % s_cap_every) == 0) {
        seq = InterlockedIncrement(&k->caps) - 1;
        if (seq < s_cap_max) {
            snap = (uint8_t*)malloc(sizeof ctx->gpr + SPU_LS_SIZE);
            if (snap) {
                memcpy(snap, ctx->gpr, sizeof ctx->gpr);
                memcpy(snap + sizeof ctx->gpr, ctx->ls, SPU_LS_SIZE);
            }
        }
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    /* run the lifted kernel synchronously, exactly as the call site would */
    t_in_hook = 1;
    ctx->host_depth++;
    spu_indirect_branch(ctx);
    spu_drain_call(ctx, lr);
    ctx->host_depth--;
    t_in_hook = 0;
    QueryPerformanceCounter(&t1);
    InterlockedIncrement64(&k->n);
    InterlockedAdd64(&k->ticks, t1.QuadPart - t0.QuadPart);
    if (snap) {
        dof_write_capture(k, seq, ctx, snap, snap + sizeof ctx->gpr);
        free(snap);
    }
    if (vfy) {
        uint32_t fail_pc = 0;
        const int rc = dof_gen_for(owner)(t_vls, t_vgpr, pc - off, pc, lr, &fail_pc);
        const int same = rc == 1 && !memcmp(t_vls, ctx->ls, SPU_LS_SIZE) && !memcmp(t_vgpr, ctx->gpr, sizeof ctx->gpr);
        const long nv = InterlockedIncrement(&s_vfy_n);
        if (!same) {
            const long nb = InterlockedIncrement(&s_vfy_bad);
            if (nb <= 10) {
                long first = -1, nd = 0;
                for (long i = 0; i < SPU_LS_SIZE; i++) if (t_vls[i] != ctx->ls[i]) { if (first < 0) first = i; nd++; }
                int reg = -1;
                for (int r = 0; r < 128; r++) if (memcmp(&t_vgpr[r], &ctx->gpr[r], 16)) { reg = r; break; }
                fprintf(stderr, "[ndof] VERIFY MISMATCH img=%u off=0x%X rc=%d: %ld LS bytes differ (first 0x%lX), first reg r%d%c",
                        owner, off, rc, nd, first, reg, 10);
            }
        }
        if ((nv & 0xFFFF) == 0 || nv == 1000)
            fprintf(stderr, "[ndof] verify: %ld kernel calls compared, %ld mismatches%c", nv, s_vfy_bad, 10);
    }
    if (s_trace && t1.QuadPart > s_next_print) {
        s_next_print = t1.QuadPart + 5 * s_qf;
        dof_print_stats();
    }
    ctx->pc = lr;
    return 1;
}

static int env_int(const char* n, int d)
{
    const char* e = getenv(n);
    return (e && *e) ? atoi(e) : d;
}

__attribute__((constructor)) static void r3_native_dof_init(void)
{
    LARGE_INTEGER q; QueryPerformanceFrequency(&q); s_qf = q.QuadPart;
    s_trace = env_int("R3_DOF_TRACE", 0);
    s_cap_max = env_int("R3_DOF_CAPTURE", 0);
    s_cap_every = env_int("R3_DOF_CAPTURE_EVERY", 1); if (s_cap_every < 1) s_cap_every = 1;
    s_native = env_int("R3_NATIVE_DOF", R3_NATIVE_DOF_DEFAULT);
    s_verify = env_int("R3_DOF_VERIFY", 0);
    { const int ts = env_int("R3_NATIVE_DOF_TOGGLE", 0); if (ts > 0) s_toggle_ticks = (long long)ts * s_qf; }
    if (s_cap_max > 0) _mkdir("scratch\\dof_capture");
    if (s_trace || s_cap_max > 0 || s_native || s_verify || s_toggle_ticks) {
        g_spu_native_ib_hook = dof_hook;
        fprintf(stderr, "[ndof] hook on: trace=%d capture=%d native=%d verify=%d toggle=%llds%c", s_trace, s_cap_max,
                s_native, s_verify, s_qf ? s_toggle_ticks / s_qf : 0, 10);
    }
}

/* Native kernels: every kernel image job 419 calls (618..623), re-emitted by
 * scratch\dof_native\transpile.py as one native function each (all SPU
 * registers in locals, same spu_* float/integer helpers in the same order ->
 * bit-identical; verify_dof.exe replays the captures). R3_NATIVE_DOF_IMG=<id>
 * limits the native path to one image (debugging). */
static dof_gen_fn dof_gen_for(uint32_t img)
{
    switch (img) {
    case 618: return r3dof_gen_618; case 619: return r3dof_gen_619; case 620: return r3dof_gen_620;
    case 621: return r3dof_gen_621; case 622: return r3dof_gen_622; case 623: return r3dof_gen_623;
    default: return NULL;
    }
}
int r3_native_dof_kernel(struct spu_context* ctx, uint32_t owner_img, uint32_t off)
{
    static int only = -1;
    if (only < 0) only = env_int("R3_NATIVE_DOF_IMG", 0);
    if (only && (int)owner_img != only) return 0;
    const dof_gen_fn g = dof_gen_for(owner_img);
    if (!g) return 0;
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK & ~3u;
    const uint32_t lr = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    uint32_t fail_pc = 0;
    if (g(ctx->ls, ctx->gpr, pc - off, pc, lr, &fail_pc) == 1) return 1;
    /* Left the image (never seen): the registers / LS are the exact SPU state
     * at that branch, so the lifted code carries on from there. */
    static volatile long nwarn;
    if (InterlockedIncrement(&nwarn) <= 8)
        fprintf(stderr, "[ndof] native img %u left the kernel at 0x%05X -- lifted code continues%c",
                owner_img, fail_pc & SPU_LS_MASK, 10);
    ctx->pc = fail_pc & SPU_LS_MASK;
    t_in_hook = 1;
    ctx->host_depth++;
    spu_indirect_branch(ctx);
    spu_drain_call(ctx, lr);
    ctx->host_depth--;
    t_in_hook = 0;
    return 1;
}