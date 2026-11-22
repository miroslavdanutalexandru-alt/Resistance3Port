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
 *   R3_DOF_GRAPHLOG=1 log job 419's op graph ([dof-graph] lines: op blocks,
 *                     per-frame parameter blocks, call counts, op sequence).
 *   R3_DOF_HOSTJOB=1  shadow-verify the host decode of job 419's step list
 *                     ([dof-host] lines; see hj_pre). Implies GRAPHLOG.
 *                     R3_DOF_HOSTJOB_VERIFY_N / R3_DOF_HOSTJOB_EVERY: sampling.
 *   R3_NATIVE_DOF_JOB=1  kernel 623 (the hot one) runs the bit-exact AVX2 bulk
 *                     loops of r3_native_dof_job.c (0 = lifted SPU code).
 *                     Only in PORT_SPU_AVX2 builds (R3_NATIVE_DOF_JOB_BUILT);
 *                     elsewhere it is ignored with a notice. Check in game
 *                     first with R3_DOF_VERIFY=1.
 *   R3_NATIVE_DOF_JOB=2  also modes 8 and 9 (the mode-9 gather blur) of kernel
 *                     623 (1 = modes 4 and 6 only).
 *   R3_NATIVE_DOF_JOB=3  =2 plus a lean DMA path for job 419's own transfers
 *                     (spu_dma.h mfc_do_transfer_lean: same state/memory
 *                     effects, no per-transfer env diagnostics). Not armed
 *                     if any SPU DMA diagnostic env (list below) is set, or
 *                     R3_DOF_LEAN_DMA_OFF=1. Works without PORT_SPU_AVX2 too.
 *   R3_NATIVE_DOF_JOB=4  =3 plus bulk list DMA: runs of contiguous list
 *                     elements of job 419 move with one range check and one
 *                     memcpy (spu_dma.h mfc_list_bulk; per-element notes
 *                     kept, anything unusual falls back per element). Needs
 *                     the lean path armed; R3_DOF_BULK_DMA_OFF=1 disables.
 *   R3_DOF_GPU_APPROX=1  "Aim blur: PC fast": job 419 is skipped and a host
 *                     approximation (r3_dof_fast.c) writes its output, after
 *                     the PS3 path was checked on probe frames ([dof-fast]
 *                     lines; see fast_entry). Not pixel-identical. Default 1
 *                     (R3_DOF_GPU_APPROX_DEFAULT); 0 = "PS3 exact".
 *   R3_DOF_GPU=0      with R3_DOF_GPU_APPROX=1: the host approximation runs on
 *                     the CPU (light buffer readback). Default 1: the RSX
 *                     computes it in Vulkan compute (VKR3DofGpu.cpp, [dof-gpu]).
 *   R3_DOF_FAST_CACHE=0  every session probes job 419 on the PS3 path before
 *                     PC fast runs (first ADS ~1.5 s at ~25 fps). Default 1:
 *                     an arming is saved to r3_dof_fast.cache next to the exe
 *                     and the next session arms from it (fast_cache_try_arm).
 *   R3_DOF_FAST_RAMP_KEEP=0  a CoC-settling probe resets the good-probe count
 *                     (old rule). Default 1: it is not counted either way.
 *   Default R3_NATIVE_DOF_JOB = 4 (R3_NATIVE_DOF_JOB_DEFAULT) with
 *   R3_DOF_FAST_SUBMIT = 1 (both 0 = off).
 *   R3_SEMFIX_CYCLE=0  don't tag label-77 stores with their job serial (semfix
 *                     may then restore a consumed cycle's store: ADS purple).
 *   R3_DOF_WAIT_RSX=1  (default 0) hold job 419's start until the RSX processed
 *                     label 77's release; R3_DOF_WAIT_RSX_MS cap (40).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif
#include "spu_context.h"
#include "r3_native_dof.h"
#include "r3_native_dof_kern.h"
#include "r3_dof_fast.h"
#include "port_native_summary.h"
#ifdef R3_NATIVE_DOF_JOB_BUILT
#include "r3_native_dof_job.h"
#endif

extern int (*volatile g_spu_native_ib_hook)(spu_context*);
extern void spu_drain_call(spu_context* ctx, uint32_t return_pc);
extern void spu_indirect_branch(spu_context* ctx);
extern volatile int g_spu_dma_lean_img;   /* spu_dma.h (selectany) */
extern volatile int g_spu_dma_bulk_img;   /* spu_dma.h (selectany) */
extern volatile int g_spu_dma_submit_img; /* spu_dma.h (selectany) */

#define DOF_JOB_IMG 419
extern volatile long g_port_frame_no;

static int s_trace, s_cap_max, s_cap_every = 1, s_native, s_native_job, s_verify, s_toggle_last = -1;
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
#ifdef R3_NATIVE_DOF_JOB_BUILT
    if (s_native_job)
        fprintf(stderr, "[ndof] job623: m4 bulk=%ld fallback=%ld groups=%ld | m6 bulk=%ld fallback=%ld groups=%ld%c",
                (long)g_r3dofj_stat[0], (long)g_r3dofj_stat[1], (long)g_r3dofj_stat[2],
                (long)g_r3dofj_stat[3], (long)g_r3dofj_stat[4], (long)g_r3dofj_stat[5], 10);
    if (s_native_job >= 2)
        fprintf(stderr, "[ndof] job623: m8 bulk=%ld fallback=%ld groups=%ld | m9 gather bulk=%ld fallback=%ld groups=%ld%c",
                (long)g_r3dofj_stat[9], (long)g_r3dofj_stat[10], (long)g_r3dofj_stat[11],
                (long)g_r3dofj_stat[6], (long)g_r3dofj_stat[7], (long)g_r3dofj_stat[8], 10);
#endif
    fflush(stderr);
}

/* R3_DOF_GRAPHLOG=1: read-only dump of job 419's op graph, the input for a
 * host / GPU port of the DOF (r3_ads_round.md). At each kernel call LS[r3]
 * points at the current op block (0x60 bytes, big-endian): +0 counts, +0xC
 * kernel image EA, +0x10/+0x14 code sizes, +0x18/+0x1C per-frame parameter
 * block EA/size (triple-buffered), +0x20 mode, +0x24..+0x3C float constants,
 * +0x40/+0x50 output/input buffer descriptors {EA, pitch, w<<16|h, flags}.
 * Each distinct op (all but +0x18) is logged once, with its parameter block;
 * every 5 s the per-op call counts and the parameter blocks again (30 times
 * at most); and the op index sequence of the first 3 x 512 calls per thread. */
static int s_graphlog;
typedef struct { uint8_t op[0x60]; uint32_t img; volatile long long n; } dof_gop;
#define DOF_NGOP 64
static dof_gop s_gop[DOF_NGOP];
static volatile long s_ngop, s_gseq_threads, s_greports;
static SRWLOCK s_glock = SRWLOCK_INIT;
static long long s_gnext;
static __declspec(thread) uint8_t t_gseq[512];
static __declspec(thread) int t_gseq_n, t_gseq_done;

static uint32_t dof_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void dof_graph_params(const char* tag, long i, uint32_t ea, uint32_t size)
{
    extern uint8_t* vm_base;
    char hex[0x100 * 9 + 1]; int o = 0;
    if (size > 0x100) size = 0x100;
    if (!vm_base || ea < 0x10000u || ea >= 0x10000000u || (ea & 3u)) size = 0;
    if (size) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (uint8_t*)mi.BaseAddress + mi.RegionSize < vm_base + ea + size) size = 0;
    }
    for (uint32_t k = 0; k < size; k += 4) o += snprintf(hex + o, sizeof hex - o, "%08X ", dof_be32(vm_base + ea + k));
    hex[o] = 0;
    fprintf(stderr, "[dof-graph] %s op#%ld params ea=0x%08X: %s%c", tag, i, ea, hex, 10);
}

static void dof_graphlog(spu_context* ctx, uint32_t owner)
{
    const uint8_t* ls = (const uint8_t*)ctx->ls;
    const uint32_t r3 = ctx->gpr[3]._u32[0] & 0x3FFF0u;
    const uint32_t p = dof_be32(ls + r3) & 0x3FFF0u;
    if (p > SPU_LS_SIZE - 0x60u) return;
    uint8_t op[0x60];
    memcpy(op, ls + p, sizeof op);
    memset(op + 0x18, 0, 4);
    long idx = -1;
    AcquireSRWLockExclusive(&s_glock);
    for (long i = 0; i < s_ngop; i++)
        if (s_gop[i].img == owner && !memcmp(s_gop[i].op, op, sizeof op)) { idx = i; break; }
    if (idx < 0 && s_ngop < DOF_NGOP) {
        idx = s_ngop;
        memcpy(s_gop[idx].op, op, sizeof op); s_gop[idx].img = owner;
        char w[0x60 / 4 * 9 + 1]; int o = 0;
        for (int k = 0; k < 0x60; k += 4) o += snprintf(w + o, sizeof w - o, "%08X ", dof_be32(ls + p + k));
        fprintf(stderr, "[dof-graph] new op#%ld img=%u: %s%c", idx, owner, w, 10);
        dof_graph_params("new", idx, dof_be32(ls + p + 0x18), dof_be32(ls + p + 0x1C));
        s_ngop = idx + 1;
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (t.QuadPart > s_gnext && s_greports < 30) {
        s_gnext = t.QuadPart + 5 * s_qf; s_greports++;
        for (long i = 0; i < s_ngop; i++)
            fprintf(stderr, "[dof-graph] op#%ld img=%u calls=%lld%c", i, s_gop[i].img, s_gop[i].n, 10);
        if (idx >= 0) dof_graph_params("now", idx, dof_be32(ls + p + 0x18), dof_be32(ls + p + 0x1C));
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&s_glock);
    if (idx < 0) return;
    InterlockedIncrement64(&s_gop[idx].n);
    if (t_gseq_done) return;
    t_gseq[t_gseq_n++] = (uint8_t)idx;
    if (t_gseq_n < (int)sizeof t_gseq) return;
    t_gseq_done = 1;
    if (InterlockedIncrement(&s_gseq_threads) > 3) return;
    char s[512 * 8]; int o = 0;
    for (int i = 0; i < t_gseq_n;) {
        int j = i; while (j < t_gseq_n && t_gseq[j] == t_gseq[i]) j++;
        o += snprintf(s + o, sizeof s - o, "%d", t_gseq[i]);
        if (j - i > 1) o += snprintf(s + o, sizeof s - o, "x%d", j - i);
        o += snprintf(s + o, sizeof s - o, " ");
        i = j;
    }
    fprintf(stderr, "[dof-graph] seq spu=%u: %s%c", ctx->spu_id, s, 10);
}

/* R3_DOF_HOSTJOB=1: shadow verification of the host decode of job 419's step
 * list (r3_ads_round.md, round 4). Each op is one small image (e.g. 120x88)
 * read from the op block's input descriptor and written to its output
 * descriptor; ops chain through ping-pong buffers. A host / GPU job needs the
 * exact data flow, so on sampled process-kernel calls (the first
 * R3_DOF_HOSTJOB_VERIFY_N, default 256, then every R3_DOF_HOSTJOB_EVERY-th,
 * default 64) the op is decoded from the same LS the SPU job reads and:
 *   get: the input rows in guest memory are located in the kernel's LS
 *        (row stride, bytes per row, apron learned on the first sample);
 *   put: the kernel's LS result is located in guest memory at the output
 *        descriptor when the next op on that lane reads it.
 * An op whose layout cannot be found is "unsupported" (logged once with a dump
 * and left to the SPU path). An op that matched before and then does not is a
 * verify FAIL: logged once, verification stops for the session. Nothing is
 * substituted -- the SPU path (native kernels) still produces the image -- so
 * a wrong guess cannot change the picture. Implies R3_DOF_GRAPHLOG=1. */
static int s_hostjob, s_hj_vn = 256, s_hj_every = 64;
static volatile long s_hj_off, s_hj_calls, s_hj_get_ok, s_hj_put_ok, s_hj_unsup, s_hj_inconc, s_hj_relink;
static long long s_hj_next;
typedef struct {
    uint32_t img, mode, in[4], out[4];
    uint32_t istride, irowb, ostride, orowb;   /* learned layout, 0 = not yet */
    int iunsup, ounsup;
} hj_op;
#define HJ_NOP 64
static hj_op s_hj[HJ_NOP];
static volatile long s_hjn;
static SRWLOCK s_hjlock = SRWLOCK_INIT;
static __declspec(thread) uint8_t* t_hj_ls;
static __declspec(thread) int t_hj_pend;
static __declspec(thread) long t_hj_pidx;
static __declspec(thread) uint32_t t_hj_poutls;

static const uint8_t* hj_mem(uint32_t ea, uint32_t size)
{
    extern uint8_t* vm_base;
    if (!vm_base || ea < 0x10000u || (uint64_t)ea + size > 0xF0000000ull || !size) return NULL;
    for (uint32_t o = 0; o < size;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea + o, &mi, sizeof mi) || mi.State != MEM_COMMIT) return NULL;
        const uint64_t end = (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - (vm_base + ea));
        if (end <= o) return NULL;
        o = end >= size ? size : (uint32_t)end;
    }
    return vm_base + ea;
}

static long hj_find(const uint8_t* ls, uint32_t from, const uint8_t* nee, uint32_t nn)
{
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t a = pass ? 0 : from, b = pass ? from : SPU_LS_SIZE;
        for (uint32_t o = a & ~3u; o + nn <= b; o += 4)
            if (ls[o] == nee[0] && !memcmp(ls + o, nee, nn)) return (long)o;
    }
    return -1;
}

/* Locate the rows of a guest image {ea, pitch, w<<16|h} in an LS image.
 * 1 = found (stride / rowb / base set), 0 = not found, -1 = inconclusive
 * (rows too uniform to tell, or the guest range is not readable). */
static int hj_locate(const uint8_t* ls, uint32_t from, const uint32_t d[4],
                     uint32_t* stride, uint32_t* rowb, uint32_t* base, uint32_t* bad_off)
{
    const uint32_t ea = d[0], pitch = d[1], w = d[2] >> 16, h = d[2] & 0xFFFFu;
    *bad_off = 0;
    if (!w || !h || !pitch || h > 4096 || pitch > 0x100000u) return -1;
    const uint32_t mid = h / 2, rows_len = pitch * (h - 1) + (pitch < w * 16 ? pitch : w * 16);
    const uint8_t* m = hj_mem(ea, rows_len);
    if (!m) return -1;
    uint32_t pn = w * 4 < 64 ? w * 4 : 64;
    if (pn > pitch) pn = pitch;
    if (pn < 8) return -1;
    const uint8_t* probe = m + (size_t)mid * pitch;
    int varied = 0;
    for (uint32_t i = 4; i < pn && !varied; i++) varied = probe[i] != probe[i & 3];
    if (!varied) return -1;
    const long pos = hj_find(ls, from, probe, pn);
    if (pos < 0) { *bad_off = mid * pitch; return 0; }
    uint32_t ml = 0;
    while (ml < pitch && pos + ml < SPU_LS_SIZE && ls[pos + ml] == probe[ml]) ml++;
    static const uint32_t bpps[] = { 16, 12, 8, 4, 2, 1 };
    uint32_t rb = 0;
    for (int i = 0; i < 6 && !rb; i++) if (w * bpps[i] <= ml) rb = w * bpps[i];
    if (!rb) { *bad_off = mid * pitch + ml; return 0; }
    uint32_t st = 0;
    if (h > 1) {
        const uint32_t r2 = mid + 1 < h ? mid + 1 : mid - 1;
        const long p2 = hj_find(ls, (uint32_t)pos + 4, m + (size_t)r2 * pitch, pn);
        if (p2 < 0) { *bad_off = r2 * pitch; return 0; }
        const long dlt = r2 > mid ? p2 - pos : pos - p2;
        if (dlt < (long)rb) { *bad_off = r2 * pitch; return 0; }
        st = (uint32_t)dlt;
    }
    if ((uint64_t)st * mid > (uint64_t)pos) { *bad_off = 0; return 0; }
    const uint32_t b = (uint32_t)pos - st * mid;
    if ((uint64_t)b + (uint64_t)st * (h - 1) + rb > SPU_LS_SIZE) { *bad_off = (h - 1) * pitch; return 0; }
    for (uint32_t y = 0; y < h; y++)
        if (memcmp(ls + b + y * st, m + (size_t)y * pitch, rb)) {
            uint32_t c = 0; while (c < rb && ls[b + y * st + c] == m[(size_t)y * pitch + c]) c++;
            *bad_off = y * pitch + c;
            return 0;
        }
    *stride = st; *rowb = rb; *base = b;
    return 1;
}

static void hj_dump16(char* o, size_t n, const uint8_t* p)
{
    int k = 0;
    for (int i = 0; i < 16; i += 4) k += snprintf(o + k, n - k, "%08X ", dof_be32(p + i));
}

/* one check: stage 0 = get (input rows in the kernel's LS), 1 = put */
static void hj_check(long idx, int stage, const uint8_t* ls, uint32_t from)
{
    hj_op* o = &s_hj[idx];
    const uint32_t* d = stage ? o->out : o->in;
    uint32_t st = 0, rb = 0, base = 0, bad = 0;
    const int r = hj_locate(ls, from, d, &st, &rb, &base, &bad);
    if (r < 0) { InterlockedIncrement(&s_hj_inconc); return; }
    AcquireSRWLockExclusive(&s_hjlock);
    uint32_t* lst = stage ? &o->ostride : &o->istride;
    uint32_t* lrb = stage ? &o->orowb : &o->irowb;
    int* uns = stage ? &o->ounsup : &o->iunsup;
    const char* sn = stage ? "put" : "get";
    if (*uns || s_hj_off) { ReleaseSRWLockExclusive(&s_hjlock); return; }
    if (r == 1 && !*lrb) {
        *lst = st; *lrb = rb;
        fprintf(stderr, "[dof-host] layout op#%ld img=%u mode=%u %s: ea=0x%08X pitch=0x%X %ux%u -> LS 0x%05X stride=0x%X row=0x%X (%u B/px)%c",
                idx, o->img, o->mode, sn, d[0], d[1], d[2] >> 16, d[2] & 0xFFFFu, base, st, rb, rb / (d[2] >> 16), 10);
    } else if (r == 1 && *lst == st && *lrb == rb) {
        if (base != (stage ? t_hj_poutls : from)) InterlockedIncrement(&s_hj_relink);
        InterlockedIncrement(stage ? &s_hj_put_ok : &s_hj_get_ok);
    } else {
        char lh[48] = "-", mh[48] = "-";
        const uint8_t* m = hj_mem(d[0] + (bad & ~15u), 16);
        if (m) hj_dump16(mh, sizeof mh, m);
        if (from + 16 <= SPU_LS_SIZE) hj_dump16(lh, sizeof lh, ls + (from & ~15u));
        if (!*lrb) {
            *uns = 1; InterlockedIncrement(&s_hj_unsup);
            fprintf(stderr, "[dof-host] unsupported op#%ld img=%u mode=%u %s: rows of ea=0x%08X pitch=0x%X %ux%u flags=0x%08X not found in LS (off=0x%X) -> SPU path; mem: %sls@0x%05X: %s%c",
                    idx, o->img, o->mode, sn, d[0], d[1], d[2] >> 16, d[2] & 0xFFFFu, d[3], bad, mh, from, lh, 10);
        } else {
            s_hj_off = 1;
            fprintf(stderr, "[dof-host] verify FAIL op=%ld img=%u mode=%u %s off=0x%X (layout %s: stride 0x%X row 0x%X, learned 0x%X/0x%X) frame=%ld; mem: %sls@0x%05X: %s -- host decode disabled for the session%c",
                    idx, o->img, o->mode, sn, bad, r == 1 ? "changed" : "lost", st, rb, *lst, *lrb,
                    (long)g_port_frame_no, mh, from, lh, 10);
        }
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&s_hjlock);
}

