/* Developer fast boot for automated test runs (opt-in, default off; tools\fastboot.ps1 sets these):
 *   PORT_FAST_BOOT=1            skip the start-up logo / intro movies (SKIP_INTRO=1).
 *   PORT_FAST_BOOT_CUTSCENES=1  also skip story movies, through the port's own cutscene skip (r3_pcextra.cpp).
 *   PORT_BOOT_LEVEL=<where>     start in that level. The retail game ignores its "-level" option, so the port
 *                               drives the menus with injected pad presses (r3_pcextra.cpp fb_drive_tick):
 *                               install notice, Press START, Campaign, Load Level, the location on the map,
 *                               the checkpoint, Normal. <where> is a level name from the table below, or
 *                               <location>[:<checkpoint>] with location 1-5 (Haven, St. Louis, Mt. Pleasant,
 *                               Graterford, New York) or its name, checkpoint 1-based in the Load Level list.
 *                               Set R3_UNLOCK_ALL=1 so every location is selectable (nothing is saved).
 * Once the player is in control, "[fastboot] gameplay ..." is printed (r3_pcextra.cpp). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>

static const char* const port_fastboot_cities[] = { "haven", "stlouis", "mtpleasant", "graterford", "newyork" };
/* level name -> Load Level entry (checked on screen) */
static const struct { const char* level; int city, cp; const char* title; } port_fastboot_levels[] = {
    { "haven_town_ridge", 1, 4, "Haven, OK - Run!" },
};

static inline int port_fastboot_env_on(const char* name)
{
    const char* e = getenv(name);
    return e && *e && *e != '0';
}

/* PORT_BOOT_LEVEL -> location 1-5 and checkpoint 1.. (0 = none) */
static inline int port_fastboot_target(int* city, int* cp)
{
    *city = *cp = 0;
    const char* e = getenv("PORT_BOOT_LEVEL");
    if (!e || !*e) return 0;
    std::string v(e);
    const size_t s = v.find_last_of("/\\");                /* "levels/haven_town_ridge" works too */
    if (s != std::string::npos) v = v.substr(s + 1);
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    for (auto& l : port_fastboot_levels)
        if (v == l.level) { *city = l.city; *cp = l.cp; return 1; }
    std::string a = v, b = "1";
    const size_t k = v.find(':');
    if (k != std::string::npos) { a = v.substr(0, k); b = v.substr(k + 1); }
    std::string an; for (char c : a) if (isalnum((unsigned char)c)) an += c;
    int n = atoi(an.c_str());
    for (int i = 0; i < 5; i++) if (an == port_fastboot_cities[i]) n = i + 1;
    const int m = atoi(b.c_str());
    if (n < 1 || n > 5 || m < 1) {
        fprintf(stderr, "[fastboot] PORT_BOOT_LEVEL=%s: unknown (a level name, or <location 1-5|name>[:<checkpoint>])\n", e);
        return 0;
    }
    *city = n; *cp = m;
    return 1;
}

static inline void port_fastboot_putenv(const char* k, const char* v)
{
#ifdef _WIN32
    std::string s = std::string(k) + "=" + v;
    _putenv(s.c_str());
#else
    setenv(k, v, 1);
#endif
}

/* main(), before the guest starts */
static inline void port_fastboot_init(void)
{
    if (port_fastboot_env_on("PORT_FAST_BOOT")) {
        port_fastboot_putenv("SKIP_INTRO", "1");
        fprintf(stderr, "[fastboot] PORT_FAST_BOOT=1: intro movies skipped%s\n",
                port_fastboot_env_on("PORT_FAST_BOOT_CUTSCENES") ? ", cutscenes skipped (PORT_FAST_BOOT_CUTSCENES=1)" : "");
    }
    int city, cp;
    if (port_fastboot_target(&city, &cp))
        fprintf(stderr, "[fastboot] PORT_BOOT_LEVEL: the menus are driven to Load Level, %s, checkpoint %d\n",
                port_fastboot_cities[city - 1], cp);
}
