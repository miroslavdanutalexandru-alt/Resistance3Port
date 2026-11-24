// R3 port: native replacements for the game's generic libc routines
// (port of R2's project/r2_hot.cpp, R2_HOT_NATIVE).
//
// The lifted versions run every byte/word through the guest-memory accessors
// (bounds check, coherence-bitmap probe, byte swap each); these do the same
// work on the host copy of guest memory directly. Guest memory is raw
// big-endian bytes at vm_base + ea, so a byte copy, fill or compare needs no
// swapping at all.
//
// Each lifted function starts with `if (r3_hot_xxx(ctx)) return;` (hooks in
// tools/hooks/r3.json, applied by hand to recompiled/ppu_recomp_003.cpp). The
// native routine handles the call only in the "plain" domain and returns 0
// otherwise, so the original lifted body runs:
//   - every byte touched, widened to the aligned 8-byte blocks the lifted
//     word-at-a-time loops may over-read, lies in [0x10000, 0xDFFFFFF8) (the
//     lifted accessors' inline range) and outside the gcm poll-word block;
//   - no destination line is reserved by an SPU (g_spu_coh_bitmap_p): such a
//     store must take the out-of-line path that raises the lost-reservation
//     event;
//   - source and destination do not overlap (memcpy/strcpy);
//   - lengths below 2 GiB.
// R3's libc (same newlib family as R2's): memset 0xB604D8 and memcmp 0xB605B0
// are word-for-word R2's 0xA63258 / 0xA63330; memcpy 0xB558E8, strcmp 0xB5D514
// and strcpy 0xB5D668 are the same algorithms, but strcmp/strcpy build the
// 0x7F7F7F7F zero-byte constant inline (lis/ori) instead of loading it from the
// TOC, so there is no TOC check. strlen and R2's game strcmp (0x6BB908) have no
// R3 counterpart that was found.
// Equivalence is at the ABI level: memory, r3 and every non-volatile register
// (r1, r2, r13-r31, lr, f14-f31, v20-v31, cr2-cr4) match the lifted code.
// The scratch registers the lifted body would also have changed (r0, r4-r12,
// ctr, xer, cr0/1/5-7) are left alone -- no caller may depend on them.
// scratch/hot_r3/difftest proves it on random and edge-case inputs.
//
//   R3_HOT_NATIVE=1           the routines below native (DEFAULT on, as R2:
//                             difftest 0 failures)
//   R3_HOT_NATIVE=0           all off: the lifted code runs, exactly as before
//   R3_HOT_MEMCPY=0  R3_HOT_MEMSET=0  R3_HOT_STRCMP=0  R3_HOT_STRCPY=0
//   R3_HOT_MEMCMP=0           one routine off (under R3_HOT_NATIVE=1)
//   R3_HOT_LOG=1              one [hot] block every ~10 s (default 0: none)
//   R3_HOT_SAMPLE=<n>         time 1 call in n per thread (default 4096): the
//                             lifted body, then the native one on the same
//                             inputs (every replacement is idempotent), and
//                             check r3 agrees -- [hot] reports the estimated
//                             time saved and any mismatch. A mismatch is run
//                             once more on the same inputs: if both sides
//                             repeat their results, the routine is DISABLED
//                             (lifted from then on); otherwise the inputs
//                             changed under a concurrent writer and it counts
//                             as race=. 0 = no sampling.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include "ppu_recomp.h"
#include "port_native_summary.h"

/* lifted functions come from ppu_recomp.h (C++ linkage) */
void func_00B558E8(ppu_context* ctx);   /* memcpy */
void func_00B604D8(ppu_context* ctx);   /* memset */
void func_00B5D514(ppu_context* ctx);   /* strcmp */
void func_00B5D668(ppu_context* ctx);   /* strcpy */
void func_00B605B0(ppu_context* ctx);   /* memcmp */

