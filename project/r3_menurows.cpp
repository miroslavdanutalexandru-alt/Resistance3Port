/* Resistance 3 PC port: the PC settings as their own tabs in the game's Options menu, one per category
 * (PC DISPLAY, PC GRAPHICS, PC EFFECTS, PC CONTROLS & HUD, PC AUDIO & LANGUAGE; same names and order
 * as the R1 / R2 ports).
 *
 * The Options screen (lobby.swf front end, hud_pause.swf pause; AS class lobby.Options) gets its
 * rows from native code: func_003816E8 builds a GFx array of rows and Invokes "Refresh(rows)" through
 * func_0032D788 (call site 0x383D44, return address 0x383D74). A row is an array
 * [id, type, label, value, choices]; type 1 = a selection (value = index into choices). The AS sorts
 * the rows into one array per tab by id (optionArrays: CONTROLS, AUDIO, VISUAL, [3D in the front
 * end,] MOVE; tab labels in categories.listArray, set by Options.Intro); an id it does not know would
 * land at the end of CONTROLS. The MOVE tab must stay last: Refresh locks the last tab
 * (LockPSMoveRelatedOptions) and the front end locks tab 3 (3D) without a 3D TV.
 * Changes come back as ExternalInterface "<manager>:changeOption"(id, value) -> func_00383E08 with the
 * method hash at [0x11508B8]; for an unknown id the game only rebuilds the list (no profile save,
 * nothing written into the game's options), which re-sends Refresh and so re-runs the steps below.
 *
 *   r3_menu_rows_append  (tools/hooks/r3.json, func_0032D788 entry): on that Refresh call, notes the
 *                        menu and the selected PC row (mode 1), or appends the rows to the list the
 *                        game sends (mode 2, and mode 1 while the menu still queues its calls).
 *   r3_menu_rows_after   (r3.json, after func_0032D788): mode 1, once the game's Refresh has run, with
 *                        nested Invokes on the same menu: adds one tab [label, 1] per category before
 *                        MOVE (categories.listArray.splice + categories.Refresh) if they are not there
 *                        yet, moves the MOVE lock to the new last tab, inserts each category's rows as
 *                        its own optionArrays entry (ids 100+), re-shows the current tab
 *                        (SetOptionsList) and puts the selection back. The game's own tab navigation
 *                        does the rest: up/down picks a category, accept / right opens its list, back
 *                        returns to the tab list. If the tabs cannot be added (an Invoke returns
 *                        nothing), it falls back to mode 2 for this and every later rebuild, so the
 *                        rows never vanish.
 *   r3_menu_rows_change  (r3.json, func_00383E08): changeOption for our ids -> the setting.
 *
 * The settings are the PC menu's own (F10: r3_pcport.cpp pcm_opt_*, "More PC Options":
 * r3_pcextra.cpp pcx_opt_*): a change takes the same path (settings.ini + settings.bat, applied at
 * once where it can be), so the F10 menu, the launcher and these rows always agree. Rows whose
 * setting needs a restart say "(restart)". Brightness is the More PC Options "gamma" row (one shared
 * state with F10); upscaling quality, a launcher-only setting, is written here the way the launcher does.
 *
 *   R3_MENU_ROWS=1   the PC category tabs (default)
 *   R3_MENU_ROWS=2   the rows at the end of the CONTROLS tab, grouped under "--- PC DISPLAY ---" etc.
 *                    section headers (ids 90+, changes ignored)
 *   R3_MENU_ROWS=0   off (F10 / hold Esc still has every PC setting)
 * Log: "[menurows] ..." in logs\run.log (first tabs / appends, fallbacks, every change). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "ppu_recomp.h"

void func_0032F0B4(ppu_context* ctx);   /* GFx CreateArray(r3 = GFxValue* out) */
void func_009FCAE0(ppu_context* ctx);   /* GFx array PushBack(r3 = arr+0, r4 = arr+8, r5 = GFxValue*) */
void func_009FBC6C(ppu_context* ctx);   /* GFx release managed value(r3 = v+0, r4 = v, r5 = v+8) */
void func_0032D788(ppu_context* ctx);   /* menu Invoke(r3 menu, r4 name, r5 GFxValue args, r6 count, r7 result) */

