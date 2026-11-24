// launcher_extra.inc -- PC extras for the launcher (included by launcher.cpp):
//   * the options behind the game's "More PC Options" menu (r2_pcextra.cpp), so the in-game menu and the
//     launcher write the same settings.ini keys / settings.bat values,
//   * controller button layout (PAD_MAP_*, PAD_SOUTHPAW; libs/input/cellPad.c XInput hook),
//   * the "Saves & tools" page: save list, back up / restore / open, automatic backup before Play
//     (keeps the newest N), import from RPCS3, the mods list (mods\order.txt) and the built-in benchmark
//     (R2_BENCH=1),
//   * the save location (PS3_GAMEDATA_ROOT: game folder, Saved Games or a folder you pick).
// Needs from launcher.cpp: add_option, g_set, path_of, toast, add, deco, SF, layout_section, play, g_hw, g_dir.
#ifndef LX_ENV
#define LX_ENV(x) "R2_" x
#define LX_TAG "R2"
#endif
#ifndef LX_BG_ENV
#define LX_BG_ENV "R2_BACKGROUND"       // the launcher's "When the game is in the background" variable
#endif
#ifndef LX_TITLE_IDS                    // every disc / region of the game: RPCS3 save import
#define LX_TITLE_IDS "BCUS98120", "BCES00226", "BCJS30059", "BCAS20055", "BCKS10114"   // Resistance 2
#endif
#ifndef LX_USER_DIR
#define LX_USER_DIR "Resistance 2"      // Saved Games\<this> (save location)
#endif
#include "../ps3recomp/include/ps3emu/port_pc_extras.h"
#ifndef LX_HAS_FX
#define LX_HAS_FX 1                     // the game's post-effect toggles (r2_pcextra.cpp s_fx)
#endif

// Frame Generation: keep only the multipliers the detected GPU can run (RTX 40: 2x; RTX 50: up to 4x; other GPUs:
// FSR 3 2x). The game checks again at start (Streamline / FidelityFX) and logs why it is off.
static void lx_framegen_filter()
{
    for (auto& s : g_set) {
        if (strcmp(s.key, "dlss_fg") != 0) continue;
        int series = 0;
        const char* g = g_hwi.gpu;
        if (g_hwi.vendor == 0x10DE) {
            for (const char* p = strstr(g, "RTX "); p; p = strstr(p + 4, "RTX ")) {
                const char* d = p + 4;
                if ((d[0] == '4' || d[0] == '5') && d[1] >= '0' && d[1] <= '9' && d[2] >= '0' && d[2] <= '9' && d[3] >= '0' && d[3] <= '9')
                    series = d[0] - '0';
            }
            if (strstr(g, "Blackwell")) series = 5;
            else if (strstr(g, "Ada") && !series) series = 4;
        }
        const int max_mult = series == 5 ? 4 : series == 4 ? 2 : 1;
        std::vector<Choice> keep;
        for (const auto& c : s.choices) {
            const bool fsr = strcmp(c.value, "fsr3") == 0;
            if (!*c.value || fsr || atoi(c.value) <= max_mult) keep.push_back(c);  // FSR 3 2x: any GPU
        }
        s.choices = keep;
        if (s.def >= (int)s.choices.size()) s.def = 0;
        if (s.sel >= (int)s.choices.size()) s.sel = 0;
    }
}