static long hj_op_get(uint32_t img, const uint8_t* blk)
{
    uint32_t mode = dof_be32(blk + 0x20), in[4], out[4];
    for (int i = 0; i < 4; i++) { out[i] = dof_be32(blk + 0x40 + 4 * i); in[i] = dof_be32(blk + 0x50 + 4 * i); }
    long n = s_hjn;
    for (long i = 0; i < n; i++)
        if (s_hj[i].img == img && s_hj[i].mode == mode && !memcmp(s_hj[i].in, in, 16) && !memcmp(s_hj[i].out, out, 16)) return i;
    AcquireSRWLockExclusive(&s_hjlock);
    long r = -1;
    for (long i = 0; i < s_hjn; i++)
        if (s_hj[i].img == img && s_hj[i].mode == mode && !memcmp(s_hj[i].in, in, 16) && !memcmp(s_hj[i].out, out, 16)) { r = i; break; }
    if (r < 0 && s_hjn < HJ_NOP) {
        r = s_hjn;
        hj_op* o = &s_hj[r];
        memset(o, 0, sizeof *o);
        o->img = img; o->mode = mode; memcpy(o->in, in, 16); memcpy(o->out, out, 16);
        MemoryBarrier();
        s_hjn = r + 1;
    }
    ReleaseSRWLockExclusive(&s_hjlock);
    return r;
}

static void hj_report(void)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (t.QuadPart < s_hj_next) return;
    s_hj_next = t.QuadPart + 5 * s_qf;
    if (s_hj_off) return;
    fprintf(stderr, "[dof-host] verify ok frame=%ld: %ld get + %ld put checks matched, %ld op kinds (%ld unsupported), %ld inconclusive, %ld not at r4%c",
            (long)g_port_frame_no, s_hj_get_ok, s_hj_put_ok, s_hjn, s_hj_unsup, s_hj_inconc, s_hj_relink, 10);
    fflush(stderr);
}

/* before a process-kernel call: put check of this lane's previous op, get
 * check of this one. 1 = sampled (capture the kernel's result in hj_post). */
static int hj_pre(spu_context* ctx, uint32_t owner)
{
    const uint8_t* ls = (const uint8_t*)ctx->ls;
    const uint32_t r3 = ctx->gpr[3]._u32[0] & 0x3FFF0u;
    const uint32_t p = dof_be32(ls + r3) & 0x3FFF0u;
    const int pend = t_hj_pend;
    t_hj_pend = 0;
    if (p > SPU_LS_SIZE - 0x60u) return 0;
    if (pend && t_hj_ls && dof_be32(ls + p + 0x50) == s_hj[t_hj_pidx].out[0])
        hj_check(t_hj_pidx, 1, t_hj_ls, t_hj_poutls);
    const long n = InterlockedIncrement(&s_hj_calls);
    if (n > s_hj_vn && (n % s_hj_every) != 0) return 0;
    const long idx = hj_op_get(owner, ls + p);
    if (idx < 0) return 0;
    hj_check(idx, 0, ls, ctx->gpr[4]._u32[0] & SPU_LS_MASK & ~3u);
    hj_report();
    if (s_hj_off || s_hj[idx].ounsup) return 0;
    if (!t_hj_ls && !(t_hj_ls = (uint8_t*)_aligned_malloc(SPU_LS_SIZE, 64))) return 0;
    t_hj_pidx = idx;
    t_hj_poutls = ctx->gpr[4]._u32[1] & SPU_LS_MASK & ~3u;
    return 1;
}

static void hj_post(spu_context* ctx)
{
    memcpy(t_hj_ls, ctx->ls, SPU_LS_SIZE);
    t_hj_pend = 1;
}

/* R3_DOF_GPU_APPROX=1 ("Aim blur: PC fast", r3_dof_fast.c): job 419 is not
 * run; the first lane of a frame computes the approximation on the host from
 * the colour + depth the RSX left at 0x30030000, writes it where the job puts
 * its result, leaves the job's sync line in the state the job leaves it and
 * stores the job's label-77 value, so the RSX does not wait for the SPU.
 * Nothing is substituted until the PS3 path has been checked on this machine:
 *   learning: the job runs as today; kernel 618's calls give the frame's image
 *     size and depth column (its 0x20-byte block: w, h, depth byte column,
 *     .., base, serial; 480x320 at 960x640) and where its detile ops put the
 *     linear colour and depth copies (addresses differ between runs). On
 *     probe frames the tiled region is read at job entry and, when the job
 *     stores label 77, detiled with that geometry and compared with the
 *     job's copies (depth byte-exact, colour too unless another op of the job
 *     writes over it) and with what the job wrote (r3dof_fast_probe_verdict:
 *     A = CoC byte within 2 on >= 25% of pixels, 25-60% only on a blurring
 *     frame at >= 45 dB; composite PSNR >= R3_DOF_GPU_APPROX_MINPSNR, default
 *     20 dB, on frames that blur, and >= 35 dB on every frame when < 90% of
 *     CoC bytes match), computed with the parameter block as it was at job
 *     entry (a block rewritten while the job ran: probe not counted); the per-frame descriptor, parameter block,
 *     label value and the sync line's end state are learned and must be
 *     consistent. Arms after 3 good probes in a row (one of them blurring; zoom-ramp
 *     and CoC-settling probes in between are not counted, R3_DOF_FAST_RAMP_KEEP), or
 *     at once from the previous session's arming (R3_DOF_FAST_CACHE). A
 *     probe whose copies are not known yet does not count; 6 of them in a row,
 *     or 3 whose entry-time images differ from the job's copies, turn the path
 *     off. A failed probe resets the count and probes again
 *     R3_DOF_GPU_APPROX_RETRY frames later (default 60); after
 *     R3_DOF_GPU_APPROX_MAXFAIL failed probes (default 10, 1 = first failure)
 *     without an arming in between the path is off for the session.
 *   armed: the frame's 618 block must hold the frame's serial or the
 *     previous frame's (it is written after job entry; a stale block -> PS3
 *     path for the frame) and the probed geometry (else back to learning);
 *     every R3_DOF_GPU_APPROX_RECHECK-th frame (default 3600, 0 = never) is a probe
 *     again; a failed probe goes back to learning (PS3 path, same failure
 *     budget), an unreadable image uses the PS3 path for the frame. The tile
 *     bank comes from the tile setup (cellGcmSetTile) of the region. */
#define FAST_LINK 0x3F8F4u     /* ig kernel: return pc of the job call */
#define FAST_DESC 0x3FA10u     /* ig kernel: current job's 16-byte parameters */
#define FAST_LABEL 77u
#define FAST_DETILE_IMG 618u   /* job 419's detile / tile kernel */
#define FAST_NPX (R3DOF_FAST_W * R3DOF_FAST_H)
#define FAST_NOUT 64
extern volatile uint32_t g_semfix_base;
extern void (*volatile g_semfix_spu_hook)(uint32_t, uint32_t);
extern uint32_t (*volatile g_semfix_store_tag_hook)(uint32_t);
extern void semfix_note_spu(uint32_t, uint32_t);
extern void semfix_pre_spu(uint32_t, uint32_t);
extern void ps3_guest_mem_notify_put(uint32_t);
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
extern int spu_coh_is_reserved(uint32_t);
extern void spu_coh_notify_write_except(uint32_t, const void*);
extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
extern int cellGcm_port_tile_at(uint32_t loc, uint32_t off, uint32_t* offset, uint32_t* pitch, uint32_t* bank, uint32_t* comp);
static int s_fast, s_fast_recheck = 3600;
static float s_fast_minpsnr = 20.f;
static volatile long s_fast_state;   /* 0 learning, 1 armed, -1 off */
static SRWLOCK s_flock = SRWLOCK_INIT, s_fwork = SRWLOCK_INIT;
static uint8_t *s_pc, *s_pz, *s_ps, *s_po, *s_wc, *s_wz, *s_wo, *s_preg;
static float *s_pscr, *s_wscr;
typedef struct { uint32_t serial, ea, mode; uint8_t start[128]; int ended; int dof_off; } fast_rec;   /* dof_off: zero DOF params at entry */
#define FAST_NREC 8
static fast_rec s_frec[FAST_NREC];
static unsigned s_frec_next;
static int s_end_n;
static uint8_t s_end_c[128], s_end_unch[128], s_end_const[128];
static uint32_t s_probe_serial, s_probe_ea, s_entry_off = 0xFFFFFFFFu;
static long long s_probe_t;
static int s_probe_ok, s_probe_blur, s_label_n, s_since_check, s_preg_ok, s_inconc, s_indiff;
/* failed probes are retried (R3_DOF_GPU_APPROX_RETRY frames later) until
 * R3_DOF_GPU_APPROX_MAXFAIL of them without an arming in between */
static int s_fast_retry = 60, s_fast_maxfail = 10, s_qfail, s_pchg;
static long s_probe_after;
static uint8_t s_probe_pblk[0xC0];   /* parameter block at probe entry */
static int s_probe_pblk_ok;
static uint32_t s_label_val;
static volatile long s_kdesc_ok, s_kdesc_bad, s_kpar_ok, s_kpar_bad;
static volatile long s_fast_frames;
static volatile long long s_fast_ticks;
static long long s_fast_next_log;
static __declspec(thread) int t_fast_self;
/* learned from kernel 618 (under s_flock) */
typedef struct { uint32_t ea, pitch, in; } fast_copy;
typedef struct { uint32_t out, in; } fast_out;
static r3dof_fast_geom s_geo, s_geo_arm;   /* last 618 block seen / geometry of the last good probe */
static uint32_t s_geo_serial, s_geo_pe, s_geo_cin, s_geo_zin;   /* + where 618 reads colour / depth */
static fast_copy s_ccopy, s_zcopy;
static fast_out s_outs[FAST_NOUT];
static volatile long s_nout;
static int s_gser_ok, s_gser_bad;

static volatile uint32_t s_sw_ea;   /* sync line watched for the current probe (R3_DOF_GPU_APPROX_ROBUST), 0 = none */

/* R3_DOF_GPU (default 1, needs R3_DOF_GPU_APPROX=1): on an armed frame the host
 * no longer reads the light buffer back nor runs the approximation; it posts a
 * request (serial, geometry, CoC parameters) before its sync-line and label-77
 * writes, and the RSX computes the same output on the GPU when the composite
 * samples 0x30030000 (VKR3DofGpu.cpp). The first such frame also runs the CPU
 * path and the RSX compares both (PSNR below R3_DOF_GPU_MINPSNR, default
 * 30 dB -> off). Until that check passed the CPU result is still written to
 * guest memory, so a request the RSX does not serve shows the CPU image;
 * R3_DOF_GPU_MAXMISS (default 3) such misses -> off. After it passed the CPU
 * work is skipped and the first unserved request turns the GPU path off (that
 * one frame shows guest memory without the blur: the miss is only known on the
 * RSX after label 77 was released). Only while the Vulkan RSX texture cache
 * exists (r3_dof_gpu_set_ready); other backends keep the CPU fast path.
 * Off = the CPU fast path. */
static int s_gpu, s_gpu_maxmiss = 3, s_gmiss;
static volatile long s_gpu_ready, s_gtrusted;
static volatile uint32_t s_gcheck_serial;   /* outstanding check request, 0 = none */
static float s_gpu_minpsnr = 30.f;
static volatile long s_gpu_state;   /* 0 on, -1 off */
static SRWLOCK s_gpulock = SRWLOCK_INIT;
static uint32_t s_greq_serial, s_greq_geom[4], s_gprev;
static float s_greq_par[12];
static uint8_t* s_gref;
static volatile uint32_t s_gserved;
/* a later frame ran job 419 on the SPU (probe/recheck frame, block or parameter
 * reject, ...): its output is in guest memory, so the request is withdrawn once
 * it was served (a still pending one belongs to an earlier composite and is
 * served first): r3_dof_gpu_request then returns 2, the RSX still serves its
 * image to further binds of that composite (unchanged surfaces) but neither
 * recomputes nor reuses it once the light buffer is redrawn */
static volatile long s_gwithdrawn;
static volatile long s_gframes, s_gpost;
static volatile long long s_grec_us;
/* R3_DOF_GPU_CHECK_MAXFAIL (default 3, min 1) / R3_DOF_GPU_CHECK_RETRY (frames, default 300): the comparison
 * frame is only taken from an armed frame whose kernel 618 block is this frame's own (not a
 * R3_DOF_FAST_BLOCK_STALE neighbour) and whose DOF parameters did not ramp in the last
 * R3_DOF_GPU_APPROX_RAMP_HOLD frames; a failed check runs the CPU fast path for RETRY frames and checks again,
 * GAVE UP only after MAXFAIL failed checks. Both 2026-11-17 runs checked a stale-block frame (t1 failed at
 * 28.45 dB right after an aim zoom ramp and gave up for the session, t2 passed at 91.56 dB).
 * R3_DOF_GPU_CHECK_STALE (frames, default 60, 0 = own block only): in scenes where every armed frame uses the
 * neighbouring frame's block (the 2026-11-18 diner run: the check never came and every aim frame stayed on the
 * 8.4 ms CPU path), a stale-block frame with settled parameters becomes the comparison frame after that many
 * consecutive armed frames without an own block. The CPU and GPU paths read the same block, light buffer and
 * parameters, so the comparison stays like for like; a failed check still retries / gives up as above.
 * R3_DOF_GPU_CHECK_RAMP_HOLD (frames, default 4; was R3_DOF_GPU_APPROX_RAMP_HOLD = 30): firing ramps the DOF
 * parameters about every 7 frames, so with 30 the check never came during ADS fire (round-6 log: 256 deferrals,
 * every aim frame on the ~7 ms CPU readback path at 33-41 fps). STALE default 15 (was 60) for the same reason. */
static int s_gcheck_maxfail = 3, s_gcheck_retry = 300, s_gcheck_fail, s_gcheck_stale = 15, s_gcheck_ramp_hold = 4;
static long s_gstale_run;
static volatile long s_gcheck_after = -0x40000000L, s_gcheck_deferred;
static long s_gramp_frame = -0x40000000L;

static void gpu_clear(void)
{
    if (!s_gpu) return;
    AcquireSRWLockExclusive(&s_gpulock);
    s_greq_serial = 0;
    ReleaseSRWLockExclusive(&s_gpulock);
}

static void gpu_off(const char* why)
{
    if (InterlockedCompareExchange(&s_gpu_state, -1, 0) != 0) return;
    gpu_clear();
    fprintf(stderr, "[dof-gpu] GAVE UP: %s -> CPU fast path (host approximation) for the session%c", why, 10);
    fflush(stderr);
}

/* RSX side (VKR3DofGpu.cpp): the Vulkan texture cache exists */
void r3_dof_gpu_set_ready(int ready)
{
    InterlockedExchange(&s_gpu_ready, ready ? 1 : 0);
}

/* RSX side: 1 = the CPU result is no longer written (a failed serve has no guest fallback) */
int r3_dof_gpu_trusted(void)
{
    return s_gtrusted != 0;
}

/* RSX side (VKR3DofGpu.cpp): the current request, 0 = none, 1 = active, 2 = served and withdrawn */
int r3_dof_gpu_request(uint32_t* serial, uint32_t* geom, float* par, const uint8_t** ref)
{
    if (!s_gpu || s_gpu_state < 0) return 0;
    AcquireSRWLockShared(&s_gpulock);
    const int ok = s_greq_serial == 0 ? 0 : s_gwithdrawn && s_gserved == s_greq_serial ? 2 : 1;
    if (ok) {
        *serial = s_greq_serial;
        memcpy(geom, s_greq_geom, sizeof s_greq_geom);
        memcpy(par, s_greq_par, sizeof s_greq_par);
        *ref = s_greq_geom[3] ? s_gref : NULL;
    }
    ReleaseSRWLockShared(&s_gpulock);
    return ok;
}

/* status 1 served (a = RSX record ms), 2 check result (a = PSNR dB, b = % equal CoC bytes), < 0 not served */
void r3_dof_gpu_result(uint32_t serial, int status, double a, double b)
{
    if (status == 1) {
        s_gserved = serial;
        InterlockedIncrement(&s_gframes);
        InterlockedAdd64(&s_grec_us, (long long)(a * 1000.0));
    } else if (status < 0) {
        static const char* const k[] = { "?", "no light buffer surface", "surface format/pitch/samples", "geometry", "texture dimensions" };
        char why[128];
        snprintf(why, sizeof why, "the RSX could not serve serial 0x%X (%s)", serial, k[-status < 5 ? -status : 0]);
        gpu_off(why);
    } else if (status == 2) {
        if (!(a >= s_gpu_minpsnr)) {
            char why[160];
            snprintf(why, sizeof why, "check frame serial 0x%X: GPU output differs from the CPU path (PSNR %.2f dB < %.0f, CoC bytes %.1f%% equal)",
                     serial, a, s_gpu_minpsnr, b);
            if (++s_gcheck_fail >= s_gcheck_maxfail) gpu_off(why);
            else {
                s_gcheck_after = g_port_frame_no + s_gcheck_retry;
                fprintf(stderr, "[dof-gpu] %s -> CPU fast path, checking again in %d frames (failed check %d of %d)%c",
                        why, s_gcheck_retry, s_gcheck_fail, s_gcheck_maxfail, 10);
                fflush(stderr);
            }
        } else {
            fprintf(stderr, "[dof-gpu] check frame serial 0x%X passed: PSNR %.2f dB vs the CPU path, CoC bytes %.1f%% equal -> CPU work skipped from now on%c", serial, a, b, 10);
            fflush(stderr);
            s_gtrusted = 1;
        }
        s_gcheck_serial = 0;
    } else if (status == 3) {
        fprintf(stderr, "[dof-gpu] check frame serial 0x%X: the RSX could not compare (texture smaller than the image) -> trusted without the comparison%c", serial, 10);
        fflush(stderr);
        s_gtrusted = 1;
        s_gcheck_serial = 0;
    }
}