extern "C" {
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
uint32_t ps3_hle_heap_alloc(uint32_t size);
/* r3_pcport.cpp (F10 PC menu) */
int pcm_opt_find(const char* key);
int pcm_opt_nch(int i);
int pcm_opt_choice(int i, int k, char* out, int n);
int pcm_opt_sel(int i);
void pcm_opt_set(int i, int k);
int pcm_ini_get(const char* key, char* out, int n);
void pcm_ini_set(const char* key, const char* v);
void pcm_bat_set(const char* env, const char* v);
/* r3_pcextra.cpp (More PC Options) */
int pcx_opt_find(const char* key);
int pcx_opt_nch(int i);
int pcx_opt_choice(int i, int k, char* out, int n);
int pcx_opt_sel(int i);
void pcx_opt_set(int i, int k);
}

namespace {

enum { SRC_PCM, SRC_PCX, SRC_UPSCALE };
/* The PC categories, one tab each (in this order, before MOVE). Same names and order in the R1 and
 * R2 ports' menus. A category none of whose rows exist is left out. */
enum { CAT_DISPLAY, CAT_GRAPHICS, CAT_EFFECTS, CAT_CONTROLS, CAT_AUDIO, kNCats };
constexpr const char* k_cat_label[kNCats] = { "PC DISPLAY", "PC GRAPHICS", "PC EFFECTS", "PC CONTROLS & HUD", "PC AUDIO & LANGUAGE" };
struct RowDef { const char* key; int src; int cat; const char* label; };
/* the rows, grouped by category in tab order; a key the menus do not have (any more) is skipped */
constexpr RowDef k_rows[] = {
    { "fps",           SRC_PCM,     CAT_DISPLAY,  "Frame rate (restart)" },
    { "fov",           SRC_PCM,     CAT_DISPLAY,  "Field of view" },
    { "aspect",        SRC_PCM,     CAT_DISPLAY,  "Ultrawide / aspect ratio (restart)" },
    { "gamma",         SRC_PCX,     CAT_DISPLAY,  "Brightness (restart)" },
    { "hdr",           SRC_PCX,     CAT_DISPLAY,  "HDR output (restart)" },
    { "fsr",           SRC_PCM,     CAT_GRAPHICS, "Anti-aliasing (restart)" },
    { "upscale",       SRC_UPSCALE, CAT_GRAPHICS, "Upscaling (restart)" },
    { "dlss_fg",       SRC_PCX,     CAT_GRAPHICS, "Frame generation (restart)" },
    { "reflex",        SRC_PCX,     CAT_GRAPHICS, "NVIDIA Reflex (restart)" },
    { "fx_dof",        SRC_PCX,     CAT_EFFECTS,  "Depth of field" },
    { "fx_ads_blur",   SRC_PCM,     CAT_EFFECTS,  "Aim-down-sights blur" },
    { "fx_ads_approx", SRC_PCM,     CAT_EFFECTS,  "Aim blur (restart)" },
    { "fx_motionblur", SRC_PCX,     CAT_EFFECTS,  "Motion blur" },
    { "fx_filmgrain",  SRC_PCX,     CAT_EFFECTS,  "Film grain" },
    { "colorblind",    SRC_PCX,     CAT_EFFECTS,  "Colour-blind filter" },
    { "mouse_sens",    SRC_PCM,     CAT_CONTROLS, "Mouse sensitivity" },
    { "aimassist",     SRC_PCX,     CAT_CONTROLS, "Aim assist" },
    { "padglyphs",     SRC_PCM,     CAT_CONTROLS, "Button prompts" },
    { "rumble",        SRC_PCX,     CAT_CONTROLS, "Vibration" },
    { "crosshair",     SRC_PCX,     CAT_CONTROLS, "Crosshair" },
    { "xhair_color",   SRC_PCX,     CAT_CONTROLS, "Crosshair colour" },
    { "xhair_size",    SRC_PCX,     CAT_CONTROLS, "Crosshair size" },
    { "cutscene_skip", SRC_PCX,     CAT_CONTROLS, "Hold to skip movies" },
    { "cutscene_skip_all", SRC_PCX, CAT_CONTROLS, "Skip unwatched movies" },
    { "volume",        SRC_PCX,     CAT_AUDIO,    "Master volume" },
    { "audio_mono",    SRC_PCX,     CAT_AUDIO,    "Mono audio" },
    { "language",      SRC_PCX,     CAT_AUDIO,    "Game language (restart)" },
};
constexpr int kNRows = (int)(sizeof k_rows / sizeof k_rows[0]);
constexpr bool rows_grouped(void)
{
    for (int r = 1; r < kNRows; r++) if (k_rows[r].cat < k_rows[r - 1].cat) return false;
    return true;
}
constexpr int max_cat_rows(void)
{
    int best = 0;
    for (int c = 0; c < kNCats; c++) {
        int n = 0;
        for (int r = 0; r < kNRows; r++) n += k_rows[r].cat == c;
        if (n > best) best = n;
    }
    return best;
}
/* header rows (only in the CONTROLS layout, R3_MENU_ROWS=2): kHeaderBase + category */
constexpr int kHeaderBase = 90, kIdBase = 100;
constexpr int kGameMaxId = 24;                     /* lobby.Options.Refresh knows ids 0..24 */
/* A tab is a scrolling lobby.components.List (_visibleI = 10); keep every category to one screen.
 * The tab list (categories) shows every tab: 3-4 game tabs + kNCats + MOVE = at most 10 rows of
 * ~30 px from y 184 (lobby.swf) / 28 px from y 227 (hud_pause.swf), well above the button hints. */
static_assert(rows_grouped(), "k_rows must be grouped by category, in tab order");
static_assert(max_cat_rows() <= 10, "a PC category tab shows at most 10 rows without scrolling");
static_assert(kNCats <= 5, "the Options tab list has room for at most 5 PC tabs");
static_assert(kHeaderBase > kGameMaxId && kHeaderBase + kNCats <= kIdBase, "PC row ids must be unknown to the game's options code");
constexpr uint32_t kRetRefresh = 0x00383D74u;      /* func_003816E8's Invoke("Refresh") */
constexpr uint32_t kHashChangeOption = 0x011508B8u; /* func_0038492C: hash of "changeOption" */

/* upscaling: a launcher-only setting (no F10 row keeps its own copy), kept the way launcher.cpp writes it */
struct LocalChoice { const char* label; const char* value; };
const LocalChoice k_upscale[] = { { "Native (DLAA)", "1" }, { "Quality (67%)", "0.667" }, { "Balanced (58%)", "0.58" },
                                  { "Performance (50%)", "0.5" } };
struct LocalOpt { const char* ini; const char* env; const LocalChoice* ch; int n; const char* def; };
const LocalOpt k_local[1] = {
    { "upscale", "RSX_UPSCALE", k_upscale, 4, "1" },
};
std::string local_value(const LocalOpt& o)
{
    char b[64];
    if (pcm_ini_get(o.ini, b, sizeof b)) return b;
    const char* e = getenv(o.env);
    return e ? e : o.def;
}
int local_sel(const LocalOpt& o)
{
    const std::string v = local_value(o);
    for (int k = 0; k < o.n; k++) if (v == o.ch[k].value) return k;
    for (int k = 0; k < o.n; k++) if (*o.ch[k].value && atof(v.c_str()) == atof(o.ch[k].value)) return k;
    for (int k = 0; k < o.n; k++) if (!strcmp(o.ch[k].value, o.def)) return k;
    return 0;
}
void local_set(int which, int k)
{
    const LocalOpt& o = k_local[which];
    const char* v = o.ch[k].value;
    pcm_ini_set(o.ini, v);
    /* as launcher.cpp / r3_pcport.cpp: upscaling needs an upscaler */
    char fsr[32];
    if (!pcm_ini_get("fsr", fsr, sizeof fsr)) { const char* e = getenv("RSX_FSR"); snprintf(fsr, sizeof fsr, "%s", e ? e : "0"); }
    pcm_bat_set(o.env, (!fsr[0] || !strcmp(fsr, "0")) ? "1" : v);
}

/* one resolved row: where its setting lives */
struct Row { int def; int idx; };
std::vector<Row> resolve_rows(void)
{
    std::vector<Row> out;
    for (int r = 0; r < kNRows; r++) {
        const RowDef& d = k_rows[r];
        int idx = -1;
        if (d.src == SRC_PCM) idx = pcm_opt_find(d.key);
        else if (d.src == SRC_PCX) idx = pcx_opt_find(d.key);
        else idx = d.src - SRC_UPSCALE;
        if (idx < 0) continue;
        out.push_back({ r, idx });
    }
    return out;
}
int row_nch(const Row& w)
{
    switch (k_rows[w.def].src) {
    case SRC_PCM: return pcm_opt_nch(w.idx);
    case SRC_PCX: return pcx_opt_nch(w.idx);
    default: return k_local[w.idx].n;
    }
}
std::string row_choice(const Row& w, int k)
{
    char b[256] = "";
    switch (k_rows[w.def].src) {
    case SRC_PCM: pcm_opt_choice(w.idx, k, b, sizeof b); break;
    case SRC_PCX: pcx_opt_choice(w.idx, k, b, sizeof b); break;
    default: snprintf(b, sizeof b, "%s", k_local[w.idx].ch[k].label); break;
    }
    return b;
}
int row_sel(const Row& w)
{
    switch (k_rows[w.def].src) {
    case SRC_PCM: return pcm_opt_sel(w.idx);
    case SRC_PCX: return pcx_opt_sel(w.idx);
    default: return local_sel(k_local[w.idx]);
    }
}
void row_set(const Row& w, int k)
{
    switch (k_rows[w.def].src) {
    case SRC_PCM: pcm_opt_set(w.idx, k); break;
    case SRC_PCX: pcx_opt_set(w.idx, k); break;
    default: local_set(w.idx, k); break;
    }
}

int menu_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char* e = getenv("R3_MENU_ROWS");
        m = (e && *e) ? atoi(e) : 1;
        if (m != 0 && m != 2) m = 1;
    }
    return m;
}
int enabled(void) { return menu_mode() != 0; }
std::mutex s_mu;

