/* Resistance port: CPU detection, CPU profile and soft thread placement.
 * Included by main.cpp; port_cpu_profile_init() runs first thing in main(),
 * before any subsystem starts or reads its switches.
 *
 * Detection: physical cores, logical CPUs, SMT and hybrid P/E cores
 * (GetLogicalProcessorInformationEx, EfficiencyClass), counted inside the
 * process affinity mask, so "start /affinity F" is seen as a 2-core CPU.
 *
 * Profile: PORT_CPU_PROFILE=auto (default) | low | mid | high | off.
 *   auto: low <= 4 physical cores, mid 5-7, high >= 8 (hybrid: P + E/2).
 *   The profile only sets the existing env switches the user has not set:
 *   low  GCM_VBLANK_SPIN_US=0 R2_PACE_SPIN_US=100 RSX_SPIN_US=50
 *        RSX_PERF_VK_SUBMIT_THREAD=0 RSX_VK_PIPELINE_THREADS=1 (2 with >= 8
 *        logical CPUs) PS3_PRIO_BOOST=0, and below 8 logical CPUs
 *        PORT_RSX_PRIO=1 (rsx::thread above normal; needs thread placement)
 *        (SPURS_AUDIO_BOOST_FORCE is now 1 by default on every CPU: the synth
 *        lane keeps its priority boost. Opt out: SPURS_AUDIO_BOOST_FORCE=0,
 *        or PORT_PLACE_AUDIO=0, which sets it to 0 with any profile but off)
 *   mid  GCM_VBLANK_SPIN_US=100 R2_PACE_SPIN_US=250
 *   high nothing (the shipped defaults).
 *   off: no switches and no placement -- exactly the old behaviour.
 *   The low-core no-spin waits are a separate switch, PORT_NOSPIN
 *   (sys_ppu_thread.c ps3_host_nospin; scratch\cpu_nospin.md).
 *
 * Placement (PORT_THREAD_PLACEMENT=off disables; off with PORT_CPU_PROFILE=off):
 *   hybrid CPUs with >= 4 P-cores: process default CPU sets = the P-cores, so
 *   the PPU, SPU, FIFO walker and RSX engine threads stay off the E-cores;
 *   background threads (monitors, pipeline compilers) get all CPUs back and a
 *   lower priority. The main PPU thread, the RSX engine thread (rsx::thread in
 *   RPCS3 RSX mode), the SPURS audio synth lane and the FIFO walker get ideal
 *   processors on different physical cores -- P-cores of the largest L3
 *   domain (Ryzen CCX/CCD), core 0 last -- a soft hint; the scheduler still
 *   moves them, unless main_core_keeper pins them (>= 12 logical CPUs or
 *   PORT_MAIN_CPU). PORT_PLACE_AUDIO=0: no synth-lane slot (the old layout). */
#ifdef _WIN32
#include <intrin.h>
#include <tlhelp32.h>
#include <wchar.h>
#include "port_cpu_sets.h"

enum { PORT_CPU_OFF = 0, PORT_CPU_LOW, PORT_CPU_MID, PORT_CPU_HIGH };
static int s_cpu_profile = PORT_CPU_OFF;
static int s_cpu_place = 0;
static int s_rsx_prio = 0;   /* PORT_RSX_PRIO */

static struct {
    int sys_phys, sys_logical;      /* the whole CPU */
    int phys, logical;              /* inside the process affinity mask */
    int smt, hybrid, pcores, ecores;
    int ncore;
    DWORD_PTR core_mask[64];        /* available logical CPUs of each core, P-cores first */
    BYTE core_eff[64];
    DWORD_PTR pmask;                /* logical CPUs of available P-cores (all when not hybrid) */
    DWORD_PTR affinity;
    int nl3;                        /* L3 caches (Ryzen CCX/CCD) seen inside the affinity mask */
    DWORD_PTR l3_mask[16];
    BYTE core_l3[64];               /* L3 index of each core_mask[] entry (0 when unknown) */
} s_cpu;

static int port_popcount(DWORD_PTR m) { int n = 0; for (; m; m &= m - 1) n++; return n; }

