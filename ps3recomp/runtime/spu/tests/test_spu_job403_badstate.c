/*
 * Resistance 3 job 403 (igjob_00D7A480) bad-state callback guard
 * (spu_channels.c, spu_j403_bad_state_skip).
 *
 * Job 403 calls a callback from LS 0x14C2C + 4*state (bisl $r0,$r90 at
 * 0xF8F0, link 0xF8F4), state = the claimed object's halfword +0x18 at LS
 * 0x14C78. Issue #42's 0.3.1 report (end of Haven) had a state past the table
 * whose slot held 0x32800, data that starts 05FF2800: the first 47 words
 * decode, the unlifted-job interpreter ran them, halted at 0x328BC and the
 * lane stopped -- the game froze. The guard must skip such a call before the
 * target is resolved and leave a valid state's call alone.
 *
 * Build (from ps3recomp/):
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o test_spu_job403_badstate runtime/spu/tests/test_spu_job403_badstate.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread -lm
 *   (Windows, clang + MSVC environment: add -Wl,/force:unresolved for the
 *   port symbols the channel layer references off this path.)
 *
 * Exit code: 0 if all passed, 1 if any failed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

static int g_pass, g_fail;
static const char* g_current = "(none)";
#define TEST(name) g_current = (name)
#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

static spu_context g_ctx;

static void ls_put32(uint32_t lsa, uint32_t v)
{
    uint8_t* p = g_ctx.ls + (lsa & SPU_LS_MASK);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Job 403 at its call: the load sequence in LS, r3 = the object, r90 = the
 * target read from slot 0x14C2C + 4*state, link 0xF8F4. */
static void setup(uint16_t state, uint32_t target, int with_site)
{
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.ls = g_ctx.ls_store;
    g_ctx.policy_mode = 1;
    g_ctx.image_id = 2;
    g_ctx.status = SPU_STATUS_RUNNING;
    if (with_site)
        for (int k = 0; k < 13; k++) ls_put32(0xF8C0u + 4u * (uint32_t)k, k_j403_f8c0[k]);
    g_ctx.ls[0x14C78] = (uint8_t)(state >> 8);
    g_ctx.ls[0x14C79] = (uint8_t)state;
    for (uint32_t a = 0x32800u; a < 0x328BCu; a += 4) ls_put32(a, 0x05FF2800u);
    ls_put32(0x328BCu, 0x03FF2800u);
    g_ctx.gpr[0]._u32[0] = 0xF8F4u;
    g_ctx.gpr[3]._u32[0] = 0x14C60u;
    g_ctx.gpr[90]._u32[0] = target;
    g_ctx.pc = target;
    g_spu_trampoline_fn = 0;
}

/* The handler call at 0xF6AC: r65 = state index, r2 = LS 0x108D0 + 4*index, link 0xF6B0. */
static void setup_handler(uint16_t state, uint32_t target, int with_site)
{
    setup(state, target, 0);
    if (with_site)
        for (int k = 0; k < 16; k++) ls_put32(0xF670u + 4u * (uint32_t)k, k_j403_f670[k]);
    g_ctx.gpr[0]._u32[0] = 0xF6B0u;
    g_ctx.gpr[65]._u32[0] = state;
    g_ctx.gpr[2]._u32[0] = target;
    g_ctx.gpr[90]._u32[0] = 0;
}

int main(void)
{
    TEST("end of Haven: state 0x8003 -> slot past the table -> 0x32800 data");
    setup(0x8003, 0x32800u, 1);
    spu_indirect_branch(&g_ctx);
    CHECK((g_ctx.pc & SPU_LS_MASK) == 0xF8F4u);
    CHECK(g_spu_trampoline_fn == spu_indirect_branch);
    CHECK(!(g_ctx.status & (SPU_STATUS_STOPPED_BY_HALT | SPU_STATUS_STOPPED_BY_STOP)));

    TEST("#42 Haven evacuation: unaligned 0x8D from a bad state");
    setup(0x8000, 0x8Du, 1);
    spu_indirect_branch(&g_ctx);
    CHECK((g_ctx.pc & SPU_LS_MASK) == 0xF8F4u);
    CHECK(g_spu_trampoline_fn == spu_indirect_branch);

    TEST("first state past the table (13) is skipped");
    setup(13, 0x32800u, 1);
    CHECK(spu_j403_bad_state_skip(&g_ctx) == 1);

    TEST("valid states are left to the call");
    for (uint16_t s = 0; s < 13; s++) {
        setup(s, 0x32800u, 1);
        CHECK(spu_j403_bad_state_skip(&g_ctx) == 0);
        CHECK(g_ctx.pc == 0x32800u);
    }

    TEST("another job's call with link 0xF8F4 is left alone");
    setup(0x8003, 0x32800u, 0);
    CHECK(spu_j403_bad_state_skip(&g_ctx) == 0);

    TEST("target not the one r90 holds (a return, not this call)");
    setup(0x8003, 0x32800u, 1);
    g_ctx.pc = 0xF8F4u;
    CHECK(spu_j403_bad_state_skip(&g_ctx) == 0);

    TEST("handler table: done state 0x8003 -> LS 0x308DC word -> skipped back to 0xF6B0");
    setup_handler(0x8003, 0x32800u, 1);
    spu_indirect_branch(&g_ctx);
    CHECK((g_ctx.pc & SPU_LS_MASK) == 0xF6B0u);
    CHECK(g_spu_trampoline_fn == spu_indirect_branch);
    CHECK(!(g_ctx.status & (SPU_STATUS_STOPPED_BY_HALT | SPU_STATUS_STOPPED_BY_STOP)));

    TEST("handler table: valid indexes are left to the call");
    for (uint16_t s = 0; s < 13; s++) {
        setup_handler(s, 0x32800u, 1);
        CHECK(spu_j403_bad_state_skip(&g_ctx) == 0);
        CHECK(g_ctx.pc == 0x32800u);
    }

    TEST("handler table: another job's call with link 0xF6B0 is left alone");
    setup_handler(0x8003, 0x32800u, 0);
    CHECK(spu_j403_bad_state_skip(&g_ctx) == 0);

    printf("job 403 bad state: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed as in
 * test_spurs_job_stack.c. None of it is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

/* Port symbols the channel layer references (none on this path). */
#undef getenv
char* rsx_getenv(const char* name) { return getenv(name); }
volatile int g_ps3_host_paused = 0;
volatile int g_spu_jw_on = 0;
int ps3_host_prio_boost(void) { return 0; }