/* strings handed to GFx stay alive for good (GFx copies them, but a queued Invoke may not have yet) */
uint32_t guest_str(const std::string& s)
{
    static std::unordered_map<std::string, uint32_t> s_pool;
    auto it = s_pool.find(s);
    if (it != s_pool.end()) return it->second;
    const uint32_t ea = ps3_hle_heap_alloc((uint32_t)s.size() + 16);
    if (!ea) return 0;
    memcpy(vm_base + ea, s.c_str(), s.size() + 1);
    s_pool.emplace(s, ea);
    return ea;
}

void gcall(ppu_context* ctx, void (*f)(ppu_context*))
{
    ctx->lr = kRetRefresh;
    f(ctx);
    while (g_trampoline_fn) { void (*t)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t((void*)ctx); }
}
void gfx_zero(uint32_t v) { for (int i = 0; i < 16; i += 4) vm_write32(v + i, 0); }
void gfx_new_array(ppu_context* ctx, uint32_t v) { gfx_zero(v); ctx->gpr[3] = v; gcall(ctx, func_0032F0B4); }
void gfx_push(ppu_context* ctx, uint32_t arr, uint32_t v)
{
    ctx->gpr[3] = vm_read32(arr); ctx->gpr[4] = vm_read32(arr + 8); ctx->gpr[5] = v;
    gcall(ctx, func_009FCAE0);
}
void gfx_release(ppu_context* ctx, uint32_t v)
{
    if (vm_read32(v + 4) & 0x40) {
        ctx->gpr[3] = vm_read32(v); ctx->gpr[4] = v; ctx->gpr[5] = vm_read32(v + 8);
        gcall(ctx, func_009FBC6C);
    }
    gfx_zero(v);
}
void gfx_push_num(ppu_context* ctx, uint32_t arr, uint32_t tmp, double d)
{
    uint64_t b; memcpy(&b, &d, 8);
    vm_write32(tmp, 0); vm_write32(tmp + 4, 3); vm_write64(tmp + 8, b);
    gfx_push(ctx, arr, tmp);
}
void gfx_push_str(ppu_context* ctx, uint32_t arr, uint32_t tmp, const std::string& s)
{
    const uint32_t ea = guest_str(s);
    if (!ea) { gfx_push_num(ctx, arr, tmp, 0.0); return; }
    vm_write32(tmp, 0); vm_write32(tmp + 4, 4); vm_write32(tmp + 8, ea); vm_write32(tmp + 12, 0);
    gfx_push(ctx, arr, tmp);
}
/* one row [id, 1, label, value, [choices]] pushed onto `outer`; scratch = 3 GFxValues */
void push_row(ppu_context* ctx, uint32_t outer, uint32_t scratch, int id, const std::string& label, int sel,
              const std::vector<std::string>& choices)
{
    const uint32_t row = scratch, ch = scratch + 16, tmp = scratch + 32;
    gfx_new_array(ctx, row);
    if ((vm_read32(row + 4) & 0x8F) != 7) { gfx_release(ctx, row); return; }
    gfx_push_num(ctx, row, tmp, (double)id);
    gfx_push_num(ctx, row, tmp, 1.0);
    gfx_push_str(ctx, row, tmp, label);
    gfx_push_num(ctx, row, tmp, (double)sel);
    gfx_new_array(ctx, ch);
    for (auto& c : choices) gfx_push_str(ctx, ch, tmp, c);
    gfx_push(ctx, row, ch);
    gfx_release(ctx, ch);
    gfx_push(ctx, outer, row);
    gfx_release(ctx, row);
}