static void port_cpu_detect(void)
{
    DWORD_PTR pm = 0, sm = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) pm = ~(DWORD_PTR)0;
    s_cpu.affinity = pm;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    BYTE* buf = len ? (BYTE*)malloc(len) : NULL;
    BYTE maxeff = 0;
    struct { DWORD_PTR m; BYTE eff; } cores[64]; int nc = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Relationship == RelationProcessorCore) {
                const GROUP_AFFINITY& g = e->Processor.GroupMask[0];
                s_cpu.sys_phys++;
                s_cpu.sys_logical += port_popcount(g.Mask);
                const DWORD_PTR m = g.Group == 0 ? (g.Mask & pm) : 0;
                if (m && nc < 64) {
                    cores[nc].m = m; cores[nc].eff = e->Processor.EfficiencyClass; nc++;
                    if (e->Processor.EfficiencyClass > maxeff) maxeff = e->Processor.EfficiencyClass;
                }
            }
            off += e->Size;
        }
    }
    free(buf);
    if (!nc) {   /* no topology: treat every logical CPU as a core */
        SYSTEM_INFO si; GetSystemInfo(&si);
        s_cpu.sys_phys = s_cpu.sys_logical = (int)si.dwNumberOfProcessors;
        for (int i = 0; i < 64 && nc < 64; i++)
            if (pm >> i & 1) { cores[nc].m = (DWORD_PTR)1 << i; cores[nc].eff = 0; nc++; }
    }
    BYTE mineff = 255;
    for (int i = 0; i < nc; i++) if (cores[i].eff < mineff) mineff = cores[i].eff;
    s_cpu.hybrid = nc && mineff != maxeff;
    /* P-cores first, in topology order */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nc; i++) {
            const int p = cores[i].eff == maxeff;
            if (pass == 0 ? !p : p) continue;
            s_cpu.core_mask[s_cpu.ncore] = cores[i].m; s_cpu.core_eff[s_cpu.ncore] = cores[i].eff; s_cpu.ncore++;
            if (p) { s_cpu.pcores++; s_cpu.pmask |= cores[i].m; } else s_cpu.ecores++;
            s_cpu.phys++;
            s_cpu.logical += port_popcount(cores[i].m);
            if (port_popcount(cores[i].m) > 1) s_cpu.smt = 1;
        }
    if (!s_cpu.hybrid) s_cpu.ecores = 0;
    /* L3 domains (Ryzen CCX / CCD; one on Intel and on single-CCX Ryzen):
     * the hot-thread hints below keep main / RSX / synth inside one. */
    len = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
    buf = len ? (BYTE*)malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0 &&
                (e->Cache.GroupMask.Mask & pm) && s_cpu.nl3 < 16)
                s_cpu.l3_mask[s_cpu.nl3++] = e->Cache.GroupMask.Mask & pm;
            off += e->Size;
        }
    }
    free(buf);
    for (int i = 0; i < s_cpu.ncore; i++)
        for (int j = 0; j < s_cpu.nl3; j++)
            if (s_cpu.core_mask[i] & s_cpu.l3_mask[j]) { s_cpu.core_l3[i] = (BYTE)j; break; }
}

static void port_cpu_setdef(const char* name, const char* value, char* log, size_t cap)
{
    const char* cur = getenv(name);
    if (cur) { /* the user's (or settings.bat's) value wins */
        size_t n = strlen(log); snprintf(log + n, cap - n, " %s=%s(user)", name, cur); return; }
    _putenv_s(name, value);
    size_t n = strlen(log); snprintf(log + n, cap - n, " %s=%s", name, value);
}

/* PORT_THREAD_PLACEMENT=off or PORT_CPU_PROFILE=off: no placement anywhere. */
extern "C" int port_thread_placement_on(void) { return s_cpu_place; }

/* P-cores on hybrid CPUs (port_cpu_sets.h, the same rules in all three ports):
 * the threads the frame rate hangs on -- main guest thread, raw SPUs, SPURS
 * lanes, RSX FIFO / render / engine threads (rsx::thread, FIFO walker), audio
 * mixer -- get thread CPU sets = the usable P-cores and EcoQoS off, as many
 * of them as the usable P-cores allow (port_pcore_role_ok). The process
 * default sets do the same only with >= 4 usable P-cores, and threads that
 * later get a background placement or an explicit affinity are not covered by
 * them. While main_core_keeper (debugtools.inc) hard-pins, the hot threads are
 * planned against its rest mask, the threads it pins get no CPU sets, and the
 * default sets are dropped when fewer than 4 usable P-cores are left in it.
 * PORT_CPU_PCORES=off (older name PORT_PCORE_HOT) off; no-op on CPUs that are
 * not hybrid. PORT_THREAD_PRIORITY=on: main / RSX threads above normal. */
