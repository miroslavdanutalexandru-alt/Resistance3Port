/* Resistance 3 PC port: optional single-player cheats (God mode, Infinite ammo,
 * Infinite grenades), as in the R1 / R2 ports. All off by default; they only touch
 * heroes driven by a local controller / the keyboard (r3_hero_input, stubs.cpp).
 *
 * Plain guest-memory reads and writes, no guest calls and no hook of its own: it runs
 * from r3_mouselook_pre (stubs.cpp), which the hero update func_0017BCE8 calls every
 * gameplay frame for each hero (hook at 0x0017C670 in tools/hooks/r3.json, already
 * there for mouse look / aim assist), on the game's main thread. Every pointer is
 * checked before use; when something is not where it should be nothing is written
 * and it is logged once. Addresses are for BCUS98176 1.00 (the EBOOT the port uses).
 *
 *   God mode           the hero's health component (vtable 0x00D58FE8, owner moby
 *                      handle at +0x4; health float +0xF8, maximum +0xF4, clamped by
 *                      func_00641AE8; the HUD's SetHealth func_0064F6E8 is its vtable
 *                      slot +0x3C) is made invulnerable: byte +0x101, the flag the
 *                      game's own setter func_00641CC4 writes and the damage handler
 *                      func_006409B0 checks first (non-zero: the damage is zeroed;
 *                      kept at 2 so the game's own writes stay visible, see below).
 *                      Health is also kept at the maximum. The component is looked up
 *                      in the component-type table [0x0101DCD8] (entries of 0x14 bytes:
 *                      array, count; health is type 26) and cached.
 *   Infinite ammo      for every weapon the hero owns (moby table 0x012CA980: [+4]
 *                      array of 0x180-byte mobys, [+0x20] count; an owned moby has its
 *                      owner's handle at +0x58, its object at +0x40; weapons are class
 *                      0x11B or derived, parent table 0x01224980) the clip +0x1B8,
 *                      reserve +0x1BA and alt ammo +0x1BC (int16; func_0057064C /
 *                      func_00570758 / func_00570EC8) are not allowed to drop below the
 *                      highest value seen for that weapon.
 *   Infinite grenades  the same for the grenade counts, bytes hero+0x9EC + type for
 *                      the 6 grenade types (func_001580E8).
 *
 * Turned on by the launcher / F9 menu rows (PORT_CHEAT_GODMODE, PORT_CHEAT_AMMO,
 * PORT_CHEAT_GRENADES = 1) or, when PORT_CHEAT_KEYS=1, Ctrl+F1 / F2 (r3_pcextra.cpp).
 * Once any has been on, trophy unlocks are not recorded until the game is restarted
 * (g_trophy_unlocks_blocked, sceNpTrophy.c). In an R3_BENCH run and with PORT_ONLINE
 * on they all stay off. Every change is logged.
 *
 * Also here: "Unlock all chapters and extras" (R3_UNLOCK_ALL), see below. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "ppu_recomp.h"

extern "C" volatile int g_trophy_unlocks_blocked;   /* sceNpTrophy.c */

extern "C" volatile int g_r3_cheat_god = 0;
extern "C" volatile int g_r3_cheat_ammo = 0;
extern "C" volatile int g_r3_cheat_gren = 0;

static int env_on(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0; }
/* R3_BENCH runs stay comparable and PORT_ONLINE (RPCN, port_online.c) is never
 * cheated in: no cheats at all (env vars, menu rows and keys are ignored). */