/* armed frame on the GPU path: miss accounting for the previous request, then this one */
static int gpu_post(uint32_t serial, const r3dof_fast_geom* g, const r3dof_fast_params* par, int check)
{
    AcquireSRWLockExclusive(&s_gpulock);
    int off = 0, after = 0;
    if (s_gprev && s_gserved != s_gprev) {
        if (s_gtrusted) off = after = 1;
        else {
            off = ++s_gmiss >= s_gpu_maxmiss;
            if (s_gcheck_serial == s_gprev) s_gcheck_serial = 0;   /* check request missed: the next frame checks */
        }
    }
    if (!off) {
        s_greq_serial = serial;
        s_gwithdrawn = 0;
        s_greq_geom[0] = g->w; s_greq_geom[1] = g->h; s_greq_geom[2] = g->zx; s_greq_geom[3] = (uint32_t)check;
        memcpy(s_greq_par, par, sizeof s_greq_par);
        s_gprev = serial;
    }
    ReleaseSRWLockExclusive(&s_gpulock);
    if (off) {
        char why[200];
        if (after)
            snprintf(why, sizeof why, "the RSX did not serve request 0x%X after %ld served frames (that frame showed the light buffer without the blur)", s_gprev, s_gframes);
        else
            snprintf(why, sizeof why, "the RSX did not serve %d requests while the CPU result was still written (last serial 0x%X)", s_gmiss, s_gprev);
        gpu_off(why);
        return 0;
    }
    static int s_logged;
    if (!s_logged++) {
        fprintf(stderr, "[dof-gpu] R3_DOF_GPU=1: first aim-blur frame handed to the GPU (serial 0x%X, %ux%u, depth col %u); the CPU result is still written until the one-time comparison passed%c",
                serial, g->w, g->h, g->zx, 10);
        fflush(stderr);
    }
    return 1;
}

static void fast_cache_drop(const char* why);

static void fast_off(const char* why)
{
    if (s_fast_state < 0) return;
    s_fast_state = -1;
    s_sw_ea = 0;
    gpu_clear();
    fast_cache_drop(why);
    fprintf(stderr, "[dof-fast] GAVE UP: %s -> PS3 exact aim blur (job 419) for the session%c", why, 10);
    fflush(stderr);
}

static int fast_params(uint32_t line_ea, r3dof_fast_params* p)
{
    const uint8_t* b = hj_mem(line_ea + 0x90u, 0xC0u);
    return b && r3dof_fast_params_from_block(b, p);
}

/* bank of the tile region holding R3DOF_FAST_EA (main-memory tile, IO offset
 * 0x30000, pitch 8192 as the tiling formula assumes; R3 sets it as tile 4);
 * 2 (R3's value) when it is not found */
static uint32_t fast_tile_bank(void)
{
    static volatile long s_logged = -1;
    uint32_t off = 0, pitch = 0, bank = 0;
    const int ok = cellGcm_port_tile_at(1u, R3DOF_FAST_IO_OFF, &off, &pitch, &bank, NULL) &&
                   off == R3DOF_FAST_IO_OFF && pitch == R3DOF_FAST_PITCH && bank < 4u;
    const long key = ok ? (long)bank : 4;
    if (InterlockedExchange(&s_logged, key) != key) {
        if (ok) fprintf(stderr, "[dof-fast] tile region at main-memory offset 0x%X: pitch %u, bank %u%c", off, pitch, bank, 10);
        else fprintf(stderr, "[dof-fast] no bound main-memory tile region at offset 0x%X with pitch %u (found offset 0x%X pitch %u) -> bank 2%c",
                     R3DOF_FAST_IO_OFF, R3DOF_FAST_PITCH, off, pitch, 10);
        fflush(stderr);
    }
    return ok ? bank : 2u;
}

/* kernel 618's per-frame block -> geometry; 1 = sane and at R3DOF_FAST_EA */
static int fast_geom_block(uint32_t pe, r3dof_fast_geom* g, uint32_t* serial)
{
    const uint8_t* b = hj_mem(pe, 0x20u);
    if (!b || dof_be32(b + 0x14) != R3DOF_FAST_EA) return 0;
    g->w = dof_be32(b); g->h = dof_be32(b + 4); g->zx = dof_be32(b + 8); g->bank = fast_tile_bank();
    *serial = dof_be32(b + 0x18);
    return 1;
}

/* R3_DOF_GPU_APPROX_ROBUST (default 1; 0 = the 2026-11-15 rules): which bytes
 * of the sync line job 419 itself changes is observed on probe frames
 * (g_spu_write_watch, called before an SPU PUT / list PUT / PUTLLC commit /
 * PUTLLUC lands, with the old bytes in memory and the new ones in the source:
 * only bytes inside the write that it changes count, and only for an SPU
 * thread inside the job, i.e. between the job call and its return to
 * FAST_LINK: g_spu_igjob_return_hook from spu_drain_call, or the indirect
 * branch to the link), instead of comparing the whole line two frames later.
 * That old rule turned PC fast off for the session in every run since
 * 2026-11-15 ("sync line byte 0: end state not learnable"): by then the line
 * has been rewritten by others (the PPU / the job manager re-use it), and
 * those writes happen with the job skipped too. Learned per aligned big-endian
 * 32-bit word the job changes: either every changed byte ends at the same
 * value, or the word moves by the same amount (mod 2^32, so a counter's carry
 * is kept). Armed frames apply exactly that to the current line under the line
 * lock, committed like a PUTLLUC. A probe whose job word fits neither rule is
 * logged with the values and not counted; a probe run in which the job changed
 * no byte of the line counts as a failed probe (backoff, GAVE UP after
 * R3_DOF_GPU_APPROX_MAXFAIL). A probe that fails while the DOF parameters ramp
 * (aim zoom in/out: CoC parameters differ by > 2% from the previous frame's)
 * is not counted, the next frame probes again. */
static int s_robust = 1;
#define FAST_SL_NS 4
typedef struct { uint8_t st[128], ev[128], jm[128]; } fast_sl_sample;
static SRWLOCK s_swlock = SRWLOCK_INIT;
static uint32_t s_sw_serial;
static int s_sw_labeled;
static uint8_t s_sw_start[128], s_sw_jval[128], s_sw_jmask[128], s_sw_omask[128];
static __declspec(thread) uint32_t t_in_job;   /* serial of the job this SPU thread is running, 0 = none */
static fast_sl_sample s_sl[FAST_SL_NS];
static int s_sl_n, s_sl_bad, s_sl_nobs;
/* per 32-bit word: 0 = untouched, 1 = constant bytes (s_sl_bmask / s_sl_cval), 2 = constant word delta */
static uint8_t s_sl_wkind[32], s_sl_bmask[128], s_sl_cval[128];
static uint32_t s_sl_wdelta[32];
static uint8_t s_arm_wkind[32], s_arm_bmask[128], s_arm_cval[128];   /* copy used by armed frames */
static uint32_t s_arm_wdelta[32];
static uint8_t s_last_pblk[0xC0];
static int s_last_pblk_ok, s_probe_ramp, s_ramp_skips;
/* R3_DOF_GPU_APPROX_RAMP_HOLD (default 30 frames, 0 = off): the job's CoC keeps
 * settling for a few frames after the parameter block stopped ramping (2026-11-17
 * 09:45 log: 4 counted FAILs at 36-56% CoC match each 1 frame after a ramp-skipped
 * probe; 60 PS3-path frames each, ~10 s to arm). A non-good probe within this many
 * frames of a ramp is not counted either (probing again next frame). */
static int s_ramp_hold = 30, s_ramp_settle;
/* R3_DOF_FAST_RAMP_KEEP (default 1, 0 = the old reset): a CoC-settling probe
 * (not counted, see above) no longer throws away the good probes before it; like
 * a zoom-ramp probe it is neither good nor bad, so the arming needs 3 good probes
 * with only not-counted ones in between. 2026-11-19 T1 log: 2 good probes at the
 * start of the first ADS, then ~45 ramp/settle probes each on the PS3 path. */
static int s_ramp_keep = 1;
static long s_ramp_frame = -0x40000000L;