static port_pcore_plan s_pcore = { 0, 0, 0, 0, 0, 0, 0, { 0 }, "not started" };
static port_pcore_plan s_pcore_rest;              /* the hot threads' plan while the keeper hard-pins */
static const port_pcore_plan* volatile s_phot = &s_pcore;
static int s_pcore_prio = 0;
static int s_keeper_expected = 0, s_default_sets = 0;
static DWORD s_place_main_tid = 0;
extern "C" volatile long port_keeper_state;           /* debugtools.inc main_core_keeper */
extern "C" volatile unsigned long long port_keeper_rest;
extern "C" volatile unsigned long port_keeper_tid[4];
static int port_keeper_pinned(DWORD tid)
{
    if (!tid || port_keeper_state != 1) return 0;
    for (int i = 0; i < 4; i++) if (port_keeper_tid[i] == tid) return 1;
    return 0;
}
/* [native] summary (main.cpp): NULL = P-core placement in effect, else why not */
static const char* port_pcore_why(void) { return s_pcore.why_off; }
extern "C" unsigned long long port_cpu_pmask(void) { return s_cpu.hybrid ? (unsigned long long)s_cpu.pmask : 0; }
/* Lowest logical CPU of physical core #core (P-cores first, topology order,
 * inside the process affinity mask); -1 when there is no such core. */
extern "C" int port_cpu_core_first(int core)
{
    if (core < 0 || core >= s_cpu.ncore || !s_cpu.core_mask[core]) return -1;
    int c = 0; for (DWORD_PTR m = s_cpu.core_mask[core]; !(m & 1); m >>= 1) c++;
    return c;
}

