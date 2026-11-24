/* rsx_d3d12_engine_draw.inc -- part of rsx_d3d12_engine.c: occlusion queries,
 * guest pipelines, every rsx_draw_backend callback, device creation and the
 * public API. Mirrors rsx_vulkan_backend.c's vk_eng_* callbacks.
 *
 * Vertex input: the engine already converts every RSX attribute format
 * (CMP, S32K, UB256, the 3-component 16-bit types...) to float4 rows on the
 * CPU (rsx_vertex_compact), so one R32G32B32A32_FLOAT element per declared
 * ATTR<n> covers them all; there is no per-format input-layout explosion. */
#include "rsx_vp_decompiler.h"

/* ---- occlusion queries ------------------------------------------------------
 * A logical query (0..4095) is a list of segments, each one physical
 * D3D12_QUERY_TYPE_OCCLUSION slot. A segment opens lazily at the first draw
 * inside the query and closes (EndQuery + ResolveQueryData into s_d.qrb) at
 * any submission, internal pass, or the query's end. */

static u32 s_q_active;      /* logical id + 1 that draws count towards */
static u32 s_q_open_id;     /* logical id owning s_d.qactive_slot */
static u8  s_q_unknown[D3E_MAX_QUERIES];
static u8  s_q_scaled[D3E_MAX_QUERIES];    /* counted a render-scaled draw */

static void d3e_query_close_open(void)
{
    if (!s_d.qactive_slot || !s_d.cl) return;
    const u32 slot = s_d.qactive_slot - 1u;
    s_d.qactive_slot = 0;
    ID3D12GraphicsCommandList_EndQuery(s_d.cl, s_d.qheap, D3D12_QUERY_TYPE_OCCLUSION, slot);
    ID3D12GraphicsCommandList_ResolveQueryData(s_d.cl, s_d.qheap, D3D12_QUERY_TYPE_OCCLUSION, slot, 1,
                                               s_d.qrb, (u64)slot * 8u);
    d3e_query* q = &s_q[s_q_open_id];
    for (u32 i = 0; i < q->nseg; i++)
        if (q->seg[i] == slot + 1u) q->seg_fence[i] = s_d.fence_val + 1u;
}

/* Segments open on demand at the next draw. */
static void d3e_query_reopen(void) {}

static void d3e_query_fold(d3e_query* q, int all)
{
    const u64 done = ID3D12Fence_GetCompletedValue(s_d.fence);
    u32 k = 0;
    for (u32 i = 0; i < q->nseg; i++) {
        const u32 slot = q->seg[i] - 1u;
        if (all || (q->seg_fence[i] && q->seg_fence[i] <= done)) {
            q->accum += s_d.qrb_cpu[slot];
            s_d.qslot_used[slot] = 0;
        } else {
            q->seg[k] = q->seg[i];
            q->seg_fence[k] = q->seg_fence[i];
            k++;
        }
    }
    q->nseg = k;
}

static void d3e_query_free(u32 id)
{
    d3e_query* q = &s_q[id];
    if (s_d.qactive_slot && s_q_open_id == id) d3e_query_close_open();
    for (u32 i = 0; i < q->nseg; i++) s_d.qslot_used[q->seg[i] - 1u] = 0;
    q->nseg = 0;
    q->accum = 0;
    q->used = 0;
    s_q_unknown[id] = 0;
    s_q_scaled[id] = 0;
}

/* Called before a counted draw is recorded. 0: the draw is not counted. */
static int d3e_query_open(void)
{
    if (!s_q_active || !s_d.qheap) return 0;
    if (s_d.qactive_slot) return 1;
    const u32 id = s_q_active - 1u;
    d3e_query* q = &s_q[id];
    if (q->nseg == D3E_QUERY_SEGS) {
        d3e_query_fold(q, 0);
        if (q->nseg == D3E_QUERY_SEGS) {
            d3e_submit(1);
            d3e_query_fold(q, 1);
        }
    }
    u32 slot = D3E_QUERY_SLOTS;
    for (u32 n = 0; n < D3E_QUERY_SLOTS; n++) {
        const u32 s = (s_d.qslot_next + n) % D3E_QUERY_SLOTS;
        if (!s_d.qslot_used[s]) { slot = s; break; }
    }
    if (slot == D3E_QUERY_SLOTS) {
        D3E_WARN_ONCE(8, "occlusion: out of query slots, reporting visible\n");
        s_q_unknown[id] = 1;
        return 0;
    }
    s_d.qslot_next = (slot + 1u) % D3E_QUERY_SLOTS;
    s_d.qslot_used[slot] = 1;
    q->seg[q->nseg] = slot + 1u;
    q->seg_fence[q->nseg] = 0;
    q->nseg++;
    q->used = 1;
    ID3D12GraphicsCommandList_BeginQuery(s_d.cl, s_d.qheap, D3D12_QUERY_TYPE_OCCLUSION, slot);
    s_d.qactive_slot = slot + 1u;
    s_q_open_id = id;
    return 1;
}

static int d3e_occlusion_begin(void* user, u32 id)
{
    (void)user;
    if (!s_d.up || !s_d.qheap) return 0;
    id &= D3E_MAX_QUERIES - 1u;
    if (s_q_active && s_q_active != id + 1u) {
        d3e_query_close_open();
        s_q[s_q_active - 1u].active = 0;
    }
    d3e_query_free(id);
    s_q[id].active = 1;
    s_q_active = id + 1u;
    return 1;
}

static void d3e_occlusion_end(void* user, u32 id)
{
    (void)user;
    if (!s_d.up) return;
    id &= D3E_MAX_QUERIES - 1u;
    if (s_d.qactive_slot && s_q_open_id == id) d3e_query_close_open();
    s_q[id].active = 0;
    if (s_q_active == id + 1u) s_q_active = 0;
}

/* wait: 0 poll, 1 submit and block, 2 only submit what holds the query. */
static int d3e_occlusion_result(void* user, u32 id, int wait, u64* out)
{
    (void)user;
    if (!s_d.up || !s_d.qheap) return 0;
    id &= D3E_MAX_QUERIES - 1u;
    d3e_query* q = &s_q[id];
    const int open_here = s_d.qactive_slot && s_q_open_id == id;
    if (wait == 2) {
        int unsub = open_here;
        for (u32 i = 0; i < q->nseg; i++)
            if (!q->seg_fence[i] || q->seg_fence[i] > s_d.fence_val) unsub = 1;
        if (unsub) d3e_submit(0);
        return 0;
    }
    if (open_here) d3e_query_close_open();
    for (u32 i = 0; i < q->nseg; i++) {
        if (q->seg_fence[i] > s_d.fence_val) {
            if (!wait) return 0;
            d3e_submit(0);
        }
        if (ID3D12Fence_GetCompletedValue(s_d.fence) < q->seg_fence[i]) {
            if (!wait) return 0;
            d3e_wait_fence(q->seg_fence[i]);
        }
    }
    d3e_query_fold(q, 1);
    u64 sum = q->accum;
    /* render scale: samples of a corner-drawn draw count u^2 of the guest's */
    if (s_q_scaled[id] && sum && s_up.u > 0.0f && s_up.u < 1.0f) {
        const u64 s = (u64)((double)sum / ((double)s_up.u * (double)s_up.u) + 0.5);
        sum = s ? s : 1u;
    }
    if (s_q_unknown[id] && !sum) sum = 0xFFFFu;
    d3e_query_free(id);
    if (out) *out = sum;
    return 1;
}

static void d3e_occlusion_discard(void* user, u32 id)
{
    (void)user;
    if (!s_d.up) return;
    id &= D3E_MAX_QUERIES - 1u;
    d3e_query_free(id);
    s_q[id].active = 0;
    if (s_q_active == id + 1u) s_q_active = 0;
}

/* ---- the guest root signature ------------------------------------------------ */

static int d3e_rs_main_create(void)
{
    D3D12_DESCRIPTOR_RANGE r[4] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV,     16, 0,  0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV,      4, 16, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0,  0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,  4, 0,  0, 0 },
    };
    D3D12_ROOT_PARAMETER p[8];
    memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.ShaderRegister = 0;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].Descriptor.ShaderRegister = 1;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    static const D3D12_SHADER_VISIBILITY vis[4] = {
        D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_VERTEX,
        D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_VERTEX };
    for (u32 i = 0; i < 4; i++) {
        p[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        p[2 + i].DescriptorTable.pDescriptorRanges = &r[i];
        p[2 + i].ShaderVisibility = vis[i];
    }
    /* b2: the expansion GS's viewport size and line width (rsx_d3d12_engine_gs.inc) */
    p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[6].Constants.ShaderRegister = 2;
    p[6].Constants.Num32BitValues = 4;
    p[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    /* b3: the render scale's per-unit texture coordinate factors (rsx_d3d12_engine_up.inc) */
    p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[7].Constants.ShaderRegister = 3;
    p[7].Constants.Num32BitValues = 16;
    p[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC d = { s_up.on ? 8u : 7u, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    return SUCCEEDED(d3e_serialize_rs(&d, &s_d.rs_main)) ? 0 : -1;
}

/* ---- guest pipelines ------------------------------------------------------------ */

static const char s_attr_name[] = "ATTR";

/* The declared dimension (0 2D, 1 cube, 2 3D) of `name` (e.g. "rsx_tex3")
 * in `src`: the type word on the declaring line. */
static u32 d3e_decl_dim(const char* src, const char* name)
{
    char pat[48];
    snprintf(pat, sizeof pat, "%s : register", name);
    const char* m = strstr(src, pat);
    if (!m) return 0;
    const char* ls = m;
    while (ls > src && ls[-1] != '\n') ls--;
    const size_t n = (size_t)(m - ls);
    char line[128];
    const size_t c = n < sizeof line - 1 ? n : sizeof line - 1;
    memcpy(line, ls, c);
    line[c] = 0;
    if (strstr(line, "TextureCube")) return 1;
    if (strstr(line, "Texture3D")) return 2;
    return 0;
}

static u32 d3e_pipeline_create2(const char* vs_hlsl, const char* ps_hlsl,
                                const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt_fmt, u32 rt_count);

static u32 d3e_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                               const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                               u32 stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    if (!s_d.up || !vs_hlsl || !ps_hlsl || !rs) return 0;
    /* point sprites: the PS reads the expansion GS's sprite coordinate */
    char* ps_fixed = d3e_gs_on() ? rsx_d3d12_ps_pcoord_fix(ps_hlsl) : NULL;
    if (ps_fixed) ps_hlsl = ps_fixed;
    /* render scale: texture coordinates of targets drawn in their corner */
    char* ps_up = s_up.on ? rsx_d3d12_ps_uv_scale(ps_hlsl) : NULL;
    if (ps_up) ps_hlsl = ps_up;
    const u32 r = d3e_pipeline_create2(vs_hlsl, ps_hlsl, rs, layout, stride, rt_fmt, rt_count);
    free(ps_up);
    free(ps_fixed);
    return r;
}

static u32 d3e_pipeline_create2(const char* vs_hlsl, const char* ps_hlsl,
                                const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt_fmt, u32 rt_count)
{
    const int async = d3e_async_on();
    ID3DBlob* vs = NULL;
    ID3DBlob* ps = NULL;
    if (!async) {
        vs = d3e_compile(vs_hlsl, "main", "vs_5_0", "rsx_vp");
        if (!vs) return 0;
        ps = d3e_compile(ps_hlsl, "main", "ps_5_0", "rsx_fp");
        if (!ps) { ID3D10Blob_Release(vs); return 0; }
    }
    u32 idx = 0;
    while (idx < s_pipe_cap && s_pipe[idx].used) idx++;
    if (idx == s_pipe_cap) {
        const u32 nc = s_pipe_cap ? s_pipe_cap * 2u : 256u;
        d3e_pipe* np = (d3e_pipe*)realloc(s_pipe, (size_t)nc * sizeof *np);
        if (!np) { d3e_release(vs); d3e_release(ps); return 0; }
        memset(np + s_pipe_cap, 0, (size_t)(nc - s_pipe_cap) * sizeof *np);
        s_pipe = np;
        s_pipe_cap = nc;
    }
    d3e_pipe* P = &s_pipe[idx];
    memset(P, 0, sizeof *P);
    P->used = 1;
    if (!++s_pipe_gen) s_pipe_gen = 1;
    P->gen = s_pipe_gen;
    P->vs = vs; P->ps = ps;
    P->rs = *rs;
    P->stride = stride;
    P->rt_fmt = rt_fmt;
    P->rt_count = rt_count ? rt_count : 1u;
    P->gsio = d3e_gsio_intern(vs_hlsl, ps_hlsl);
    /* Inputs: the k-th declared ATTR<n> is the k-th float4 of a vertex. */
    const u32 count = layout ? layout->count : 0u;
    const char* in = strstr(vs_hlsl, "struct VSInput");
    const char* in_end = in ? strstr(in, "};") : NULL;
    u32 k = 0;
    for (const char* a = in; a && in_end && k < 16u; ) {
        a = strstr(a, ":ATTR");
        if (!a || a > in_end) break;
        a += 5;
        const u32 n = (u32)strtoul(a, NULL, 10);
        D3D12_INPUT_ELEMENT_DESC* e = &P->il[k];
        e->SemanticName = s_attr_name;
        e->SemanticIndex = n;
        e->Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        e->InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        if (k < count && 16u * k + 16u <= stride) {
            e->InputSlot = 0;
            e->AlignedByteOffset = 16u * k;
        } else {
            e->InputSlot = 1;          /* the zero vertex */
            e->AlignedByteOffset = 0;
        }
        k++;
    }
    P->nil = k;
    char nm[24];
    for (u32 u = 0; u < 16u; u++) {
        snprintf(nm, sizeof nm, "rsx_tex%u", u);
        const u32 d = d3e_decl_dim(ps_hlsl, nm);
        if (d == 1) P->ps_cube |= 1u << u;
        else if (d == 2) P->ps_3d |= 1u << u;
    }
    for (u32 u = 0; u < 4u; u++) {
        snprintf(nm, sizeof nm, "rsx_vtex%u", u);
        const u32 d = d3e_decl_dim(vs_hlsl, nm);
        if (d == 1) P->vs_cube |= 1u << u;
        else if (d == 2) P->vs_3d |= 1u << u;
    }
    const u32 f[4] = { rs->sf_rgb, rs->df_rgb, rs->sf_a, rs->df_a };
    for (u32 i = 0; i < 2; i++) {
        if (f[i] == 0x8001u || f[i] == 0x8002u) P->uses_const_color = 1;
        if (f[i] == 0x8003u || f[i] == 0x8004u) P->uses_const_alpha = 1;
    }
    if (async) {
        P->pending = 1;
        if (!d3e_async_compile(P, idx, vs_hlsl, ps_hlsl)) {
            /* out of memory for the job: compile here */
            P->pending = 0;
            P->vs = d3e_compile(vs_hlsl, "main", "vs_5_0", "rsx_vp");
            P->ps = P->vs ? d3e_compile(ps_hlsl, "main", "ps_5_0", "rsx_fp") : NULL;
            if (!P->vs || !P->ps) {
                d3e_release(P->vs); d3e_release(P->ps);
                memset(P, 0, sizeof *P);
                return 0;
            }
        }
    }
    return idx + 1u;
}

static void d3e_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > s_pipe_cap) return;
    d3e_pipe* P = &s_pipe[pipeline - 1u];
    if (!P->used) return;
    d3e_async_forget(P);
    for (u32 i = 0; i < P->nvar; i++) {
        if (P->var[i].pso == s_d.cur_pso) s_d.cur_pso = NULL;
        d3e_defer_release(P->var[i].pso);
    }
    if (P->vs) ID3D10Blob_Release(P->vs);
    if (P->ps) ID3D10Blob_Release(P->ps);
    memset(P, 0, sizeof *P);
    if (s_e.pipeline == pipeline) s_e.pipeline = 0;
}