static uint32_t fast_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* R3_DOF_FAST_CACHE (default 1, 0 = off: every session probes on the PS3 path
 * first, the pre-2026-11-19 behaviour): what an arming learned and what only
 * depends on the game's code and the guest resolution -- job 419's entry
 * offset, the image geometry (size, depth column, tile bank), kernel 618's
 * block address, the label-77 value and the sync-line rule (which words the
 * job changes and how) -- is saved to r3_dof_fast.cache next to the exe. The
 * next session arms from it at the first job-419 frame whose kernel 618 block
 * (cached address, or the one learned on a PS3-path frame of this session)
 * already shows exactly the cached geometry, so the first ADS skips the probe
 * phase (2026-11-19 log: ~50 PS3-path frames, 31-63 ms hitches, ~1.5 s at
 * ~25 fps). Everything an armed frame checks still applies (fresh block with
 * the probed geometry, parameter block, RSX gate, black hold, first-use
 * guard), the regular R3_DOF_GPU_APPROX_RECHECK probes still run, and while
 * armed from the cache any probe's sync-line sample must fit the cached rule
 * and its label the cached value: a mismatch or a failed probe drops the cache
 * (file deleted) and learns again on the PS3 path. Robust mode only.
 * The header carries a build stamp (the exe's size and write time, plus this
 * file's compile time): a cache written by another build is ignored, since
 * job 419's entry offset and the kernel 618 block can move between builds. */
static int s_cache = 1, s_cache_state;   /* state: 0 none, 1 loaded (unused), 2 used or given up */
static int s_cache_tries, s_arm_from_cache;
static char s_cache_path[MAX_PATH];
typedef struct {
    char magic[8];
    uint32_t size, build_lo, build_hi, entry_off, w, h, zx, bank, pe, label;
    uint8_t wkind[32], bmask[128], cval[128];
    uint32_t wdelta[32];
    uint32_t sum;
} fast_cache;
static fast_cache s_cfile;
static uint32_t s_build_lo, s_build_hi;
#define FAST_CACHE_MAGIC "R3DOFC02"
#define FAST_CACHE_TRIES 8
/* first-ADS summary: PS3-path job-419 frames before the session's first PC fast frame */
static long long s_ads_t0;
static long s_ads_f0, s_ads_ps3, s_ads_probes;
static int s_ads_logged;

static uint32_t fast_cache_sum(const fast_cache* c)
{
    const uint8_t* p = (const uint8_t*)c;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(fast_cache, sum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int fast_cache_valid(const fast_cache* c)
{
    const r3dof_fast_geom g = { c->w, c->h, c->zx, c->bank };
    if (memcmp(c->magic, FAST_CACHE_MAGIC, 8) || c->size != sizeof *c || c->sum != fast_cache_sum(c) ||
        c->build_lo != s_build_lo || c->build_hi != s_build_hi ||
        !r3dof_fast_geom_ok(&g) || !c->pe || (c->pe & 3u) || c->entry_off >= SPU_LS_SIZE)
        return 0;
    int nchg = 0;
    for (int w = 0; w < 32; w++) {
        if (c->wkind[w] > 2) return 0;
        for (int k = 0; k < 4; k++) {
            if (c->bmask[w * 4 + k] > 1 || (c->bmask[w * 4 + k] && !c->wkind[w])) return 0;
            nchg += c->bmask[w * 4 + k];
        }
    }
    return nchg > 0;
}

static void fast_cache_init(void)
{
    s_cache = port_native_parse_bool(getenv("R3_DOF_FAST_CACHE"), 1);
    if (!s_cache || !s_robust) {
        if (s_cache) fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE needs R3_DOF_GPU_APPROX_ROBUST=1: off%c", 10);
        s_cache = 0;
        return;
    }
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = n && n < MAX_PATH ? strrchr(exe, '\\') : NULL;
    {
        /* build stamp: FNV-1a over the exe's size + last-write time and this file's __DATE__ __TIME__ */
        uint64_t h = 1469598103934665603ull;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (n && n < MAX_PATH && GetFileAttributesExA(exe, GetFileExInfoStandard, &fa)) {
            const uint32_t v[4] = { fa.nFileSizeLow, fa.nFileSizeHigh, fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime };
            for (size_t i = 0; i < sizeof v; i++) h = (h ^ ((const uint8_t*)v)[i]) * 1099511628211ull;
        }
        for (const char* s = __DATE__ " " __TIME__; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
        s_build_lo = (uint32_t)h; s_build_hi = (uint32_t)(h >> 32);
    }
    if (slash) { *slash = 0; snprintf(s_cache_path, sizeof s_cache_path, "%s\\r3_dof_fast.cache", exe); }
    else snprintf(s_cache_path, sizeof s_cache_path, "r3_dof_fast.cache");
    FILE* f = fopen(s_cache_path, "rb");
    if (!f) {
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE=1: no %s yet -> the first ADS probes job 419 on the PS3 path, the arming is saved for the next session%c", s_cache_path, 10);
        return;
    }
    const size_t rd = fread(&s_cfile, 1, sizeof s_cfile, f);
    fclose(f);
    if (rd != sizeof s_cfile || !fast_cache_valid(&s_cfile)) {
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE=1: %s unreadable or from another build -> ignored (rewritten at the next arming)%c", s_cache_path, 10);
        return;
    }
    s_cache_state = 1;
    fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE=1: %s loaded (image %ux%u depth col %u bank %u, kernel 618 block 0x%08X, label 77 = 0x%08X) -> the first ADS arms from it once kernel 618's block shows that geometry%c",
            s_cache_path, s_cfile.w, s_cfile.h, s_cfile.zx, s_cfile.bank, s_cfile.pe, s_cfile.label, 10);
}

static void fast_cache_save(void)
{
    if (!s_cache || !s_robust || !s_geo_pe || s_entry_off == 0xFFFFFFFFu) return;
    fast_cache c;
    memset(&c, 0, sizeof c);
    memcpy(c.magic, FAST_CACHE_MAGIC, 8);
    c.size = sizeof c; c.build_lo = s_build_lo; c.build_hi = s_build_hi; c.entry_off = s_entry_off;
    c.w = s_geo_arm.w; c.h = s_geo_arm.h; c.zx = s_geo_arm.zx; c.bank = s_geo_arm.bank;
    c.pe = s_geo_pe; c.label = s_label_val;
    memcpy(c.wkind, s_arm_wkind, 32); memcpy(c.bmask, s_arm_bmask, 128);
    memcpy(c.cval, s_arm_cval, 128); memcpy(c.wdelta, s_arm_wdelta, sizeof c.wdelta);
    for (int k = 0; k < 128; k++) c.bmask[k] = c.bmask[k] ? 1 : 0;
    c.sum = fast_cache_sum(&c);
    if (!fast_cache_valid(&c)) return;
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_cache_path);
    FILE* f = fopen(tmp, "wb");
    int ok = f && fwrite(&c, 1, sizeof c, f) == sizeof c;
    if (f) ok &= fclose(f) == 0;
    ok = ok && MoveFileExA(tmp, s_cache_path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileA(tmp);
    fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: arming %s %s (next session arms from it)%c", ok ? "saved to" : "NOT saved to", s_cache_path, 10);
}

static void fast_cache_drop(const char* why)
{
    if (!s_cache || !s_cache_path[0]) return;
    const int was_armed = s_arm_from_cache;
    s_arm_from_cache = 0;
    if (s_cache_state == 1) s_cache_state = 2;
    if (DeleteFileA(s_cache_path))
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: %s deleted (%s%s)%c", s_cache_path, why, was_armed ? ", was armed from it" : "", 10);
}

/* armed from the cache: a probe's sync-line sample must fit the cached rule */
static int fast_rule_fits_r(const fast_sl_sample* s, const uint8_t* wkind, const uint8_t* bmask, const uint8_t* cval, const uint32_t* wdelta)
{
    for (int w = 0; w < 32; w++) {
        const uint8_t kind = wkind[w];
        if (kind == 2) {
            /* counter word: judged on its step only (a carry changes more bytes than the learning samples did) */
            if (fast_be32(s->ev + w * 4) - fast_be32(s->st + w * 4) != wdelta[w]) return 0;
            continue;
        }
        for (int k = w * 4; k < w * 4 + 4; k++) {
            if (s->jm[k] && !bmask[k]) return 0;
            if (kind == 1 && bmask[k] && s->ev[k] != cval[k]) return 0;
        }
    }
    return 1;
}
static int fast_rule_fits(const fast_sl_sample* s) { return fast_rule_fits_r(s, s_arm_wkind, s_arm_bmask, s_arm_cval, s_arm_wdelta); }

/* learning, first lane of a job-419 frame (under s_flock): arm from the cache
 * when kernel 618's block (cached address first, then the one this session's
 * PS3-path frames learned) shows exactly the cached geometry */
static void fast_cache_try_arm(uint32_t serial, uint32_t ea)
{
    if (s_cache_state != 1 || s_fast_state != 0) return;
    const fast_cache* c = &s_cfile;
    if (s_entry_off != c->entry_off) {
        s_cache_state = 2;
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: job 419 entry offset 0x%X, cached 0x%X -> cache not used, probing%c", s_entry_off, c->entry_off, 10);
        return;
    }
    /* what this session's PS3-path frames already saw must agree with the cache */
    if (s_qfail || s_indiff || s_sl_bad) {
        s_cache_state = 2;
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: a probe of this session failed (%d failed, %d input differences, %d unlearnable sync lines) -> cache not used this session, probing%c", s_qfail, s_indiff, s_sl_bad, 10);
        return;
    }
    if (s_probe_serial) {
        /* wait for the verdict of the probe still running instead of dropping it */
        if (++s_cache_tries >= FAST_CACHE_TRIES) {
            s_cache_state = 2;
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: a probe was still pending in %d job 419 frames -> probing%c", s_cache_tries, 10);
        }
        return;
    }
    int sl_fit = 1;
    for (int i = 0; i < s_sl_n && sl_fit; i++) sl_fit = fast_rule_fits_r(&s_sl[i], c->wkind, c->bmask, c->cval, c->wdelta);
    if ((s_label_n && s_label_val != c->label) || !sl_fit) {
        s_cache_state = 2;
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: this session's probes saw %s (cached label 77 = 0x%08X) -> cache not used, probing%c",
                !sl_fit ? "another sync line change" : "another label 77 value", c->label, 10);
        fast_cache_drop(!sl_fit ? "sync line rule differs" : "label 77 value differs");
        return;
    }
    const r3dof_fast_geom want = { c->w, c->h, c->zx, c->bank };
    r3dof_fast_geom g;
    uint32_t bs = 0, pe = 0;
    r3dof_fast_params par;
    const uint32_t cand[2] = { c->pe, s_geo_pe };
    for (int i = 0; i < 2 && !pe; i++)
        if (cand[i] && fast_geom_block(cand[i], &g, &bs) && !memcmp(&g, &want, sizeof g) && r3dof_fast_geom_ok(&g)) pe = cand[i];
    if (!pe || !fast_params(ea, &par) || !g_semfix_base || !hj_mem(R3DOF_FAST_EA, R3DOF_FAST_REGION)) {
        if (++s_cache_tries >= FAST_CACHE_TRIES) {
            s_cache_state = 2;
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: no kernel 618 block with the cached geometry %ux%u depth col %u bank %u in %d job 419 frames (resolution changed?) -> probing%c",
                    c->w, c->h, c->zx, c->bank, s_cache_tries, 10);
        } else if (s_cache_tries == 1)
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: serial 0x%X: kernel 618 block not (yet) showing the cached geometry -> PS3 path for this frame, retried next frame%c", serial, 10);
        return;
    }
    s_cache_state = 2;
    s_geo_arm = g;
    if (memcmp(&g, &s_geo, sizeof g)) {
        /* as fast_kernel_note: the copy sources go with the geometry (the copies themselves are learned on the next probe) */
        s_geo_cin = R3DOF_FAST_EA + r3dof_fast_min_off(&g, 0);
        s_geo_zin = R3DOF_FAST_EA + r3dof_fast_min_off(&g, g.zx);
        s_geo = g; s_geo_serial = bs;
    }
    s_geo_pe = pe;
    s_label_val = c->label;
    if (!s_label_n) s_label_n = 1;
    memcpy(s_arm_wkind, c->wkind, 32); memcpy(s_arm_bmask, c->bmask, 128);
    memcpy(s_arm_cval, c->cval, 128); memcpy(s_arm_wdelta, c->wdelta, sizeof s_arm_wdelta);
    MemoryBarrier();
    s_arm_from_cache = 1;
    s_fast_state = 1;
    s_since_check = 0;
    int nchg = 0;
    for (int k = 0; k < 128; k++) nchg += c->bmask[k];
    fprintf(stderr, "[dof-fast] ARMED from R3_DOF_FAST_CACHE (PC fast aim blur on, no PS3-path probe frames) at serial 0x%X frame %ld: host approximation %ux%u (depth column %u, bank %u), kernel 618 block 0x%08X (serial 0x%X), label %u = 0x%08X, sync line: job changes %d bytes; regular rechecks every %d frames%c",
            serial, (long)g_port_frame_no, g.w, g.h, g.zx, g.bank, pe, bs, FAST_LABEL, s_label_val, nchg, s_fast_recheck, 10);
    fflush(stderr);
}

static void fast_try_arm(void)
{
    const int sync_ok = s_robust ? s_sl_n >= 3 : s_end_n >= 3;
    if (s_fast_state != 0 || s_probe_ok < 3 || !s_probe_blur || !sync_ok || s_label_n < 3 ||
        s_kdesc_bad || s_kpar_bad || !s_kdesc_ok || !s_kpar_ok || !s_geo_arm.w || !s_gser_ok)
        return;
    int nchg = 0, nconst = 0, ndelta = 0;
    if (s_robust)
        for (int w = 0; w < 32; w++) {
            nconst += s_sl_wkind[w] == 1;
            ndelta += s_sl_wkind[w] == 2;
            for (int k = 0; k < 4; k++) nchg += s_sl_wkind[w] && s_sl_bmask[w * 4 + k];
        }
    else
        for (int i = 0; i < 128; i++) nchg += !s_end_unch[i];
    if (s_robust && !nchg) {
        /* job 419 never changed its sync line itself: without its completion
         * write a skipped job could not be stood in for -- treat as a failed probe */
        s_probe_ok = 0; s_probe_blur = 0; s_sl_n = 0;
        if (++s_qfail >= s_fast_maxfail) {
            char b[128];
            snprintf(b, sizeof b, "job 419 changed no byte of its sync line in %d probe runs", s_qfail);
            fast_off(b);
        } else {
            s_probe_after = g_port_frame_no + s_fast_retry;
            fprintf(stderr, "[dof-fast] not arming: probes good but job 419 changed no sync line byte (%d write events seen; %d of %d) -> probing again in %d frames%c",
                    s_sl_nobs, s_qfail, s_fast_maxfail, s_fast_retry, 10);
            fflush(stderr);
        }
        return;
    }
    if (s_robust) {
        memcpy(s_arm_wkind, s_sl_wkind, sizeof s_arm_wkind); memcpy(s_arm_bmask, s_sl_bmask, 128);
        memcpy(s_arm_cval, s_sl_cval, 128); memcpy(s_arm_wdelta, s_sl_wdelta, sizeof s_arm_wdelta);
        MemoryBarrier();
    }
    s_fast_state = 1;
    s_qfail = 0;
    s_arm_from_cache = 0;
    if (s_cache_state == 1) s_cache_state = 2;
    if (s_robust)
        fprintf(stderr, "[dof-fast] ARMED (PC fast aim blur on) after %d good probe frames (%d blurring): job 419 skipped, host approximation %ux%u (depth column %u, bank %u) + label %u = 0x%08X, sync line: job changes %d bytes (%d constant words, %d counter words), %d write events seen; descriptor checks %ld, parameter checks %ld%c",
                s_probe_ok, s_probe_blur, s_geo_arm.w, s_geo_arm.h, s_geo_arm.zx, s_geo_arm.bank, FAST_LABEL, s_label_val, nchg, nconst, ndelta,
                s_sl_nobs, s_kdesc_ok, s_kpar_ok, 10);
    else
        fprintf(stderr, "[dof-fast] armed after %d good probe frames (%d blurring): job 419 skipped, host approximation %ux%u (depth column %u, bank %u) + label %u = 0x%08X, sync line end state %d bytes; descriptor checks %ld, parameter checks %ld%c",
                s_probe_ok, s_probe_blur, s_geo_arm.w, s_geo_arm.h, s_geo_arm.zx, s_geo_arm.bank, FAST_LABEL, s_label_val, nchg,
                s_kdesc_ok, s_kpar_ok, 10);
    fflush(stderr);
    if (s_robust) fast_cache_save();
}

/* g_spu_write_watch: an SPU is about to write [ea, ea+size) (memory still old, src new) */
static void fast_write_watch(spu_context* ctx, uint32_t ea, uint32_t size, const uint8_t* src)
{
    const uint32_t line = s_sw_ea;
    if (!line || ea >= line + 128u || ea + size <= line) return;
    extern uint8_t* vm_base;
    (void)ctx;
    const uint8_t* m = vm_base + line;   /* readable: hj_mem-checked when the watch started */
    const uint32_t k0 = ea > line ? ea - line : 0, k1 = ea + size < line + 128u ? ea + size - line : 128u;
    const int job = t_in_job && t_in_job == s_sw_serial;
    AcquireSRWLockExclusive(&s_swlock);
    if (s_sw_ea == line) {
        s_sl_nobs++;
        for (uint32_t k = k0; k < k1; k++) {
            const uint8_t v = src[line + k - ea];
            if (v == m[k]) continue;
            if (job) { s_sw_jmask[k] = 1; s_sw_jval[k] = v; }
            else s_sw_omask[k] = 1;
        }
    }
    ReleaseSRWLockExclusive(&s_swlock);
}

/* g_spu_igjob_return_hook: a drained ig job returned to 0x3F8F4 */
static void fast_job_return(spu_context* ctx) { (void)ctx; t_in_job = 0; }

/* probe frame (under s_flock): watch the job's line */
static void fast_sync_begin(uint32_t serial, uint32_t ea)
{
    extern uint8_t* vm_base;
    AcquireSRWLockExclusive(&s_swlock);
    memcpy(s_sw_start, vm_base + ea, 128);
    memset(s_sw_jmask, 0, 128); memset(s_sw_omask, 0, 128);
    s_sw_serial = serial; s_sw_labeled = 0;
    MemoryBarrier();
    s_sw_ea = ea;
    ReleaseSRWLockExclusive(&s_swlock);
}

/* under s_flock, at the first lane of a later serial: the watched probe's job
 * is done; learn which line words it changes and how */
static void fast_sync_finish(uint32_t serial)
{
    if (!s_sw_ea || serial == s_sw_serial) return;
    fast_sl_sample smp;
    int labeled, nother = 0;
    AcquireSRWLockExclusive(&s_swlock);
    const uint32_t line = s_sw_ea;
    s_sw_ea = 0;
    labeled = s_sw_labeled;
    for (int k = 0; k < 128; k++) {
        smp.st[k] = s_sw_start[k];
        smp.jm[k] = s_sw_jmask[k];
        smp.ev[k] = s_sw_jmask[k] ? s_sw_jval[k] : s_sw_start[k];
        nother += s_sw_omask[k] && !s_sw_jmask[k];
    }
    ReleaseSRWLockExclusive(&s_swlock);
    if (!labeled) return;   /* the job's label-77 store was not seen: not a complete sample */
    if (s_arm_from_cache && s_fast_state == 1 && !fast_rule_fits(&smp)) {
        s_fast_state = 0; s_probe_ok = 0; s_probe_blur = 0; s_label_n = 0;
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: probe serial 0x%X: job 419's sync line change does not fit the cached rule -> PS3 exact aim blur, learning again%c", s_sw_serial, 10);
        fast_cache_drop("sync line rule differs");
    }
    if (s_sl_n == FAST_SL_NS) { memmove(s_sl, s_sl + 1, sizeof s_sl[0] * (FAST_SL_NS - 1)); s_sl_n--; }
    s_sl[s_sl_n++] = smp;
    int bad = -1;
    for (int w = 0; w < 32 && bad < 0; w++) {
        int any = 0, isc = 1, isd = 1;
        const uint32_t d0 = fast_be32(s_sl[0].ev + w * 4) - fast_be32(s_sl[0].st + w * 4);
        for (int k = w * 4; k < w * 4 + 4; k++) {
            s_sl_bmask[k] = 0;
            for (int i = 0; i < s_sl_n; i++) s_sl_bmask[k] |= s_sl[i].jm[k];
            any |= s_sl_bmask[k];
            s_sl_cval[k] = s_sl[0].ev[k];
            if (s_sl_bmask[k])
                for (int i = 0; i < s_sl_n; i++) isc &= s_sl[i].ev[k] == s_sl[0].ev[k];
        }
        for (int i = 0; i < s_sl_n; i++) isd &= fast_be32(s_sl[i].ev + w * 4) - fast_be32(s_sl[i].st + w * 4) == d0;
        s_sl_wkind[w] = !any ? 0 : isc ? 1 : isd ? 2 : 0;
        s_sl_wdelta[w] = d0;
        if (any && !isc && !isd) bad = w;
    }
    static int nlog;
    if (bad >= 0) {
        char vals[128]; int o = 0;
        for (int i = 0; i < s_sl_n && o < (int)sizeof vals - 24; i++)
            o += snprintf(vals + o, sizeof vals - o, " %08X->%08X", fast_be32(s_sl[i].st + bad * 4), fast_be32(s_sl[i].ev + bad * 4));
        fprintf(stderr, "[dof-fast] sync line 0x%08X word +0x%X: job 419 changes it with neither constant bytes nor a constant step (entry->end:%s) -> samples dropped%c",
                line, bad * 4, vals, 10);
        s_sl_n = 0;
        memset(s_sl_wkind, 0, sizeof s_sl_wkind);
        if (s_fast_state == 1) {
            s_fast_state = 0; s_probe_ok = 0; s_probe_blur = 0;
            fprintf(stderr, "[dof-fast] recheck: sync line rule broke -> PS3 exact aim blur, learning again%c", 10);
        }
        if (++s_sl_bad >= s_fast_maxfail) {
            char b[96];
            snprintf(b, sizeof b, "sync line word +0x%X not learnable in %d probes", bad * 4, s_sl_bad);
            fast_off(b);
        }
    } else if (nlog++ < 4) {
        int nj = 0;
        for (int k = 0; k < 128; k++) nj += s_sl_bmask[k];
        fprintf(stderr, "[dof-fast] sync line 0x%08X probe serial 0x%X: job changes %d bytes, %d other bytes changed by other SPU code (left alone), %d consistent samples%c",
                line, s_sw_serial, nj, nother, s_sl_n, 10);
    }
    fflush(stderr);
    fast_try_arm();
}

/* armed frame: apply the job's learned change to the current line, committed
 * like a PUTLLUC (spu_channels.c) */
static void fast_sync_put(uint32_t ea)
{
    extern uint8_t* vm_base;
    extern void spu_coh_notify_write(uint32_t);
    extern void spu_taskset_line_committed(uint32_t);
    extern void (*g_spu_line_commit_hook)(uint32_t);
    extern void ps3_guest_mem_notify(uint32_t);
    uint8_t* m = vm_base + ea;
    int any = 0;
    spu_lockline_lock();
    for (int w = 0; w < 32; w++) {
        uint8_t* p = m + w * 4;
        if (s_arm_wkind[w] == 1) {
            for (int k = 0; k < 4; k++)
                if (s_arm_bmask[w * 4 + k] && p[k] != s_arm_cval[w * 4 + k]) { p[k] = s_arm_cval[w * 4 + k]; any = 1; }
        } else if (s_arm_wkind[w] == 2 && s_arm_wdelta[w]) {
            const uint32_t v = fast_be32(p) + s_arm_wdelta[w];
            p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
            any = 1;
        }
    }
    if (any) spu_coh_notify_write(ea);
    spu_lockline_unlock();
    if (!any) return;
    spu_taskset_line_committed(ea);
    if (g_spu_line_commit_hook) g_spu_line_commit_hook(ea);
    if (g_spu_put_hook) g_spu_put_hook(ea, 128);
    ps3_guest_mem_notify(ea);
}

/* CoC / linearisation parameters differ by > 2% from the previous frame's (aim zoom ramp) */
static int fast_params_ramp(const uint8_t* prev, const uint8_t* cur)
{
    r3dof_fast_params p, q;
    if (!r3dof_fast_params_from_block(prev, &p) || !r3dof_fast_params_from_block(cur, &q)) return 1;
    const float a[] = { p.lin_split, p.lin_a, p.lin_b, p.lin_c, p.lin_d, p.near_a, p.near_b, p.near_max, p.split, p.far_a, p.far_b, p.far_max };
    const float b[] = { q.lin_split, q.lin_a, q.lin_b, q.lin_c, q.lin_d, q.near_a, q.near_b, q.near_max, q.split, q.far_a, q.far_b, q.far_max };
    for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) {
        const float m = fabsf(a[i]) > fabsf(b[i]) ? fabsf(a[i]) : fabsf(b[i]);
        if (fabsf(a[i] - b[i]) > 0.02f * m + 1e-12f) return 1;
    }
    return 0;
}

/* the sync line of an earlier frame is finished once a frame two serials later starts */
static void fast_learn_end(uint32_t serial, uint32_t ea)
{
    for (int i = 0; i < FAST_NREC; i++) {
        fast_rec* r = &s_frec[i];
        if (!r->serial || r->ended || r->mode != 1 || r->ea == ea || serial - r->serial < 2u || serial - r->serial > 8u) continue;
        r->ended = 1;
        const uint8_t* m = hj_mem(r->ea, 128);
        if (!m) continue;
        for (int k = 0; k < 128; k++) {
            if (!s_end_n) { s_end_c[k] = m[k]; s_end_unch[k] = m[k] == r->start[k]; s_end_const[k] = 1; continue; }
            s_end_unch[k] &= m[k] == r->start[k];
            s_end_const[k] &= m[k] == s_end_c[k];
        }
        s_end_n++;
        for (int k = 0; k < 128; k++)
            if (!s_end_unch[k] && !s_end_const[k]) {
                char b[96];
                snprintf(b, sizeof b, "sync line 0x%08X byte %d: end state not learnable", r->ea, k);
                fast_off(b);
                return;
            }
    }
    fast_try_arm();
}

/* armed frame (under s_flock): kernel 618's block must be fresh, holding this
 * frame's serial or the previous frame's (at job entry it still held the
 * previous one in the 2026-11-14 20:5x run: the game writes it after the job
 * starts). While armed the job is skipped, so a fresh serial proves the game
 * (not the job) refreshed the block and its geometry is this frame's (or the
 * previous one's); a stale block says nothing about the image and the frame
 * uses the PS3 path. A fresh block whose image size, depth column or tile
 * bank differs from the probed geometry (resolution change) drops back to
 * learning: PS3 path, probes with the new geometry before PC fast runs again.
 * 1 = PC fast with *g, 0 = PS3 path for this frame, -1 = back to learning.
 * Why a serial check at all: the block is the only source of the geometry
 * (image size, depth column, bank) and is not rewritten when the effect stops,
 * so a block from an earlier aim (or another user of kernel 618) could describe
 * a different image; the serial proves the game wrote it for this aim. The
 * CoC parameters always come from this frame's own job parameter block.
 * R3_DOF_FAST_BLOCK_AHEAD (default 1, 0 = the old serial / serial - 1 rule):
 * a block holding serial + 1 (the game already wrote the next frame's; 17:12
 * K3 run) or serial - 2 .. serial - FAST_BLOCK_BACK (the SPU ran this frame's
 * job ahead of the PPU's block write; 21:00 K1 run: every PS3-path frame was
 * block serial - 2 with the identical geometry, and those frames were the
 * purple ones) is used when its geometry equals the probed one exactly - then
 * it carries nothing that could differ from a fresh block; a different
 * geometry there is not this frame's (PS3 path, no relearn).
 * R3_DOF_FAST_BLOCK_STALE (default 1, needs R3_DOF_FAST_BLOCK_AHEAD): an even
 * older block with exactly the probed geometry is used too. 21:49 K3 run: the
 * only PS3-path frames left were the first frames of a new aim (block 25-49
 * frames old: the game had not written it for this aim yet), and the purple
 * showed there; the image size is fixed by the guest resolution, so the old
 * block's geometry is this frame's. */
#define FAST_BLOCK_BACK 8u
static int s_block_ahead = 1, s_block_stale = 1;
static volatile long s_ag_fast, s_ag_noblk, s_ag_old, s_ag_new, s_ag_ahead, s_ag_stale;
static int fast_armed_geom(uint32_t serial, r3dof_fast_geom* g, uint32_t* bs)
{
    *bs = 0;
    if (!fast_geom_block(s_geo_pe, g, bs)) { s_ag_noblk++; return 0; }
    if (s_block_ahead && *bs != serial && *bs != serial - 1u && (*bs == serial + 1u || serial - *bs <= FAST_BLOCK_BACK)) {
        if (memcmp(g, &s_geo_arm, sizeof *g)) { *bs == serial + 1u ? s_ag_new++ : s_ag_old++; return 0; }
        s_ag_ahead++;
        return 1;
    }
    if (s_block_ahead && s_block_stale && (int32_t)(serial - *bs) > 0 && !memcmp(g, &s_geo_arm, sizeof *g)) {
        const long n = ++s_ag_stale;
        if (n <= 8 || !(n & (n - 1))) {
            fprintf(stderr, "[dof-fast] frame serial 0x%X (frame %ld): kernel 618 block is %u frames old (serial 0x%X, not rewritten for this aim yet) with the probed geometry -> used (R3_DOF_FAST_BLOCK_STALE; %ld times)%c",
                    serial, (long)g_port_frame_no, serial - *bs, *bs, n, 10);
            fflush(stderr);
        }
        return 1;
    }
    if (*bs != serial && *bs != serial - 1u) { (int32_t)(*bs - serial) > 0 ? s_ag_new++ : s_ag_old++; return 0; }
    if (*bs != serial) {
        static int once;
        if (!once++) {
            fprintf(stderr, "[dof-fast] frame serial 0x%X: kernel 618 block holds the previous frame's serial 0x%X -> its geometry is used%c", serial, *bs, 10);
            fflush(stderr);
        }
    }
    if (!memcmp(g, &s_geo_arm, sizeof *g)) { s_ag_fast++; return 1; }
    static int nlog;
    if (nlog++ < 8) {
        fprintf(stderr, "[dof-fast] frame serial 0x%X: image %ux%u depth col %u bank %u, probed %ux%u depth col %u bank %u -> PS3 exact aim blur, probing the new geometry%c",
                serial, g->w, g->h, g->zx, g->bank, s_geo_arm.w, s_geo_arm.h, s_geo_arm.zx, s_geo_arm.bank, 10);
        fflush(stderr);
    }
    s_fast_state = 0; s_probe_ok = 0; s_probe_blur = 0; s_since_check = 0;
    s_probe_after = g_port_frame_no;
    return -1;
}