/* Background threads: lower priority, and every CPU (E-cores included). */
extern "C" void port_place_background(HANDLE th)
{
    if (!s_cpu_place) return;
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    if (s_cpu.hybrid) {
        typedef BOOL (WINAPI *stscs_t)(HANDLE, const ULONG*, ULONG);
        static stscs_t stscs = (stscs_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadSelectedCpuSets");
        static ULONG all[256]; static ULONG nall = 0; static int init = 0;
        if (!init) {
            init = 1;
            typedef BOOL (WINAPI *gscsi_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
            gscsi_t g = (gscsi_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation");
            ULONG need = 0;
            if (g) g(NULL, 0, &need, GetCurrentProcess(), 0);
            BYTE* b = need ? (BYTE*)malloc(need) : NULL;
            if (b && g((PSYSTEM_CPU_SET_INFORMATION)b, need, &need, GetCurrentProcess(), 0))
                for (ULONG o = 0; o < need && nall < 256;) {
                    const PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(b + o);
                    if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 &&
                        (s_cpu.affinity >> c->CpuSet.LogicalProcessorIndex & 1)) all[nall++] = c->CpuSet.Id;
                    o += c->Size;
                }
            free(b);
        }
        if (stscs && nall) stscs(th, all, nall);
    }
}

/* Main thread / RSX engine / FIFO walker / audio synth lane: ideal processor
 * on its own physical core (below 12 logical CPUs; above that
 * main_core_keeper hard-pins). Hot cores: P-cores only, those of the L3
 * domain (Ryzen CCX/CCD) with the most of them first, skipping core 0 (most
 * interrupts land there) when there are >= 4. Distinct cores = no hot thread
 * is hinted onto another hot thread's SMT sibling. */
enum { PORT_ROLE_MAIN = 0, PORT_ROLE_ENGINE = 1, PORT_ROLE_WALKER = 2, PORT_ROLE_SYNTH = 3 };
static int s_place_ideal = 0;
static int s_place_audio = 0;   /* PORT_PLACE_AUDIO: synth lane gets its own hot core */
static int s_hot_core[64], s_nhot = -1;
static void port_hot_cores_init(void)
{
    if (s_nhot >= 0) return;
    int best = 0, bestn = -1;
    for (int j = 0; j < (s_cpu.nl3 ? s_cpu.nl3 : 1); j++) {
        int n = 0;
        for (int i = 0; i < s_cpu.ncore; i++)
            if ((!s_cpu.hybrid || (s_cpu.core_mask[i] & s_cpu.pmask)) && s_cpu.core_l3[i] == j) n++;
        if (n > bestn) { bestn = n; best = j; }
    }
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s_cpu.ncore; i++) {
            if (s_cpu.hybrid && !(s_cpu.core_mask[i] & s_cpu.pmask)) continue;
            if ((s_cpu.core_l3[i] == best) != (pass == 0)) continue;
            s_hot_core[n++] = i;
        }
    /* core 0 (interrupts) last when there are >= 4: it is then out of reach */
    if (n >= 4) {
        int z = -1; for (int i = 0; i < n; i++) if (s_hot_core[i] == 0) z = i;
        if (z >= 0) { for (int i = z; i < n - 1; i++) s_hot_core[i] = s_hot_core[i + 1]; s_hot_core[n - 1] = 0; }
    }
    s_nhot = n;
}
static void port_place_hot(HANDLE th, int role)
{
    if (!s_cpu_place || !s_place_ideal || s_cpu.ncore < 2) return;
    port_hot_cores_init();
    const int np = s_nhot;
    const int avail = np >= 4 ? np - 1 : np;   /* without core 0 */
    if (avail < 1) return;
    /* slots: main 0, engine 1, synth 2 (with PORT_PLACE_AUDIO), walker next.
     * The walker shares the engine's core when there is none left for it
     * (they run in lockstep); the synth shares the engine's, never main's. */
    const int nslot = s_place_audio ? 4 : 3;
    int slot;
    if (role == PORT_ROLE_MAIN) slot = 0;
    else if (role == PORT_ROLE_ENGINE) slot = 1;
    else if (role == PORT_ROLE_SYNTH) slot = avail >= 3 ? 2 : 1;
    else slot = avail >= nslot ? nslot - 1 : 1;
    const DWORD_PTR m = s_cpu.core_mask[s_hot_core[slot % avail]];
    PROCESSOR_NUMBER pn = { 0 };
    pn.Group = 0;
    for (BYTE i = 0; i < 64; i++) if (m >> i & 1) { pn.Number = i; break; }
    SetThreadIdealProcessorEx(th, &pn, NULL);
}

extern "C" unsigned long rsx_draw_engine_host_tid(void);
extern "C" unsigned long spu_raw_host_tid(unsigned n);
extern "C" unsigned long spurs_lane_host_tid(unsigned lane);
extern "C" unsigned long rsx_render_host_tid(void);
extern "C" unsigned long rsx_fifo_host_tid(void);
extern "C" unsigned long spurs_synth_host_tid(void);   /* cellSpurs.c: lane running the synth list */
extern "C" int ps3_host_nospin(void);                  /* sys_ppu_thread.c */
static DWORD s_place_walker_tid = 0;

/* PORT_RSX_PRIO=1 (the low profile's default below 8 logical CPUs; 0 = off): rsx::thread at ABOVE_NORMAL
 * (never lowered). On a 4-core / 4-thread CPU (R3 #33, i5-7300HQ) it ran only 55-77% of the time it was
 * busy: the SPURS lanes, the RSX Offloader and the guest threads at the same priority took its core
 * while the main thread waited for it. */
static void port_rsx_prio_raise(HANDLE h, DWORD tid)
{
    if (!s_rsx_prio) return;
    const int cur = GetThreadPriority(h);
    if (cur == THREAD_PRIORITY_ERROR_RETURN || cur >= THREAD_PRIORITY_ABOVE_NORMAL) return;
    const BOOL ok = SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    fprintf(stderr, "[cpu] rsx::thread (thread %lu): priority above normal %s (PORT_RSX_PRIO; 0 = off)\n",
            (unsigned long)tid, ok ? "ok" : "FAILED");
}

/* Applies placement to threads that appear later: the RSX engine thread
 * (RSX_PERF_MT starts it on the first FIFO work) and background threads that
 * name themselves (pipeline compilers, monitors). Every 2 s while threads keep
 * appearing, then every PORT_KEEPER_MS (default 5000), below normal. */
static DWORD WINAPI port_place_keeper(LPVOID)
{
    SetThreadDescription(GetCurrentThread(), L"placement keeper");
    port_place_background(GetCurrentThread());
    typedef HRESULT (WINAPI *gtd_t)(HANDLE, PWSTR*);
    static gtd_t gtd = (gtd_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    static DWORD seen[1024]; int nseen = 0;
    DWORD eng_done = 0, syn_done = 0;
    /* hot roles: last tid placed for each (threads restart, e.g. a raw SPU on a checkpoint reload) */
    enum { R_RENDER, R_FIFO, R_ENGINE, R_RAW0, R_LANE0 = R_RAW0 + 4, R_N = R_LANE0 + 16 };
    DWORD hot_done[R_N] = { 0 };
    const char* ke = getenv("PORT_KEEPER_MS");
    const DWORD idle_ms = ke && atoi(ke) >= 250 ? (DWORD)atoi(ke) : 5000;
    int quiet = 0, kwait = 0, replanned = 0;
    DWORD kpin_done[4] = { 0 };
    const DWORD pid = GetCurrentProcessId();
    for (;;) {
        int fresh = 0;
        /* main_core_keeper decides once, shortly after start-up: wait for it (<= 30 s) before placing hot threads */
        const long kst = s_keeper_expected ? port_keeper_state : 2;
        if (!kst && kwait < 30) { kwait++; Sleep(1000); continue; }
        if (kst == 1 && !replanned) {
            char kb[512];
            replanned = 1;
            port_pcore_query(&s_pcore_rest, port_keeper_rest);
            if (s_pcore.why_off) s_pcore_rest.why_off = s_pcore.why_off;
            s_phot = &s_pcore_rest;
            fprintf(stderr, "[cpu] core keeper pins: hot threads by its rest mask 0x%llX: %s\n",
                    (unsigned long long)port_keeper_rest, port_pcore_describe(&s_pcore_rest, kb, sizeof kb));
            if (s_default_sets && (s_pcore_rest.why_off || s_pcore_rest.usable < 4)) {
                typedef BOOL (WINAPI *spdcs_t)(HANDLE, const ULONG*, ULONG);
                spdcs_t s = (spdcs_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessDefaultCpuSets");
                if (s && s(GetCurrentProcess(), NULL, 0)) {
                    s_default_sets = 0;
                    fprintf(stderr, "[cpu] game threads: P-core default dropped (%d usable P-cores beside the pinned ones)\n",
                            s_pcore_rest.usable);
                }
            }
            if (s_place_walker_tid)
                port_pcore_place_role(&s_pcore_rest, s_place_walker_tid, "FIFO walker", PORT_PCORE_FIFO, 0, PORT_PCORE_PRIO);
            fresh = 1;
        }
        const port_pcore_plan* hp = s_phot;
        /* threads the keeper hard-pins (main, FIFO / rsx::thread, rawSPU0, synth lane): no CPU sets */
        if (kst == 1) {
            static const char* const kn[4] = { "main guest thread", "RSX FIFO thread", "raw SPU 0", "audio synth lane" };
            for (int i = 0; i < 4; i++) {
                const DWORD t = (DWORD)port_keeper_tid[i];
                if (!t || t == kpin_done[i]) continue;
                port_pcore_place_tid(&s_pcore, t, kn[i], PORT_PCORE_PINNED | (i < 2 ? PORT_PCORE_PRIO : 0));
                kpin_done[i] = t; fresh = 1;
            }
        }
        const DWORD et = (DWORD)rsx_draw_engine_host_tid();
        if (et && et != eng_done && et != s_place_walker_tid) {
            HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, et);
            if (h) { port_place_hot(h, PORT_ROLE_ENGINE); CloseHandle(h); eng_done = et; fresh = 1; }
        }
        /* the SPURS audio synth lane (R2 scratch\r2_audio_lowcore.md): its own
         * hot core. The lane serving the list can change; the new one is hinted. */
        if (s_place_audio && s_place_ideal) {
            const DWORD yt = (DWORD)spurs_synth_host_tid();
            if (yt && yt != syn_done) {
                HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, yt);
                if (h) { port_place_hot(h, PORT_ROLE_SYNTH); CloseHandle(h); syn_done = yt; fresh = 1;
                         fprintf(stderr, "[cpu] audio synth lane thread %lu: ideal CPU on its own core%c", (unsigned long)yt, 10); }
            }
        }
        if (!hp->why_off || s_pcore_prio) {
            for (int r = 0; r < R_N; r++) {
                DWORD tid; char nm[48]; int role, idx = 0;
                if (r == R_RENDER) { tid = (DWORD)rsx_render_host_tid(); snprintf(nm, sizeof nm, "RSX render thread"); role = PORT_PCORE_RENDER; }
                else if (r == R_FIFO) { tid = (DWORD)rsx_fifo_host_tid(); snprintf(nm, sizeof nm, "RSX FIFO thread"); role = PORT_PCORE_FIFO; }
                else if (r == R_ENGINE) { tid = et; snprintf(nm, sizeof nm, "RSX engine thread"); role = PORT_PCORE_RENDER; }
                else if (r < R_LANE0) { idx = r - R_RAW0; tid = (DWORD)spu_raw_host_tid((unsigned)idx); snprintf(nm, sizeof nm, "raw SPU %d", idx); role = PORT_PCORE_RAW; }
                else { idx = r - R_LANE0; tid = (DWORD)spurs_lane_host_tid((unsigned)idx); snprintf(nm, sizeof nm, "SPURS lane %d", idx); role = PORT_PCORE_LANE; }
                if (!tid || tid == hot_done[r] || tid == s_place_main_tid || tid == s_place_walker_tid || port_keeper_pinned(tid)) continue;
                port_pcore_place_role(hp, tid, nm, role, idx, r < R_RAW0 ? PORT_PCORE_PRIO : 0);
                hot_done[r] = tid; fresh = 1;
            }
        }
        HANDLE snap = gtd ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                int k = 0; while (k < nseen && seen[k] != te.th32ThreadID) k++;
                if (k < nseen) continue;
                HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                PWSTR d = NULL;
                if (SUCCEEDED(gtd(h, &d)) && d && d[0]) {
                    /* named threads are named for good: decide once */
                    if (wcsstr(d, L"ompil") || wcsstr(d, L"shader") || wcsstr(d, L"monitor") ||
                        wcsstr(d, L"thrcpu") || wcsstr(d, L"affinity keeper") || wcsstr(d, L"watchdog"))
                        port_place_background(h);
                    else if (!wcscmp(d, L"rsx::thread") || (!wcscmp(d, L"FIFO walker") && te.th32ThreadID != s_place_walker_tid)) {
                        if (!port_keeper_pinned(te.th32ThreadID))
                            port_pcore_place_role(hp, te.th32ThreadID, !wcscmp(d, L"rsx::thread") ? "rsx::thread" : "FIFO walker",
                                                  PORT_PCORE_FIFO, 0, PORT_PCORE_PRIO);
                        /* RPCS3 RSX mode: rsx::thread is the RSX thread (no engine tid) */
                        if (s_place_audio && !wcscmp(d, L"rsx::thread") && !et) port_place_hot(h, PORT_ROLE_ENGINE);
                        if (!wcscmp(d, L"rsx::thread")) port_rsx_prio_raise(h, te.th32ThreadID);
                    }
                    else if (!wcscmp(d, L"cellAudio mixer"))
                        port_pcore_place_role(hp, te.th32ThreadID, "audio mixer", PORT_PCORE_AUDIO, 0, 0);
                    if (nseen < 1024) seen[nseen++] = te.th32ThreadID;
                    fresh = 1;
                }
                if (d) LocalFree(d);
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        quiet = fresh ? 0 : quiet + 1;
        Sleep(quiet >= 5 ? idle_ms : 2000);
    }
}

/* The FIFO walker (frame_clock) calls this from its own thread. */
static void port_place_walker(void)
{
    s_place_walker_tid = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"FIFO walker");
    port_place_hot(GetCurrentThread(), PORT_ROLE_WALKER);
    if (s_cpu_place) port_pcore_place_role(s_phot, GetCurrentThreadId(), "FIFO walker", PORT_PCORE_FIFO, 0, PORT_PCORE_PRIO);
    /* PORT_MMCSS_RENDER=1 (A/B, default off as in R2): MMCSS "Games" for the
     * native renderer's FIFO walker; the RPCS3 path does it for rsx::thread. */
    const char* mm = getenv("PORT_MMCSS_RENDER");
    if (mm && atoi(mm) > 0) {
        typedef HANDLE (WINAPI *avset_t)(LPCWSTR, LPDWORD);
        HMODULE av = LoadLibraryA("avrt.dll");
        avset_t avset = av ? (avset_t)GetProcAddress(av, "AvSetMmThreadCharacteristicsW") : NULL;
        DWORD task = 0;
        HANDLE h = avset ? avset(L"Games", &task) : NULL;
        fprintf(stderr, "[cpu] MMCSS \"Games\" on FIFO walker: %s (PORT_MMCSS_RENDER)\n", h ? "on" : "failed");
    }
}