/* ---- pipeline variants -----------------------------------------------------------
 * A guest pipeline is one VS/PS pair plus its render state; the PSO also
 * depends on the topology class, the depth attachment's format, the depth
 * bias and the polygon mode, so each pipeline keeps a few variants. */

typedef struct d3e_vkey {
    u32 topo;       /* D3D12_PRIMITIVE_TOPOLOGY_TYPE */
    u32 dsf;        /* DXGI format of the depth attachment, UNKNOWN: none */
    u32 stencil;    /* the attachment has stencil */
    s32 bias;
    float slope;
    u32 wire;
    u32 gs;         /* expansion GS: 0 none, 1 points, 2 wide lines */
    u32 samples;    /* host MSAA twin sample count (rsx_d3d12_engine_msaa.inc); 0 / 1: none */
} d3e_vkey;

static int d3e_mrt_inherit(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_VK_MRT_INHERIT", 0);
    return v;
}

static int d3e_flip_front(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_FLIP_FRONT", 0);
    return v;
}

static D3D12_BLEND d3e_bf(u32 f, int alpha)
{
    int b = rsx_d3d12_blend_factor(f, alpha);
    if (!alpha && s_d.alpha_factor_ok) {
        if (f == 0x8003u) b = RSX_D3D12_BLEND_ALPHA_FACTOR;
        else if (f == 0x8004u) b = RSX_D3D12_BLEND_INV_ALPHA_FACTOR;
    }
    return (D3D12_BLEND)b;
}

/* One pipeline-state-stream subobject: a u32 type, the inner struct at its
 * natural alignment, the whole padded to 8 bytes. */
static void d3e_ss(u8* b, size_t* n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const void* d, size_t sz, size_t al)
{
    const size_t o = (*n + 7u) & ~(size_t)7u;
    const u32 t = (u32)type;
    memcpy(b + o, &t, 4);
    const size_t dof = (o + 4u + al - 1u) & ~(al - 1u);
    memcpy(b + dof, d, sz);
    *n = (dof + sz + 7u) & ~(size_t)7u;
}

static ID3D12PipelineState* d3e_build_pso(d3e_pipe* P, const d3e_vkey* k)
{
    const rsx_be_render_state* rs = &P->rs;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_d.rs_main;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(P->vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(P->vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(P->ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(P->ps);
    ID3DBlob* gs = NULL;
    if (k->gs) {
        gs = d3e_gs_compile(P->gsio, k->gs);
        if (!gs) {
            D3E_LOG("expansion GS (%s) failed to compile: draws with it are skipped\n", k->gs == 1 ? "points" : "lines");
            return NULL;
        }
        pd.GS.pShaderBytecode = ID3D10Blob_GetBufferPointer(gs);
        pd.GS.BytecodeLength = ID3D10Blob_GetBufferSize(gs);
    }

    /* blend, per target */
    DXGI_FORMAT rtf = DXGI_FORMAT_R8G8B8A8_UNORM;
    u32 bpp = 0;
    d3e_rt_format(P->rt_fmt, &rtf, &bpp);
    const int blendable = d3e_rt_blendable(P->rt_fmt);
    pd.BlendState.IndependentBlendEnable = TRUE;
    const int logic = rs->logic_op_enable && d3e_logic_on();
    if (rs->logic_op_enable) {
        D3D12_RENDER_TARGET_BLEND_DESC probe;
        memset(&probe, 0, sizeof probe);
        const int lq = logic ? rsx_d3d12_logic_blend(rs->logic_op, &probe) : 0;
        if (!lq) D3E_WARN_ONCE(17, "logic op 0x%X is not emulated: drawn without blending\n", rs->logic_op);
        else if (lq == 2) D3E_WARN_ONCE(32, "logic op 0x%X approximated by blending (exact on 0 / 1 values; unverified)\n", rs->logic_op);
    }
    for (u32 r = 0; r < P->rt_count && r < RSX_BE_MAX_COLOR_TARGETS; r++) {
        D3D12_RENDER_TARGET_BLEND_DESC* t = &pd.BlendState.RenderTarget[r];
        u8 wm = 0;
        int on;
        if (r == 0 || d3e_mrt_inherit()) {
            const u32 cm = rs->color_mask;
            if ((cm >> 16) & 0xFFu) wm |= D3D12_COLOR_WRITE_ENABLE_RED;
            if ((cm >> 8) & 0xFFu)  wm |= D3D12_COLOR_WRITE_ENABLE_GREEN;
            if (cm & 0xFFu)         wm |= D3D12_COLOR_WRITE_ENABLE_BLUE;
            if ((cm >> 24) & 0xFFu) wm |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
            on = rs->blend_enable || (rs->blend_mrt & 1u);
        } else {
            const u32 nib = (rs->color_mask_mrt >> (4u * r)) & 0xFu;
            if (nib & 2u) wm |= D3D12_COLOR_WRITE_ENABLE_RED;
            if (nib & 4u) wm |= D3D12_COLOR_WRITE_ENABLE_GREEN;
            if (nib & 8u) wm |= D3D12_COLOR_WRITE_ENABLE_BLUE;
            if (nib & 1u) wm |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
            on = (rs->blend_mrt >> r) & 1u;
        }
        t->RenderTargetWriteMask = (UINT8)rsx_d3d12_cv_write_mask(d3e_cv_kind(P->rt_fmt), wm);
        t->LogicOp = D3D12_LOGIC_OP_NOOP;
        t->BlendEnable = (on && !rs->logic_op_enable && blendable) ? TRUE : FALSE;
        if (on && !blendable)
            D3E_WARN_ONCE(18, "blending on a non-blendable target format %d: drawn without blending\n", (int)P->rt_fmt);
        t->SrcBlend = d3e_bf(rs->sf_rgb, 0);
        t->DestBlend = d3e_bf(rs->df_rgb, 0);
        t->BlendOp = (D3D12_BLEND_OP)rsx_d3d12_blend_op(rs->eq_rgb);
        t->SrcBlendAlpha = d3e_bf(rs->sf_a, 1);
        t->DestBlendAlpha = d3e_bf(rs->df_a, 1);
        t->BlendOpAlpha = (D3D12_BLEND_OP)rsx_d3d12_blend_op(rs->eq_a);
        /* RSX logic ops replace blending (NV4097 LOGIC_OP_ENABLE); colour targets are UNORM / FLOAT here,
         * where D3D12 has no logic ops: the subset that blending expresses (rsx_d3d12_logic_blend) */
        if (logic && blendable) rsx_d3d12_logic_blend(rs->logic_op, t);
        pd.RTVFormats[r] = rtf;
    }
    pd.NumRenderTargets = P->rt_count;
    pd.SampleMask = 0xFFFFFFFFu;

    /* rasteriser */
    D3D12_RASTERIZER_DESC* ra = &pd.RasterizerState;
    ra->FillMode = k->wire ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    const u32 cull = rsx_vk_cull_mode(rs->cull_enable, rs->cull_face);
    ra->CullMode = cull == 1 ? D3D12_CULL_MODE_FRONT : cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    if (k->gs) ra->CullMode = D3D12_CULL_MODE_NONE;   /* points and lines are never culled */
    int ccw = rs->front_face == 0x0901u;
    if (d3e_flip_front()) ccw = !ccw;
    ra->FrontCounterClockwise = ccw ? TRUE : FALSE;
    ra->DepthBias = k->bias;
    ra->DepthBiasClamp = 0.0f;
    ra->SlopeScaledDepthBias = k->slope;
    ra->DepthClipEnable = rs->depth_clamp ? FALSE : TRUE;
    ra->ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    /* depth-stencil */
    rsx_d3d12_depth_stencil_desc2 ds2;
    memset(&ds2, 0, sizeof ds2);
    int need_ds2 = 0;
    if (k->dsf != DXGI_FORMAT_UNKNOWN) {
        ds2.DepthEnable = rs->depth_test ? TRUE : FALSE;
        ds2.DepthWriteMask = (rs->depth_test && rs->depth_write) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        ds2.DepthFunc = (D3D12_COMPARISON_FUNC)rsx_d3d12_compare(rs->depth_func);
        if (rs->stencil_enable && k->stencil) {
            ds2.StencilEnable = TRUE;
            rsx_d3d12_stencilop_desc1* f = &ds2.FrontFace;
            f->StencilFailOp = (D3D12_STENCIL_OP)rsx_d3d12_stencil_op(rs->s_fail);
            f->StencilDepthFailOp = (D3D12_STENCIL_OP)rsx_d3d12_stencil_op(rs->s_zfail);
            f->StencilPassOp = (D3D12_STENCIL_OP)rsx_d3d12_stencil_op(rs->s_zpass);
            f->StencilFunc = (D3D12_COMPARISON_FUNC)rsx_d3d12_compare(rs->s_func);
            f->StencilReadMask = (UINT8)rs->s_func_mask;
            f->StencilWriteMask = (UINT8)rs->s_write_mask;
            rsx_d3d12_stencilop_desc1* b = &ds2.BackFace;
            if (rs->stencil_two_sided) {
                b->StencilFailOp = (D3D12_STENCIL_OP)rsx_d3d12_stencil_op(rs->bs_fail);
                b->StencilDepthFailOp = (D3D12_STENCIL_OP)rsx_d3d12_stencil_op(rs->bs_zfail);
                b->StencilPassOp = (D3D12_STENCIL_OP)rsx_d3d12_stencil_op(rs->bs_zpass);
                b->StencilFunc = (D3D12_COMPARISON_FUNC)rsx_d3d12_compare(rs->bs_func);
                b->StencilReadMask = (UINT8)rs->bs_func_mask;
                b->StencilWriteMask = (UINT8)rs->bs_write_mask;
                if ((b->StencilReadMask != f->StencilReadMask || b->StencilWriteMask != f->StencilWriteMask)) {
                    if (s_d.sep_stencil_ok) need_ds2 = 1;
                    else {
                        D3E_WARN_ONCE(19, "separate back-face stencil masks are not supported here: front masks used\n");
                        b->StencilReadMask = f->StencilReadMask;
                        b->StencilWriteMask = f->StencilWriteMask;
                    }
                }
            } else *b = *f;
        }
        if (rs->depth_bounds_test) {
            if (s_d.depth_bounds_ok && s_d.cl1) ds2.DepthBoundsTestEnable = TRUE;
            else D3E_WARN_ONCE(20, "depth bounds test is not supported here: ignored\n");
        }
    }
    pd.DepthStencilState.DepthEnable = ds2.DepthEnable;
    pd.DepthStencilState.DepthWriteMask = ds2.DepthWriteMask;
    pd.DepthStencilState.DepthFunc = ds2.DepthFunc ? ds2.DepthFunc : D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.StencilEnable = ds2.StencilEnable;
    pd.DepthStencilState.StencilReadMask = ds2.FrontFace.StencilReadMask;
    pd.DepthStencilState.StencilWriteMask = ds2.FrontFace.StencilWriteMask;
    {
        const rsx_d3d12_stencilop_desc1* s[2] = { &ds2.FrontFace, &ds2.BackFace };
        D3D12_DEPTH_STENCILOP_DESC* d[2] = { &pd.DepthStencilState.FrontFace, &pd.DepthStencilState.BackFace };
        for (u32 i = 0; i < 2; i++) {
            d[i]->StencilFailOp = s[i]->StencilFailOp ? s[i]->StencilFailOp : D3D12_STENCIL_OP_KEEP;
            d[i]->StencilDepthFailOp = s[i]->StencilDepthFailOp ? s[i]->StencilDepthFailOp : D3D12_STENCIL_OP_KEEP;
            d[i]->StencilPassOp = s[i]->StencilPassOp ? s[i]->StencilPassOp : D3D12_STENCIL_OP_KEEP;
            d[i]->StencilFunc = s[i]->StencilFunc ? s[i]->StencilFunc : D3D12_COMPARISON_FUNC_ALWAYS;
        }
        ds2.FrontFace.StencilFailOp = d[0]->StencilFailOp; ds2.FrontFace.StencilDepthFailOp = d[0]->StencilDepthFailOp;
        ds2.FrontFace.StencilPassOp = d[0]->StencilPassOp; ds2.FrontFace.StencilFunc = d[0]->StencilFunc;
        ds2.BackFace.StencilFailOp = d[1]->StencilFailOp; ds2.BackFace.StencilDepthFailOp = d[1]->StencilDepthFailOp;
        ds2.BackFace.StencilPassOp = d[1]->StencilPassOp; ds2.BackFace.StencilFunc = d[1]->StencilFunc;
        ds2.DepthFunc = pd.DepthStencilState.DepthFunc;
    }
    pd.DSVFormat = (DXGI_FORMAT)k->dsf;
    pd.InputLayout.pInputElementDescs = P->nil ? P->il : NULL;
    pd.InputLayout.NumElements = P->nil;
    pd.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)k->topo;
    pd.SampleDesc.Count = 1;
    d3e_ms_pso_state(P, k, &pd);

    ID3D12PipelineState* pso = NULL;
    HRESULT hr;
    if (s_d.dev2 && (need_ds2 || ds2.DepthBoundsTestEnable)) {
        u64 buf[160];
        u8* b = (u8*)buf;
        memset(buf, 0, sizeof buf);
        size_t n = 0;
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &pd.pRootSignature, sizeof(void*), sizeof(void*));
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &pd.VS, sizeof pd.VS, sizeof(void*));
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &pd.PS, sizeof pd.PS, sizeof(void*));
        if (gs) d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, &pd.GS, sizeof pd.GS, sizeof(void*));
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &pd.BlendState, sizeof pd.BlendState, 4);
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &pd.SampleMask, sizeof pd.SampleMask, 4);
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &pd.RasterizerState, sizeof pd.RasterizerState, 4);
        if (need_ds2) {
            d3e_ss(b, &n, RSX_D3D12_SUBOBJECT_DEPTH_STENCIL2, &ds2, sizeof ds2, 4);
        } else {
            D3D12_DEPTH_STENCIL_DESC1 ds1;
            memset(&ds1, 0, sizeof ds1);
            ds1.DepthEnable = pd.DepthStencilState.DepthEnable;
            ds1.DepthWriteMask = pd.DepthStencilState.DepthWriteMask;
            ds1.DepthFunc = pd.DepthStencilState.DepthFunc;
            ds1.StencilEnable = pd.DepthStencilState.StencilEnable;
            ds1.StencilReadMask = pd.DepthStencilState.StencilReadMask;
            ds1.StencilWriteMask = pd.DepthStencilState.StencilWriteMask;
            ds1.FrontFace = pd.DepthStencilState.FrontFace;
            ds1.BackFace = pd.DepthStencilState.BackFace;
            ds1.DepthBoundsTestEnable = ds2.DepthBoundsTestEnable;
            d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds1, sizeof ds1, 4);
        }
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &pd.InputLayout, sizeof pd.InputLayout, sizeof(void*));
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &pd.PrimitiveTopologyType,
               sizeof pd.PrimitiveTopologyType, 4);
        struct D3D12_RT_FORMAT_ARRAY rta;
        memset(&rta, 0, sizeof rta);
        rta.NumRenderTargets = pd.NumRenderTargets;
        for (u32 i = 0; i < 8; i++) rta.RTFormats[i] = pd.RTVFormats[i];
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rta, sizeof rta, 4);
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &pd.DSVFormat, sizeof pd.DSVFormat, 4);
        d3e_ss(b, &n, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &pd.SampleDesc, sizeof pd.SampleDesc, 4);
        D3D12_PIPELINE_STATE_STREAM_DESC sd = { n, b };
        hr = d3e_plib_pso(&pd, &sd, &ds2, sizeof ds2, &pso);
    } else {
        if (need_ds2 || ds2.DepthBoundsTestEnable)
            D3E_WARN_ONCE(21, "ID3D12Device2 missing: depth bounds / separate stencil masks ignored\n");
        hr = d3e_plib_pso(&pd, NULL, NULL, 0, &pso);
    }
    if (gs) ID3D10Blob_Release(gs);
    if (FAILED(hr)) {
        D3E_LOG("guest PSO failed 0x%08lX (topology %u, ds %u, rt %d x%u, inputs %u): draws with it are skipped\n",
                (unsigned long)hr, k->topo, k->dsf, (int)P->rt_fmt, P->rt_count, P->nil);
        return NULL;
    }
    return pso;
}