struct SavedRegs {
    uint64_t gpr[32]; double fpr[32]; ppu_vr vr[32];
    uint32_t cr, xer, fpscr, vscr; uint64_t lr, ctr;
};
void save_regs(const ppu_context* c, SavedRegs& s)
{
    memcpy(s.gpr, c->gpr, sizeof s.gpr); memcpy(s.fpr, c->fpr, sizeof s.fpr); memcpy(s.vr, c->vr, sizeof s.vr);
    s.cr = c->cr; s.xer = c->xer; s.fpscr = c->fpscr; s.vscr = c->vscr; s.lr = c->lr; s.ctr = c->ctr;
}
void load_regs(ppu_context* c, const SavedRegs& s)
{
    memcpy(c->gpr, s.gpr, sizeof s.gpr); memcpy(c->fpr, s.fpr, sizeof s.fpr); memcpy(c->vr, s.vr, sizeof s.vr);
    c->cr = s.cr; c->xer = s.xer; c->fpscr = s.fpscr; c->vscr = s.vscr; c->lr = s.lr; c->ctr = s.ctr;
}

bool row_shown(const Row& w) { return row_nch(w) > 0; }
/* the categories that have at least one row, in tab order */
std::vector<int> present_cats(const std::vector<Row>& rows)
{
    std::vector<int> out;
    for (const Row& w : rows)
        if (row_shown(w) && (out.empty() || out.back() != k_rows[w.def].cat)) out.push_back(k_rows[w.def].cat);
    return out;
}
/* one row per setting of category `cat` (-1 = all), pushed onto `arr`; scratch = 3 GFxValues */
int push_cat_rows(ppu_context* ctx, uint32_t arr, uint32_t scratch, const std::vector<Row>& rows, int cat)
{
    int n_rows = 0;
    for (const Row& w : rows) {
        if (cat >= 0 && k_rows[w.def].cat != cat) continue;
        std::vector<std::string> ch;
        const int n = row_nch(w);
        for (int k = 0; k < n; k++) ch.push_back(row_choice(w, k));
        if (ch.empty()) continue;
        int sel = row_sel(w);
        if (sel < 0 || sel >= n) sel = 0;
        push_row(ctx, arr, scratch, kIdBase + w.def, k_rows[w.def].label, sel, ch);
        n_rows++;
    }
    return n_rows;
}
/* the CONTROLS layout: per category a section header row, then its rows */
int push_all_rows(ppu_context* ctx, uint32_t arr, uint32_t scratch, const std::vector<Row>& rows)
{
    int n_rows = 0;
    const std::vector<int> cats = present_cats(rows);
    for (const int c : cats) {
        push_row(ctx, arr, scratch, kHeaderBase + c, std::string("--- ") + k_cat_label[c] + " ---", 0, { "" });
        n_rows += 1 + push_cat_rows(ctx, arr, scratch, rows, c);
    }
    return n_rows;
}