static int cheats_blocked(void)
{
    static int why = -1;
    if (why < 0) why = env_on("R3_BENCH") ? 1 : env_on("PORT_ONLINE") ? 2 : 0;
    return why;
}
/* Once a cheat has been on, trophies stay off until the game is restarted. */
static void block_trophies(void)
{
    if (g_trophy_unlocks_blocked) return;
    g_trophy_unlocks_blocked = 1;
    fprintf(stderr, "[cheat] trophies are off until the game is restarted\n");
}
static const char* const s_name[3] = { "god mode", "infinite ammo", "infinite grenades" };
static volatile int& cheat_flag(int which) { return which == 0 ? g_r3_cheat_god : which == 1 ? g_r3_cheat_ammo : g_r3_cheat_gren; }
/* Called before the F9 menu reads the rows (r3_pcextra.cpp opts_init). */
extern "C" void r3_cheats_init_env(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    const int why = cheats_blocked();
    if (why && (env_on("PORT_CHEAT_GODMODE") || env_on("PORT_CHEAT_AMMO") || env_on("PORT_CHEAT_GRENADES")))
        fprintf(stderr, "[cheat] %s: cheats are off\n", why == 1 ? "benchmark run" : "online (PORT_ONLINE)");
}
/* 0 = God mode, 1 = Infinite ammo, 2 = Infinite grenades; on: 1 / 0, -1 = toggle.
 * Returns the new state, -1 in a benchmark run, -2 with online on. Any thread. */
extern "C" int r3_cheat_set(int which, int on)
{
    if (which < 0 || which > 2) return 0;
    r3_cheats_init_env();
    if (const int why = cheats_blocked()) return -why;
    volatile int& v = cheat_flag(which);
    if (on < 0) on = !v;
    on = on ? 1 : 0;
    if (on) block_trophies();
    if (v != on) fprintf(stderr, "[cheat] %s %s\n", s_name[which], on ? "on" : "off");
    v = on;
    return on;
}
extern "C" int r3_cheat_toggle(int which) { return r3_cheat_set(which, -1); }

/* ---- guest memory, checked ---- */
static const uint32_t MAIN_LO = 0x00010000u, MAIN_HI = 0x01600000u;   /* EBOOT image (.data / .bss) */
static const uint32_t HEAP_LO = 0x30000000u, HEAP_HI = 0x40000000u;   /* the game's heap (objects, mobys) */
static bool heap_ok(uint32_t a, uint32_t size) { return a >= HEAP_LO && a < HEAP_HI - size && !(a & 3u); }
static bool main_ok(uint32_t a) { return a >= MAIN_LO && a < MAIN_HI && !(a & 3u); }

static const uint32_t MOBY_TABLE = 0x012CA980u;   /* [+4] moby array, [+0x20] count; 0x180 bytes each */
static const uint32_t CLASS_PARENT = 0x01224980u; /* u32 parent class per class id (func_00476368) */
static const uint32_t COMP_TABLE = 0x0101DCD8u;   /* -> component-type table, 0x14 bytes per type */
static const uint32_t HEALTH_VT = 0x00D58FE8u;
static const uint32_t HEALTH_TYPE = 26u;
static const uint32_t CLS_WEAPON = 0x11Bu;

static void told(int* once, const char* fmt, uint32_t a, uint32_t b)
{
    if (*once) return;
    *once = 1;
    fprintf(stderr, fmt, a, b);
}
static bool isa(uint32_t cls, uint32_t base)
{
    for (int guard = 0; guard < 32 && cls < 0xBB8u; guard++) {
        if (cls == base) return true;
        cls = vm_read32(CLASS_PARENT + cls * 4u);
    }
    return false;
}
/* the hero's own moby and its handle ([moby+0x40] == hero, handle at [moby+0x44]) */
static bool hero_moby(uint32_t hero, uint32_t* moby, uint32_t* handle)
{
    const uint32_t m = vm_read32(hero + 0xC);
    if (!heap_ok(m, 0x180) || vm_read32(m + 0x40) != hero) return false;
    *moby = m;
    *handle = vm_read32(m + 0x44);
    return *handle != 0 && *handle != 0xFFFFFFFFu;
}