namespace {

enum { H_MEMCPY, H_MEMSET, H_STRCMP, H_STRCPY, H_MEMCMP, H_N };
const char* const k_name[H_N] = { "memcpy", "memset", "strcmp", "strcpy", "memcmp" };
const char* const k_env[H_N] = { "R3_HOT_MEMCPY", "R3_HOT_MEMSET", "R3_HOT_STRCMP", "R3_HOT_STRCPY", "R3_HOT_MEMCMP" };

/* ---- the domain ---- */
constexpr uint64_t kLo = 0x10000u, kHi = 0xDFFFFFF8u;
constexpr uint64_t kPollLo = 0x20002000u, kPollHi = 0x20002010u;

/* bytes [ea, ea+n), widened to whole 8-byte blocks, all in the inline range */
inline bool span_ok(uint32_t ea, uint64_t n)
{
    const uint64_t a = ea & ~7ull, b = ((uint64_t)ea + n + 7) & ~7ull;
    return a >= kLo && b <= kHi && !(a < kPollHi && b > kPollLo);
}
inline bool blk_ok(uint32_t ea) { return span_ok(ea, 1); }

/* no SPU-reserved 128-byte line in [ea, ea+n) */
inline bool coh_ok(uint32_t ea, uint64_t n)
{
    if (!g_spu_coh_armed || !n) return true;
    const unsigned char* bm = g_spu_coh_bitmap_p;
    const uint32_t l1 = (uint32_t)(((uint64_t)ea + n - 1) >> 7);
    for (uint32_t l = ea >> 7; l <= l1; l++) {
        if (!(l & 7) && l + 7 <= l1) { if (bm[l >> 3]) return false; l += 7; continue; }
        if ((bm[l >> 3] >> (l & 7)) & 1u) return false;
    }
    return true;
}

/* widened ranges intersect */
inline bool overlap(uint32_t a, uint64_t na, uint32_t b, uint64_t nb)
{
    const uint64_t a0 = a & ~7ull, a1 = ((uint64_t)a + na + 7) & ~7ull;
    const uint64_t b0 = b & ~7ull, b1 = ((uint64_t)b + nb + 7) & ~7ull;
    return a0 < b1 && b0 < a1;
}

inline bool has_zero(uint64_t w) { return ((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull) != 0; }

/* length of the NUL-terminated string at s, staying in the inline range;
 * -1 if it runs out of it */
inline int64_t bounded_strlen(uint32_t s)
{
    if (s < kLo || s >= kHi) return -1;
    const uint64_t lim = kHi - s;
    const size_t len = strnlen((const char*)vm_base + s, (size_t)lim);
    if (len >= lim) return -1;
    if (!span_ok(s, (uint64_t)len + 1)) return -1;
    return (int64_t)len;
}

/* ---- the routines: 1 = done (ctx updated), 0 = run the lifted body ---- */
int nat_memcpy(ppu_context* c, uint64_t* bytes)
{
    const uint64_t n = c->gpr[5];
    if (n >= 0x80000000ull) return 0;
    const uint32_t d = (uint32_t)c->gpr[3], s = (uint32_t)c->gpr[4];
    if (n) {
        if (!span_ok(s, n) || !span_ok(d, n) || overlap(d, n, s, n) || !coh_ok(d, n)) return 0;
        memcpy(vm_base + d, vm_base + s, (size_t)n);
    }
    c->gpr[3] = d;
    *bytes = n;
    return 1;
}

/* memset leaves r3 as passed; with a high word set the lifted loop walks a
 * 64-bit range -- decline */
int nat_memset(ppu_context* c, uint64_t* bytes)
{
    const uint64_t n = c->gpr[5], r3 = c->gpr[3];
    if (n >= 0x80000000ull || (r3 >> 32)) return 0;
    const uint32_t d = (uint32_t)r3;
    if (n) {
        if (!span_ok(d, n) || !coh_ok(d, n)) return 0;
        memset(vm_base + d, (int)(uint8_t)c->gpr[4], (size_t)n);
    }
    *bytes = n;
    return 1;
}

int nat_strcpy(ppu_context* c, uint64_t* bytes)
{
    const uint32_t d = (uint32_t)c->gpr[3], s = (uint32_t)c->gpr[4];
    const int64_t len = bounded_strlen(s);
    if (len < 0) return 0;
    const uint64_t n = (uint64_t)len + 1;
    if (!span_ok(d, n) || overlap(d, n, s, n) || !coh_ok(d, n)) return 0;
    memcpy(vm_base + d, vm_base + s, (size_t)n);
    c->gpr[3] = d;
    *bytes = n;
    return 1;
}

int nat_memcmp(ppu_context* c, uint64_t* bytes)
{
    const uint32_t n = (uint32_t)c->gpr[5];
    if (n >= 0x80000000u) return 0;
    const uint32_t a = (uint32_t)c->gpr[3], b = (uint32_t)c->gpr[4];
    int r = 0;
    if (n) {
        if (!span_ok(a, n) || !span_ok(b, n)) return 0;
        r = memcmp(vm_base + a, vm_base + b, n);
    }
    c->gpr[3] = (uint64_t)(int64_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
    *bytes = n;
    return 1;
}

/* unsigned-char strcmp returning -1/0/1 */
int nat_strcmp(ppu_context* c, uint64_t* bytes)
{
    const uint32_t a = (uint32_t)c->gpr[3], b = (uint32_t)c->gpr[4];
    const uint8_t* m = vm_base;
    int r = 0;
    for (uint32_t i = 0;; ) {
        const uint32_t pa = a + i, pb = b + i;
        if ((i == 0 || !(pa & 7)) && !blk_ok(pa)) return 0;
        if ((i == 0 || !(pb & 7)) && !blk_ok(pb)) return 0;
        if (!((pa | pb) & 7)) {
            uint64_t wa, wb; memcpy(&wa, m + pa, 8); memcpy(&wb, m + pb, 8);
            if (wa == wb && !has_zero(wa)) { i += 8; continue; }
        }
        const uint8_t x = m[pa], y = m[pb];
        if (x != y) { r = x < y ? -1 : 1; *bytes = i; break; }
        if (!x) { *bytes = i; break; }
        i++;
    }
    c->gpr[3] = (uint64_t)(int64_t)r;
    return 1;
}

int env_int(const char* n, int def)
{
    const char* e = getenv(n);
    if (!e || !*e) return def;
    if (!_stricmp(e, "off") || !_stricmp(e, "false") || !_stricmp(e, "no")) return 0;
    if (!_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes")) return 1;
    return atoi(e);
}

std::atomic<int> g_init{0};
std::atomic<unsigned> g_on{0};   /* bit per routine; a sampled mismatch clears it */
unsigned g_sample = 4096;
int g_log = 0;

struct Stat {
    std::atomic<long long> calls{0}, native{0}, bytes{0}, samples{0}, cyc_orig{0}, cyc_nat{0}, mismatch{0}, races{0};
};
Stat g_st[H_N];
std::atomic<unsigned long long> g_next_log{0};
long long g_qpc0, g_qpf; unsigned long long g_tsc0;

struct Tls {
    unsigned calls[H_N], native[H_N], samp[H_N];
    unsigned long long bytes[H_N];
    unsigned tick;
    int bypass;
};
thread_local Tls t_tls;

void init_once()
{
    int z = 0;
    if (!g_init.compare_exchange_strong(z, 1)) { while (g_init.load() != 2) _mm_pause(); return; }
    unsigned on = 0;
    if (port_native_parse_bool(getenv("R3_HOT_NATIVE"), 1))
        for (int i = 0; i < H_N; i++) if (env_int(k_env[i], 1)) on |= 1u << i;
    const int s = env_int("R3_HOT_SAMPLE", 4096);
    g_sample = s <= 0 ? 0 : (unsigned)s;
    g_log = env_int("R3_HOT_LOG", 0);
    LARGE_INTEGER f, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q);
    g_qpf = f.QuadPart; g_qpc0 = q.QuadPart; g_tsc0 = __rdtsc();
    g_next_log.store(GetTickCount64() + 10000);
    g_on = on;
    fprintf(stderr, "[hot] native routines:");
    for (int i = 0; i < H_N; i++) fprintf(stderr, " %s=%u", k_name[i], (on >> i) & 1u);
    fprintf(stderr, " (R3_HOT_NATIVE=0: all lifted) sample 1/%u\n", g_sample);
    g_init.store(2, std::memory_order_release);
}

inline bool enabled(int h)
{
    if (__builtin_expect(g_init.load(std::memory_order_acquire) != 2, 0)) init_once();
    return (g_on.load(std::memory_order_relaxed) >> h) & 1u;
}

void report()
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double secs = (double)(q.QuadPart - g_qpc0) / (double)g_qpf;
    const double tsc_per_ms = secs > 0 ? (double)(__rdtsc() - g_tsc0) / (secs * 1000.0) : 3.0e6;
    static long long l_calls[H_N], l_nat[H_N], l_bytes[H_N];
    double saved_total = 0;
    for (int h = 0; h < H_N; h++) {
        Stat& s = g_st[h];
        const long long c = s.calls.load(), n = s.native.load(), b = s.bytes.load();
        const long long dc = c - l_calls[h], dn = n - l_nat[h], db = b - l_bytes[h];
        l_calls[h] = c; l_nat[h] = n; l_bytes[h] = b;
        if (!dc) continue;
        const long long ns = s.samples.load();
        const double co = ns ? (double)s.cyc_orig.load() / ns : 0, cn = ns ? (double)s.cyc_nat.load() / ns : 0;
        const double saved_ms = ns ? (double)dn * (co - cn) / tsc_per_ms : 0;
        saved_total += saved_ms;
        fprintf(stderr, "[hot] %-7s calls=%lld native=%.1f%% KB=%lld | cyc/call lifted=%.0f native=%.0f (n=%lld) "
                        "saved~%.1f ms/10s mismatch=%lld race=%lld\n",
                k_name[h], dc, 100.0 * (double)dn / (double)dc, db >> 10, co, cn, ns, saved_ms, s.mismatch.load(), s.races.load());
    }
    if (saved_total > 0) fprintf(stderr, "[hot] total saved ~%.1f ms per 10 s across all PPU threads\n", saved_total);
}

void flush(Tls& t)
{
    for (int h = 0; h < H_N; h++) {
        if (!t.calls[h]) continue;
        g_st[h].calls.fetch_add(t.calls[h], std::memory_order_relaxed);
        g_st[h].native.fetch_add(t.native[h], std::memory_order_relaxed);
        g_st[h].bytes.fetch_add((long long)t.bytes[h], std::memory_order_relaxed);
        t.calls[h] = t.native[h] = 0; t.bytes[h] = 0;
    }
    if (!g_log) return;
    const unsigned long long now = GetTickCount64();
    unsigned long long nx = g_next_log.load(std::memory_order_relaxed);
    if (now >= nx && g_next_log.compare_exchange_strong(nx, now + 10000)) report();
}

typedef int (*nat_fn)(ppu_context*, uint64_t*);

/* A sampled call whose lifted (r3 l1) and native (r3 n1) results differ:
 * run both once more on the same inputs. 2 = confirmed (each side repeats
 * its own result and they still differ: a real bug, disable), 1 = the inputs
 * changed between runs (another thread wrote the buffers): ignore. */
int sample_verdict(const ppu_context* in, uint64_t l1, uint64_t n1, nat_fn nat, void (*lifted)(ppu_context*))
{
    Tls& t = t_tls;
    ppu_context l2 = *in, n3 = *in;
    uint64_t b = 0;
    const int saved = t.bypass;
    t.bypass = 1; lifted(&l2); t.bypass = saved;
    if (!nat(&n3, &b)) return 1;
    return (l2.gpr[3] == l1 && n3.gpr[3] == n1 && n1 != l1) ? 2 : 1;
}

int run(int h, ppu_context* c, nat_fn nat, void (*lifted)(ppu_context*))
{
    Tls& t = t_tls;
    if (t.bypass) return 0;
    t.calls[h]++;
    if ((++t.tick & 1023u) == 0) flush(t);
    uint64_t bytes = 0;
    if (g_sample && ++t.samp[h] >= g_sample) {
        t.samp[h] = 0;
        ppu_context in = *c;
        ppu_context tmp = in;
        if (!nat(&tmp, &bytes)) return 0;          /* outside the domain: the lifted body runs */
        /* time the lifted body on the real ctx, then the native one again on
         * a copy of the inputs (no overlap in the domain, so redoing a copy or
         * fill is a no-op) */
        const unsigned long long t0 = __rdtsc();
        t.bypass = 1; lifted(c); t.bypass = 0;
        const unsigned long long t1 = __rdtsc();
        ppu_context n2 = in;
        nat(&n2, &bytes);
        const unsigned long long t2 = __rdtsc();
        Stat& s = g_st[h];
        s.samples.fetch_add(1, std::memory_order_relaxed);
        s.cyc_orig.fetch_add((long long)(t1 - t0), std::memory_order_relaxed);
        s.cyc_nat.fetch_add((long long)(t2 - t1), std::memory_order_relaxed);
        if (n2.gpr[3] != c->gpr[3]) {
            const int v = sample_verdict(&in, c->gpr[3], n2.gpr[3], nat, lifted);
            if (v == 1) {
                s.races.fetch_add(1, std::memory_order_relaxed);
            } else {
                const long long k = s.mismatch.fetch_add(1, std::memory_order_relaxed);
                if (k < 8)
                    fprintf(stderr, "[hot] MISMATCH %s r3 lifted=0x%llX native=0x%llX (in r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                            k_name[h], (unsigned long long)c->gpr[3], (unsigned long long)n2.gpr[3],
                            (unsigned long long)in.gpr[3], (unsigned long long)in.gpr[4], (unsigned long long)in.gpr[5]);
                if ((g_on.fetch_and(~(1u << h)) >> h) & 1u)
                    fprintf(stderr, "[hot] %s DISABLED after a confirmed mismatch: the lifted code runs from now on\n", k_name[h]);
            }
        }
        t.native[h]++; t.bytes[h] += bytes;
        return 1;                                   /* the lifted body already ran */
    }
    if (!nat(c, &bytes)) return 0;
    t.native[h]++; t.bytes[h] += bytes;
    return 1;
}

}  // namespace

#define R3_HOT_ENTRY(fn, h, nat, lifted) \
    extern "C" int fn(ppu_context* ctx) { return enabled(h) ? run(h, ctx, nat, lifted) : 0; }
R3_HOT_ENTRY(r3_hot_memcpy, H_MEMCPY, nat_memcpy, func_00B558E8)
R3_HOT_ENTRY(r3_hot_memset, H_MEMSET, nat_memset, func_00B604D8)
R3_HOT_ENTRY(r3_hot_strcmp, H_STRCMP, nat_strcmp, func_00B5D514)
R3_HOT_ENTRY(r3_hot_strcpy, H_STRCPY, nat_strcpy, func_00B5D668)
R3_HOT_ENTRY(r3_hot_memcmp, H_MEMCMP, nat_memcmp, func_00B605B0)

/* difftest entry: the native routine alone (no switches, stats or sampling) */
extern "C" int r3_hot_native_only(int h, ppu_context* ctx)
{
    static const nat_fn k[H_N] = { nat_memcpy, nat_memset, nat_strcmp, nat_strcpy, nat_memcmp };
    uint64_t b = 0;
    return h >= 0 && h < H_N ? k[h](ctx, &b) : 0;
}

/* difftest entry: the sampled-mismatch confirmation (0 1 2 as sample_verdict) */
extern "C" int r3_hot_sample_verdict(const ppu_context* in, uint64_t l1, uint64_t n1,
                                     int (*nat)(ppu_context*, uint64_t*), void (*lifted)(ppu_context*))
{
    return sample_verdict(in, l1, n1, nat, lifted);
}