/* a stack frame of our own below the caller's; every register comes back on scope exit */
struct Frame {
    ppu_context* ctx; SavedRegs sv; uint32_t sp;
    explicit Frame(ppu_context* c) : ctx(c)
    {
        save_regs(c, sv);
        const uint32_t r1 = (uint32_t)sv.gpr[1];
        sp = (r1 - 0x800u) & ~0xFu;
        vm_write64(sp, r1);                          /* back chain */
        c->gpr[1] = sp;
    }
    ~Frame() { load_regs(ctx, sv); }
};

PPU_THREAD_LOCAL int t_nest = 0;                   /* inside one of our own Invokes */

void gfx_arg_num(uint32_t a, double d)
{
    uint64_t b; memcpy(&b, &d, 8);
    vm_write32(a, 0); vm_write32(a + 4, 3); vm_write64(a + 8, b);
}
void gfx_copy(uint32_t dst, uint32_t src) { for (int i = 0; i < 16; i += 4) vm_write32(dst + i, vm_read32(src + i)); }
bool gfx_read_str(uint32_t v, std::string& out)
{
    const uint32_t ty = vm_read32(v + 4);
    uint32_t s = vm_read32(v + 8);
    if ((ty & 0x8F) != 4 || !s || s >= 0xF0000000u) return false;
    if (ty & 0x40) s = vm_read32(s);                 /* managed: a string node, +0 = the characters */
    if (!s || s >= 0xF0000000u) return false;
    for (uint32_t i = 0; i < 1024 && vm_base[s + i]; i++) out += (char)vm_base[s + i];
    return true;
}

/* nested Invokes on the options menu (same wrapper, same f1 as the game's own Refresh call);
 * frame layout: +0x100 result, +0x120 up to 4 args, +0x180.. our arrays, +0x200 row scratch */