static ID3D12PipelineState* d3e_variant_get(d3e_pipe* P, const d3e_vkey* k)
{
    const u64 key = d3e_hash(k, sizeof *k, 0) | 1u;
    for (u32 i = 0; i < P->nvar; i++)
        if (P->var[i].key == key) return P->var[i].pso;    /* NULL while pending */
    const int queued = d3e_async_active() && d3e_async_variant(P, k);
    ID3D12PipelineState* pso = queued ? NULL : d3e_build_pso(P, k);
    u32 slot;
    if (P->nvar < D3E_PIPE_VARIANTS) slot = P->nvar++;
    else {
        slot = P->next_evict;
        P->next_evict = (P->next_evict + 1u) % D3E_PIPE_VARIANTS;
        if (P->var[slot].pso == s_d.cur_pso) s_d.cur_pso = NULL;
        d3e_defer_release(P->var[slot].pso);
    }
    P->var[slot].key = key;
    P->var[slot].pso = pso;     /* NULL is cached too: the failure is not retried */
    P->var[slot].pending = (u8)queued;
    return pso;
}
/* ---- engine state ------------------------------------------------------------- */

static void d3e_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) s_e.rt[i] = (surfaces && i < count) ? surfaces[i] : 0;
    s_e.nrt = surfaces ? count : 0;
    s_e.depth = depth;
}

static void d3e_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e.pipeline = pipeline; }

static void d3e_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!data) { s_e.vs_bytes = 0; return; }
    if (bytes > D3E_VS_CB_BYTES) {
        bytes = D3E_VS_CB_BYTES;
        D3E_WARN_ONCE(9, "VS constants truncated to %u bytes\n", D3E_VS_CB_BYTES);
    }
    if (bytes != s_e.vs_bytes || memcmp(s_e.vs_data, data, bytes)) {
        memcpy(s_e.vs_data, data, bytes);
        if (bytes < s_e.vs_bytes) memset(s_e.vs_data + bytes, 0, s_e.vs_bytes - bytes);
        s_e.vs_bytes = bytes;
        s_e.vs_dirty = 1;
    }
}

static void d3e_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!data) { s_e.ps_bytes = 0; s_e.ps_dirty = 1; return; }
    if (bytes > D3E_PS_CB_BYTES) {
        bytes = D3E_PS_CB_BYTES;
        D3E_WARN_ONCE(10, "PS constants truncated to %u bytes\n", D3E_PS_CB_BYTES);
    }
    if (bytes != s_e.ps_bytes || memcmp(s_e.ps_data, data, bytes)) {
        memcpy(s_e.ps_data, data, bytes);
        s_e.ps_bytes = bytes;
        s_e.ps_dirty = 1;
    }
}

static void d3e_bind_textures(void* user, const u32* textures, const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    if (!textures) mask = 0;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e.samp[u] = samplers[u];
    }
    s_e.tex_mask = mask;
}

static void d3e_bind_vertex_textures(void* user, const u32* textures, const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    if (!textures) mask = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e.vtex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e.vsamp[u] = samplers[u];
    }
    s_e.vtex_mask = mask & ((1u << RSX_BE_MAX_VERTEX_TEXTURES) - 1u);
}

static void d3e_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e.vp[0] = x; s_e.vp[1] = y; s_e.vp[2] = w; s_e.vp[3] = h;
    s_e.have_vp = 1;
}

static void d3e_set_depth_range(void* user, float min_z, float max_z)
{
    (void)user;
    s_e.zr[0] = min_z; s_e.zr[1] = max_z;
    s_e.have_zr = 1;
}

static void d3e_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e.sc[0] = x; s_e.sc[1] = y; s_e.sc[2] = w; s_e.sc[3] = h;
    s_e.have_sc = 1;
}

static void d3e_set_stencil_ref(void* user, u32 ref) { (void)user; s_e.sref = ref; }

static void d3e_set_raster_dynamic(void* user, u32 poly_offset_fill, float scale, float bias,
                                   float line_width, u32 back_ref)
{
    (void)user;
    s_e.dbias[0] = poly_offset_fill ? 1.0f : 0.0f;
    s_e.dbias[1] = bias;
    s_e.dbias[2] = scale;
    s_e.line_width = line_width;
    s_e.bref = 0x100u | (back_ref & 0xFFu);
}