/* highest value seen per (weapon / hero, its config, field) */
struct Hwm { uint32_t obj, cfg; uint16_t field; int16_t hi; };
static Hwm s_hwm[512];
static int s_nhwm = 0;
static int16_t* hwm_slot(uint32_t obj, uint32_t cfg, uint16_t field, int16_t cur)
{
    for (int i = 0; i < s_nhwm; i++)
        if (s_hwm[i].obj == obj && s_hwm[i].cfg == cfg && s_hwm[i].field == field) return &s_hwm[i].hi;
    if (s_nhwm >= (int)(sizeof s_hwm / sizeof s_hwm[0])) s_nhwm = 0;   /* full: start over */
    s_hwm[s_nhwm] = { obj, cfg, field, cur };
    return &s_hwm[s_nhwm++].hi;
}
static void hold16(uint32_t obj, uint32_t cfg, uint16_t field)
{
    const int16_t cur = (int16_t)vm_read16(obj + field);
    if (cur < 0 || cur > 9999) return;
    int16_t* hi = hwm_slot(obj, cfg, field, cur);
    if (cur > *hi) *hi = cur;
    else if (cur < *hi) vm_write16(obj + field, (uint16_t)*hi);
}
static void hold8(uint32_t obj, uint16_t field)
{
    const int16_t cur = (int16_t)vm_read8(obj + field);
    if (cur > 99) return;
    int16_t* hi = hwm_slot(obj, 0xFFFFFFFFu, field, cur);
    if (cur > *hi) *hi = cur;
    else if (cur < *hi) vm_write8(obj + field, (uint8_t)*hi);
}

static int s_diag = 0;
static void ammo_hero(uint32_t hero, uint32_t handle)
{
    static int bad = 0, none = 0;
    const uint32_t arr = vm_read32(MOBY_TABLE + 4), cnt = vm_read32(MOBY_TABLE + 0x20);
    if (!heap_ok(arr, 0x180) || cnt == 0 || cnt > 0x2000 || arr + cnt * 0x180u > HEAP_HI) {
        told(&bad, "[cheat] infinite ammo: moby table not usable (array 0x%08X, count %u)\n", arr, cnt);
        return;
    }
    int n = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint32_t m = arr + i * 0x180u;
        if (vm_read32(m + 0x58) != handle) continue;
        const uint32_t w = vm_read32(m + 0x40);
        if (!heap_ok(w, 0x200) || w == hero || vm_read32(w + 0xC) != m) continue;
        if (!isa(vm_read32(w + 4), CLS_WEAPON)) continue;
        const uint32_t cfg = vm_read32(w + 0xC4);
        if (!main_ok(cfg)) continue;
        n++;
        if (s_diag)
            fprintf(stderr, "[cheat] hero 0x%08X weapon 0x%08X class 0x%X clip %d reserve %d alt %d\n", hero, w,
                    vm_read32(w + 4), (int16_t)vm_read16(w + 0x1B8), (int16_t)vm_read16(w + 0x1BA), (int16_t)vm_read16(w + 0x1BC));
        hold16(w, cfg, 0x1B8);
        hold16(w, cfg, 0x1BA);
        hold16(w, cfg, 0x1BC);
    }
    if (!n) told(&none, "[cheat] infinite ammo: no weapon found for hero 0x%08X (handle 0x%08X)\n", hero, handle);
}
static void grenades_hero(uint32_t hero)
{
    if (s_diag)
        fprintf(stderr, "[cheat] hero 0x%08X grenades %u %u %u %u %u %u\n", hero, vm_read8(hero + 0x9EC), vm_read8(hero + 0x9ED),
                vm_read8(hero + 0x9EE), vm_read8(hero + 0x9EF), vm_read8(hero + 0x9F0), vm_read8(hero + 0x9F1));
    for (uint16_t k = 0; k < 6; k++) hold8(hero, (uint16_t)(0x9EC + k));
}

/* ---- God mode ---- */
static bool health_comp_ok(uint32_t c, uint32_t handle)
{
    return heap_ok(c, 0x120) && vm_read32(c) == HEALTH_VT && vm_read32(c + 4) == handle;
}
static uint32_t find_in_type(uint32_t table, uint32_t type, uint32_t handle)
{
    const uint32_t arr = vm_read32(table + type * 0x14u), cnt = vm_read32(table + type * 0x14u + 4);
    if (!heap_ok(arr, 4) || cnt == 0 || cnt > 256) return 0;
    for (uint32_t j = 0; j < cnt; j++) {
        const uint32_t c = vm_read32(arr + j * 4u);
        if (health_comp_ok(c, handle)) return c;
    }
    return 0;
}
static uint32_t find_health(uint32_t handle)
{
    const uint32_t table = vm_read32(COMP_TABLE);
    if (!heap_ok(table, 0x14u * 128)) return 0;
    if (const uint32_t c = find_in_type(table, HEALTH_TYPE, handle)) return c;
    for (uint32_t t = 0; t < 128; t++)            /* another build of the type table: look at every type */
        if (t != HEALTH_TYPE) if (const uint32_t c = find_in_type(table, t, handle)) return c;
    return 0;
}
static float rdf(uint32_t a) { const uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(a, u); }

