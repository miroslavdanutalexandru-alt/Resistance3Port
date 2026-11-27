/* Resistance 3 PC port: PC extras (r3_pcport.cpp's "More PC Options" page and
 * everything behind it). Runs on the game window's thread from r3_pcport.cpp
 * ui_tick (r2x_ui_tick) plus the frame limiter (r2x_frame_tick, stubs.cpp).
 * Same code as Resistance 2's project/r2_pcextra.cpp, with Resistance 3's
 * movies, levels and benchmark route.
 *
 *   Extras menu      F10 -> More PC Options (or F9 directly): colour-blind filter,
 *                    crosshair, HDR / Reflex (next start), cutscene skip, intro
 *                    skip, Discord, key bindings, controller layout. Changes go to
 *                    settings.ini + settings.bat (launcher).
 *   Key bindings     controls.txt rows (KEY = ACTION # Label); Enter = press a new
 *                    key, Esc cancels; cellPad re-reads the file at once.
 *   Controller       PAD_MAP_<button>=<button>, PAD_SOUTHPAW=1 (cellPad XInput hook).
 *   Post effects     R3_FX_FILMGRAIN / R3_FX_DOF / R3_FX_MOTIONBLUR (default 1 = game's
 *                    own; 0 = off; R3_FX_ADS_BLUR=0 (or the older R3_FX_DOF=aim) = aim-down-sights blur off; live in the menu): hooks in tools/hooks/r3.json zero
 *                    the strength the post pass hands each effect (see fx_init below).
 *   Cutscene skip    R3_CUTSCENE_SKIP (default on): hold Space / Enter / A / Menu
 *                    for 1 s while a pre-rendered movie (.bik) plays. R3_CUTSCENE_SKIP_ALL
 *                    (default on): movies not watched before too (hooks in func_003D6810).
 *   Photo mode       R3_PHOTO (default on; 0 = off), F7: pause, hide the port's
 *                    overlays, F12 = shot. R3_PHOTO_HIDE_HUD (default 1; 0 = off):
 *                    game's HUD too (the last frame's scene from before its first
 *                    HUD draw, VKR2FrameGen.cpp; one display-buffer copy a frame).
 *   Crosshair        R3_CROSSHAIR=dot|cross: a small marker at the centre while on foot.
 *   Pause on pad loss R3_PAD_DISCONNECT_PAUSE (default on).
 *   Discord          R3_DISCORD=1 (+ R3_DISCORD_APPID or discord_appid.txt): Rich Presence over the
 *                    local IPC pipe (\\.\pipe\discord-ipc-N); silent if not running.
 *   Benchmark        R3_BENCH=1: scripted boot into the first level, measure
 *                    R3_BENCH_SECONDS, write logs\bench\R3_bench_<date>.txt, quit.
 *
 * The menu, the photo-mode note, the controller-lost dialog and the crosshair are
 * drawn into the game's swapchain image (port_osd.h), so they show in exclusive
 * fullscreen too; layered windows only with PORT_OSD=layered or before the renderer's
 * first present. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <set>
#include "port_osd.h"
#include "ps3emu/port_pc_extras2.h"
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#ifndef PCX_PREFIX
#define PCX_PREFIX "R3"
#define PCX_GAME_NAME "Resistance 3"
#define PCX_TAG "r3x"
#endif

extern "C" {
extern volatile int g_port_ui_modal;                         /* cellPad.c */
extern volatile int g_port_input_kbm;                        /* cellPad.c: port 0 is the keyboard */
extern volatile unsigned long long g_pad_inj_until;          /* cellPad.c: injected presses on port 0 */
extern volatile uint32_t g_pad_inj_btn, g_pad_inj_sticks, g_pad_inj_clear;
void cellpad_reload_controls(void);                          /* cellPad.c */
extern void (*volatile g_port_xi_hook)(int port, void* gamepad);
extern void (*volatile g_port_file_open_cb)(int slot, const char* path);   /* sys_fs.c */
extern void (*volatile g_port_file_close_cb)(int slot);
extern volatile unsigned long long g_r2_look_ms;             /* stubs.cpp: on-foot look code last ran */
extern uint8_t* vm_base;
void r2pf_toast(const char* title, const char* text, char badge);
void r2pf_hint(const char* title, const char* text, unsigned ms);
int r3_cheat_set(int which, int on);                         /* r3_cheats.cpp: new state, -1 bench / -2 online */
void r3_cheats_init_env(void);
extern volatile int g_r3_cheat_ammo;                         /* r3_cheats.cpp */
int r2pf_overlay_get(void);
void r2pf_overlay_set(int on);
void r2rsx_prof_enable(int on);
int r2rsx_prof_sample(double* frames, double* draws_per_frame, double* us_per_draw);
/* RPCS3 side (VKR2PcExtras.cpp): live post-process parameters */
extern volatile int g_pc_cvd_mode;                           /* 0 off, 1 protan, 2 deutan, 3 tritan */
extern volatile int g_pc_hdr_active;                         /* the swapchain is HDR */
extern volatile int g_pc_reflex_active;
void r2pc_reflex_sleep(void);
/* cellPad.c: called on the first pad read of a game frame (Reflex INPUT_SAMPLE) */
extern void (*volatile g_pad_input_hook)(void);
/* rsx_d3d12_engine.c: Direct3D 12 renderer latency reduction (no-op on the other renderers) */
int  rsx_d3d12_engine_latency_frame(void);
void rsx_d3d12_engine_latency_input(void);
}

/* ---- small helpers (same file formats as r3_pcport.cpp / the launcher) ---- */
static std::string ltrim(const std::string& s) { size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; return s.substr(i); }
static bool starts_ci(const std::string& s, const std::string& p) { return s.size() >= p.size() && !_strnicmp(s.c_str(), p.c_str(), p.size()); }
static bool read_lines(const char* path, std::vector<std::string>& out)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; out.push_back(line); }
    fclose(f);
    return true;
}
static bool write_lines(const char* path, const std::vector<std::string>& out)
{
    char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.pcx.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    for (auto& l : out) fprintf(f, "%s\n", l.c_str());
    const bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return false; }
    return true;
}
static bool file_set(const char* path, const std::string& prefix, const std::string& line, const char* header)
{
    std::vector<std::string> in, out;
    const bool had = read_lines(path, in);
    if (!had && line.empty()) return true;
    bool done = false;
    for (auto& l : in) {
        if (starts_ci(ltrim(l), prefix)) { if (!done && !line.empty()) out.push_back(line); done = true; continue; }
        out.push_back(l);
    }
    if (!done && !line.empty()) { if (!had && header) out.push_back(header); out.push_back(line); }
    return write_lines(path, out);
}
static void ini_set(const char* key, const std::string& v) { file_set("settings.ini", std::string(key) + "=", std::string(key) + "=" + v, NULL); }
static void bat_set(const char* env, const std::string& v)
{
    file_set("settings.bat", std::string("set \"") + env + "=", v.empty() ? std::string() : std::string("set \"") + env + "=" + v + "\"", "@echo off");
}
static std::string envs(const char* name, const char* def)
{
    char full[96]; snprintf(full, sizeof full, "%s_%s", PCX_PREFIX, name);
    const char* e = getenv(full); return (e && *e) ? e : def;
}
static int envi(const char* name, int def) { std::string v = envs(name, ""); return v.empty() ? def : atoi(v.c_str()); }
/* the "Hold Space / A to skip" hint when a movie starts. PORT_SKIP_HINT (or <prefix>_SKIP_HINT; launcher
   "Show cutscene skip hint") unset/empty = auto: shown while Hold to skip movies and Skip unwatched movies
   are both on (checked live), since every story movie can then be skipped. 0/off = never, 1/on = always. */
static int skip_hint_auto(void);
static int skip_hint_on(void)
{
    static int mode = -2;  /* -1 auto, 0 never, 1 always; only the explicit env value is cached */
    if (mode == -2) {
        const char* e = getenv("PORT_SKIP_HINT");
        const std::string v = (e && *e) ? std::string(e) : envs("SKIP_HINT", "");
        const char* s = v.c_str();
        mode = (v == "1" || _stricmp(s, "on") == 0 || _stricmp(s, "true") == 0) ? 1
             : (v == "0" || _stricmp(s, "off") == 0 || _stricmp(s, "false") == 0) ? 0 : -1;
    }
    return mode < 0 ? skip_hint_auto() : mode;
}
static double now_s(void)
{
    static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
static int host_focused(HWND game)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && !IsIconic(game);
}
#define LOG(...) do { fprintf(stderr, "[" PCX_TAG "] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ======================================================================
 * Post effects. The engine's post pass (0x746A00 area) reads the per-view
 * post block (0x12B3E80 + view*0x230; filled every frame from post volumes
 * and the Lua pfx_* nodes) and hands each effect a parameter struct on its
 * stack. Each effect function skips itself when its strength is zero, so
 * the r3.json hooks zero the strength on entry (func_00744A18 film grain:
 * p+8; func_007424F8 camera motion blur: p+0x14; func_00855A98 object motion
 * blur: p+0xC; func_008557A8 depth of field: near/far max blur p+0x28/0x38).
 * The game then takes its own "effect off" path. Nothing persistent changes.
 *
 * R3_FX_DOF=aim ("Off while aiming"): the aim-down-sights blur is its own pfx node. The
 * player's zoom update func_00160108 (player r3; called at 0x001607A8 / 0x00161038 right
 * after the zoom is written; the lifter also emits the same body as func_001600FC behind
 * three data words, hooked the same way) reads the zoom fraction player+0xACC (= zoom time +0x978 / the weapon's zoom time, clamped to 0..1, written just before 0x00160E08).
 * When it is > 0 it gets the player's node (slot 0xF086C8 + player[+0x1680]*4) and
 * gives it only a depth-of-field setting (func_007517B0 0x10 + func_007518D4 = node flag
 * 0x10); otherwise it releases the node (func_007534A0). A hook on its zoom test
 * (r3.json) asks r3_fx_dof_aim_skip(); in "aim" mode the game takes its own "not
 * aiming" path, so only that node goes away. Cutscene / level DOF come from other
 * nodes and post volumes and stay as they are. */
static volatile int s_fx_grain = 1, s_fx_dof = 1, s_fx_mb = 1, s_fx_init = 0;   /* s_fx_dof: 0 off, 1 on, 2 off while aiming */
/* R3_FX_ADS_BLUR=0 ("Aim-down-sights blur: Off", launcher + F10 > PC Options): the same skip as R3_FX_DOF=aim,
 * as its own On/Off setting (default 1 = on). The legacy R3_FX_DOF=aim still works (= DOF on + this off). */
static volatile int s_fx_ads = 1;
static int fx_dof_mode(const std::string& v) { return v == "aim" ? 2 : (v.empty() ? 1 : atoi(v.c_str()) != 0); }
static void fx_init(void)
{
    if (s_fx_init) return;
    s_fx_grain = envi("FX_FILMGRAIN", 1) != 0;
    s_fx_dof = fx_dof_mode(envs("FX_DOF", ""));
    s_fx_ads = envi("FX_ADS_BLUR", 1) != 0;
    if (s_fx_dof == 2) { s_fx_dof = 1; if (envs("FX_ADS_BLUR", "").empty()) s_fx_ads = 0; }   /* an explicit R3_FX_ADS_BLUR wins */
    s_fx_mb = envi("FX_MOTIONBLUR", 1) != 0;
    s_fx_init = 1;
    if (!s_fx_grain || s_fx_dof != 1 || !s_fx_ads || !s_fx_mb)
        LOG("post effects: film grain %s, depth of field %s, aim-down-sights blur %s, motion blur %s", s_fx_grain ? "on" : "off",
            s_fx_dof ? "on" : "off", s_fx_ads ? "on" : "off", s_fx_mb ? "on" : "off");
}
extern "C" void r3_fx_set_ads_blur(int on) { fx_init(); s_fx_ads = on ? 1 : 0; }
static void fx_zero(uint32_t ea) { if (ea && vm_base) memset(vm_base + ea, 0, 4); }   /* 0.0f, any endianness */
/* Depth-of-field parameter block (func_008557A8 r5; big-endian floats): +0 flags, +0x10/+0x14,
 * +0x20/+0x24, +0x30/+0x34 three distance ramps, +0x28 / +0x38 near / far max blur (the effect runs
 * when either is >= 0.01; it then queues the SPU DOF job, igjob 419).
 *   R3_DOF_PLOG=1   log the block whenever it changes (first 400 changes).
 *   R3_FORCE_DOF=1  TEST ONLY (default off): when the block is present but both strengths are below
 *                   0.01, write the aim-down-sights values (R3_FORCE_DOF_P="off=val,..." overrides them)
 *                   so the DOF job runs without a weapon (QA captures / perf A/B). */
static float fx_getf(uint32_t ea) { uint32_t u; memcpy(&u, vm_base + ea, 4); u = _byteswap_ulong(u); float f; memcpy(&f, &u, 4); return f; }
static void fx_setf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); u = _byteswap_ulong(u); memcpy(vm_base + ea, &u, 4); }
static void fx_dof_debug(uint32_t p)
{
    static int plog = -1, force = -1, nlog = 0;
    static uint8_t last[0x40];
    static float fv[16]; static int fset[16];
    if (plog < 0) {
        plog = envi("DOF_PLOG", 0); force = envi("FORCE_DOF", 0);
        if (force) {
            /* defaults: plausible near/far ramps chosen by hand (NOT measured from a real ADS
             * frame -- no weapon at the test save); they exercise the same job / kernels / modes
             * (see scratch\overnight_native_dof.md). */
            static const struct { int off; float v; } def[] = {
                { 0x10, 0.0f }, { 0x14, 1.5f }, { 0x20, 3.0f }, { 0x24, 6.0f }, { 0x28, 1.0f },
                { 0x30, 30.0f }, { 0x34, 60.0f }, { 0x38, 1.0f } };
            for (auto& d : def) { fv[d.off / 4] = d.v; fset[d.off / 4] = 1; }
            std::string s = envs("FORCE_DOF_P", "");
            for (size_t i = 0; i < s.size();) {
                size_t c = s.find(',', i); if (c == std::string::npos) c = s.size();
                std::string kv = s.substr(i, c - i); size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    int off = (int)strtol(kv.substr(0, eq).c_str(), NULL, 16);
                    if (off >= 0 && off < 0x40 && !(off & 3)) { fv[off / 4] = (float)atof(kv.substr(eq + 1).c_str()); fset[off / 4] = 1; }
                }
                i = c + 1;
            }
            LOG("R3_FORCE_DOF: forcing depth of field (test only)");
        }
    }
    if (!p || !vm_base) return;
    if (force && fx_getf(p + 0x28) < 0.01f && fx_getf(p + 0x38) < 0.01f)
        for (int i = 1; i < 16; i++) if (fset[i]) fx_setf(p + 4 * i, fv[i]);
    if (plog && nlog < 400 && memcmp(last, vm_base + p, 0x40)) {
        memcpy(last, vm_base + p, 0x40); nlog++;
        char b[512]; int n = snprintf(b, sizeof b, "dof params @%08X flags=%08X", p, (unsigned)_byteswap_ulong(*(uint32_t*)(vm_base + p)));
        for (int i = 1; i < 16 && n < (int)sizeof b - 24; i++) n += snprintf(b + n, sizeof b - n, " %02X=%g", 4 * i, fx_getf(p + 4 * i));
        LOG("%s", b);
    }
}
/* r3.json hooks in func_00160108 / func_001600FC, game thread; only reached while the player's zoom fraction is
 * > 0 (aiming). 1 = take the game's own "not aiming" path (release the zoom DOF node). The first
 * 20 aims (calls after a >= 0.5 s gap or a mode / player change) are logged. */