/* D3D12 wants the bounds inside [0, 1]; a NaN opens that end. */
static float d3e_depth_bound(float v, float open)
{
    if (!(v == v)) return open;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void d3e_set_depth_bounds(void* user, float min, float max)
{
    (void)user;
    s_e.dbmin = d3e_depth_bound(min, 0.0f);
    s_e.dbmax = d3e_depth_bound(max, 1.0f);
}

static void d3e_set_blend_constants(void* user, const float rgba[4])
{
    (void)user;
    if (rgba) memcpy(s_e.blend, rgba, sizeof s_e.blend);
}

/* ---- draw --------------------------------------------------------------------- */

static int d3e_env_polygon_mode(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_ENG_POLYGON_MODE", 0);
    return v;
}

static int d3e_env_nv_d24_half(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_NV_D24_HALF", 0);
    return v;
}

/* RSX_D3D12_DEPTH_RANGE=0 ignores SET_DEPTH_RANGE (always 0..1). */
static int d3e_env_depth_range(void)
{
    static int v = -1;
    if (v < 0) v = d3e_env_on("RSX_D3D12_DEPTH_RANGE", 1);
    return v;
}

static float d3e_clamp01(float v) { return !(v == v) ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static const D3D12_SAMPLER_DESC s_samp_default = {
    D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f, 1, D3D12_COMPARISON_FUNC_NEVER, { 0, 0, 0, 0 }, 0.0f, D3D12_FLOAT32_MAX
};

static u32 s_vs_epoch = 0xFFFFFFFFu, s_ps_epoch = 0xFFFFFFFFu;

/* A sampled unit, resolved for one draw. */
typedef struct d3e_unit {
    u32 srv;            /* CPU heap index */
    d3e_obj* base;      /* the resource to transition, or NULL */
} d3e_unit;

/* Resolve unit `h` declared with dimension `ddim`: its SRV, and a feedback
 * snapshot when the resource is one of the draw's own colour targets. */
static void d3e_resolve_unit(u32 h, u32 ddim, u32 nrt, u32 own_depth, d3e_unit* out, u32* snaps, u32* nsnap)
{
    out->srv = D3E_SRV_NULL + (ddim > 2 ? 0 : ddim);
    out->base = NULL;
    d3e_obj* o = d3e_get(h, 0);
    if (!o || !o->has_srv) return;
    d3e_obj* b = d3e_base(o);
    if (!b || !b->res) return;
    if ((u32)o->dim != ddim) {
        D3E_WARN_ONCE(11, "texture unit dimension %u bound to a %u-D program unit: unit reads 0\n",
                      (unsigned)o->dim, ddim);
        return;
    }
    if (b->kind == D3E_DEPTH && (u32)(b - s_obj) == own_depth) {
        D3E_WARN_ONCE(12, "draw samples its own depth attachment: unit reads 0\n");
        return;
    }
    if (b->kind == D3E_COLOR) {
        for (u32 i = 0; i < nrt; i++) {
            if (s_e.rt[i] != (u32)(b - s_obj)) continue;
            /* feedback: sample a copy taken before the draw */
            const u32 sn = d3e_snapshot(b, 0, 0, b->w, b->h);
            if (!sn) return;
            d3e_obj* so = &s_obj[sn];
            if (o != b) {
                so->vfmt = o->vfmt;
                d3e_make_srv(sn, so->res, o->vfmt, 0, 1, o->map ? o->map : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
            }
            if (*nsnap < 20u) snaps[(*nsnap)++] = sn;
            out->srv = sn;
            out->base = so;
            return;
        }
    }
    out->srv = (u32)(o - s_obj);
    out->base = b;
}

static void d3e_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                     u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!s_d.up || !s_e.pipeline || s_e.pipeline > s_pipe_cap || !vertices || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    d3e_async_poll();
    d3e_pipe* P = &s_pipe[s_e.pipeline - 1u];
    if (!P->used || stride != P->stride) return;
    if (P->pending || P->failed) {
        if (P->pending) d3e_async_draw_skipped();
        if (s_q_active) s_q_unknown[s_q_active - 1u] = 1;
        return;
    }
    const u32 n = s_e.nrt;
    if (n < 1u || n != P->rt_count) {
        D3E_WARN_ONCE(13, "draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count);
        if (s_q_active) s_q_unknown[s_q_active - 1u] = 1;
        return;
    }
    d3e_obj* rt[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n; i++) {
        rt[i] = d3e_get(s_e.rt[i], D3E_COLOR);
        if (!rt[i]) return;
    }
    if (rt[0]->fmt != P->rt_fmt) {
        D3E_WARN_ONCE(14, "draw into colour format %d with a pipeline built for %d skipped\n",
                      (int)rt[0]->fmt, (int)P->rt_fmt);
        if (s_q_active) s_q_unknown[s_q_active - 1u] = 1;
        return;
    }
    const u32 w = rt[0]->w, h = rt[0]->h;
    d3e_obj* zo = d3e_get(s_e.depth, D3E_DEPTH);
    const int own = zo && zo->w >= w && zo->h >= h;
    const int scratch = !own && zo != NULL;
    if (scratch && !d3e_scratch_depth(w, h)) return;
    const u32 own_depth = own ? s_e.depth : 0u;

    /* topology */
    const int tri = topology >= RSX_TOPOLOGY_TRIANGLES;
    const rsx_be_render_state* rs = &P->rs;
    if (tri && rsx_vk_cull_mode(rs->cull_enable, rs->cull_face) == 3u) {
        s_draws++;           /* FRONT_AND_BACK: nothing is rasterised */
        return;
    }
    /* point sizes / sprites and wide lines: the expansion GS (rsx_d3d12_engine_gs.inc) */
    const u32 gsk = !P->gsio ? 0u : topology == RSX_TOPOLOGY_POINTS ? 1u
                  : (!tri && s_e.line_width > 1.0f) ? 2u : 0u;
    if (topology == RSX_TOPOLOGY_POINTS && !gsk)
        D3E_WARN_ONCE(15, "points are drawn 1 pixel wide (no point size on Direct3D 12 without the expansion GS)\n");
    if (!tri && topology != RSX_TOPOLOGY_POINTS && s_e.line_width > 1.0f && !gsk)
        D3E_WARN_ONCE(16, "wide lines are drawn 1 pixel wide on Direct3D 12 without the expansion GS\n");
    d3e_vkey k;
    memset(&k, 0, sizeof k);
    k.gs = gsk;
    k.topo = topology == RSX_TOPOLOGY_POINTS ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
           : tri ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    k.dsf = own ? (u32)d3e_dsv_format(zo->zcls) : scratch ? (u32)DXGI_FORMAT_D24_UNORM_S8_UINT : (u32)DXGI_FORMAT_UNKNOWN;
    k.stencil = own ? zo->has_stencil : scratch ? 1u : 0u;
    if (s_e.dbias[0] != 0.0f && (own || scratch)) {
        const int zk = d3e_zhost_kind(own ? zo->zcls : D3E_ZD24S8);
        const float c = rsx_vk_depth_bias_constant(s_e.dbias[1], own && zo->gz16, zk,
                                                   s_d.vendor == 0x10DEu && d3e_env_nv_d24_half());
        k.bias = (s32)(c < 0.0f ? c - 0.5f : c + 0.5f);
        k.slope = rsx_vk_depth_bias_slope(s_e.dbias[2], 1.0f, 0);
    }
    k.wire = tri && d3e_env_polygon_mode() && rs->polygon_mode_front == 0x1B01u;
    const u32 msn = d3e_ms_plan(n, own, scratch);     /* host MSAA: the twins' sample count, 0 none */
    k.samples = msn;
    if (k.dsf != DXGI_FORMAT_UNKNOWN) d3e_async_note_dsf(k.dsf);
    ID3D12PipelineState* pso = d3e_variant_get(P, &k);
    if (!pso) {
        if (d3e_async_active()) d3e_async_draw_skipped();
        if (s_q_active) s_q_unknown[s_q_active - 1u] = 1;
        return;
    }

    /* render scale: into the corner of the scene window's targets (may rescale them first) */
    const int up_sc = s_up.on ? d3e_up_draw_begin(rt, n, own ? zo : NULL) : 0;
    s_up.scaled = up_sc;
    d3e_ms_draw_prep(msn, n, own);

    /* sampled units (feedback snapshots are recorded here, before any target
     * transition of this draw) */
    u32 snaps[20], nsnap = 0;
    d3e_unit pu[RSX_BE_MAX_TEXTURES], vu[RSX_BE_MAX_VERTEX_TEXTURES];
    D3D12_SAMPLER_DESC psd[RSX_BE_MAX_TEXTURES], vsd[RSX_BE_MAX_VERTEX_TEXTURES];
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const u32 dd = ((P->ps_cube >> u) & 1u) ? 1u : ((P->ps_3d >> u) & 1u) ? 2u : 0u;
        psd[u] = s_samp_default;
        if (!((s_e.tex_mask >> u) & 1u)) { pu[u].srv = D3E_SRV_NULL + dd; pu[u].base = NULL; continue; }
        d3e_resolve_unit(s_e.tex[u], dd, n, own_depth, &pu[u], snaps, &nsnap);
        if (pu[u].base) {
            const d3e_obj* so = d3e_get(pu[u].srv, 0);
            d3e_sampler_desc(&s_e.samp[u], d3e_filterable(so ? so->vfmt : pu[u].base->vfmt), &psd[u]);
        }
    }
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        const u32 dd = ((P->vs_cube >> u) & 1u) ? 1u : ((P->vs_3d >> u) & 1u) ? 2u : 0u;
        vsd[u] = s_samp_default;
        if (!((s_e.vtex_mask >> u) & 1u)) { vu[u].srv = D3E_SRV_NULL + dd; vu[u].base = NULL; continue; }
        d3e_resolve_unit(s_e.vtex[u], dd, n, own_depth, &vu[u], snaps, &nsnap);
        if (vu[u].base) {
            const d3e_obj* so = d3e_get(vu[u].srv, 0);
            d3e_sampler_desc(&s_e.vsamp[u], d3e_filterable(so ? so->vfmt : vu[u].base->vfmt), &vsd[u]);
        }
    }

    if (s_up.on) {
        /* b3: units reading a target drawn in its corner sample the corner; the
         * other textures of a scaled draw get the LOD bias of the scale */
        for (u32 u = 0; u < 16u; u++) {
            s_up.uvk[u] = 1.0f;
            if (!((s_e.tex_mask >> u) & 1u)) continue;
            const d3e_obj* b = d3e_base(d3e_get(s_e.tex[u], 0));
            if (b && d3e_up_marked(b)) s_up.uvk[u] = s_up.u;
            else if (up_sc && b && b->kind == D3E_TEX && b->rsc_rs == 0.0f && b->mips > 1u && s_up.bias != 0.0f)
                psd[u].MipLODBias += s_up.bias;
        }
        /* fp_rscale (rsx_fp_set_res_scale_dynamic): the target's 1/scale for
         * WPOS, then each unit's texel scale (corner region snapshots) */
        if (s_e.ps_bytes && s_e.ps_bytes + D3E_PS_EXTRA <= (u32)sizeof s_e.ps_data) {
            float rk[D3E_PS_EXTRA / 4u];
            memset(rk, 0, sizeof rk);
            rk[0] = up_sc ? 1.0f / s_up.u : 1.0f;
            for (u32 u = 0; u < 16u; u++) {
                const d3e_obj* b = ((s_e.tex_mask >> u) & 1u) ? d3e_base(d3e_get(s_e.tex[u], 0)) : NULL;
                rk[4 + u] = b && b->rsc_rs > 0.0f ? 1.0f / b->rsc_rs : 1.0f;
            }
            u8* dst = s_e.ps_data + s_e.ps_bytes;
            if (s_e.ps_extra != D3E_PS_EXTRA || memcmp(dst, rk, sizeof rk)) {
                memcpy(dst, rk, sizeof rk);
                s_e.ps_extra = D3E_PS_EXTRA;
                s_e.ps_dirty = 1;
            }
        } else if (s_e.ps_extra) {
            s_e.ps_extra = 0;
            s_e.ps_dirty = 1;
        }
    }

    /* Everything that can submit comes first; a submission in between
     * invalidates the ring allocations, so the whole set is retried. */
    const u64 vbytes = (u64)vertex_count * stride;
    const u64 ibytes = (indices && index_count) ? (u64)index_count * 4u : 0u;
    if (vbytes + ibytes + D3E_VS_CB_BYTES + D3E_PS_CB_BYTES + 4096u > D3E_UPLOAD_HALF) {
        D3E_WARN_ONCE(22, "draw of %u vertices / %u indices exceeds the upload ring: skipped\n", vertex_count, index_count);
        if (s_q_active) s_q_unknown[s_q_active - 1u] = 1;
        goto out;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE ps_samp, vs_samp, tab;
    D3D12_GPU_VIRTUAL_ADDRESS vb_va = 0, ib_va = 0;
    int counted = 0;
    for (int attempt = 0;; attempt++) {
        /* one check for both tables: a heap reset between them would leave
         * ps_samp pointing at slots the next tables overwrite */
        d3e_sampler_reserve(RSX_BE_MAX_TEXTURES + RSX_BE_MAX_VERTEX_TEXTURES, 2u);
        d3e_reserve(vbytes + ibytes + D3E_VS_CB_BYTES + D3E_PS_CB_BYTES + 2304u, 20u);
        const u32 ep = s_d.ring_epoch;
        ps_samp = d3e_sampler_table(psd, RSX_BE_MAX_TEXTURES);
        vs_samp = d3e_sampler_table(vsd, RSX_BE_MAX_VERTEX_TEXTURES);
        u8* vp = (u8*)d3e_upload_alloc(vbytes, 16, &vb_va, NULL, NULL);
        if (!vp) goto out;
        memcpy(vp, vertices, (size_t)vbytes);
        if (ibytes) {
            u8* ip = (u8*)d3e_upload_alloc(ibytes, 4, &ib_va, NULL, NULL);
            if (!ip) goto out;
            memcpy(ip, indices, (size_t)ibytes);
        }
        if (s_e.vs_dirty || s_vs_epoch != s_d.ring_epoch || !s_e.vs_va) {
            u8* c = (u8*)d3e_upload_alloc(D3E_VS_CB_BYTES, 256, &s_e.vs_va, NULL, NULL);
            if (!c) goto out;
            memcpy(c, s_e.vs_data, D3E_VS_CB_BYTES);
            s_vs_epoch = s_d.ring_epoch;
            s_e.vs_dirty = 0;
        }
        if (s_e.ps_dirty || s_ps_epoch != s_d.ring_epoch || !s_e.ps_va) {
            const u32 psb = s_e.ps_bytes + s_e.ps_extra;
            u32 pb = (psb + 255u) & ~255u;
            if (pb < 1280u) pb = 1280u;
            if (pb > D3E_PS_CB_BYTES + 256u) pb = D3E_PS_CB_BYTES + 256u;
            u8* c = (u8*)d3e_upload_alloc(pb, 256, &s_e.ps_va, NULL, NULL);
            if (!c) goto out;
            const u32 nb = psb < pb ? psb : pb;
            memcpy(c, s_e.ps_data, nb);
            if (nb < pb) memset(c + nb, 0, pb - nb);
            s_ps_epoch = s_d.ring_epoch;
            s_e.ps_dirty = 0;
        }
        u32 srcs[20];
        for (u32 u = 0; u < 20u; u++) srcs[u] = u < 16u ? pu[u].srv : vu[u - 16u].srv;
        if (d3e_state_cache() && s_rc.tab.ptr && s_rc.tab_epoch == s_d.ring_epoch && s_rc.tab_gen == s_srv_gen &&
            !memcmp(s_rc.tab_src, srcs, sizeof srcs)) {
            tab = s_rc.tab;           /* same sources, still in this epoch's ring: the copies are there */
        } else {
            static const UINT ones[20] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
            const UINT twenty = 20u;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu, src[20];
            d3e_desc_alloc(20u, &cpu, &tab);
            for (u32 u = 0; u < 20u; u++) src[u] = d3e_srv_cpu(srcs[u]);
            ID3D12Device_CopyDescriptors(s_d.dev, 1, &cpu, &twenty, 20u, src, ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            memcpy(s_rc.tab_src, srcs, sizeof srcs);
            s_rc.tab = tab;
            s_rc.tab_epoch = s_d.ring_epoch;
            s_rc.tab_gen = s_srv_gen;
        }
        counted = s_q_active ? d3e_query_open() : 0;
        if (ep == s_d.ring_epoch) break;
        if (attempt >= 2) {
            D3E_WARN_ONCE(23, "draw allocations kept being invalidated: skipped\n");
            goto out;
        }
    }
    if (s_q_active && !counted) s_q_unknown[s_q_active - 1u] = 1;
    if (s_q_active && counted && up_sc) s_q_scaled[s_q_active - 1u] = 1;

    /* transitions (one ResourceBarrier call) */
    d3e_bar_begin();
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++)
        if (pu[u].base) d3e_trans(pu[u].base, d3e_srv_state(pu[u].base));
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++)
        if (vu[u].base) d3e_trans(vu[u].base, d3e_srv_state(vu[u].base));
    for (u32 i = 0; i < n; i++) d3e_trans(rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (own) d3e_trans(zo, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (scratch) d3e_barrier_res(s_d.scratch, &s_d.scratch_st, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    d3e_bar_end();

    ID3D12GraphicsCommandList* cl = s_d.cl;
    const u32 omd = own ? own_depth : scratch ? 0xFFFFFFFFu : 0u;
    int om_same = !msn && s_d.om_valid && s_d.om_nrt == n && s_d.om_depth == omd;
    for (u32 i = 0; om_same && i < n; i++) if (s_d.om_rt[i] != s_e.rt[i]) om_same = 0;
    if (msn) {
        d3e_ms_om(n, own);
    } else if (!om_same) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv[RSX_BE_MAX_COLOR_TARGETS];
        for (u32 i = 0; i < n; i++) { rtv[i] = d3e_rtv_cpu(s_e.rt[i]); s_d.om_rt[i] = s_e.rt[i]; }
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = d3e_dsv_cpu(own ? own_depth : D3E_DSV_SCRATCH);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, n, rtv, FALSE, (own || scratch) ? &dsv : NULL);
        s_d.om_nrt = n;
        s_d.om_depth = omd;
        s_d.om_valid = 1;
    }
    if (s_d.cur_rs != s_d.rs_main) {
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, s_d.rs_main);
        s_d.cur_rs = s_d.rs_main;
        s_rc.valid = 0;
    }
    if (s_d.cur_pso != pso) {
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
        s_d.cur_pso = pso;
    }
    /* rc: what s_rc holds is bound (root arguments survive PSO changes) */
    const int rc = s_rc.valid && d3e_state_cache();
    if (!rc || s_rc.cbv[0] != s_e.vs_va)
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, s_e.vs_va);
    if (!rc || s_rc.cbv[1] != s_e.ps_va)
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 1, s_e.ps_va);
    s_rc.cbv[0] = s_e.vs_va; s_rc.cbv[1] = s_e.ps_va;
    if (!rc || s_rc.tab_bound != tab.ptr) {
        D3D12_GPU_DESCRIPTOR_HANDLE vt = tab;
        vt.ptr += (UINT64)16u * s_d.srv_inc;
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 2, tab);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 3, vt);
        s_rc.tab_bound = tab.ptr;
    }
    if (!rc || s_rc.samp[0] != ps_samp.ptr) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 4, ps_samp);
    if (!rc || s_rc.samp[1] != vs_samp.ptr) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 5, vs_samp);
    s_rc.samp[0] = ps_samp.ptr; s_rc.samp[1] = vs_samp.ptr;
    if (s_up.on) ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 7, 16, s_up.uvk, 0);

    /* viewport and scissor: per draw, so split-screen views each get theirs */
    D3D12_VIEWPORT v = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e.have_vp && s_e.vp[2] > 0.0f && s_e.vp[3] > 0.0f) {
        v.TopLeftX = s_e.vp[0]; v.TopLeftY = s_e.vp[1];
        v.Width = s_e.vp[2]; v.Height = s_e.vp[3];
        if (v.TopLeftX < -32768.0f) v.TopLeftX = -32768.0f;
        if (v.TopLeftY < -32768.0f) v.TopLeftY = -32768.0f;
        if (v.TopLeftX + v.Width > 32767.0f) v.Width = 32767.0f - v.TopLeftX;
        if (v.TopLeftY + v.Height > 32767.0f) v.Height = 32767.0f - v.TopLeftY;
    }
    if (s_e.have_zr && d3e_env_depth_range()) {
        v.MinDepth = d3e_clamp01(s_e.zr[0]);
        v.MaxDepth = d3e_clamp01(s_e.zr[1]);
    }
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    if (s_e.have_sc) {
        const u64 x1 = (u64)s_e.sc[0] + s_e.sc[2], y1 = (u64)s_e.sc[1] + s_e.sc[3];
        sc.left = (LONG)(s_e.sc[0] < w ? s_e.sc[0] : w);
        sc.top = (LONG)(s_e.sc[1] < h ? s_e.sc[1] : h);
        sc.right = (LONG)(x1 < w ? x1 : w);
        sc.bottom = (LONG)(y1 < h ? y1 : h);
        if (sc.right < sc.left) sc.right = sc.left;
        if (sc.bottom < sc.top) sc.bottom = sc.top;
    }
    /* the expansion GS sizes points / lines in guest pixels of the unscaled viewport */
    const float gvw = v.Width, gvh = v.Height;
    if (up_sc) {
        const float u = s_up.u;
        v.TopLeftX *= u; v.TopLeftY *= u; v.Width *= u; v.Height *= u;
        u32 sx = (u32)sc.left, sy = (u32)sc.top, sw = (u32)(sc.right - sc.left), sh = (u32)(sc.bottom - sc.top);
        rsx_d3d12_up_rect(u, &sx, &sy, &sw, &sh);
        sc.left = (LONG)sx; sc.top = (LONG)sy; sc.right = (LONG)(sx + sw); sc.bottom = (LONG)(sy + sh);
    }
    if (msn) d3e_ms_viewport(&v, &sc);
    if (!rc || memcmp(&s_rc.vp, &v, sizeof v)) ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &v);
    if (!rc || memcmp(&s_rc.sc, &sc, sizeof sc)) ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    s_rc.vp = v; s_rc.sc = sc;
    if (gsk) {
        const int gccw = (rs->front_face == 0x0901u) != (d3e_flip_front() != 0);
        const float gc[4] = { gvw, gvh, s_e.line_width > 1.0f ? s_e.line_width : 1.0f, gccw ? 1.0f : 0.0f };
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 6, 4, gc, 0);
    }

    static const D3D_PRIMITIVE_TOPOLOGY topo[6] = {
        D3D_PRIMITIVE_TOPOLOGY_UNDEFINED, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST,
        D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP };
    if (!rc || s_rc.topo != (u32)topology + 1u) ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, topo[topology]);
    s_rc.topo = (u32)topology + 1u;
    if (s_rc.zero_res != s_d.zero_vb) {
        s_rc.zero_res = s_d.zero_vb;
        s_rc.zero_va = ID3D12Resource_GetGPUVirtualAddress(s_d.zero_vb);
    }
    D3D12_VERTEX_BUFFER_VIEW vbv[2];
    vbv[0].BufferLocation = vb_va;
    vbv[0].SizeInBytes = (UINT)vbytes;
    vbv[0].StrideInBytes = stride;
    vbv[1].BufferLocation = s_rc.zero_va;
    vbv[1].SizeInBytes = 64;
    vbv[1].StrideInBytes = 0;
    /* slot 1 (the zero vertex) only changes with the device */
    ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, (rc && s_rc.vb1 == s_rc.zero_va) ? 1u : 2u, vbv);
    s_rc.vb1 = s_rc.zero_va;

    const u32 sref = s_e.sref & 0xFFu;
    const u32 bref = (s_e.bref & 0x100u) ? (s_e.bref & 0xFFu) : sref;
    if (bref != sref && rs->stencil_two_sided && k.stencil) {
        if (s_d.cl8 && s_d.stencil_ref_ok) {
            s_d.cl8->lpVtbl->OMSetFrontAndBackStencilRef(s_d.cl8, sref, bref);
            s_rc.sref = 0xFFFFFFFFu;
        } else {
            D3E_WARN_ONCE(24, "separate back-face stencil reference not supported here: front used\n");
            ID3D12GraphicsCommandList_OMSetStencilRef(cl, sref);
            s_rc.sref = sref;
        }
    } else {
        if (!rc || s_rc.sref != sref) ID3D12GraphicsCommandList_OMSetStencilRef(cl, sref);
        s_rc.sref = sref;
    }
    if (rs->depth_bounds_test && s_d.depth_bounds_ok && s_d.cl1 && (own || scratch))
        ID3D12GraphicsCommandList1_OMSetDepthBounds(s_d.cl1, s_e.dbmin, s_e.dbmax);
    {
        float bf[4] = { s_e.blend[0], s_e.blend[1], s_e.blend[2], s_e.blend[3] };
        if (P->uses_const_alpha && !s_d.alpha_factor_ok) {
            if (P->uses_const_color)
                D3E_WARN_ONCE(25, "blend uses constant colour and constant alpha together: approximated\n");
            else bf[0] = bf[1] = bf[2] = bf[3];
        }
        if (!rc || memcmp(s_rc.bf, bf, sizeof bf)) ID3D12GraphicsCommandList_OMSetBlendFactor(cl, bf);
        memcpy(s_rc.bf, bf, sizeof bf);
    }
    s_rc.valid = 1;
    if (ibytes) {
        D3D12_INDEX_BUFFER_VIEW ibv = { ib_va, (UINT)ibytes, DXGI_FORMAT_R32_UINT };
        ID3D12GraphicsCommandList_IASetIndexBuffer(cl, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(cl, index_count, 1, 0, 0, 0);
    } else {
        ID3D12GraphicsCommandList_DrawInstanced(cl, vertex_count, 1, 0, 0);
    }
    d3e_ms_draw_done(msn, n, own);
    if (!s_draws) d3e_lat_render(0);          /* Reflex RENDERSUBMIT_START */
    s_draws++;
out:
    for (u32 i = 0; i < nsnap; i++) d3e_release_obj(snaps[i]);
}

