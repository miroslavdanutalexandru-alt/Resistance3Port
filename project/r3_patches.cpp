// Resistance 3: community RPCS3 patches (patch.yml), ported as guest-memory
// data patches applied once at boot, after the EBOOT is loaded and before the
// game's entry runs -- the moment RPCS3 applies them.
//
// Only disc EBOOT 1.00 is supported: BCUS98176 / BCES01118 01.00, PPU hashes
// 502feb2351d2b637a2704d5a50cb44bc1b781532 (US) and
// 2701cf372814d59f26e771dc9394323d4f799682 (EU), which share every address.
// Every write checks the original value first and is skipped (logged) on a
// mismatch, so another EBOOT (e.g. 1.05) never gets patched at the wrong place.
//
// Code patches (instruction edits) cannot be byte-patched: the PPU code is
// recompiled. Those live as hooks in tools/hooks/r3.json (see README,
// "RPCS3 patches").
//
//   Unlock FPS (Whatcookie, boma), tied to R2_FPS:
//     be32 0x00F02AD4 0x38D1B717   0.001 s frame-time floor -> 0.0001 (here,
//                                  and again by the limiter hook)
//     be32 0x007EEAC8 lfs ...      the game clock's 1/75 lower clamp: the
//                                  r3_dt_floor / r3_dt_ceil hooks in
//                                  func_007EEA44 (tools/hooks/r3.json)
//     be32 0x00736C44 nop          the limiter's wait: the r2_frame_period
//                                  hook in func_00736BAC
//   R2_PATCH_FPS_FLOORS=1  applies the floor at 30 fps too.
//   R2_PATCH_FOV=1         "Extended FOV" (illusion), off by default.
//                          R2_PATCH_FOV_VALUE=<1.0..2.0> view width multiplier, or
//                          <0.3..1.0> the patch's constant (0.7 by default); a value
//                          turns it on by itself unless R2_PATCH_FOV=0. Applied at the
//                          camera frustum setter (R3_FOV_SAFE, below), not as the
//                          constant, unless R3_FOV_SAFE=0.
//   R3_ULTRAWIDE, R3_LOD_SCALE: hooks in tools/hooks/r3.json, see below.
//   R3_MOVE_START_FIX=0    the game's own walk-start test (fails above ~30-75 fps):
//                          r3_move_start_hook in func_001B0EC0, see below.
//   R3_NO_LOADSHED=0       restores the game's GPU-time governor (LOD x0.75 and the
//                          960x640 drop on "slow" frames); default: off (no shedding).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ps3emu/ps3types.h"
#include "ppu_recomp.h"
#include "r3_fov_math.h"
#include "r3_dtfloor.h"
#include <atomic>
#include <chrono>
#include <mutex>

extern "C" double r2_fps_target(void);   /* stubs.cpp: 30 = the game's own limiter */
extern "C" void r3_fov_set_live(const char* v);
extern "C" int r3_fov_safe_mode(void);
extern "C" double r3_target_aspect(void);
extern "C" double r3_lod_scale(void);
extern "C" int r2rsx_enabled(void);   /* rpcs3_bridge: 1 = Compatible (RPCS3) renderer */

namespace {

/* -1 unset, 0 off, 1 on */
int env_flag(const char* name)
{
    const char* e = getenv(name);
    if (!e || !*e) return -1;
    if (*e == '0' || !_stricmp(e, "off") || !_stricmp(e, "false") || !_stricmp(e, "no")) return 0;
    return 1;
}

uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Write `val` at `ea` if it still holds `orig`. */
bool poke32(const char* patch, uint32_t ea, uint32_t orig, uint32_t val)
{
    const uint32_t cur = vm_read32(ea);
    if (cur == val) return true;
    if (cur != orig) {
        fprintf(stderr, "[patch] %s: 0x%08X holds %08X, expected %08X -- skipped (not EBOOT 1.00?)\n",
                patch, ea, cur, orig);
        return false;
    }
    vm_write32(ea, val);
    return true;
}

} // namespace