extern "C" int r3_fx_dof_aim_skip(uint32_t player)
{
    if (!s_fx_init) fx_init();
    static uint32_t last_player; static int last_skip = -1, nlog;
    static ULONGLONG last_t;
    int skip = s_fx_dof == 2 || (s_fx_dof && !s_fx_ads);
    ULONGLONG t = GetTickCount64();
    if ((skip != last_skip || player != last_player || t - last_t >= 500) && nlog < 20 && vm_base && player) {
        nlog++;
        LOG("[fx] dof aim-off: %s (player %08X idx %d zoom %.2f)", skip ? "aiming, zoom DOF node skipped" : (s_fx_dof ? "aiming, DOF kept" : "aiming, DOF off (setting Off)"),
            player, (int)_byteswap_ulong(*(uint32_t*)(vm_base + player + 0x1680)), fx_getf(player + 0xACC));
    }
    last_skip = skip; last_player = player; last_t = t;
    return skip;
}
extern "C" void r3_fx_pass(int kind, uint32_t p)   /* r3.json hooks, game thread */
{
    if (!s_fx_init) fx_init();
    switch (kind) {
    case 0: if (!s_fx_grain) fx_zero(p + 0x8); break;
    case 1: if (!s_fx_mb) fx_zero(p + 0x14); break;
    case 2: if (!s_fx_mb) fx_zero(p + 0xC); break;
    case 3: fx_dof_debug(p); if (!s_fx_dof) { fx_zero(p + 0x28); fx_zero(p + 0x38); } break;
    }
}
static void fx_apply(void) { if (!s_fx_init) fx_init(); }

/* ======================================================================
 * Controller layout (XInput hook in cellPad.c pad_poll_xinput) */
enum { XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_LT, XB_RT, XB_LS, XB_RS, XB_BACK, XB_START, XB_COUNT };
static const char* s_xb_key[XB_COUNT] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START" };
static const wchar_t* s_xb_name[XB_COUNT] = { L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"Left stick click",
                                              L"Right stick click", L"View", L"Menu" };
static const WORD s_xb_bit[XB_COUNT] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START };
static volatile int s_xmap[XB_COUNT];   /* physical -> acts as */
static volatile int s_southpaw = 0, s_xmap_ident = 1;
static void xmap_load(void)
{
    s_xmap_ident = 1;
    for (int i = 0; i < XB_COUNT; i++) {
        char k[32]; snprintf(k, sizeof k, "PAD_MAP_%s", s_xb_key[i]);
        const char* e = getenv(k); s_xmap[i] = i;
        if (e && *e) for (int j = 0; j < XB_COUNT; j++) if (!_stricmp(e, s_xb_key[j])) s_xmap[i] = j;
        if (s_xmap[i] != i) s_xmap_ident = 0;
    }
    const char* sp = getenv("PAD_SOUTHPAW"); s_southpaw = sp && *sp && *sp != '0';
}
static void xi_hook(int port, void* gp_)
{
    (void)port;
    XINPUT_GAMEPAD* gp = (XINPUT_GAMEPAD*)gp_;
    if (s_southpaw) {
        std::swap(gp->sThumbLX, gp->sThumbRX);
        std::swap(gp->sThumbLY, gp->sThumbRY);
    }
    if (s_xmap_ident) return;
    int down[XB_COUNT];
    for (int i = 0; i < XB_COUNT; i++) down[i] = s_xb_bit[i] ? (gp->wButtons & s_xb_bit[i]) != 0 : 0;
    down[XB_LT] = gp->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    down[XB_RT] = gp->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const BYTE lt = gp->bLeftTrigger, rt = gp->bRightTrigger;
    WORD b = gp->wButtons;
    for (int i = 0; i < XB_COUNT; i++) b &= ~s_xb_bit[i];
    BYTE olt = 0, ort = 0;
    for (int i = 0; i < XB_COUNT; i++) {
        if (!down[i]) continue;
        const int t = s_xmap[i];
        const BYTE analog = i == XB_LT ? lt : i == XB_RT ? rt : 255;   /* trigger to trigger keeps its travel */
        if (t == XB_LT) olt = std::max(olt, analog);
        else if (t == XB_RT) ort = std::max(ort, analog);
        else b |= s_xb_bit[t];
    }
    gp->wButtons = b; gp->bLeftTrigger = olt; gp->bRightTrigger = ort;
}

/* ======================================================================
 * Movies (cutscene skip) and level names (Discord) from the file hooks */
static volatile int s_movie_slot = -1;          /* >= 0 while a movie handle is open */
/* issue #6: count every open movie handle; a second open (or a probe handle closed while the
 * stream plays) used to clear the single slot and disable the skip. PORT_MOVIE_TRACK=0: one slot. */
static std::mutex s_mv_mu;
static std::set<int> s_mv_slots;
static int movie_track(void)
{
    static int t = -1;
    if (t < 0) { const char* e = getenv("PORT_MOVIE_TRACK"); t = (e && *e) ? atoi(e) : 1; }
    return t;
}
static volatile ULONGLONG s_movie_t0 = 0;
/* Skip unwatched movies (R3_CUTSCENE_SKIP_ALL, default on; needs R3_CUTSCENE_SKIP). The game's movie
 * update func_003D6810 (once a frame per full-screen movie, r4 = &skip) takes the skip button
 * (func_0019D500(0x200)) only for a movie whose seen bit is set: with flag 0x400 of [0x0121E480] it
 * calls is_movie_seen (func_003C84F0: profile bitfield +0x2F50, index [0x011A3614]) at 0x003D6990 and
 * ignores the button when it is 0. Two hooks there (tools/hooks/r3.json) act only while the port's
 * 1 s hold is pulsing: r3_movie_seen_hook reports the bit as set, so the game runs its own skip
 * (same path as a seen movie or a natural end: Bink stopped, bit set, game continues), and
 * r3_movie_skip_btn_hook (PORT_SKIP_FORCE, default 1) sets the skip once 3 s of held pulses were
 * not seen as a press. The game's own gates (1.5 s into the movie, level loaded) stay. */
static volatile int s_cut_skip_all = 1;
static volatile ULONGLONG s_skip_armed_until = 0;   /* the hold is pulsing: hooks act until then */
static volatile ULONGLONG s_skip_hold_t0 = 0;       /* when that hold started */
static volatile ULONGLONG s_skip_hook_ms = 0;       /* a hook last saw the armed hold */
static volatile int s_skip_mech = 0;                /* 1 game skip, 2 + seen-check override, 3 forced skip flag */
static const char* skip_mech_name(int m)
{
    return m == 1 ? "game's own skip (movie already seen)" :
           m == 2 ? "game's own skip, seen check overridden (unwatched movie)" :
           m == 3 ? "forced skip flag (no skip press seen in 3 s)" : "natural end";
}
static int skip_force_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PORT_SKIP_FORCE"); on = (e && *e) ? atoi(e) != 0 : 1; }
    return on;
}
/* Test only (PORT_SKIP_TEST_UNSEEN=1, not in launcher/menu): the seen check answers "unwatched" for
 * every movie unless the hold override below is armed, so the override can be tested on a profile
 * that has seen everything. Only the answer changes; the game still sets the seen bit as usual. */
static int skip_test_unseen(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PORT_SKIP_TEST_UNSEEN"); on = (e && *e) ? atoi(e) != 0 : 0; }
    return on;
}
extern "C" int r3_movie_seen_hook(int seen)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= s_skip_armed_until) {
        if (seen && skip_test_unseen()) {
            static ULONGLONG logged_movie = ~0ull;
            if (logged_movie != s_movie_t0) {
                logged_movie = s_movie_t0;
                const int idx = vm_base ? (int)_byteswap_ulong(*(uint32_t*)(vm_base + 0x011A3614)) : -1;
                LOG("movie skip test: PORT_SKIP_TEST_UNSEEN forcing movie idx %d to unwatched", idx);
            }
            return 0;
        }
        return seen;
    }
    s_skip_hook_ms = now;
    if (!s_cut_skip_all) return skip_test_unseen() ? 0 : seen;
    if (seen && !skip_test_unseen()) return seen;
    if (s_skip_mech != 2) { s_skip_mech = 2; LOG("movie skip: unwatched movie, overriding the game's seen check (func_003D6810 @ 0x003D6990)"); }
    return 1;
}
extern "C" int r3_movie_skip_btn_hook(int pressed)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= s_skip_armed_until) return pressed;
    s_skip_hook_ms = now;
    if (pressed) { if (!s_skip_mech) s_skip_mech = 1; return pressed; }
    /* the forced flag only belongs to "skip unwatched movies"; with it off the game decides alone */
    if (!s_cut_skip_all || !skip_force_on() || now - s_skip_hold_t0 < 4000) return pressed;
    if (s_skip_mech != 3) { s_skip_mech = 3; LOG("movie skip: no skip press seen after 3 s of pulses, setting the game's skip flag (func_003D6810 @ 0x003D69A8)"); }
    return 1;
}
static std::mutex s_lvl_mu;
static std::string s_level;                /* "" = not known yet */
static volatile int s_level_gen = 0;
extern "C" int r3_level_gen(void) { return s_level_gen; }   /* r3_patches.cpp: FOV memo roles reset per level */
static void file_open_cb(int slot, const char* path)
{
    if (!path) return;
    std::string p(path);
    for (auto& c : p) { if (c == '\\') c = '/'; c = (char)tolower((unsigned char)c); }
    if (slot >= 0 && p.size() > 4 && p.compare(p.size() - 4, 4, ".bik") == 0) {
        /* not the boot movies (SKIP_INTRO) or the weapon previews in the menus */
        if (p.find("lby_insom") == std::string::npos && p.find("opn_loadingmovie") == std::string::npos &&
            p.find("mini_wpn") == std::string::npos) {
            if (movie_track()) {
                std::lock_guard<std::mutex> g(s_mv_mu);
                if (s_mv_slots.empty()) { s_movie_t0 = GetTickCount64(); s_skip_mech = 0; }
                s_mv_slots.insert(slot);
            } else { s_movie_t0 = GetTickCount64(); s_skip_mech = 0; }
            s_movie_slot = slot;
            LOG("movie open: %s (slot %d)", p.c_str(), slot);
        }
        return;
    }
    const size_t k = p.find("/levels/");   /* packed/levels/<name>/level_cached.psarc */
    if (k == std::string::npos || p.find("/level_") == std::string::npos) return;
    const size_t e = p.find('/', k + 8);
    if (e == std::string::npos) return;
    std::string lv = p.substr(k + 8, e - (k + 8));
    std::lock_guard<std::mutex> g(s_lvl_mu);
    if (lv != s_level) { s_level = lv; s_level_gen++; LOG("level: %s", lv.c_str()); }
}
static void movie_ended(void)
{
    if (s_skip_mech || s_skip_armed_until) LOG("movie closed: ended by %s", skip_mech_name(s_skip_mech));
    else LOG("movie closed");
    s_skip_mech = 0; s_skip_armed_until = 0;
}
static void file_close_cb(int slot)
{
    if (movie_track()) {
        std::lock_guard<std::mutex> g(s_mv_mu);
        if (s_mv_slots.erase(slot)) {
            s_movie_slot = s_mv_slots.empty() ? -1 : *s_mv_slots.begin();
            if (s_movie_slot < 0) movie_ended();
        }
    } else if (slot == s_movie_slot) { s_movie_slot = -1; movie_ended(); }
}
static std::string level_pretty(void)
{
    std::string lv; { std::lock_guard<std::mutex> g(s_lvl_mu); lv = s_level; }
    if (lv.empty()) return "Starting up";
    if (lv == "lobby" || lv == "frontend" || lv == "mainmenu" || lv.compare(0, 3, "mp_") == 0) return "In the menus";
    for (auto& c : lv) if (c == '_') c = ' ';
    bool up = true;
    for (auto& c : lv) { if (up && c >= 'a' && c <= 'z') c -= 32; up = c == ' '; }
    return lv;
}