/* ---- lifecycle -------------------------------------------------------------------- */

#define D3E_SV_CACHE 1024u
static u32 s_sv_cache[D3E_SV_CACHE];     /* surface_view lookups */

static int d3e_be_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(&s_e, 0, sizeof s_e);
    s_vs_epoch = s_ps_epoch = 0xFFFFFFFFu;
    return s_d.up ? 0 : -1;
}

static void d3e_pipelines_release_all(void);

static void d3e_be_shutdown(void* user)
{
    (void)user;
    if (!s_d.up) return;
    d3e_submit(1);
    for (u32 i = 1; i < D3E_MAX_OBJ; i++)
        if (s_obj[i].kind != D3E_VIEW && s_obj[i].res) d3e_defer_release(s_obj[i].res);
    memset(s_obj, 0, sizeof s_obj);
    memset(s_sv_cache, 0, sizeof s_sv_cache);
    s_d.om_valid = 0;
    d3e_pipelines_release_all();
    for (u32 i = 0; i < D3E_MAX_QUERIES; i++) s_q[i].active = 0;
    s_q_active = 0;
    memset(&s_e, 0, sizeof s_e);
}

/* GUEST_REFERENCE only needs the GPU going (a readback that follows waits
 * itself); the rest wait, as the Vulkan backend does. */
static void d3e_be_submit_and_wait(void* user, u32 reason)
{
    (void)user;
    if (!s_d.up) return;
    d3e_submit(reason == RSX_BE_FLUSH_GUEST_REFERENCE ? 0 : 1);
}

/* ---- textures ------------------------------------------------------------------- */

static u32 d3e_be_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces,
                                 u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!s_d.up) return 0;
    return d3e_tex_new(fmt, w, h, 1, mips, faces, remap, rsx_fmt, 0);
}

static void d3e_be_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    d3e_obj* o = d3e_get(texture, D3E_TEX);
    if (!o || o->dim == 2 || mip >= o->mips || face >= o->layers) return;
    d3e_upload_sub(o, mip + face * o->mips, 0, 0, 0, w, h, src, row_bytes, rows);
}

static u32 d3e_be_texture_create_3d(void* user, rsx_be_format fmt, u32 w, u32 h, u32 depth, u32 mips,
                                    u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!s_d.up) return 0;
    return d3e_tex_new(fmt, w, h, depth, mips, 1, remap, rsx_fmt, 1);
}

static void d3e_be_texture_upload_3d(void* user, u32 texture, u32 z, u32 mip, u32 w, u32 h,
                                     const void* src, u32 row_bytes)
{
    (void)user;
    d3e_obj* o = d3e_get(texture, D3E_TEX);
    if (!o || o->dim != 2 || mip >= o->mips) return;
    u32 d = o->d >> mip;
    if (!d) d = 1;
    if (z >= d) return;
    const u32 rows = o->bc ? (h + 3u) / 4u : h;
    d3e_upload_sub(o, mip, 0, 0, z, w, h, src, row_bytes, rows);
}

static void d3e_be_release(void* user, u32 handle)
{
    (void)user;
    if (!s_d.up) return;
    d3e_release_obj(handle);
}

/* ---- colour targets ------------------------------------------------------------- */

static u32 d3e_be_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    if (!s_d.up) return 0;
    const u32 hd = d3e_color_new(fmt, w, h);
    if (!hd) return 0;
    d3e_obj* o = &s_obj[hd];
    /* RPCS3 render_target::clear_memory: opaque black, then the guest's own
     * bytes when the engine could resolve them (R,G,B,A rows). */
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(s_d.cl, d3e_rtv_cpu(hd), black, 0, NULL);
    if (seed && seed_row_bytes && fmt == RSX_BE_FMT_R8G8B8A8)
        d3e_upload_sub(o, 0, 0, 0, 0, w, h, seed, seed_row_bytes, h);
    else if (seed && seed_row_bytes && d3e_native16_kind(fmt))
        d3e_upload_rgba_to16(o, 0, 0, w, h, seed, seed_row_bytes);
    return hd;
}

static void d3e_be_color_target_write(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                                      const void* rows, u32 row_bytes)
{
    (void)user;
    d3e_obj* o = d3e_get(surface, D3E_COLOR);
    if (!o || !rows || !w || !h || x + w > o->w || y + h > o->h) return;
    if (d3e_native16_kind(o->fmt)) { d3e_upload_rgba_to16(o, x, y, w, h, rows, row_bytes); return; }   /* 16 / 8-bit */
    if (o->fmt != RSX_BE_FMT_R8G8B8A8) return;
    d3e_upload_sub(o, 0, x, y, 0, w, h, rows, row_bytes, h);
}

/* A colour target seen through a unit's crossbar: a VIEW of its resource,
 * found again through a small direct-mapped cache (the engine asks per draw). */
static u32 d3e_be_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    d3e_obj* t = d3e_get(surface, D3E_COLOR);
    if (!t) return 0;
    u32 key[3] = { surface, remap, rsx_format };
    const u32 slot = (u32)(d3e_hash(key, sizeof key, 0) % D3E_SV_CACHE);
    const u32 c = s_sv_cache[slot];
    if (c && c < D3E_MAX_OBJ && s_obj[c].kind == D3E_VIEW && s_obj[c].parent == surface &&
        s_obj[c].remap == remap && s_obj[c].rsx_fmt == rsx_format)
        return c;
    const u32 hd = d3e_alloc_obj();
    if (!hd) return 0;
    t = &s_obj[surface];
    d3e_obj* o = &s_obj[hd];
    o->kind = D3E_VIEW;
    o->parent = surface;
    o->remap = remap; o->rsx_fmt = rsx_format;
    o->fmt = t->fmt; o->rfmt = t->rfmt; o->vfmt = t->vfmt;
    o->w = t->w; o->h = t->h; o->d = 1; o->mips = 1; o->layers = 1; o->bpp = t->bpp;
    o->map = rsx_d3d12_cv_map(d3e_cv_kind(t->fmt), d3e_remap(remap, rsx_format));
    d3e_make_srv(hd, t->res, t->vfmt, 0, 1, o->map, 0);
    o->has_srv = 1;
    t->nviews++;
    s_sv_cache[slot] = hd;
    return hd;
}

#define D3E_TAG_SNORM 0xFFFFFFFFu
#define D3E_TAG_SRGB  0xFFFFFFFEu