extern "C" void r3_patches_apply(void)
{
    /* Unlock FPS: the 0.001 s floor. Written here so nothing reads it before
     * the first limiter call; the limiter hook writes it again if needed. */
    if (r2_fps_target() != 30.0 || env_flag("R2_PATCH_FPS_FLOORS") == 1) {
        const bool ok = poke32("fps_floor", 0x00F02AD4u, 0x3A83126Fu, 0x38D1B717u);
        fprintf(stderr, "[patch] Unlock FPS frame-time floor: %s\n", ok ? "on" : "skipped");
    } else {
        fprintf(stderr, "[patch] Unlock FPS: off (R2_FPS=30, the game's own limiter)\n");
    }

    /* Field of view (R2_PATCH_FOV_VALUE, the launcher / F10 / Options "Field of view"): on with
     * R2_PATCH_FOV=1 (= 0.7) or a value alone; R2_PATCH_FOV=0 keeps it off either way. Applied at
     * the frustum setter (R3_FOV_SAFE, default) or as the old constant patch (R3_FOV_SAFE=0). */
    const int fov_flag = env_flag("R2_PATCH_FOV");
    const char* fov_val = getenv("R2_PATCH_FOV_VALUE");
    if (fov_flag == 1 || (fov_flag != 0 && fov_val && *fov_val)) {
        const char* v = (fov_val && *fov_val) ? fov_val : "0.7";
        double sc, k;
        if (!r3fov::parse_fov(v, &sc, &k)) {
            fprintf(stderr, "[patch] R2_PATCH_FOV_VALUE=%s out of range (1.0..2.0 wider, or 0.3..1.0 legacy), using 0.7\n", v);
            v = "0.7";
        }
        r3_fov_set_live(v);
    } else {
        fprintf(stderr, "[patch] Extended FOV (R2_PATCH_FOV): off\n");
    }
    r3_fov_safe_mode();
    r3_target_aspect();

    /* GPU-time load shedding (same governor family as R2's func_002C3780). func_003DCA40, the
     * per-frame step around the frame body func_003DC3F8, converts the previous frame's RSX
     * time (report timestamps, 0x012B6054) to ms and compares it with budget = [0x012B1F40] *
     * 2000.0 (float pool 0x003DC9F8: +0x28 2000.0, +0x2C 1.125, +0x30 2.0, +0x34 1.33, +0x38 1.01,
     * +0x3C 0.9, +0x40 0.84, +0x44 0.75). Above 1.33x / 2x it multiplies the frame's LOD
     * distance scale [0x010F4570+0x2C] by 0.75 (restored at the end of the step), above 1.125x
     * it flags the frame and drops the scene to 960x640 inside the 960x704 targets
     * ([0x012B1F20] = {w, h}). The port's RSX times are host times (shader builds, readbacks,
     * 3x resolution), not PS3 GPU times, so heavy view directions trip it frame by frame:
     * models switch LOD and the scene changes size (upscaler resets) -- detail popping in and
     * out with the view angle. The pool is read only by func_003DCA40 (checked over every
     * function that forms 0x003DC9F8); a huge 2000.0 means no frame is over budget, i.e. the
     * game's normal full-detail path. Default on; R3_NO_LOADSHED=0 restores the game's governor. */
    if (env_flag("R3_NO_LOADSHED") != 0) {
        const bool ok = poke32("no_loadshed", 0x003DCA20u, f2u(2000.0f), f2u(1.0e20f));
        fprintf(stderr, "[patch] no GPU load shedding (LOD x0.75 / 960x640 drop) (R3_NO_LOADSHED): %s\n", ok ? "on" : "skipped");
    } else {
        fprintf(stderr, "[patch] no GPU load shedding (R3_NO_LOADSHED): off (game governor)\n");
    }

    const double lod = r3_lod_scale();
    if (lod != 1.0) fprintf(stderr, "[patch] LOD distance scale (R3_LOD_SCALE): %.2f\n", lod);
    else            fprintf(stderr, "[patch] LOD distance scale (R3_LOD_SCALE): off\n");
}

