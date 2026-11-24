/* rsx_d3d12_engine.c -- "Direct3D 12 (experimental)": a D3D12 backend for the
 * register-file draw engine (rsx_draw_engine.c).
 *
 * The engine decodes the guest's NV4097 register file, decompiles its vertex
 * and fragment programs to HLSL (rsx_vp_decompiler.c / rsx_fp_decompiler.c),
 * expands the primitives a host lacks and hands this file one callback per
 * operation (rsx_draw_backend). The Vulkan backend (rsx_vulkan_backend.c)
 * implements the same interface and is the behavioural reference for every
 * callback here; differences are called out where D3D12 cannot express the
 * Vulkan behaviour.
 *
 * Design:
 *   - One direct queue and one command list, always recording. A frame owns
 *     one half of every ring (upload memory, shader-visible CBV/SRV
 *     descriptors) and one command allocator; present switches halves after
 *     waiting for the frame that last used the other one, so the CPU runs at
 *     most one frame ahead. A ring that fills mid-frame submits and waits.
 *   - Every object (texture, colour target, depth target, view) is a slot in
 *     s_obj with its CPU-only SRV/RTV/DSV descriptors at the slot's index;
 *     draws copy the SRVs they use into the shader-visible ring.
 *   - Released resources are kept until the GPU has passed the submission
 *     that last could have used them (s_dead).
 *   - One root signature for guest draws (b0 VS constants, b1 PS constants,
 *     t0-15 + s0-15 pixel, t16-19 + s0-3 vertex) and one for the internal
 *     passes (blit, depth resolve/copy, masked clear, present).
 *
 * Windows only; everything is inside _WIN32. The guard also keeps the tree
 * building while the .inc parts are being written. */
#if defined(_WIN32) && defined(__has_include)
#if __has_include("rsx_d3d12_engine_pass.inc") && __has_include("rsx_d3d12_engine_draw.inc")
#define RSX_D3D12_ENGINE_ON 1
#endif
#endif
#ifdef RSX_D3D12_ENGINE_ON

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include "rsx_d3d12_compat.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include "rsx_d3d12_engine.h"
#include "rsx_draw_engine.h"
#include "rsx_texture_layout.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vk_scale_math.h"
#include "rsx_vk_present_rules.h"

#define D3E_LOG(...) do { fprintf(stderr, "[RSX d3d12] " __VA_ARGS__); fflush(stderr); } while (0)
/* Log a message once per bit of s_d.warned. */
#define D3E_WARN_ONCE(bit, ...) do { if (!(s_d.warned & (1ull << (bit)))) { s_d.warned |= (1ull << (bit)); D3E_LOG(__VA_ARGS__); } } while (0)

extern float cellAvconfExt_display_gamma(void);

/* ---- pure helpers (no device) ------------------------------------------- */

int rsx_d3d12_blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    /* CONSTANT_ALPHA: the blend factor's alpha in every lane. The caller
     * swaps in D3D12_BLEND_ALPHA_FACTOR where the device has it, else feeds
     * (a, a, a, a) as the blend factor when no colour factor is used. */
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}

/* RPCS3 get_blend_op: ADD_SIGNED / REVERSE_ADD_SIGNED as ADD,
 * REVERSE_SUBTRACT_SIGNED as REVERSE_SUBTRACT. */
int rsx_d3d12_blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    case 0xF005: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

/* GL NEVER..ALWAYS (0x0200..0x0207) are D3D12's 1..8 in the same order. */
int rsx_d3d12_compare(uint32_t f)
{
    f |= 0x200u;
    if (f <= 0x207u) return (int)(f - 0x200u) + 1;
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

int rsx_d3d12_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;
    case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D12_STENCIL_OP_INVERT;
    case 0x8507: return D3D12_STENCIL_OP_INCR;
    case 0x8508: return D3D12_STENCIL_OP_DECR;
    default:     return D3D12_STENCIL_OP_KEEP;
    }
}

/* RSX wrap: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE, 4 BORDER, 5 CLAMP,
 * 6 MIRROR_ONCE_CLAMP_TO_EDGE, 7 MIRROR_ONCE_BORDER, 8 MIRROR_ONCE_CLAMP. */