/* Byte +0x101 is shared with the game, which writes it itself (func_00641CC4 from about
 * 40 places, Lua: scripted sections, the hero's own state code at 0x00164A5C, ...) and
 * only ever writes 0 or 1; the game only tests it against 0 (func_006409B0, every caller
 * of the getter func_00641CBC). So while God mode is on the cheat keeps it at 2 (GOD_MARK):
 * still "not damageable" for the game, but any write by the game shows up (0 or 1 instead
 * of 2). That value is remembered as what the game wants (game_val) and the 2 is put back.
 * Turning God mode off writes game_val back, and only where the byte is still the cheat's 2
 * on the same component (address, vtable, owner handle), so a scripted invulnerability
 * that started while God mode was on stays on. A health component built anew (new level,
 * checkpoint) starts with the game's own 0 / 1, which is simply seen as a game write. */
static const uint8_t GOD_MARK = 2;
struct Inv { uint32_t comp, handle; uint8_t game_val; };
static Inv s_inv[4];                 /* one per hero slot (god_hero cache index) */
static void inv_release(int k, const char* why)
{
    Inv& o = s_inv[k];
    if (!o.comp) return;
    if (health_comp_ok(o.comp, o.handle) && vm_read8(o.comp + 0x101) == GOD_MARK) {
        vm_write8(o.comp + 0x101, o.game_val);
        fprintf(stderr, "[cheat] god mode %s: health 0x%08X back to the game's %s\n", why, o.comp,
                o.game_val ? "not damageable (scripted)" : "damageable");
    } else
        fprintf(stderr, "[cheat] god mode %s: health 0x%08X already the game's again\n", why, o.comp);
    o = {};
}
static void god_hero(uint32_t hero, uint32_t handle)
{
    static struct { uint32_t hero, handle, comp; int miss; } cache[4];
    static int nf = 0, badhp = 0;
    int k = 0;
    while (k < 4 && cache[k].hero != hero) k++;
    if (k == 4) { k = 0; for (int i = 1; i < 4; i++) if (!cache[i].hero) { k = i; break; } inv_release(k, "(other hero)"); cache[k] = { hero, handle, 0, 0 }; }
    if (cache[k].handle != handle) { inv_release(k, "(other hero)"); cache[k] = { hero, handle, 0, 0 }; }
    uint32_t c = cache[k].comp;
    if (!c || !health_comp_ok(c, handle)) {
        c = 0;
        inv_release(k, "(component gone)");
        if (cache[k].miss-- <= 0) {               /* not every frame while there is none */
            c = find_health(handle);
            cache[k].miss = c ? 0 : 30;
            if (c) fprintf(stderr, "[cheat] god mode: hero 0x%08X health 0x%08X (%.1f / %.1f)\n", hero, c,
                           (double)rdf(c + 0xF8), (double)rdf(c + 0xF4));
            else told(&nf, "[cheat] god mode: no health component for hero 0x%08X (handle 0x%08X)\n", hero, handle);
        }
        cache[k].comp = c;
        if (!c) return;
    }
    if (s_inv[k].comp && (s_inv[k].comp != c || s_inv[k].handle != handle)) inv_release(k, "(other component)");
    const uint8_t b = vm_read8(c + 0x101);
    if (!s_inv[k].comp) {
        /* first sight: a 2 here can only be left from an earlier record of this cheat */
        s_inv[k] = { c, handle, (uint8_t)(b == GOD_MARK ? 0 : b) };
        fprintf(stderr, "[cheat] god mode: health 0x%08X (hero 0x%08X) not damageable (game's flag %u)\n", c, hero, s_inv[k].game_val);
    } else if (b != GOD_MARK) {
        if (b != s_inv[k].game_val)
            fprintf(stderr, "[cheat] god mode: the game set its not-damageable flag of 0x%08X to %u (kept for when God mode is off)\n", c, b);
        s_inv[k].game_val = b;
    }
    if (b != GOD_MARK) vm_write8(c + 0x101, GOD_MARK);
    const float hp = rdf(c + 0xF8), mx = rdf(c + 0xF4);
    if (!std::isfinite(hp) || !std::isfinite(mx) || mx <= 0.0f || mx > 100000.0f) {
        told(&badhp, "[cheat] god mode: health values not usable (component 0x%08X, hero 0x%08X)\n", c, hero);
        return;
    }
    if (hp > 0.0f && hp < mx) wrf(c + 0xF8, mx);  /* 0 = already dead: left to the game */
}
static void god_restore(void)
{
    for (int k = 0; k < 4; k++) inv_release(k, "off");
}