#ifdef PORT_PGO_GEN
/* PGO training build (PORT_PGO=gen): the counters live in memory and are
 * written to LLVM_PROFILE_FILE only by an orderly exit, which the game does
 * not always reach (window close -> TerminateProcess paths). Write them every
 * PORT_PGO_DUMP_S seconds (default 30) and at exit; each write replaces the
 * file with the counters so far. */
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
static DWORD WINAPI port_pgo_dumper(LPVOID p)
{
    SetThreadDescription(GetCurrentThread(), L"pgo monitor");
    const DWORD ms = (DWORD)(uintptr_t)p * 1000;
    for (;;) { Sleep(ms); __llvm_profile_write_file(); }
}
static void port_pgo_atexit(void) { __llvm_profile_write_file(); }
static void port_pgo_start(void)
{
    /* the profile runtime read the variable before main(): set the name directly */
    if (!getenv("LLVM_PROFILE_FILE")) { _putenv_s("LLVM_PROFILE_FILE", "pgo\\game-%p.profraw");
                                        __llvm_profile_set_filename("pgo\\game-%p.profraw"); }
    CreateDirectoryA("pgo", NULL);
    const char* e = getenv("PORT_PGO_DUMP_S");
    const int s = e && atoi(e) > 0 ? atoi(e) : 30;
    atexit(port_pgo_atexit);
    /* sys_process_exit leaves with ExitProcess (no atexit) unless PS3_EXIT_CRT=1: keep the final profile dump */
    if (!getenv("PS3_EXIT_CRT")) _putenv_s("PS3_EXIT_CRT", "1");
    HANDLE h = CreateThread(NULL, 0, port_pgo_dumper, (LPVOID)(uintptr_t)s, 0, NULL);
    if (h) CloseHandle(h);
    fprintf(stderr, "[pgo] instrumented build: profile -> %s every %d s and at exit\n", getenv("LLVM_PROFILE_FILE"), s);
}
#endif