class MenuCalls {
public:
    MenuCalls(ppu_context* c, uint32_t menu, double f1, uint32_t base) : ctx_(c), menu_(menu), f1_(f1), base_(base) {}
    uint32_t arg(int i) const { return base_ + 0x120 + 16u * (uint32_t)i; }
    uint32_t slot(int i) const { return base_ + 0x180 + 16u * (uint32_t)i; }
    uint32_t scratch() const { return base_ + 0x200; }
    void invoke_ea(uint32_t name, int nargs, uint32_t res)
    {
        if (res) gfx_zero(res);
        ctx_->gpr[3] = menu_; ctx_->gpr[4] = name; ctx_->gpr[5] = nargs ? arg(0) : 0; ctx_->gpr[6] = (uint32_t)nargs;
        ctx_->gpr[7] = res; ctx_->gpr[9] = 0; ctx_->gpr[10] = 0; ctx_->fpr[1] = f1_;
        t_nest++;
        gcall(ctx_, func_0032D788);
        t_nest--;
    }
    bool call(const char* name, int nargs)
    {
        const uint32_t ea = guest_str(name);
        if (!ea) return false;
        invoke_ea(ea, nargs, res());
        gfx_release(ctx_, res());
        return true;
    }
    bool str(const char* name, std::string& out)
    {
        out.clear();
        const uint32_t ea = guest_str(name);
        if (!ea) return false;
        invoke_ea(ea, 0, res());
        const bool ok = gfx_read_str(res(), out);
        gfx_release(ctx_, res());
        return ok;
    }
    bool num(const char* name, int nargs, double& d)
    {
        const uint32_t ea = guest_str(name);
        if (!ea) return false;
        invoke_ea(ea, nargs, res());
        const bool ok = (vm_read32(res() + 4) & 0x8F) == 3;
        if (ok) { const uint64_t b = vm_read64(res() + 8); memcpy(&d, &b, 8); }
        gfx_release(ctx_, res());
        return ok;
    }
    int boolean(const char* name, int nargs)      /* 1 / 0, -1 = no answer */
    {
        const uint32_t ea = guest_str(name);
        if (!ea) return -1;
        invoke_ea(ea, nargs, res());
        const uint32_t ty = vm_read32(res() + 4) & 0x8F;
        int r = -1;
        if (ty == 2) r = vm_base[res() + 8] != 0;
        else if (ty == 3) { double d; const uint64_t b = vm_read64(res() + 8); memcpy(&d, &b, 8); r = d != 0.0; }
        gfx_release(ctx_, res());
        return r;
    }
    bool array(const char* name, int nargs, uint32_t out)   /* the result moves to `out`; release it there */
    {
        const uint32_t ea = guest_str(name);
        if (!ea) return false;
        invoke_ea(ea, nargs, res());
        if ((vm_read32(res() + 4) & 0x8F) != 7) { gfx_release(ctx_, res()); return false; }
        gfx_copy(out, res());
        gfx_zero(res());
        return true;
    }
private:
    uint32_t res() const { return base_ + 0x100; }
    ppu_context* ctx_; uint32_t menu_; double f1_; uint32_t base_;
};

/* noted on the game's Refresh call, used once it returns */
struct Pending { uint32_t menu = 0, name = 0, outer = 0; double f1 = 0; int sel = -1; };
PPU_THREAD_LOCAL Pending t_pend;
bool s_fallback = false;                           /* the tab did not take: mode 2 from now on */