/* ======================================================================
 * Discord Rich Presence over the local IPC pipe */
static volatile int s_discord_quit = 0;
static bool pipe_send(HANDLE h, uint32_t op, const std::string& json)
{
    std::string buf(8 + json.size(), 0);
    const uint32_t len = (uint32_t)json.size();
    memcpy(&buf[0], &op, 4); memcpy(&buf[4], &len, 4); memcpy(&buf[8], json.data(), json.size());
    DWORD w = 0;
    return WriteFile(h, buf.data(), (DWORD)buf.size(), &w, NULL) && w == buf.size();
}
static void pipe_drain(HANDLE h)
{
    DWORD avail = 0; char tmp[4096];
    while (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail) { DWORD r = 0; if (!ReadFile(h, tmp, std::min<DWORD>(avail, sizeof tmp), &r, NULL) || !r) break; }
}
static std::string json_esc(const std::string& s)
{
    std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
}
static DWORD WINAPI discord_thread(LPVOID)
{
    std::string appid = envs("DISCORD_APPID", "");
    if (appid.empty()) {                         /* discord_appid.txt in the game folder (the launcher can't hold text) */
        std::vector<std::string> l;
        if (read_lines("discord_appid.txt", l))
            for (auto& s : l) { std::string t = ltrim(s); while (!t.empty() && (unsigned char)t.back() <= ' ') t.pop_back();
                                if (!t.empty() && isdigit((unsigned char)t[0])) { appid = t; break; } }
    }
    if (appid.empty()) { LOG("Discord: no application id (set " PCX_PREFIX "_DISCORD_APPID or discord_appid.txt); Rich Presence off"); return 0; }
    const long long start = (long long)time(NULL);
    HANDLE h = INVALID_HANDLE_VALUE; int sent_gen = -1; ULONGLONG retry = 0, next_send = 0; int nonce = 0;
    while (!s_discord_quit) {
        const ULONGLONG now = GetTickCount64();
        if (h == INVALID_HANDLE_VALUE) {
            if (now < retry) { Sleep(500); continue; }
            retry = now + 60000;
            for (int i = 0; i < 10 && h == INVALID_HANDLE_VALUE; i++) {
                char name[64]; snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
                h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            }
            if (h == INVALID_HANDLE_VALUE) continue;   /* Discord not running: try again in a minute */
            if (!pipe_send(h, 0, "{\"v\":1,\"client_id\":\"" + json_esc(appid) + "\"}")) { CloseHandle(h); h = INVALID_HANDLE_VALUE; continue; }
            LOG("Discord: connected");
            sent_gen = -1;
        }
        pipe_drain(h);
        if (sent_gen != s_level_gen || now >= next_send) {
            sent_gen = s_level_gen; next_send = now + 15000 * 4;
            char js[1024];
            snprintf(js, sizeof js,
                "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"%s\",\"state\":\"%s\","
                "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"%s\"}}},\"nonce\":\"%d\"}",
                GetCurrentProcessId(), PCX_GAME_NAME, json_esc(level_pretty()).c_str(), start, PCX_GAME_NAME, ++nonce);
            if (!pipe_send(h, 1, js)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; retry = now + 60000; continue; }
        }
        Sleep(1000);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}

/* ======================================================================
 * Benchmark: inputs on a timeline, then frame times for N seconds */
struct BenchStep { double at; uint32_t btn; };
static std::vector<BenchStep> s_bench_steps;
static int s_bench = 0, s_bench_step = 0, s_bench_state = 0;   /* 0 script, 1 measuring, 2 done */
static double s_bench_t0 = 0, s_bench_m0 = 0, s_bench_secs = 60;
static double s_last_frame = 0;
static uint32_t btn_named(const std::string& n)
{
    static const struct { const char* n; uint32_t b; } t[] = { {"SELECT",1},{"L3",2},{"R3",4},{"START",8},{"UP",0x10},{"RIGHT",0x20},
        {"DOWN",0x40},{"LEFT",0x80},{"L2",0x100},{"R2",0x200},{"L1",0x400},{"R1",0x800},{"TRIANGLE",0x1000},{"CIRCLE",0x2000},
        {"CROSS",0x4000},{"SQUARE",0x8000} };
    for (auto& e : t) if (!_stricmp(n.c_str(), e.n)) return e.b;
    return 0;
}
/* "wait:BUTTON,wait:BUTTON,...,wait" -- seconds after the previous step; the
 * last plain number is the settle time before measuring. Default: the
 * scratch\r3bench.ps1 route (title, main menu, Campaign, continue / first
 * level), timed from process start. */
static void bench_parse(void)
{
    std::string sc = envs("BENCH_SCRIPT", "100:CROSS,8:CROSS,8:START,8:CROSS,8:CROSS,60:START,3:CROSS,30");
    double t = 0; size_t i = 0;
    while (i < sc.size()) {
        size_t e = sc.find(',', i); if (e == std::string::npos) e = sc.size();
        std::string tok = sc.substr(i, e - i); i = e + 1;
        const size_t c = tok.find(':');
        t += atof(tok.c_str());
        s_bench_steps.push_back({ t, c == std::string::npos ? 0u : btn_named(tok.substr(c + 1)) });
    }
}
/* Frame-time capture, shared by the scripted benchmark (R3_BENCH=1) and the hotkey capture (F11,
 * R3_BENCH_KEY): the game thread (r2x_frame_tick) appends to a preallocated buffer, the UI thread
 * starts / stops it and writes the report. */
#define BENCH_CAP_MAX 400000u                    /* ~28 min at 240 fps */
static float* s_cap_buf = NULL;
static volatile LONG s_cap_n = 0, s_cap_on = 0;
static double s_cap_t0 = 0;
static int s_cap_kind = 0;                       /* 1 scripted benchmark, 2 hotkey capture */
static int s_cap_key = 0x7A;                     /* VK_F11; 0 = off */
static int cap_start(int kind)
{
    if (!s_cap_buf) s_cap_buf = (float*)malloc(BENCH_CAP_MAX * sizeof(float));
    if (!s_cap_buf) return 0;
    s_cap_kind = kind; s_cap_t0 = now_s();
    InterlockedExchange(&s_cap_n, 0);
    InterlockedExchange(&s_cap_on, 1);
    return 1;
}
/* logs\bench\<P>_bench_<date>.txt (summary, read by the launcher), .csv (every frame) and a row in bench_history.csv */
static std::string bench_report(const float* ft, unsigned n, double secs, int kind)
{
    double us_draw = 0, dpf = 0, fr = 0;
    if (kind == 1) r2rsx_prof_sample(&fr, &dpf, &us_draw);
    std::vector<float> scratch(n ? n : 1);
    PcxBenchStats st; pcx_bench_stats(ft, n, secs, scratch.data(), &st);
    CreateDirectoryA("logs", NULL); CreateDirectoryA("logs\\bench", NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char base[MAX_PATH]; snprintf(base, sizeof base, "logs\\bench\\%s_bench_%04u%02u%02u_%02u%02u%02u", PCX_PREFIX,
                                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string txt = std::string(base) + ".txt", csv = std::string(base) + ".csv";
    if (FILE* f = fopen(txt.c_str(), "w")) {
        fprintf(f, "%s built-in benchmark\n", PCX_GAME_NAME);
        fprintf(f, "mode             %s\n", kind == 1 ? "scripted" : "hotkey capture");
        fprintf(f, "level            %s\n", level_pretty().c_str());
        fprintf(f, "seconds          %.1f\n", st.secs);
        fprintf(f, "frames           %u\n", st.frames);
        fprintf(f, "avg fps          %.2f\n", st.avg_fps);
        fprintf(f, "1%% low fps       %.2f\n", st.low1_fps);
        fprintf(f, "0.1%% low fps     %.2f\n", st.low01_fps);
        fprintf(f, "stutters         %u\n", st.stutters);
        fprintf(f, "lows             average of the slowest 1%% / 0.1%% frame times (CapFrameX style)\n");
        fprintf(f, "stutter rule     frame > 2x the average of the previous 30 and >= 4 ms over it\n");
        fprintf(f, "median ms        %.3f\n", st.median_ms);
        fprintf(f, "p99 ms           %.3f\n", st.p99_ms);
        fprintf(f, "max ms           %.3f\n", st.max_ms);
        if (kind == 1) {
            fprintf(f, "draws/frame      %.1f\n", dpf);
            fprintf(f, "us/draw          %.3f\n", us_draw);
        }
        fprintf(f, "frame cap        %s\n", getenv("R2_FPS") ? getenv("R2_FPS") : "default");
        fprintf(f, "frame times      %s\n", csv.c_str());
        fclose(f);
    }
    if (FILE* f = fopen(csv.c_str(), "w")) {
        fprintf(f, "frame,t_ms,frame_ms\n");
        double at = 0;
        for (unsigned i = 0; i < n; i++) { at += ft[i]; fprintf(f, "%u,%.3f,%.3f\n", i, at, ft[i]); }
        fclose(f);
    }
    const bool hdr = GetFileAttributesA("logs\\bench\\bench_history.csv") == INVALID_FILE_ATTRIBUTES;
    if (FILE* h = fopen("logs\\bench\\bench_history.csv", "a")) {
        if (hdr) fprintf(h, "date,game,level,seconds,frames,avg_fps,low1_fps,low01_fps,draws_per_frame,us_per_draw\n");
        fprintf(h, "%04u-%02u-%02u %02u:%02u:%02u,%s,%s,%.1f,%u,%.2f,%.2f,%.2f,%.1f,%.3f\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond, PCX_PREFIX, level_pretty().c_str(), st.secs, st.frames, st.avg_fps, st.low1_fps, st.low01_fps, dpf, us_draw);
        fclose(h);
    }
    LOG("benchmark (%s): avg %.2f fps, 1%% low %.2f, 0.1%% low %.2f, %u stutters, %.3f us/draw -> %s",
        kind == 1 ? "scripted" : "hotkey", st.avg_fps, st.low1_fps, st.low01_fps, st.stutters, us_draw, txt.c_str());
    char s[200]; snprintf(s, sizeof s, "%.1f fps avg, %.1f 1%% low, %.1f 0.1%% low, %u stutters (%u frames, %.0f s)",
                          st.avg_fps, st.low1_fps, st.low01_fps, st.stutters, st.frames, st.secs);
    return s;
}
static std::string cap_stop_report(void)
{
    InterlockedExchange(&s_cap_on, 0);
    const unsigned n = (unsigned)InterlockedCompareExchange(&s_cap_n, 0, 0);
    return bench_report(s_cap_buf, n, now_s() - s_cap_t0, s_cap_kind);
}
static void bench_tick(HWND game)
{
    if (!s_bench || s_bench_state == 2) return;
    const double t = now_s() - s_bench_t0;
    if (s_bench_state == 0) {
        while (s_bench_step < (int)s_bench_steps.size() && t >= s_bench_steps[s_bench_step].at) {
            const BenchStep& st = s_bench_steps[s_bench_step++];
            if (st.btn) { g_pad_inj_sticks = 0x80808080u; g_pad_inj_clear = 0; g_pad_inj_btn = st.btn; g_pad_inj_until = GetTickCount64() + 150; }
            LOG("benchmark: step %d at %.0f s", s_bench_step, t);
        }
        if (s_bench_step >= (int)s_bench_steps.size()) {
            if (s_cap_on) cap_stop_report();     /* a hotkey capture still running is saved first */
            s_bench_state = 1; s_bench_m0 = now_s(); cap_start(1);
            r2rsx_prof_enable(1); r2rsx_prof_sample(NULL, NULL, NULL);
            LOG("benchmark: measuring for %.0f s", s_bench_secs);
        }
    } else if (now_s() - s_bench_m0 >= s_bench_secs) {
        s_bench_state = 2;
        cap_stop_report();
        r2rsx_prof_enable(0);
        if (envi("BENCH_QUIT", 1) && game) PostMessageA(game, WM_CLOSE, 0, 0);
    }
}
/* the capture hotkey: press to start, press again to stop and save (also stops when the buffer is full) */
static void bench_key_tick(int focused, int other_prompt)
{
    static int was = 0;
    const int d = s_cap_key && focused && (GetAsyncKeyState(s_cap_key) & 0x8000) != 0;
    char key[8]; snprintf(key, sizeof key, "F%d", s_cap_key - 0x6F);
    if (d && !was && s_bench_state != 1 && !other_prompt) {
        if (!s_cap_on) {
            if (cap_start(2)) {
                char m[96]; snprintf(m, sizeof m, "Recording frame times. Press %s again to stop.", key);
                r2pf_hint("Benchmark", m, 3000); LOG("benchmark: hotkey capture started");
            }
        } else r2pf_hint("Benchmark saved (logs\\bench)", cap_stop_report().c_str(), 8000);
    } else if (s_cap_on && s_cap_kind == 2 && (unsigned)s_cap_n >= BENCH_CAP_MAX)
        r2pf_hint("Benchmark saved (buffer full)", cap_stop_report().c_str(), 8000);
    was = d;
}
/* stubs.cpp r2_frame_period: once per game frame */
extern "C" void r2x_frame_tick(void)
{
    const double t = now_s();
    if (s_cap_on && s_last_frame > 0) {
        const LONG i = s_cap_n;
        if ((unsigned)i < BENCH_CAP_MAX) { s_cap_buf[i] = (float)((t - s_last_frame) * 1000.0); InterlockedExchange(&s_cap_n, i + 1); }
    }
    s_last_frame = t;
    r2pc_reflex_sleep();                         /* NVIDIA Reflex: no-op unless R3_REFLEX is on and the driver has it */
    if (rsx_d3d12_engine_latency_frame() && g_pad_input_hook != rsx_d3d12_engine_latency_input)
        g_pad_input_hook = rsx_d3d12_engine_latency_input;   /* Direct3D 12: Reflex / Anti-Lag 2 */
    static int n = 0;
    if ((++n & 15) == 0) fx_apply();
}

/* ======================================================================
 * The extras menu */
struct XChoice { std::wstring label; std::string value; };
struct XOpt {
    const wchar_t* title; const wchar_t* help; const char* ini; const char* env; int restart;
    std::vector<XChoice> ch; int sel, run;
    void (*live)(const std::string& v);
};
static std::vector<XOpt> s_opts;
static int s_photo_on = 0;
static int s_crosshair = 0;             /* 0 off, 1 dot, 2 cross */
static int s_disc_pause_on = 1, s_cut_skip = 1, s_photo_allowed = 1;
static void live_fx_grain(const std::string& v) { fx_init(); s_fx_grain = v != "0"; }
static void live_fx_dof(const std::string& v) { fx_init(); const int m = fx_dof_mode(v); if (m == 2) s_fx_ads = 0; s_fx_dof = m == 2 ? 1 : m; }
static void live_fx_mb(const std::string& v) { fx_init(); s_fx_mb = v != "0"; }
static void live_cvd(const std::string& v) { g_pc_cvd_mode = atoi(v.c_str()); }
static int crosshair_parse(const std::string& v) { return v == "dot" || v == "1" ? 1 : v == "cross" || v == "2" ? 2 : 0; }
static void live_xhair(const std::string& v) { s_crosshair = crosshair_parse(v); }
extern "C" void r3_hud_reticle_set(int on);     /* stubs.cpp: the game's own crosshair / hit markers */
extern "C" void r3_hud_hitmarker_set(int on);
static void live_reticle(const std::string& v) { r3_hud_reticle_set(v != "0"); }
static void live_hitmarker(const std::string& v) { r3_hud_hitmarker_set(v != "0"); }
static unsigned s_xh_rgb = 0xF0F0F0; static int s_xh_scale = 100;
static void live_xhcolor(const std::string& v) { s_xh_rgb = pcx_xhair_rgb(v.c_str()); }
static void live_xhsize(const std::string& v) { s_xh_scale = pcx_xhair_scale(v.c_str()); }
static void live_benchkey(const std::string& v) { s_cap_key = pcx_bench_key(v.c_str()); }
static void live_dpause(const std::string& v) { s_disc_pause_on = v != "0"; }
static void live_cut(const std::string& v) { s_cut_skip = v != "0"; }
static void live_cut_all(const std::string& v) { s_cut_skip_all = v != "0"; }
static int skip_hint_auto(void) { return s_cut_skip && s_cut_skip_all; }
extern "C" void cellpad_set_pad_opt(const char* key, const char* v);   /* cellPad.c (SDL3 backend) */
static void live_gyro(const std::string& v) { cellpad_set_pad_opt("gyro", v.c_str()); }
static void live_gyrosens(const std::string& v) { cellpad_set_pad_opt("gyrosens", v.c_str()); }
static void live_gyroinvy(const std::string& v) { cellpad_set_pad_opt("gyroinvy", v.c_str()); }
static void live_gyrospace(const std::string& v) { cellpad_set_pad_opt("gyrospace", v.c_str()); }
static void live_lightbar(const std::string& v) { cellpad_set_pad_opt("lightbar", v.c_str()); }
static void live_nswap(const std::string& v) { cellpad_set_pad_opt("nswap", v.c_str()); }
/* PC extras (ps3emu/port_pc_extras.h): the cellPad.c keys are the environment variable names */
static void live_rumble(const std::string& v) { cellpad_set_pad_opt("PAD_RUMBLE", v.c_str()); }
static void live_padbg(const std::string& v) { cellpad_set_pad_opt("PAD_BACKGROUND_INPUT", v.c_str()); }
static void live_outerdz(const std::string& v) { cellpad_set_pad_opt("PAD_OUTER_DEADZONE", v.c_str()); }
static void live_antidz(const std::string& v) { cellpad_set_pad_opt("PAD_ANTI_DEADZONE", v.c_str()); }
static void live_trigdz(const std::string& v) { cellpad_set_pad_opt("PAD_TRIGGER_DEADZONE", v.c_str()); }
static void live_turnboost(const std::string& v) { cellpad_set_pad_opt("PAD_TURN_BOOST", v.c_str()); }
static void live_crouch(const std::string& v) { cellpad_set_pad_opt("PAD_CROUCH_MODE", v.c_str()); }
static void live_sprint(const std::string& v) { cellpad_set_pad_opt("PAD_SPRINT_MODE", v.c_str()); }
extern "C" void r3_aim_assist_set(const char* v);   /* stubs.cpp */
extern "C" void r3_aim_assist_init(void);
static void live_aimassist(const std::string& v) { r3_aim_assist_set(v.c_str()); }
extern "C" void cellaudio_set_pc_mix(int volume_pct, int mono);        /* cellAudio.c */
static void live_volume(const std::string& v) { cellaudio_set_pc_mix(v.empty() ? 100 : pcx_parse_pct(v.c_str(), 100, 0, 100), -1); }
static void live_mono(const std::string& v) { cellaudio_set_pc_mix(-1, v == "1"); }
static void live_uiscale(const std::string& v) { posd_ui_scale_set(v.c_str()); }
extern "C" void r2pf_overlay_graph_set(int on);                        /* r3_pcfeat.cpp */
static void live_ovgraph(const std::string& v) { r2pf_overlay_graph_set(v != "0"); }
/* cheats (r3_cheats.cpp): 0 God mode, 1 Infinite ammo, 2 Infinite grenades */
static int s_cheat_keys = 0;
static void cheat_row(int which, const std::string& v)
{
    const int on = r3_cheat_set(which, v == "1");
    if (on < 0 && v == "1") r2pf_hint("Cheat", on == -2 ? "Cheats are off while online (PORT_ONLINE)" : "Cheats are off in a benchmark run", 2500);
}
static void live_cheat_god(const std::string& v) { cheat_row(0, v); }
static void live_cheat_ammo(const std::string& v) { cheat_row(1, v); }
static void live_cheat_gren(const std::string& v) { cheat_row(2, v); }
static void live_cheat_keys(const std::string& v) { s_cheat_keys = v == "1"; }
static void opt_add(const wchar_t* title, const wchar_t* help, const char* ini, const char* env, int restart,
                    std::vector<XChoice> ch, const char* def, void (*live)(const std::string&))
{
    XOpt o{ title, help, ini, env, restart, std::move(ch), 0, 0, live };
    const char* e = getenv(env); std::string cur = (e && *e) ? e : def;
    o.run = 0;
    for (size_t k = 0; k < o.ch.size(); k++) if (!_stricmp(o.ch[k].value.c_str(), cur.c_str())) o.run = (int)k;
    o.sel = o.run;
    s_opts.push_back(std::move(o));
}
#define ENVN(x) PCX_PREFIX "_" x
static void opts_init(void)
{
    if (!s_opts.empty()) return;
    /* values match launcher_extra.inc exactly ("" = default: no settings.bat line), or the launcher drops them */
    const std::vector<XChoice> onoff = { {L"On", ""}, {L"Off", "0"} };
    opt_add(L"Film grain", L"The game's film grain noise (levels, multiplayer modes).", "fx_filmgrain", ENVN("FX_FILMGRAIN"), 0,
            onoff, "", live_fx_grain);
    opt_add(L"Depth of field", L"Background / foreground blur (cutscenes, aiming, some areas). The aiming blur alone: F10 > PC Options > Aim-down-sights blur.", "fx_dof", ENVN("FX_DOF"), 0,
            onoff, "", live_fx_dof);
    opt_add(L"Motion blur", L"Camera motion blur and per-object motion blur.", "fx_motionblur", ENVN("FX_MOTIONBLUR"), 0,
            onoff, "", live_fx_mb);
    opt_add(L"Colour-blind filter", L"Daltonization filter on the final image.", "colorblind", ENVN("COLORBLIND"), 0,
            { {L"Off", ""}, {L"Protanopia (red)", "1"}, {L"Deuteranopia (green)", "2"}, {L"Tritanopia (blue)", "3"} }, "", live_cvd);
    /* same key, values and env as launcher.cpp "gamma"; title / help as R1's row (R2/R3 apply it at the next start) */
    opt_add(L"Brightness", L"Picture gamma on top of the game's own brightness option (SDR; in HDR use HDR paper white). Takes effect on restart.",
            "gamma", "PS3_DISPLAY_GAMMA", 1,
            { {L"Game default", ""}, {L"Brighter", "0.9"}, {L"Brightest", "0.8"}, {L"Darker", "1.1"}, {L"Darkest", "1.2"} }, "", NULL);
    /* same keys, values and env as the launcher's "ssao" / "aoquality" rows; the renderer reads them at start-up */
    opt_add(L"Ambient occlusion", L"Game: the game's own AO. CACAO replaces it (no slow fade-in). Port SSAO: the port's extra AO. CACAO: AMD FidelityFX CACAO. Takes effect on restart.",
            "ssao", ENVN("AO_MODE"), 1,
            { {L"Game", ""}, {L"Port SSAO", "1"}, {L"CACAO", "cacao"}, {L"Off", "off"} }, "", NULL);
    opt_add(L"AO quality", L"CACAO quality: Low (3 samples), Medium (5) or High (12). Takes effect on restart.",
            "aoquality", ENVN("AO_QUALITY"), 1, { {L"Medium", ""}, {L"Low", "low"}, {L"High", "high"} }, "", NULL);
    opt_add(L"Game crosshair", L"The game's own crosshair (the lines around the screen centre). Weapon scopes are not affected.",
            "reticle", ENVN("RETICLE"), 0, onoff, "", live_reticle);
    opt_add(L"Hit markers", L"The marker the game draws over the crosshair when a shot hits.", "hitmarker", ENVN("HITMARKER"), 0,
            onoff, "", live_hitmarker);
    opt_add(L"Extra crosshair", L"A small extra marker at the screen centre while on foot (drawn by the port, on top of the game's own).", "crosshair", ENVN("CROSSHAIR"), 0,
            { {L"Off", ""}, {L"Dot", "dot"}, {L"Cross", "cross"} }, "", live_xhair);
    opt_add(L"Crosshair colour", L"Colour of the extra crosshair (high-contrast choices; a dark outline is always drawn).", "xhair_color", ENVN("CROSSHAIR_COLOR"), 0,
            { {L"White", ""}, {L"Green", "green"}, {L"Yellow", "yellow"}, {L"Cyan", "cyan"}, {L"Red", "red"}, {L"Pink", "pink"}, {L"Orange", "orange"} }, "", live_xhcolor);
    opt_add(L"Crosshair size", L"Size of the extra crosshair.", "xhair_size", ENVN("CROSSHAIR_SIZE"), 0,
            { {L"Small", ""}, {L"Medium", "medium"}, {L"Large", "large"}, {L"Huge", "huge"} }, "", live_xhsize);
    opt_add(L"Hold to skip movies", L"Hold Space / Enter / A / Menu for 1 s to skip a pre-rendered movie.", "cutscene_skip",
            ENVN("CUTSCENE_SKIP"), 0, onoff, "", live_cut);
    opt_add(L"Skip unwatched movies", L"With Hold to skip movies: also skip movies not watched before (the game itself only lets seen ones be skipped).",
            "cutscene_skip_all", ENVN("CUTSCENE_SKIP_ALL"), 0, onoff, "", live_cut_all);
    opt_add(L"Skip intro movies", L"Skips the start-up logo and loading movies.", "skipintro", "SKIP_INTRO", 1, onoff, "", NULL);
    opt_add(L"Pause on controller loss", L"Pauses when the controller in use disconnects.", "pad_disconnect_pause",
            ENVN("PAD_DISCONNECT_PAUSE"), 0, onoff, "", live_dpause);
    opt_add(L"HDR output", L"Experimental. Needs Windows HDR on (SDR otherwise). Auto: HDR10 only when Windows HDR is on.",
            "hdr", ENVN("HDR"), 1,
            { {L"Off", ""}, {L"Auto", "auto"}, {L"On (HDR10)", "1"}, {L"On (scRGB)", "scrgb"} }, "", NULL);
    opt_add(L"HDR paper white", L"Brightness of SDR white / the HUD in nits. Only highlights go above it.", "hdr_paper",
            ENVN("HDR_PAPER"), 1,
            { {L"203 nits", ""}, {L"100 nits", "100"}, {L"160 nits", "160"}, {L"250 nits", "250"}, {L"300 nits", "300"},
              {L"400 nits", "400"} }, "", NULL);
    opt_add(L"HDR peak", L"Brightest highlights. Display: the peak Windows reports. Same as paper white = no expansion.",
            "hdr_peak", ENVN("HDR_PEAK"), 1,
            { {L"Display", ""}, {L"400 nits", "400"}, {L"600 nits", "600"}, {L"800 nits", "800"}, {L"1000 nits", "1000"},
              {L"1500 nits", "1500"}, {L"2000 nits", "2000"} }, "", NULL);
    opt_add(L"HDR saturation", L"Colour saturation in HDR (1 = as the game).", "hdr_sat", ENVN("HDR_SAT"), 1,
            { {L"1.0", ""}, {L"0.9", "0.9"}, {L"1.1", "1.1"}, {L"1.2", "1.2"} }, "", NULL);
    opt_add(L"NVIDIA Reflex", L"Low-latency mode (VK_NV_low_latency2, GeForce). Experimental.", "reflex", ENVN("REFLEX"), 1,
            { {L"Off", ""}, {L"On", "1"}, {L"On + Boost", "2"} }, "", NULL);
    opt_add(L"Latency reduction", L"NVIDIA Reflex (GeForce), AMD Anti-Lag 2 (Radeon RX 5000+) or Auto (by GPU). Default: the Reflex row "
            L"alone decides. Off while frame generation is on. Experimental.", "latency", "PORT_LATENCY", 1,
            { {L"Default", ""}, {L"Off", "off"}, {L"NVIDIA Reflex", "reflex"}, {L"AMD Anti-Lag 2", "antilag"}, {L"Auto", "auto"} }, "", NULL);
    opt_add(L"Frame Generation", L"NVIDIA DLSS Frame Generation (RTX 40: 2x, RTX 50: up to 4x). Smoother when the CPU is the limit; "
            L"a little more input lag. Needs Anti-aliasing = NVIDIA DLSS. FSR 3 2x: AMD frame interpolation for other GPUs "
            L"(Anti-aliasing = FSR, DLSS or XeSS; no Reflex).", "dlss_fg", ENVN("DLSS_FG"), 1,
            { {L"Off", ""}, {L"2x", "2"}, {L"3x", "3"}, {L"4x", "4"}, {L"FSR 3 2x", "fsr3"} }, "", NULL);
    opt_add(L"Frame generation backend", L"Auto: DLSS Frame Generation, else FSR 3 2x. DLSS only: no fallback. Takes effect on restart.",
            "fg_backend", ENVN("FG_BACKEND"), 1,
            { {L"Auto", ""}, {L"DLSS Frame Generation only", "dlss"}, {L"AMD FSR 3", "fsr3"} }, "", NULL);
    /* launcher.cpp "language" (PS3_LANG, cellSysutil sysutil_language) */
    opt_add(L"Game language", L"The language the game uses (text and speech it has). Takes effect on restart.",
            "language", "PS3_LANG", 1,
            { {L"Windows language", ""}, {L"English", "en"}, {L"English (UK)", "en-gb"}, {L"French", "fr"}, {L"Spanish", "es"},
              {L"German", "de"}, {L"Italian", "it"}, {L"Portuguese (Brazil)", "pt-br"}, {L"Portuguese", "pt"}, {L"Dutch", "nl"},
              {L"Russian", "ru"}, {L"Polish", "pl"}, {L"Danish", "da"}, {L"Finnish", "fi"}, {L"Norwegian", "no"}, {L"Swedish", "sv"} }, "", NULL);
    opt_add(L"Discord Rich Presence", L"Shows the game and level in Discord (needs discord_appid.txt or " ENVN("DISCORD_APPID") L").",
            "discord", ENVN("DISCORD"), 1, { {L"Off", ""}, {L"On", "1"} }, "", NULL);
    opt_add(L"Benchmark capture key", L"Press to start recording frame times, again to stop: logs\\bench gets a summary (avg, 1% / 0.1% low, stutters) and a per-frame CSV.",
            "benchkey", ENVN("BENCH_KEY"), 0, { {L"F11", ""}, {L"F6", "F6"}, {L"F5", "F5"}, {L"Off", "off"} }, "", live_benchkey);
    opt_add(L"Remember window position", L"Windowed mode: reopen where the window was last left (window_pos.txt).", "winremember",
            "RSX_WINDOW_REMEMBER", 1, { {L"On", ""}, {L"Off", "0"} }, "", NULL);
    /* controllers (SDL3 backend, cellPad.c); keys and values match the launcher's Controls page */
    opt_add(L"Controller support", L"SDL3: DualShock / DualSense / Switch / Xbox directly (prompts, rumble, light bar, gyro). "
            L"XInput: the old path.", "padbackend", "PAD_BACKEND", 1, { {L"SDL3", ""}, {L"XInput", "xinput"} }, "", NULL);
    opt_add(L"Gyro aiming", L"Turn the view by turning a DualShock 4 / DualSense / Switch controller. While aiming: only with "
            L"the Aim button held.", "gyro", "PAD_GYRO", 0, { {L"Off", ""}, {L"On", "1"}, {L"While aiming", "aim"} }, "", live_gyro);
    opt_add(L"Gyro sensitivity", L"View degrees per controller degree.", "gyrosens", "PAD_GYRO_SENS", 0,
            { {L"Default (2x)", ""}, {L"0.5x", "0.5"}, {L"1x", "1"}, {L"1.5x", "1.5"}, {L"3x", "3"}, {L"4x", "4"}, {L"6x", "6"} },
            "", live_gyrosens);
    opt_add(L"Gyro vertical", L"Invert the up / down gyro.", "gyroinvy", "PAD_GYRO_INVERT_Y", 0,
            { {L"Normal", ""}, {L"Inverted", "1"} }, "", live_gyroinvy);
    opt_add(L"Gyro turning axis", L"Controller: the controller's own yaw axis. Player: follows gravity (flat or upright).",
            "gyrospace", "PAD_GYRO_SPACE", 0, { {L"Controller", ""}, {L"Player", "player"} }, "", live_gyrospace);
    opt_add(L"Light bar", L"DualShock 4 / DualSense light bar in the player's colour.", "lightbar", "PAD_LIGHTBAR", 0,
            { {L"Player colour", ""}, {L"Off", "0"} }, "", live_lightbar);
    opt_add(L"Nintendo button layout", L"Switch Pro / Joy-Con: Nintendo style confirms with A and shows Xbox-style letters.",
            "nswap", "PAD_NINTENDO_SWAP", 0, { {L"By position", ""}, {L"Nintendo style (A confirms)", "1"} }, "", live_nswap);
    /* PC extras: keys and values match launcher.cpp register_input_options / register_pc_options */
    opt_add(L"Vibration", L"Controller rumble strength.", "rumble", "PAD_RUMBLE", 0,
            { {L"100%", ""}, {L"75%", "75"}, {L"50%", "50"}, {L"25%", "25"}, {L"Off", "0"} }, "", live_rumble);
    opt_add(L"Confirm button", L"Which button accepts in the game's menus (Circle: Japanese PS3 layout). The game reads it at start-up.",
            "enterbtn", "PS3_ENTER_BUTTON", 1, { {L"Cross", ""}, {L"Circle", "circle"} }, "", NULL);
    opt_add(L"Controller in the background", L"Controller input while another window has the focus. Auto: only when "
            L"\"When the game is in the background\" is Keep running or Mute only.", "padbg", "PAD_BACKGROUND_INPUT", 0,
            { {L"Auto", ""}, {L"On", "1"}, {L"Off", "0"} }, "", live_padbg);
    opt_add(L"Stick outer deadzone", L"How much of the stick's edge already counts as full deflection.", "outerdz",
            "PAD_OUTER_DEADZONE", 0, { {L"Off", ""}, {L"5%", "5"}, {L"10%", "10"}, {L"15%", "15"} }, "", live_outerdz);
    opt_add(L"Stick anti-deadzone", L"Skips the game's own deadzone: the smallest stick movement already moves.", "antidz",
            "PAD_ANTI_DEADZONE", 0, { {L"Off", ""}, {L"10%", "10"}, {L"20%", "20"}, {L"30%", "30"} }, "", live_antidz);
    opt_add(L"Trigger deadzone", L"How far a trigger must be pulled before it counts.", "trigdz", "PAD_TRIGGER_DEADZONE", 0,
            { {L"Off", ""}, {L"10%", "10"}, {L"20%", "20"}, {L"30%", "30"} }, "", live_trigdz);
    opt_add(L"Turn boost", L"Extra turning speed (degrees per second) while the right stick is held at the edge. Not while aiming.",
            "turnboost", "PAD_TURN_BOOST", 0, { {L"Off", ""}, {L"90", "90"}, {L"180", "180"}, {L"270", "270"} }, "", live_turnboost);
    opt_add(L"Crouch button", L"As the game, Toggle (press on / press off) or Hold (crouch while held). Experimental.",
            "crouchmode", "PAD_CROUCH_MODE", 0, { {L"As the game", ""}, {L"Toggle", "toggle"}, {L"Hold", "hold"} }, "", live_crouch);
    opt_add(L"Sprint button", L"As the game, Toggle or Hold (sprint while held). Experimental.",
            "sprintmode", "PAD_SPRINT_MODE", 0, { {L"As the game", ""}, {L"Toggle", "toggle"}, {L"Hold", "hold"} }, "", live_sprint);
    r3_aim_assist_init();   /* normalises PORT_AIM_ASSIST (1 / 0 / auto) for the row below */
    opt_add(L"Aim assist", L"The game's aim assist: your aim slows down over an enemy and is pulled toward it. Auto: off "
            L"with keyboard & mouse, on with a controller. On: always, as on PS3. Off: never.", "aimassist",
            "PORT_AIM_ASSIST", 0, { {L"Auto", ""}, {L"On", "on"}, {L"Off", "off"} }, "", live_aimassist);
    opt_add(L"Master volume", L"All game sound, on top of the game's own volume options.", "volume", "PORT_VOLUME", 0,
            { {L"100%", ""}, {L"90%", "90"}, {L"80%", "80"}, {L"70%", "70"}, {L"60%", "60"}, {L"50%", "50"}, {L"40%", "40"},
              {L"30%", "30"}, {L"20%", "20"}, {L"10%", "10"}, {L"Muted", "0"} }, "", live_volume);
    opt_add(L"Mono audio", L"Mixes all channels to both ears (hearing in one ear).", "audio_mono", "PORT_AUDIO_MONO", 0,
            { {L"Off", ""}, {L"On", "1"} }, "", live_mono);
    opt_add(L"UI scale", L"Size of the PC menus and overlays. Auto grows them on 1440p / 4K screens. The F3 overlay and "
            L"notifications change at the next start.", "uiscale", "PORT_UI_SCALE", 0,
            { {L"Auto", ""}, {L"75%", "75"}, {L"100%", "100"}, {L"125%", "125"}, {L"150%", "150"}, {L"175%", "175"},
              {L"200%", "200"} }, "", live_uiscale);
    opt_add(L"Frame-time graph", L"A graph of the last 120 frame times under the F3 performance overlay.", "ovgraph",
            "PORT_OVERLAY_GRAPH", 0, { {L"On", ""}, {L"Off", "0"} }, "", live_ovgraph);
    /* cheats: single player only, never online / in a benchmark run; trophies off once one has been on */
    r3_cheats_init_env();
    const std::vector<XChoice> offon = { {L"Off", ""}, {L"On", "1"} };
    opt_add(L"Cheat: God mode", L"Single-player cheat: your health stays full and enemies can't hurt you. Enemies are not affected. "
            L"Trophies are off until the game is restarted.", "cheat_god", "PORT_CHEAT_GODMODE", 0, offon, "", live_cheat_god);
    opt_add(L"Cheat: Infinite ammo", L"Single-player cheat: your weapons never run out. Trophies are off until the game is restarted.",
            "cheat_ammo", "PORT_CHEAT_AMMO", 0, offon, "", live_cheat_ammo);
    opt_add(L"Cheat: Infinite grenades", L"Single-player cheat: your grenade count never drops. Trophies are off until the game is restarted.",
            "cheat_grenades", "PORT_CHEAT_GRENADES", 0, offon, "", live_cheat_gren);
    opt_add(L"Cheat hotkeys", L"Ctrl+F1 toggles God mode, Ctrl+F2 Infinite ammo and grenades (not while F1 / F2 is bound in controls.txt).",
            "cheat_keys", "PORT_CHEAT_KEYS", 0, offon, "", live_cheat_keys);
}
enum { PG_MAIN, PG_KEYS, PG_PAD };
static int s_open = 0, s_page = PG_MAIN, s_sel = 0, s_top = 0, s_capture = 0, s_release = 0;
static HWND s_ov = NULL, s_game = NULL;
static int s_prev_in = 0;

/* key binding rows: controls.txt lines that bind a key */
struct KeyRow { int line; std::string key, action, label; };
static std::vector<std::string> s_ctl_lines;
static std::vector<KeyRow> s_keys;
static void keys_load(void)
{
    s_ctl_lines.clear(); s_keys.clear();
    read_lines("controls.txt", s_ctl_lines);
    for (int i = 0; i < (int)s_ctl_lines.size(); i++) {
        std::string l = s_ctl_lines[i], com;
        const size_t h = l.find('#'); if (h != std::string::npos) { com = ltrim(l.substr(h + 1)); l = l.substr(0, h); }
        const size_t eq = l.find('='); if (eq == std::string::npos) continue;
        char k[64] = {}, v[64] = {};
        if (sscanf(l.substr(0, eq).c_str(), " %63s", k) != 1 || sscanf(l.substr(eq + 1).c_str(), " %63s", v) != 1) continue;
        if (starts_ci(k, "mouse_") || starts_ci(k, "menu_") || starts_ci(k, "pad_")) continue;
        s_keys.push_back({ i, k, v, com.empty() ? v : com });
    }
}
static const struct { const char* n; int vk; } s_vkn[] = {
    {"SPACE",VK_SPACE},{"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},{"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},{"ALT",VK_MENU},{"TAB",VK_TAB},
    {"ENTER",VK_RETURN},{"BACKSPACE",VK_BACK},{"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
    {"MOUSE1",VK_LBUTTON},{"MOUSE2",VK_RBUTTON},{"MOUSE3",VK_MBUTTON},{"MOUSE4",VK_XBUTTON1},{"MOUSE5",VK_XBUTTON2},
    {"F1",VK_F1},{"F2",VK_F2},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},{"F11",VK_F11},
    {"CAPSLOCK",VK_CAPITAL},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},{"HOME",VK_HOME},{"END",VK_END},{"PGUP",VK_PRIOR},{"PGDN",VK_NEXT},
    {"NUM0",VK_NUMPAD0},{"NUM1",VK_NUMPAD1},{"NUM2",VK_NUMPAD2},{"NUM3",VK_NUMPAD3},{"NUM4",VK_NUMPAD4},{"NUM5",VK_NUMPAD5},
    {"NUM6",VK_NUMPAD6},{"NUM7",VK_NUMPAD7},{"NUM8",VK_NUMPAD8},{"NUM9",VK_NUMPAD9},
    {"GRAVE",VK_OEM_3},{"MINUS",VK_OEM_MINUS},{"EQUALS",VK_OEM_PLUS},{"LBRACKET",VK_OEM_4},{"RBRACKET",VK_OEM_6},{"BACKSLASH",VK_OEM_5},
    {"SEMICOLON",VK_OEM_1},{"APOSTROPHE",VK_OEM_7},{"COMMA",VK_OEM_COMMA},{"PERIOD",VK_OEM_PERIOD},{"SLASH",VK_OEM_2},
};
/* a newly pressed key's controls.txt name ("" = none yet). F3/F7-F10/F12 and
 * Esc are the port's own keys and are not offered. */
static std::string capture_key(void)
{
    static bool held[256];
    std::string got;
    for (int vk = 1; vk < 256; vk++) {
        const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !held[vk] && got.empty()) {
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) got = std::string(1, (char)vk);
            else for (auto& e : s_vkn) if (e.vk == vk) { got = e.n; break; }
        }
        held[vk] = d;
    }
    return got;
}
static void keys_set(int row, const std::string& key)
{
    KeyRow& r = s_keys[row];
    char line[256]; snprintf(line, sizeof line, "%-10s = %-10s # %s", key.c_str(), r.action.c_str(), r.label.c_str());
    s_ctl_lines[r.line] = line; r.key = key;
    if (write_lines("controls.txt", s_ctl_lines)) cellpad_reload_controls();
    LOG("key binding: %s -> %s (%s)", key.c_str(), r.action.c_str(), r.label.c_str());
}

static int rows(void)
{
    if (s_page == PG_MAIN) return (int)s_opts.size() + 3;    /* + Key bindings, Controller layout, Back */
    if (s_page == PG_KEYS) return (int)s_keys.size() + 2;    /* + Restore defaults, Back */
    return XB_COUNT + 2;                                     /* + Southpaw, Back */
}
static std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static void row_text(int i, std::wstring& l, std::wstring& v)
{
    l.clear(); v.clear();
    const int n = rows();
    if (i == n - 1) { l = L"Back"; return; }
    if (s_page == PG_MAIN) {
        const int no = (int)s_opts.size();
        if (i == no) { l = L"Key bindings (keyboard & mouse)"; v = L"\u25B8"; return; }
        if (i == no + 1) { l = L"Controller layout"; v = L"\u25B8"; return; }
        const XOpt& o = s_opts[i];
        l = o.title; v = L"\u25C2  " + o.ch[o.sel].label + (o.restart && o.sel != o.run ? L" *" : L"") + L"  \u25B8";
    } else if (s_page == PG_KEYS) {
        if (i == (int)s_keys.size()) { l = L"Restore default keys"; return; }
        l = wide(s_keys[i].label);
        v = (s_capture && i == s_sel) ? L"press a key...  (Esc cancels)" : wide(s_keys[i].key);
    } else {
        if (i == XB_COUNT) { l = L"Southpaw (swap sticks)"; v = s_southpaw ? L"\u25C2  On  \u25B8" : L"\u25C2  Off  \u25B8"; return; }
        l = s_xb_name[i]; v = std::wstring(L"\u25C2  acts as ") + s_xb_name[s_xmap[i]] + L"  \u25B8";
    }
}
static std::wstring help_text(void)
{
    if (s_page == PG_MAIN && s_sel < (int)s_opts.size()) {
        const XOpt& o = s_opts[s_sel]; std::wstring s = o.help;
        if (o.restart) s += L"  (Applies the next time the game starts.)";
        return s;
    }
    if (s_page == PG_KEYS) return L"Enter / A: bind a new key or mouse button.  Saved to controls.txt.";
    if (s_page == PG_PAD) return L"\u2190\u2192 choose what each controller button does. Saved for the launcher too.";
    return L"";
}
static void paint(HDC dc, const RECT& rc)
{
    const int n = rows(), vis = std::min(n, 14);
    const int H = rc.bottom - rc.top, W = rc.right - rc.left, rh = H / (vis + 4);
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
    const int top = rc.top + rh * 3 / 2;
    auto font = [&](int px, int wt) { return CreateFontW(-px, 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"); };
    HFONT ft = font(rh * 62 / 100, FW_BOLD), fr = font(rh * 46 / 100, FW_SEMIBOLD), fs = font(rh * 34 / 100, FW_NORMAL);
    HGDIOBJ of = SelectObject(dc, ft);
    RECT r = { rc.left + rh / 2, rc.top + rh / 3, rc.right - rh / 2, top };
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, s_page == PG_MAIN ? L"MORE PC OPTIONS" : s_page == PG_KEYS ? L"KEY BINDINGS" : L"CONTROLLER LAYOUT", -1, &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, fr);
    const int vx = rc.left + W * 50 / 100;
    for (int k = 0; k < vis && s_top + k < n; k++) {
        const int i = s_top + k;
        RECT rr = { rc.left + 6, top + k * rh, rc.right - 6, top + (k + 1) * rh };
        if (i == s_sel) { HBRUSH b = CreateSolidBrush(RGB(150, 32, 24)); FillRect(dc, &rr, b); DeleteObject(b); }
        SetTextColor(dc, i == s_sel ? RGB(255, 255, 255) : RGB(205, 205, 210));
        std::wstring l, v; row_text(i, l, v);
        RECT rl = { rr.left + rh / 2, rr.top, vx, rr.bottom };
        DrawTextW(dc, l.c_str(), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT rv = { vx, rr.top, rr.right - rh / 2, rr.bottom };
        DrawTextW(dc, v.c_str(), -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(dc, fs);
    SetTextColor(dc, RGB(185, 185, 190));
    RECT rh1 = { rc.left + rh / 2, top + vis * rh + rh / 6, rc.right - rh / 2, top + vis * rh + rh * 7 / 6 };
    DrawTextW(dc, help_text().c_str(), -1, &rh1, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(140, 140, 150));
    RECT rh2 = { rc.left + rh / 2, top + vis * rh + rh * 7 / 6, rc.right - rh / 2, rc.bottom - rh / 6 };
    DrawTextW(dc, L"\u2191\u2193 select    \u2190\u2192 change    Enter / A  OK    Esc / B  back", -1, &rh2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of); DeleteObject(ft); DeleteObject(fr); DeleteObject(fs);
}
static std::wstring s_msg1, s_msg2;     /* a dialog instead of the menu (controller lost, photo mode) */
static void ov_draw(HDC mdc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20)); FillRect(mdc, &rc, bg); DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(190, 40, 30));
    for (int i = 0; i < 2; i++) { RECT r = { rc.left + i, rc.top + i, rc.right - i, rc.bottom - i }; FrameRect(mdc, &r, fr); }
    DeleteObject(fr);
    SetBkMode(mdc, TRANSPARENT);
    if (s_open) paint(mdc, rc);
    else {
        const int H = rc.bottom - rc.top;
        HFONT f1 = CreateFontW(-H * 26 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT f2 = CreateFontW(-H * 15 / 100, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ of = SelectObject(mdc, f1);
        RECT r1 = { rc.left + 8, rc.top + H * 10 / 100, rc.right - 8, rc.top + H * 55 / 100 };
        SetTextColor(mdc, RGB(255, 255, 255));
        DrawTextW(mdc, s_msg1.c_str(), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, f2);
        RECT r2 = { rc.left + 8, rc.top + H * 55 / 100, rc.right - 8, rc.bottom - H * 10 / 100 };
        SetTextColor(mdc, RGB(200, 200, 205));
        DrawTextW(mdc, s_msg2.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, of); DeleteObject(f1); DeleteObject(f2);
    }
}
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ obm = SelectObject(mdc, bm);
        ov_draw(mdc, rc);
        BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obm); DeleteObject(bm); DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}
/* kind 0 = menu (centre, large), 1 = dialog (centre), 2 = small note at the top.
 * Drawn into the swapchain (port_osd.h) when it is up, else a layered window. */
static int s_ov_kind = -1, s_ov_osd = 0, s_ov_cw = 0, s_ov_ch = 0;
static void ov_show(int kind)
{
    RECT rc; GetClientRect(s_game, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    int w, h, x, y;
    if (kind == 0) {
        const int rh = std::max(30, ch * 48 / 1000);
        w = std::max(640, cw * 50 / 100); h = rh * (std::min(rows(), 14) + 4);
        w = std::min(w, cw); h = std::min(h, ch);
        x = (cw - w) / 2; y = (ch - h) / 2;
    } else {
        w = std::max(kind == 2 ? 300 : 420, cw * (kind == 2 ? 28 : 46) / 100);
        h = std::max(kind == 2 ? 56 : 130, ch * (kind == 2 ? 8 : 20) / 100);
        x = (cw - w) / 2; y = kind == 2 ? ch * 4 / 100 : (ch - h) / 2;
    }
    posd_ui_scale_rect(&x, &y, &w, &h, cw, ch, kind == 2);   /* PORT_UI_SCALE */
    s_ov_kind = kind; s_ov_cw = cw; s_ov_ch = ch;
    s_ov_osd = posd_on();
    if (s_ov_osd) {
        if (s_ov && IsWindowVisible(s_ov)) ShowWindow(s_ov, SW_HIDE);
        PosdCanvas c;
        if (!c.begin(w, h)) return;
        const RECT r = { 0, 0, w, h };
        ov_draw(c.dc, r);
        c.commit(POSD_XMENU, x, y, cw, ch, 235 / 255.f);
        return;
    }
    r2rsx_osd_hide(POSD_XMENU);
    if (!s_ov) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"pcx_overlay";
        RegisterClassExW(&wc);
        s_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"pcx_overlay", L"", WS_POPUP, 0, 0, 10, 10, s_game, NULL, wc.hInstance, NULL);
        if (s_ov) SetLayeredWindowAttributes(s_ov, 0, 235, LWA_ALPHA);
    }
    if (!s_ov) return;
    POINT tl = { 0, 0 }; ClientToScreen(s_game, &tl);
    SetWindowPos(s_ov, HWND_TOPMOST, tl.x + x, tl.y + y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s_ov, NULL, TRUE);
    UpdateWindow(s_ov);
}
static void ov_hide(void) { s_ov_kind = -1; if (s_ov) ShowWindow(s_ov, SW_HIDE); r2rsx_osd_hide(POSD_XMENU); }
/* the content changed (menu selection, a value) */
static void ov_refresh(void)
{
    if (s_ov_kind < 0) return;
    if (s_ov_osd) ov_show(s_ov_kind);
    else if (s_ov) InvalidateRect(s_ov, NULL, FALSE);
}
/* per tick: follow a change of the client size or of the OSD becoming ready */
static void ov_follow(void)
{
    if (s_ov_kind < 0) return;
    RECT rc; GetClientRect(s_game, &rc);
    if (posd_on() != s_ov_osd || rc.right - rc.left != s_ov_cw || rc.bottom - rc.top != s_ov_ch) ov_show(s_ov_kind);
}

enum { IN_YES = 1, IN_NO = 2, IN_UP = 8, IN_DOWN = 16, IN_LEFT = 32, IN_RIGHT = 64, IN_F10 = 128 };
/* All controllers' buttons (XInput bits), the left stick as the d-pad;
 * from cellPad.c, which reads SDL3 or XInput (PAD_BACKEND). */
extern "C" unsigned cellpad_menu_buttons(int* any_connected);
static WORD xi_raw(int* any_connected) { return (WORD)cellpad_menu_buttons(any_connected); }
static int read_input(int focused)
{
    int r = 0;
    if (focused) {
        auto kd = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (kd(VK_RETURN) || kd(VK_SPACE)) r |= IN_YES;
        if (kd(VK_ESCAPE) || kd(VK_BACK)) r |= IN_NO;
        if (kd(VK_F10) || kd(VK_F9)) r |= IN_F10;
        if (kd(VK_UP)) r |= IN_UP;
        if (kd(VK_DOWN)) r |= IN_DOWN;
        if (kd(VK_LEFT)) r |= IN_LEFT;
        if (kd(VK_RIGHT)) r |= IN_RIGHT;
    }
    const WORD b = xi_raw(NULL);
    if (b & XINPUT_GAMEPAD_A) r |= IN_YES;
    if (b & XINPUT_GAMEPAD_B) r |= IN_NO;
    if (b & XINPUT_GAMEPAD_DPAD_UP) r |= IN_UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) r |= IN_DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) r |= IN_LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) r |= IN_RIGHT;
    return r;
}
static int edges(int in)
{
    static ULONGLONG rep_at = 0;
    const ULONGLONG now = GetTickCount64();
    int e = in & ~s_prev_in;
    const int dirs = in & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
    if (e & dirs) rep_at = now + 400;
    else if (dirs && rep_at && now >= rep_at) { e |= dirs; rep_at = now + 110; }
    if (!dirs) rep_at = 0;
    s_prev_in = in;
    return e;
}
static void opt_change(int i, int d)
{
    XOpt& o = s_opts[i];
    const int n = (int)o.ch.size();
    o.sel = (o.sel + d + n) % n;
    const std::string& v = o.ch[o.sel].value;
    ini_set(o.ini, v);
    bat_set(o.env, v);
    if (o.live) o.live(v);
    LOG("%ls = %ls%s", o.title, o.ch[o.sel].label.c_str(), o.restart ? " (next start)" : "");
}
/* the same options as rows in the game's own Options menu (r3_menurows.cpp), keyed by the ini name */
extern "C" int pcx_opt_find(const char* key)
{
    opts_init();
    for (size_t i = 0; key && i < s_opts.size(); i++) if (s_opts[i].ini && !strcmp(s_opts[i].ini, key)) return (int)i;
    return -1;
}
extern "C" int pcx_opt_nch(int i) { return i >= 0 && i < (int)s_opts.size() ? (int)s_opts[i].ch.size() : 0; }
extern "C" int pcx_opt_choice(int i, int k, char* out, int n)
{
    if (k < 0 || k >= pcx_opt_nch(i)) return 0;
    const int m = WideCharToMultiByte(CP_UTF8, 0, s_opts[i].ch[k].label.c_str(), -1, out, n, NULL, NULL);
    if (m <= 0 && n > 0) out[0] = 0;
    return m > 0;
}
extern "C" int pcx_opt_sel(int i) { return i >= 0 && i < (int)s_opts.size() ? s_opts[i].sel : 0; }
extern "C" void pcx_opt_set(int i, int k)
{
    if (k < 0 || k >= pcx_opt_nch(i)) return;
    XOpt& o = s_opts[i];
    opt_change(i, k - o.sel);                    /* (sel + d) % n == k: same path as the menu */
}
static void pad_change(int i, int d)
{
    if (i == XB_COUNT) {
        s_southpaw = !s_southpaw;
        ini_set("pad_southpaw", s_southpaw ? "1" : ""); bat_set("PAD_SOUTHPAW", s_southpaw ? "1" : "");
        return;
    }
    s_xmap[i] = (s_xmap[i] + d + XB_COUNT) % XB_COUNT;
    int ident = 1; for (int k = 0; k < XB_COUNT; k++) if (s_xmap[k] != k) ident = 0;
    s_xmap_ident = ident;
    char ini[32], env[32]; snprintf(ini, sizeof ini, "padmap_%s", s_xb_key[i]); snprintf(env, sizeof env, "PAD_MAP_%s", s_xb_key[i]);
    for (char* c = ini; *c; c++) *c = (char)tolower((unsigned char)*c);
    const std::string v = s_xmap[i] == i ? "" : s_xb_key[s_xmap[i]];
    ini_set(ini, v); bat_set(env, v);
}
extern "C" int r2x_menu_open(void) { return s_open || s_release; }
extern "C" void r2x_open_menu(void)
{
    opts_init();
    s_open = 1; s_page = PG_MAIN; s_sel = 0; s_top = 0; s_capture = 0;
    s_prev_in = -1;                              /* the key that opened it is not a press */
    g_port_ui_modal = 1;
    ClipCursor(NULL);
    ov_show(0);
    LOG("extras menu");
}
static void menu_close(void) { s_open = 0; s_capture = 0; s_release = 1; ov_hide(); }
static void menu_tick(int focused)
{
    g_port_ui_modal = 1;
    if (s_capture) {
        const std::string k = capture_key();
        if (s_capture == 1) { if (k.empty() && !(GetAsyncKeyState(VK_RETURN) & 0x8000) && !(GetAsyncKeyState(VK_SPACE) & 0x8000)) s_capture = 2; return; }
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000)) { s_capture = 0; s_prev_in = read_input(focused); ov_refresh(); return; }
        if (!k.empty() && focused) { keys_set(s_sel, k); s_capture = 0; s_prev_in = read_input(focused) | IN_YES; ov_refresh(); }
        return;
    }
    const int in = read_input(focused);
    if (s_prev_in < 0) { s_prev_in = in; return; }
    const int e = edges(in), n = rows();
    if (e & IN_F10) { menu_close(); return; }
    if (e & IN_UP) s_sel = (s_sel + n - 1) % n;
    else if (e & IN_DOWN) s_sel = (s_sel + 1) % n;
    else if (e & IN_NO) {
        if (s_page == PG_MAIN) { menu_close(); return; }
        s_sel = (int)s_opts.size() + (s_page == PG_PAD); s_page = PG_MAIN; s_top = 0; ov_show(0); return;
    } else if (e & (IN_LEFT | IN_RIGHT | IN_YES)) {
        const int d = (e & IN_LEFT) ? -1 : 1;
        const bool ok = (e & IN_YES) != 0;
        if (s_sel == n - 1) {                    /* Back */
            if (!ok) return;
            if (s_page == PG_MAIN) { menu_close(); return; }
            s_sel = (int)s_opts.size() + (s_page == PG_PAD); s_page = PG_MAIN; s_top = 0; ov_show(0); return;
        }
        if (s_page == PG_MAIN) {
            const int no = (int)s_opts.size();
            if (s_sel < no) opt_change(s_sel, d);
            else if (ok) { s_page = s_sel == no ? PG_KEYS : PG_PAD; s_sel = 0; s_top = 0; if (s_page == PG_KEYS) keys_load(); ov_show(0); return; }
        } else if (s_page == PG_KEYS) {
            if (!ok) return;
            if (s_sel == (int)s_keys.size()) {   /* defaults: cellPad writes a fresh file */
                CopyFileA("controls.txt", "controls.txt.bak", FALSE);
                DeleteFileA("controls.txt"); cellpad_reload_controls();
                LOG("key bindings reset (old file: controls.txt.bak)");
                s_page = PG_MAIN; s_sel = (int)s_opts.size(); ov_show(0); return;
            }
            s_capture = 1;
        } else pad_change(s_sel, d);
    } else return;
    ov_refresh();
}