/* ---- R3_ULTRAWIDE / R3_FOV_SAFE / R3_LOD_SCALE --------------------------------------------
 *
 * The game's tanf (func_00B6CDE8, the same code as R2's func_00788068) has a constant term K at
 * 0x00F0608C (1.0). The old "Extended FOV" patch lowers K, which widens every tangent the game
 * takes: the camera's, but also the shadow cascade fitting, light frustums and the LOD maths --
 * the misaligned / jumping shadows and the black bands of RPCS3 issue #14124 (the community patch
 * was removed for it). Here the widening is applied once, to the inputs of the camera frustum
 * setter func_00799EF0 (r3 = frustum, f1 near, f2 far, f3 horizontal FOV in degrees, f4 aspect
 * w/h, f5/f6 offsets; it stores near/far at +0/+4, the aspect at +0xC, the FOV in radians at
 * +0x10, the vertical FOV at +0x14, tan(h/2)/tan(v/2) at +0x18/+0x1C and the projection at +0x140).
 * The culling planes and the shadow cascades are derived from that frustum, so they follow; the
 * other tanf users keep the original maths. Only 16:9 frustums (aspect 1.70..1.86: the scene,
 * cutscene and viewmodel cameras) are touched; shadow and cube-map frustums (~1) are not.
 *
 * Several callers feed the frustum's own stored FOV and aspect back in every frame
 * (func_001B3E28, func_00276BA4; the viewmodel frustum takes the scene's aspect), so a memo
 * (r3fov::Table) maps a widened value back to its original before widening again.
 *
 *   R3_ULTRAWIDE=off|16:9 (default: 16:9 picture, pillarboxed)
 *               =auto        follow the game window's client area (live)
 *               =21:9 | 32:9 | <w>:<h> | <w>x<h> | <float>   fixed
 *     Hor+: wider than 16:9 keeps the vertical FOV and widens the horizontal one; narrower keeps
 *     the horizontal FOV. The renderer stretches the 16:9 picture to the window
 *     (r2rsx_bridge.cpp, stretch_to_display_area); RSX_HUD_FIX=1 keeps the HUD in a 16:9 area.
 *   R3_FOV_SAFE=1 (default) the FOV setting at the setter; =0 the old constant patch.
 *   R3_FOV_VIEWMODEL=1  the first-person arms / weapon widen with the FOV setting too (default:
 *     they keep their own FOV; ultrawide still applies to them).
 *   R3_LOD_SCALE=<1.0..4.0> LOD / fade distances x n (default 1.0).
 *   R3_LOD_FOV_COMP=0   no LOD compensation for the widening (default on: a widened frustum
 *     keeps the original LOD distances instead of switching to lower detail sooner). */
#include <mutex>
extern "C" void* g_kbm_hwnd;   /* the game window (cellPad.c) */
extern "C" volatile long g_port_frame_no;   /* cellPad.c: frames presented */
extern "C" int r3_level_gen(void);          /* r3_pcextra.cpp: +1 when the game opens another level */
extern "C" __declspec(dllimport) int __stdcall GetClientRect(void* hwnd, long* rect);
extern "C" __declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);

namespace {
const uint32_t kFovConst = 0x00F0608Cu;
/* viewmodel frustums (view+0x2280): func_003F5478 sets [cam+0x2170]+0x2280 from FOV [player+0x2144];
 * func_00276BA4 re-feeds one's own +0x10/+0xC (0x276EE8) and copies it into another view's +0x2280
 * (0x276F50). The table also remembers per frustum that it is a viewmodel. */
bool viewmodel_call(uint32_t lr) { return lr == 0x003F5608u || lr == 0x00276EE8u || lr == 0x00276F50u; }
std::mutex g_fov_mu;
r3fov::Table g_tab;
bool g_tab_used = false;
double g_fov_scale = 1.0, g_fov_k = 1.0;      /* the FOV setting (safe mode) */

double kmem(void)
{
    const float k = u2f(vm_read32(kFovConst));
    return (k >= 0.3f && k <= 1.0f) ? (double)k : 1.0;
}

bool viewmodel_widens(void)
{
    static int w = -1;
    if (w < 0) {
        w = env_flag("R3_FOV_VIEWMODEL") == 1;
        fprintf(stderr, "[patch] FOV viewmodel (R3_FOV_VIEWMODEL): %s\n", w ? "widened" : "original FOV");
    }
    return w != 0;
}
} // namespace

