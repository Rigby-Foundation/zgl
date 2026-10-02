/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* A draw on the R300 without a vertex engine (Mesa's "SW TCL"): fetch the
 * vertices, run the vertex shader on the CPU, assemble primitives, cull
 * and clip them, project to the window, and hand the GPU finished
 * vertices through DRAW_IMMD packets: position, then each fragment shader
 * input as a 4-component texture coordinate. The hardware state (the
 * fragment program, textures, depth, blending) is written in full before
 * every draw. */
#include "vrend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WAIT_UNTIL          0x1720
#define  WAIT_3D_IDLECLEAN  (1u << 17)
#define R300_US_OUT_FMT_1   0x46A8
#define R300_US_OUT_FMT_2   0x46AC
#define R300_US_OUT_FMT_3   0x46B0

#define MAX_VARY 8                  /* fragment inputs: texture coordinate slots */

struct vtx { float clip[4]; float var[MAX_VARY][4]; };

static uint32_t fu(float f) { union { float f; uint32_t u; } c = { f }; return c.u; }

/* ---- vertex fetch ---------------------------------------------------------------- */

static void fetch(uint32_t fmt, const uint8_t *p, float out[4])
{
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    int n, kind;                    /* kind: 0 float, 1 unorm, 2 snorm, 3 uscaled, 4 sscaled */
    int bits;
    switch (fmt) {
    case VIRGL_FORMAT_R32_FLOAT: n = 1; kind = 0; bits = 32; break;
    case VIRGL_FORMAT_R32G32_FLOAT: n = 2; kind = 0; bits = 32; break;
    case VIRGL_FORMAT_R32G32B32_FLOAT: n = 3; kind = 0; bits = 32; break;
    case VIRGL_FORMAT_R32G32B32A32_FLOAT: n = 4; kind = 0; bits = 32; break;
    case VIRGL_FORMAT_R8_UNORM: n = 1; kind = 1; bits = 8; break;
    case VIRGL_FORMAT_R8G8_UNORM: n = 2; kind = 1; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8_UNORM: n = 3; kind = 1; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8A8_UNORM: n = 4; kind = 1; bits = 8; break;
    case VIRGL_FORMAT_R8_SNORM: n = 1; kind = 2; bits = 8; break;
    case VIRGL_FORMAT_R8G8_SNORM: n = 2; kind = 2; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8_SNORM: n = 3; kind = 2; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8A8_SNORM: n = 4; kind = 2; bits = 8; break;
    case VIRGL_FORMAT_R8_USCALED: n = 1; kind = 3; bits = 8; break;
    case VIRGL_FORMAT_R8G8_USCALED: n = 2; kind = 3; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8_USCALED: n = 3; kind = 3; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8A8_USCALED: n = 4; kind = 3; bits = 8; break;
    case VIRGL_FORMAT_R8_SSCALED: n = 1; kind = 4; bits = 8; break;
    case VIRGL_FORMAT_R8G8_SSCALED: n = 2; kind = 4; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8_SSCALED: n = 3; kind = 4; bits = 8; break;
    case VIRGL_FORMAT_R8G8B8A8_SSCALED: n = 4; kind = 4; bits = 8; break;
    case VIRGL_FORMAT_R16_UNORM: n = 1; kind = 1; bits = 16; break;
    case VIRGL_FORMAT_R16G16_UNORM: n = 2; kind = 1; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16_UNORM: n = 3; kind = 1; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16A16_UNORM: n = 4; kind = 1; bits = 16; break;
    case VIRGL_FORMAT_R16_SNORM: n = 1; kind = 2; bits = 16; break;
    case VIRGL_FORMAT_R16G16_SNORM: n = 2; kind = 2; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16_SNORM: n = 3; kind = 2; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16A16_SNORM: n = 4; kind = 2; bits = 16; break;
    case VIRGL_FORMAT_R16_USCALED: n = 1; kind = 3; bits = 16; break;
    case VIRGL_FORMAT_R16G16_USCALED: n = 2; kind = 3; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16_USCALED: n = 3; kind = 3; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16A16_USCALED: n = 4; kind = 3; bits = 16; break;
    case VIRGL_FORMAT_R16_SSCALED: n = 1; kind = 4; bits = 16; break;
    case VIRGL_FORMAT_R16G16_SSCALED: n = 2; kind = 4; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16_SSCALED: n = 3; kind = 4; bits = 16; break;
    case VIRGL_FORMAT_R16G16B16A16_SSCALED: n = 4; kind = 4; bits = 16; break;
    default: return;
    }
    for (int i = 0; i < n; i++) {
        float f;
        if (kind == 0) memcpy(&f, p + 4 * i, 4);
        else if (bits == 8) {
            if (kind == 1) f = p[i] / 255.0f;
            else if (kind == 2) { f = (int8_t)p[i] / 127.0f; if (f < -1) f = -1; }
            else if (kind == 3) f = p[i];
            else f = (int8_t)p[i];
        } else {
            uint16_t u; memcpy(&u, p + 2 * i, 2);
            if (kind == 1) f = u / 65535.0f;
            else if (kind == 2) { f = (int16_t)u / 32767.0f; if (f < -1) f = -1; }
            else if (kind == 3) f = u;
            else f = (int16_t)u;
        }
        out[i] = f;
    }
}

/* ---- the link between the stages ------------------------------------------------ */

struct link {
    int nvary;                      /* fragment inputs */
    int src[MAX_VARY];              /* the VS output feeding each, -1: a constant (FACE = 1), -2: the window position */
    int pos;                        /* the VS position output */
};

static void make_link(const struct vr_shader *vs, const struct vr_shader *fs, struct link *l)
{
    l->pos = 0;
    for (int o = 0; o < vs->t.nout; o++) if (vs->t.out[o].sem == SEM_POSITION) l->pos = o;
    l->nvary = fs->fs.ninputs < MAX_VARY ? fs->fs.ninputs : MAX_VARY;
    for (int i = 0; i < l->nvary; i++) {
        const struct tgsi_io *in = &fs->fs.input[i];
        l->src[i] = -1;
        if (in->sem == SEM_POSITION) { l->src[i] = -2; continue; }
        for (int o = 0; o < vs->t.nout; o++) {
            const struct tgsi_io *out = &vs->t.out[o];
            if (out->sem == in->sem && out->sindex == in->sindex) { l->src[i] = o; break; }
        }
    }
}

/* ---- the hardware state ------------------------------------------------------------ */

static uint32_t blend_factor(uint32_t f)
{
    switch (f) {
    case PIPE_BLENDFACTOR_ONE: return R300_BLEND_GL_ONE;
    case PIPE_BLENDFACTOR_SRC_COLOR: return R300_BLEND_GL_SRC_COLOR;
    case PIPE_BLENDFACTOR_SRC_ALPHA: return R300_BLEND_GL_SRC_ALPHA;
    case PIPE_BLENDFACTOR_DST_ALPHA: return R300_BLEND_GL_DST_ALPHA;
    case PIPE_BLENDFACTOR_DST_COLOR: return R300_BLEND_GL_DST_COLOR;
    case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE: return R300_BLEND_GL_SRC_ALPHA_SATURATE;
    case PIPE_BLENDFACTOR_CONST_COLOR: return R300_BLEND_GL_CONST_COLOR;
    case PIPE_BLENDFACTOR_CONST_ALPHA: return R300_BLEND_GL_CONST_ALPHA;
    case PIPE_BLENDFACTOR_INV_SRC_COLOR: return R300_BLEND_GL_ONE_MINUS_SRC_COLOR;
    case PIPE_BLENDFACTOR_INV_SRC_ALPHA: return R300_BLEND_GL_ONE_MINUS_SRC_ALPHA;
    case PIPE_BLENDFACTOR_INV_DST_ALPHA: return R300_BLEND_GL_ONE_MINUS_DST_ALPHA;
    case PIPE_BLENDFACTOR_INV_DST_COLOR: return R300_BLEND_GL_ONE_MINUS_DST_COLOR;
    case PIPE_BLENDFACTOR_INV_CONST_COLOR: return R300_BLEND_GL_ONE_MINUS_CONST_COLOR;
    case PIPE_BLENDFACTOR_INV_CONST_ALPHA: return R300_BLEND_GL_ONE_MINUS_CONST_ALPHA;
    default: return R300_BLEND_GL_ZERO;
    }
}
static uint32_t blend_fcn(uint32_t f)
{
    switch (f) {
    case PIPE_BLEND_SUBTRACT: return R300_COMB_FCN_SUB_CLAMP;
    case PIPE_BLEND_REVERSE_SUBTRACT: return R300_COMB_FCN_RSUB_CLAMP;
    case PIPE_BLEND_MIN: return R300_COMB_FCN_MIN;
    case PIPE_BLEND_MAX: return R300_COMB_FCN_MAX;
    default: return R300_COMB_FCN_ADD_CLAMP;
    }
}
static uint32_t zfunc(uint32_t f)
{
    static const uint32_t m[8] = { R300_ZS_NEVER, R300_ZS_LESS, R300_ZS_EQUAL, R300_ZS_LEQUAL,
                                   R300_ZS_GREATER, R300_ZS_NOTEQUAL, R300_ZS_GEQUAL, R300_ZS_ALWAYS };
    return m[f & 7];
}
static uint32_t wrap(uint32_t w)
{
    switch (w) {
    case PIPE_TEX_WRAP_CLAMP: return R300_TX_CLAMP;
    case PIPE_TEX_WRAP_CLAMP_TO_EDGE: return R300_TX_CLAMP_TO_EDGE;
    case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return R300_TX_CLAMP_TO_BORDER;
    case PIPE_TEX_WRAP_MIRROR_REPEAT: return R300_TX_MIRRORED;
    default: return R300_TX_REPEAT;
    }
}

/* Everything the 3D pipe reads, for a draw with `fs`, `nvary` inputs.
 * color_mask/depth_write >= 0 override the bound state (clears, blits). */
#define STATE_DWORDS 1600                  /* what emit_state writes, at most */

static int emit_state(struct r300_vrend *v, struct vr_shader *fs, const float (*fs_consts)[4], int nfs_consts,
                      int nvary, int prim_is_point, int color_mask_override, int depth_override)
{
    struct r300_hw *hw = &v->hw;
    /* a draw's state and its vertices go in one submission: other programs'
     * work may run between two, and it sets its own state */
    if (r300_space(hw) < STATE_DWORDS + 1024) vr_commit(v);
    uint32_t start = hw->used;
    struct vr_res *cb = v->have_cbuf ? vr_res_get(v, v->cbuf.res) : NULL;
    struct vr_res *zb = v->have_zbuf ? vr_res_get(v, v->zbuf.res) : NULL;
    if (!cb || !cb->bo.handle) return -1;
    uint32_t cl = v->cbuf.level <= cb->last_level ? v->cbuf.level : 0;
    uint32_t cw = cb->w >> cl ? cb->w >> cl : 1, ch = cb->h >> cl ? cb->h >> cl : 1;

    r300_reg(hw, R300_RB3D_DSTCACHE_CTLSTAT, R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D | R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
    r300_reg(hw, R300_ZB_ZCACHE_CTLSTAT, R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE | R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
    r300_reg(hw, WAIT_UNTIL, WAIT_3D_IDLECLEAN);

    /* invariant (r300_init_states) */
    r300_reg(hw, R300_GB_SELECT, 0);
    r300_reg(hw, R300_GB_ENABLE, 0);
    r300_reg(hw, R300_GB_AA_CONFIG, 0);
    r300_reg(hw, R300_FG_FOG_BLEND, 0);
    r300_reg(hw, R300_GA_OFFSET, 0);
    r300_reg(hw, R300_SU_TEX_WRAP, 0);
    r300_reg(hw, R300_SU_DEPTH_SCALE, 0x4B7FFFFF);
    r300_reg(hw, R300_SU_DEPTH_OFFSET, 0);
    r300_reg(hw, R300_SC_EDGERULE, 0x2DA49525);
    r300_reg(hw, R500_RB3D_DISCARD_SRC_PIXEL_LTE_THRESHOLD, 0x01010101);
    r300_reg(hw, R500_RB3D_DISCARD_SRC_PIXEL_GTE_THRESHOLD, 0xFEFEFEFE);
    r300_reg(hw, R300_SC_HYPERZ, R300_SC_HYPERZ_ADJ_2);
    r300_reg(hw, R300_GB_Z_PEQ_CONFIG, 0);
    r300_reg(hw, R300_SC_SCREENDOOR, 0xFFFFFF);
    /* one sample per pixel, at its centre (6/12, 6/12) as GL wants; the reset value is the corner */
    r300_reg(hw, R300_GB_MSPOS0, 0x66666666);
    r300_reg(hw, R300_GB_MSPOS1, 0x06666666);

    /* the VAP in TCL bypass: position, then nvary texture coordinates */
    r300_reg(hw, R300_VAP_PVS_STATE_FLUSH_REG, 0);
    r300_reg(hw, VAP_PVS_VTX_TIMEOUT_REG, 0xFFFF);
    uint32_t adj[4] = { fu(1), fu(1), fu(1), fu(1) };
    r300_regs(hw, R300_VAP_GB_VERT_CLIP_ADJ, adj, 4);
    r300_reg(hw, R300_VAP_PSC_SGN_NORM_CNTL, R300_SGN_NORM_NO_ZERO);
    r300_reg(hw, R300_VAP_CNTL, R300_PVS_NUM_SLOTS(10) | R300_PVS_NUM_CNTLRS(5) | R300_PVS_NUM_FPUS(2) | R300_PVS_VF_MAX_VTX_NUM(5));
    r300_reg(hw, R300_VAP_CNTL_STATUS, R300_VC_NO_SWAP | R300_VAP_TCL_BYPASS);
    r300_reg(hw, R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
    r300_reg(hw, R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
    uint32_t psc[5] = { 0 }, ext[5] = { 0 };
    uint32_t swz = 0 << 0 | 1 << 3 | 2 << 6 | 3 << 9 | 0xF << R300_WRITE_ENA_SHIFT;
    int nattr = 1 + nvary;
    for (int a = 0; a < nattr; a++) {
        uint32_t t = R300_DATA_TYPE_FLOAT_4 | (uint32_t)(a == 0 ? 0 : 6 + a - 1) << R300_DST_VEC_LOC_SHIFT;
        if (a == nattr - 1) t |= R300_LAST_VEC;
        psc[a / 2] |= t << (a & 1 ? 16 : 0);
        ext[a / 2] |= swz << (a & 1 ? 16 : 0);
    }
    r300_regs(hw, R300_VAP_PROG_STREAM_CNTL_0, psc, (unsigned)(nattr + 1) / 2);
    r300_regs(hw, R300_VAP_PROG_STREAM_CNTL_EXT_0, ext, (unsigned)(nattr + 1) / 2);
    uint32_t assm = R300_INPUT_CNTL_POS, fmt1 = 0;
    for (int i = 0; i < nvary; i++) { assm |= R300_INPUT_CNTL_TC0 << i; fmt1 |= 4u << (3 * i); }
    r300_reg(hw, R300_VAP_VTX_STATE_CNTL, 0x5555);
    r300_reg(hw, R300_VAP_VSM_VTX_ASSM, assm);
    r300_reg(hw, R300_VAP_OUTPUT_VTX_FMT_0, R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
    r300_reg(hw, R300_VAP_OUTPUT_VTX_FMT_1, fmt1);
    r300_reg(hw, R300_VAP_PVS_STATE_FLUSH_REG, 0);

    /* setup: culling happened on the CPU */
    const uint32_t *rs = v->have_rs ? v->rs : NULL;
    union { uint32_t u; float f; } psize = { rs ? rs[1] : fu(1) }, lwidth = { rs ? rs[4] : fu(1) }, pou = { rs ? rs[5] : 0 }, pof = { rs ? rs[6] : 0 };
    uint32_t ps6 = (uint32_t)(psize.f * 6.0f) & 0xFFFF, lw6 = (uint32_t)(lwidth.f * 6.0f) & 0xFFFF;
    r300_reg(hw, R300_GA_POINT_SIZE, ps6 | ps6 << 16);
    r300_reg(hw, R300_GA_POINT_MINMAX, 0 | 0xFFFFu << 16);
    r300_reg(hw, R300_GA_LINE_CNTL, lw6 | R300_GA_LINE_CNTL_END_TYPE_COMP);
    r300_reg(hw, R300_GA_POLY_MODE, R300_GA_POLY_MODE_DISABLE);
    r300_reg(hw, R300_GA_ROUND_MODE, R300_GA_ROUND_MODE_GEOMETRY_ROUND_NEAREST | R300_GA_ROUND_MODE_COLOR_ROUND_NEAREST);
    r300_reg(hw, R300_GA_COLOR_CONTROL, R300_GA_COLOR_CONTROL_RGB0_SHADING_GOURAUD | R300_GA_COLOR_CONTROL_ALPHA0_SHADING_GOURAUD |
             R300_GA_COLOR_CONTROL_RGB1_SHADING_GOURAUD | R300_GA_COLOR_CONTROL_ALPHA1_SHADING_GOURAUD |
             R300_GA_COLOR_CONTROL_RGB2_SHADING_GOURAUD | R300_GA_COLOR_CONTROL_ALPHA2_SHADING_GOURAUD |
             R300_GA_COLOR_CONTROL_RGB3_SHADING_GOURAUD | R300_GA_COLOR_CONTROL_ALPHA3_SHADING_GOURAUD |
             R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_LAST);
    r300_reg(hw, R300_SU_CULL_MODE, 0);
    int poly_offset = rs && (rs[0] & VIRGL_OBJ_RS_S0_OFFSET_TRI(1)) && !prim_is_point;
    if (poly_offset) {
        uint32_t po[4] = { fu(pof.f * 12.0f), fu(pou.f * 2.0f), fu(pof.f * 12.0f), fu(pou.f * 2.0f) };
        r300_regs(hw, R300_SU_POLY_OFFSET_FRONT_SCALE, po, 4);
    }
    r300_reg(hw, R300_SU_POLY_OFFSET_ENABLE, poly_offset ? 3 : 0);

    /* scissor (from the rasterizer state; clears and blits keep what is set) and clip rectangle */
    uint32_t x0 = 0, y0 = 0, x1 = cw, y1 = ch;
    if (rs && (rs[0] & VIRGL_OBJ_RS_S0_SCISSOR(1))) {
        x0 = v->sc_minx; y0 = v->sc_miny; x1 = v->sc_maxx < cw ? v->sc_maxx : cw; y1 = v->sc_maxy < ch ? v->sc_maxy : ch;
    }
    if (x1 <= x0 || y1 <= y0) return -1;            /* nothing to draw */
    r300_reg(hw, R300_SC_SCISSORS_TL, (x0 + 1440) << R300_SCISSORS_X_SHIFT | (y0 + 1440) << R300_SCISSORS_Y_SHIFT);
    r300_reg(hw, R300_SC_SCISSORS_BR, (x1 + 1440 - 1) << R300_SCISSORS_X_SHIFT | (y1 + 1440 - 1) << R300_SCISSORS_Y_SHIFT);
    r300_reg(hw, R300_SC_CLIPRECT_TL_0, 1440 << R300_CLIPRECT_X_SHIFT | 1440 << R300_CLIPRECT_Y_SHIFT);
    r300_reg(hw, R300_SC_CLIPRECT_BR_0, (cw + 1440 - 1) << R300_CLIPRECT_X_SHIFT | (ch + 1440 - 1) << R300_CLIPRECT_Y_SHIFT);
    r300_reg(hw, R300_SC_CLIP_RULE, 0xFFFF);

    /* the rasterizer: input i as texture coordinate i into the temporary the program expects */
    const struct r300_fs *p = &fs->fs;
    if (nvary) {
        uint32_t ip[MAX_VARY], inst[MAX_VARY];
        for (int i = 0; i < nvary; i++) {
            ip[i] = R300_RS_TEX_PTR(4 * i) | R300_RS_SEL_S(R300_RS_SEL_C0) | R300_RS_SEL_T(R300_RS_SEL_C1) |
                    R300_RS_SEL_R(R300_RS_SEL_C2) | R300_RS_SEL_Q(R300_RS_SEL_C3);
            inst[i] = R300_RS_INST_TEX_ID(i);
            if (i < p->ninputs && p->input_temp[i] >= 0) inst[i] |= R300_RS_INST_TEX_CN_WRITE | R300_RS_INST_TEX_ADDR(p->input_temp[i]);
        }
        r300_reg(hw, R300_RS_COUNT, (uint32_t)(4 * nvary) << R300_IT_COUNT_SHIFT | R300_HIRES_EN);
        r300_reg(hw, R300_RS_INST_COUNT, (uint32_t)(nvary - 1));
        r300_regs(hw, R300_RS_IP_0, ip, (unsigned)nvary);
        r300_regs(hw, R300_RS_INST_0, inst, (unsigned)nvary);
    } else {                                        /* rasterize something, or it locks up */
        r300_reg(hw, R300_RS_COUNT, 1u << R300_IC_COUNT_SHIFT | R300_HIRES_EN);
        r300_reg(hw, R300_RS_INST_COUNT, 0);
        r300_reg(hw, R300_RS_IP_0, R300_RS_COL_PTR(0) | R300_RS_COL_FMT(R300_RS_COL_FMT_0001));
        r300_reg(hw, R300_RS_INST_0, R300_RS_INST_COL_ID(0));
    }

    /* the fragment program */
    r300_reg(hw, R300_US_CONFIG, p->config);
    r300_reg(hw, R300_US_PIXSIZE, p->pixsize);
    r300_reg(hw, R300_US_CODE_OFFSET, p->code_offset);
    r300_reg(hw, R400_US_CODE_EXT, 0);
    r300_regs(hw, R300_US_CODE_ADDR_0, p->code_addr, 4);
    r300_reg(hw, R400_US_CODE_BANK, 0);
    uint32_t col[R300_FS_MAX_ALU];
    for (int k = 0; k < 4; k++) {
        static const uint32_t base[4] = { R300_US_ALU_RGB_INST_0, R300_US_ALU_RGB_ADDR_0, R300_US_ALU_ALPHA_INST_0, R300_US_ALU_ALPHA_ADDR_0 };
        static const int field[4] = { 0, 1, 2, 3 };
        for (int i = 0; i < p->nalu; i++) col[i] = p->alu[i][field[k]];
        r300_regs(hw, base[k], col, (unsigned)p->nalu);
    }
    if (p->ntex) r300_regs(hw, R300_US_TEX_INST_0, p->tex, (unsigned)p->ntex);
    if (p->nconst) {
        uint32_t c[R300_FS_MAX_CONST * 4];
        for (int i = 0; i < p->nconst; i++)
            for (int j = 0; j < 4; j++) {
                float f = p->const_user[i] < 0 ? p->const_lit[i][j] : p->const_user[i] < nfs_consts ? fs_consts[p->const_user[i]][j] : 0.0f;
                c[i * 4 + j] = r300_float24(f);
            }
        r300_regs(hw, R300_PFS_PARAM_0_X, c, (unsigned)p->nconst * 4);
    }
    r300_reg(hw, R300_US_OUT_FMT_0, R300_US_OUT_FMT_C4_8 | R300_C0_SEL_B | R300_C1_SEL_G | R300_C2_SEL_R | R300_C3_SEL_A);
    r300_reg(hw, R300_US_OUT_FMT_1, R300_US_OUT_FMT_UNUSED);
    r300_reg(hw, R300_US_OUT_FMT_2, R300_US_OUT_FMT_UNUSED);
    r300_reg(hw, R300_US_OUT_FMT_3, R300_US_OUT_FMT_UNUSED);
    r300_reg(hw, R300_US_W_FMT, 0);

    /* textures */
    uint32_t tx_enable = 0;
    for (int i = 0; i < v->nviews && i < v->nsamplers && i < VR_MAX_UNITS; i++) {
        struct vr_res *t = vr_res_get(v, v->views[i].res);
        if (!t || !t->bo.handle) continue;
        uint32_t base = v->views[i].first_level <= t->last_level ? v->views[i].first_level : 0;
        uint32_t s0 = v->samplers[i].s0;
        uint32_t minf = (s0 >> 9) & 1, mip = (s0 >> 11) & 3, magf = (s0 >> 13) & 1;
        uint32_t levels = t->last_level - base;
        uint32_t f0 = R300_TX_WRAP_S(wrap(s0 & 7)) | R300_TX_WRAP_T(wrap((s0 >> 3) & 7)) |
                      (magf ? R300_TX_MAG_FILTER_LINEAR : R300_TX_MAG_FILTER_NEAREST) |
                      (minf ? R300_TX_MIN_FILTER_LINEAR : R300_TX_MIN_FILTER_NEAREST);
        if (t->npot) {                              /* no mips, no repeat on non-power-of-two */
            if ((f0 & R300_TX_WRAP_S_MASK) == R300_TX_WRAP_S(R300_TX_REPEAT) || (f0 & R300_TX_WRAP_S_MASK) == R300_TX_WRAP_S(R300_TX_MIRRORED))
                f0 = (f0 & ~R300_TX_WRAP_S_MASK) | R300_TX_WRAP_S(R300_TX_CLAMP_TO_EDGE);
            if ((f0 & R300_TX_WRAP_T_MASK) == R300_TX_WRAP_T(R300_TX_REPEAT) || (f0 & R300_TX_WRAP_T_MASK) == R300_TX_WRAP_T(R300_TX_MIRRORED))
                f0 = (f0 & ~R300_TX_WRAP_T_MASK) | R300_TX_WRAP_T(R300_TX_CLAMP_TO_EDGE);
            levels = 0;
        } else if (levels && mip != PIPE_TEX_MIPFILTER_NONE) {
            f0 |= mip == PIPE_TEX_MIPFILTER_LINEAR ? R300_TX_MIN_FILTER_MIP_LINEAR : R300_TX_MIN_FILTER_MIP_NEAREST;
        } else {
            levels = 0;
        }
        uint32_t w = t->w >> base ? t->w >> base : 1, h = t->h >> base ? t->h >> base : 1;
        uint32_t f0fmt = R300_TX_WIDTH((w - 1) & 0x7FF) | R300_TX_HEIGHT((h - 1) & 0x7FF) | R300_TX_NUM_LEVELS(levels);
        uint32_t f2 = 0;
        if (t->npot) { f0fmt |= R300_TX_PITCH_EN; f2 = (t->stride[base] / 4 - 1) & 0x1FFF; }
        /* B8G8R8A8 in memory is X=B Y=G Z=R W=A */
        uint32_t f1 = R300_TX_FORMAT_W8Z8Y8X8 | R300_TX_FORMAT_Z << R300_TX_FORMAT_R_SHIFT | R300_TX_FORMAT_Y << R300_TX_FORMAT_G_SHIFT |
                      R300_TX_FORMAT_X << R300_TX_FORMAT_B_SHIFT |
                      (t->format == VIRGL_FORMAT_B8G8R8X8_UNORM ? R300_TX_FORMAT_ONE : R300_TX_FORMAT_W) << R300_TX_FORMAT_A_SHIFT;
        f1 |= R300_TX_CACHE(R300_TX_CACHE_WHOLE);
        r300_reg(hw, R300_TX_FILTER0_0 + 4 * (uint32_t)i, f0);
        r300_reg(hw, R300_TX_FILTER1_0 + 4 * (uint32_t)i, 0);
        r300_reg(hw, R300_TX_BORDER_COLOR_0 + 4 * (uint32_t)i, 0);
        r300_reg(hw, R300_TX_FORMAT0_0 + 4 * (uint32_t)i, f0fmt);
        r300_reg(hw, R300_TX_FORMAT1_0 + 4 * (uint32_t)i, f1);
        r300_reg(hw, R300_TX_FORMAT2_0 + 4 * (uint32_t)i, f2);
        r300_reg(hw, R300_TX_OFFSET_0 + 4 * (uint32_t)i, (hw->vram_mc + t->bo.offset + t->offset[base]) & ~0x1Fu);
        tx_enable |= 1u << i;
    }
    r300_reg(hw, R300_TX_INVALTAGS, 0);
    r300_reg(hw, R300_TX_ENABLE, tx_enable);

    /* after the fragment program: alpha test, depth, blending */
    const uint32_t *dsa = v->have_dsa ? v->dsa : NULL;
    uint32_t alpha = 0;
    if (dsa && (dsa[0] & VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(1))) {
        union { uint32_t u; float f; } ref = { dsa[3] };
        float r = ref.f < 0 ? 0 : ref.f > 1 ? 1 : ref.f;
        alpha = (uint32_t)(r * 255.0f + 0.5f) | ((dsa[0] >> 9) & 7) << 8 | R300_FG_ALPHA_FUNC_ENABLE;
    }
    r300_reg(hw, R300_FG_ALPHA_FUNC, alpha);
    r300_reg(hw, R300_FG_DEPTH_SRC, 0);
    uint32_t zcntl = 0, zfn = R300_ZS_ALWAYS;
    if (zb && zb->bo.handle) {
        if (depth_override >= 0) { if (depth_override) zcntl = R300_Z_ENABLE | R300_Z_WRITE_ENABLE; }
        else if (dsa && (dsa[0] & VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(1))) {
            zcntl = R300_Z_ENABLE | ((dsa[0] & VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(1)) ? R300_Z_WRITE_ENABLE : 0);
            zfn = zfunc((dsa[0] >> 2) & 7);
        }
        r300_reg(hw, R300_ZB_FORMAT, R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL);
        r300_reg(hw, R300_ZB_DEPTHOFFSET, hw->vram_mc + zb->bo.offset);
        r300_reg(hw, R300_ZB_DEPTHPITCH, (zb->stride[0] / 4) & R300_DEPTHPITCH_MASK);
    }
    r300_reg(hw, R300_ZB_CNTL, zcntl);
    r300_reg(hw, R300_ZB_ZSTENCILCNTL, zfn << R300_Z_FUNC_SHIFT);
    r300_reg(hw, R300_ZB_ZTOP, 0);
    r300_reg(hw, R300_ZB_BW_CNTL, 0);

    const uint32_t *bl = v->have_blend ? v->blend : NULL;
    uint32_t cblend = 0, ablend = 0, mask = 0xF;
    if (bl) {
        uint32_t rt = bl[2];
        uint32_t cm = (rt >> 27) & 0xF;             /* R G B A */
        mask = (cm & 4 ? 1 : 0) | (cm & 2 ? 2 : 0) | (cm & 1 ? 4 : 0) | (cm & 8 ? 8 : 0);   /* B G R A */
        if (rt & 1) {
            cblend = R300_ALPHA_BLEND_ENABLE | R300_SEPARATE_ALPHA_ENABLE | R300_READ_ENABLE | blend_fcn((rt >> 1) & 7) |
                     blend_factor((rt >> 4) & 0x1F) << R300_SRC_BLEND_SHIFT | blend_factor((rt >> 9) & 0x1F) << R300_DST_BLEND_SHIFT;
            ablend = blend_fcn((rt >> 14) & 7) | blend_factor((rt >> 17) & 0x1F) << R300_SRC_BLEND_SHIFT |
                     blend_factor((rt >> 22) & 0x1F) << R300_DST_BLEND_SHIFT;
        }
    }
    if (color_mask_override >= 0) { mask = color_mask_override ? 0xF : 0; cblend = ablend = 0; }
    r300_reg(hw, R300_RB3D_CBLEND, cblend);
    r300_reg(hw, R300_RB3D_ABLEND, ablend);
    uint32_t bc = 0;
    for (int i = 0; i < 4; i++) {
        float f = v->blend_color[i] < 0 ? 0 : v->blend_color[i] > 1 ? 1 : v->blend_color[i];
        bc |= (uint32_t)(f * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);
    }
    r300_reg(hw, R300_RB3D_BLEND_COLOR, bc);
    r300_reg(hw, RB3D_COLOR_CHANNEL_MASK, mask);
    r300_reg(hw, R300_RB3D_ROPCNTL, 0);
    r300_reg(hw, R300_RB3D_DITHER_CTL, 0);
    r300_reg(hw, R300_RB3D_AARESOLVE_CTL, 0);
    r300_reg(hw, R300_RB3D_CCTL, 0);
    r300_reg(hw, R300_RB3D_COLOROFFSET0, hw->vram_mc + cb->bo.offset + cb->offset[cl]);
    r300_reg(hw, R300_RB3D_COLORPITCH0, (cb->stride[cl] / 4) | R300_COLOR_FORMAT_ARGB8888);
    v->busy = 1;
    cb->last_use = v->serial;
    if (zb) zb->last_use = v->serial;
    for (int i = 0; i < v->nviews && i < VR_MAX_UNITS; i++) { struct vr_res *t = vr_res_get(v, v->views[i].res); if (t && (tx_enable & (1u << i))) t->last_use = v->serial; }
    /* the same state as the previous draw of this submission: drop the copy */
    uint32_t len = hw->used - start;
    if (v->state_serial == v->serial && (int)len == v->state_len && !memcmp(v->state, hw->cmd + start, len * 4)) hw->used = start;
    else if (len <= sizeof v->state / 4) { memcpy(v->state, hw->cmd + start, len * 4); v->state_len = (int)len; v->state_serial = v->serial; }
    return 0;
}

/* ---- primitives: clip, cull, emit -------------------------------------------------- */

struct emitter {
    struct r300_vrend *v;
    int is_point;                   /* for emit_state again after a full submission */
    int nvary, vsize;               /* dwords per vertex */
    uint32_t prim;                  /* R300_VAP_VF_CNTL__PRIM_* */
    int per_prim;                   /* vertices per primitive */
    float buf[16000];
    int nverts;
    const struct link *l;
    int cull;                       /* PIPE_FACE_* culled */
    int front_ccw;
};

static void flush_emit(struct emitter *e)
{
    if (!e->nverts) return;
    struct r300_hw *hw = &e->v->hw;
    if (r300_space(hw) < (uint32_t)(e->nverts * e->vsize) + 16) {   /* next submission: the state again first */
        vr_commit(e->v);
        emit_state(e->v, e->v->fs, (const float (*)[4])e->v->fs_const, e->v->nfs_const, e->nvary, e->is_point, -1, -1);
    }
    r300_reg(hw, R300_VAP_VTX_SIZE, (uint32_t)e->vsize);
    r300_reg(hw, R300_VAP_VF_MAX_VTX_INDX, (uint32_t)e->nverts - 1);
    r300_reg(hw, R300_VAP_VF_MIN_VTX_INDX, 0);
    r300_emit(hw, R300_PACKET3(R300_PACKET3_3D_DRAW_IMMD_2, (uint32_t)(e->nverts * e->vsize)));
    r300_emit(hw, R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (uint32_t)e->nverts << 16 | e->prim);
    const uint32_t *d = (const uint32_t *)e->buf;
    for (int i = 0; i < e->nverts * e->vsize; i++) r300_emit(hw, d[i]);
    e->nverts = 0;
}

/* A clipped vertex to the window. */
static void put_vertex(struct emitter *e, const struct vtx *x)
{
    struct r300_vrend *v = e->v;
    if ((e->nverts + 1) * e->vsize > (int)(sizeof e->buf / sizeof e->buf[0])) return;
    float *o = e->buf + e->nverts * e->vsize;
    float iw = x->clip[3] != 0 ? 1.0f / x->clip[3] : 1.0f;
    float wx = x->clip[0] * iw * v->vp_scale[0] + v->vp_trans[0];
    float wy = x->clip[1] * iw * v->vp_scale[1] + v->vp_trans[1];
    float wz = x->clip[2] * iw * v->vp_scale[2] + v->vp_trans[2];
    o[0] = wx; o[1] = wy; o[2] = wz; o[3] = iw;
    for (int i = 0; i < e->nvary; i++) {
        float *t = o + 4 + 4 * i;
        if (e->l->src[i] == -2) { t[0] = wx; t[1] = wy; t[2] = wz; t[3] = iw; }
        else memcpy(t, x->var[i], 16);
    }
    e->nverts++;
}

static void lerp(struct vtx *r, const struct vtx *a, const struct vtx *b, float t, int nvary)
{
    for (int k = 0; k < 4; k++) r->clip[k] = a->clip[k] + (b->clip[k] - a->clip[k]) * t;
    for (int i = 0; i < nvary; i++)
        for (int k = 0; k < 4; k++) r->var[i][k] = a->var[i][k] + (b->var[i][k] - a->var[i][k]) * t;
}

/* distance to clip plane p (0..5: -x +x -y +y -z +z) */
static float plane_dist(const struct vtx *x, int p)
{
    const float *c = x->clip;
    switch (p) {
    case 0: return c[3] + c[0];
    case 1: return c[3] - c[0];
    case 2: return c[3] + c[1];
    case 3: return c[3] - c[1];
    case 4: return c[3] + c[2];
    default: return c[3] - c[2];
    }
}

static unsigned outcode(const struct vtx *x)
{
    unsigned m = 0;
    for (int p = 0; p < 6; p++) if (plane_dist(x, p) < 0) m |= 1u << p;
    return m;
}

static void triangle(struct emitter *e, const struct vtx *a, const struct vtx *b, const struct vtx *c)
{
    unsigned oa = outcode(a), ob = outcode(b), oc = outcode(c);
    if (oa & ob & oc) return;                       /* all outside one plane */
    /* cull in window space (GL: y up), on the unclipped triangle when all w > 0 */
    if (e->cull && a->clip[3] > 0 && b->clip[3] > 0 && c->clip[3] > 0) {
        float ax = a->clip[0] / a->clip[3], ay = a->clip[1] / a->clip[3];
        float bx = b->clip[0] / b->clip[3], by = b->clip[1] / b->clip[3];
        float cx = c->clip[0] / c->clip[3], cy = c->clip[1] / c->clip[3];
        float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
        area *= e->v->vp_scale[0] * e->v->vp_scale[1];
        int ccw = area > 0, front = e->front_ccw ? ccw : !ccw;
        if ((e->cull & PIPE_FACE_FRONT) && front) return;
        if ((e->cull & PIPE_FACE_BACK) && !front) return;
        if (area == 0) return;
    }
    if (!(oa | ob | oc)) {
        if ((e->nverts + 3) * e->vsize > (int)(sizeof e->buf / sizeof e->buf[0])) flush_emit(e);
        put_vertex(e, a); put_vertex(e, b); put_vertex(e, c);
        return;
    }
    /* Sutherland-Hodgman against the planes the triangle crosses */
    static struct vtx pa[12], pb[12];
    struct vtx *in = pa, *out = pb;
    int n = 3;
    in[0] = *a; in[1] = *b; in[2] = *c;
    unsigned cross = oa | ob | oc;
    for (int p = 0; p < 6 && n >= 3; p++) {
        if (!(cross & (1u << p))) continue;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const struct vtx *s = &in[i], *t = &in[(i + 1) % n];
            float ds = plane_dist(s, p), dt = plane_dist(t, p);
            if (ds >= 0) out[m++] = *s;
            if ((ds >= 0) != (dt >= 0) && m < 12) lerp(&out[m++], s, t, ds / (ds - dt), e->nvary);
        }
        struct vtx *tmp = in; in = out; out = tmp;
        n = m;
    }
    for (int i = 1; i + 1 < n; i++) {
        if ((e->nverts + 3) * e->vsize > (int)(sizeof e->buf / sizeof e->buf[0])) flush_emit(e);
        put_vertex(e, &in[0]); put_vertex(e, &in[i]); put_vertex(e, &in[i + 1]);
    }
}

static void line(struct emitter *e, const struct vtx *a, const struct vtx *b)
{
    struct vtx s = *a, t = *b;
    for (int p = 0; p < 6; p++) {
        float ds = plane_dist(&s, p), dt = plane_dist(&t, p);
        if (ds < 0 && dt < 0) return;
        if (ds < 0) { struct vtx r; lerp(&r, &s, &t, ds / (ds - dt), e->nvary); s = r; }
        else if (dt < 0) { struct vtx r; lerp(&r, &t, &s, dt / (dt - ds), e->nvary); t = r; }
    }
    if ((e->nverts + 2) * e->vsize > (int)(sizeof e->buf / sizeof e->buf[0])) flush_emit(e);
    put_vertex(e, &s); put_vertex(e, &t);
}

static void point(struct emitter *e, const struct vtx *a)
{
    if (outcode(a)) return;
    if ((e->nverts + 1) * e->vsize > (int)(sizeof e->buf / sizeof e->buf[0])) flush_emit(e);
    put_vertex(e, a);
}

/* ---- the draw ---------------------------------------------------------------------- */

static struct vtx *verts; static int verts_cap;
static float (*vs_temp)[4]; static int vs_temp_cap;

static void run_vs(struct r300_vrend *v, const struct link *l, uint32_t index, struct vtx *out)
{
    const struct vr_shader *vs = v->vs;
    float in[16][4], o[TGSI_MAX_IO][4];
    memset(o, 0, sizeof o);
    for (int a = 0; a < v->ve.n && a < 16; a++) {
        uint32_t b = v->ve.vb[a];
        in[a][0] = in[a][1] = in[a][2] = 0; in[a][3] = 1;
        if ((int)b >= v->nvb) continue;
        struct vr_res *r = vr_res_get(v, v->vb[b].res);
        if (!r || !r->host) continue;
        uint64_t off = (uint64_t)v->vb[b].offset + (uint64_t)v->vb[b].stride * index + v->ve.offset[a];
        if (off + 16 > r->size && off + 4 > r->size) continue;
        fetch(v->ve.format[a], r->host + off, in[a]);
    }
    r300_vs_run(&vs->t, (const float (*)[4])v->vs_const, (const float (*)[4])in, o, vs_temp);
    memcpy(out->clip, o[l->pos], 16);
    for (int i = 0; i < l->nvary; i++) {
        if (l->src[i] >= 0) memcpy(out->var[i], o[l->src[i]], 16);
        else { out->var[i][0] = out->var[i][1] = out->var[i][2] = 0; out->var[i][3] = 1; if (l->src[i] == -1) out->var[i][0] = 1; }
    }
}

static uint32_t index_at(struct r300_vrend *v, uint32_t i, int indexed, uint32_t start)
{
    if (!indexed) return start + i;
    struct vr_res *r = vr_res_get(v, v->ib.res);
    if (!r || !r->host) return 0;
    uint64_t off = v->ib.offset + (uint64_t)(start + i) * v->ib.size;
    if (off + v->ib.size > r->size) return 0;
    if (v->ib.size == 1) return r->host[off];
    if (v->ib.size == 2) { uint16_t x; memcpy(&x, r->host + off, 2); return x; }
    uint32_t x; memcpy(&x, r->host + off, 4); return x;
}

void vr_draw_vbo(struct r300_vrend *v, const uint32_t *d)
{
    uint32_t start = d[0], count = d[1], mode = d[2], indexed = d[3], min_index = d[9], max_index = d[10];
    if (!v->vs || !v->fs || !count) return;
    struct link l;
    make_link(v->vs, v->fs, &l);
    int is_point = mode == PIPE_PRIM_POINTS, is_line = mode == PIPE_PRIM_LINES || mode == PIPE_PRIM_LINE_STRIP || mode == PIPE_PRIM_LINE_LOOP;
    if (emit_state(v, v->fs, (const float (*)[4])v->fs_const, v->nfs_const, l.nvary, is_point, -1, -1)) return;

    /* every vertex of the range through the vertex shader, once */
    if (!indexed) { min_index = start; max_index = start + count - 1; }
    if (max_index < min_index || max_index - min_index > 1u << 20) return;
    int nv = (int)(max_index - min_index + 1);
    if (nv > verts_cap) { free(verts); verts = malloc(sizeof *verts * (size_t)nv); verts_cap = verts ? nv : 0; if (!verts) return; }
    if (v->vs->t.ntemps > vs_temp_cap) { free(vs_temp); vs_temp = calloc((size_t)v->vs->t.ntemps + 1, 16); vs_temp_cap = v->vs->t.ntemps; }
    if (!vs_temp) { vs_temp = calloc(1, 16); vs_temp_cap = 0; }
    for (int i = 0; i < nv; i++) run_vs(v, &l, min_index + (uint32_t)i, &verts[i]);

    static struct emitter e;
    e.v = v; e.l = &l; e.nvary = l.nvary; e.is_point = is_point; e.vsize = 4 + 4 * l.nvary; e.nverts = 0;
    e.cull = v->have_rs ? (int)((v->rs[0] >> 8) & 3) : 0;
    e.front_ccw = v->have_rs ? (int)((v->rs[0] >> 15) & 1) : 1;
    e.prim = is_point ? R300_VAP_VF_CNTL__PRIM_POINTS : is_line ? R300_VAP_VF_CNTL__PRIM_LINES : R300_VAP_VF_CNTL__PRIM_TRIANGLES;
#define V(i) (&verts[index_at(v, (i), indexed, start) - min_index])
    switch (mode) {
    case PIPE_PRIM_POINTS: for (uint32_t i = 0; i < count; i++) point(&e, V(i)); break;
    case PIPE_PRIM_LINES: for (uint32_t i = 0; i + 1 < count; i += 2) line(&e, V(i), V(i + 1)); break;
    case PIPE_PRIM_LINE_STRIP: for (uint32_t i = 0; i + 1 < count; i++) line(&e, V(i), V(i + 1)); break;
    case PIPE_PRIM_LINE_LOOP: for (uint32_t i = 0; i < count && count > 1; i++) line(&e, V(i), V((i + 1) % count)); break;
    case PIPE_PRIM_TRIANGLES: for (uint32_t i = 0; i + 2 < count; i += 3) triangle(&e, V(i), V(i + 1), V(i + 2)); break;
    case PIPE_PRIM_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) triangle(&e, V(i + 1), V(i), V(i + 2));
            else triangle(&e, V(i), V(i + 1), V(i + 2));
        }
        break;
    case PIPE_PRIM_TRIANGLE_FAN: case PIPE_PRIM_POLYGON: for (uint32_t i = 1; i + 1 < count; i++) triangle(&e, V(0), V(i), V(i + 1)); break;
    case PIPE_PRIM_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) { triangle(&e, V(i), V(i + 1), V(i + 2)); triangle(&e, V(i), V(i + 2), V(i + 3)); }
        break;
    case PIPE_PRIM_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count; i += 2) { triangle(&e, V(i), V(i + 1), V(i + 3)); triangle(&e, V(i), V(i + 3), V(i + 2)); }
        break;
    default: break;
    }
#undef V
    flush_emit(&e);
    r300_reg(&v->hw, R300_RB3D_DSTCACHE_CTLSTAT, R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D | R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
}

void vr_draw_quad(struct r300_vrend *v, struct vr_shader *fs, float x0, float y0, float x1, float y1, float z,
                  float s0, float t0, float s1, float t1, int color_mask, int depth_write)
{
    if (!fs) return;
    int nvary = fs->fs.ninputs > 0 ? 1 : 0;
    /* no rasterizer state for these: no culling, scissor as set */
    int have_rs = v->have_rs;
    uint32_t rs0 = v->rs[0];
    v->rs[0] &= VIRGL_OBJ_RS_S0_SCISSOR(1);
    if (emit_state(v, fs, (const float (*)[4])v->fs_const, v->nfs_const, nvary, 0, color_mask, depth_write)) { v->rs[0] = rs0; v->have_rs = have_rs; return; }
    v->rs[0] = rs0; v->have_rs = have_rs;
    struct r300_hw *hw = &v->hw;
    int vsize = 4 + 4 * nvary;
    const float q[4][4] = { { x0, y0, s0, t0 }, { x1, y0, s1, t0 }, { x1, y1, s1, t1 }, { x0, y1, s0, t1 } };
    r300_reg(hw, R300_VAP_VTX_SIZE, (uint32_t)vsize);
    r300_reg(hw, R300_VAP_VF_MAX_VTX_INDX, 3);
    r300_reg(hw, R300_VAP_VF_MIN_VTX_INDX, 0);
    r300_emit(hw, R300_PACKET3(R300_PACKET3_3D_DRAW_IMMD_2, (uint32_t)(4 * vsize)));
    r300_emit(hw, R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | 4u << 16 | R300_VAP_VF_CNTL__PRIM_QUADS);
    for (int i = 0; i < 4; i++) {
        r300_emit_f(hw, q[i][0]); r300_emit_f(hw, q[i][1]); r300_emit_f(hw, z); r300_emit_f(hw, 1.0f);
        if (nvary) { r300_emit_f(hw, q[i][2]); r300_emit_f(hw, q[i][3]); r300_emit_f(hw, 0); r300_emit_f(hw, 1); }
    }
    r300_reg(hw, R300_RB3D_DSTCACHE_CTLSTAT, R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D | R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
}