/* probe frame (learning or recheck): keep the tiled region as it is at job
 * entry; it is detiled once the job's kernel 618 calls gave the geometry */
static void fast_probe_begin(uint32_t serial, uint32_t ea)
{
    const uint8_t* reg = hj_mem(R3DOF_FAST_EA, R3DOF_FAST_REGION);
    s_preg_ok = reg != NULL;
    if (reg) memcpy(s_preg, reg, R3DOF_FAST_REGION);
    const uint8_t* pb = hj_mem(ea + 0x90u, 0xC0u);
    s_probe_pblk_ok = pb != NULL;
    if (pb) memcpy(s_probe_pblk, pb, 0xC0u);
    s_probe_ramp = s_robust && pb && s_last_pblk_ok && fast_params_ramp(s_last_pblk, pb);
    if (s_probe_ramp) s_ramp_frame = g_port_frame_no;
    if (s_robust) fast_sync_begin(serial, ea);
    r3dof_fast_geom g;
    uint32_t bs;
    if (s_geo_pe && fast_geom_block(s_geo_pe, &g, &bs)) {
        if (bs == serial || bs == serial - 1u) s_gser_ok++;
        else if (!s_gser_bad++) fprintf(stderr, "[dof-fast] kernel 618 block 0x%08X holds serial 0x%X at job entry of 0x%X%c", s_geo_pe, bs, serial, 10);
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    s_probe_serial = serial; s_probe_ea = ea; s_probe_t = t.QuadPart;
}

/* job's linear copy vs the entry-time image: 1 exact, 2 differs, 0 not checkable
 * (copy not learned, unreadable, or another op of the job writes into it) */
static int fast_copy_cmp(const fast_copy* c, const r3dof_fast_geom* g, const uint8_t* lin)
{
    const uint32_t lp = g->w * 4u;
    const uint64_t size64 = (uint64_t)(g->h - 1u) * c->pitch + lp;
    if (!c->ea || c->pitch < lp || c->pitch > R3DOF_FAST_PITCH || size64 > 0x10000000u) return 0;
    const uint32_t size = (uint32_t)size64;
    const long n = s_nout;
    for (long i = 0; i < n; i++)
        if (!(s_outs[i].out == c->ea && s_outs[i].in == c->in) && s_outs[i].out >= c->ea && s_outs[i].out - c->ea < size) return 0;
    const uint8_t* m = hj_mem(c->ea, size);
    if (!m) return 0;
    for (uint32_t y = 0; y < g->h; y++)
        if (memcmp(m + y * c->pitch, lin + y * lp, lp)) return 2;
    return 1;
}

static void fast_luma(const uint8_t* sharp, const uint8_t* d, float* l, int i)
{
    const float a = d[i * 4] / 255.f;
    *l = 0.299f * (sharp[i * 4 + 1] + (d[i * 4 + 1] - sharp[i * 4 + 1]) * a) +
         0.587f * (sharp[i * 4 + 2] + (d[i * 4 + 2] - sharp[i * 4 + 2]) * a) +
         0.114f * (sharp[i * 4 + 3] + (d[i * 4 + 3] - sharp[i * 4 + 3]) * a);
}

static const char* fast_cmp_name(int st) { return st == 1 ? "exact" : st == 2 ? "DIFF" : "n/a"; }

/* R3_DOF_GUEST_GUARD (default 1; 0 = off): the composite's ARGB8 sample of
 * 0x30030000 reads job 419's output from guest memory (CPU fast path, or the
 * PS3 job). The output as it was when the frame's writer finished (the host
 * encode, or the job's label-77 store) is kept; the RSX calls
 * r3_dof_guest_guard right before it decodes that region (texture_cache.h
 * upload_texture), which compares guest memory with it, restores rows that
 * something wrote over since (logged with frame, time and row span: the
 * 2026-11-16 K1/K3 purple frames are horizontal FP16 bands in the blurred
 * areas only, i.e. part of the rows lost the output after it was written) and
 * makes the texture cache decode guest memory, never the FP16 surface.
 * Two copies are kept (the SPU may finish job N+1 before the RSX composites
 * frame N): a composite (identified by the light buffer's draw tag, so every
 * bind of one composite gets the same copy) takes the newest completed copy
 * no composite has used yet (older ones are dropped, as are unused ones when
 * a newer copy arrives; a job a composite stopped waiting for, and GPU-requested
 * frames, never leave an unused copy); only when there is none while a PS3-path job is
 * running does it wait, up to R3_DOF_GUARD_WAIT_MS (default 10), for that
 * job's label-77 store. Copies expire after R3_DOF_GUARD_MAX_MS (default 100)
 * or 2 flips. */
typedef struct {
    uint8_t* buf;
    long long t, frame;
    uint32_t serial;
    int ps3, valid;
    uint64_t comp;     /* composite that used it, 0 = not used yet */
} guard_copy;
static int s_guard = 0, s_guard_wait_ms = 10, s_guard_max_ms = 100;
static guard_copy s_gc[2];
static SRWLOCK s_gslock = SRWLOCK_INIT;
static volatile uint32_t s_gpend_serial;
static volatile long s_gtakes;
static uint32_t s_gtimed_serial;   /* a PS3 job a composite gave up waiting for: its copy arrives already used */
static uint64_t s_gtimed_comp;
static volatile long s_guard_checks, s_guard_fixes, s_guard_waits, s_guard_late, s_guard_none, s_guard_idle;
/* frame of the last job 419 start / output: with no job for a while the composite reading 0x30030000 is the
 * game's normal non-aim frame (menus, loads, hip fire), not an unguarded aim-blur frame */
static volatile long long s_gjob_frame = -0x40000000LL;
#define GUARD_IDLE_FRAMES 8

static void guard_stamp(char* b, size_t n)
{
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(b, n, "frame %ld %02d:%02d:%02d.%03d", (long)g_port_frame_no, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/* first lane of a new job serial: a PS3-path job's output arrives with its label-77 store */
static void guard_begin(uint32_t serial, int ps3)
{
    if (!s_gc[1].buf) return;
    s_gjob_frame = (long long)g_port_frame_no;
    s_gpend_serial = ps3 ? serial : 0;
}

/* the frame's output is complete in guest memory: replaces the older copy */
static void guard_take(uint32_t serial, int ps3)
{
    extern uint8_t* vm_base;
    extern uint8_t* vm_sudo_base;   /* unprotected view: reading must not fault a flush */
    if (!s_gc[1].buf) return;
    AcquireSRWLockExclusive(&s_gslock);
    guard_copy* c = !s_gc[0].valid ? &s_gc[0] : !s_gc[1].valid ? &s_gc[1] :
                    (int32_t)(s_gc[0].serial - s_gc[1].serial) <= 0 ? &s_gc[0] : &s_gc[1];
    memcpy(c->buf, (vm_sudo_base ? vm_sudo_base : vm_base) + R3DOF_FAST_EA, R3DOF_FAST_REGION);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    c->t = t.QuadPart; c->frame = (long long)g_port_frame_no; c->serial = serial; c->ps3 = ps3; c->comp = 0; c->valid = 1;
    if (s_gtimed_serial && s_gtimed_serial == serial) { c->comp = s_gtimed_comp ? s_gtimed_comp : ~0ull; s_gtimed_serial = 0; }
    /* older unused copies: their composite has gone (or never sampled guest memory) */
    for (int i = 0; i < 2; i++)
        if (&s_gc[i] != c && s_gc[i].valid && !s_gc[i].comp) s_gc[i].valid = 0;
    if (s_gpend_serial == serial) s_gpend_serial = 0;
    InterlockedIncrement(&s_gtakes);
    ReleaseSRWLockExclusive(&s_gslock);
}

/* the copy for composite comp (lock held): the one it already uses, else the
 * newest unused live one (any older one is dropped: its composite is gone) */
static guard_copy* guard_pick(uint64_t comp, long long now)
{
    guard_copy* u = NULL;
    for (int i = 0; i < 2; i++) {
        guard_copy* c = &s_gc[i];
        if (c->valid && ((now - c->t) * 1000 > (long long)s_guard_max_ms * s_qf || (long long)g_port_frame_no - c->frame > 2)) c->valid = 0;
        if (!c->valid) continue;
        if (c->comp && c->comp == comp) return c;
        if (!c->comp && (!u || (int32_t)(c->serial - u->serial) > 0)) u = c;
    }
    if (u) {
        for (int i = 0; i < 2; i++)
            if (s_gc[i].valid && &s_gc[i] != u && (int32_t)(s_gc[i].serial - u->serial) < 0) s_gc[i].valid = 0;
        u->comp = comp;
    }
    return u;
}

/* RSX side: sudo = unprotected host view of [R3DOF_FAST_EA, + size), comp = the
 * light buffer's draw tag. 1 = guest memory holds the output of the frame this
 * composite shows (restored where needed): decode it; 0 = no output kept
 * (GPU-owned frame, PS3 job still running, guard off) */
int r3_dof_guest_guard(uint8_t* sudo, uint32_t size, uint64_t comp)
{
    if (!s_gc[1].buf || !sudo || size < R3DOF_FAST_REGION) return 0;
    char st[48];
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    AcquireSRWLockExclusive(&s_gslock);
    guard_copy* c = guard_pick(comp, t.QuadPart);
    const uint32_t pend = s_gpend_serial;
    if (!c && pend) {
        const long takes = s_gtakes;
        ReleaseSRWLockExclusive(&s_gslock);
        LARGE_INTEGER t0 = t;
        while (s_gtakes == takes && s_gpend_serial == pend) {
            QueryPerformanceCounter(&t);
            if ((t.QuadPart - t0.QuadPart) * 1000 >= (long long)s_guard_wait_ms * s_qf) break;
            Sleep(0);
        }
        QueryPerformanceCounter(&t);
        AcquireSRWLockExclusive(&s_gslock);
        c = guard_pick(comp, t.QuadPart);
        if (!c) { s_gtimed_serial = pend; s_gtimed_comp = comp; }
        const long n = InterlockedIncrement(c ? &s_guard_waits : &s_guard_late);
        if (n <= 16 || !(n & (n - 1))) {
            guard_stamp(st, sizeof st);
            fprintf(stderr, "[dof-guard] composite samples the light buffer with no unused job 419 output while serial 0x%X (PS3 path) has not stored label 77: %s after %.2f ms (%ld times, %s)%c",
                    pend, c ? "waited for it" : "still running, not guarded", (double)(t.QuadPart - t0.QuadPart) * 1000.0 / s_qf, n, st, 10);
            fflush(stderr);
        }
    }
    if (!c) {
        ReleaseSRWLockExclusive(&s_gslock);
        if (!pend) {
            /* not GPU-served and no CPU/PS3 output kept: what the composite decodes is the texture cache's choice.
             * Logged only while job 419 ran in the last GUARD_IDLE_FRAMES frames (otherwise counted as idle) */
            if ((long long)g_port_frame_no - s_gjob_frame > GUARD_IDLE_FRAMES) {
                InterlockedIncrement(&s_guard_idle);
                return 0;
            }
            const long n = InterlockedIncrement(&s_guard_none);
            if (n <= 16 || !(n & (n - 1))) {
                guard_stamp(st, sizeof st);
                fprintf(stderr, "[dof-guard] composite samples the light buffer (draw tag %llu) with no job 419 output kept and no job pending (%ld times, %ld more with no job 419 in the last %d frames, %s)%c",
                        (unsigned long long)comp, n, s_guard_idle, GUARD_IDLE_FRAMES, st, 10);
                fflush(stderr);
            }
        }
        return 0;
    }
    InterlockedIncrement(&s_guard_checks);
    uint32_t nrow = 0, first = 0, last = 0;
    for (uint32_t o = 0; o < R3DOF_FAST_REGION; o += 8192u)
        if (memcmp(sudo + o, c->buf + o, 8192u)) {
            if (!nrow++) first = o >> 13;
            last = o >> 13;
            memcpy(sudo + o, c->buf + o, 8192u);
        }
    const uint32_t serial = c->serial; const int ps3 = c->ps3;
    const double age = (double)(t.QuadPart - c->t) * 1000.0 / s_qf;
    ReleaseSRWLockExclusive(&s_gslock);
    if (nrow) {
        const long n = InterlockedIncrement(&s_guard_fixes);
        if (n <= 32 || !(n & (n - 1))) {
            guard_stamp(st, sizeof st);
            fprintf(stderr, "[dof-guard] composite sample of the light buffer: %u of 352 guest lines (8 KB, lines %u..%u) differ from job 419 serial 0x%X's output (%s, completed %.2f ms earlier) -> restored (%ld of %ld samples, %s)%c",
                    nrow, first, last, serial, ps3 ? "PS3 job" : "PC fast path", age, n, (long)s_guard_checks, st, 10);
            fflush(stderr);
        }
    }
    return 1;
}
/* R3_DOF_FIRST_USE_GUARD (default 1, 0 = off): the first ADS of a session (and
 * every R3_DOF_GPU_APPROX_RECHECK probe) runs job 419 on the PS3 path, which has
 * none of the fast path's defences (RSX gate, black/FP16 hold). On the first ADS
 * of the 2026-11-18 13:40:35 run the light buffer still held FP16 lighting texels
 * on probe frames 8947-8948 (+ the first fast frame): the job's CoC saturated on
 * every pixel (probe "CoC byte match 100.0%, blurred 100.0%") and its output -
 * FP16 bytes read as ARGB8 - replaced the whole scene (purple ground, black sky,
 * no weapon, HUD intact; clip 13-40-34 frames 118-122). Later ADS never showed it:
 * armed frames use the gated, held fast path or the GPU path. Here a PS3-path
 * job's output is checked right before its label-77 store (semfix_pre_spu, the
 * RSX composite still waits for that store): input FP16-garbled / black (probe
 * frames: the entry-time snapshot) or the output's CoC saturated everywhere ->
 * the output is replaced by a neutral one (A = 0: the composite shows the sharp
 * scene) and the probe is not counted. The fast path's first frame (no last good
 * output to hold) gets the same neutral output. */
static int s_fuse = 1;
static volatile uint32_t s_fuse_serial;      /* PS3-path job whose output is checked before its label-77 store */
static volatile uint32_t s_fuse_bad_serial;  /* neutralised: its probe is not counted */
static volatile long s_fuse_checks, s_fuse_n;
static const char* const s_fuse_why[5] = { "ok", "input FP16 lighting texels (purple comb)", "input black", "CoC saturated on every pixel",
                                            "DOF parameters zero (blur off: the SPU's CoC is 0, IEEE floats here give inf/NaN)" };

static void fuse_log(uint32_t serial, const char* path, int why, int dark, int sat)
{
    const long n = InterlockedIncrement(&s_fuse_n);
    if (n <= 32 || !(n & (n - 1))) {
        char st[48]; guard_stamp(st, sizeof st);
        fprintf(stderr, "[dof-fast] R3_DOF_FIRST_USE_GUARD: serial 0x%X (%s, %s): %s (input %d/1000 black, output CoC saturated %d/1000) -> neutral output (sharp scene)%s (%ld times, %ld checks)%c",
                serial, path, st, s_fuse_why[why], dark, sat, path[0] == 'P' ? ", probe not counted" : "", n, (long)s_fuse_checks, 10);
        fflush(stderr);
    }
}

/* the job of `serial` (PS3 path) is about to store label 77: its output is complete */
static void fuse_ps3_check(uint32_t serial)
{
    extern uint8_t* vm_base;
    extern uint8_t* vm_sudo_base;
    if (!serial || InterlockedCompareExchange((volatile long*)&s_fuse_serial, 0, (long)serial) != (long)serial) return;
    AcquireSRWLockExclusive(&s_flock);
    r3dof_fast_geom g = s_geo;
    const int probe = s_probe_serial == serial && s_preg_ok;
    int dof_off = 0;
    for (int i = 0; i < FAST_NREC; i++) if (s_frec[i].serial == serial) { dof_off = s_frec[i].dof_off; break; }
    ReleaseSRWLockExclusive(&s_flock);
    if (!g.w || !r3dof_fast_geom_ok(&g)) return;
    const uint8_t* rd = (vm_sudo_base ? vm_sudo_base : vm_base) + R3DOF_FAST_EA;
    const uint32_t npx = g.w * g.h;
    AcquireSRWLockExclusive(&s_fwork);
    InterlockedIncrement(&s_fuse_checks);
    int dark = 0, fp16 = 0;
    if (probe) {
        r3dof_fast_detile(&g, s_preg, 0, s_wc);
        dark = r3dof_fast_dark_permille(s_wc, npx, 7);
        fp16 = dark < R3DOF_FAST_BLACK_PM && r3dof_fast_fp16_garbled(s_wc, g.w, g.h);
    }
    r3dof_fast_detile(&g, rd, 0, s_wo);
    const int sat = r3dof_fast_coc_sat_permille(s_wo, npx, 7);
    /* Zero parameters: on the SPU the CoC is sat(x*0 + 0) * 0 = 0 everywhere (no inf/NaN in SPU floats), so the
     * PS3 shows the sharp scene; the recompiled job's IEEE maths turns the zero depth terms into inf/NaN and a
     * partly garbled output that the saturation test lets through (purple hip-fire frames). Neutral = the PS3's output. */
    const int why = dof_off ? 4 : r3dof_fast_bad_frame(probe, dark, fp16, sat);
    if (why) {
        memset(s_wo, 0, (size_t)npx * 4u);
        r3dof_fast_encode(&g, vm_base + R3DOF_FAST_EA, s_wo);
        if (g_spu_put_hook) g_spu_put_hook(R3DOF_FAST_EA, R3DOF_FAST_REGION);
        if (probe) s_fuse_bad_serial = serial;
    }
    ReleaseSRWLockExclusive(&s_fwork);
    if (why) fuse_log(serial, probe ? "PS3-path probe" : "PS3 path", why, dark, sat);
}

/* semfix: an SPU stored a label. Label 77 during a probe = job 419 is done. */
static void fast_semfix_hook(uint32_t word, uint32_t val)
{
    if (word == FAST_LABEL && !t_fast_self && s_fuse_serial) fuse_ps3_check(s_fuse_serial);   /* R3_SEMFIX_CYCLE=0: after the store */
    if (word == FAST_LABEL && !t_fast_self && s_gpend_serial && (!s_label_n || val == s_label_val))
        guard_take(s_gpend_serial, 1);
    if (word != FAST_LABEL || t_fast_self || !s_probe_serial) return;
    AcquireSRWLockExclusive(&s_flock);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (!s_probe_serial || s_fast_state < 0 || t.QuadPart - s_probe_t > s_qf / 2) { s_probe_serial = 0; ReleaseSRWLockExclusive(&s_flock); return; }
    const uint32_t serial = s_probe_serial;
    s_probe_serial = 0;
    if (s_robust) {
        AcquireSRWLockExclusive(&s_swlock);
        if (s_sw_ea && s_sw_serial == serial) s_sw_labeled = 1;
        ReleaseSRWLockExclusive(&s_swlock);
    }
    if (s_label_n && val != s_label_val) {
        if (!s_arm_from_cache) { fast_off("label 77 value differs between frames"); ReleaseSRWLockExclusive(&s_flock); return; }
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE: probe serial 0x%X: label 77 = 0x%08X, cached 0x%08X -> PS3 exact aim blur, learning again%c", serial, val, s_label_val, 10);
        if (s_fast_state == 1) s_fast_state = 0;
        s_probe_ok = 0; s_probe_blur = 0; s_label_n = 0;
        fast_cache_drop("label 77 value differs");
    }
    s_label_val = val; s_label_n++;
    if (s_fuse_bad_serial == serial) {
        /* R3_DOF_FIRST_USE_GUARD replaced this job's output: no verdict on garbage input */
        s_fuse_bad_serial = 0;
        if (s_fast_state == 1) s_since_check = s_fast_recheck;
        ReleaseSRWLockExclusive(&s_flock);
        return;
    }
    const r3dof_fast_geom g = s_geo;
    const int gok = s_preg_ok && s_geo_serial == serial && g.w;
    const long npx = gok ? (long)(g.w * g.h) : 1;
    int zst = 0, cst = 0;
    if (gok) {
        r3dof_fast_detile(&g, s_preg, 0, s_pc);
        r3dof_fast_detile(&g, s_preg, g.zx, s_pz);
        zst = fast_copy_cmp(&s_zcopy, &g, s_pz);
        cst = fast_copy_cmp(&s_ccopy, &g, s_pc);
    }
    const uint8_t* reg = hj_mem(R3DOF_FAST_EA, R3DOF_FAST_REGION);
    /* the job's parameters: the block as it was at job entry; a block the PPU
     * rewrote while the job ran (aim zoom ramp) cannot be compared */
    const uint8_t* pbn = hj_mem(s_probe_ea + 0x90u, 0xC0u);
    const int pchg = s_probe_pblk_ok && pbn && memcmp(pbn, s_probe_pblk, 0xC0u) != 0;
    r3dof_fast_params par;
    const int pok = s_probe_pblk_ok && r3dof_fast_params_from_block(s_probe_pblk, &par);
    /* DOF off on this frame (hip fire): the parameter block is zero (zn, zf, near/far max), the job only
     * copies the colour (no kernel 618 depth copy) and there is no blur to compare. Counting it as
     * "inconclusive" gave the fast path up after 6 hip-fire probes for the whole session, before the first
     * real ADS frame; that put every later job 419 on the PS3 path. Not counted: probe again next frame. */
    if (s_probe_pblk_ok && !dof_be32(s_probe_pblk + 0x10) && !dof_be32(s_probe_pblk + 0x14) &&
        !dof_be32(s_probe_pblk + 0x58) && !dof_be32(s_probe_pblk + 0x78)) {
        static volatile long s_offn;
        if (InterlockedIncrement(&s_offn) <= 8)
            fprintf(stderr, "[dof-fast] probe serial=0x%X frame=%ld: DOF parameters are zero (blur off, e.g. hip fire) -> not counted, probing again%c",
                    serial, (long)g_port_frame_no, 10);
        if (s_fast_state == 1) s_since_check = s_fast_recheck;
        ReleaseSRWLockExclusive(&s_flock);
        return;
    }
    const int inok = zst == 1 && cst != 2;
    long amatch = 0, blur = 0, dasum = 0;
    double se = 0;
    if (inok && pok && reg && !pchg) {
        r3dof_fast_detile(&g, reg, 0, s_ps);
        r3dof_fast_run(&g, s_pc, s_pz, s_po, &par, s_pscr);
        for (long i = 0; i < npx; i++) {
            const int da = (int)s_ps[i * 4] - (int)s_po[i * 4];
            amatch += da >= -2 && da <= 2;
            dasum += da < 0 ? -da : da;
            blur += s_ps[i * 4] > 25;
            float l0, l1; fast_luma(s_pc, s_ps, &l0, (int)i); fast_luma(s_pc, s_po, &l1, (int)i);
            se += (double)(l0 - l1) * (l0 - l1);
        }
    }
    const double psnr = se > 0 ? 10.0 * log10(255.0 * 255.0 * npx / se) : 99.0;
    const int isblur = blur * 20 >= npx;
    const int checked = inok && pok && reg && !pchg;
    const int verdict = checked ? r3dof_fast_probe_verdict(amatch, blur, npx, psnr, s_fast_minpsnr) : 0;
    const int good = checked && verdict == 1;
    const int inconc = !gok || zst == 0;   /* the job's depth copy could not be compared */
    const int indiff = zst == 2 || cst == 2;
    /* a zoom-ramp / CoC-settling probe is not counted below: its label says so instead of FAIL */
    const int ramp_nc = !good && !inconc && !indiff && checked &&
                        (s_probe_ramp || (s_robust && s_ramp_hold > 0 && g_port_frame_no - s_ramp_frame <= s_ramp_hold));
    static int nlog;
    if (pchg && !inconc && !indiff) {
        if (s_pchg++ < 8)
            fprintf(stderr, "[dof-fast] probe serial=0x%X frame=%ld: parameter block changed while the job ran -> not counted%c",
                    serial, (long)g_port_frame_no, 10);
        if (s_fast_state == 1) s_since_check = s_fast_recheck;
        ReleaseSRWLockExclusive(&s_flock);
        return;
    }
    if (nlog++ < 12 || !good)
        fprintf(stderr, "[dof-fast] probe serial=0x%X frame=%ld: image %ux%u depth col %u bank %u, depth %s (copy 0x%08X), colour %s (copy 0x%08X), params %s, CoC byte match %.1f%% (mean |dA| %.2f), blurred %.1f%%, PSNR %.1f dB (PC fast vs PS3 job) -> %s%c",
                serial, (long)g_port_frame_no, gok ? g.w : 0, gok ? g.h : 0, g.zx, g.bank, gok ? fast_cmp_name(zst) : "not learned", s_zcopy.ea,
                fast_cmp_name(cst), s_ccopy.ea, pok ? "ok" : "rejected", 100.0 * amatch / npx, (double)dasum / npx, 100.0 * blur / npx, psnr,
                good ? "ok" : inconc ? "inconclusive" : indiff ? "input DIFF" : ramp_nc ? (s_probe_ramp ? "SKIP (zoom ramp, not counted)" : "SKIP (CoC settling, not counted)") : "FAIL", 10);
    if (good) {
        s_inconc = 0;
        s_geo_arm = g;
        if (s_fast_state == 0) {
            s_probe_ok++; s_probe_blur += isblur;
            fast_try_arm();
        }
    } else if (inconc) {
        s_probe_ok = 0;
        if (s_fast_state == 1) s_since_check = s_fast_recheck;   /* probe again next frame */
        if (++s_inconc >= 6) fast_off("6 probes in a row could not compare the job's depth copy");
    } else if (indiff) {
        s_probe_ok = 0;
        if (s_fast_state == 1 || ++s_indiff >= 3) fast_off("image read at job entry differs from the job's copy");
    } else if (ramp_nc) {
        /* aim zoom ramp: the job's CoC moves faster than its per-frame parameters say; not a layout / quality verdict */
        if (!s_probe_ramp) {
            if (!s_ramp_keep) { s_probe_ok = 0; s_probe_blur = 0; }   /* arm only on good probes after the CoC settled */
            if (s_ramp_settle++ < 8)
                fprintf(stderr, "[dof-fast] probe serial=0x%X: %ld frames after a DOF parameter ramp (CoC still settling, R3_DOF_GPU_APPROX_RAMP_HOLD=%d) -> not counted, probing again next frame%c",
                        serial, (long)(g_port_frame_no - s_ramp_frame), s_ramp_hold, 10);
        } else if (s_ramp_skips++ < 8)
            fprintf(stderr, "[dof-fast] probe serial=0x%X: DOF parameters ramping (aim zoom) -> not counted, probing again next frame%c", serial, 10);
        if (s_fast_state == 1) s_since_check = s_fast_recheck;
    } else {
        char b[128];
        const char* why = !pok ? "parameter block" : !reg ? "image unreadable" : verdict == 0 ? "CoC/output layout" : "PSNR";
        s_probe_ok = 0; s_probe_blur = 0;
        if (++s_qfail >= s_fast_maxfail) {
            snprintf(b, sizeof b, "probe FAIL (%s), %d failed probes", why, s_qfail);
            fast_off(b);
        } else {
            s_probe_after = g_port_frame_no + s_fast_retry;
            if (s_fast_state == 1) { s_fast_state = 0; fast_cache_drop("probe failed while armed"); }   /* PS3 path until 3 good probes again */
            fprintf(stderr, "[dof-fast] probe FAIL (%s, %d of %d) -> PS3 exact aim blur, probing again in %d frames%c",
                    why, s_qfail, s_fast_maxfail, s_fast_retry, 10);
        }
    }
    fflush(stderr);
    ReleaseSRWLockExclusive(&s_flock);
}

/* learning / probe: kernel calls of job 419 check the entry-time descriptor and
 * the parameter block address (line + 0x90); kernel 618's calls give the image
 * geometry and its detile copies; every op's output is noted (copy clobbers) */
static void fast_kernel_note(spu_context* ctx, uint32_t owner)
{
    const uint8_t* ls = (const uint8_t*)ctx->ls;
    InterlockedIncrement(memcmp(ls + 0x2C60u, ls + FAST_DESC, 16) ? &s_kdesc_bad : &s_kdesc_ok);
    const uint32_t r3 = ctx->gpr[3]._u32[0] & 0x3FFF0u;
    const uint32_t p = dof_be32(ls + r3) & 0x3FFF0u;
    if (p > SPU_LS_SIZE - 0x60u) return;
    const fast_out o = { dof_be32(ls + p + 0x40), dof_be32(ls + p + 0x50) };
    long n = s_nout, i = 0;
    for (; i < n; i++) if (s_outs[i].out == o.out && s_outs[i].in == o.in) break;
    if (i == n && n < FAST_NOUT) {
        AcquireSRWLockExclusive(&s_flock);
        for (i = 0; i < s_nout; i++) if (s_outs[i].out == o.out && s_outs[i].in == o.in) break;
        if (i == s_nout && s_nout < FAST_NOUT) { s_outs[i] = o; MemoryBarrier(); s_nout = i + 1; }
        ReleaseSRWLockExclusive(&s_flock);
    }
    const uint32_t psz = dof_be32(ls + p + 0x1C), pe = dof_be32(ls + p + 0x18);
    if (owner == FAST_DETILE_IMG && psz == 0x20u) {
        r3dof_fast_geom g;
        uint32_t serial;
        if (!fast_geom_block(pe, &g, &serial)) return;
        const uint32_t mode = dof_be32(ls + p + 0x20), opitch = dof_be32(ls + p + 0x44);
        AcquireSRWLockExclusive(&s_flock);
        if (memcmp(&g, &s_geo, sizeof g) && !r3dof_fast_geom_ok(&g)) {
            static int once;
            if (!once++) fprintf(stderr, "[dof-fast] kernel 618 block 0x%08X: image %ux%u depth col %u bank %u not usable%c", pe, g.w, g.h, g.zx, g.bank, 10);
            s_geo_serial = 0;
        } else {
            if (memcmp(&g, &s_geo, sizeof g)) {
                s_geo_cin = R3DOF_FAST_EA + r3dof_fast_min_off(&g, 0);
                s_geo_zin = R3DOF_FAST_EA + r3dof_fast_min_off(&g, g.zx);
            }
            s_geo = g; s_geo_serial = serial; s_geo_pe = pe;
            if (mode == 0 && opitch >= g.w * 4u && opitch <= R3DOF_FAST_PITCH && !(opitch & 15u)) {
                const fast_copy c = { o.out, opitch, o.in };
                if (o.in == s_geo_cin) s_ccopy = c;
                else if (o.in == s_geo_zin) s_zcopy = c;
            }
        }
        ReleaseSRWLockExclusive(&s_flock);
        return;
    }
    if (psz != 0xC0u) return;
    InterlockedIncrement(pe == dof_be32(ls + 0x2C60u) + 0x90u ? &s_kpar_ok : &s_kpar_bad);
    if (s_kdesc_bad == 1 || s_kpar_bad == 1) {
        AcquireSRWLockExclusive(&s_flock);
        fast_off(s_kdesc_bad ? "job descriptor at kernel time differs from the one at job entry" : "parameter block is not at sync line + 0x90");
        ReleaseSRWLockExclusive(&s_flock);
    }
}

static void fast_put(uint32_t ea, const void* src, uint32_t n)
{
    extern uint8_t* vm_base;
    memcpy(vm_base + ea, src, n);
    for (uint32_t a = ea & ~127u; a < ea + n; a += 128u)
        if (spu_coh_is_reserved(a)) { spu_lockline_lock(); spu_coh_notify_write_except(a, NULL); spu_lockline_unlock(); }
    if (g_spu_put_hook) g_spu_put_hook(ea, n);
    ps3_guest_mem_notify_put(ea);
}

/* R3_DOF_FAST_RSX_GATE (default 1, 0 = off): the PC fast path reads the light
 * buffer only after the RSX processed this cycle's label-77 reset release (the
 * label no longer holds our last store), i.e. after the post colour/depth blits.
 * Firing (muzzle-flash lighting) puts the RSX behind; the first-touch flush then
 * read the cleared light buffer -> RGB 0 output -> black ADS scene (round-6 log
 * + clip, 2026-11-18 09:33). Bounded by R3_DOF_FAST_RSX_GATE_MS (default 20);
 * 5 timeouts in a row turn it off for the session. The same race one pass later
 * (lighting done, post blit pending) read FP16 lighting texels as ARGB8: purple
 * with a vertical comb (clip 09:33:24 frames 29-31, lightning + firing). */
static int s_fgate = 1, s_fgate_ms = 20, s_fgate_tmo_run;
static long s_fgate_n, s_fgate_held, s_fgate_tmo;
static long long s_fgate_ticks, s_fgate_max, s_fgate_next;

/* R3_DOF_FAST_BLACK_HOLD (default 1, 0 = off): a black or FP16-garbled input
 * (or one read after a gate timeout) after a clean frame shows the last good
 * output (at most 2 frames in a row); only from the
 * immediately preceding CPU fast-path frame, dropped on any other frame */
static int s_bhold = 1, s_bhold_run, s_bgood_pm = -1, s_bfirst;
static long s_bhold_n, s_blast_frame = -0x40000000L;
static uint8_t* s_wgood;
static r3dof_fast_geom s_bgood_geo;

/* PS3-path, GPU-served or ADS-exit frame: the kept output is no longer the previous frame's */
static volatile long s_bdrop;
static void fast_black_drop(void) { InterlockedExchange(&s_bdrop, 1); }

static int fast_rsx_gate(uint32_t serial)
{
    extern uint8_t* vm_base;
    const uint32_t base = g_semfix_base;
    if (!s_fgate || !base || !s_label_n) return 0;
    const volatile uint32_t* lab = (const volatile uint32_t*)(vm_base + base + FAST_LABEL * 16u);
    const uint32_t mine = __builtin_bswap32(s_label_val);
    s_fgate_n++;
    if (*lab != mine) { s_fgate_tmo_run = 0; return 0; }
    LARGE_INTEGER t0, t; QueryPerformanceCounter(&t0);
    const long long cap = (long long)s_fgate_ms * s_qf / 1000;
    int timed_out = 0;
    for (unsigned spin = 0; *lab == mine; spin++) {
        QueryPerformanceCounter(&t);
        if (t.QuadPart - t0.QuadPart > cap) { timed_out = 1; break; }
        if (spin < 256) YieldProcessor(); else SwitchToThread();
    }
    QueryPerformanceCounter(&t);
    const long long d = t.QuadPart - t0.QuadPart;
    s_fgate_held++; s_fgate_ticks += d;
    if (d > s_fgate_max) s_fgate_max = d;
    if (timed_out) {
        s_fgate_tmo++;
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_RSX_GATE: serial 0x%X: RSX not at label 77's release after %d ms -> light buffer read anyway, last good output kept if recent (frame %ld)%c",
                serial, s_fgate_ms, (long)g_port_frame_no, 10);
        if (++s_fgate_tmo_run >= 5) {
            s_fgate = 0;
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_RSX_GATE: 5 timeouts in a row -> off for this session%c", 10);
        }
        fflush(stderr);
    } else s_fgate_tmo_run = 0;
    if (t.QuadPart > s_fgate_next) {
        s_fgate_next = t.QuadPart + 10 * s_qf;
        fprintf(stderr, "[dof-fast] R3_DOF_FAST_RSX_GATE: %ld of %ld fast frames waited for the RSX's input blits, avg %.2f max %.2f ms, %ld timeouts; black-hold %ld%c",
                s_fgate_held, s_fgate_n, (double)s_fgate_ticks * 1000.0 / s_qf / s_fgate_held,
                (double)s_fgate_max * 1000.0 / s_qf, s_fgate_tmo, s_bhold_n, 10);
        fflush(stderr);
    }
    return timed_out;
}

/* 1 = this frame's input is black / FP16 lighting (purple comb) / read after a gate
 * timeout, after a clean frame: s_wo = last good output */
static int fast_black_hold(const r3dof_fast_geom* g, uint32_t serial, int gate_tmo)
{
    if (!s_bhold || !s_wgood) return 0;
    if (InterlockedExchange(&s_bdrop, 0)) { s_bgood_pm = -1; s_bhold_run = 0; }
    const uint32_t npx = g->w * g->h;
    const int dark = r3dof_fast_dark_permille(s_wc, npx, 7);
    const int fp16 = dark < R3DOF_FAST_BLACK_PM && r3dof_fast_fp16_garbled(s_wc, g->w, g->h);
    const int pm = fp16 || gate_tmo ? 1000 : dark;
    const int same = !memcmp(&s_bgood_geo, g, sizeof *g);
    const long now = (long)g_port_frame_no, gap = now - s_blast_frame;
    s_blast_frame = now;
    if (r3dof_fast_black_hold(pm, s_bgood_pm, same, gap, s_bhold_run, 2)) {
        s_bhold_run++;
        memcpy(s_wo, s_wgood, (size_t)npx * 4u);
        const long n = ++s_bhold_n;
        if (n <= 16 || !(n & (n - 1))) {
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_BLACK_HOLD: serial 0x%X frame %ld: light buffer input %s (%d/1000 black) after a clean frame -> last good output shown (%ld times)%c",
                    serial, (long)g_port_frame_no, fp16 ? "FP16 lighting texels (purple comb)" : gate_tmo ? "read after a gate timeout" : "black", dark, n, 10);
            fflush(stderr);
        }
        return 1;
    }
    if (s_fuse && (fp16 || dark >= R3DOF_FAST_BLACK_PM)) {
        /* R3_DOF_FIRST_USE_GUARD: garbled or black input and no last good output to show
         * (first fast frame, e.g. the first ADS of a session armed from R3_DOF_FAST_CACHE,
         * or the hold already used): the sharp scene instead of a blurred black/purple one */
        memset(s_wo, 0, (size_t)npx * 4u);
        if (fp16) s_bhold_run = 0;
        fuse_log(serial, "fast path, nothing to hold", fp16 ? 1 : 2, dark, 0);
        return 1;
    }
    s_bfirst = s_bgood_pm < 0;
    s_bhold_run = 0;
    s_bgood_pm = pm;
    s_bgood_geo = *g;
    return 0;
}

/* frame work of an armed frame (first lane): image, sync line, label */
static void fast_frame(const fast_rec* r, const r3dof_fast_params* par, const r3dof_fast_geom* g, int check_ok)
{
    extern uint8_t* vm_base;
    LARGE_INTEGER t0, t1, ta, tb, tc; QueryPerformanceCounter(&t0);
    AcquireSRWLockExclusive(&s_fwork);
    uint8_t* reg = vm_base + R3DOF_FAST_EA;
    /* R3_DOF_GPU: the RSX computes the image; until its one-time check passed
     * the CPU path still runs and writes guest memory (fallback for a miss) */
    int gpu = s_gpu && s_gpu_state == 0 && s_gpu_ready;
    if (s_gpu && s_gpu_state == 0 && !s_gpu_ready) {
        static int once;
        if (!once++) { fprintf(stderr, "[dof-gpu] R3_DOF_GPU=1 but no Vulkan RSX texture cache (other backend?) -> CPU fast path%c", 10); fflush(stderr); }
    }
    /* not trusted yet and no check outstanding: the GPU only takes a frame that can be the comparison frame
     * (own kernel 618 block, settled parameters, not in the retry wait after a failed check); the rest
     * stays on the CPU fast path */
    if (gpu && !s_gtrusted && !s_gcheck_serial && (!check_ok || (long)(g_port_frame_no - s_gcheck_after) < 0)) {
        gpu = 0;
        if (s_gwithdrawn == 0) {
            AcquireSRWLockExclusive(&s_gpulock);
            s_gwithdrawn = 1;   /* this frame's output is in guest memory (as for a PS3-path frame) */
            ReleaseSRWLockExclusive(&s_gpulock);
        }
        if (!check_ok) {
            const long n = InterlockedIncrement(&s_gcheck_deferred);
            if (n <= 4 || !(n & (n - 1))) {
                fprintf(stderr, "[dof-gpu] frame serial 0x%X: kernel 618 block of a neighbouring frame or DOF parameters ramping -> CPU fast path, the comparison waits for a settled frame (%ld times)%c",
                        r->serial, n, 10);
                fflush(stderr);
            }
        }
    }
    const int check = gpu && !s_gtrusted && !s_gcheck_serial;
    int enc = !gpu || !s_gtrusted;
    if (!enc && !gpu_post(r->serial, g, par, 0)) { gpu = 0; enc = 1; }
    ta = tb = tc = t0;
    if (enc) {
        const int gate_tmo = fast_rsx_gate(r->serial);
        /* first touch of every page: a locked light buffer faults here and the RSX
         * reads it back (timed apart from the approximation itself) */
        { volatile uint8_t sink = 0; for (uint32_t o = 0; o < R3DOF_FAST_REGION; o += 4096u) sink ^= reg[o]; (void)sink; }
        QueryPerformanceCounter(&ta);
        r3dof_fast_detile(g, reg, 0, s_wc);
        r3dof_fast_detile(g, reg, g->zx, s_wz);
        QueryPerformanceCounter(&tb);
        int held = fast_black_hold(g, r->serial, gate_tmo);
        if (!held) {
            r3dof_fast_run(g, s_wc, s_wz, s_wo, par, s_wscr);
            int sat;
            if (s_fuse && s_bhold && s_bfirst && (sat = r3dof_fast_coc_sat_permille(s_wo, g->w * g->h, 7)) >= R3DOF_FAST_SAT_PM) {
                /* R3_DOF_FIRST_USE_GUARD: first fast frame (nothing to hold) with garbage depth */
                memset(s_wo, 0, (size_t)g->w * g->h * 4u);
                s_bgood_pm = -1;
                held = 1;
                fuse_log(r->serial, "fast path, no last good output", 3, 0, sat);
            } else if (s_wgood) memcpy(s_wgood, s_wo, (size_t)g->w * g->h * 4u);
        }
        QueryPerformanceCounter(&tc);
        int posted = 0;
        if (gpu && !(held && check)) {
            if (check) memcpy(s_gref, s_wo, (size_t)g->w * g->h * 4u);
            posted = gpu_post(r->serial, g, par, check);
            if (posted && check) s_gcheck_serial = r->serial;
        }
        r3dof_fast_encode(g, reg, s_wo);
        if (g_spu_put_hook) g_spu_put_hook(R3DOF_FAST_EA, R3DOF_FAST_REGION);
        /* a GPU-requested frame is shown from the RSX's image: its copy would stay unused */
        if (!posted) guard_take(r->serial, 0);
    } else fast_black_drop();
    ReleaseSRWLockExclusive(&s_fwork);
    if (s_robust) fast_sync_put(r->ea);
    else {
        uint8_t line[128];
        int chg = 0;
        for (int k = 0; k < 128; k++) { line[k] = s_end_unch[k] ? r->start[k] : s_end_c[k]; chg |= line[k] != r->start[k]; }
        if (chg) fast_put(r->ea, line, 128);
    }
    const uint32_t lea = g_semfix_base + FAST_LABEL * 16u, be = __builtin_bswap32(s_label_val);
    semfix_pre_spu(lea, 4);
    fast_put(lea, &be, 4);
    t_fast_self = 1; semfix_note_spu(lea, 4); t_fast_self = 0;
    QueryPerformanceCounter(&t1);
    if (gpu && !enc) {
        static volatile long long s_gtk, s_gnext;
        const long gn = InterlockedIncrement(&s_gpost);
        const long long gtk = InterlockedAdd64(&s_gtk, t1.QuadPart - t0.QuadPart);
        if (t1.QuadPart > s_gnext) {
            s_gnext = t1.QuadPart + 10 * s_qf;
            const long sv = s_gframes;
            fprintf(stderr, "[dof-gpu] %ld aim-blur frames posted to the GPU, %ld computed by the RSX (avg RSX record %.3f ms), host work avg %.3f ms/frame; armed frames %ld + %ld with a neighbouring frame's block, %ld on the PS3 path (no block %ld, older %ld, newer/other %ld)%c",
                    gn, sv, sv ? (double)s_grec_us / 1000.0 / sv : 0.0, (double)gtk * 1000.0 / s_qf / gn,
                    s_ag_fast, s_ag_ahead, s_ag_noblk + s_ag_old + s_ag_new, s_ag_noblk, s_ag_old, s_ag_new, 10);
            fflush(stderr);
        }
        return;
    }
    const long n = InterlockedIncrement(&s_fast_frames);
    const long long tk = InterlockedAdd64(&s_fast_ticks, t1.QuadPart - t0.QuadPart);
    static volatile long long s_tk_rb, s_tk_dt, s_tk_run;
    const long long trb = InterlockedAdd64(&s_tk_rb, ta.QuadPart - t0.QuadPart);
    const long long tdt = InterlockedAdd64(&s_tk_dt, tb.QuadPart - ta.QuadPart);
    const long long trun = InterlockedAdd64(&s_tk_run, tc.QuadPart - tb.QuadPart);
    if (t1.QuadPart > s_fast_next_log) {
        s_fast_next_log = t1.QuadPart + 10 * s_qf;
        const double k = 1000.0 / s_qf / n;
        fprintf(stderr, "[dof-fast] %ld frames on the PC fast path, host work avg %.2f ms (light buffer readback/fault %.2f, detile %.2f, approximation %.2f, encode+sync+label %.2f)%c",
                n, (double)tk * k, (double)trb * k, (double)tdt * k, (double)trun * k, (double)(tk - trb - tdt - trun) * k, 10);
        fflush(stderr);
    }
}

/* job entry (indirect branch with link FAST_LINK into image 419): 1 = skipped */
static int fast_entry(spu_context* ctx)
{
    extern uint8_t* vm_base;
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK & ~3u;
    if (pc == FAST_LINK) { t_in_job = 0; return 0; }   /* the job (or a skipped lane) returns */
    if (s_fast_state < 0) return 0;
    uint32_t owner, off;
    dof_owner(ctx, pc, &owner, &off);
    if (owner != DOF_JOB_IMG) return 0;
    const uint8_t* ls = (const uint8_t*)ctx->ls;
    const uint32_t ea = dof_be32(ls + FAST_DESC), serial = dof_be32(ls + FAST_DESC + 8);
    if ((ea & 127u) || !serial || !hj_mem(ea, 128)) return 0;
    AcquireSRWLockExclusive(&s_flock);
    if (s_entry_off == 0xFFFFFFFFu) s_entry_off = off;
    if (off != s_entry_off) { ReleaseSRWLockExclusive(&s_flock); return 0; }   /* not the entry: a jump inside the job */
    fast_rec* r = NULL;
    for (int i = 0; i < FAST_NREC; i++) if (s_frec[i].serial == serial) { r = &s_frec[i]; break; }
    int first = 0;
    r3dof_fast_params par;
    r3dof_fast_geom g = s_geo_arm;
    int check_ok = 0;
    if (!r) {
        first = 1;
        if (s_robust) fast_sync_finish(serial);
        else fast_learn_end(serial, ea);
        r = &s_frec[s_frec_next++ % FAST_NREC];
        r->serial = serial; r->ea = ea; r->ended = 0; r->mode = 1;
        memcpy(r->start, vm_base + ea, 128);
        {   /* zero zn / zf / near max / far max: the blur is off this frame (hip fire), see fuse_ps3_check */
            const uint8_t* pb0 = hj_mem(ea + 0x90u, 0xC0u);
            r->dof_off = pb0 && !dof_be32(pb0 + 0x10) && !dof_be32(pb0 + 0x14) && !dof_be32(pb0 + 0x58) && !dof_be32(pb0 + 0x78);
        }
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        if (s_probe_serial && t.QuadPart - s_probe_t > s_qf / 2) s_probe_serial = 0;   /* job never stored the label */
        if (!s_ads_t0) { s_ads_t0 = t.QuadPart; s_ads_f0 = (long)g_port_frame_no; }
        if (s_fast_state == 0) fast_cache_try_arm(serial, ea);
        if (s_fast_state == 0) {
            if (!s_probe_serial && (long)(g_port_frame_no - s_probe_after) >= 0) fast_probe_begin(serial, ea);
        } else if (s_fast_state == 1) {
            uint32_t bs = 0;
            if (s_fast_recheck > 0 && ++s_since_check >= s_fast_recheck && !s_probe_serial) {
                s_since_check = 0;
                fast_probe_begin(serial, ea);
            } else if (!fast_params(ea, &par)) {
                static int once;
                if (!once++) fprintf(stderr, "[dof-fast] frame serial 0x%X: parameter block rejected -> PS3 path for this frame%c", serial, 10);
            } else if (fast_armed_geom(serial, &g, &bs) != 1) {
                static volatile long n;
                const long k = InterlockedIncrement(&n);
                if (k <= 8 || !(k & (k - 1))) {
                    fprintf(stderr, "[dof-fast] frame serial 0x%X (frame %ld): kernel 618 block (serial 0x%X, %ux%u depth col %u bank %u) is not this frame's probed geometry -> PS3 path for this frame (%ld times; armed frames: %ld fast, %ld fast with a block of a neighbouring frame, PS3 path: %ld no block, %ld older serial, %ld newer serial / other geometry)%c",
                            serial, (long)g_port_frame_no, bs, g.w, g.h, g.zx, g.bank, k, s_ag_fast, s_ag_ahead, s_ag_noblk, s_ag_old, s_ag_new, 10);
                    fflush(stderr);
                }
            } else if (!hj_mem(R3DOF_FAST_EA, R3DOF_FAST_REGION) || !g_semfix_base) {
                static int once;
                if (!once++) fprintf(stderr, "[dof-fast] frame serial 0x%X: image or label memory unavailable -> PS3 path for this frame%c", serial, 10);
            } else {
                r->mode = 2;
                check_ok = bs == serial;   /* this frame's own kernel 618 block (R3_DOF_GPU check frame) */
                if (check_ok) s_gstale_run = 0;
                else if (s_gcheck_stale > 0 && ++s_gstale_run >= s_gcheck_stale) check_ok = 1;   /* R3_DOF_GPU_CHECK_STALE */
            }
        }
    }
    if (first && s_robust) {
        const uint8_t* pb = hj_mem(ea + 0x90u, 0xC0u);
        if (pb && s_last_pblk_ok && s_fast_state == 0 && s_ramp_hold > 0 && fast_params_ramp(s_last_pblk, pb))
            s_ramp_frame = g_port_frame_no;   /* any frame's ramp, not only probe frames' (R3_DOF_GPU_APPROX_RAMP_HOLD) */
        if (pb && s_last_pblk_ok && fast_params_ramp(s_last_pblk, pb)) s_gramp_frame = g_port_frame_no;   /* R3_DOF_GPU check frame */
        s_last_pblk_ok = pb != NULL;
        if (pb) memcpy(s_last_pblk, pb, 0xC0u);
    }
    if (check_ok && g_port_frame_no - s_gramp_frame <= s_gcheck_ramp_hold) check_ok = 0;
    const int mode = r->mode;
    if (first && !s_ads_logged) {
        if (mode != 2) {
            s_ads_ps3++;
            s_ads_probes += s_probe_serial == serial;
        } else {
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            s_ads_logged = 1;
            fprintf(stderr, "[dof-fast] first ADS: first PC fast frame at serial 0x%X frame %ld, %.0f ms / %ld frames after the session's first job 419 frame; %ld job 419 frames ran on the PS3 path before it (%ld of them probes) -> %s%c",
                    serial, (long)g_port_frame_no, (double)(t.QuadPart - s_ads_t0) * 1000.0 / s_qf, (long)g_port_frame_no - s_ads_f0,
                    s_ads_ps3, s_ads_probes, s_arm_from_cache ? "armed from R3_DOF_FAST_CACHE" : "armed by probes", 10);
            fflush(stderr);
        }
    }
    fast_rec rc = *r;
    ReleaseSRWLockExclusive(&s_flock);
    if (first) guard_begin(serial, mode != 2);
    if (first && mode != 2) fast_black_drop();
    if (first && mode != 2 && s_gpu && !s_gwithdrawn) {
        AcquireSRWLockExclusive(&s_gpulock);
        s_gwithdrawn = 1;
        ReleaseSRWLockExclusive(&s_gpulock);
    }
    if (first && mode != 2 && s_fuse) s_fuse_serial = serial;
    if (mode != 2) { t_in_job = serial; return 0; }
    if (first) fast_frame(&rc, &par, &g, check_ok);
    memset(&ctx->gpr[3], 0, sizeof ctx->gpr[3]);   /* the job's return value */
    ctx->pc = FAST_LINK;
    return 1;
}

/* R3_SEMFIX_CYCLE (default on, spu_channels.c): the job 419 serial an SPU
 * thread last entered tags its label-77 stores (the job's own store, the PC
 * fast path's, and a job manager completion after the job returned), so semfix
 * does not restore a second store of a cycle the RSX acquire already consumed. */
static __declspec(thread) uint32_t t_job_tag;
static uint32_t s_job_entry_off = 0xFFFFFFFFu;
static uint32_t dof_store_tag(uint32_t word)
{
    if (word == FAST_LABEL && t_job_tag && t_job_tag == s_fuse_serial) fuse_ps3_check(t_job_tag);   /* R3_DOF_FIRST_USE_GUARD */
    return word == FAST_LABEL ? t_job_tag : 0u;
}

/* job 419's serial when ctx branches to the job's entry, else 0 */
static uint32_t dof_job_entry(spu_context* ctx)
{
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK & ~3u;
    if (pc == FAST_LINK) return 0;
    uint32_t owner, off;
    dof_owner(ctx, pc, &owner, &off);
    if (owner != DOF_JOB_IMG) return 0;
    if (s_job_entry_off == 0xFFFFFFFFu) s_job_entry_off = off;   /* the first branch into the job is its entry */
    if (off != s_job_entry_off) return 0;
    return dof_be32((const uint8_t*)ctx->ls + FAST_DESC + 8);
}

/* R3_DOF_WAIT_RSX (default 0 since the 2026-11-16 G1 run; 1 = on): job 419 does not start before the RSX
 * has processed this cycle's label-77 reset release. The game blits the post
 * colour + depth into the FP16 light buffer at 0x30030000, releases label 77 =
 * DEAD0001, and the PPU kicks job 419 once an occlusion report it polls is
 * written; the job (or the PC fast path) reads that region and stores label 77 =
 * 0. Here the job sometimes starts while the RSX is still behind those blits:
 * every ADS purple frame in the 2026-11-16 logs is a "label 77 ... restored over
 * a late RSX release" followed by an [rt-cpu] "surface drawn after its flush"
 * on the composite's sample of 0x30030000, i.e. the job's fault flush read the
 * light buffer before the blits (FP16 lighting bytes blurred as BGRA8 =
 * magenta bands) and the blits landed afterwards. The first lane of a job
 * serial waits (bounded by R3_DOF_WAIT_RSX_MS, default 40) until the RSX is no
 * longer behind (spu_channels.c semfix_rsx_behind); three timeouts in a row
 * turn the wait off for the session. G1 (2026-11-16 15:20) disproved it: the
 * purple frames were stale semfix restores (R3_SEMFIX_CYCLE fixes those), the
 * gate then waited 40 ms on an RSX parked in the label-77 acquire and its lock
 * held the other serial's lanes -> off by default. */
extern int semfix_rsx_behind(uint32_t word);
static int s_rsxgate, s_rsxgate_ms = 40, s_rsxgate_tmo_run;
static volatile uint32_t s_rsxgate_serial;
static volatile long s_rsxgate_n, s_rsxgate_held, s_rsxgate_tmo;
static volatile long long s_rsxgate_ticks, s_rsxgate_max, s_rsxgate_next_log;
static SRWLOCK s_rsxgate_lock = SRWLOCK_INIT;

static void dof_stamp(char* b, size_t n)
{
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(b, n, "frame %ld %02u:%02u:%02u.%03u", (long)g_port_frame_no, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void dof_rsx_gate(uint32_t serial)
{
    if (!serial || serial == s_rsxgate_serial) return;
    AcquireSRWLockExclusive(&s_rsxgate_lock);   /* other lanes of the serial wait for the first */
    if (serial == s_rsxgate_serial || !s_rsxgate) { ReleaseSRWLockExclusive(&s_rsxgate_lock); return; }
    InterlockedIncrement(&s_rsxgate_n);
    if (semfix_rsx_behind(FAST_LABEL)) {
        LARGE_INTEGER t0, t; QueryPerformanceCounter(&t0);
        const long long cap = (long long)s_rsxgate_ms * s_qf / 1000;
        int timed_out = 0;
        for (unsigned spin = 0; semfix_rsx_behind(FAST_LABEL); spin++) {
            QueryPerformanceCounter(&t);
            if (t.QuadPart - t0.QuadPart > cap) { timed_out = 1; break; }
            if (spin < 256) YieldProcessor(); else SwitchToThread();
        }
        QueryPerformanceCounter(&t);
        const long long d = t.QuadPart - t0.QuadPart;
        const long n = InterlockedIncrement(&s_rsxgate_held);
        InterlockedAdd64(&s_rsxgate_ticks, d);
        if (d > s_rsxgate_max) s_rsxgate_max = d;
        char st[48]; dof_stamp(st, sizeof st);
        if (timed_out) {
            InterlockedIncrement(&s_rsxgate_tmo);
            fprintf(stderr, "[dof-sync] R3_DOF_WAIT_RSX: job 419 serial 0x%X (%s): RSX still behind label 77's release after %d ms -> job runs anyway%c",
                    serial, st, s_rsxgate_ms, 10);
            if (++s_rsxgate_tmo_run >= 3) {
                s_rsxgate = 0;
                fprintf(stderr, "[dof-sync] R3_DOF_WAIT_RSX: 3 timeouts in a row -> off for this session%c", 10);
            }
        } else {
            s_rsxgate_tmo_run = 0;
            if (n <= 16 || (n & (n - 1)) == 0)
                fprintf(stderr, "[dof-sync] R3_DOF_WAIT_RSX: job 419 serial 0x%X (%s) kicked before the RSX processed label 77's release (its input blits) -> held %.2f ms (%ld times)%c",
                        serial, st, (double)d * 1000.0 / s_qf, n, 10);
        }
        fflush(stderr);
    } else s_rsxgate_tmo_run = 0;
    s_rsxgate_serial = serial;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (s_rsxgate_held && t.QuadPart > s_rsxgate_next_log) {
        if (s_rsxgate_next_log)
            fprintf(stderr, "[dof-sync] R3_DOF_WAIT_RSX: %ld of %ld job 419 starts held for the RSX, avg %.2f max %.2f ms, %ld timeouts%c",
                    s_rsxgate_held, s_rsxgate_n, (double)s_rsxgate_ticks * 1000.0 / s_qf / s_rsxgate_held,
                    (double)s_rsxgate_max * 1000.0 / s_qf, s_rsxgate_tmo, 10);
        s_rsxgate_next_log = t.QuadPart + 10 * s_qf;
    }
    ReleaseSRWLockExclusive(&s_rsxgate_lock);
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
    if (lr == FAST_LINK && !t_in_hook && (s_rsxgate || g_semfix_store_tag_hook)) {
        const uint32_t serial = dof_job_entry(ctx);
        if (serial) t_job_tag = serial;
        if (s_rsxgate) dof_rsx_gate(serial);
    }
    if (s_fast && lr == FAST_LINK && !t_in_hook && fast_entry(ctx)) return 1;
    if (!dof_is_call_site(lr) || t_in_hook) return 0;
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK & ~3u;
    uint32_t owner, off;
    dof_owner(ctx, pc, &owner, &off);
    if (s_fast && lr == 0x16D8u && (s_fast_state == 0 || s_probe_serial)) fast_kernel_note(ctx, owner);
    if (s_graphlog) dof_graphlog(ctx, owner);
    if (s_hostjob && lr == 0x16D8u && !s_hj_off && hj_pre(ctx, owner)) {
        if (s_native && !s_trace && !s_verify && !s_toggle_ticks && s_cap_max <= 0 &&
            r3_native_dof_kernel(ctx, owner, off)) {
            hj_post(ctx);
            ctx->pc = lr;
            return 1;
        }
    }
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
    s_native = port_native_parse_bool(getenv("R3_NATIVE_DOF"), R3_NATIVE_DOF_DEFAULT);
    s_verify = env_int("R3_DOF_VERIFY", 0);
    s_native_job = port_native_parse_level(getenv("R3_NATIVE_DOF_JOB"), R3_NATIVE_DOF_JOB_DEFAULT);
#ifdef R3_NATIVE_DOF_JOB_BUILT
    g_r3dofj_level = s_native_job >= 2 ? 2 : 1;
    if (s_native_job && !g_r3dofj_native)
        fprintf(stderr, "[ndof] R3_NATIVE_DOF_JOB=%d: AVX2/FMA bulk loops compiled out, kernel 623 runs transpiled%c", s_native_job, 10);
#endif
    /* ADS60: =3 also arms the lean DMA path for job 419's own transfers
     * (spu_dma.h mfc_do_transfer_lean). Only when no per-transfer DMA
     * diagnostic is armed -- the lean path skips them. */
    if (s_native_job >= 3) {
        static const char* const diag[] = {
            "SPU_DMA_WATCH", "SPU_DMA_RANGE", "SPU_DMA_LAX", "SPU_DMACHK", "SPU_GETHIST",
            "SPU_PUTHIST", "SPU_MFC_TRACE", "SPU_DMA_SKIP_DUMP", "SPU_STACKEA_WATCH",
            "SPU_DMATRACE_ALL", "YDKJ_DMA_IMG", "SPU_DUMP_GET", "SPU_DSP_IMAGE_EA",
            "LBP_SKIP_NULL_DMA", "SPU_OVL_DIAG", "SPU_SMC_WATCH", "SPU_LS_WATCH",
            "SPU_LOCKGUARD", "SPU_POISON_DMA", "R3_DOF_LEAN_DMA_OFF" };
        const char* why = 0;
        for (unsigned i = 0; i < sizeof diag / sizeof diag[0] && !why; i++)
            if (getenv(diag[i])) why = diag[i];
        if (why)
            fprintf(stderr, "[ndof] R3_NATIVE_DOF_JOB=3: lean job-419 DMA path NOT armed (%s set)%c", why, 10);
        else {
            g_spu_dma_lean_img = DOF_JOB_IMG;
            fprintf(stderr, "[ndof] R3_NATIVE_DOF_JOB=3: lean job-419 DMA path armed%c", 10);
            if (s_native_job >= 4) {
                if (getenv("R3_DOF_BULK_DMA_OFF"))
                    fprintf(stderr, "[ndof] R3_NATIVE_DOF_JOB=4: bulk list DMA NOT armed (R3_DOF_BULK_DMA_OFF set)%c", 10);
                else {
                    g_spu_dma_bulk_img = DOF_JOB_IMG;
                    fprintf(stderr, "[ndof] R3_NATIVE_DOF_JOB=4: bulk job-419 list DMA armed%c", 10);
                }
            }
            /* R3_DOF_FAST_SUBMIT (default 1; 0 = off): also skip mfc_submit's per-command
             * diagnostics for job 419 (spu_dma.h g_spu_dma_submit_img). */
            if (port_native_parse_bool(getenv("R3_DOF_FAST_SUBMIT"), 1)) {
                static const char* const sdiag[] = {
                    "SPU_CMDHIST", "SPU_WATCHEA", "SPU_WATCHLSA", "SPU_ROWCOV", "SPU_VRAMPC",
                    "SPU_H2LSRC", "SPU_PUTEA", "SPU_DMATRACE", "SPU_DMATRACE_RAW", "SPU_CMDDUMP",
                    "YDKJ_CRI_CHAIN" };
                const char* swhy = 0;
                for (unsigned i = 0; i < sizeof sdiag / sizeof sdiag[0] && !swhy; i++)
                    if (getenv(sdiag[i])) swhy = sdiag[i];
                if (swhy)
                    fprintf(stderr, "[ndof] R3_DOF_FAST_SUBMIT: fast job-419 MFC submit NOT armed (%s set)%c", swhy, 10);
                else {
                    g_spu_dma_submit_img = DOF_JOB_IMG;
                    fprintf(stderr, "[ndof] R3_DOF_FAST_SUBMIT: fast job-419 MFC submit armed%c", 10);
                }
            }
        }
    }
#ifdef R3_NATIVE_DOF_JOB_BUILT
#else
    if (s_native_job) {
        const char* je = getenv("R3_NATIVE_DOF_JOB");
        if (je && *je)   /* the built-in default stays quiet on non-AVX2 builds */
            fprintf(stderr, "[ndof] R3_NATIVE_DOF_JOB=%d: kernel loops ignored, built without PORT_SPU_AVX2%c", s_native_job, 10);
        s_native_job = 0;
    }
#endif
    { const int ts = env_int("R3_NATIVE_DOF_TOGGLE", 0); if (ts > 0) s_toggle_ticks = (long long)ts * s_qf; }
    if (s_cap_max > 0) _mkdir("scratch\\dof_capture");
    s_graphlog = env_int("R3_DOF_GRAPHLOG", 0);
    s_hostjob = env_int("R3_DOF_HOSTJOB", 0);
    if (s_hostjob) {
        s_hj_vn = env_int("R3_DOF_HOSTJOB_VERIFY_N", 256);
        s_hj_every = env_int("R3_DOF_HOSTJOB_EVERY", 64); if (s_hj_every < 1) s_hj_every = 1;
        if (!s_graphlog) s_graphlog = 1;
        fprintf(stderr, "[dof-host] R3_DOF_HOSTJOB=%d: step-list decode shadow verify on (first %d process calls, then every %d-th; SPU path still renders)%c",
                s_hostjob, s_hj_vn, s_hj_every, 10);
    }
    s_fast = port_native_parse_bool(getenv("R3_DOF_GPU_APPROX"), R3_DOF_GPU_APPROX_DEFAULT);
    if (s_fast) {
        const size_t img = FAST_NPX * 4, scr = sizeof(float) * R3DOF_FAST_SCRATCH;
        s_fast_recheck = env_int("R3_DOF_GPU_APPROX_RECHECK", 3600);
        { const char* e = getenv("R3_DOF_GPU_APPROX_MINPSNR"); if (e && *e) s_fast_minpsnr = (float)atof(e); }
        s_fast_retry = env_int("R3_DOF_GPU_APPROX_RETRY", 60); if (s_fast_retry < 0) s_fast_retry = 0;
        s_fast_maxfail = env_int("R3_DOF_GPU_APPROX_MAXFAIL", 10); if (s_fast_maxfail < 1) s_fast_maxfail = 1;
        s_ramp_hold = env_int("R3_DOF_GPU_APPROX_RAMP_HOLD", 30); if (s_ramp_hold < 0) s_ramp_hold = 0;
        uint8_t** bufs[] = { &s_pc, &s_pz, &s_ps, &s_po, &s_wc, &s_wz, &s_wo };
        int ok = 1;
        for (unsigned i = 0; i < sizeof bufs / sizeof bufs[0]; i++) ok &= (*bufs[i] = (uint8_t*)_aligned_malloc(img, 64)) != NULL;
        ok &= (s_pscr = (float*)_aligned_malloc(scr, 64)) != NULL;
        ok &= (s_wscr = (float*)_aligned_malloc(scr, 64)) != NULL;
        ok &= (s_preg = (uint8_t*)_aligned_malloc(R3DOF_FAST_REGION, 64)) != NULL;
        if (!ok) { s_fast = 0; fprintf(stderr, "[dof-fast] R3_DOF_GPU_APPROX=1: out of memory, off%c", 10); }
        else {
            s_robust = port_native_parse_bool(getenv("R3_DOF_GPU_APPROX_ROBUST"), 1);
            if (s_robust) {
                extern void (*volatile g_spu_write_watch)(spu_context*, uint32_t, uint32_t, const uint8_t*);
                extern void (*volatile g_spu_igjob_return_hook)(spu_context*);
                g_spu_igjob_return_hook = fast_job_return;
                g_spu_write_watch = fast_write_watch;
            }
            g_semfix_spu_hook = fast_semfix_hook;
            fprintf(stderr, "[dof-fast] R3_DOF_GPU_APPROX=1 (Aim blur: PC fast): checking job 419 on the PS3 path first (3 probe frames), then the host approximation replaces it; recheck every %d frames, min PSNR %.0f dB, failed probe retried after %d frames (off after %d; not counted within %d frames of a zoom ramp, R3_DOF_GPU_APPROX_RAMP_HOLD); R3_DOF_GPU_APPROX_ROBUST=%d (%s)%c",
                    s_fast_recheck, s_fast_minpsnr, s_fast_retry, s_fast_maxfail, s_ramp_hold, s_robust,
                    s_robust ? "sync line learned from the job's own writes, zoom-ramp probes not counted" : "2026-11-15 rules", 10);
            s_block_ahead = port_native_parse_bool(getenv("R3_DOF_FAST_BLOCK_AHEAD"), 1);
            s_fgate = port_native_parse_bool(getenv("R3_DOF_FAST_RSX_GATE"), 1);
            s_fgate_ms = env_int("R3_DOF_FAST_RSX_GATE_MS", 20); if (s_fgate_ms < 1) s_fgate_ms = 1;
            s_bhold = port_native_parse_bool(getenv("R3_DOF_FAST_BLACK_HOLD"), 1);
            if (s_bhold && !(s_wgood = (uint8_t*)_aligned_malloc(img, 64))) s_bhold = 0;
            s_fuse = port_native_parse_bool(getenv("R3_DOF_FIRST_USE_GUARD"), 1);
            s_ramp_keep = port_native_parse_bool(getenv("R3_DOF_FAST_RAMP_KEEP"), 1);
            fast_cache_init();
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_CACHE=%d (arm from the last session's arming, no PS3-path probe phase on the first ADS), R3_DOF_FAST_RAMP_KEEP=%d (CoC-settling probes keep the good probes before them)%c", s_cache, s_ramp_keep, 10);
            fprintf(stderr, "[dof-fast] R3_DOF_FIRST_USE_GUARD=%d (PS3-path / first fast frame with FP16-garbled or black input, or CoC saturated everywhere -> neutral output)%c", s_fuse, 10);
            fprintf(stderr, "[dof-fast] R3_DOF_FAST_RSX_GATE=%d (light buffer read after the RSX's label-77 release, max %d ms), R3_DOF_FAST_BLACK_HOLD=%d (black input after a lit frame -> last good output)%c",
                    s_fgate, s_fgate_ms, s_bhold, 10);
    s_block_stale = port_native_parse_bool(getenv("R3_DOF_FAST_BLOCK_STALE"), 1);
            s_guard = port_native_parse_bool(getenv("R3_DOF_GUEST_GUARD"), 1);
            s_guard_wait_ms = env_int("R3_DOF_GUARD_WAIT_MS", 10); if (s_guard_wait_ms < 0) s_guard_wait_ms = 0;
            s_guard_max_ms = env_int("R3_DOF_GUARD_MAX_MS", 100); if (s_guard_max_ms < 1) s_guard_max_ms = 1;
            if (s_guard && (!(s_gc[0].buf = (uint8_t*)_aligned_malloc(R3DOF_FAST_REGION, 64)) || !(s_gc[1].buf = (uint8_t*)_aligned_malloc(R3DOF_FAST_REGION, 64)))) s_guard = 0;
            fprintf(stderr, "[dof-guard] R3_DOF_GUEST_GUARD=%d: the composite decodes job 419's output exactly as its writer left it (overwritten lines restored and logged; PS3-path sample waits up to %d ms for the job, kept %d ms)%c",
                    s_guard, s_guard_wait_ms, s_guard_max_ms, 10);
            s_gpu = port_native_parse_bool(getenv("R3_DOF_GPU"), 1);
            if (s_gpu && !s_robust) { s_gpu = 0; fprintf(stderr, "[dof-gpu] R3_DOF_GPU needs R3_DOF_GPU_APPROX_ROBUST=1: off%c", 10); }
            if (s_gpu && !(s_gref = (uint8_t*)_aligned_malloc(img, 64))) s_gpu = 0;
            if (s_gpu) {
                { const char* e = getenv("R3_DOF_GPU_MINPSNR"); if (e && *e) s_gpu_minpsnr = (float)atof(e); }
                s_gpu_maxmiss = env_int("R3_DOF_GPU_MAXMISS", 3); if (s_gpu_maxmiss < 1) s_gpu_maxmiss = 1;
                s_gcheck_maxfail = env_int("R3_DOF_GPU_CHECK_MAXFAIL", 3); if (s_gcheck_maxfail < 1) s_gcheck_maxfail = 1;
                s_gcheck_retry = env_int("R3_DOF_GPU_CHECK_RETRY", 300); if (s_gcheck_retry < 0) s_gcheck_retry = 0;
    s_gcheck_stale = env_int("R3_DOF_GPU_CHECK_STALE", 15); if (s_gcheck_stale < 0) s_gcheck_stale = 0;
                s_gcheck_ramp_hold = env_int("R3_DOF_GPU_CHECK_RAMP_HOLD", 4); if (s_gcheck_ramp_hold < 0) s_gcheck_ramp_hold = 0;
                fprintf(stderr, "[dof-gpu] R3_DOF_GPU=1: armed aim-blur frames are computed by the RSX (Vulkan compute from the light buffer surface, no readback); CPU comparison on a settled own-block frame, min PSNR %.0f dB, retried every %d frames, off after %d failed checks or %d unserved requests%c",
                        s_gpu_minpsnr, s_gcheck_retry, s_gcheck_maxfail, s_gpu_maxmiss, 10);
            }
        }
    }
    { extern volatile long* volatile g_semfix_frame_ptr; g_semfix_frame_ptr = &g_port_frame_no; }
    s_rsxgate = port_native_parse_bool(getenv("R3_DOF_WAIT_RSX"), 0);
    {
        const char* e = getenv("R3_SEMFIX_CYCLE");
        if (!(e && atoi(e) == 0)) {
            g_semfix_store_tag_hook = dof_store_tag;
            fprintf(stderr, "[semfix] R3_SEMFIX_CYCLE=1: label-77 stores tagged with their job 419 serial (no restore of an already consumed cycle's store)%c", 10);
        }
    }
    if (s_rsxgate) {
        s_rsxgate_ms = env_int("R3_DOF_WAIT_RSX_MS", 40); if (s_rsxgate_ms < 1) s_rsxgate_ms = 1;
        fprintf(stderr, "[dof-sync] R3_DOF_WAIT_RSX=1: job 419 starts only after the RSX processed label 77's release (max %d ms)%c", s_rsxgate_ms, 10);
    }
    if (s_trace || s_cap_max > 0 || s_native || s_verify || s_toggle_ticks || s_graphlog || s_fast || s_rsxgate || g_semfix_store_tag_hook) {
        g_spu_native_ib_hook = dof_hook;
        fprintf(stderr, "[ndof] hook on: trace=%d capture=%d native=%d job=%d verify=%d toggle=%llds graphlog=%d hostjob=%d%c", s_trace, s_cap_max,
                s_native, s_native_job, s_verify, s_qf ? s_toggle_ticks / s_qf : 0, s_graphlog, s_hostjob, 10);
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
    case 621: return r3dof_gen_621; case 622: return r3dof_gen_622;
    case 623:
#ifdef R3_NATIVE_DOF_JOB_BUILT
        if (s_native_job) return r3dof_gen_job_623;
#endif
        return r3dof_gen_623;
    default: return NULL;
    }
}
int r3_native_dof_job_level(void) { return s_native_job; }

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