extern "C" int r3_fov_safe_mode(void)
{
    static int m = -1;
    if (m < 0) {
        m = env_flag("R3_FOV_SAFE") != 0;
        fprintf(stderr, "[patch] FOV safe mode (R3_FOV_SAFE): %s\n", m ? "on (camera frustum only)" : "off (constant patch)");
    }
    return m;
}

/* R3_ULTRAWIDE, at boot and live (F10 menu / Options rows, r3_pcport.cpp): the mode and the fixed
 * aspect are read by the frustum hook every frame, so a change takes effect on the next frame. */
extern "C" void r2rsx_set_stretch(int on);   /* rpcs3_bridge: the picture stretched over the window */
namespace {
std::atomic<int> g_aspect_mode{-1};          /* -1 not read yet, 0 off, 1 follow the window, 2 fixed */
std::atomic<double> g_aspect_fixed{0.0};
std::once_flag g_aspect_once;
} // namespace

extern "C" void r3_aspect_set_live(const char* e)
{
    double fixed = 0.0;
    int mode = r3fov::parse_aspect(e, &fixed);
    fprintf(stderr, "[aspect] R3_ULTRAWIDE=%s: %s\n", (e && *e) ? e : "(unset)",
            mode == 0 ? "off (16:9)" : mode == 1 ? "Hor+, follows the window" : "Hor+, fixed");
    if (mode == 2) fprintf(stderr, "[aspect] target %.4f\n", fixed);
    /* the native renderers (R2_RSX=vulkan/native/d3d12) always pillarbox the 16:9 image, so a
     * wider frustum would only squeeze the picture: stay at 16:9 there */
    if (mode != 0 && !r2rsx_enabled()) {
        fprintf(stderr, "[aspect] WARNING: R3_ULTRAWIDE=%s needs the Compatible (RPCS3) renderer; the native "
                        "renderer (R2_RSX=%s) shows 16:9 with side bars -- ultrawide off\n",
                e, getenv("R2_RSX") ? getenv("R2_RSX") : "?");
        mode = 0;
    }
    g_aspect_fixed.store(fixed);
    g_aspect_mode.store(mode);
    if (r2rsx_enabled()) r2rsx_set_stretch(mode != 0);
}

extern "C" double r3_target_aspect(void)
{
    static double live = 16.0 / 9.0;
    static unsigned long long next = 0;
    std::call_once(g_aspect_once, [] { if (g_aspect_mode.load() < 0) r3_aspect_set_live(getenv("R3_ULTRAWIDE")); });
    const int mode = g_aspect_mode.load();
    if (mode <= 0) return 0.0;
    if (mode == 2) return g_aspect_fixed.load();
    const unsigned long long now = GetTickCount64();
    if (now >= next && g_kbm_hwnd) {
        next = now + 250;
        long r[4] = {0, 0, 0, 0};
        if (GetClientRect(g_kbm_hwnd, r) && r[2] > 64 && r[3] > 64) {
            const double a = (double)r[2] / (double)r[3];
            if (fabs(a - live) > 0.002) {
                live = a;
                fprintf(stderr, "[aspect] window %ldx%ld: aspect %.4f\n", r[2], r[3], a);
            }
        }
    }
    return live;
}