static int d3e_fmt_sampleable(DXGI_FORMAT f)
{
    static DXGI_FORMAT seen[16];
    static int ok[16], n;
    for (int i = 0; i < n; i++) if (seen[i] == f) return ok[i];
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { f, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
    const int r = SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
                  (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
    if (!r) D3E_LOG("view format %d is not sampleable here: the shader keeps the conversion\n", (int)f);
    if (n < 16) { seen[n] = f; ok[n] = r; n++; }
    return r;
}

/* A twin view of a TYPELESS texture in format `vf`, cached in *cache. */
static u32 d3e_twin_view(u32 texture, DXGI_FORMAT vf, u32 tag, u32* cache_field)
{
    const u32 c = *cache_field;
    if (c && c < D3E_MAX_OBJ && s_obj[c].kind == D3E_VIEW && s_obj[c].parent == texture && s_obj[c].rsx_fmt == tag)
        return c;
    const u32 hd = d3e_alloc_obj();
    if (!hd) return 0;
    d3e_obj* t = &s_obj[texture];
    d3e_obj* o = &s_obj[hd];
    o->kind = D3E_VIEW;
    o->parent = texture;
    o->remap = 0; o->rsx_fmt = tag;
    o->fmt = t->fmt; o->rfmt = t->rfmt; o->vfmt = vf;
    o->dim = t->dim;
    o->w = t->w; o->h = t->h; o->d = t->d; o->mips = t->mips; o->layers = t->layers;
    o->bpp = t->bpp; o->bc = t->bc;
    o->map = d3e_remap(t->remap, t->rsx_fmt);
    d3e_make_srv(hd, t->res, vf, t->dim, t->mips, o->map, 0);
    o->has_srv = 1;
    t->nviews++;
    s_obj[texture].snorm_view = tag == D3E_TAG_SNORM ? hd : s_obj[texture].snorm_view;
    s_obj[texture].srgb_view = tag == D3E_TAG_SRGB ? hd : s_obj[texture].srgb_view;
    return hd;
}

/* RPCS3 VKDraw hw_SNORM_possible (get_compatible_snorm_format). */
static u32 d3e_be_texture_snorm_view(void* user, u32 texture)
{
    (void)user;
    d3e_obj* t = d3e_get(texture, D3E_TEX);
    if (!t || rsx_texture_legacy()) return 0;
    DXGI_FORMAT sf;
    switch (t->fmt) {
    case RSX_BE_FMT_R8:       sf = DXGI_FORMAT_R8_SNORM; break;
    case RSX_BE_FMT_R8G8:     sf = DXGI_FORMAT_R8G8_SNORM; break;
    case RSX_BE_FMT_R8G8B8A8: sf = DXGI_FORMAT_R8G8B8A8_SNORM; break;
    case RSX_BE_FMT_R16G16:   sf = DXGI_FORMAT_R16G16_SNORM; break;
    default: return 0;
    }
    if (!d3e_fmt_sampleable(sf)) return 0;
    return d3e_twin_view(texture, sf, D3E_TAG_SNORM, &t->snorm_view);
}

/* RPCS3 VKFormats get_compatible_srgb_format. D3D12 has no R8 / R8G8 sRGB
 * formats: those keep gamma in the shader. */
static DXGI_FORMAT d3e_srgb_twin(rsx_be_format f)
{
    switch (f) {
    case RSX_BE_FMT_R8G8B8A8: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case RSX_BE_FMT_BC1:      return DXGI_FORMAT_BC1_UNORM_SRGB;
    case RSX_BE_FMT_BC2:      return DXGI_FORMAT_BC2_UNORM_SRGB;
    case RSX_BE_FMT_BC3:      return DXGI_FORMAT_BC3_UNORM_SRGB;
    default:                  return DXGI_FORMAT_UNKNOWN;
    }
}

static u32 d3e_be_texture_srgb_view(void* user, u32 texture, rsx_be_format fmt)
{
    (void)user;
    if (!s_d.up || rsx_texture_legacy()) return 0;
    if (!texture) {
        const DXGI_FORMAT sf = d3e_srgb_twin(fmt);
        return sf != DXGI_FORMAT_UNKNOWN && d3e_fmt_sampleable(sf);
    }
    d3e_obj* t = d3e_get(texture, D3E_TEX);
    if (!t) return 0;
    const DXGI_FORMAT sf = d3e_srgb_twin(t->fmt);
    if (sf == DXGI_FORMAT_UNKNOWN || !d3e_fmt_sampleable(sf)) return 0;
    return d3e_twin_view(texture, sf, D3E_TAG_SRGB, &t->srgb_view);
}

/* ---- depth targets ------------------------------------------------------------- */

/* RPCS3 get_compatible_depth_surface_format: Z16 -> D16, Z24S8 -> D24S8, and
 * with depth_float D32 / D32S8. RSX_D3D12_DEPTH_LEGACY=1 makes every target
 * D24S8. */
static u32 d3e_be_depth_target_create_ex(void* user, u32 w, u32 h, u32 zfmt, u32 depth_float)
{
    (void)user;
    if (!s_d.up) return 0;
    static int legacy = -1;
    if (legacy < 0) legacy = d3e_env_on("RSX_D3D12_DEPTH_LEGACY", 0);
    u32 zcls = D3E_ZD24S8;
    if (!legacy) {
        if (zfmt == 1u) zcls = depth_float ? D3E_ZD32 : D3E_ZD16;
        else if (depth_float) zcls = D3E_ZD32S8;
    }
    const u32 hd = d3e_depth_new(w, h, zcls);
    if (hd) s_obj[hd].gz16 = (u8)(zfmt == 1u && !depth_float);
    return hd;
}

static u32 d3e_be_depth_target_create(void* user, u32 w, u32 h)
{
    return d3e_be_depth_target_create_ex(user, w, h, 2u, 0u);
}

/* The depth target's single R32F snapshot (refilled on every request,
 * released with the target), resolved with a full-screen pass. */
static u32 d3e_be_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    (void)user;
    d3e_obj* z = d3e_get(depth, D3E_DEPTH);
    if (!z) return 0;
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;
    u32 sn = z->dsnap;
    d3e_obj* t = d3e_get(sn, D3E_COLOR);
    if (!t || t->w != w || t->h != h) {
        if (t) { t->owned = 0; z->dsnap = 0; d3e_release_obj(sn); }
        z = &s_obj[depth];
        sn = d3e_color_new(RSX_BE_FMT_R32F, w, h);
        if (!sn) return 0;
        t = &s_obj[sn];
        t->owned = 1;
        z->dsnap = sn;
    }
    d3e_trans(z, d3e_srv_state(z));
    d3e_trans(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d3e_pass_args a;
    d3e_pass_args_init(&a, D3E_PS_DEPTH_R32F);
    a.rtf = DXGI_FORMAT_R32_FLOAT;
    a.rtv = d3e_rtv_cpu(sn);
    a.srv0 = depth;
    a.c[0] = 0.0f; a.c[1] = 0.0f;
    a.c[2] = (float)w / (float)z->w; a.c[3] = (float)h / (float)z->h;
    d3e_pass_target(&a, 0, 0, w, h);
    if (!d3e_pass_run(&a)) return 0;
    d3e_trans(t, D3E_SRV_STATES);
    return sn;
}

/* RPCS3 render_target::memory_barrier for a depth surface: depth (and
 * stencil where the shader can export it) of one target's rectangle onto
 * another's, point sampled. Same host format only, like the Vulkan backend. */
static int d3e_be_depth_target_copy(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                    u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)user;
    if (src == dst) return 0;
    d3e_obj* s = d3e_get(src, D3E_DEPTH);
    d3e_obj* d = d3e_get(dst, D3E_DEPTH);
    if (!s || !d || s->zcls != d->zcls) return 0;
    if (!rsx_d3d12_clip_blit(s->w, s->h, d->w, d->h, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh)) return 0;
    const int stencil = d->has_stencil && s->has_ssrv && s_d.pass_ps[D3E_PS_DEPTH_STENCIL_COPY];
    if (d->has_stencil && !stencil)
        D3E_WARN_ONCE(26, "depth target copy without stencil export: stencil left as it was\n");
    d3e_trans(s, d3e_srv_state(s));
    d3e_trans(d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    d3e_pass_args a;
    d3e_pass_args_init(&a, stencil ? D3E_PS_DEPTH_STENCIL_COPY : D3E_PS_DEPTH_COPY);
    a.mode = D3E_PM_DEPTH | (stencil ? (D3E_PM_STENCIL | (0xFFu << D3E_PM_SMASK_SHIFT)) : 0u);
    a.dsf = d3e_dsv_format(d->zcls);
    a.dsv = d3e_dsv_cpu(dst);
    a.srv0 = src;
    if (stencil) a.srv1 = D3E_SRV_STENCIL + src;
    a.c[0] = (float)sx / (float)s->w; a.c[1] = (float)sy / (float)s->h;
    a.c[2] = (float)sw / (float)s->w; a.c[3] = (float)sh / (float)s->h;
    a.c[6] = (float)s->w; a.c[7] = (float)s->h;
    d3e_pass_target(&a, dx, dy, dw, dh);
    return d3e_pass_run(&a);
}

/* ---- snapshots and copies --------------------------------------------------------- */

static u32 d3e_be_color_snapshot(void* user, u32 surface)
{
    (void)user;
    d3e_obj* t = d3e_get(surface, D3E_COLOR);
    return t ? d3e_snapshot(t, 0, 0, t->w, t->h) : 0;
}

static u32 d3e_be_color_region_snapshot(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    d3e_obj* t = d3e_get(surface, D3E_COLOR);
    if (!t || !w || !h || x >= t->w || y >= t->h || w > t->w - x || h > t->h - y) return 0;
    return d3e_snapshot(t, x, y, w, h);
}

/* RPCS3 cubemap_unwrap / _3d_unwrap: a face or slice copied out of a target. */
static int d3e_be_texture_copy_from_color(void* user, u32 texture, u32 layer, u32 mip,
                                          u32 surface, u32 sx, u32 sy, u32 w, u32 h)
{
    (void)user;
    d3e_obj* t = d3e_get(surface, D3E_COLOR);
    d3e_obj* o = d3e_get(texture, D3E_TEX);
    if (!t || !o || !w || !h || t->fmt != o->fmt || o->bc || mip >= o->mips ||
        sx >= t->w || sy >= t->h || w > t->w - sx || h > t->h - sy)
        return 0;
    u32 mw = o->w >> mip, mh = o->h >> mip, md = o->d >> mip;
    if (!mw) mw = 1;
    if (!mh) mh = 1;
    if (!md) md = 1;
    if (w > mw || h > mh) return 0;
    if (o->dim == 1 ? layer >= 6u : o->dim == 2 ? layer >= md : layer != 0u) return 0;
    d3e_trans(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d3e_trans(o, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl, sl;
    memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
    dl.pResource = o->res; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = mip + (o->dim == 1 ? layer * o->mips : 0u);
    sl.pResource = t->res; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { sx, sy, 0, sx + w, sy + h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(s_d.cl, &dl, 0, 0, o->dim == 2 ? layer : 0u, &sl, &box);
    return 1;
}

/* Scaled copy of a colour target rectangle onto another's through the blit
 * pass; flip bit 0 mirrors X, bit 1 Y. A copy within one target goes through
 * a snapshot of the source. */
static void d3e_blit(u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy, u32 dw, u32 dh,
                     int linear, u32 flip)
{
    d3e_obj* s = d3e_get(src, D3E_COLOR);
    d3e_obj* d = d3e_get(dst, D3E_COLOR);
    if (!s || !d) return;
    /* R8 / R8G8 targets hold B8 / G8B8 in other lanes than RGBA8 */
    if (s->fmt != d->fmt && (s->fmt == RSX_BE_FMT_R8 || s->fmt == RSX_BE_FMT_R8G8 ||
                             d->fmt == RSX_BE_FMT_R8 || d->fmt == RSX_BE_FMT_R8G8))
        return;
    if (!rsx_d3d12_clip_blit(s->w, s->h, d->w, d->h, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh)) return;
    const u32 sW = s->w, sH = s->h;
    u32 srv = src, snap = 0;
    if (src == dst) {
        snap = d3e_snapshot(s, 0, 0, sW, sH);
        if (!snap) return;
        srv = snap;
        s = &s_obj[snap];
        d = &s_obj[dst];
    }
    if (linear && !d3e_filterable(s->vfmt)) linear = 0;
    d3e_trans(s, D3E_SRV_STATES);
    d3e_trans(d, D3D12_RESOURCE_STATE_RENDER_TARGET);
    d3e_pass_args a;
    d3e_pass_args_init(&a, linear ? D3E_PS_BLIT_LINEAR : D3E_PS_BLIT_POINT);
    a.rtf = d->rfmt;
    a.rtv = d3e_rtv_cpu(dst);
    a.srv0 = srv;
    float u0 = (float)sx / (float)sW, v0 = (float)sy / (float)sH;
    float du = (float)sw / (float)sW, dv = (float)sh / (float)sH;
    if (flip & 1u) { u0 += du; du = -du; }
    if (flip & 2u) { v0 += dv; dv = -dv; }
    a.c[0] = u0; a.c[1] = v0; a.c[2] = du; a.c[3] = dv;
    d3e_pass_target(&a, dx, dy, dw, dh);
    d3e_pass_run(&a);
    if (snap) d3e_release_obj(snap);
}

static void d3e_be_color_target_blit(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                     u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)user;
    if (src == dst) return;     /* the Vulkan backend's rule for the unfiltered entry */
    d3e_blit(src, sx, sy, sw, sh, dst, dx, dy, dw, dh, 1, 0);
}

static void d3e_be_color_target_blit_filtered(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                              u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear)
{
    (void)user;
    d3e_blit(src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear, 0);
}

static void d3e_be_color_target_blit_flip(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                          u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear, u32 flip)
{
    (void)user;
    d3e_blit(src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear, flip & 3u);
}

/* ---- clears --------------------------------------------------------------------- */

static void d3e_be_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    d3e_obj* o = d3e_get(surface, D3E_COLOR);
    if (!o || !rgba) return;
    d3e_trans(o, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float cv[4];
    rsx_d3d12_cv_clear(d3e_cv_kind(o->fmt), rgba, cv);
    ID3D12GraphicsCommandList_ClearRenderTargetView(s_d.cl, d3e_rtv_cpu(surface), cv, 0, NULL);
}

static void d3e_be_clear_depth_stencil(void* user, u32 depth, u32 flags, float depth_value, u8 stencil)
{
    (void)user;
    d3e_obj* o = d3e_get(depth, D3E_DEPTH);
    if (!o) return;
    D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)0;
    if (flags & RSX_BE_CLEAR_DEPTH) f |= D3D12_CLEAR_FLAG_DEPTH;
    if ((flags & RSX_BE_CLEAR_STENCIL) && o->has_stencil) f |= D3D12_CLEAR_FLAG_STENCIL;
    if (!f) return;
    d3e_trans(o, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, d3e_dsv_cpu(depth), f, d3e_clamp01(depth_value),
                                                    stencil, 0, NULL);
}

/* RPCS3 VKGSRender::clear_surface: the scissored rectangle, with a colour
 * mask or stencil write mask taking the masked-clear pass. */
static void d3e_be_clear_rect(void* user, const u32* rts, u32 n_rts, u32 depth, const float rgba[4],
                              u32 color_mask, u32 ds_flags, float depth_value, u32 stencil, u32 stencil_mask,
                              u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    if (!w || !h) return;
    if (n_rts > RSX_BE_MAX_COLOR_TARGETS) n_rts = RSX_BE_MAX_COLOR_TARGETS;
    color_mask &= 0xFu;
    for (u32 i = 0; rts && rgba && color_mask && i < n_rts; i++) {
        d3e_obj* o = d3e_get(rts[i], D3E_COLOR);
        if (!o || x >= o->w || y >= o->h) continue;
        const u32 cw = w < o->w - x ? w : o->w - x, ch = h < o->h - y ? h : o->h - y;
        d3e_trans(o, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const int ck = d3e_cv_kind(o->fmt);
        const u32 cm = rsx_d3d12_cv_write_mask(ck, color_mask);
        float cv[4];
        rsx_d3d12_cv_clear(ck, rgba, cv);
        if (!cm) continue;
        if (cm == 0xFu) {
            D3D12_RECT r = { (LONG)x, (LONG)y, (LONG)(x + cw), (LONG)(y + ch) };
            ID3D12GraphicsCommandList_ClearRenderTargetView(s_d.cl, d3e_rtv_cpu(rts[i]), cv, 1, &r);
        } else {
            d3e_pass_args a;
            d3e_pass_args_init(&a, D3E_PS_CLEAR);
            a.mode = cm;             /* bit0 R, bit1 G, bit2 B, bit3 A: D3D12's order too */
            a.rtf = o->rfmt;
            a.rtv = d3e_rtv_cpu(rts[i]);
            memcpy(&a.c[8], cv, 4 * sizeof(float));
            d3e_pass_target(&a, x, y, cw, ch);
            d3e_pass_run(&a);
        }
    }
    d3e_obj* z = d3e_get(depth, D3E_DEPTH);
    if (!z || x >= z->w || y >= z->h) return;
    const int cd = (ds_flags & RSX_BE_CLEAR_DEPTH) != 0;
    const int cs = (ds_flags & RSX_BE_CLEAR_STENCIL) && z->has_stencil && (stencil_mask & 0xFFu);
    if (!cd && !cs) return;
    const u32 zw = w < z->w - x ? w : z->w - x, zh = h < z->h - y ? h : z->h - y;
    d3e_trans(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (cs && (stencil_mask & 0xFFu) != 0xFFu) {
        d3e_pass_args a;
        d3e_pass_args_init(&a, D3E_PS_CLEAR);
        a.mode = (cd ? D3E_PM_DEPTH : 0u) | D3E_PM_STENCIL | ((stencil_mask & 0xFFu) << D3E_PM_SMASK_SHIFT);
        a.dsf = d3e_dsv_format(z->zcls);
        a.dsv = d3e_dsv_cpu(depth);
        a.c[12] = d3e_clamp01(depth_value);
        a.stencil_ref = stencil & 0xFFu;
        d3e_pass_target(&a, x, y, zw, zh);
        d3e_pass_run(&a);
        return;
    }
    D3D12_RECT r = { (LONG)x, (LONG)y, (LONG)(x + zw), (LONG)(y + zh) };
    D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)((cd ? D3D12_CLEAR_FLAG_DEPTH : 0) | (cs ? D3D12_CLEAR_FLAG_STENCIL : 0));
    ID3D12GraphicsCommandList_ClearDepthStencilView(s_d.cl, d3e_dsv_cpu(depth), f, d3e_clamp01(depth_value),
                                                    (UINT8)(stencil & 0xFFu), 1, &r);
}

/* ---- readbacks ------------------------------------------------------------------- */

static void d3e_be_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h, void* out, u32 out_pitch)
{
    (void)user;
    d3e_obj* o = d3e_get(surface, D3E_COLOR);
    if (!o || !out || !out_pitch) return;
    if (d3e_native16_kind(o->fmt)) { d3e_read_rect16(o, x, y, w, h, out, out_pitch); return; }
    d3e_read_rect(o, x, y, w, h, out, out_pitch);
}

/* Depth plane texel i of a row as a float in [0, 1]. */
static float d3e_depth_texel(u32 zcls, const u8* row, u32 i)
{
    if (zcls == D3E_ZD16) { u16 v; memcpy(&v, row + (size_t)i * 2u, 2); return (float)v / 65535.0f; }
    u32 v; memcpy(&v, row + (size_t)i * 4u, 4);
    if (zcls == D3E_ZD24S8) return (float)((double)(v & 0xFFFFFFu) / 16777215.0);
    float f; memcpy(&f, &v, 4);
    return f;
}

static int d3e_be_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, float* out, u32 out_pitch)
{
    (void)user;
    d3e_obj* z = d3e_get(depth, D3E_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h || out_pitch < w * 4u) return -1;
    u32 pitch = 0, texel = 0;
    const u8* p = d3e_read_depth_plane(z, 0, &pitch, &texel);
    if (!p) return -1;
    const u32 zc = s_obj[depth].zcls;
    for (u32 r = 0; r < h; r++) {
        const u8* row = p + (size_t)(y + r) * pitch + (size_t)x * texel;
        float* d = (float*)((u8*)out + (size_t)r * out_pitch);
        for (u32 i = 0; i < w; i++) d[i] = d3e_depth_texel(zc, row, i);
    }
    d3e_rb_unmap();
    return 0;
}

static int d3e_be_stencil_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, u8* out, u32 out_pitch)
{
    (void)user;
    d3e_obj* z = d3e_get(depth, D3E_DEPTH);
    if (!z || !z->has_stencil || !out || !w || !h || x + w > z->w || y + h > z->h || out_pitch < w) return -1;
    u32 pitch = 0, texel = 0;
    const u8* p = d3e_read_depth_plane(z, 1, &pitch, &texel);
    if (!p) return -1;
    if (!texel) texel = 1;
    for (u32 r = 0; r < h; r++) {
        const u8* row = p + (size_t)(y + r) * pitch + (size_t)x * texel;
        u8* d = out + (size_t)r * out_pitch;
        if (texel == 1) memcpy(d, row, w);
        else for (u32 i = 0; i < w; i++) d[i] = row[(size_t)i * texel];
    }
    d3e_rb_unmap();
    return 0;
}

/* Both aspects, D16 / D24 handed back raw (rows tight). Two plane copies:
 * a depth-stencil resource cannot copy a box, so each is a whole plane. */
static int d3e_be_zeta_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                float* out_f, u32* out_raw, u32* raw_bits, u8* out_stencil, int* have_stencil)
{
    (void)user;
    if (raw_bits) *raw_bits = 0;
    if (have_stencil) *have_stencil = 0;
    d3e_obj* z = d3e_get(depth, D3E_DEPTH);
    if (!z || !out_f || !raw_bits || !have_stencil || !w || !h || x + w > z->w || y + h > z->h) return -1;
    const u32 zc = z->zcls;
    const int raw = out_raw && (zc == D3E_ZD16 || zc == D3E_ZD24S8);
    u32 pitch = 0, texel = 0;
    const u8* p = d3e_read_depth_plane(z, 0, &pitch, &texel);
    if (!p) return -1;
    for (u32 r = 0; r < h; r++) {
        const u8* row = p + (size_t)(y + r) * pitch + (size_t)x * texel;
        if (raw) {
            u32* d = out_raw + (size_t)r * w;
            for (u32 i = 0; i < w; i++) {
                if (zc == D3E_ZD16) { u16 v; memcpy(&v, row + (size_t)i * 2u, 2); d[i] = v; }
                else { u32 v; memcpy(&v, row + (size_t)i * 4u, 4); d[i] = v & 0xFFFFFFu; }
            }
        } else {
            float* d = out_f + (size_t)r * w;
            for (u32 i = 0; i < w; i++) d[i] = d3e_depth_texel(zc, row, i);
        }
    }
    d3e_rb_unmap();
    if (raw) *raw_bits = zc == D3E_ZD16 ? 16u : 24u;
    if (out_stencil && s_obj[depth].has_stencil &&
        d3e_be_stencil_readback(NULL, depth, x, y, w, h, out_stencil, w) == 0)
        *have_stencil = 1;
    return 0;
}