/* The tabs, after the game's Refresh: returns null, or why they could not be added. */
const char* add_tab(ppu_context* ctx, MenuCalls& m, const std::vector<Row>& rows, const Pending& p)
{
    const std::vector<int> cats = present_cats(rows);
    if (cats.empty()) return nullptr;
    const int ncat = (int)cats.size();
    auto has_all = [&](const std::string& t) {
        for (int c : cats) if (t.find(k_cat_label[c]) == std::string::npos) return false;
        return true;
    };
    std::string tabs;
    if (!m.str("categories.listArray.toString", tabs)) return "the tab list did not answer";
    double n_opt = 0;
    if (!m.num("optionArrays.push", 0, n_opt) || n_opt < 1) return "optionArrays did not answer";
    /* Refresh just locked the last tab clip (MOVE, or one of our tabs when they are already there):
     * note the lock, take it off, and put it back on MOVE once our tabs sit before it */
    gfx_arg_num(m.arg(0), n_opt - 1);
    const int locked = m.boolean("categories.CheckLocked", 1);
    gfx_arg_num(m.arg(0), 0);
    m.call("LockPSMoveRelatedOptions", 1);
    auto relock = [&](const char* why) {
        if (locked > 0) { gfx_arg_num(m.arg(0), 1); m.call("LockPSMoveRelatedOptions", 1); }
        return why;
    };
    const bool had = has_all(tabs);
    const uint32_t entry = m.slot(0), list = m.slot(1), copy = m.slot(2), tmp = m.slot(3);
    if (!had) {
        if (tabs.find(k_cat_label[cats[0]]) != std::string::npos) return relock("only some of the PC tabs are there");
        /* splice(-1, 0, [label, 1]) per category: each goes in before MOVE, so they keep their order */
        for (int c : cats) {
            gfx_new_array(ctx, entry);
            if ((vm_read32(entry + 4) & 0x8F) != 7) { gfx_release(ctx, entry); return relock("no GFx array"); }
            gfx_push_str(ctx, entry, tmp, k_cat_label[c]);
            gfx_push_num(ctx, entry, tmp, 1.0);
            gfx_arg_num(m.arg(0), -1); gfx_arg_num(m.arg(1), 0); gfx_copy(m.arg(2), entry);
            m.call("categories.listArray.splice", 3);
            gfx_release(ctx, entry);
        }
        /* List.Refresh(newArray) re-reads the length and places a clip for each new tab */
        const bool got = m.array("categories.listArray.concat", 0, copy);
        if (got) { gfx_copy(m.arg(0), copy); m.call("categories.Refresh", 1); gfx_release(ctx, copy); }
        std::string now;
        if (!got || !m.str("categories.listArray.toString", now) || !has_all(now))
            return relock("the tabs could not be added");
        tabs = now;
    }
    int n_rows = 0;
    for (int c : cats) {
        gfx_new_array(ctx, list);
        if ((vm_read32(list + 4) & 0x8F) != 7) { gfx_release(ctx, list); return relock("no GFx array"); }
        n_rows += push_cat_rows(ctx, list, m.scratch(), rows, c);
        gfx_arg_num(m.arg(0), -1); gfx_arg_num(m.arg(1), 0); gfx_copy(m.arg(2), list);
        m.call("optionArrays.splice", 3);
        gfx_release(ctx, list);
    }
    double n_now = 0;
    if (!m.num("optionArrays.push", 0, n_now) || (int)n_now != (int)n_opt + ncat) return relock("the rows could not be added");
    if (locked > 0) { gfx_arg_num(m.arg(0), 1); m.call("LockPSMoveRelatedOptions", 1); }
    m.call("SetOptionsList", 0);                     /* shows optionArrays[categories._I] again */
    /* on one of our tabs, the game's own SetOptionsList showed a shorter list (or none) and clamped the row */
    if (had && p.sel >= 0) { gfx_arg_num(m.arg(0), p.sel); m.call("options.Set", 1); }
    static int s_logged = 0;
    if (s_logged < 3) {
        s_logged++;
        fprintf(stderr, "[menurows] Options: %s %d PC tabs, %d rows; tabs: %s\n", had ? "refreshed" : "added", ncat, n_rows,
                tabs.c_str());
    }
    return nullptr;
}

uint16_t rd16(uint32_t a) { return (uint16_t)((vm_base[a] << 8) | vm_base[a + 1]); }

} // namespace

/* func_0032D788 (GFx Invoke) entry: r3 menu, r4 method name, r5 GFxValue args, r6 count, lr the caller.
 * Returns the menu when r3_menu_rows_after must run after the call. */