/* ---- crosshair: a click-through colour-keyed window (or OSD layer) at the centre ---- */
static HWND s_xh = NULL; static int s_xh_kind = -1;
static unsigned s_xh_krgb = 0; static int s_xh_kscale = 0;    /* colour / size the marker was last drawn with */
static int xh_px(void) { return (24 * s_xh_scale / 100 + 1) & ~1; }   /* canvas side: 24 px at 100 % */
static void xh_draw(HDC dc, const RECT& rc)
{
    HBRUSH k = CreateSolidBrush(RGB(255, 0, 255)); FillRect(dc, &rc, k); DeleteObject(k);
    const int c = rc.right / 2;
    const double s = s_xh_scale / 100.0;
    auto px = [&](double v) { return (int)floor(v * s + 0.5); };
    const int ol = std::max(1, px(1));           /* dark outline width */
    HBRUSH o = CreateSolidBrush(RGB(0, 0, 0)),
           f = CreateSolidBrush(RGB((s_xh_rgb >> 16) & 255, (s_xh_rgb >> 8) & 255, s_xh_rgb & 255));
    if (s_xh_kind == 1) {
        const int h = std::max(2, px(3));
        RECT a = { c - h, c - h, c + h, c + h }; FillRect(dc, &a, o);
        RECT b = { c - h + ol, c - h + ol, c + h - ol, c + h - ol }; FillRect(dc, &b, f);
    } else {
        const int n = px(3), x = px(10), w = std::max(2, px(2));
        RECT a = { c - x, c - w, c - n, c + w }, b = { c + n, c - w, c + x, c + w }, d = { c - w, c - x, c + w, c - n }, e = { c - w, c + n, c + w, c + x };
        for (RECT* r : { &a, &b, &d, &e }) { FillRect(dc, r, o); RECT in = { r->left + ol, r->top + ol, r->right - ol, r->bottom - ol }; FillRect(dc, &in, f); }
    }
    DeleteObject(o); DeleteObject(f);
    s_xh_krgb = s_xh_rgb; s_xh_kscale = s_xh_scale;
}
static LRESULT CALLBACK xh_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        xh_draw(dc, rc);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
static void crosshair_tick(int show)
{
    if (!show) { if (s_xh && IsWindowVisible(s_xh)) ShowWindow(s_xh, SW_HIDE); r2rsx_osd_hide(POSD_XHAIR); return; }
    RECT rc; GetClientRect(s_game, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    const int sz = xh_px(), changed = s_xh_kind != s_crosshair || s_xh_krgb != s_xh_rgb || s_xh_kscale != s_xh_scale;
    if (posd_on()) {
        if (s_xh && IsWindowVisible(s_xh)) ShowWindow(s_xh, SW_HIDE);
        static int kcw = 0, kch = 0;
        if (changed || cw != kcw || ch != kch || !r2rsx_osd_visible(POSD_XHAIR)) {
            s_xh_kind = s_crosshair; kcw = cw; kch = ch;
            if (s_xh) InvalidateRect(s_xh, NULL, TRUE);
            PosdCanvas c;
            if (!c.begin(sz, sz)) return;
            const RECT r = { 0, 0, sz, sz };
            xh_draw(c.dc, r);
            c.commit(POSD_XHAIR, cw / 2 - sz / 2, ch / 2 - sz / 2, cw, ch, 220 / 255.f, RGB(255, 0, 255));
        }
        return;
    }
    r2rsx_osd_hide(POSD_XHAIR);
    if (!s_xh) {
        WNDCLASSEXW wc = { sizeof wc }; wc.lpfnWndProc = xh_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"pcx_crosshair";
        RegisterClassExW(&wc);
        s_xh = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"pcx_crosshair", L"", WS_POPUP, 0, 0, sz, sz, s_game, NULL, wc.hInstance, NULL);
        if (!s_xh) return;
        SetLayeredWindowAttributes(s_xh, RGB(255, 0, 255), 220, LWA_COLORKEY | LWA_ALPHA);
    }
    if (changed) { s_xh_kind = s_crosshair; s_xh_krgb = s_xh_rgb; s_xh_kscale = s_xh_scale; InvalidateRect(s_xh, NULL, TRUE); }
    POINT c = { cw / 2, ch / 2 }; ClientToScreen(s_game, &c);
    SetWindowPos(s_xh, HWND_TOPMOST, c.x - sz / 2, c.y - sz / 2, sz, sz, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
/* ======================================================================
 * The per-tick driver, called at the end of r3_pcport.cpp ui_tick */
static int s_init = 0, s_disc_paused = 0, s_photo_ov = 0, s_photo_hide = 0;
/* R3_PHOTO_HIDE_HUD: 1 = enabled, 2 = enabled and photo mode on (VKR2FrameGen.cpp r2fg_photo_source) */
extern "C" { volatile int g_port_photo_hud = 0; }
static void init_once(void)
{
    if (s_init) return;
    s_init = 1;
    opts_init();
    for (auto& o : s_opts) if (o.live) o.live(o.ch[o.run].value);   /* apply the start-up values */
    s_photo_allowed = envi("PHOTO", 1) != 0;
    s_photo_hide = s_photo_allowed && envi("PHOTO_HIDE_HUD", 1) != 0;
    g_port_photo_hud = s_photo_hide ? 1 : 0;
    if (envi("DISCORD", 0)) { HANDLE t = CreateThread(NULL, 0, discord_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
    s_bench = envi("BENCH", 0) != 0;
    if (s_bench) {
        bench_parse();
        s_bench_secs = std::max(5.0, atof(envs("BENCH_SECONDS", "60").c_str()));
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
        const double since = ((double)((((ULONGLONG)nowft.dwHighDateTime) << 32 | nowft.dwLowDateTime) -
                                       (((ULONGLONG)c.dwHighDateTime) << 32 | c.dwLowDateTime))) / 1e7;
        s_bench_t0 = now_s() - since;            /* the script is timed from process start */
        LOG("benchmark mode: %zu steps, then %.0f s", s_bench_steps.size(), s_bench_secs);
    }
    LOG("extras: F9 / PC menu -> More PC Options; photo mode %s (F7); cutscene skip %s (unwatched movies %s); crosshair %d; pause on pad loss %s",
        s_photo_allowed ? "on" : "off", s_cut_skip ? "on" : "off", s_cut_skip_all ? "on" : "off", s_crosshair, s_disc_pause_on ? "on" : "off");
    if (skip_test_unseen())
        LOG("movie skip test: PORT_SKIP_TEST_UNSEEN=1, every movie reports unwatched to the game's skip check (seen bits unchanged)");
}
static int s_want_pause = 0;
extern "C" int r2x_want_pause(void) { return s_want_pause; }
/* controls.txt binds this key to something (a game action, or a port key such as mouse_release_key):
 * then Ctrl+<key> would press that too, so the cheat key is not used. */
static bool cheat_key_bound(const char* key)
{
    std::vector<std::string> lines;
    read_lines("controls.txt", lines);
    for (std::string l : lines) {
        const size_t h = l.find('#'); if (h != std::string::npos) l = l.substr(0, h);
        const size_t eq = l.find('='); if (eq == std::string::npos) continue;
        char k[64] = {}, v[64] = {};
        if (sscanf(l.substr(0, eq).c_str(), " %63s", k) != 1) continue;
        sscanf(l.substr(eq + 1).c_str(), " %63s", v);
        const size_t kl = strlen(k);
        if (!_stricmp(k, key) || (kl > 4 && !_stricmp(k + kl - 4, "_key") && !_stricmp(v, key))) return true;
    }
    return false;
}
/* the F9 row of a cheat follows a hotkey toggle (shown and saved like a menu change) */
static void cheat_row_sync(const char* ini, int on)
{
    const int i = pcx_opt_find(ini);
    if (i < 0) return;
    XOpt& o = s_opts[i];
    for (size_t k = 0; k < o.ch.size(); k++)
        if (o.ch[k].value == (on ? "1" : "")) { o.sel = o.run = (int)k; ini_set(o.ini, o.ch[k].value); bat_set(o.env, o.ch[k].value); }
}
/* Ctrl+F1 God mode, Ctrl+F2 Infinite ammo and grenades (r3_cheats.cpp); only with the Cheat hotkeys row on */
static void cheat_key(int which, const char* key)
{
    const char* name = which ? "Infinite ammo and grenades" : "God mode";
    char t[128];
    if (cheat_key_bound(key)) {
        snprintf(t, sizeof t, "Ctrl+%s does nothing: %s is bound in controls.txt", key, key);
        r2pf_hint("Cheat", t, 3500);
        LOG("cheat: Ctrl+%s ignored, %s is bound in controls.txt", key, key);
        return;
    }
    const int on = which ? r3_cheat_set(1, !g_r3_cheat_ammo) : r3_cheat_set(0, -1);
    if (on < 0) { r2pf_hint("Cheat", on == -2 ? "Cheats are off while online (PORT_ONLINE)" : "Cheats are off in a benchmark run", 2500); return; }
    if (which) r3_cheat_set(2, on);
    if (which) { cheat_row_sync("cheat_ammo", on); cheat_row_sync("cheat_grenades", on); }
    else cheat_row_sync("cheat_god", on);
    snprintf(t, sizeof t, "%s %s (trophies off until you restart the game)", name, on ? "ON" : "OFF");
    r2pf_hint("Cheat", t, 3500);
    LOG("cheat: %s %s", name, on ? "on" : "off");
}
extern "C" void r2x_ui_tick(void* hwnd_, int focused, int other_prompt)
{
    HWND hwnd = (HWND)hwnd_;
    s_game = hwnd;
    init_once();
    const ULONGLONG now = GetTickCount64();
    bench_tick(hwnd);
    bench_key_tick(focused, other_prompt);

    /* F9: the extras menu directly; F10 -> More PC Options opens it from the PC menu */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      if (d && !was && !s_open && !other_prompt && !s_photo_on) r2x_open_menu();
      was = d; }
    /* cheat hotkeys (opt-in): plain Ctrl (no Shift / Alt), not in photo mode, the port's menus or another
       prompt (r3_cheats.cpp refuses in a benchmark run and online) */
    { const int ctrl = focused && s_cheat_keys && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
                       && !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000);
      static int was1 = 0, was2 = 0;
      const int d1 = ctrl && (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
      const int d2 = ctrl && (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
      const int ok = !s_open && !s_release && !other_prompt && !s_photo_on;
      if (d1 && !was1 && ok) cheat_key(0, "F1");
      if (d2 && !was2 && ok) cheat_key(1, "F2");
      was1 = d1; was2 = d2; }
    if (s_open) menu_tick(focused);
    else if (s_release) {                        /* hold the game's input until the closing key is let go */
        if (!(read_input(focused) & (IN_YES | IN_NO | IN_F10))) { s_release = 0; g_port_ui_modal = 0; }
        else g_port_ui_modal = 1;
    }

    /* photo mode */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
      if (d && !was && s_photo_allowed && !s_open && !other_prompt) {
          s_photo_on = !s_photo_on;
          static int ov_was = 0;
          if (s_photo_on) { ov_was = r2pf_overlay_get(); r2pf_overlay_set(0); s_msg1 = L"Photo mode"; s_msg2 = L"F12 screenshot    F7 back to the game";
                            ov_show(2); s_photo_ov = 1; LOG("photo mode on"); }
          else { r2pf_overlay_set(ov_was); ov_hide(); s_photo_ov = 0; s_release = 1; LOG("photo mode off"); }
      }
      was = d;
      if (s_photo_hide) g_port_photo_hud = s_photo_on ? 2 : 1; }
    /* the note fades after 2 s so it is not in the way (screenshots never include it) */
    { static ULONGLONG t_on = 0;
      if (s_photo_on && s_photo_ov) { if (!t_on) t_on = now; if (now - t_on > 2000) { ov_hide(); s_photo_ov = 0; } }
      if (!s_photo_on) t_on = 0; }

    /* pause when the controller in use goes away */
    { int conn = 0; xi_raw(&conn);
      static int was_conn = -1;
      if (was_conn == 1 && !conn && s_disc_pause_on && !s_open && !s_photo_on && !s_bench) {
          s_disc_paused = 1; s_msg1 = L"Controller disconnected"; s_msg2 = L"Reconnect it, or press Enter to continue";
          ov_show(1); LOG("controller lost: paused");
      }
      if (s_disc_paused && (conn || (focused && (GetAsyncKeyState(VK_RETURN) & 0x8000)))) {
          s_disc_paused = 0; s_release = 1; if (!s_open) ov_hide(); LOG("controller pause ended");
      }
      was_conn = conn; }

    /* hold to skip a pre-rendered movie (PORT_MOVIE_TRACK: never while the gameplay look code
     * runs, so a leaked movie handle cannot turn a held A into Start presses in play). Whenever
     * skipping is not possible (no movie, or the look guard blocks) the 1 s hold restarts. */
    static ULONGLONG held_since = 0;
    if (!(s_cut_skip && s_movie_slot >= 0 && !s_open &&
          !(movie_track() && g_r2_look_ms && now - g_r2_look_ms < 250))) held_since = 0;
    else {
        static ULONGLONG told = 0;
        const WORD b = xi_raw(NULL);
        const int held = (focused && ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000))) ||
                         (b & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START));
        if (told != s_movie_t0 && now - s_movie_t0 > 500) { told = s_movie_t0; if (skip_hint_on()) r2pf_hint("Cutscene", "Hold Space / A to skip", 4000); }
        if (!held) held_since = 0;
        else if (!held_since) held_since = now;
        else if (now - held_since >= 1000) {
            /* issue #6: A = Cross and Menu = Start (Space/Enter = Cross), so the held key kept that
             * button down and its injected pulse never made a press edge. While skipping, Cross/Start
             * read as released between pulses. PORT_SKIP_CLEAR=0: old OR-only pulses. */
            static int clear_on = -1;
            if (clear_on < 0) { const char* e = getenv("PORT_SKIP_CLEAR"); clear_on = (e && *e) ? atoi(e) : 1; }
            const int pulse = (now - held_since) % 400 < 120;
            if (pulse || clear_on) {
                g_pad_inj_sticks = 0x80808080u;
                g_pad_inj_clear = clear_on ? 0x4008u /* CROSS|START */ : 0u;
                g_pad_inj_btn = !pulse ? 0u : ((now - held_since) / 400) & 1 ? 0x4000u /* CROSS */ : 0x0008u /* START */;
                g_pad_inj_until = now + (pulse ? 80 : 30);
            }
            static ULONGLONG logged = 0;
            if (logged != s_movie_t0) { logged = s_movie_t0; LOG("movie skip: held 1 s, pulsing Start/Cross%s", s_cut_skip_all ? " (unwatched movies too)" : ""); }
            /* arms the func_003D6810 hooks (r3_movie_seen_hook / r3_movie_skip_btn_hook) */
            s_skip_hold_t0 = held_since;
            s_skip_armed_until = now + 250;
            static ULONGLONG waited = 0;
            if (waited != s_movie_t0 && now - held_since > 6000 && now - s_skip_hook_ms > 6000) {
                waited = s_movie_t0;
                LOG("movie skip: the game has not reached its skip check for 5 s (still loading, or not a skippable movie)");
            }
        }
    }

    const int on_foot = g_r2_look_ms && now - g_r2_look_ms < 250;
    crosshair_tick(s_crosshair && focused && on_foot && !s_open && !s_photo_on && !other_prompt);

    ov_follow();
    s_want_pause = s_open || s_photo_on || s_disc_paused;
    /* the game gets no input while any of ours is up; released once, when they all end */
    { static int held = 0;
      if (s_open || s_release || s_photo_on || s_disc_paused) { g_port_ui_modal = 1; held = 1; }
      else if (held) { held = 0; if (!other_prompt) g_port_ui_modal = 0; } }
}

/* registered before main(); callbacks run once the game does */
static struct PcExtraInit {
    PcExtraInit()
    {
        xmap_load();
        g_port_xi_hook = xi_hook;
        g_port_file_open_cb = file_open_cb;
        g_port_file_close_cb = file_close_cb;
    }
} s_pcextra_init;