/* ---- present --------------------------------------------------------------------- */

static void d3e_be_present(void* user, u32 surface)
{
    (void)user;
    d3e_present_region(surface, 0, 0, 0, 0, 0, 0);
}

static void d3e_be_present_region(void* user, u32 surface, u32 sx, u32 sy, u32 sw, u32 sh, u32 aw, u32 ah)
{
    (void)user;
    d3e_present_region(surface, sx, sy, sw, sh, aw, ah);
}

static void d3e_pipelines_release_all(void)
{
    for (u32 i = 0; i < s_pipe_cap; i++)
        if (s_pipe[i].used) d3e_pipeline_release(NULL, i + 1u);
}

/* ---- the backend table ------------------------------------------------------------ */

static const rsx_draw_backend s_d3e_backend = {
    .user = NULL,
    .init = d3e_be_init,
    .shutdown = d3e_be_shutdown,
    .submit_and_wait = d3e_be_submit_and_wait,
    .texture_create = d3e_be_texture_create,
    .texture_upload = d3e_be_texture_upload,
    .texture_release = d3e_be_release,
    .color_target_create = d3e_be_color_target_create,
    .color_target_release = d3e_be_release,
    .color_target_write = d3e_be_color_target_write,
    .surface_view = d3e_be_surface_view,
    .depth_target_create = d3e_be_depth_target_create,
    .depth_target_release = d3e_be_release,
    .depth_snapshot = d3e_be_depth_snapshot,
    .pipeline_create = d3e_pipeline_create,
    .pipeline_release = d3e_pipeline_release,
    .bind_targets = d3e_bind_targets,
    .bind_pipeline = d3e_bind_pipeline,
    .bind_vs_constants = d3e_bind_vs_constants,
    .bind_ps_constants = d3e_bind_ps_constants,
    .bind_textures = d3e_bind_textures,
    .bind_vertex_textures = d3e_bind_vertex_textures,
    .set_viewport = d3e_set_viewport,
    .set_scissor = d3e_set_scissor,
    .set_stencil_ref = d3e_set_stencil_ref,
    .set_depth_bounds = d3e_set_depth_bounds,
    .set_blend_constants = d3e_set_blend_constants,
    .draw = d3e_draw,
    .clear_color = d3e_be_clear_color,
    .clear_depth_stencil = d3e_be_clear_depth_stencil,
    .present = d3e_be_present,
    .readback = d3e_be_readback,
    .depth_readback = d3e_be_depth_readback,
    .stencil_readback = d3e_be_stencil_readback,
    .color_snapshot = d3e_be_color_snapshot,
    .texture_create_3d = d3e_be_texture_create_3d,
    .texture_upload_3d = d3e_be_texture_upload_3d,
    .color_region_snapshot = d3e_be_color_region_snapshot,
    .texture_copy_from_color = d3e_be_texture_copy_from_color,
    .color_target_blit = d3e_be_color_target_blit,
    .present_region = d3e_be_present_region,
    .surface_reinterpret = NULL,
    .color_target_blit_filtered = d3e_be_color_target_blit_filtered,
    .occlusion_begin = d3e_occlusion_begin,
    .occlusion_end = d3e_occlusion_end,
    .occlusion_result = d3e_occlusion_result,
    .occlusion_discard = d3e_occlusion_discard,
    .set_raster_dynamic = d3e_set_raster_dynamic,
    .texture_snorm_view = d3e_be_texture_snorm_view,
    .texture_srgb_view = d3e_be_texture_srgb_view,
    .set_depth_range = d3e_set_depth_range,
    .pipeline_async = d3e_pipeline_async,
    .pipeline_prewarm = d3e_pipeline_prewarm,
    .depth_target_create_ex = d3e_be_depth_target_create_ex,
    .target_set_aa = NULL,
    .scene_aa = d3e_scene_aa,
    .clear_rect = d3e_be_clear_rect,
    .point_sprite_supported = d3e_be_point_sprite_supported,
    .depth_target_copy = d3e_be_depth_target_copy,
    .surface_typeless_copy = NULL,
    .color_target_blit_flip = d3e_be_color_target_blit_flip,
    .color_format_supported = d3e_be_color_format_supported,
    .zeta_readback = d3e_be_zeta_readback,
};

/* ---- device ------------------------------------------------------------------------ */

/* RSX_D3D12_DEBUG=1: what DRED recorded when the device was removed. */
static void d3e_dred_report(void)
{
    if (!s_d.dev) return;
    const HRESULT why = ID3D12Device_GetDeviceRemovedReason(s_d.dev);
    if (why == S_OK) return;
    D3E_LOG("device removed: reason 0x%08lX\n", (unsigned long)why);
    ID3D12DeviceRemovedExtendedData* dred = NULL;
    if (FAILED(ID3D12Device_QueryInterface(s_d.dev, &IID_ID3D12DeviceRemovedExtendedData, (void**)&dred)) || !dred)
        return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc;
    memset(&bc, 0, sizeof bc);
    if (SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetAutoBreadcrumbsOutput(dred, &bc))) {
        u32 n = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* nd = bc.pHeadAutoBreadcrumbNode; nd && n < 16; nd = nd->pNext, n++) {
            const u32 done = nd->pLastBreadcrumbValue ? *nd->pLastBreadcrumbValue : 0u;
            D3E_LOG("DRED list %u: %u of %u operations completed", n, done, nd->BreadcrumbCount);
            if (done < nd->BreadcrumbCount && nd->pCommandHistory)
                fprintf(stderr, " (next op %d)", (int)nd->pCommandHistory[done]);
            fprintf(stderr, "\n");
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf;
    memset(&pf, 0, sizeof pf);
    if (SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetPageFaultAllocationOutput(dred, &pf)) && pf.PageFaultVA)
        D3E_LOG("DRED page fault at GPU VA 0x%llX\n", (unsigned long long)pf.PageFaultVA);
    ID3D12DeviceRemovedExtendedData_Release(dred);
}

static void d3e_device_destroy(void)
{
    d3e_async_stop();
    if (s_d.dev && s_d.queue && s_d.fence) {
        if (s_d.cl) {
            d3e_query_close_open();
            if (SUCCEEDED(ID3D12GraphicsCommandList_Close(s_d.cl))) {
                ID3D12CommandList* l = (ID3D12CommandList*)s_d.cl;
                ID3D12CommandQueue_ExecuteCommandLists(s_d.queue, 1, &l);
            }
        }
        s_d.fence_val++;
        ID3D12CommandQueue_Signal(s_d.queue, s_d.fence, s_d.fence_val);
        d3e_wait_fence(s_d.fence_val);
        d3e_dred_report();
    }
    s_d.up = 0;
    d3e_fg_swap_down();       /* waits for FFX's present queue before its inputs (HUD-less image) go */
    d3e_ua_device_down();
    d3e_lat_device_down();
    d3e_ms_device_down();
    d3e_osd_device_down();
    d3e_pipelines_release_all();
    for (u32 i = 1; i < D3E_MAX_OBJ; i++)
        if (s_obj[i].res) d3e_defer_release(s_obj[i].res);
    memset(s_obj, 0, sizeof s_obj);
    for (u32 i = 0; i < s_dead_n; i++) IUnknown_Release(s_dead[i].p);
    s_dead_n = 0;
    for (u32 i = 0; i < s_ppso_n; i++) d3e_release(s_ppso[i].pso);
    s_ppso_n = 0;
    s_stab_n = 0;
    s_stab_last[0].n = s_stab_last[1].n = 0;
    memset(&s_rc, 0, sizeof s_rc);
    d3e_blob_cache_clear();
    memset(s_sv_cache, 0, sizeof s_sv_cache);
    if (s_d.swap) {
        d3e_swap_release_buffers();
        d3e_fg_swap_down();
        IDXGISwapChain3_Release(s_d.swap);
        s_d.swap = NULL;
    }
    if (s_d.upload) ID3D12Resource_Unmap(s_d.upload, 0, NULL);
    if (s_d.qrb) ID3D12Resource_Unmap(s_d.qrb, 0, NULL);
    d3e_plib_close();
    void* objs[] = {
        s_d.rs_main, s_d.rs_pass, s_d.pass_vs, s_d.zero_vb, s_d.scratch, s_d.qheap, s_d.qrb,
        s_d.upload, s_d.rb, s_d.srv_cpu, s_d.rtv, s_d.dsv, s_d.srv_gpu, s_d.samp_gpu,
        s_d.cl8, s_d.cl1, s_d.cl, s_d.alloc[0], s_d.alloc[1], s_d.fence, s_d.queue,
        s_d.dev2, s_d.dev, s_d.adapter, s_d.factory,
    };
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) d3e_release(objs[i]);
    for (u32 i = 0; i < D3E_PS_COUNT; i++) d3e_release(s_d.pass_ps[i]);
    if (s_d.fence_event) CloseHandle(s_d.fence_event);
    const HWND hwnd = s_d.hwnd;
    const u64 warned = s_d.warned;
    memset(&s_d, 0, sizeof s_d);
    s_d.hwnd = hwnd;
    s_d.warned = warned;
    memset(s_q, 0, sizeof s_q);
    memset(s_q_unknown, 0, sizeof s_q_unknown);
    memset(s_q_scaled, 0, sizeof s_q_scaled);
    s_q_active = 0;
}

static ID3D12DescriptorHeap* d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE t, u32 n, int shader_visible)
{
    D3D12_DESCRIPTOR_HEAP_DESC d = { t, n, shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                          : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ID3D12DescriptorHeap* h = NULL;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(s_d.dev, &d, &IID_ID3D12DescriptorHeap, (void**)&h))) return NULL;
    return h;
}

/* RSX_D3D12_ADAPTER=<n>, else the high-performance hardware adapter that
 * makes a feature level 11_0 device. */
static IDXGIAdapter1* d3e_pick_adapter(IDXGIFactory4* f)
{
    IDXGIAdapter1* a = NULL;
    const char* e = getenv("RSX_D3D12_ADAPTER");
    if (e && *e) {
        if (SUCCEEDED(IDXGIFactory4_EnumAdapters1(f, (UINT)atoi(e), &a)) && a) return a;
        D3E_LOG("RSX_D3D12_ADAPTER=%s: no such adapter, picking one\n", e);
    }
    IDXGIFactory6* f6 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(f, &IID_IDXGIFactory6, (void**)&f6)) && f6) {
        for (UINT i = 0; ; i++) {
            a = NULL;
            if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                &IID_IDXGIAdapter1, (void**)&a)) || !a)
                break;
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice((IUnknown*)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL))) {
                IDXGIFactory6_Release(f6);
                return a;
            }
            IDXGIAdapter1_Release(a);
        }
        IDXGIFactory6_Release(f6);
    }
    for (UINT i = 0; ; i++) {
        a = NULL;
        if (FAILED(IDXGIFactory4_EnumAdapters1(f, i, &a)) || !a) break;
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice((IUnknown*)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL)))
            return a;
        IDXGIAdapter1_Release(a);
    }
    return NULL;
}