extern "C" uint32_t r3_menu_rows_append(ppu_context* ctx)
{
    if (!vm_base || t_nest || (uint32_t)ctx->lr != kRetRefresh || (uint32_t)ctx->gpr[6] != 1 || !enabled()) return 0;
    const uint32_t menu = (uint32_t)ctx->gpr[3], name = (uint32_t)ctx->gpr[4], outer = (uint32_t)ctx->gpr[5];
    if (!name || !outer || name >= 0xF0000000u || outer >= 0xF0000000u) return 0;
    if (memcmp(vm_base + name, "Refresh", 8)) return 0;
    if ((vm_read32(outer + 4) & 0x8F) != 7) return 0;
    std::lock_guard<std::mutex> lk(s_mu);
    const std::vector<Row> rows = resolve_rows();
    const double f1 = ctx->fpr[1];
    Frame fr(ctx);

    if (menu_mode() == 1 && !s_fallback) {
        const char* why = nullptr;
        if (!menu || menu >= 0xF0000000u) why = "no menu object";
        else if (rd16(menu + 0x40) != 3) why = "the menu is still loading (its calls are queued)";
        else {
            MenuCalls m(ctx, menu, f1, fr.sp);
            std::string tabs;
            if (!m.str("categories.listArray.toString", tabs)) why = "the tab list did not answer";
            else {
                Pending p; p.menu = menu; p.name = name; p.outer = outer; p.f1 = f1;
                const std::vector<int> cats = present_cats(rows);
                if (!cats.empty() && tabs.find(k_cat_label[cats[0]]) != std::string::npos) {
                    /* on one of our tabs (they sit right before MOVE, the last one): keep its row */
                    const int ncat = (int)cats.size();
                    double cat = -1, ntab = 0, sel = -1;
                    if (m.num("categories._I.valueOf", 0, cat) && m.num("categories.listArray.push", 0, ntab) &&
                        (int)cat >= (int)ntab - 1 - ncat && (int)cat <= (int)ntab - 2 &&
                        m.num("options._I.valueOf", 0, sel) && sel >= 0)
                        p.sel = (int)sel;
                }
                t_pend = p;
                return menu;
            }
        }
        static int s_why = 0;
        if (s_why < 3) {
            s_why++;
            fprintf(stderr, "[menurows] PC tabs: %s; this time the PC rows go to the end of CONTROLS\n", why);
        }
    }
    if (rows.empty()) return 0;
    const int n = push_all_rows(ctx, outer, fr.sp + 0x200, rows);
    static int s_logged = 0;
    if (s_logged < 3) {
        s_logged++;
        fprintf(stderr, "[menurows] Options list: appended %d PC rows (section headers + settings) at the end of the CONTROLS tab (R3_MENU_ROWS=%d)\n",
                n, menu_mode());
    }
    return 0;
}

/* after func_0032D788 returned from that Refresh call (the game's AS has rebuilt the tabs' lists) */
extern "C" void r3_menu_rows_after(ppu_context* ctx, uint32_t menu)
{
    if (!vm_base || !menu || t_nest || t_pend.menu != menu) return;
    const Pending p = t_pend;
    t_pend = Pending{};
    std::lock_guard<std::mutex> lk(s_mu);
    const std::vector<Row> rows = resolve_rows();
    Frame fr(ctx);
    MenuCalls m(ctx, menu, p.f1, fr.sp);
    const char* why = add_tab(ctx, m, rows, p);
    if (!why) return;
    s_fallback = true;
    fprintf(stderr, "[menurows] PC tabs: %s; from now on the PC rows go to the end of the CONTROLS tab\n", why);
    push_all_rows(ctx, p.outer, m.scratch(), rows);
    gfx_copy(m.arg(0), p.outer);                     /* the game's list (still alive: its caller frees it) */
    m.invoke_ea(p.name, 1, 0);                       /* the game's Refresh again, now with the rows */
}

/* func_00383E08 (options ExternalInterface handler) entry: r5 method hash, r6 GFxValue args, r7 count.
 * Returns 1 for our ids; the game's handler still runs and, the id being unknown to it, re-sends the list. */
extern "C" int r3_menu_rows_change(ppu_context* ctx)
{
    if (!vm_base || !enabled() || (uint32_t)ctx->gpr[7] < 2) return 0;
    if ((uint32_t)ctx->gpr[5] != vm_read32(kHashChangeOption)) return 0;
    const uint32_t a = (uint32_t)ctx->gpr[6];
    if (!a || a >= 0xF0000000u) return 0;
    if ((vm_read32(a + 4) & 0x8F) != 3 || (vm_read32(a + 20) & 0x8F) != 3) return 0;
    double id, val; uint64_t b;
    b = vm_read64(a + 8); memcpy(&id, &b, 8);
    b = vm_read64(a + 24); memcpy(&val, &b, 8);
    const int iid = (int)id;
    if (iid >= kHeaderBase && iid < kHeaderBase + kNCats) return 1;   /* a section header */
    if (iid < kIdBase || iid >= kIdBase + kNRows) return 0;
    std::lock_guard<std::mutex> lk(s_mu);
    const std::vector<Row> rows = resolve_rows();
    for (const Row& w : rows) {
        if (kIdBase + w.def != iid) continue;
        const int k = (int)lround(val);
        const int n = row_nch(w);
        if (k < 0 || k >= n) return 1;
        if (k != row_sel(w)) {
            row_set(w, k);
            fprintf(stderr, "[menurows] %s = %s (from the game's Options menu)\n", k_rows[w.def].key, row_choice(w, k).c_str());
        }
        return 1;
    }
    return 1;
}
