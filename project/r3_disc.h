/* Which Resistance 3 disc and program this is, logged at boot (main.cpp):
 *
 *   [boot] disc: BCES01118 (Europe) APP_VER 01.00, program EBOOT 1.00 Europe, PPU hash 2701cf37... (experimental)
 *
 * The program is identified by RPCS3's PPU hash (PPUModule.cpp: SHA-1 over every program header's type and flags,
 * and each loaded segment's address, size and bytes; the "PPU-..." in RPCS3's log and patch.yml). The same list is
 * in tools/installer/game_profile.psd1 (Eboots), which Setup uses. The US and European 1.00 programs have different
 * hashes but the same layout (patch.yml patches both at the same addresses); the European one has not been run on
 * a real copy yet.
 *
 * R3_GEN_PPU_HASH (from recompiled/.eboot_ppu, which Setup writes after generating the game code; see
 * CMakeLists.txt) is the program this build's game code was generated from. Running it on another program would
 * run code that does not match the program's data, so that is an error too.
 * R3_SKIP_EBOOTCHECK=1 skips the dialogs (the [boot] line is still logged). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/nid.h"             /* ps3_sha1_* */
#include "ps3emu/port_pc_extras.h"  /* pcx_sfo_get */

#ifndef R3_GEN_PPU_HASH
#define R3_GEN_PPU_HASH ""
#endif

struct r3_eboot_known { const char* ppu; const char* id; const char* ver; const char* region; const char* status; };
static const r3_eboot_known k_r3_eboots[] = {
    { "502feb2351d2b637a2704d5a50cb44bc1b781532", "BCUS98176", "1.00", "US",     "ok" },
    { "2701cf372814d59f26e771dc9394323d4f799682", "BCES01118", "1.00", "Europe", "experimental" },
    { "4aa1887bfdd285e3026e6a97d65ed617ddcf4789", "BCUS98176", "1.05", "US",     "update" },
    { "cd72aa19da7e24a00802932d06460034c7d94fc6", "BCES01118", "1.05", "Europe", "update" },
};

static inline uint64_t r3_be_(const uint8_t* p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | p[i]; return v; }

/* RPCS3's PPU hash of an ELF image in memory, as 40 hex digits. 0 when it is not a 64-bit big-endian ELF. */
static inline int r3_ppu_hash(const uint8_t* d, size_t n, char out[41])
{
    if (n < 64 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 2) return 0;
    const uint64_t phoff = r3_be_(d + 0x20, 8);
    const unsigned phent = (unsigned)r3_be_(d + 0x36, 2), phnum = (unsigned)r3_be_(d + 0x38, 2);
    if (phent < 56 || phoff > n || (uint64_t)phent * phnum > n - phoff) return 0;
    ps3_sha1_ctx c; ps3_sha1_init(&c);
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = d + phoff + (uint64_t)i * phent;
        ps3_sha1_update(&c, ph, 8);                                   /* p_type, p_flags */
        if (r3_be_(ph, 4) == 1 && r3_be_(ph + 40, 8)) {               /* PT_LOAD with p_memsz */
            const uint64_t off = r3_be_(ph + 8, 8), fsz = r3_be_(ph + 32, 8);
            if (off > n || fsz > n - off) return 0;
            ps3_sha1_update(&c, ph + 16, 8);                          /* p_vaddr */
            ps3_sha1_update(&c, ph + 40, 8);                          /* p_memsz */
            ps3_sha1_update(&c, d + off, (size_t)fsz);
        }
    }
    uint8_t dg[20]; ps3_sha1_final(&c, dg);
    for (int i = 0; i < 20; i++) snprintf(out + 2 * i, 3, "%02x", dg[i]);
    return 1;
}

static inline unsigned char* r3_read_file_(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    unsigned char* b = nullptr;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len > 0 && len < (1L << 30) && fseek(f, 0, SEEK_SET) == 0 && (b = (unsigned char*)malloc((size_t)len))) {
            if (fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = nullptr; }
            else *n = (size_t)len;
        }
    }
    fclose(f);
    return b;
}

/* Logs the disc and program; returns 1 to go on, 0 when the player chose to quit. */
static int r3_disc_check(const char* elf_path, const char* sfo_path)
{
    char hash[41] = "", title[32] = "", ver[16] = "";
    size_t n = 0;
    if (unsigned char* d = r3_read_file_(elf_path, &n)) { r3_ppu_hash(d, n, hash); free(d); }
    if (unsigned char* s = r3_read_file_(sfo_path, &n)) {
        pcx_sfo_get(s, n, "TITLE_ID", title, sizeof title);
        pcx_sfo_get(s, n, "APP_VER", ver, sizeof ver);
        free(s);
    }
    const r3_eboot_known* k = nullptr;
    for (const auto& e : k_r3_eboots) if (hash[0] && !strcmp(hash, e.ppu)) k = &e;
    const char* region = !strncmp(title, "BCUS", 4) ? "US" : !strncmp(title, "BCES", 4) ? "Europe" : "?";
    char prog[96];
    if (k) snprintf(prog, sizeof prog, "EBOOT %s %s", k->ver, k->region);
    else   snprintf(prog, sizeof prog, "%s", hash[0] ? "UNKNOWN program" : "unreadable program");
    const char* gen = R3_GEN_PPU_HASH;
    const bool gen_differs = gen[0] && hash[0] && strcmp(gen, hash);
    printf("[boot] disc: %s (%s) APP_VER %s, program %s, PPU hash %s%s%s\n", title[0] ? title : "(no PARAM.SFO)", region,
           ver[0] ? ver : "?", prog, hash[0] ? hash : "-",
           k && !strcmp(k->status, "experimental") ? " (experimental: not yet tested on a real copy)" : "",
           gen_differs ? "; the game code was generated from ANOTHER program" : "");
    if (k && strcmp(title, k->id) && title[0])
        printf("[boot] note: PARAM.SFO is %s but the program is %s's (a mixed copy?); saves use %s\n", title, k->id, title);
    fflush(stdout);

    const char* sk = getenv("R3_SKIP_EBOOTCHECK");
    if (sk && *sk && *sk != '0') return 1;
    char m[1024];
    if (!k || !strcmp(k->status, "update")) {
        snprintf(m, sizeof m,
                 "game\\EBOOT.elf is %s.\n\nThe port supports the unpatched disc, version 1.00: US BCUS98176, or "
                 "(experimental) Europe BCES01118. A game update or another release does not match the port's "
                 "fixes and will most likely crash.\n\nRun Setup.exe again with your disc copy.\n\nStart anyway?",
                 k ? "a game update (1.05), not the disc's program" : "not a known version of Resistance 3");
    } else if (gen_differs) {
        snprintf(m, sizeof m,
                 "This build's game code was generated from another version of the game (another disc or region) "
                 "than game\\EBOOT.elf (%s %s).\n\nRun Setup.exe again: it generates the game code from this copy "
                 "and rebuilds the port.\n\nStart anyway? (it will most likely crash)", k->id, k->region);
    } else {
        return 1;
    }
    fprintf(stderr, "[boot] %s (R3_SKIP_EBOOTCHECK=1 skips this check)\n", k ? "game code / program mismatch" : "unknown program");
    fflush(NULL);
#ifdef _WIN32
    return MessageBoxA(NULL, m, "Resistance 3", MB_ICONERROR | MB_YESNO | MB_DEFBUTTON2) == IDYES;
#else
    return 1;
#endif
}