/* ---- Unlock all chapters and extras (R3_UNLOCK_ALL, issue #10) ----
 * Nothing is written to the save: the game's own queries answer "unlocked" while it is on.
 *   Extras    func_00438230 IsUnlocked(manager, category, id) (bit id of stat 0xE4 in the
 *             stats DB; categories 0 cheats, 1 skins, 2 concept art, 3 titles, 4 videos)
 *             returns 1 for categories 0, 1, 2 and 4. Titles (3, stats 0xE5-0xE8) are left
 *             alone, and so is the store's points total (func_004383EC, call at 0x0043849C).
 *             Picking an item that reports unlocked makes the game's Purchase
 *             (func_00438688) toggle it instead of buying it, so the game's cheats (Bloody
 *             Hell included; the game itself still hides it on a low-violence system) turn
 *             on and off exactly as with a real unlock.
 *   Chapters  the profile's campaign block (profile + 0x2BE4: u16 location mask at +0x88,
 *             11 x 64 chapter bytes at +0x8A, 0xFF = not reached, else the highest
 *             difficulty reached) as read by the Load Level menu (SetCheckpointData,
 *             func_0034A558), the Load Level entry (func_0034AE78, func_00357DE0): every
 *             location is unlocked and an unreached chapter reads as reached on the first
 *             difficulty. The game's progress code, hasCompletedCampaign and the trophy
 *             checks still read the real bytes.
 * Trophies: once the option has been on, turning one of the game's Extras cheats on
 * (cheat object table 0x011FDDC0, enabled byte +4) stops trophy unlocks until the game is
 * restarted, as with the port's own cheats. Off in an R3_BENCH run and with PORT_ONLINE. */