static void port_cpu_profile_init(void)
{
#ifdef PORT_PGO_GEN
    port_pgo_start();
#endif
    port_cpu_detect();
    int brand_r[4]; char brand[49] = { 0 };
    __cpuid(brand_r, 0x80000000);
    if ((unsigned)brand_r[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) { __cpuid(brand_r, 0x80000002 + i); memcpy(brand + 16 * i, brand_r, 16); }
    const char* b = brand; while (*b == ' ') b++;

    const double eff_cores = s_cpu.hybrid ? s_cpu.pcores + s_cpu.ecores / 2.0 : (double)s_cpu.phys;
    const int autop = eff_cores <= 4.0 ? PORT_CPU_LOW : eff_cores < 8.0 ? PORT_CPU_MID : PORT_CPU_HIGH;
    const char* e = getenv("PORT_CPU_PROFILE");
    int prof = autop, forced = 0;
    if (e && *e && _stricmp(e, "auto")) {
        forced = 1;
        if (!_stricmp(e, "low")) prof = PORT_CPU_LOW;
        else if (!_stricmp(e, "mid")) prof = PORT_CPU_MID;
        else if (!_stricmp(e, "high")) prof = PORT_CPU_HIGH;
        else if (!_stricmp(e, "off") || !strcmp(e, "0")) prof = PORT_CPU_OFF;
        else forced = 0;
    }
    s_cpu_profile = prof;
    static const char* const pn[] = { "off", "low", "mid", "high" };
    fprintf(stderr, "[cpu] %s: %d cores / %d threads%s%s | available to the process: %d cores / %d threads "
                    "(affinity 0x%llX)%s, %d P + %d E | profile %s (%s)\n",
            *b ? b : "unknown CPU", s_cpu.sys_phys, s_cpu.sys_logical, s_cpu.smt ? " SMT" : "",
            s_cpu.hybrid ? " hybrid" : "", s_cpu.phys, s_cpu.logical, (unsigned long long)s_cpu.affinity,
            s_cpu.smt ? " SMT" : " no SMT", s_cpu.hybrid ? s_cpu.pcores : s_cpu.phys, s_cpu.ecores,
            pn[prof], forced ? "PORT_CPU_PROFILE" : "auto");
    {
        /* Opt out of EcoQoS / power throttling. Windows 11 may otherwise run the
         * game's threads at an efficient clock or on E-cores when the window loses
         * focus or the scheduler thinks the process is background work, and may
         * ignore its 1 ms timer resolution (the frame limiter and the GPU pacing
         * depend on it). PORT_ECOQOS=1 keeps the Windows default. */
        /* PORT_TIMER_NOTHROTTLE=0 separately lets Windows 11 ignore the 1 ms timer
         * resolution again while the window is minimised / occluded. */
        const char* eq = getenv("PORT_ECOQOS");
        const char* tn = getenv("PORT_TIMER_NOTHROTTLE");
        const bool eco_off = !(eq && *eq && atoi(eq) != 0);
        const bool tmr_off = !(tn && *tn && atoi(tn) == 0);
        if (eco_off || tmr_off) {
            PROCESS_POWER_THROTTLING_STATE pt;
            memset(&pt, 0, sizeof pt);
            pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            pt.ControlMask = (eco_off ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0u) |
                             (tmr_off ? PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0u);
            pt.StateMask = 0;   /* control bit set + state bit clear = never throttle */
            const BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
            fprintf(stderr, "[cpu] power throttling opt-out (EcoQoS %s, timer resolution %s): %s "
                            "(PORT_ECOQOS=1 / PORT_TIMER_NOTHROTTLE=0 keep the Windows default)\n",
                    eco_off ? "off" : "default", tmr_off ? "kept" : "default", ok ? "ok" : "failed");
        }
    }
    port_pcore_query(&s_pcore, 0);
    s_pcore_prio = port_thread_priority_env() && port_thread_placement_env();
    if (prof == PORT_CPU_OFF) {
        if (!s_pcore.why_off) s_pcore.why_off = "PORT_CPU_PROFILE=off";
        s_pcore_prio = 0;
        return;
    }

    char log[1024] = { 0 };
    if (prof == PORT_CPU_LOW) {
        port_cpu_setdef("GCM_VBLANK_SPIN_US", "0", log, sizeof log);
        port_cpu_setdef("R2_PACE_SPIN_US", "100", log, sizeof log);
        port_cpu_setdef("RSX_SPIN_US", "50", log, sizeof log);
        port_cpu_setdef("RSX_PERF_VK_SUBMIT_THREAD", "0", log, sizeof log);
        port_cpu_setdef("RSX_VK_PIPELINE_THREADS", s_cpu.logical >= 8 ? "2" : "1", log, sizeof log);
        port_cpu_setdef("PS3_PRIO_BOOST", "0", log, sizeof log);
        if (s_cpu.logical < 8) port_cpu_setdef("PORT_RSX_PRIO", "1", log, sizeof log);
    } else if (prof == PORT_CPU_MID) {
        port_cpu_setdef("GCM_VBLANK_SPIN_US", "100", log, sizeof log);
        port_cpu_setdef("R2_PACE_SPIN_US", "250", log, sizeof log);
    }
    /* PS3_PRIO_BOOST is off with the low profile and below 8 logical CPUs (a
     * boosted spinner starved the normal threads there), which also dropped
     * the synth lane's boost and left audio short of realtime on 4-core CPUs
     * (R2 scratch\r2_audio_lowcore.md). The synth lane alone keeps it: it runs
     * a bounded list per batch and its DMA poll naps after SPURS_AUDIO_SPIN_US. */
    /* SPURS_AUDIO_BOOST_FORCE defaults to 1 in the runtime (cellSpurs.c), so
     * the profile no longer sets it; PORT_PLACE_AUDIO=0 (the old layout) also
     * opts out of the boost, unless SPURS_AUDIO_BOOST_FORCE is set explicitly. */
    {
        const char* pa = getenv("PORT_PLACE_AUDIO");
        if (pa && *pa && atoi(pa) == 0) port_cpu_setdef("SPURS_AUDIO_BOOST_FORCE", "0", log, sizeof log);
    }
    if (log[0]) fprintf(stderr, "[cpu] profile %s sets:%s\n", pn[prof], log);
    s_rsx_prio = port_native_parse_bool(getenv("PORT_RSX_PRIO"), 0);

    s_cpu_place = port_thread_placement_env();
    if (!s_cpu_place) { fprintf(stderr, "[cpu] thread placement off (PORT_THREAD_PLACEMENT=off)\n"); return; }

    char plog[512] = { 0 };
    /* main_core_keeper (debugtools.inc) hard-pins main and the FIFO thread
     * with >= 12 logical CPUs or PORT_MAIN_CPU; leave those to it. */
    s_place_main_tid = GetCurrentThreadId();
    fprintf(stderr, "[cpu] %s\n", port_pcore_describe(&s_pcore, plog, sizeof plog));
    plog[0] = 0;
    if (!s_pcore.why_off && s_pcore.usable >= 4) {
        typedef BOOL (WINAPI *spdcs_t)(HANDLE, const ULONG*, ULONG);
        spdcs_t s = (spdcs_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessDefaultCpuSets");
        if (s && s(GetCurrentProcess(), (const ULONG*)s_pcore.ids, (ULONG)s_pcore.n)) {
            s_default_sets = 1;
            snprintf(plog + strlen(plog), sizeof plog - strlen(plog), " game threads on the %d usable P-cores (%lu CPU sets);",
                     s_pcore.usable, (unsigned long)s_pcore.n);
        }
    }
    port_pcore_place_role(&s_pcore, s_place_main_tid, "main guest thread", PORT_PCORE_MAIN, 0, PORT_PCORE_PRIO);
    const char* mc = getenv("PORT_MAIN_CORE");
    const int keeper_pins = !(mc && atoi(mc) == 0) && (getenv("PORT_MAIN_CPU") || s_cpu.logical >= 12);
    s_keeper_expected = keeper_pins;
    s_place_ideal = !keeper_pins && s_cpu.ncore >= 2;
    { const char* pa = getenv("PORT_PLACE_AUDIO");
      s_place_audio = !(pa && *pa && atoi(pa) == 0); }
    if (s_place_ideal) {
        port_place_hot(GetCurrentThread(), PORT_ROLE_MAIN);
        port_hot_cores_init();
        snprintf(plog + strlen(plog), sizeof plog - strlen(plog),
                 " main / RSX engine%s / FIFO walker ideal CPUs on separate cores (%d hot cores, %d L3 domain%s);",
                 s_place_audio ? " / audio synth lane" : "", s_nhot >= 4 ? s_nhot - 1 : s_nhot,
                 s_cpu.nl3 ? s_cpu.nl3 : 1, s_cpu.nl3 > 1 ? "s, the largest first" : "");
    }
    HANDLE kh = CreateThread(NULL, 0, port_place_keeper, NULL, 0, NULL);
    if (kh) CloseHandle(kh);
    fprintf(stderr, "[cpu] thread placement:%s background threads below normal\n", plog);
}
#else
static void port_cpu_profile_init(void) {}
static void port_place_walker(void) {}
extern "C" unsigned long long port_cpu_pmask(void) { return 0; }
extern "C" int port_cpu_core_first(int core) { (void)core; return -1; }
extern "C" int port_thread_placement_on(void) { return 0; }
static const char* port_pcore_why(void) { return "not Windows"; }
#endif
