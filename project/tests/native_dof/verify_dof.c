/* Offline verifier for the native job-419 DOF kernels (project\r3_native_dof_gen_*.c).
 * Replays every R3DOF001 capture (scratch\dof_capture\*.bin, written by the
 * lifted kernel in-game with R3_DOF_CAPTURE=N) through the native kernel and
 * compares the whole 256 KB LS and all 128 registers byte for byte.
 *
 *   verify_dof.exe [capture_dir] [reps]     exit code 0 = every capture identical,
 *                                           1 = mismatch, 77 = no captures (ctest skip)
 *   VERIFY_RN=1  run with round-to-nearest instead of the SPU's truncation (negative test:
 *                must report mismatches, proving the comparison is sensitive)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "r3_native_dof_kern.h"

typedef int (*gen_fn)(uint8_t*, u128*, uint32_t, uint32_t, uint32_t, uint32_t*);
static gen_fn gen_for(uint32_t img)
{
    switch (img) {
    case 618: return r3dof_gen_618; case 619: return r3dof_gen_619; case 620: return r3dof_gen_620;
    case 621: return r3dof_gen_621; case 622: return r3dof_gen_622; case 623: return r3dof_gen_623;
    default: return NULL;
    }
}

#define CAP_SIZE (8 + 32 + 2 * (2048 + SPU_LS_SIZE))
static double g_mode_us[16]; static long g_mode_n[16];
typedef struct { uint32_t img; long n, pass, fail, skip; double us; } stat_t;

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "D:\\Games\\Resistance3Port\\scratch\\dof_capture";
    const int reps = argc > 2 ? atoi(argv[2]) : 1;
    /* the SPU threads' MXCSR: FTZ | DAZ | round toward zero, exceptions masked */
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x8040u | (getenv("VERIFY_RN") ? 0u : 0x6000u));
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    stat_t st[16] = {0}; int nst = 0;
    uint8_t* buf = (uint8_t*)malloc(CAP_SIZE);
    uint8_t* ls = (uint8_t*)_aligned_malloc(SPU_LS_SIZE, 64);
    u128* gpr = (u128*)_aligned_malloc(2048, 64);
    char pat[600]; snprintf(pat, sizeof pat, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "no captures in %s (skipped)\n", dir); return 77; }
    long total = 0, bad = 0;
    do {
        char fn[900]; snprintf(fn, sizeof fn, "%s\\%s", dir, fd.cFileName);
        FILE* f = fopen(fn, "rb");
        if (!f) continue;
        size_t got = fread(buf, 1, CAP_SIZE, f); fclose(f);
        if (got != CAP_SIZE || memcmp(buf, "R3DOF001", 8)) { fprintf(stderr, "%s: bad file\n", fd.cFileName); bad++; continue; }
        uint32_t hd[8]; memcpy(hd, buf + 8, 32);
        const uint32_t lr = hd[0], pc = hd[1], img = hd[2], off = hd[3];
        const uint8_t* pre_gpr = buf + 40, *pre_ls = pre_gpr + 2048;
        const uint8_t* post_gpr = pre_ls + SPU_LS_SIZE, *post_ls = post_gpr + 2048;
        stat_t* s = NULL;
        for (int i = 0; i < nst; i++) if (st[i].img == img) s = &st[i];
        if (!s) { s = &st[nst++]; s->img = img; }
        s->n++; total++;
        gen_fn g = gen_for(img);
        if (!g) { s->skip++; continue; }
        int rc = 0; uint32_t fpc = 0;
        double best = 1e30;
        for (int r = 0; r < reps; r++) {
            memcpy(ls, pre_ls, SPU_LS_SIZE); memcpy(gpr, pre_gpr, 2048);
            LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
            rc = g(ls, gpr, pc - off, pc, lr, &fpc);
            QueryPerformanceCounter(&t1);
            const double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)qf.QuadPart;
            if (us < best) best = us;
        }
        s->us += best;
        if (img == 623) {
            const uint32_t a = gpr == NULL ? 0 : 0; (void)a;
            const uint32_t r3 = ((const u128*)pre_gpr)[3]._u32[0] & 0x3FFF0;
            const uint32_t p = ((uint32_t)pre_ls[r3] << 24 | pre_ls[r3+1] << 16 | pre_ls[r3+2] << 8 | pre_ls[r3+3]) & 0x3FFFF;
            const uint32_t mode = (uint32_t)pre_ls[p+0x20] << 24 | pre_ls[p+0x21] << 16 | pre_ls[p+0x22] << 8 | pre_ls[p+0x23];
            if (mode < 16) { g_mode_us[mode] += best; g_mode_n[mode]++; }
        }
        int ok = rc == 1;
        long first_ls = -1; int first_reg = -1;
        if (ok) {
            for (long i = 0; i < SPU_LS_SIZE; i++) if (ls[i] != post_ls[i]) { first_ls = i; break; }
            for (int r = 0; r < 128; r++) if (memcmp(&gpr[r], post_gpr + r * 16, 16)) { first_reg = r; break; }
            ok = first_ls < 0 && first_reg < 0;
        }
        if (ok) s->pass++;
        else {
            s->fail++; bad++;
            if (s->fail <= 5) {
                long nd = 0;
                for (long i = 0; i < SPU_LS_SIZE; i++) nd += ls[i] != post_ls[i];
                fprintf(stderr, "FAIL %s: rc=%d fail_pc=0x%X first LS diff 0x%lX (%ld bytes differ) first reg diff r%d\n",
                        fd.cFileName, rc, fpc, first_ls, nd, first_reg);
                if (first_ls >= 0) {
                    const long a = first_ls & ~15L;
                    fprintf(stderr, "   native:");
                    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", ls[a + i]);
                    fprintf(stderr, "\n   lifted:");
                    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", post_ls[a + i]);
                    fprintf(stderr, "\n   pre   :");
                    for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", pre_ls[a + i]);
                    fprintf(stderr, "\n");
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (int i = 0; i < nst; i++)
        printf("img %u: %ld captures, %ld identical, %ld differ, %ld no native kernel, native avg %.1f us\n",
               st[i].img, st[i].n, st[i].pass, st[i].fail, st[i].skip,
               (st[i].n - st[i].skip) ? st[i].us / (double)(st[i].n - st[i].skip) : 0.0);
    for (int m = 0; m < 16; m++) if (g_mode_n[m]) printf("  img 623 mode %d: %ld calls, native avg %.1f us\n", m, g_mode_n[m], g_mode_us[m] / g_mode_n[m]);
    printf("%s: %ld captures, %ld not identical\n", bad ? "FAILED" : "ALL IDENTICAL", total, bad);
    return bad ? 1 : 0;
}