/* the FOV setting, at boot and live (F10 menu / Options rows, r3_pcport.cpp): "" = original */
extern "C" void r3_fov_set_live(const char* v)
{
    double sc, k;
    if (!r3fov::parse_fov(v, &sc, &k)) { fprintf(stderr, "[patch] FOV: bad value '%s', ignored\n", v ? v : ""); return; }
    if (r3_fov_safe_mode()) {
        std::lock_guard<std::mutex> lk(g_fov_mu);
        g_fov_scale = sc; g_fov_k = k;
        fprintf(stderr, "[patch] Extended FOV: %s (safe: camera frustum only)\n",
                (sc == 1.0 && k == 1.0) ? "original" : v);
        return;
    }
    const double kk = k != 1.0 ? k : sc != 1.0 ? r3fov::k_for_scale(sc) : 1.0;
    const uint32_t cur = vm_read32(kFovConst);
    if (!(u2f(cur) >= 0.3f && u2f(cur) <= 1.0f)) {
        fprintf(stderr, "[patch] fov: 0x%08X holds %08X -- skipped (not EBOOT 1.00?)\n", kFovConst, cur);
        return;
    }
    vm_write32(kFovConst, f2u((float)kk));
    fprintf(stderr, "[patch] Extended FOV: %s (constant patch, K %.3f)\n", kk == 1.0 ? "original" : "on", kk);
}

/* func_00799EF0 entry (tools/hooks/r3.json) */
extern "C" void r3_frustum_hook(ppu_context* ctx)
{
    const double t = r3_target_aspect();
    const bool safe = r3_fov_safe_mode() != 0;
    std::lock_guard<std::mutex> lk(g_fov_mu);
    const bool fov_on = safe && (g_fov_scale != 1.0 || g_fov_k != 1.0);
    if (!g_tab_used && t <= 0.0 && !fov_on) return;
    const r3fov::Setting st{ t, fov_on ? g_fov_scale : 1.0, fov_on ? g_fov_k : 1.0, kmem(), viewmodel_widens() };
    g_tab.begin((uint32_t)g_port_frame_no, r3_level_gen());
    double nf, na;
    if (!r3fov::frustum_step(g_tab, (uint32_t)ctx->gpr[3], ctx->fpr[3], ctx->fpr[4], viewmodel_call((uint32_t)ctx->lr),
                             st, &nf, &na))
        return;
    ctx->fpr[3] = (double)(float)nf;
    ctx->fpr[4] = (double)(float)na;
    g_tab_used = true;
}

/* R3_LOD_SCALE=<1.0..4.0>, default 1.0 (off). func_0079C918 (r3 = view, f1 = multiplier; called
 * by the frame renderer at 0x274EB4 with [R17+0x2C]) computes the LOD projection scale
 * (W/2) / tan(fov/2) * tan(36 deg) / (W0/2) * f1 into view+0x4C and its inverse into the globals
 * 0x012B64D0.. that the LOD distance tests use; larger = detail kept further out. */
extern "C" double r3_lod_scale(void)
{
    static double s = -1.0;
    if (s < 0.0) {
        s = 1.0;
        if (const char* e = getenv("R3_LOD_SCALE")) {
            const double x = atof(e);
            if (x >= 1.0 && x <= 4.0) s = x;
            else if (*e && x != 0.0)
                fprintf(stderr, "[patch] R3_LOD_SCALE=%s out of range 1.0..4.0, using 1.0\n", e);
        }
    }
    return s;
}

/* LOD multiplier for view `view`: R3_LOD_SCALE times, with R3_LOD_FOV_COMP (default on), the
 * widening applied to that view's frustum (so a wider FOV does not drop detail sooner). */
static double lod_factor(uint32_t view)
{
    static int comp = -1;
    if (comp < 0) {
        comp = env_flag("R3_LOD_FOV_COMP") != 0;
        fprintf(stderr, "[patch] LOD FOV compensation (R3_LOD_FOV_COMP): %s\n", comp ? "on" : "off");
    }
    double m = r3_lod_scale();
    if (comp) {
        std::lock_guard<std::mutex> lk(g_fov_mu);
        if (g_tab_used) m *= g_tab.widening(view, (double)u2f(vm_read32(view + 0x10)), kmem());
    }
    return m;
}

/* func_0079C918 entry (tools/hooks/r3.json): f1 is the caller's multiplier */
extern "C" void r3_lod_hook(ppu_context* ctx)
{
    const double m = lod_factor((uint32_t)ctx->gpr[3]);
    if (m != 1.0) ctx->fpr[1] = (double)(float)(ctx->fpr[1] * m);
}