int rsx_d3d12_wrap(uint32_t c)
{
    switch (c) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 6: case 7: case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

void rsx_d3d12_letterbox(uint32_t W, uint32_t H, uint32_t aw, uint32_t ah,
                         uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h)
{
    *x = 0; *y = 0; *w = W; *h = H;
    if (!aw || !ah || !W || !H) return;
    const unsigned long long lhs = (unsigned long long)W * ah, rhs = (unsigned long long)H * aw;
    if (lhs > rhs) {            /* window wider than the picture: pillarbox */
        const uint32_t nw = (uint32_t)(rhs / ah);
        *x = (W - nw) / 2u; *w = nw ? nw : 1u;
    } else if (lhs < rhs) {     /* taller: letterbox */
        const uint32_t nh = (uint32_t)(lhs / aw);
        *y = (H - nh) / 2u; *h = nh ? nh : 1u;
    }
}

static int d3e_clip_axis(uint32_t* sp, uint32_t* sl, uint32_t smax,
                         uint32_t* dp, uint32_t* dl, uint32_t dmax)
{
    double s0 = *sp, s1 = (double)*sp + *sl, d0 = *dp, d1 = (double)*dp + *dl;
    if (s1 <= s0 || d1 <= d0) return 0;
    const double k = (s1 - s0) / (d1 - d0);
    if (d1 > dmax) { s1 -= (d1 - dmax) * k; d1 = dmax; }
    if (s1 > smax) { d1 -= (s1 - smax) / k; s1 = smax; }
    if (d0 >= d1 || s0 >= s1) return 0;
    const uint32_t is0 = (uint32_t)(s0 + 0.5), is1 = (uint32_t)(s1 + 0.5);
    const uint32_t id0 = (uint32_t)(d0 + 0.5), id1 = (uint32_t)(d1 + 0.5);
    if (is1 <= is0 || id1 <= id0) return 0;
    *sp = is0; *sl = is1 - is0; *dp = id0; *dl = id1 - id0;
    return 1;
}

int rsx_d3d12_clip_blit(uint32_t sW, uint32_t sH, uint32_t dW, uint32_t dH,
                        uint32_t* sx, uint32_t* sy, uint32_t* sw, uint32_t* sh,
                        uint32_t* dx, uint32_t* dy, uint32_t* dw, uint32_t* dh)
{
    if (*sx >= sW || *sy >= sH || *dx >= dW || *dy >= dH) return 0;
    return d3e_clip_axis(sx, sw, sW, dx, dw, dW) && d3e_clip_axis(sy, sh, sH, dy, dh, dH);
}

/* RSX_FSR as the D3D12 / native renderers number it (1 FSR1, 2 DLAA, 3 FSR 3.1, 4 XeSS) ->
 * the RPCS3 renderer's (VKR2Upscaler.cpp: 1 FSR 3.1, 2 DLSS, 4 XeSS; 3 is invalid there).
 * FSR1 has no RSX_FSR value there: 0, and *fsr1_filter = 1 (its output scaling RSX_FILTER=fsr). */
int rsx_d3d12_fsr_to_rpcs3(int fsr, int* fsr1_filter)
{
    if (fsr1_filter) *fsr1_filter = fsr == 1;
    switch (fsr) {
    case 2: return 2;
    case 3: return 1;
    case 4: return 4;
    default: return 0;
    }
}

/* Switch the environment over to the RPCS3 renderer: R2_RSX=rpcs3 and RSX_FSR renumbered. */
void rsx_d3d12_env_to_rpcs3(void)
{
    _putenv("R2_RSX=rpcs3");
    const char* e = getenv("RSX_FSR");
    if (!e || !*e) return;
    int f1 = 0;
    const int v = rsx_d3d12_fsr_to_rpcs3(atoi(e), &f1);
    char b[32];
    snprintf(b, sizeof b, "RSX_FSR=%d", v);
    _putenv(b);
    if (f1) {
        const char* fl = getenv("RSX_FILTER");
        if (!fl || !*fl) _putenv("RSX_FILTER=fsr");
    }
}

#include "rsx_d3d12_engine_ext.inc"
/* RSX_D3D12_HELPERS_ONLY: just the helpers above (project/tests/d3d12_helpers_test.c). */
#ifndef RSX_D3D12_HELPERS_ONLY

/* ---- state --------------------------------------------------------------- */

#define D3E_MAX_OBJ       32768u
#define D3E_SRV_STENCIL   D3E_MAX_OBJ             /* + id: a depth target's stencil SRV */
#define D3E_SRV_NULL      (2u * D3E_MAX_OBJ)      /* + 0 2D, 1 cube, 2 3D, 3 2D uint */
#define D3E_SRV_CPU_COUNT (2u * D3E_MAX_OBJ + 8u)
#define D3E_RTV_SWAP      D3E_MAX_OBJ             /* + i: swap chain buffers */
#define D3E_RTV_COUNT     (D3E_MAX_OBJ + 8u)
#define D3E_DSV_SCRATCH   D3E_MAX_OBJ
#define D3E_DSV_COUNT     (D3E_MAX_OBJ + 2u)
#define D3E_GPU_DESC_HALF 262144u                 /* shader-visible CBV/SRV ring, per frame half */
#define D3E_SAMP_HEAP     2048u                   /* shader-visible sampler heap (D3D12 limit) */
#define D3E_UPLOAD_HALF   (96u << 20)             /* upload ring, per frame half */
#define D3E_BIG_UPLOAD    (16u << 20)             /* larger single uploads get their own buffer */
#define D3E_SWAP_BUFFERS  3u
#define D3E_VS_CB_BYTES   9216u                   /* VPConst: 512+3 float4, 16 uint4, 16 float4, uint4 */
#define D3E_PS_CB_BYTES   33024u                  /* PSConstants (up to 2049 float4) / RsxShadowScale (float4 index 69) */
#define D3E_MAX_QUERIES   4096u
#define D3E_QUERY_SLOTS   16384u
#define D3E_QUERY_SEGS    8u

enum { D3E_FREE = 0, D3E_TEX, D3E_COLOR, D3E_DEPTH, D3E_VIEW };
enum { D3E_ZD16 = 0, D3E_ZD24S8, D3E_ZD32, D3E_ZD32S8 };

#define D3E_SRV_STATES (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)

typedef struct d3e_obj {
    u8  kind;
    u8  dim;            /* 0 2D, 1 cube, 2 3D */
    u8  zcls;           /* depth: D3E_Z* */
    u8  has_stencil;
    u8  bc;             /* block bytes of a BC format, else 0 */
    u8  gz16;           /* depth: guest Z16 fixed point (depth bias units) */
    u8  owned;          /* a depth target's R32F snapshot: released with it */
    u8  has_srv, has_rtv, has_dsv, has_ssrv;
    u16 nviews;         /* VIEW objects whose parent this is */
    rsx_be_format fmt;
    DXGI_FORMAT rfmt;   /* resource format */
    DXGI_FORMAT vfmt;   /* SRV format */
    u32 w, h, d, mips, layers, bpp;
    u32 parent;         /* view: the object whose resource it reads */
    u32 remap, rsx_fmt;
    u32 dsnap;          /* depth: its R32F snapshot texture */
    u32 snorm_view, srgb_view;
    UINT map;           /* view: its SRV's component mapping */
    u32 rsc_gen;        /* render scale: drawn in the corner of the window rsc_gen (rsx_d3d12_engine_up.inc) */
    float rsc_rs;       /* render scale: a corner region snapshot's scale, else 0 */
    ID3D12Resource* res;
    D3D12_RESOURCE_STATES st;
} d3e_obj;

typedef struct d3e_variant {
    u64 key;
    ID3D12PipelineState* pso;
    u8  pending;        /* queued on the compiler pool (async) */
} d3e_variant;

#define D3E_PIPE_VARIANTS 12u
typedef struct d3e_pipe {
    u8  used;
    u8  uses_const_color, uses_const_alpha;
    ID3DBlob* vs;
    ID3DBlob* ps;
    rsx_be_render_state rs;
    D3D12_INPUT_ELEMENT_DESC il[16];
    u32 stride;
    rsx_be_format rt_fmt;
    u32 rt_count;
    u32 ps_cube, ps_3d, vs_cube, vs_3d;
    u32 nvar, next_evict;
    u32 nil;            /* input elements in il */
    d3e_variant var[D3E_PIPE_VARIANTS];
    /* async compile (rsx_d3d12_engine_async.inc) */
    u8  pending, failed;  /* shaders queued / FXC failed: draws skipped */
    u32 gen;              /* identifies this use of the slot */
    u32 prewarm;          /* topologies asked for while pending */
    struct d3e_job* job;  /* the queued compile, while pending */
    const char* gsio;     /* interned point / line expansion GS I/O (rsx_d3d12_engine_gs.inc), NULL: none */
} d3e_pipe;

typedef struct d3e_dead { IUnknown* p; u64 fence; } d3e_dead;

typedef struct d3e_query {
    u8  active, used;
    u32 nseg;
    u32 seg[D3E_QUERY_SEGS];     /* physical query slots, + 1 */
    u64 seg_fence[D3E_QUERY_SEGS];
    u64 accum;
} d3e_query;

static struct {
    int  up;
    HWND hwnd;
    u32  width, height;
    u64  warned;

    IDXGIFactory4*       factory;
    IDXGIAdapter1*       adapter;
    ID3D12Device*        dev;
    ID3D12Device2*       dev2;
    ID3D12CommandQueue*  queue;
    ID3D12CommandAllocator* alloc[2];
    ID3D12GraphicsCommandList* cl;
    ID3D12GraphicsCommandList1* cl1;     /* OMSetDepthBounds */
    rsx_d3d12_cl8* cl8;                  /* OMSetFrontAndBackStencilRef */
    ID3D12Fence* fence;
    HANDLE fence_event;
    u64  fence_val;                       /* last value signalled */
    u64  half_fence[2];
    u32  half;
    u32  vendor;

    /* capabilities */
    int  depth_bounds_ok, stencil_ref_ok, alpha_factor_ok, sep_stencil_ok, tearing_ok;
    int  blendable_r32f, blendable_rgba32f;

    /* descriptor heaps */
    ID3D12DescriptorHeap *srv_cpu, *rtv, *dsv, *srv_gpu, *samp_gpu;
    u32  srv_inc, rtv_inc, dsv_inc, samp_inc;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu0, rtv0, dsv0, srv_gpu_cpu0, samp_cpu0;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu0, samp_gpu0;
    u32  desc_off;                        /* within the current half */
    u32  samp_off;                        /* sampler heap: linear, reset on full */

    /* upload ring */
    ID3D12Resource* upload;
    u8*  upload_cpu;
    D3D12_GPU_VIRTUAL_ADDRESS upload_gpu;
    u64  upload_off;                      /* within the current half */
    u32  ring_epoch;                      /* bumped whenever upload_off resets */

    /* readback */
    ID3D12Resource* rb;
    u64  rb_size;

    /* root signatures and the internal passes */
    ID3D12RootSignature* rs_main;
    ID3D12RootSignature* rs_pass;
    ID3DBlob* pass_vs;
    ID3DBlob* pass_ps[12];               /* D3E_PS_COUNT (rsx_d3d12_engine_pass.inc) */

    /* zero vertex for attributes the layout does not carry */
    ID3D12Resource* zero_vb;

    /* swap chain */
    IDXGISwapChain3* swap;
    ID3D12Resource* back[D3E_SWAP_BUFFERS];
    D3D12_RESOURCE_STATES back_st[D3E_SWAP_BUFFERS];
    u32  swap_w, swap_h;
    u32  presents;
    u32  last_center;

    /* scratch depth (D24S8) */
    ID3D12Resource* scratch;
    D3D12_RESOURCE_STATES scratch_st;
    u32  scratch_w, scratch_h;

    /* occlusion */
    ID3D12QueryHeap* qheap;
    ID3D12Resource* qrb;
    u64* qrb_cpu;
    u64  qslot_fence[D3E_QUERY_SLOTS];
    u8   qslot_used[D3E_QUERY_SLOTS];
    u32  qslot_next;
    u32  qactive_slot;                    /* + 1 while a query is open */

    /* recording state, reset when the list is */
    ID3D12RootSignature* cur_rs;
    ID3D12PipelineState* cur_pso;
    u32  om_rt[RSX_BE_MAX_COLOR_TARGETS], om_nrt, om_depth;   /* bound OM (depth 0xFFFFFFFF scratch) */
    int  om_valid;
} s_d;

static d3e_obj  s_obj[D3E_MAX_OBJ];
static u32      s_obj_hint = 1;
static d3e_pipe* s_pipe;
static u32      s_pipe_cap;
static d3e_dead* s_dead;
static u32      s_dead_n, s_dead_cap;
static d3e_query s_q[D3E_MAX_QUERIES];

/* The engine's per-draw state (the vtable's set_* / bind_* calls). */
static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
    u32 pipeline;
    u8  vs_data[D3E_VS_CB_BYTES];
    u8  ps_data[D3E_PS_CB_BYTES + 256u];
    u32 vs_bytes, ps_bytes;
    u32 ps_extra;                         /* render scale: fp_rscale block after ps_bytes */
    int vs_dirty, ps_dirty;
    D3D12_GPU_VIRTUAL_ADDRESS vs_va, ps_va;
    u32 tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32 tex_mask;
    u32 vtex[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
    u32 vtex_mask;
    float vp[4]; int have_vp;
    float zr[2]; int have_zr;
    u32 sc[4]; int have_sc;
    u32 sref, bref;                       /* bref: 0x100 | ref when set */
    float dbmin, dbmax;
    float blend[4];
    float dbias[3];                       /* enable, constant, slope */
    float line_width;
} s_e;

/* Render scale < 1 for the temporal upscaler (rsx_d3d12_engine_up.inc). */
#define D3E_UP_LIST 1024u
#define D3E_PS_EXTRA 80u                  /* fp_rscale[5] after the PS constants */
static struct {
    int   on;                             /* set before init: root parameter 7, dynamic fp_rscale */
    float u, bias;
    int   open;                           /* the scene window */
    u32   gen;                            /* d3e_obj.rsc_gen of this window's marks */
    u32   ww, wh;                         /* its colour size */
    u32   list[D3E_UP_LIST], n;           /* marked targets */
    u32   twin;                           /* depth copy source for the in-place rescales */
    int   scaled;                         /* the draw being recorded */
    float uvk[16];
} s_up = { 0, 1.0f, 0.0f, 0, 1u };

/* ---- small utilities ----------------------------------------------------- */

static u64 d3e_hash(const void* p, size_t n, u64 h)
{
    const u8* b = (const u8*)p;
    if (!h) h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* In-memory keys only (never persisted, unlike d3e_hash): 8 bytes a step. */
static u64 d3e_hash_fast(const void* p, size_t n, u64 h)
{
    const u8* b = (const u8*)p;
    h ^= 0x9E3779B97F4A7C15ull + (u64)n;
    while (n >= 8u) {
        u64 w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
        b += 8; n -= 8u;
    }
    if (n) {
        u64 w = 0;
        memcpy(&w, b, n);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33;
    return h;
}

static int d3e_env_on(const char* name, int def)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

static void d3e_release(void* p)
{
    if (p) IUnknown_Release((IUnknown*)p);
}

static D3D12_CPU_DESCRIPTOR_HANDLE d3e_srv_cpu(u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s_d.srv_cpu0;
    h.ptr += (SIZE_T)i * s_d.srv_inc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE d3e_rtv_cpu(u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s_d.rtv0;
    h.ptr += (SIZE_T)i * s_d.rtv_inc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE d3e_dsv_cpu(u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s_d.dsv0;
    h.ptr += (SIZE_T)i * s_d.dsv_inc;
    return h;
}

static d3e_obj* d3e_get(u32 h, int kind)
{
    if (!h || h >= D3E_MAX_OBJ) return NULL;
    d3e_obj* o = &s_obj[h];
    if (o->kind == D3E_FREE || (kind && o->kind != kind)) return NULL;
    return o;
}

/* The object that owns the resource (a view reads its parent's). */
static d3e_obj* d3e_base(d3e_obj* o)
{
    if (o && o->kind == D3E_VIEW) {
        d3e_obj* p = d3e_get(o->parent, 0);
        return p && p->kind != D3E_VIEW ? p : NULL;
    }
    return o;
}

static u32 d3e_alloc_obj(void)
{
    for (u32 n = 0; n < D3E_MAX_OBJ - 1u; n++) {
        u32 i = s_obj_hint + n;
        if (i >= D3E_MAX_OBJ) i -= D3E_MAX_OBJ - 1u;
        if (s_obj[i].kind == D3E_FREE) {
            s_obj_hint = i + 1u >= D3E_MAX_OBJ ? 1u : i + 1u;
            memset(&s_obj[i], 0, sizeof s_obj[i]);
            return i;
        }
    }
    D3E_WARN_ONCE(0, "engine: object table full (%u)\n", D3E_MAX_OBJ);
    return 0;
}

/* ---- submission, fences, deferred release ------------------------------- */

static void d3e_wait_fence(u64 v)
{
    if (!v || ID3D12Fence_GetCompletedValue(s_d.fence) >= v) return;
    ID3D12Fence_SetEventOnCompletion(s_d.fence, v, s_d.fence_event);
    WaitForSingleObject(s_d.fence_event, 10000);
    if (ID3D12Fence_GetCompletedValue(s_d.fence) < v) {
        const HRESULT r = ID3D12Device_GetDeviceRemovedReason(s_d.dev);
        D3E_WARN_ONCE(1, "fence wait timed out (device removed reason 0x%08lX)\n", (unsigned long)r);
    }
}

/* Keep `p` alive until the GPU has passed everything recorded so far. */
static void d3e_defer_release(void* p)
{
    if (!p) return;
    if (s_dead_n == s_dead_cap) {
        const u32 nc = s_dead_cap ? s_dead_cap * 2u : 256u;
        d3e_dead* nd = (d3e_dead*)realloc(s_dead, nc * sizeof *nd);
        if (!nd) { D3E_WARN_ONCE(2, "deferred release list: out of memory (leaking)\n"); return; }
        s_dead = nd; s_dead_cap = nc;
    }
    s_dead[s_dead_n].p = (IUnknown*)p;
    s_dead[s_dead_n].fence = s_d.fence_val + 1u;
    s_dead_n++;
}

static void d3e_collect_dead(void)
{
    if (!s_dead_n) return;
    const u64 done = ID3D12Fence_GetCompletedValue(s_d.fence);
    u32 k = 0;
    for (u32 i = 0; i < s_dead_n; i++) {
        if (s_dead[i].fence <= done) IUnknown_Release(s_dead[i].p);
        else s_dead[k++] = s_dead[i];
    }
    s_dead_n = k;
}

/* What d3e_draw last bound with rs_main: root arguments and dynamic state.
 * Valid while rs_main stays bound -- every other pass binds its own root
 * signature first (cur_rs), and a provider (upscaler / FG) that records into
 * the list is followed by d3e_list_begin_state, as for cur_pso.
 *   RSX_D3D12_STATE_CACHE=0   set everything on every draw (the old way) */
static struct {
    int valid;
    D3D12_GPU_VIRTUAL_ADDRESS cbv[2];
    UINT64 samp[2];
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;
    u32 topo;                     /* index + 1 */
    u32 sref;                     /* 0xFFFFFFFF: front / back set separately */
    float bf[4];
    D3D12_GPU_VIRTUAL_ADDRESS vb1;
    /* the last SRV table: reused while the same sources are drawn in one ring epoch */
    u32 tab_src[20];
    D3D12_GPU_DESCRIPTOR_HANDLE tab;
    u32 tab_epoch, tab_gen;
    UINT64 tab_bound;             /* root parameters 2 / 3 */
    ID3D12Resource* zero_res;
    D3D12_GPU_VIRTUAL_ADDRESS zero_va;
} s_rc;
static u32 s_srv_gen = 1;          /* bumped by every write to the CPU SRV heap (d3e_make_srv) */

static int d3e_state_cache(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_STATE_CACHE", 1);
    return v;
}

static void d3e_list_begin_state(void)
{
    ID3D12DescriptorHeap* heaps[2] = { s_d.srv_gpu, s_d.samp_gpu };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(s_d.cl, 2, heaps);
    s_d.cur_rs = NULL;
    s_d.cur_pso = NULL;
    s_d.om_valid = 0;
    s_rc.valid = 0;
}

static void d3e_query_close_open(void);
static void d3e_query_reopen(void);

/* Close and execute the list. wait: block until the GPU is idle, then every
 * ring of the current half is free again. The list reopens either way. */
static void d3e_submit(int wait)
{
    if (!s_d.cl) return;
    d3e_query_close_open();
    HRESULT hr = ID3D12GraphicsCommandList_Close(s_d.cl);
    if (FAILED(hr)) D3E_WARN_ONCE(3, "command list Close failed 0x%08lX\n", (unsigned long)hr);
    else {
        ID3D12CommandList* l = (ID3D12CommandList*)s_d.cl;
        ID3D12CommandQueue_ExecuteCommandLists(s_d.queue, 1, &l);
    }
    s_d.fence_val++;
    ID3D12CommandQueue_Signal(s_d.queue, s_d.fence, s_d.fence_val);
    s_d.half_fence[s_d.half] = s_d.fence_val;
    if (wait) {
        d3e_wait_fence(s_d.fence_val);
        ID3D12CommandAllocator_Reset(s_d.alloc[s_d.half]);
        s_d.desc_off = 0;
        s_d.upload_off = 0; s_d.ring_epoch++;
    }
    ID3D12GraphicsCommandList_Reset(s_d.cl, s_d.alloc[s_d.half], NULL);
    d3e_list_begin_state();
    d3e_collect_dead();
    d3e_query_reopen();
}

/* End of a frame: submit, then move to the other half once the frame that
 * last used it has finished. */
static void d3e_frame_end(void)
{
    d3e_query_close_open();
    HRESULT hr = ID3D12GraphicsCommandList_Close(s_d.cl);
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* l = (ID3D12CommandList*)s_d.cl;
        ID3D12CommandQueue_ExecuteCommandLists(s_d.queue, 1, &l);
    } else D3E_WARN_ONCE(3, "command list Close failed 0x%08lX\n", (unsigned long)hr);
    s_d.fence_val++;
    ID3D12CommandQueue_Signal(s_d.queue, s_d.fence, s_d.fence_val);
    s_d.half_fence[s_d.half] = s_d.fence_val;
}

static void d3e_frame_begin_next(void)
{
    s_d.half ^= 1u;
    d3e_wait_fence(s_d.half_fence[s_d.half]);
    ID3D12CommandAllocator_Reset(s_d.alloc[s_d.half]);
    s_d.desc_off = 0;
    s_d.upload_off = 0; s_d.ring_epoch++;
    ID3D12GraphicsCommandList_Reset(s_d.cl, s_d.alloc[s_d.half], NULL);
    d3e_list_begin_state();
    d3e_collect_dead();
    d3e_query_reopen();
}

/* ---- rings --------------------------------------------------------------- */

/* Make room for `bytes` of upload memory and `descs` shader-visible
 * descriptors before any of them is taken, so one draw's allocations never
 * straddle a flush. */
static void d3e_reserve(u64 bytes, u32 descs)
{
    if (s_d.upload_off + bytes + 1024u > D3E_UPLOAD_HALF ||
        s_d.desc_off + descs > D3E_GPU_DESC_HALF)
        d3e_submit(1);
}

static void* d3e_upload_alloc(u64 bytes, u64 align, D3D12_GPU_VIRTUAL_ADDRESS* va, ID3D12Resource** buf, u64* buf_off)
{
    u64 off = (s_d.upload_off + align - 1u) & ~(align - 1u);
    if (off + bytes > D3E_UPLOAD_HALF) {
        d3e_submit(1);
        off = 0;
        if (bytes > D3E_UPLOAD_HALF) return NULL;
    }
    s_d.upload_off = off + bytes;
    const u64 abs_off = (u64)s_d.half * D3E_UPLOAD_HALF + off;
    if (va) *va = s_d.upload_gpu + abs_off;
    if (buf) *buf = s_d.upload;
    if (buf_off) *buf_off = abs_off;
    return s_d.upload_cpu + abs_off;
}

static ID3D12Resource* d3e_buffer(u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = { heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1,
                               DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(s_d.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, st, NULL,
                                                     &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    return r;
}

/* Upload memory for one texture copy: the ring, or a buffer of its own
 * (released once the GPU is past it) for a large one. */
static u8* d3e_upload_staging(u64 bytes, ID3D12Resource** buf, u64* off)
{
    if (bytes > D3E_BIG_UPLOAD) {
        ID3D12Resource* b = d3e_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!b) return NULL;
        void* p = NULL;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(ID3D12Resource_Map(b, 0, &none, &p))) { ID3D12Resource_Release(b); return NULL; }
        *buf = b; *off = 0;
        d3e_defer_release(b);
        return (u8*)p;
    }
    return (u8*)d3e_upload_alloc(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, NULL, buf, off);
}

/* `n` contiguous shader-visible CBV/SRV descriptors (caller reserved). */
static u32 d3e_desc_alloc(u32 n, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (s_d.desc_off + n > D3E_GPU_DESC_HALF) d3e_submit(1);
    const u32 i = s_d.half * D3E_GPU_DESC_HALF + s_d.desc_off;
    s_d.desc_off += n;
    cpu->ptr = s_d.srv_gpu_cpu0.ptr + (SIZE_T)i * s_d.srv_inc;
    gpu->ptr = s_d.srv_gpu0.ptr + (UINT64)i * s_d.srv_inc;
    return i;
}

/* ---- sampler tables ------------------------------------------------------ */

typedef struct d3e_samp_tab { u64 key; u32 off; u32 n; } d3e_samp_tab;
#define D3E_SAMP_TABS 1024u
static d3e_samp_tab s_stab[D3E_SAMP_TABS];
static u32 s_stab_n;
/* the last table of each size (pixel / vertex units): consecutive draws mostly repeat it */
static struct { u32 n, off; D3D12_SAMPLER_DESC sd[RSX_BE_MAX_TEXTURES]; } s_stab_last[2];

static void d3e_stab_reset(void)
{
    memset(s_stab, 0, sizeof s_stab);
    s_stab_n = 0;
    s_d.samp_off = 0;
    s_stab_last[0].n = s_stab_last[1].n = 0;
}

/* Make room for `nsamp` samplers in `ntab` new tables before any table of a
 * draw is taken, so a heap reset never strands a handle already returned for
 * the same draw. The reset submits, which bumps ring_epoch: callers capture
 * the epoch first and retry on a change. */
static void d3e_sampler_reserve(u32 nsamp, u32 ntab)
{
    if (s_d.samp_off + nsamp > D3E_SAMP_HEAP || s_stab_n + ntab >= D3E_SAMP_TABS * 3u / 4u) {
        d3e_submit(1);
        d3e_stab_reset();
    }
}

/* A table of `n` samplers in the shader-visible heap, shared by every draw
 * that uses the same set. When the heap is full the GPU is drained and the
 * cache starts over. */
static D3D12_GPU_DESCRIPTOR_HANDLE d3e_sampler_table(const D3D12_SAMPLER_DESC* sd, u32 n)
{
    const u32 mi = n == RSX_BE_MAX_TEXTURES ? 0u : 1u;
    const int memo = n <= RSX_BE_MAX_TEXTURES;
    if (memo && s_stab_last[mi].n == n && !memcmp(s_stab_last[mi].sd, sd, sizeof *sd * n)) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = s_d.samp_gpu0;
        g.ptr += (UINT64)s_stab_last[mi].off * s_d.samp_inc;
        return g;
    }
    const u64 key = d3e_hash_fast(sd, sizeof *sd * n, (u64)n * 0x9E3779B97F4A7C15ull);
    u32 slot = (u32)(key % D3E_SAMP_TABS);
    for (u32 p = 0; p < D3E_SAMP_TABS; p++) {
        d3e_samp_tab* t = &s_stab[(slot + p) % D3E_SAMP_TABS];
        if (!t->n) break;
        if (t->key == key && t->n == n) {
            if (memo) {
                memcpy(s_stab_last[mi].sd, sd, sizeof *sd * n);
                s_stab_last[mi].off = t->off;
                s_stab_last[mi].n = n;
            }
            D3D12_GPU_DESCRIPTOR_HANDLE g = s_d.samp_gpu0;
            g.ptr += (UINT64)t->off * s_d.samp_inc;
            return g;
        }
    }
    if (s_d.samp_off + n > D3E_SAMP_HEAP || s_stab_n >= D3E_SAMP_TABS * 3u / 4u) {
        d3e_submit(1);
        d3e_stab_reset();
    }
    const u32 off = s_d.samp_off;
    if (memo) {
        memcpy(s_stab_last[mi].sd, sd, sizeof *sd * n);
        s_stab_last[mi].off = off;
        s_stab_last[mi].n = n;
    }
    s_d.samp_off += n;
    for (u32 i = 0; i < n; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = s_d.samp_cpu0;
        c.ptr += (SIZE_T)(off + i) * s_d.samp_inc;
        ID3D12Device_CreateSampler(s_d.dev, &sd[i], c);
    }
    for (u32 p = 0; p < D3E_SAMP_TABS; p++) {
        d3e_samp_tab* t = &s_stab[(slot + p) % D3E_SAMP_TABS];
        if (t->n) continue;
        t->key = key; t->off = off; t->n = n;
        s_stab_n++;
        break;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE g = s_d.samp_gpu0;
    g.ptr += (UINT64)off * s_d.samp_inc;
    return g;
}

/* rsx_be_sampler_desc -> D3D12_SAMPLER_DESC, as vk_eng_sampler does it. */
static void d3e_sampler_desc(const rsx_be_sampler_desc* s, int filterable, D3D12_SAMPLER_DESC* o)
{
    memset(o, 0, sizeof *o);
    int minl = s->min_linear, magl = s->mag_linear, mipl = s->mip_present && s->mip_linear;
    if (!filterable) minl = magl = mipl = 0;
    const int aniso = filterable && s->max_aniso > 1;
    if (aniso) {
        o->Filter = D3D12_FILTER_ANISOTROPIC;
        o->MaxAnisotropy = s->max_aniso > 16 ? 16u : s->max_aniso;
    } else {
        o->Filter = D3D12_ENCODE_BASIC_FILTER(minl ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                              magl ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                              mipl ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                              D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        o->MaxAnisotropy = 1;
    }
    o->AddressU = (D3D12_TEXTURE_ADDRESS_MODE)rsx_d3d12_wrap(s->wrap_s);
    o->AddressV = (D3D12_TEXTURE_ADDRESS_MODE)rsx_d3d12_wrap(s->wrap_t);
    o->AddressW = (D3D12_TEXTURE_ADDRESS_MODE)rsx_d3d12_wrap(s->wrap_r);
    float bias = 0.0f;
    if (s->mip_present) {
        bias = s->lod_bias;
        if (!s->mip_linear) bias = floorf(bias * 2.0f + 0.5f) * 0.5f;
        if (bias > 15.0f) bias = 15.0f;
        if (bias < -15.0f) bias = -15.0f;
        if (!(bias == bias)) bias = 0.0f;
    }
    o->MipLODBias = bias;
    o->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    for (int i = 0; i < 4; i++) {
        float c = s->border_rgba[i];
        if (!(c == c)) c = 0.0f;
        o->BorderColor[i] = c;
    }
    float mn = s->min_lod, mx = s->mip_present ? s->max_lod : s->min_lod;
    if (!(mn == mn) || mn < 0.0f) mn = 0.0f;
    if (!(mx == mx)) mx = mn;
    if (mx < mn) mx = mn;
    o->MinLOD = mn;
    o->MaxLOD = mx;
}

/* ---- barriers ------------------------------------------------------------ */

/* d3e_bar_begin .. d3e_bar_end: transitions are collected and issued with one
 * ResourceBarrier call (nothing else may be recorded in between). A resource
 * already in the batch flushes it first, so per-resource order is kept.
 *   RSX_D3D12_BARRIER_BATCH=0   one call per transition (the old way) */
#define D3E_BAR_BATCH 32u
static struct { int on; u32 n; D3D12_RESOURCE_BARRIER b[D3E_BAR_BATCH]; } s_bar;

static void d3e_bar_flush(void)
{
    if (!s_bar.n) return;
    ID3D12GraphicsCommandList_ResourceBarrier(s_d.cl, s_bar.n, s_bar.b);
    s_bar.n = 0;
}
static void d3e_bar_begin(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_BARRIER_BATCH", 1);
    s_bar.on = v;
}
static void d3e_bar_end(void)
{
    d3e_bar_flush();
    s_bar.on = 0;
}

static void d3e_barrier_res(ID3D12Resource* r, D3D12_RESOURCE_STATES* cur, D3D12_RESOURCE_STATES ns)
{
    if (!r || *cur == ns) return;
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = *cur;
    b.Transition.StateAfter = ns;
    if (s_bar.on) {
        for (u32 i = 0; i < s_bar.n; i++)
            if (s_bar.b[i].Transition.pResource == r) { d3e_bar_flush(); break; }
        if (s_bar.n == D3E_BAR_BATCH) d3e_bar_flush();
        s_bar.b[s_bar.n++] = b;
    } else {
        ID3D12GraphicsCommandList_ResourceBarrier(s_d.cl, 1, &b);
    }
    *cur = ns;
    s_d.om_valid = 0;     /* an OM target may have left RENDER_TARGET */
}

static void d3e_trans(d3e_obj* o, D3D12_RESOURCE_STATES ns)
{
    o = d3e_base(o);
    if (!o || !o->res) return;
    d3e_barrier_res(o->res, &o->st, ns);
}

/* The state an object is sampled in. */
static D3D12_RESOURCE_STATES d3e_srv_state(const d3e_obj* o)
{
    return o->kind == D3E_DEPTH ? (D3E_SRV_STATES | D3D12_RESOURCE_STATE_DEPTH_READ) : D3E_SRV_STATES;
}

/* ---- formats ------------------------------------------------------------- */

/* Texture formats: the resource format (TYPELESS where an SNORM or sRGB view
 * may be wanted), the plain view format, and bytes per pixel / per block. */
static int d3e_tex_format(rsx_be_format f, DXGI_FORMAT* res, DXGI_FORMAT* view, u32* bpp, u32* block)
{
    *bpp = 0; *block = 0;
    switch (f) {
    case RSX_BE_FMT_R8:       *res = DXGI_FORMAT_R8_TYPELESS;  *view = DXGI_FORMAT_R8_UNORM; *bpp = 1; return 1;
    case RSX_BE_FMT_R8G8:     *res = DXGI_FORMAT_R8G8_TYPELESS; *view = DXGI_FORMAT_R8G8_UNORM; *bpp = 2; return 1;
    case RSX_BE_FMT_R8G8B8A8: *res = DXGI_FORMAT_R8G8B8A8_TYPELESS; *view = DXGI_FORMAT_R8G8B8A8_UNORM; *bpp = 4; return 1;
    case RSX_BE_FMT_BC1:      *res = DXGI_FORMAT_BC1_TYPELESS; *view = DXGI_FORMAT_BC1_UNORM; *block = 8; return 1;
    case RSX_BE_FMT_BC2:      *res = DXGI_FORMAT_BC2_TYPELESS; *view = DXGI_FORMAT_BC2_UNORM; *block = 16; return 1;
    case RSX_BE_FMT_BC3:      *res = DXGI_FORMAT_BC3_TYPELESS; *view = DXGI_FORMAT_BC3_UNORM; *block = 16; return 1;
    case RSX_BE_FMT_R16:      *res = DXGI_FORMAT_R16_TYPELESS; *view = DXGI_FORMAT_R16_UNORM; *bpp = 2; return 1;
    case RSX_BE_FMT_R16G16:   *res = DXGI_FORMAT_R16G16_TYPELESS; *view = DXGI_FORMAT_R16G16_UNORM; *bpp = 4; return 1;
    case RSX_BE_FMT_R16G16F:  *res = DXGI_FORMAT_R16G16_FLOAT; *view = DXGI_FORMAT_R16G16_FLOAT; *bpp = 4; return 1;
    case RSX_BE_FMT_R16G16B16A16F: *res = DXGI_FORMAT_R16G16B16A16_FLOAT; *view = *res; *bpp = 8; return 1;
    case RSX_BE_FMT_R32F:     *res = DXGI_FORMAT_R32_FLOAT; *view = *res; *bpp = 4; return 1;
    case RSX_BE_FMT_R32G32B32A32F: *res = DXGI_FORMAT_R32G32B32A32_FLOAT; *view = *res; *bpp = 16; return 1;
    /* colour targets only (d3e_be_color_format_supported); same bit layouts as the RSX's */
    case RSX_BE_FMT_R5G6B5:   *res = DXGI_FORMAT_B5G6R5_UNORM; *view = *res; *bpp = 2; return 1;
    case RSX_BE_FMT_A1R5G5B5: *res = DXGI_FORMAT_B5G5R5A1_UNORM; *view = *res; *bpp = 2; return 1;
    default: return 0;
    }
}

/* Colour targets: typed resources (they are render targets). */
static int d3e_rt_format(rsx_be_format f, DXGI_FORMAT* fmt, u32* bpp)
{
    DXGI_FORMAT res, view;
    u32 block;
    if (!d3e_tex_format(f, &res, &view, bpp, &block) || block) return 0;
    *fmt = view;
    return 1;
}

static int d3e_rt_blendable(rsx_be_format f)
{
    if (f == RSX_BE_FMT_R32F) return s_d.blendable_r32f;
    if (f == RSX_BE_FMT_R32G32B32A32F) return s_d.blendable_rgba32f;
    return 1;
}

static int d3e_filterable(DXGI_FORMAT f)
{
    /* FL11 guarantees filtering for everything here but 32-bit float, where
     * it is optional; check once. */
    static int r32 = -1, rgba32 = -1;
    if (f != DXGI_FORMAT_R32_FLOAT && f != DXGI_FORMAT_R32G32B32A32_FLOAT &&
        f != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) return 1;
    int* c = f == DXGI_FORMAT_R32G32B32A32_FLOAT ? &rgba32 : &r32;
    if (*c < 0) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT : f,
                                                 D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        *c = SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
             (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
    }
    return *c;
}

static DXGI_FORMAT d3e_dsv_format(u32 zcls)
{
    switch (zcls) {
    case D3E_ZD16:   return DXGI_FORMAT_D16_UNORM;
    case D3E_ZD32:   return DXGI_FORMAT_D32_FLOAT;
    case D3E_ZD32S8: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:         return DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
}
static DXGI_FORMAT d3e_depth_res_format(u32 zcls)
{
    switch (zcls) {
    case D3E_ZD16:   return DXGI_FORMAT_R16_TYPELESS;
    case D3E_ZD32:   return DXGI_FORMAT_R32_TYPELESS;
    case D3E_ZD32S8: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:         return DXGI_FORMAT_R24G8_TYPELESS;
    }
}
static DXGI_FORMAT d3e_depth_srv_format(u32 zcls)
{
    switch (zcls) {
    case D3E_ZD16:   return DXGI_FORMAT_R16_UNORM;
    case D3E_ZD32:   return DXGI_FORMAT_R32_FLOAT;
    case D3E_ZD32S8: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:         return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
}
static int d3e_zhost_kind(u32 zcls)
{
    return zcls == D3E_ZD16 ? RSX_VK_ZHOST_UNORM16 : zcls == D3E_ZD24S8 ? RSX_VK_ZHOST_UNORM24 : RSX_VK_ZHOST_FLOAT;
}

/* TEXTURE_CONTROL1's crossbar as a D3D12 component mapping. */
static UINT d3e_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case 1: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case 2: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case 3: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
    case RSX_REMAP_ONE: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    default: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    }
}
static UINT d3e_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(d3e_remap_sel(sel[1]), d3e_remap_sel(sel[2]),
                                                   d3e_remap_sel(sel[3]), d3e_remap_sel(sel[0]));
}

/* ---- descriptors ----------------------------------------------------------- */

static void d3e_make_srv(u32 slot, ID3D12Resource* r, DXGI_FORMAT f, u32 dim, u32 mips, UINT map, UINT plane)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = f;
    sd.Shader4ComponentMapping = map;
    if (dim == 1) {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = mips ? mips : 1;
    } else if (dim == 2) {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = mips ? mips : 1;
    } else {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = mips ? mips : 1;
        sd.Texture2D.PlaneSlice = plane;
    }
    ID3D12Device_CreateShaderResourceView(s_d.dev, r, &sd, d3e_srv_cpu(slot));
    s_srv_gen++;
}

static void d3e_null_srvs(void)
{
    d3e_make_srv(D3E_SRV_NULL + 0, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
    d3e_make_srv(D3E_SRV_NULL + 1, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
    d3e_make_srv(D3E_SRV_NULL + 2, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
    d3e_make_srv(D3E_SRV_NULL + 3, NULL, DXGI_FORMAT_R8_UINT, 0, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
}

/* ---- object creation ------------------------------------------------------ */

static ID3D12Resource* d3e_texture_resource(D3D12_RESOURCE_DIMENSION dim, DXGI_FORMAT f, u32 w, u32 h,
                                            u32 depth_or_layers, u32 mips, D3D12_RESOURCE_FLAGS flags,
                                            D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.Dimension = dim;
    rd.Width = w; rd.Height = h;
    rd.DepthOrArraySize = (UINT16)(depth_or_layers ? depth_or_layers : 1);
    rd.MipLevels = (UINT16)(mips ? mips : 1);
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    ID3D12Resource* r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(s_d.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, st, cv,
                                                      &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr)) {
        D3E_LOG("CreateCommittedResource %ux%ux%u fmt %d mips %u failed 0x%08lX\n",
                w, h, depth_or_layers, (int)f, mips, (unsigned long)hr);
        return NULL;
    }
    return r;
}

static u32 d3e_max_mips(u32 w, u32 h, u32 d)
{
    u32 m = 1, s = w > h ? w : h;
    if (d > s) s = d;
    while (s > 1u) { s >>= 1; m++; }
    return m;
}

static u32 d3e_tex_new(rsx_be_format fmt, u32 w, u32 h, u32 depth, u32 mips, u32 faces,
                       u32 remap, u32 rsx_fmt, int vol)
{
    DXGI_FORMAT rf, vf;
    u32 bpp, block;
    if (!d3e_tex_format(fmt, &rf, &vf, &bpp, &block) || !w || !h) return 0;
    if (w > 16384u || h > 16384u || (vol && (depth > 2048u || !depth))) return 0;
    if (block && ((w & 3u) || (h & 3u))) {
        /* D3D12 wants a BC texture's top level in whole blocks. */
        D3E_WARN_ONCE(4, "engine: a %ux%u BC texture is not a multiple of 4: left to the engine\n", w, h);
        return 0;
    }
    const int cube = !vol && faces == 6u;
    if (!mips) mips = 1;
    const u32 mm = d3e_max_mips(w, h, vol ? depth : 1u);
    if (mips > mm) mips = mm;
    if (rsx_texture_legacy()) rf = vf;
    const u32 hd = d3e_alloc_obj();
    if (!hd) return 0;
    d3e_obj* o = &s_obj[hd];
    o->res = d3e_texture_resource(vol ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  rf, w, h, vol ? depth : (cube ? 6u : 1u), mips, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!o->res) return 0;
    o->kind = D3E_TEX;
    o->st = D3D12_RESOURCE_STATE_COPY_DEST;
    o->dim = vol ? 2 : cube ? 1 : 0;
    o->fmt = fmt; o->rfmt = rf; o->vfmt = vf;
    o->w = w; o->h = h; o->d = vol ? depth : 1u; o->mips = mips; o->layers = cube ? 6u : 1u;
    o->bpp = bpp; o->bc = (u8)block;
    o->remap = remap; o->rsx_fmt = rsx_fmt;
    d3e_make_srv(hd, o->res, vf, o->dim, mips, d3e_remap(remap, rsx_fmt), 0);
    o->has_srv = 1;
    return hd;
}

/* Copy rows into one subresource (or a z slice of a 3D one) at (x, y). */
static void d3e_upload_sub(d3e_obj* o, u32 sub, u32 x, u32 y, u32 z, u32 w, u32 h,
                           const void* src, u32 row_bytes, u32 rows)
{
    if (!o || !o->res || !src || !w || !h) return;
    const u32 bw = o->bc ? (w + 3u) / 4u : w;
    const u32 bh = o->bc ? (h + 3u) / 4u : h;
    const u32 tight = o->bc ? bw * o->bc : w * o->bpp;
    if (!tight) return;
    if (rows > bh) rows = bh;
    if (!rows) return;
    const u32 pitch = (tight + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const u64 bytes = (u64)pitch * bh;
    ID3D12Resource* buf = NULL;
    u64 off = 0;
    u8* dst = d3e_upload_staging(bytes, &buf, &off);
    if (!dst) { D3E_WARN_ONCE(5, "upload of %ux%u failed (staging)\n", w, h); return; }
    const u32 n = tight < row_bytes ? tight : row_bytes;
    for (u32 r = 0; r < bh; r++) {
        u8* d = dst + (size_t)r * pitch;
        if (r < rows) {
            memcpy(d, (const u8*)src + (size_t)r * row_bytes, n);
            if (n < tight) memset(d + n, 0, tight - n);
        } else memset(d, 0, tight);
    }
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = o->res;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = sub;
    sl.pResource = buf;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = off;
    sl.PlacedFootprint.Footprint.Format = o->rfmt;
    sl.PlacedFootprint.Footprint.Width = o->bc ? bw * 4u : w;
    sl.PlacedFootprint.Footprint.Height = o->bc ? bh * 4u : h;
    sl.PlacedFootprint.Footprint.Depth = 1;
    sl.PlacedFootprint.Footprint.RowPitch = pitch;
    /* A BC level smaller than a block is copied as the whole block: clip the
     * box to the level's real size. */
    const u32 mip = o->mips ? sub % o->mips : 0u;
    u32 lw = o->w >> mip, lh = o->h >> mip;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    D3D12_BOX box = { 0, 0, 0, sl.PlacedFootprint.Footprint.Width, sl.PlacedFootprint.Footprint.Height, 1 };
    if (o->bc) {
        const u32 pw = (lw + 3u) & ~3u, ph = (lh + 3u) & ~3u;
        if (box.right > pw - x) box.right = pw - x;
        if (box.bottom > ph - y) box.bottom = ph - y;
    } else {
        if (x + box.right > lw) box.right = lw > x ? lw - x : 0u;
        if (y + box.bottom > lh) box.bottom = lh > y ? lh - y : 0u;
    }
    if (!box.right || !box.bottom) return;
    D3D12_RESOURCE_STATES keep = o->st;
    d3e_barrier_res(o->res, &o->st, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, x, y, z, &sl, &box);
    if (o->kind != D3E_TEX) d3e_barrier_res(o->res, &o->st, keep);
}

static void d3e_release_obj(u32 h);
static void d3e_ms_forget(u32 h);        /* rsx_d3d12_engine_msaa.inc */

/* 1 R5G6B5, 2 A1R5G5B5, 3 R8 (B8), 4 R8G8 (G8B8): colour targets whose texels are
 * converted to / from the RGBA8 lanes (rsx_d3d12_cv_*); 0 none. */
static int d3e_cv_kind(rsx_be_format f)
{
    return f == RSX_BE_FMT_R5G6B5 ? 1 : f == RSX_BE_FMT_A1R5G5B5 ? 2 : f == RSX_BE_FMT_R8 ? 3 : f == RSX_BE_FMT_R8G8 ? 4 : 0;
}

static u32 d3e_color_new(rsx_be_format fmt, u32 w, u32 h)
{
    DXGI_FORMAT f;
    u32 bpp;
    if (!w || !h || w > 16384u || h > 16384u) return 0;
    if (!d3e_rt_format(fmt, &f, &bpp)) {
        D3E_WARN_ONCE(6, "engine: colour target format %d not supported\n", (int)fmt);
        return 0;
    }
    const u32 hd = d3e_alloc_obj();
    if (!hd) return 0;
    d3e_obj* o = &s_obj[hd];
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof cv);
    cv.Format = f;
    cv.Color[3] = 1.0f;
    o->res = d3e_texture_resource(D3D12_RESOURCE_DIMENSION_TEXTURE2D, f, w, h, 1, 1,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    if (!o->res) return 0;
    o->kind = D3E_COLOR;
    o->st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    o->fmt = fmt; o->rfmt = f; o->vfmt = f;
    o->w = w; o->h = h; o->d = 1; o->mips = 1; o->layers = 1; o->bpp = bpp;
    ID3D12Device_CreateRenderTargetView(s_d.dev, o->res, NULL, d3e_rtv_cpu(hd));
    o->has_rtv = 1;
    d3e_make_srv(hd, o->res, f, 0, 1, rsx_d3d12_cv_map(d3e_cv_kind(fmt), D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING), 0);
    o->has_srv = 1;
    return hd;
}

static u32 d3e_depth_new(u32 w, u32 h, u32 zcls)
{
    if (!w || !h || w > 16384u || h > 16384u) return 0;
    const u32 hd = d3e_alloc_obj();
    if (!hd) return 0;
    d3e_obj* o = &s_obj[hd];
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof cv);
    cv.Format = d3e_dsv_format(zcls);
    cv.DepthStencil.Depth = 1.0f;
    cv.DepthStencil.Stencil = 0xFF;
    o->res = d3e_texture_resource(D3D12_RESOURCE_DIMENSION_TEXTURE2D, d3e_depth_res_format(zcls), w, h, 1, 1,
                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                  D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    if (!o->res) return 0;
    o->kind = D3E_DEPTH;
    o->st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    o->zcls = (u8)zcls;
    o->has_stencil = zcls == D3E_ZD24S8 || zcls == D3E_ZD32S8;
    o->rfmt = d3e_depth_res_format(zcls);
    o->vfmt = d3e_depth_srv_format(zcls);
    o->w = w; o->h = h; o->d = 1; o->mips = 1; o->layers = 1;
    o->bpp = zcls == D3E_ZD16 ? 2u : zcls == D3E_ZD32S8 ? 8u : 4u;
    D3D12_DEPTH_STENCIL_VIEW_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.Format = cv.Format;
    dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateDepthStencilView(s_d.dev, o->res, &dd, d3e_dsv_cpu(hd));
    o->has_dsv = 1;
    d3e_make_srv(hd, o->res, o->vfmt, 0, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
    o->has_srv = 1;
    if (o->has_stencil) {
        d3e_make_srv(D3E_SRV_STENCIL + hd, o->res,
                     zcls == D3E_ZD32S8 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT,
                     0, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 1);
        o->has_ssrv = 1;
    }
    /* A fresh RSX zeta reads 1.0 / stencil 0xFF (RPCS3 clear_memory). */
    ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, d3e_dsv_cpu(hd),
        D3D12_CLEAR_FLAG_DEPTH | (o->has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0), 1.0f, 0xFF, 0, NULL);
    return hd;
}

/* A sampleable copy of (x, y, w, h) of a colour target. */
static u32 d3e_snapshot(d3e_obj* t, u32 x, u32 y, u32 w, u32 h)
{
    if (!t || !t->res) return 0;
    const u32 hd = d3e_alloc_obj();
    if (!hd) return 0;
    d3e_obj* o = &s_obj[hd];
    o->res = d3e_texture_resource(D3D12_RESOURCE_DIMENSION_TEXTURE2D, t->rfmt, w, h, 1, 1,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!o->res) return 0;
    o->kind = D3E_TEX;
    o->st = D3D12_RESOURCE_STATE_COPY_DEST;
    o->fmt = t->fmt; o->rfmt = t->rfmt; o->vfmt = t->vfmt;
    o->w = w; o->h = h; o->d = 1; o->mips = 1; o->layers = 1; o->bpp = t->bpp;
    d3e_trans(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = o->res; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.pResource = t->res; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { x, y, 0, x + w, y + h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, 0, &sl, &box);
    d3e_barrier_res(o->res, &o->st, D3E_SRV_STATES);
    d3e_make_srv(hd, o->res, o->vfmt, 0, 1, rsx_d3d12_cv_map(d3e_cv_kind(o->fmt), D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING), 0);
    o->has_srv = 1;
    return hd;
}

static void d3e_release_obj(u32 h)
{
    d3e_obj* o = d3e_get(h, 0);
    if (!o) return;
    d3e_ms_forget(h);
    /* views of this object (surface views, SNORM/sRGB twins) go with it */
    if (o->kind != D3E_VIEW && o->nviews) {
        for (u32 i = 1; i < D3E_MAX_OBJ; i++)
            if (s_obj[i].kind == D3E_VIEW && s_obj[i].parent == h) memset(&s_obj[i], 0, sizeof s_obj[i]);
    } else if (o->kind == D3E_VIEW) {
        d3e_obj* p = d3e_get(o->parent, 0);
        if (p && p->nviews) p->nviews--;
    }
    if (o->kind == D3E_DEPTH && o->dsnap) {
        const u32 sn = o->dsnap;
        o->dsnap = 0;
        d3e_obj* s = d3e_get(sn, 0);
        if (s) { s->owned = 0; d3e_release_obj(sn); }
    }
    if (o->owned) {
        /* a depth snapshot the engine let go of: its depth target forgets it */
        for (u32 i = 1; i < D3E_MAX_OBJ; i++)
            if (s_obj[i].kind == D3E_DEPTH && s_obj[i].dsnap == h) s_obj[i].dsnap = 0;
    }
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) {
        if (s_d.om_rt[i] == h) s_d.om_valid = 0;
        if (s_e.rt[i] == h) s_e.rt[i] = 0;
    }
    if (s_d.om_depth == h) s_d.om_valid = 0;
    if (s_e.depth == h) s_e.depth = 0;
    if (o->res) d3e_defer_release(o->res);
    memset(o, 0, sizeof *o);
}

/* rsx_d3d12_engine_async.inc */
struct d3e_vkey;
static int  d3e_async_on(void);
static int  d3e_async_active(void);
static int  d3e_async_compile(d3e_pipe* P, u32 idx, const char* vs_src, const char* ps_src);
static int  d3e_async_variant(d3e_pipe* P, const struct d3e_vkey* k);
static void d3e_async_poll(void);
static void d3e_async_forget(d3e_pipe* P);
static void d3e_async_frame(void);
static void d3e_async_stop(void);
static void d3e_async_draw_skipped(void);
static void d3e_async_note_dsf(u32 dsf);
static void d3e_plib_open(void);
static void d3e_plib_close(void);
static HRESULT d3e_plib_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, const D3D12_PIPELINE_STATE_STREAM_DESC* sd,
                            const void* ds2, size_t ds2n, ID3D12PipelineState** pso);
static int  d3e_pipeline_async(void* user);
static void d3e_pipeline_prewarm(void* user, u32 pipeline, u32 topology_mask);
static u32  s_pipe_gen;
static int  s_d3e_probing;           /* rsx_d3d12_engine_probe's throwaway device */
/* rsx_d3d12_engine_aa.inc */
static int  d3e_scene_aa(void* user, const rsx_be_scene_aa* q);
static void d3e_ua_device_up(void);
static void d3e_ua_device_down(void);
static int  d3e_fsr1_present(d3e_obj* o, u32 srv, u32 sx, u32 sy, u32 sw, u32 sh,
                             D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT rtf,
                             u32 lx, u32 ly, u32 lw, u32 lh, int gm, float g);
/* rsx_d3d12_engine_hdr.inc */
static DXGI_FORMAT d3e_hdr_swap_format(void);
static DXGI_FORMAT d3e_hdr_back_format(void);
static int  d3e_hdr_apply(void);
static int  d3e_hdr_target(D3D12_CPU_DESCRIPTOR_HANDLE* rtv, DXGI_FORMAT* rtf);
static void d3e_hdr_resolve(D3D12_CPU_DESCRIPTOR_HANDLE rtv);
static void d3e_lat_device_up(void);
static void d3e_lat_device_down(void);
static void d3e_lat_render(int ev);

/* rsx_d3d12_engine_up.inc */
static int  d3e_up_draw_begin(d3e_obj* const* rt, u32 n, d3e_obj* zo);
static int  d3e_up_marked(const d3e_obj* o);
static void d3e_up_close(u32 keep);
static void d3e_up_reset(void);
static void d3e_up_backend(rsx_draw_backend* b);
/* rsx_d3d12_engine_msaa.inc */
static u32  d3e_ms_plan(u32 n, int own, int scratch);
static void d3e_ms_draw_prep(u32 samples, u32 n, int own);
static void d3e_ms_om(u32 n, int own);
static void d3e_ms_viewport(D3D12_VIEWPORT* v, D3D12_RECT* sc);
static void d3e_ms_draw_done(u32 samples, u32 n, int own);
static void d3e_ms_pso_state(const d3e_pipe* P, const struct d3e_vkey* k, D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd);
static void d3e_ms_backend(rsx_draw_backend* b);
static int  d3e_ms_setup(void);
static void d3e_ms_device_down(void);
/* rsx_d3d12_engine_fg.inc */
struct d3e_aimg;
static int  d3e_fg_swapchain(DXGI_SWAP_CHAIN_DESC1* sd, IDXGISwapChain1** out);
static void d3e_fg_before_resize(void);
static void d3e_fg_swap_down(void);
static void d3e_fg_prepare(const struct d3e_aimg* depth, const struct d3e_aimg* mv, u32 rw, u32 rh, float jx, float jy,
                           float mvx, float mvy, int reset, int inverted, float ms);
static void d3e_fg_scene(u32 color, u32 w, u32 h);
static void d3e_fg_hudless(u32 surface, u32 rx, u32 ry, u32 rw, u32 rh, u32 lx, u32 ly, u32 lw, u32 lh,
                           int linear, int gm, float g, int other_path);
static void d3e_fg_present(int presented, u32 lx, u32 ly, u32 lw, u32 lh);
/* rsx_d3d12_engine_osd.inc */
static void d3e_osd_draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT rtf);
static void d3e_osd_device_down(void);
static void d3e_cap_present(int hdr, u32 bi, u32 x, u32 y, u32 w, u32 h);
static void d3e_cap_finish(void);
static void d3e_osd_note_present(u32 surface, u32 sx, u32 sy, u32 sw, u32 sh, u32 aw, u32 ah);
static int  s_d3e_repost;               /* rsx_d3d12_engine_osd_repost: a UI-only re-present */
#include "rsx_d3d12_engine_pass.inc"
#include "rsx_d3d12_engine_gs.inc"
#include "rsx_d3d12_engine_draw.inc"
#include "rsx_d3d12_engine_async.inc"
#include "rsx_d3d12_engine_aa.inc"
#include "rsx_d3d12_engine_hdr.inc"
#include "rsx_d3d12_engine_up.inc"
#include "rsx_d3d12_engine_msaa.inc"
#include "rsx_d3d12_engine_fg.inc"
#include "rsx_d3d12_engine_osd.inc"

#endif /* !RSX_D3D12_HELPERS_ONLY */

#else /* !RSX_D3D12_ENGINE_ON: no D3D12 here; the host falls back to another renderer */
#include "rsx_d3d12_engine.h"
int  rsx_d3d12_engine_probe(void) { return 0; }
int  rsx_d3d12_engine_relaunch_compatible(void) { return 0; }
void rsx_d3d12_engine_set_window(void* hwnd) { (void)hwnd; }
int  rsx_d3d12_engine_init(uint32_t width, uint32_t height, const char* title)
{ (void)width; (void)height; (void)title; return -1; }
void rsx_d3d12_engine_present(void) {}
void rsx_d3d12_engine_shutdown(void) {}
uint32_t rsx_d3d12_engine_guest_draws(void) { return 0; }
uint32_t rsx_d3d12_engine_presented_center(void) { return 0; }
int  rsx_d3d12_engine_use_upscaler(int kind, float sharpness, unsigned dlss_preset)
{ (void)kind; (void)sharpness; (void)dlss_preset; return 0; }
int  rsx_d3d12_engine_temporal_upscaler_ready(void) { return 0; }
void rsx_d3d12_engine_force_output_fsr1(void) {}
int  rsx_d3d12_engine_latency_frame(void) { return 0; }
void rsx_d3d12_engine_latency_input(void) {}
int  rsx_d3d12_engine_latency_kind(void) { return 0; }
int  rsx_d3d12_engine_hdr_active(void) { return 0; }
int  rsx_d3d12_engine_fg_active(void) { return 0; }
const rsx_d3d12_osd_api* rsx_d3d12_engine_osd_api(void) { return NULL; }
int  rsx_d3d12_engine_capture(void** bgra, int* w, int* h, unsigned timeout_ms)
{ (void)timeout_ms; if (bgra) *bgra = NULL; if (w) *w = 0; if (h) *h = 0; return 0; }
void rsx_d3d12_engine_capture_free(void* p) { (void)p; }
int  rsx_d3d12_engine_capture_available(void) { return 0; }
int  rsx_d3d12_engine_osd_repost(unsigned idle_ms, int parked) { (void)idle_ms; (void)parked; return 0; }
float rsx_d3d12_engine_set_render_scale(float u) { (void)u; return 1.0f; }
void rsx_d3d12_engine_aa_window(int open, unsigned color_handle) { (void)open; (void)color_handle; }
#endif /* RSX_D3D12_ENGINE_ON */