static void register_extra_options()
{
    // Display / Upscaling / Interface / Advanced pages (launcher_cats.inc)
    add_option(SEC_X_DISPLAY, "resizable", "Resizable window",
               "Windowed mode: drag the window edges to resize it; the picture follows the new size.",
               "RSX_RESIZABLE", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "altenter", "Alt+Enter",
               "Alt+Enter switches between windowed and borderless fullscreen in game.",
               "RSX_ALT_ENTER", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "hdr", "HDR output",
               "Experimental. Needs Windows HDR on (Settings > Display > Use HDR). Draws the final image into an HDR10 "
               "(PQ) or scRGB swapchain: the game's SDR image stays at the paper-white brightness below and only "
               "highlights are expanded towards the peak. Auto turns HDR on only when Windows HDR is on for the game's "
               "display. Falls back to SDR otherwise (both renderers). The performance overlay and screenshots stay SDR. Pros: brighter "
               "highlights and deeper contrast. Cons: needs a real HDR display; wrong brightness settings look washed "
               "out or clip highlights.",
               LX_ENV("HDR"), { {"Off", ""}, {"Auto", "auto"}, {"On  (HDR10)", "1"}, {"On  (scRGB)", "scrgb"} }, 0, {}, "rpcs3|d3d12");
    add_option(SEC_X_DISPLAY, "hdr_paper", "HDR paper white",
               "How bright the HUD, menus and the game's normal image are in HDR, in nits (203 is the HDR reference "
               "white). Lower for a dark room, higher for a bright one.",
               LX_ENV("HDR_PAPER"), { {"203 nits", ""}, {"100 nits", "100"}, {"160 nits", "160"}, {"250 nits", "250"},
                                      {"300 nits", "300"}, {"400 nits", "400"} }, 0, {}, "rpcs3|d3d12");
    add_option(SEC_X_DISPLAY, "hdr_peak", "HDR peak brightness",
               "How bright the brightest highlights get, in nits (also sent to the display as HDR metadata). Display = "
               "the peak Windows reports for the monitor. Set it equal to paper white to turn highlight expansion off.",
               LX_ENV("HDR_PEAK"), { {"Display", ""}, {"400 nits", "400"}, {"600 nits", "600"}, {"800 nits", "800"},
                                     {"1000 nits", "1000"}, {"1500 nits", "1500"}, {"2000 nits", "2000"} }, 0, {}, "rpcs3|d3d12");
    add_option(SEC_X_DISPLAY, "hdr_sat", "HDR saturation",
               "Colour saturation of the HDR picture (1.0 = as the game).",
               LX_ENV("HDR_SAT"), { {"1.0", ""}, {"0.9", "0.9"}, {"1.1", "1.1"}, {"1.2", "1.2"} }, 0, {}, "rpcs3|d3d12");
    add_option(SEC_X_DISPLAY, "reflex", "NVIDIA Reflex",
               "Experimental. Lower input lag on GeForce cards (Vulkan VK_NV_low_latency2 / Direct3D 12 NVAPI, driver 535 or newer): the "
               "game waits for the GPU before it starts a frame. It is not a frame limiter - the Frame rate setting "
               "still decides the cap. Does nothing on other GPUs.",
               LX_ENV("REFLEX"), { {"Off", ""}, {"On", "1"}, {"On + Boost", "2"} }, 0, {}, "rpcs3|d3d12");
    add_option(SEC_X_DISPLAY, "latency", "Latency reduction",
               "Which low-latency technology to use. NVIDIA Reflex: GeForce cards (Vulkan VK_NV_low_latency2, driver "
               "535 or newer), with latency markers; the NVIDIA Reflex row above picks On or On + Boost. AMD Anti-Lag 2: "
               "Radeon RX 5000 or newer with a recent AMD driver (Vulkan VK_AMD_anti_lag / Direct3D 12 driver extension). Auto: Reflex on NVIDIA, "
               "Anti-Lag 2 on AMD, nothing elsewhere. Default: only the NVIDIA Reflex row decides (as before). Neither is "
               "a frame limiter, and both stay off while frame generation is on (it has its own). Takes effect on restart.",
               "PORT_LATENCY", { {"Default  (Reflex row)", ""}, {"Off", "off"}, {"NVIDIA Reflex", "reflex"},
                                 {"AMD Anti-Lag 2", "antilag"}, {"Auto", "auto"} }, 0, {}, "rpcs3|d3d12");
    add_option(SEC_X_DISPLAY, "dlss_fg", "Frame Generation",
               "NVIDIA DLSS Frame Generation: the GPU draws extra frames between the game's own, so motion looks "
               "smoother when the CPU is the limit (big fights). 2x shows two frames per game frame, 3x / 4x "
               "(Multi Frame Generation) three or four - 4x is for 165 Hz+ displays. Adds a little input lag "
               "(Reflex is turned on with it) and needs a GeForce RTX 40 or 50 card, Anti-aliasing set to NVIDIA "
               "DLSS and SDR or HDR10 output. The HUD is kept out of the generated frames. Turns itself off in menus, "
               "movies, loading screens and split-screen. Only the multipliers your GPU supports are listed. "
               "FSR 3 2x (any GPU): AMD FidelityFX frame interpolation, two frames per game frame; needs Anti-"
               "aliasing set to FSR, DLSS or XeSS and SDR or HDR10 output; no Reflex, so it adds a bit more lag. "
               "Frame generation backend (below) picks the backend by hand; PORT_FG=dlssg|fsr3|off overrides both.",
               LX_ENV("DLSS_FG"), { {"Off", ""}, {"2x", "2"}, {"3x", "3"}, {"4x", "4"}, {"FSR 3 2x", "fsr3"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "fg_backend", "Frame generation backend",
               "Which frame generation runs when Frame Generation is a DLSS choice. Auto: DLSS Frame Generation, or "
               "AMD FSR 3 2x when DLSS Frame Generation can't start (no RTX 40/50, old driver). DLSS only: no fallback - "
               "frame generation stays off if DLSS Frame Generation can't start. FSR 3: always AMD FSR 3 2x (any GPU). "
               "Takes effect at the next start.",
               LX_ENV("FG_BACKEND"), { {"Auto", ""}, {"DLSS Frame Generation only", "dlss"}, {"AMD FSR 3", "fsr3"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "discord", "Discord Rich Presence",
               "Shows \"Playing Resistance\" and the level in your Discord status. Needs the Discord app running and "
               "an application id in discord_appid.txt (or the " LX_ENV("DISCORD_APPID") " variable); does nothing otherwise.",
               LX_ENV("DISCORD"), { {"Off", ""}, {"On", "1"} }, 0);
    add_option(SEC_X_DISPLAY, "photo", "Photo mode  (F7)",
               "F7 in game pauses, hides the port's overlays and lets you take clean F12 screenshots. "
               "No free camera (see the README).",
               LX_ENV("PHOTO"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "cutscene_skip", "Hold to skip movies",
               "Hold Space / Enter / A / Menu for a second to skip a pre-rendered movie (the game's own skip).",
               LX_ENV("CUTSCENE_SKIP"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "cutscene_skip_all", "Skip unwatched movies",
               "With Hold to skip movies on: the hold also skips movies you have not watched before (the game "
               "itself only lets you skip a movie once it has been seen). Off: only movies already seen.",
               LX_ENV("CUTSCENE_SKIP_ALL"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "savebackup", "Back up saves before playing",
               "Play copies your saved games to save_backups\\ first (only when they changed), keeping the newest "
               "ones. See the Saves & tools page.",
               LX_ENV("SAVE_BACKUP"), { {"On  (keep 10)", ""}, {"On  (keep 5)", "5"}, {"On  (keep 30)", "30"}, {"Off", "0"} }, 0);
    // PS3_GAMEDATA_ROOT (cellSaveData.c, sceNpTrophy.c): where saves and trophies live. The launcher copies the
    // saves over on the next Play when the new place has none (lx_save_migrate); the old ones stay.
    {
        std::vector<Choice> ch = { {"Game folder  (gamedata\\)", ""} };
        const std::string sg = lt_saved_games();
        if (!sg.empty()) ch.push_back({ "Saved Games\\" LX_USER_DIR, _strdup((sg + "\\" LX_USER_DIR).c_str()) });
        if (FILE* f = fopen(path_of("settings.ini").c_str(), "r")) {   // a folder picked before (Choose folder...)
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (strncmp(line, "savedir=", 8) || !line[8]) continue;
                bool known = false;
                for (auto& c : ch) if (!strcmp(c.value, line + 8)) known = true;
                if (!known) ch.push_back({ _strdup((std::string("Custom: ") + (line + 8)).c_str()), _strdup(line + 8) });
            }
            fclose(f);
        }
        add_option(SEC_X_DISPLAY, "savedir", "Save location",
                   "Where saved games and trophies are kept. Game folder keeps everything next to the game (portable). "
                   "Saved Games is the Windows folder for game saves (kept when the game folder is deleted). A folder you "
                   "pick: Saves & tools > Choose save folder. Your saves are copied to the new place on the next Play "
                   "when it has none yet; the old copy stays where it was.",
                   "PS3_GAMEDATA_ROOT", std::move(ch), 0);
    }
    // hybrid CPUs (port_cpu_sets.h): Advanced > CPU THREADS
    add_option(SEC_X_DISPLAY, "pcores", "Prefer P-cores",
               "Intel 12th gen and newer (P-cores + E-cores): the game's main, SPU, RSX and audio threads prefer the fast "
               "P-cores and are never power-throttled; other threads use any core. Does nothing on CPUs without E-cores "
               "(AMD, older Intel). Off leaves every thread to Windows.",
               "PORT_CPU_PCORES", { {"Auto  (hybrid CPUs only)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "threadprio", "Raise game thread priority",
               "Runs the main and RSX threads at above-normal priority, so background programs interrupt them less. "
               "Can make other programs sluggish on CPUs with 4 cores or fewer. Off is the Windows default.",
               "PORT_THREAD_PRIORITY", { {"Off", ""}, {"On", "1"} }, 0);

    // Graphics page "EFFECTS AND ACCESSIBILITY", the crosshair on Controls (all also in game: F10 > More PC Options, or F9)
#if LX_HAS_FX
    add_option(SEC_X_FX, "fx_blur", "Screen blur",
               "The game's full-screen blur effects (damage, sprint, explosions). Off keeps the picture sharp.",
               LX_ENV("FX_SCREEN_BLUR"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_shake", "Camera shake",
               "Explosion and impact camera shake.", LX_ENV("FX_CAMERA_SHAKE"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_letterbox", "Cinematic black bars",
               "Black bars at the top and bottom during in-engine cutscenes.",
               LX_ENV("FX_LETTERBOX"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
#endif
#if defined(LX_HAS_PFX) && LX_HAS_PFX
    add_option(SEC_X_FX, "fx_filmgrain", "Film grain",
               "The game's film grain noise. Off gives a clean picture.",
               LX_ENV("FX_FILMGRAIN"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_dof", "Depth of field",
               "Blur of the background / foreground in cutscenes, while aiming and in some areas. "
               "The in-game menu (F10 > More PC Options) changes it live and saves it here.",
               LX_ENV("FX_DOF"), { {"On  (game default)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_ads_blur", "Aim-down-sights blur",
               "The depth-of-field blur around the sights while aiming. Off keeps the view sharp and skips its CPU work, "
               "so aiming runs as fast as normal play. Cutscene and scene blur are not affected. "
               "Also in game: F10 > PC Options.",
               LX_ENV("FX_ADS_BLUR"), { {"On  (game default)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_ads_approx", "Aim blur",
               "How the aim-down-sights blur is made. PC fast replaces the PS3 blur job with a quicker PC approximation "
               "(close, not pixel-identical; it checks itself against the PS3 blur first and switches back if they differ). "
               "Also in game: F10 > PC Options (next start).",
               LX_ENV("DOF_GPU_APPROX"), { {"Default  (PC fast)", ""}, {"PS3 exact", "0"}, {"PC fast", "1"} }, 0);
    add_option(SEC_X_FX, "fx_motionblur", "Motion blur",
               "Camera motion blur and the blur on fast-moving objects.",
               LX_ENV("FX_MOTIONBLUR"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
#endif
    add_option(SEC_X_FX, "colorblind", "Colour-blind filter",
               "Shifts colours so they are easier to tell apart (daltonization of the final image).",
               LX_ENV("COLORBLIND"), { {"Off", ""}, {"Protanopia  (red)", "1"}, {"Deuteranopia  (green)", "2"},
                                       {"Tritanopia  (blue)", "3"} }, 0, {}, "rpcs3");
    add_option(SEC_X_FX, "reticle", "Game crosshair",
               "The game's own crosshair (the lines around the screen centre). Off hides it; weapon scopes are not affected. "
               "Also in game: F9 / PC CONTROLS & HUD.",
               LX_ENV("RETICLE"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "hitmarker", "Hit markers",
               "The marker the game draws over the crosshair when a shot hits. Also in game: F9 / PC CONTROLS & HUD.",
               LX_ENV("HITMARKER"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "crosshair", "Extra crosshair",
               "A small dot or cross at the screen centre while on foot, for when the game's reticle is hard to see.",
               LX_ENV("CROSSHAIR"), { {"Off", ""}, {"Dot", "dot"}, {"Cross", "cross"} }, 0);
    add_option(SEC_X_FX, "xhair_color", "Crosshair colour",
               "Colour of the extra crosshair. A dark outline is always drawn around it, so every colour stays visible.",
               LX_ENV("CROSSHAIR_COLOR"), { {"White", ""}, {"Green", "green"}, {"Yellow", "yellow"}, {"Cyan", "cyan"}, {"Red", "red"},
                                            {"Pink", "pink"}, {"Orange", "orange"} }, 0);
    add_option(SEC_X_FX, "xhair_size", "Crosshair size", "Size of the extra crosshair (Small = the original 24 px marker).",
               LX_ENV("CROSSHAIR_SIZE"), { {"Small", ""}, {"Medium", "medium"}, {"Large", "large"}, {"Huge", "huge"} }, 0);
    add_option(SEC_X_DISPLAY, "benchkey", "Benchmark capture key",
               "In game, press it to start recording frame times and again to stop. Writes a summary (average, 1% and 0.1% low fps, "
               "stutters) and a per-frame CSV to logs\\bench\\; the last result shows on the Saves & tools page.",
               LX_ENV("BENCH_KEY"), { {"F11", ""}, {"F6", "F6"}, {"F5", "F5"}, {"Off", "off"} }, 0);

    // Controls page: "CONTROLLER" and "CONTROLLER BUTTONS"
    add_option(SEC_X_PAD, "pad_disconnect_pause", "Pause when the controller disconnects",
               "Pauses the game and shows a message when the controller you play with is unplugged or runs out of "
               "battery. Reconnect it, or press Enter, to continue.",
               LX_ENV("PAD_DISCONNECT_PAUSE"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_PAD, "pad_southpaw", "Swap sticks  (southpaw)",
               "Left stick looks, right stick moves.", "PAD_SOUTHPAW", { {"Off", ""}, {"On", "1"} }, 0);
    static const char* keys[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START" };
    static const char* names[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Left stick click", "Right stick click",
                                   "View", "Menu" };
    for (int i = 0; i < 12; i++) {
        std::vector<Choice> ch = { { "Default", "" } };
        for (int j = 0; j < 12; j++) {
            if (j == i) continue;
            char* l = (char*)malloc(48); snprintf(l, 48, "Acts as %s", names[j]);
            ch.push_back({ l, keys[j] });
        }
        char* key = (char*)malloc(24); snprintf(key, 24, "padmap_%s", keys[i]);
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        char* env = (char*)malloc(24); snprintf(env, 24, "PAD_MAP_%s", keys[i]);
        char* title = (char*)malloc(48); snprintf(title, 48, "Button %s", names[i]);
        add_option(SEC_X_PAD, key, title,
                   "What this controller button does in game (Xbox names; a PlayStation pad through Steam Input / "
                   "DS4Windows maps the same way). Swapping two buttons needs both rows. Also in game: F10 > More PC "
                   "Options > Controller layout.", env, std::move(ch), 0);
    }
}

static const char* lx_val(const char* key)
{
    for (auto& s : g_set) if (!strcmp(s.key, key)) return s.choices[s.sel].value;
    return "";
}

// ---- saves -----------------------------------------------------------------------------------------
// The launcher's own PS3_GAMEDATA_ROOT follows the Save location row (the trophies page, Open save folder and
// the game started from here read it); without a choice it is what the launcher was started with.
static void lx_sync_root()
{
    static std::string orig = getenv("PS3_GAMEDATA_ROOT") ? getenv("PS3_GAMEDATA_ROOT") : "", cur = orig;
    const char* v = lx_val("savedir");
    const std::string want = *v ? v : orig;
    if (want != cur) { cur = want; _putenv_s("PS3_GAMEDATA_ROOT", want.c_str()); }
}
static std::string lx_gamedata_root()
{
    lx_sync_root();
    const char* p = getenv("PS3_GAMEDATA_ROOT");
    return (p && *p) ? std::string(p) : path_of("gamedata");
}
static std::string lx_save_dir() { return lx_gamedata_root() + "\\dev_hdd0\\home\\00000001\\savedata"; }
static void lx_make_save_dir()
{
    std::string p = lx_gamedata_root();
    CreateDirectoryA(p.c_str(), NULL);
    for (const char* part : { "\\dev_hdd0", "\\home", "\\00000001", "\\savedata" }) { p += part; CreateDirectoryA(p.c_str(), NULL); }
}
static std::string lx_backup_root() { return path_of("save_backups"); }
struct LxEntry { std::string name, title; ULONGLONG newest = 0, bytes = 0; int files = 0; };
static void lx_scan(const std::string& dir, LxEntry& e)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { lx_scan(dir + "\\" + fd.cFileName, e); continue; }
        const ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        e.newest = std::max(e.newest, t);
        e.bytes += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.files++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
// PARAM.SFO: SUB_TITLE (or TITLE) of a PS3 save
static std::string lx_sfo_title(const std::string& dir)
{
    FILE* f = fopen((dir + "\\PARAM.SFO").c_str(), "rb");
    if (!f) return "";
    std::vector<unsigned char> b(65536);
    const size_t n = fread(b.data(), 1, b.size(), f); fclose(f);
    if (n < 20 || memcmp(b.data(), "\0PSF", 4)) return "";
    auto u32 = [&](size_t o) { return o + 4 <= n ? (unsigned)b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (unsigned)b[o + 3] << 24 : 0u; };
    auto u16 = [&](size_t o) { return o + 2 <= n ? (unsigned)(b[o] | b[o + 1] << 8) : 0u; };
    const unsigned kt = u32(8), dt = u32(12), cnt = u32(16);
    std::string sub, title;
    for (unsigned i = 0; i < cnt && i < 64; i++) {
        const size_t ent = 20 + i * 16;
        const size_t ko = kt + u16(ent), dof = dt + u32(ent + 12), len = u32(ent + 4);
        if (ko >= n || dof + len > n) continue;
        const char* k = (const char*)&b[ko];
        std::string v((const char*)&b[dof], strnlen((const char*)&b[dof], len));
        if (!strcmp(k, "SUB_TITLE")) sub = v; else if (!strcmp(k, "TITLE")) title = v;
    }
    return !sub.empty() ? sub : title;
}
static std::vector<LxEntry> lx_list(const std::string& root, bool saves)
{
    std::vector<LxEntry> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        LxEntry e; e.name = fd.cFileName;
        lx_scan(root + "\\" + e.name, e);
        if (saves) e.title = lx_sfo_title(root + "\\" + e.name);
        out.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [&](const LxEntry& a, const LxEntry& b) { return saves ? a.name < b.name : a.name > b.name; });
    return out;
}
static bool lx_copy_tree(const std::string& from, const std::string& to)
{
    CreateDirectoryA(to.c_str(), NULL);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string a = from + "\\" + fd.cFileName, b = to + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = lx_copy_tree(a, b) && ok;
        else ok = CopyFileA(a.c_str(), b.c_str(), FALSE) && ok;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static void lx_delete_tree(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) lx_delete_tree(p);
            else { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(p.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}
// a backup is save_backups\<YYYY-MM-DD_HHMMSS>_<kind>\<the savedata folders>; kind = auto / manual / before-restore
static std::string lx_backup(const char* kind)
{
    LxEntry cur; lx_scan(lx_save_dir(), cur);
    if (!cur.files) return "";
    CreateDirectoryA(lx_backup_root().c_str(), NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char name[96];
    snprintf(name, sizeof name, "%04u-%02u-%02u_%02u%02u%02u_%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind);
    const std::string dst = lx_backup_root() + "\\" + name;
    if (!lx_copy_tree(lx_save_dir(), dst)) { lx_delete_tree(dst); return ""; }
    return name;
}

// RPCS3 import staging: savedata\.pcimport\<slot>.tmp (the copy) and <slot>.old (the replaced save). The game and
// the saves list skip folders starting with '.', so a leftover never shows as a slot. A run that died mid-swap is
// finished here: <slot> missing + <slot>.old = put the original back; <slot>.old next to the new <slot> = only the
// delete was missed; a .tmp is always an incomplete copy. Returns the originals that could not be moved back.
static std::string lx_import_recover()
{
    const std::string sd = lx_save_dir(), st = sd + "\\.pcimport";
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((st + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::string stuck;
    for (auto& n : names) {
        const std::string p = st + "\\" + n;
        const bool is_old = n.size() > 4 && !_stricmp(n.c_str() + n.size() - 4, ".old");
        if (!is_old) { lx_delete_tree(p); continue; }
        const std::string dst = sd + "\\" + n.substr(0, n.size() - 4);
        if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES) lx_delete_tree(p);
        else if (!MoveFileExA(p.c_str(), dst.c_str(), 0)) stuck += "  " + p + "\n";
    }
    RemoveDirectoryA(st.c_str());   // only goes when empty
    return stuck;
}
// a new Save location without saves yet: copy the ones from the game folder (and the trophy record) once
static void lx_save_migrate()
{
    const std::string root = lx_gamedata_root(), old = path_of("gamedata");
    if (!_stricmp(root.c_str(), old.c_str())) return;
    LxEntry now, before;
    lx_scan(lx_save_dir(), now);
    lx_scan(old + "\\dev_hdd0\\home\\00000001\\savedata", before);
    if (now.files || !before.files) return;
    lx_make_save_dir();
    lx_copy_tree(old + "\\dev_hdd0\\home\\00000001\\savedata", lx_save_dir());
    if (GetFileAttributesA((old + "\\trophies").c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA((root + "\\trophies").c_str()) == INVALID_FILE_ATTRIBUTES)
        lx_copy_tree(old + "\\trophies", root + "\\trophies");
}
// before Play: back up when the saves changed since the newest backup; keep the newest N automatic ones
static void lx_region_switch();
static void lx_auto_backup()
{
    lx_import_recover();
    lx_save_migrate();
    lx_region_switch();
    const char* v = lx_val("savebackup");
    if (!strcmp(v, "0")) return;
    const int keep = *v ? atoi(v) : 10;
    LxEntry cur; lx_scan(lx_save_dir(), cur);
    if (!cur.files) return;
    auto bk = lx_list(lx_backup_root(), false);
    for (auto& b : bk) if (b.newest >= cur.newest && b.files == cur.files && b.bytes == cur.bytes) return;   // unchanged
    lx_backup("auto");
    bk = lx_list(lx_backup_root(), false);      // newest first
    int n = 0;
    for (auto& b : bk) {
        if (b.name.size() < 5 || b.name.compare(b.name.size() - 5, 5, "_auto") != 0) continue;
        if (++n > keep) lx_delete_tree(lx_backup_root() + "\\" + b.name);
    }
}
// ---- mods list (mods\order.txt, read by the game's mods layer: ps3emu/port_pc_extras.h pcx_mod_rank) -------
// Listed in the order they are applied (a later mod wins a file both replace). Folders order.txt does not
// name follow alphabetically; '_' / '-' at the start of a folder name still disables it (older layout).
struct LxMod { std::string name; bool on = true, legacy_off = false; };
static std::string lx_mods_root() { return path_of("mods"); }
static std::vector<LxMod> lx_mods_load()
{
    std::vector<std::pair<std::string, bool>> lines;     // name, disabled
    if (FILE* f = fopen((lx_mods_root() + "\\order.txt").c_str(), "r")) {
        char b[512];
        while (fgets(b, sizeof b, f)) {
            std::string l = b;
            l.erase(0, l.find_first_not_of(" \t"));
            l.erase(l.find_last_not_of(" \t\r\n") + 1);
            if (l.empty() || l[0] == '#') continue;
            const bool dis = l[0] == '-';
            lines.push_back({ dis ? l.substr(1) : l, dis });
        }
        fclose(f);
    }
    std::vector<std::pair<size_t, LxMod>> v;
    for (auto& e : lx_list(lx_mods_root(), false)) {
        // not mods: the HD UI pack and texture pack folders (Advanced > HD UI pack / Texture pack) and caches
        if (!_stricmp(e.name.c_str(), "hd_ui") || !_stricmp(e.name.c_str(), "textures") || e.name[0] == '.') continue;
        LxMod m; m.name = e.name;
        m.legacy_off = m.name[0] == '_' || m.name[0] == '-';
        size_t pos = (size_t)1 << 30;
        for (size_t i = 0; i < lines.size(); i++)
            if (!_stricmp(lines[i].first.c_str(), m.name.c_str())) { pos = i; m.on = !lines[i].second; break; }
        if (m.legacy_off) m.on = false;
        v.push_back({ pos, m });
    }
    std::sort(v.begin(), v.end(), [](const std::pair<size_t, LxMod>& a, const std::pair<size_t, LxMod>& b) {
        return a.first != b.first ? a.first < b.first : _stricmp(a.second.name.c_str(), b.second.name.c_str()) < 0; });
    std::vector<LxMod> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}
static bool lx_mods_save(const std::vector<LxMod>& mods)
{
    FILE* f = fopen((lx_mods_root() + "\\order.txt").c_str(), "w");
    if (!f) return false;
    fprintf(f, "# Mods in the order they are applied: a later mod wins a file both replace. \"-Name\" = off.\n"
               "# Written by the launcher (Saves & tools > Mods); folders not listed here come last, alphabetically.\n");
    for (auto& m : mods) if (!m.legacy_off) fprintf(f, "%s%s\n", m.on ? "" : "-", m.name.c_str());
    fclose(f);
    return true;
}

static std::vector<LxEntry> g_lx_saves, g_lx_backups;
static std::vector<LxMod> g_lx_mods;
static DWORD g_lx_loaded = 0;
static void lx_refresh(bool force)
{
    if (!force && g_lx_loaded && GetTickCount() - g_lx_loaded < 3000) return;
    g_lx_loaded = GetTickCount();
    lx_import_recover();
    g_lx_saves = lx_list(lx_save_dir(), true);
    g_lx_backups = lx_list(lx_backup_root(), false);
    g_lx_mods = lx_mods_load();
}

// ---- folder picker, save location, RPCS3 import ------------------------------------------------------
static std::string lx_pick_folder(const wchar_t* title)
{
    std::string out;
    const HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        d->SetTitle(title);
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->Show(g_hw)) && SUCCEEDED(d->GetResult(&it))) {
            PWSTR w = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
                char b[MAX_PATH * 4];
                if (WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof b, NULL, NULL) > 0) out = b;
                CoTaskMemFree(w);
            }
            it->Release();
        }
        d->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    return out;
}
static void lx_choose_save_folder()
{
    const std::string p = lx_pick_folder(L"Choose where saved games and trophies are kept");
    if (p.empty()) return;
    for (auto& st : g_set) {
        if (strcmp(st.key, "savedir")) continue;
        int idx = -1;
        for (size_t i = 0; i < st.choices.size(); i++) if (!_stricmp(st.choices[i].value, p.c_str())) idx = (int)i;
        if (idx < 0) {
            st.choices.push_back({ _strdup(("Custom: " + p).c_str()), _strdup(p.c_str()) });
            idx = (int)st.choices.size() - 1;
        }
        st.sel = idx;
        save_settings();
        lx_sync_root();
        toast("Save location: " + p + ". Your saves are copied there on the next Play.");
    }
    lx_refresh(true);
}
static const char* const k_lx_ids[] = { LX_TITLE_IDS, nullptr };
static bool lx_read_file(const std::string& p, std::vector<unsigned char>& b)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    b.resize(65536);
    b.resize(fread(b.data(), 1, b.size(), f));
    fclose(f);
    return !b.empty();
}
// the title ID this copy of the game saves under: the game's PARAM.SFO, else the saves already there
static std::string lx_local_id()
{
    std::vector<unsigned char> b;
    char id[32];
    for (const char* p : { "game\\PS3_GAME\\PARAM.SFO", "gamefiles\\PS3_GAME\\PARAM.SFO", "PS3_GAME\\PARAM.SFO", "game\\PARAM.SFO" })
        if (lx_read_file(path_of(p), b) && pcx_sfo_get(b.data(), b.size(), "TITLE_ID", id, sizeof id) && strlen(id) == 9) return id;
    for (auto& e : lx_list(lx_save_dir(), false))
        if (pcx_title_match(e.name.c_str(), k_lx_ids)) return e.name.substr(0, 9);
    return k_lx_ids[0];
}
// Puts the save <src>\<from> into the save folder as <to> (another region's save: <to> is renamed to this copy's
// title ID, and its PARAM.SFO's SAVEDATA_DIRECTORY with it). It is copied into savedata\.pcimport first; only a
// complete copy replaces the slot (the old one is moved aside, then deleted; lx_import_recover finishes a swap a
// crash cut short). An original that could not be put back is added to <stuck>.
static bool lx_place_save(const std::string& src, const std::string& from, const std::string& to, std::string& stuck)
{
    const std::string sd = lx_save_dir(), stage = sd + "\\.pcimport";
    CreateDirectoryA(stage.c_str(), NULL);
    const std::string dst = sd + "\\" + to, tmp = stage + "\\" + to + ".tmp", old = stage + "\\" + to + ".old";
    lx_delete_tree(tmp);
    bool good = lx_copy_tree(src + "\\" + from, tmp);
    if (good && to != from) {
        std::vector<unsigned char> b;
        size_t off = 0, len = 0;
        if (lx_read_file(tmp + "\\PARAM.SFO", b) && pcx_sfo_find(b.data(), b.size(), "SAVEDATA_DIRECTORY", &off, &len) &&
            len >= 9 && !memcmp(&b[off], from.data(), 9)) {
            memcpy(&b[off], to.data(), 9);
            FILE* f = fopen((tmp + "\\PARAM.SFO").c_str(), "r+b");
            good = f && fwrite(b.data(), 1, b.size(), f) == b.size();
            if (f && fclose(f)) good = false;
        }
    }
    const bool had = GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (good && had) { lx_delete_tree(old); good = MoveFileExA(dst.c_str(), old.c_str(), 0) != 0; }
    if (good && !MoveFileExA(tmp.c_str(), dst.c_str(), 0)) {
        if (had && !MoveFileExA(old.c_str(), dst.c_str(), 0)) stuck += "  " + old + "\n";   // put the original back
        good = false;
    }
    if (!good) { lx_delete_tree(tmp); return false; }
    if (had) lx_delete_tree(old);
    return true;
}
// Before Play: the game saves under its disc's title ID (BCUS98176 US, BCES01118 Europe), so after a switch to
// another region's disc it would not see the saves made with the other one. When this copy's ID has no saves yet
// but another region's are here, offer once (per ID) to copy them under this copy's ID; the originals stay, so
// switching back finds them as they were. Answering No is remembered (gamedata\region_saves_<ID>.txt).
static void lx_region_switch()
{
    const std::string local = lx_local_id(), sd = lx_save_dir();
    std::vector<LxEntry> other;
    for (auto& e : lx_list(sd, true)) {
        if (!pcx_title_match(e.name.c_str(), k_lx_ids)) continue;
        if (!_strnicmp(e.name.c_str(), local.c_str(), 9)) return;   // this copy already has saves
        other.push_back(e);
    }
    if (other.empty()) return;
    const std::string mark = lx_gamedata_root() + "\\region_saves_" + local + ".txt";
    if (GetFileAttributesA(mark.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    struct Item { std::string from, to, title; ULONGLONG newest; };
    std::vector<Item> items;
    std::string list;
    for (auto& e : other) {
        char to[260];
        if (!pcx_save_retarget(e.name.c_str(), k_lx_ids, local.c_str(), to, sizeof to)) continue;
        bool dup = false;   // two other regions' saves of the same slot: the newer one
        for (auto& o : items)
            if (!_stricmp(o.to.c_str(), to)) { if (e.newest > o.newest) o = { e.name, to, e.title, e.newest }; dup = true; break; }
        if (!dup) items.push_back({ e.name, to, e.title, e.newest });
    }
    if (items.empty()) return;
    for (auto& it : items) list += "  " + (it.title.empty() ? it.from : it.title + "  (" + it.from + ")") + "  -> " + it.to + "\n";
    const int ans = MessageBoxA(g_hw, ("Your game copy is " + local + ", but your saved games are from another region's "
                                       "disc, and the game only sees the saves of its own disc.\n\nCopy them for " + local +
                                       "?\n\n" + list + "\nThe originals stay as they are (the other disc still finds them).").c_str(),
                                "Saved games from another disc", MB_YESNO | MB_ICONQUESTION);
    if (FILE* f = fopen(mark.c_str(), "w")) { fprintf(f, "asked: %s\n", ans == IDYES ? "copied" : "no"); fclose(f); }
    if (ans != IDYES) return;
    lx_make_save_dir();
    int ok = 0;
    std::string stuck;
    for (auto& it : items) if (lx_place_save(sd, it.from, it.to, stuck)) ok++;
    RemoveDirectoryA((sd + "\\.pcimport").c_str());
    if (ok < (int)items.size())
        MessageBoxA(g_hw, (std::to_string(ok) + " of " + std::to_string(items.size()) + " saved games were copied. The originals are "
                           "unchanged in\n" + sd).c_str(), "Saved games from another disc", MB_OK | MB_ICONWARNING);
}
static void lx_import_rpcs3()
{
    const std::string pick = lx_pick_folder(L"Pick the RPCS3 folder (or its dev_hdd0 or savedata folder)");
    if (pick.empty()) return;
    const std::string left = lx_import_recover();
    if (!left.empty()) {
        MessageBoxA(g_hw, ("An earlier import left these saved games aside and they could not be moved back yet (in use?):\n\n" +
                           left + "\nClose the game and try again.").c_str(), "Import saves from RPCS3", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string src;
    for (const char* sub : { "\\dev_hdd0\\home\\00000001\\savedata", "\\home\\00000001\\savedata", "\\00000001\\savedata", "\\savedata", "" }) {
        const DWORD a = GetFileAttributesA((pick + sub).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { src = pick + sub; break; }
    }
    const std::string local = lx_local_id(), sd = lx_save_dir();
    struct Item { std::string from, to, title; bool local; ULONGLONG mtime; };
    std::vector<Item> items;
    std::string list, skipped;
    for (auto& e : lx_list(src, true)) {
        if (!pcx_title_match(e.name.c_str(), k_lx_ids)) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA((src + "\\" + e.name + "\\PARAM.SFO").c_str(), GetFileExInfoStandard, &fa)) continue;
        char to[260];
        Item it{ e.name, pcx_save_retarget(e.name.c_str(), k_lx_ids, local.c_str(), to, sizeof to) ? std::string(to) : e.name,
                 e.title, !_strnicmp(e.name.c_str(), local.c_str(), 9),
                 ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime };
        // two regions' saves of the same slot: keep this copy's region, else the newer one
        bool dup = false;
        for (auto& o : items) {
            if (_stricmp(o.to.c_str(), it.to.c_str())) continue;
            if (pcx_import_prefer(it.local, it.mtime, o.local, o.mtime)) std::swap(o, it);   // o = the winner
            skipped += "  " + it.from + "  (same slot as " + o.from + ")\n";
            dup = true;
            break;
        }
        if (!dup) items.push_back(it);
    }
    for (auto& it : items) {
        const bool exists = GetFileAttributesA((sd + "\\" + it.to).c_str()) != INVALID_FILE_ATTRIBUTES;
        list += "  " + (it.title.empty() ? it.from : it.title + "  (" + it.from + ")") + (it.to != it.from ? "  -> " + it.to : "") +
                (exists ? "  [replaces yours]" : "") + "\n";
    }
    if (items.empty()) {
        MessageBoxA(g_hw, ("No " LX_USER_DIR " saved games found in\n" + (src.empty() ? pick : src) +
                           "\n\nPick the RPCS3 folder (the one with rpcs3.exe and dev_hdd0).").c_str(),
                    "Import saves from RPCS3", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxA(g_hw, ("Copy these saved games from RPCS3?\n\n" + list +
                           (skipped.empty() ? std::string() : "\nSkipped (another region's save of the same slot; this "
                            "copy's region or the newer one is used):\n" + skipped) +
                           "\nYour current saves are backed up first "
                           "(\"before-import\"). Don't do this while the game is running.").c_str(),
                    "Import saves from RPCS3", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    {
        LxEntry cur; lx_scan(sd, cur);
        if (cur.files && lx_backup("before-import").empty()) {
            MessageBoxA(g_hw, ("Could not back up your current saves to\n" + lx_backup_root() +
                               "\n\n(disk full, or the folder is read-only?), so nothing was imported.").c_str(),
                        "Import saves from RPCS3", MB_OK | MB_ICONERROR);
            return;
        }
    }
    lx_make_save_dir();
    const std::string stage = sd + "\\.pcimport";
    CreateDirectoryA(stage.c_str(), NULL);
    int ok = 0;
    std::string stuck;
    for (auto& it : items)
        if (lx_place_save(src, it.from, it.to, stuck)) ok++;
    RemoveDirectoryA(stage.c_str());
    if (!stuck.empty())
        MessageBoxA(g_hw, ("These saved games could not be put back after a failed copy. They are safe here (the "
                           "launcher retries on its next start), or move them back into\n" + sd +
                           "\nwithout the .old ending:\n\n" + stuck).c_str(), "Import saves from RPCS3", MB_OK | MB_ICONWARNING);
    toast(std::to_string(ok) + " of " + std::to_string(items.size()) + " saved games imported from RPCS3." +
          (ok < (int)items.size() ? " The others could not be copied; those slots were left as they were." : ""));
    lx_refresh(true);
}
static std::string lx_when(ULONGLONG ft)
{
    if (!ft) return "";
    FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) }, l; SYSTEMTIME s;
    FileTimeToLocalFileTime(&f, &l); FileTimeToSystemTime(&l, &s);
    char b[64]; snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
    return b;
}
static std::string lx_size(ULONGLONG b) { char s[32]; snprintf(s, sizeof s, b > 1048576 ? "%.1f MB" : "%.0f KB", b > 1048576 ? b / 1048576.0 : b / 1024.0); return s; }

// ---- benchmark ---------------------------------------------------------------------------------------
static std::string lx_last_bench()
{
    WIN32_FIND_DATAA fd; std::string best; FILETIME bt = {};
    HANDLE h = FindFirstFileA(path_of("logs\\bench\\" LX_TAG "_bench_*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do { if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; best = fd.cFileName; } } while (FindNextFileA(h, &fd));
    FindClose(h);
    FILE* f = fopen(path_of(("logs\\bench\\" + best).c_str()).c_str(), "r");
    if (!f) return "";
    std::string avg, low, low01, stut, usd, mode, line; char b[256];
    while (fgets(b, sizeof b, f)) {
        line = b; line.erase(line.find_last_not_of("\r\n ") + 1);
        auto v = [&](const char* k) { return line.compare(0, strlen(k), k) == 0 ? line.substr(line.find_first_not_of(' ', strlen(k))) : std::string(); };
        if (!v("avg fps").empty()) avg = v("avg fps");
        if (!v("1% low fps").empty()) low = v("1% low fps");
        if (!v("us/draw").empty()) usd = v("us/draw");
        if (!v("0.1% low fps").empty()) low01 = v("0.1% low fps");
        if (!v("stutters").empty()) stut = v("stutters");
        if (!v("mode").empty()) mode = v("mode");
    }
    fclose(f);
    std::string s = std::string(mode == "hotkey capture" ? "Last capture (" : "Last run (") +
                    lx_when(((ULONGLONG)bt.dwHighDateTime << 32) | bt.dwLowDateTime) + "):  " + avg + " fps average,  " + low + " fps 1% low";
    if (!low01.empty()) s += ",  " + low01 + " fps 0.1% low";
    if (!stut.empty()) s += ",  " + stut + (stut == "1" ? " stutter" : " stutters");
    if (!usd.empty()) s += ",  " + usd + " \xC2\xB5s per draw";
    return s;
}

enum { BT_X_BACKUP = 100, BT_X_OPEN_BACKUPS, BT_X_BENCH, BT_X_BENCH_LOGS, BT_X_RESTORE, BT_X_DELETE, BT_X_IMPORT,
       BT_X_SAVEDIR, BT_X_MOD_UP, BT_X_MOD_DOWN, BT_X_MOD_TOGGLE, BT_X_OPEN_MODS, BT_X_HDUI_MAKE, BT_X_HDUI_REMOVE };

static void extra_layout_tools(float x, float& y, float w)
{
    lx_refresh(false);
    deco(D_H1, RectF(x, y, w, SF(34)), "Saves & tools"); y += SF(36);
    deco(D_INTRO, RectF(x, y, w, SF(20)), "Back up, restore and import saved games, manage mods, and measure performance.");
    y += SF(40);
    deco(D_H2, RectF(x, y, w, SF(22)), "SAVED GAMES"); y += SF(30);
    if (g_lx_saves.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No saved games yet."); y += SF(30); }
    for (size_t i = 0; i < g_lx_saves.size(); i++) {
        const LxEntry& e = g_lx_saves[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(34)), -300 - (int)i);
        row.title = e.name; row.help = e.title.empty() ? e.name : e.title + "  (" + e.name + ")";
        deco(D_LABEL, RectF(x, y, w - SF(260), SF(34)), e.title.empty() ? e.name : e.title);
        deco(D_LABEL_DIM, RectF(x + w - SF(250), y, SF(250), SF(34)), lx_when(e.newest) + "  \xC2\xB7  " + lx_size(e.bytes));
        y += SF(36);
    }
    y += SF(8);
    Widget& b1 = add(W_BUTTON, RectF(x, y, SF(150), SF(38)), BT_X_BACKUP);
    b1.title = "Back up now"; b1.help = "Copies all saved games to save_backups\\ (kept until you delete them).";
    Widget& b2 = add(W_BUTTON, RectF(x + SF(162), y, SF(170), SF(38)), BT_SAVES);
    b2.title = "Open save folder"; b2.help = "The game's save folder (gamedata\\dev_hdd0\\home\\00000001\\savedata).";
    Widget& b3 = add(W_BUTTON, RectF(x + SF(344), y, SF(190), SF(38)), BT_X_OPEN_BACKUPS);
    b3.title = "Open backups folder"; b3.help = "save_backups\\ in the game folder.";
    y += SF(46);
    Widget& bi = add(W_BUTTON, RectF(x, y, SF(240), SF(38)), BT_X_IMPORT);
    bi.title = "Import saves from RPCS3"; bi.help = "Pick your RPCS3 folder: copies its " LX_USER_DIR " saved games (any "
        "region) into this game's saves. Yours are backed up first; a save of the same slot is replaced.";
    Widget& bd = add(W_BUTTON, RectF(x + SF(252), y, SF(210), SF(38)), BT_X_SAVEDIR);
    bd.title = "Choose save folder"; bd.help = "Keeps saved games and trophies in a folder you pick (Advanced > Save location).";
    y += SF(56);
    deco(D_H2, RectF(x, y, w, SF(22)), "BACKUPS"); y += SF(30);
    if (g_lx_backups.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No backups yet. Play makes one automatically (Advanced page, "
                                                                     "\"Back up saves before playing\")."); y += SF(30); }
    for (size_t i = 0; i < g_lx_backups.size() && i < 40; i++) {
        const LxEntry& e = g_lx_backups[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(40)), -400 - (int)i);
        row.title = e.name; row.help = "Backup " + e.name + ": " + std::to_string(e.files) + " files, " + lx_size(e.bytes) + ".";
        deco(D_LABEL, RectF(x, y, w - SF(230), SF(40)), e.name);
        Widget& r = add(W_BUTTON, RectF(x + w - SF(220), y + SF(4), SF(110), SF(32)), BT_X_RESTORE, (int)i);
        r.title = "Restore"; r.help = "Replaces your current saved games with this backup (they are backed up first).";
        Widget& d = add(W_BUTTON, RectF(x + w - SF(100), y + SF(4), SF(100), SF(32)), BT_X_DELETE, (int)i);
        d.title = "Delete"; d.help = "Deletes this backup.";
        y += SF(42);
    }
    y += SF(24);
    deco(D_H2, RectF(x, y, w, SF(22)), "MODS"); y += SF(30);
    deco(D_NOTE, RectF(x, y, w, SF(40)), std::string(*lx_val("mods") ? "" : "Mods are off: turn on Advanced > Mods folder. ") +
         "Each folder in mods\\ is one mod. They are applied top to bottom: a lower one wins a file both replace.");
    y += SF(44);
    if (g_lx_mods.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No mods installed (mods\\<mod name>\\USRDIR\\...)."); y += SF(30); }
    for (size_t i = 0; i < g_lx_mods.size() && i < 60; i++) {
        const LxMod& m = g_lx_mods[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(40)), -500 - (int)i);
        row.title = m.name;
        row.help = m.legacy_off ? "Off: the folder name starts with '_' or '-'. Rename it to turn it on." : "mods\\" + m.name;
        deco(m.on ? D_LABEL : D_LABEL_DIM, RectF(x, y, w - SF(330), SF(40)), m.name + (m.on ? "" : "   (off)"));
        Widget& u = add(W_BUTTON, RectF(x + w - SF(320), y + SF(4), SF(90), SF(32)), BT_X_MOD_UP, (int)i);
        u.title = "Up"; u.help = "Applied earlier (a mod below it wins a shared file)."; u.disabled = i == 0 || m.legacy_off;
        Widget& dn = add(W_BUTTON, RectF(x + w - SF(220), y + SF(4), SF(90), SF(32)), BT_X_MOD_DOWN, (int)i);
        dn.title = "Down"; dn.help = "Applied later (wins a file it shares with the mods above)."; dn.disabled = i + 1 == g_lx_mods.size() || m.legacy_off;
        Widget& t = add(W_BUTTON, RectF(x + w - SF(120), y + SF(4), SF(120), SF(32)), BT_X_MOD_TOGGLE, (int)i);
        t.title = m.on ? "Turn off" : "Turn on"; t.help = "Turns this mod on or off (mods\\order.txt)."; t.disabled = m.legacy_off;
        y += SF(42);
    }
    Widget& bm = add(W_BUTTON, RectF(x, y + SF(4), SF(190), SF(38)), BT_X_OPEN_MODS);
    bm.title = "Open mods folder"; bm.help = "mods\\ in the game folder (created if missing).";
    y += SF(56);
    deco(D_H2, RectF(x, y, w, SF(22)), "HD UI PACK"); y += SF(30);
    {
        const bool have = hdui_have_pack();
        deco(D_NOTE, RectF(x, y, w, SF(60)), std::string(have ? (*lx_val("hdui") ? "Installed and on. " : "Installed, but off (Advanced > HD UI pack). ")
                                                              : "Not created yet. ") +
             "Upscales the menu and HUD textures 2-4x with an AI model on the graphics card (the CPU if that fails), from "
             "your own game files (minutes on a recent graphics card, several GB once). Then turns Advanced > HD UI pack on.");
        y += SF(66);
        Widget& hm = add(W_BUTTON, RectF(x, y, SF(240), SF(38)), BT_X_HDUI_MAKE);
        hm.title = have ? "Update HD UI pack" : "Create HD UI pack";
        hm.help = "Makes mods\\hd_ui from the game files on the graphics card, or the CPU if that fails (Real-ESRGAN comes with the "
                  "port), skips textures already made. "
                  "Log: logs\\hd_ui_pack.log.";
        Widget& hr = add(W_BUTTON, RectF(x + SF(252), y, SF(190), SF(38)), BT_X_HDUI_REMOVE);
        hr.title = "Remove HD UI pack"; hr.help = "Deletes mods\\hd_ui and turns HD UI pack off."; hr.disabled = !have;
        y += SF(56);
    }
    deco(D_H2, RectF(x, y, w, SF(22)), "BENCHMARK"); y += SF(30);
    deco(D_NOTE, RectF(x, y, w, SF(60)), "Starts the game with your settings, goes from the title screen into the first level by "
                                         "itself, measures for 60 seconds, writes logs\\bench\\ and closes the game. Don't touch "
                                         "the keyboard or controller while it runs (about 4 minutes).");
    y += SF(66);
    Widget& b4 = add(W_BUTTON, RectF(x, y, SF(170), SF(38)), BT_X_BENCH);
    b4.title = "Run benchmark"; b4.help = "Average / 1% / 0.1% low fps, stutters and GPU time per draw call, to logs\\bench\\ (summary + per-frame CSV).";
    Widget& b5 = add(W_BUTTON, RectF(x + SF(182), y, SF(170), SF(38)), BT_X_BENCH_LOGS);
    b5.title = "Open results"; b5.help = "logs\\bench\\: one report per run and bench_history.csv.";
    y += SF(46);
    const std::string last = lx_last_bench();
    if (!last.empty()) { deco(D_LABEL_DIM, RectF(x, y, w, SF(28)), last); y += SF(30); }
}

// returns true when the button was ours
static bool extra_click(int id, int sub)
{
    if (id == BT_X_BACKUP) {
        const std::string n = lx_backup("manual");
        toast(n.empty() ? "Nothing to back up (no saved games yet)." : "Saved games backed up to save_backups\\" + n);
        lx_refresh(true);
    } else if (id == BT_X_OPEN_BACKUPS) {
        CreateDirectoryA(lx_backup_root().c_str(), NULL);
        ShellExecuteA(g_hw, "open", lx_backup_root().c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == BT_X_RESTORE || id == BT_X_DELETE) {
        lx_refresh(true);
        if (sub < 0 || sub >= (int)g_lx_backups.size()) return true;
        const std::string name = g_lx_backups[sub].name, src = lx_backup_root() + "\\" + name;
        if (id == BT_X_DELETE) {
            if (MessageBoxA(g_hw, ("Delete the backup " + name + "?").c_str(), "Saves", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                lx_delete_tree(src); toast("Backup deleted.");
            }
        } else if (MessageBoxA(g_hw, ("Replace your saved games with the backup " + name + "?\n\nYour current saves are backed "
                                      "up first (\"before-restore\"). Don't do this while the game is running.").c_str(),
                               "Restore saves", MB_YESNO | MB_ICONWARNING) == IDYES) {
            lx_backup("before-restore");
            const std::string sd = lx_save_dir();
            for (auto& s : lx_list(sd, true)) lx_delete_tree(sd + "\\" + s.name);
            lx_make_save_dir();
            toast(lx_copy_tree(src, sd) ? "Saved games restored from " + name + "." : "Restore failed - see save_backups\\.");
        }
        lx_refresh(true);
    } else if (id == BT_X_IMPORT) {
        lx_import_rpcs3();
    } else if (id == BT_X_SAVEDIR) {
        lx_choose_save_folder();
    } else if (id == BT_X_MOD_UP || id == BT_X_MOD_DOWN || id == BT_X_MOD_TOGGLE) {
        g_lx_mods = lx_mods_load();
        if (sub < 0 || sub >= (int)g_lx_mods.size() || g_lx_mods[sub].legacy_off) return true;
        if (id == BT_X_MOD_TOGGLE) g_lx_mods[sub].on = !g_lx_mods[sub].on;
        else {
            const int to = sub + (id == BT_X_MOD_UP ? -1 : 1);
            if (to < 0 || to >= (int)g_lx_mods.size()) return true;
            std::swap(g_lx_mods[sub], g_lx_mods[to]);
        }
        if (!lx_mods_save(g_lx_mods)) toast("Could not write mods\\order.txt.");
        lx_refresh(true);
    } else if (id == BT_X_OPEN_MODS) {
        CreateDirectoryA(lx_mods_root().c_str(), NULL);
        ShellExecuteA(g_hw, "open", lx_mods_root().c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == BT_X_HDUI_MAKE) {
        hdui_start();
    } else if (id == BT_X_HDUI_REMOVE) {
        hdui_remove();
        lx_refresh(true);
    } else if (id == BT_X_BENCH) {
        if (MessageBoxA(g_hw, "Run the benchmark now?\n\nThe game starts, goes into the first level by itself, measures for 60 "
                              "seconds and closes. Don't press anything while it runs.", "Benchmark",
                        MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return true;
        SetEnvironmentVariableA(LX_ENV("BENCH"), "1");
        SetEnvironmentVariableA(LX_BG_ENV, "run");   // keeps going if the window loses focus
        SetEnvironmentVariableA(LX_ENV("PAD_DISCONNECT_PAUSE"), "0");
        play();
    } else if (id == BT_X_BENCH_LOGS) {
        CreateDirectoryA(path_of("logs").c_str(), NULL); CreateDirectoryA(path_of("logs\\bench").c_str(), NULL);
        ShellExecuteA(g_hw, "open", path_of("logs\\bench").c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else return false;
    return true;
}