/* func_0079C810 return (tools/hooks/r3.json): the same LOD scale as func_0079C918 with no
 * multiplier and no store, returned in f1 (r31 = view). Its callers keep it in f31 for their
 * size-vs-distance tests: 0x0075F50C (object radius [+0x14] * scale vs distance, func_007B9A28)
 * and 0x00778404 (scale passed on via f31 around func_00776FA8). */
extern "C" void r3_lod_ret_hook(ppu_context* ctx)
{
    const double m = lod_factor((uint32_t)ctx->gpr[31]);
    if (m != 1.0) ctx->fpr[1] = (double)(float)(ctx->fpr[1] * m);
}
/* R3_MOVE_START_FIX (default on; 0 = the game's original test). Ground state 0x2B1
 * (func_001B0EC0), before its func_00187130 call at 0x001B11B4: while the measured velocity
 * is zero (r27 != 0), the steering rates f2/f3 are scaled so the first step from a standstill
 * is as large as at 30 fps; otherwise the 0.3 m/s start test at 0x001B11E4 zeroes it above
 * ~30-75 fps and the hero cannot walk again until a jump (GitHub R3 #14). See r3_dtfloor.h. */
static uint32_t g_move_entry;   /* current controller ring entry of the ground-state update */

/* func_001B0EC0 at 0x001B0F3C (tools/hooks/r3.json): r30 = the hero's controller entry */
extern "C" void r3_move_note_entry(uint64_t entry) { g_move_entry = (uint32_t)entry; }

extern "C" void r3_move_start_hook(ppu_context* ctx, uint64_t vel_zero, uint64_t state)
{
    static int on = -1;
    static r3_movelock_watch watch = {};
    if (on < 0) {
        on = env_flag("R3_MOVE_START_FIX") != 0;
        fprintf(stderr, "[patch] walk-start fix (R3_MOVE_START_FIX): %s\n", on ? "on" : "off");
    }
    const bool vz = (uint32_t)vel_zero != 0;
    const double dt = (double)u2f(vm_read32(0x012DAE0Cu)), scale = (double)u2f(vm_read32(0x012DAE08u));
    const double k = r3_move_start_boost(dt, scale);
    if (!watch.fired && g_move_entry) {
        const double sx = (double)u2f(vm_read32(g_move_entry + 0x10)), sy = (double)u2f(vm_read32(g_move_entry + 0x14));
        const int held = sx * sx + sy * sy >= 0.0625;
        const double now = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
        if (r3_movelock_step(&watch, now, held, vz, 150.0)) {
            const uint32_t h = vm_read32((uint32_t)state + 0x14);
            fprintf(stderr,
                    "[move-lock] stick held 150 ms with zero hero velocity (ground state 0x2B1): fix=%d dt=%.5f scale=%.3f "
                    "boost=%.2f maxspd=%.3f rate=%.3f/%.3f stick=(%.2f,%.2f) vel120=(%.3f,%.3f,%.3f) "
                    "17a7=%u 17a8=%u ba=%u 252=%u 534=%08X 1684=%08X 16b0=%.2f\n",
                    on, dt, scale, k, ctx->fpr[1], ctx->fpr[2], ctx->fpr[3], sx, sy,
                    (double)u2f(vm_read32(h + 0x120)), (double)u2f(vm_read32(h + 0x124)), (double)u2f(vm_read32(h + 0x128)),
                    vm_read8(h + 0x17A7), vm_read8(h + 0x17A8), vm_read8(h + 0xBA), vm_read8(h + 0x252),
                    vm_read32(h + 0x534), vm_read32(h + 0x1684), (double)u2f(vm_read32(h + 0x16B0)));
        }
    }
    if (!on || !vz || k == 1.0) return;
    ctx->fpr[2] = (double)(float)(ctx->fpr[2] * k);
    ctx->fpr[3] = (double)(float)(ctx->fpr[3] * k);
}