static int d3e_device_create(u32 width, u32 height)
{
    const int debug = d3e_env_on("RSX_D3D12_DEBUG", 0);
    if (debug) {
        ID3D12Debug* dbg = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg)) && dbg) {
            ID3D12Debug_EnableDebugLayer(dbg);
            ID3D12Debug_Release(dbg);
            D3E_LOG("debug layer on\n");
        } else D3E_LOG("RSX_D3D12_DEBUG: the debug layer is not installed (Graphics Tools)\n");
        ID3D12DeviceRemovedExtendedDataSettings* dr = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&dr)) && dr) {
            ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(dr, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(dr, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ID3D12DeviceRemovedExtendedDataSettings_Release(dr);
        }
    }
    if (FAILED(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0u, &IID_IDXGIFactory4, (void**)&s_d.factory)) &&
        FAILED(CreateDXGIFactory2(0u, &IID_IDXGIFactory4, (void**)&s_d.factory))) {
        D3E_LOG("CreateDXGIFactory2 failed\n");
        return -1;
    }
    s_d.adapter = d3e_pick_adapter(s_d.factory);
    if (!s_d.adapter) { D3E_LOG("no hardware adapter with feature level 11_0\n"); return -1; }
    DXGI_ADAPTER_DESC1 ad;
    memset(&ad, 0, sizeof ad);
    IDXGIAdapter1_GetDesc1(s_d.adapter, &ad);
    s_d.vendor = ad.VendorId;
    if (FAILED(D3D12CreateDevice((IUnknown*)s_d.adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&s_d.dev))) {
        D3E_LOG("D3D12CreateDevice failed\n");
        return -1;
    }
    {
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
        D3E_LOG("adapter: %s (vendor 0x%04X, %llu MB)\n", name, ad.VendorId,
                (unsigned long long)(ad.DedicatedVideoMemory >> 20));
    }
    ID3D12Device_QueryInterface(s_d.dev, &IID_ID3D12Device2, (void**)&s_d.dev2);

    /* capabilities */
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o;
        memset(&o, 0, sizeof o);
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
        {
            D3E_LOG("PS stencil ref %s\n", o.PSSpecifiedStencilRefSupported ? "yes" : "no");
            /* D3D12 logic ops exist only on UINT render targets (OutputMergerLogicOp); the guest's
             * targets are UNORM views here, so XOR / NAND / INVERT & co. stay on the warn path
             * (rsx_d3d12_logic_blend: CLEAR / COPY / NOOP exact, AND / OR family blended). */
            D3E_LOG("output-merger logic ops %s (not used: UNORM targets; see rsx_d3d12_logic_blend)\n",
                    o.OutputMergerLogicOp ? "supported" : "unsupported");
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2;
        memset(&o2, 0, sizeof o2);
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof o2)))
            s_d.depth_bounds_ok = o2.DepthBoundsTestSupported ? 1 : 0;
        rsx_d3d12_options13 o13;
        memset(&o13, 0, sizeof o13);
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, RSX_D3D12_FEATURE_OPTIONS13, &o13, sizeof o13)))
            s_d.alpha_factor_ok = o13.AlphaBlendFactorSupported ? 1 : 0;
        rsx_d3d12_options14 o14;
        memset(&o14, 0, sizeof o14);
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, RSX_D3D12_FEATURE_OPTIONS14, &o14, sizeof o14)))
            s_d.sep_stencil_ok = s_d.stencil_ref_ok = o14.IndependentFrontAndBackStencilRefMaskSupported ? 1 : 0;
        IDXGIFactory5* f5 = NULL;
        if (SUCCEEDED(IDXGIFactory4_QueryInterface(s_d.factory, &IID_IDXGIFactory5, (void**)&f5)) && f5) {
            BOOL t = FALSE;
            if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t)))
                s_d.tearing_ok = t ? 1 : 0;
            IDXGIFactory5_Release(f5);
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { DXGI_FORMAT_R32_FLOAT, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)))
            s_d.blendable_r32f = (fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
        fs.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        fs.Support1 = D3D12_FORMAT_SUPPORT1_NONE; fs.Support2 = D3D12_FORMAT_SUPPORT2_NONE;
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(s_d.dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)))
            s_d.blendable_rgba32f = (fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
        D3E_LOG("caps: depth bounds %d, alpha blend factor %d, front/back stencil %d, tearing %d, blend R32F %d RGBA32F %d\n",
                s_d.depth_bounds_ok, s_d.alpha_factor_ok, s_d.sep_stencil_ok, s_d.tearing_ok,
                s_d.blendable_r32f, s_d.blendable_rgba32f);
    }

    /* queue, allocators, list, fence */
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    if (FAILED(ID3D12Device_CreateCommandQueue(s_d.dev, &qd, &IID_ID3D12CommandQueue, (void**)&s_d.queue))) return -1;
    for (u32 i = 0; i < 2; i++)
        if (FAILED(ID3D12Device_CreateCommandAllocator(s_d.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       &IID_ID3D12CommandAllocator, (void**)&s_d.alloc[i])))
            return -1;
    if (FAILED(ID3D12Device_CreateCommandList(s_d.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_d.alloc[0], NULL,
                                              &IID_ID3D12GraphicsCommandList, (void**)&s_d.cl)))
        return -1;
    ID3D12GraphicsCommandList_QueryInterface(s_d.cl, &IID_ID3D12GraphicsCommandList1, (void**)&s_d.cl1);
    ID3D12GraphicsCommandList_QueryInterface(s_d.cl, &rsx_d3d12_iid_cl8, (void**)&s_d.cl8);
    if (FAILED(ID3D12Device_CreateFence(s_d.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_d.fence)))
        return -1;
    s_d.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_d.fence_event) return -1;

    /* descriptor heaps */
    s_d.srv_cpu = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3E_SRV_CPU_COUNT, 0);
    s_d.rtv = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3E_RTV_COUNT, 0);
    s_d.dsv = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3E_DSV_COUNT, 0);
    s_d.srv_gpu = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2u * D3E_GPU_DESC_HALF, 1);
    s_d.samp_gpu = d3e_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3E_SAMP_HEAP, 1);
    if (!s_d.srv_cpu || !s_d.rtv || !s_d.dsv || !s_d.srv_gpu || !s_d.samp_gpu) {
        D3E_LOG("descriptor heap creation failed\n");
        return -1;
    }
    s_d.srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_d.rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d.dsv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    s_d.samp_inc = ID3D12Device_GetDescriptorHandleIncrementSize(s_d.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_d.srv_cpu, &s_d.srv_cpu0);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_d.rtv, &s_d.rtv0);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_d.dsv, &s_d.dsv0);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_d.srv_gpu, &s_d.srv_gpu_cpu0);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s_d.samp_gpu, &s_d.samp_cpu0);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(s_d.srv_gpu, &s_d.srv_gpu0);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(s_d.samp_gpu, &s_d.samp_gpu0);

    /* upload ring, zero vertex, occlusion */
    s_d.upload = d3e_buffer(2ull * D3E_UPLOAD_HALF, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!s_d.upload) { D3E_LOG("upload ring (%u MB) could not be created\n", (2u * D3E_UPLOAD_HALF) >> 20); return -1; }
    {
        void* p = NULL;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(ID3D12Resource_Map(s_d.upload, 0, &none, &p)) || !p) return -1;
        s_d.upload_cpu = (u8*)p;
        s_d.upload_gpu = ID3D12Resource_GetGPUVirtualAddress(s_d.upload);
    }
    s_d.zero_vb = d3e_buffer(64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!s_d.zero_vb) return -1;
    {
        void* p = NULL;
        D3D12_RANGE none = { 0, 0 };
        if (SUCCEEDED(ID3D12Resource_Map(s_d.zero_vb, 0, &none, &p)) && p) {
            memset(p, 0, 64);
            ID3D12Resource_Unmap(s_d.zero_vb, 0, NULL);
        }
    }
    {
        D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, D3E_QUERY_SLOTS, 0 };
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(s_d.dev, &qh, &IID_ID3D12QueryHeap, (void**)&s_d.qheap))) {
            s_d.qrb = d3e_buffer((u64)D3E_QUERY_SLOTS * 8u, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            void* p = NULL;
            if (!s_d.qrb || FAILED(ID3D12Resource_Map(s_d.qrb, 0, NULL, &p)) || !p) {
                d3e_release(s_d.qrb); s_d.qrb = NULL;
                d3e_release(s_d.qheap); s_d.qheap = NULL;
            } else s_d.qrb_cpu = (u64*)p;
        }
        if (!s_d.qheap) D3E_LOG("occlusion queries unavailable: the engine reports everything visible\n");
    }

    if (d3e_rs_main_create() != 0) { D3E_LOG("guest root signature failed\n"); return -1; }
    d3e_plib_open();
    if (d3e_pass_init() != 0) { D3E_LOG("internal pass shaders failed to compile\n"); return -1; }
    d3e_null_srvs();
    d3e_list_begin_state();
    s_d.width = width; s_d.height = height;
    s_d.up = 1;
    d3e_ua_device_up();
    d3e_lat_device_up();
    return 0;
}

/* ---- public API ---------------------------------------------------------------------- */

/* A flip-model swap chain on a hidden window of the probe device, presented
 * once: the step most likely to fail on odd setups (remote sessions, old
 * drivers, a broken DXGI). */
static int d3e_probe_swap(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "rsx_d3d12_probe";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!w) { D3E_LOG("probe: no window for the swap chain test (skipped)\n"); return 1; }
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = 64; sd.Height = 64;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = D3E_SWAP_BUFFERS;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(s_d.factory, (IUnknown*)s_d.queue, w, &sd, NULL, NULL, &sc);
    int ok = SUCCEEDED(hr) && sc;
    if (!ok) D3E_LOG("probe: CreateSwapChainForHwnd failed 0x%08lX\n", (unsigned long)hr);
    else {
        ID3D12Resource* b = NULL;
        ok = SUCCEEDED(IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D12Resource, (void**)&b)) && b;
        if (b) ID3D12Resource_Release(b);
        if (!ok) D3E_LOG("probe: swap chain buffers unavailable\n");
        IDXGISwapChain1_Release(sc);
    }
    DestroyWindow(w);
    return ok;
}

/* The full device bring-up (queue, heaps, ring, root signatures, the
 * internal pass shaders through FXC) plus a swap chain, torn down again, so
 * the host can still pick another renderer before the first GCM call. */
int rsx_d3d12_engine_probe(void)
{
    static int r = -1;
    if (r >= 0) return r;
    if (s_d.up) return r = 1;
    r = 0;
    s_d3e_probing = 1;
    const HWND keep = s_d.hwnd;
    if (d3e_device_create(64, 64) != 0) D3E_LOG("probe: Direct3D 12 device bring-up failed\n");
    else r = d3e_probe_swap();
    d3e_device_destroy();
    s_d.hwnd = keep;
    s_d.warned = 0;
    s_d3e_probing = 0;
    D3E_LOG("probe: %s\n", r ? "Direct3D 12 usable" : "Direct3D 12 unusable");
    return r;
}

/* Late failure (device creation failed after the renderer was chosen): start
 * this executable again with R2_RSX=rpcs3 and the same command line, once. */
int rsx_d3d12_engine_relaunch_compatible(void)
{
    if (getenv("R2_D3D12_FALLBACK_RELAUNCH")) return 0;     /* we are the relaunch */
    rsx_d3d12_env_to_rpcs3();     /* R2_RSX=rpcs3, RSX_FSR renumbered */
    _putenv("R2_D3D12_FALLBACK_RELAUNCH=1");
    WCHAR exe[MAX_PATH + 2];
    const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    exe[n] = 0;
    const WCHAR* cl = GetCommandLineW();
    const size_t cn = wcslen(cl) + 1u;
    WCHAR* cmd = (WCHAR*)malloc(cn * sizeof(WCHAR));
    if (!cmd) return 0;
    memcpy(cmd, cl, cn * sizeof(WCHAR));
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    const BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) { D3E_LOG("relaunch with the Compatible renderer failed (%lu)\n", (unsigned long)GetLastError()); return 0; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    D3E_LOG("Direct3D 12 failed to start: relaunched with the Compatible (RPCS3) renderer\n");
    return 1;
}

void rsx_d3d12_engine_set_window(void* hwnd)
{
    s_d.hwnd = (HWND)hwnd;
}

static int s_eng_active;

int rsx_d3d12_engine_init(uint32_t width, uint32_t height, const char* title)
{
    if (s_d.up) return 0;
    if (!width) width = 1280;
    if (!height) height = 720;
    if (d3e_device_create(width, height) != 0) {
        D3E_LOG("device creation failed\n");
        d3e_device_destroy();
        return -1;
    }
    const int nv = s_d.vendor == 0x10DEu;
    rsx_fp_set_res_scale(1.0);
    rsx_fp_set_res_scale_dynamic(s_up.on ? 1 : 0);
    rsx_vp_set_low_precision_compare(nv);
    rsx_draw_engine_set_host_nvidia(nv);
    rsx_vp_set_clip_distances(1);
    /* D3D12 static/descriptor border colours ignore the SRV's component mapping */
    rsx_draw_engine_set_border_color_remap(1);
    rsx_draw_engine_set_last_provoking_vertex(0);
    static rsx_draw_backend up_be;
    const int ms_wrap = d3e_ms_setup();
    if (s_up.on) {
        up_be = s_d3e_backend;
        d3e_up_backend(&up_be);
    } else if (ms_wrap) {
        up_be = s_d3e_backend;
        d3e_ms_backend(&up_be);
    }
    rsx_draw_engine_set_backend((s_up.on || ms_wrap) ? &up_be : &s_d3e_backend);
    rsx_draw_engine_set_default(1);
    if (!rsx_draw_engine_enabled() || rsx_draw_engine_init(width, height) != 0) {
        D3E_LOG("the register-file draw engine is off (PS3RECOMP_RSX_ENGINE=vtable?) or failed to start\n");
        rsx_draw_engine_set_backend(NULL);
        d3e_device_destroy();
        return -1;
    }
    s_eng_active = 1;
    D3E_LOG("Direct3D 12 (experimental) up: %ux%u (%s)\n", width, height, title ? title : "");
    return 0;
}

void rsx_d3d12_engine_present(void)
{
    if (!s_d.up || !s_eng_active) return;
    rsx_draw_engine_present();
}

void rsx_d3d12_engine_shutdown(void)
{
    if (s_eng_active) { rsx_draw_engine_shutdown(); s_eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_d.dev) d3e_device_destroy();
}

uint32_t rsx_d3d12_engine_guest_draws(void)
{
    return s_eng_active ? rsx_draw_engine_guest_draws() : s_last_draws;
}

uint32_t rsx_d3d12_engine_presented_center(void)
{
    return s_d.presents ? (0xFF000000u | s_d.last_center) : 0u;
}