extern "C" volatile int g_r3_unlock_all = 0;
static int s_unlock_init = 0, s_unlock_used = 0;
static void unlock_init(void)
{
    if (s_unlock_init) return;
    s_unlock_init = 1;
    if (!env_on("R3_UNLOCK_ALL")) return;
    if (const int why = cheats_blocked()) {
        fprintf(stderr, "[unlock] %s: unlock all chapters and extras is off\n", why == 1 ? "benchmark run" : "online (PORT_ONLINE)");
        return;
    }
    g_r3_unlock_all = s_unlock_used = 1;
    fprintf(stderr, "[unlock] all chapters and extras unlocked (R3_UNLOCK_ALL, nothing is written to the save)\n");
}
/* on: 1 / 0. Returns the new state, -1 in a benchmark run, -2 with online on (F9 row). */
extern "C" int r3_unlock_all_set(int on)
{
    unlock_init();
    if (const int why = cheats_blocked()) return -why;
    on = on ? 1 : 0;
    if (g_r3_unlock_all != on)
        fprintf(stderr, "[unlock] all chapters and extras %s\n", on ? "unlocked (nothing is written to the save)" : "back to the save's own unlocks");
    g_r3_unlock_all = on;
    if (on) s_unlock_used = 1;
    return on;
}
static inline int unlock_on(void)
{
    if (!s_unlock_init) unlock_init();
    return g_r3_unlock_all;
}
/* func_00438230 hook: 1 = report the item unlocked. lr = the caller's return address. */
extern "C" int r3_unlock_extra(uint32_t lr, uint32_t category)
{
    if (!unlock_on() || category > 4 || category == 3 || lr == 0x004384A0u) return 0;
    static int told_cat[5];
    if (!told_cat[category]) {
        told_cat[category] = 1;
        static const char* const nm[5] = { "cheats", "skins", "concept art", "titles", "videos" };
        fprintf(stderr, "[unlock] Extras %s report unlocked\n", nm[category]);
    }
    return 1;
}
/* func_0034A558 / func_0034AE78 / func_00357DE0 hooks: the location mask and chapter bytes */
extern "C" uint32_t r3_unlock_locmask(uint32_t mask) { return unlock_on() ? (mask | 0x7FFu) : mask; }
extern "C" uint32_t r3_unlock_chapter(uint32_t v) { return unlock_on() && (v & 0xFFu) == 0xFFu ? 0u : v; }
static const uint32_t GAME_CHEATS = 0x011FDDC0u;   /* category 0: 8 cheat objects, byte +4 = on */
static void unlock_watch(void)
{
    if (!s_unlock_used || g_trophy_unlocks_blocked) return;
    for (uint32_t id = 0; id < 8; id++) {
        const uint32_t o = vm_read32(GAME_CHEATS + id * 4u);
        if (!main_ok(o) && !heap_ok(o, 8)) continue;
        if (vm_read8(o + 4)) {
            fprintf(stderr, "[unlock] the game's Extras cheat %u is on\n", id);
            block_trophies();
            return;
        }
    }
}

/* Every gameplay frame, per hero (stubs.cpp r3_mouselook_pre); local = the hero has a
 * local controller / keyboard (r3_hero_input). */
extern "C" void r3_cheats_hero(uint32_t hero, int local)
{
    static int init = 0;
    if (!init) {
        init = 1;
        r3_cheats_init_env();
        if (!cheats_blocked()) {
            if (env_on("PORT_CHEAT_GODMODE")) r3_cheat_set(0, 1);
            if (env_on("PORT_CHEAT_AMMO")) r3_cheat_set(1, 1);
            if (env_on("PORT_CHEAT_GRENADES")) r3_cheat_set(2, 1);
            if (g_r3_cheat_god || g_r3_cheat_ammo || g_r3_cheat_gren)
                fprintf(stderr, "[cheat] at start: god %d, ammo %d, grenades %d\n", g_r3_cheat_god, g_r3_cheat_ammo, g_r3_cheat_gren);
        }
    }
    if (cheats_blocked()) return;
    unlock_watch();
    static int hold_was = 0, god_was = 0;
    const int god = g_r3_cheat_god, ammo = g_r3_cheat_ammo, gren = g_r3_cheat_gren;
    if (god || ammo || gren) block_trophies();
    const int hold = ammo || gren;
    if (hold && !hold_was) { s_nhwm = 0; s_diag = 1; }   /* fresh start every time it is turned on */
    hold_was = hold;
    if (god_was && !god) god_restore();
    god_was = god;
    if ((!god && !hold) || !local || !heap_ok(hero, 0x2000)) return;
    static int nomoby = 0;
    uint32_t moby = 0, handle = 0;
    if (!hero_moby(hero, &moby, &handle) || !isa(vm_read32(hero + 4), 0xF1u)) {
        told(&nomoby, "[cheat] hero 0x%08X: own moby / class not as expected (moby 0x%08X)\n", hero, moby);
        return;
    }
    static uint32_t heroes[4]; static int nh = 0;
    { int i = 0; while (i < nh && heroes[i] != hero) i++;
      if (i == nh) { if (nh == 4) nh = 0; heroes[nh++] = hero; fprintf(stderr, "[cheat] hero 0x%08X (moby 0x%08X, handle 0x%08X)\n", hero, moby, handle); s_diag = 1; } }
    if (god) god_hero(hero, handle);
    if (ammo) ammo_hero(hero, handle);
    if (gren) grenades_hero(hero);
    s_diag = 0;
}
