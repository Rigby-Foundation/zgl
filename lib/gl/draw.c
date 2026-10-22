/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* From GL state to a draw: the blend/rasterizer/depth-stencil objects
 * (recreated when the state they encode changes), vertex elements and
 * buffers (client arrays go through a scratch buffer, constant attributes
 * are one element read by every vertex), textures, constants and uniform
 * blocks, DRAW_VBO with instancing, base vertices and primitive restart. */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH_SIZE (4u << 20)

/* ---- state objects ------------------------------------------------------------- */

static uint32_t blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO: return PIPE_BLENDFACTOR_ZERO;
    case GL_ONE: return PIPE_BLENDFACTOR_ONE;
    case GL_SRC_COLOR: return PIPE_BLENDFACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return PIPE_BLENDFACTOR_INV_SRC_COLOR;
    case GL_SRC_ALPHA: return PIPE_BLENDFACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
    case GL_DST_ALPHA: return PIPE_BLENDFACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
    case GL_DST_COLOR: return PIPE_BLENDFACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return PIPE_BLENDFACTOR_INV_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case GL_CONSTANT_COLOR: return PIPE_BLENDFACTOR_CONST_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
    case GL_CONSTANT_ALPHA: return PIPE_BLENDFACTOR_CONST_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
    case GL_SRC1_COLOR: return PIPE_BLENDFACTOR_SRC1_COLOR;
    case GL_ONE_MINUS_SRC1_COLOR: return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
    case GL_SRC1_ALPHA: return PIPE_BLENDFACTOR_SRC1_ALPHA;
    case GL_ONE_MINUS_SRC1_ALPHA: return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
    default: return PIPE_BLENDFACTOR_ONE;
    }
}

static uint32_t blend_func(GLenum e)
{
    switch (e) {
    case GL_FUNC_SUBTRACT: return PIPE_BLEND_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT: return PIPE_BLEND_REVERSE_SUBTRACT;
    case GL_MIN: return PIPE_BLEND_MIN;
    case GL_MAX: return PIPE_BLEND_MAX;
    default: return PIPE_BLEND_ADD;
    }
}

static uint32_t compare_func(GLenum f)
{
    switch (f) {
    case GL_NEVER: return PIPE_FUNC_NEVER; case GL_LESS: return PIPE_FUNC_LESS; case GL_EQUAL: return PIPE_FUNC_EQUAL;
    case GL_LEQUAL: return PIPE_FUNC_LEQUAL; case GL_GREATER: return PIPE_FUNC_GREATER; case GL_NOTEQUAL: return PIPE_FUNC_NOTEQUAL;
    case GL_GEQUAL: return PIPE_FUNC_GEQUAL; default: return PIPE_FUNC_ALWAYS;
    }
}

static uint32_t stencil_op(GLenum op)
{
    switch (op) {
    case GL_ZERO: return PIPE_STENCIL_OP_ZERO; case GL_REPLACE: return PIPE_STENCIL_OP_REPLACE; case GL_INCR: return PIPE_STENCIL_OP_INCR;
    case GL_DECR: return PIPE_STENCIL_OP_DECR; case GL_INCR_WRAP: return PIPE_STENCIL_OP_INCR_WRAP; case GL_DECR_WRAP: return PIPE_STENCIL_OP_DECR_WRAP;
    case GL_INVERT: return PIPE_STENCIL_OP_INVERT; default: return PIPE_STENCIL_OP_KEEP;
    }
}

static uint64_t fnv(const void *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull;
    return h;
}

/* what the current framebuffer can do: depth, stencil */
static void fb_has_ds(int *depth, int *stencil)
{
    if (!zgl->fbo) {
        *depth = 1;
        *stencil = !(zgl->depth_format == VIRGL_FORMAT_Z24X8_UNORM || zgl->depth_format == VIRGL_FORMAT_Z32_FLOAT || zgl->depth_format == VIRGL_FORMAT_Z16_UNORM);
        return;
    }
    const struct zgl_attachment *d = &zgl->fbo->depth, *s = &zgl->fbo->stencil;
    const struct zgl_format *df = d->type == GL_TEXTURE && d->tex ? d->tex->fmt : d->type == GL_RENDERBUFFER && d->rb ? d->rb->fmt : NULL;
    const struct zgl_format *sf = s->type == GL_TEXTURE && s->tex ? s->tex->fmt : s->type == GL_RENDERBUFFER && s->rb ? s->rb->fmt : NULL;
    *depth = df != NULL;
    *stencil = (sf && (sf->kind == ZF_DEPTH_STENCIL || sf->kind == ZF_STENCIL)) || (df && df->kind == ZF_DEPTH_STENCIL && sf);
}

static int is_src1(GLenum f) { return f == GL_SRC1_COLOR || f == GL_ONE_MINUS_SRC1_COLOR || f == GL_SRC1_ALPHA || f == GL_ONE_MINUS_SRC1_ALPHA; }

static void apply_blend(int for_draw)
{
    uint32_t dw[VIRGL_OBJ_BLEND_SIZE - 1] = { 0 };
    int independent = 0;
    for (int i = 1; i < ZGL_MAX_DRAW_BUFFERS; i++) if (zgl->blend[i] != zgl->blend[0] || zgl->color_masks[i] != zgl->color_masks[0]) independent = 1;
    dw[0] = VIRGL_OBJ_BLEND_S0_INDEPENDENT_BLEND_ENABLE(independent) | VIRGL_OBJ_BLEND_S0_LOGICOP_ENABLE(zgl->color_logic_op) |
            VIRGL_OBJ_BLEND_S0_DITHER(zgl->dither) | VIRGL_OBJ_BLEND_S0_ALPHA_TO_COVERAGE(zgl->sample_alpha_to_coverage) |
            VIRGL_OBJ_BLEND_S0_ALPHA_TO_ONE(zgl->sample_alpha_to_one);
    dw[1] = VIRGL_OBJ_BLEND_S1_LOGICOP_FUNC(zgl->logic_op ? zgl->logic_op - GL_CLEAR : 3);
    int dual = 0;
    for (int i = 0; i < (independent ? ZGL_MAX_DRAW_BUFFERS : 1); i++) {
        uint32_t rt = VIRGL_OBJ_BLEND_S2_RT_COLORMASK((uint32_t)zgl->color_masks[i]);
        if (zgl->blend[i] && !zgl->color_logic_op) dual |= is_src1(zgl->blend_src) | is_src1(zgl->blend_dst) | is_src1(zgl->blend_src_a) | is_src1(zgl->blend_dst_a);
        if (!zgl->blend[i] && !zgl->color_logic_op && zgl->host_dual_src) {
            /* the host GL keeps the last blend factors while blending is off, and
             * macOS then refuses draws to several buffers if they were dual-source
             * ones: blend with ONE, ZERO (no change to the result) to replace them */
            rt |= VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(1) | VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(PIPE_BLEND_ADD) |
                  VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(PIPE_BLENDFACTOR_ONE) | VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(PIPE_BLENDFACTOR_ZERO) |
                  VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(PIPE_BLEND_ADD) |
                  VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(PIPE_BLENDFACTOR_ONE) | VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(PIPE_BLENDFACTOR_ZERO);
        }
        else if (zgl->blend[i] && !zgl->color_logic_op)
            rt |= VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(1) | VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(blend_func(zgl->blend_eq)) |
                  VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(blend_factor(zgl->blend_src)) | VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(blend_factor(zgl->blend_dst)) |
                  VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(blend_func(zgl->blend_eq_a)) |
                  VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(blend_factor(zgl->blend_src_a)) | VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(blend_factor(zgl->blend_dst_a));
        dw[2 + i] = rt;
    }
    uint64_t key = fnv(dw, sizeof dw);
    if (key != zgl->blend_key || !zgl->h_blend) {
        if (zgl->h_blend) virgl_destroy_object(zgl->v, zgl->h_blend, VIRGL_OBJECT_BLEND);
        zgl->h_blend = virgl_handle(zgl->v);
        virgl_create_blend(zgl->v, zgl->h_blend, dw);
        virgl_bind_object(zgl->v, zgl->h_blend, VIRGL_OBJECT_BLEND);
        zgl->blend_key = key;
    }
    /* after this draw the host's factors are dual-source ones, or replaced (a clear doesn't send them) */
    if (for_draw) zgl->host_dual_src = dual;
}

static void apply_dsa(void)
{
    int has_depth, has_stencil;
    fb_has_ds(&has_depth, &has_stencil);
    union { float f; uint32_t u; } aref = { zgl->alpha_ref };
    uint32_t dw[VIRGL_OBJ_DSA_SIZE - 1] = { 0 };
    int dt = zgl->depth_test && has_depth;
    dw[0] = VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(dt) | VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(zgl->depth_mask && dt) |
            VIRGL_OBJ_DSA_S0_DEPTH_FUNC(compare_func(zgl->depth_func)) |
            VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(zgl->alpha_test) | VIRGL_OBJ_DSA_S0_ALPHA_FUNC(compare_func(zgl->alpha_func));
    if (zgl->stencil_test && has_stencil)
        for (int f = 0; f < 2; f++)
            dw[1 + f] = VIRGL_OBJ_DSA_S1_STENCIL_ENABLED(1) | VIRGL_OBJ_DSA_S1_STENCIL_FUNC(compare_func(zgl->stencil_func[f])) |
                        VIRGL_OBJ_DSA_S1_STENCIL_FAIL_OP(stencil_op(zgl->stencil_fail[f])) | VIRGL_OBJ_DSA_S1_STENCIL_ZPASS_OP(stencil_op(zgl->stencil_zpass[f])) |
                        VIRGL_OBJ_DSA_S1_STENCIL_ZFAIL_OP(stencil_op(zgl->stencil_zfail[f])) |
                        VIRGL_OBJ_DSA_S1_STENCIL_VALUEMASK(zgl->stencil_vmask[f] & 0xFF) | VIRGL_OBJ_DSA_S1_STENCIL_WRITEMASK(zgl->stencil_wmask[f] & 0xFF);
    dw[3] = aref.u;
    uint64_t key = fnv(dw, sizeof dw);
    if (key != zgl->dsa_key || !zgl->h_dsa) {
        if (zgl->h_dsa) virgl_destroy_object(zgl->v, zgl->h_dsa, VIRGL_OBJECT_DSA);
        zgl->h_dsa = virgl_handle(zgl->v);
        virgl_create_dsa(zgl->v, zgl->h_dsa, dw);
        virgl_bind_object(zgl->v, zgl->h_dsa, VIRGL_OBJECT_DSA);
        zgl->dsa_key = key;
    }
    uint32_t ref = (uint32_t)(zgl->stencil_ref[0] & 0xFF) | (uint32_t)(zgl->stencil_ref[1] & 0xFF) << 8;
    if (!zgl->stencil_ref_valid || ref != zgl->stencil_ref_sent) {
        virgl_set_stencil_ref(zgl->v, (uint32_t)(zgl->stencil_ref[0] & 0xFF), (uint32_t)(zgl->stencil_ref[1] & 0xFF));
        zgl->stencil_ref_sent = ref; zgl->stencil_ref_valid = 1;
    }
}

static void apply_rasterizer(void)
{
    union { float f; uint32_t u; } lw = { zgl->line_width }, ps = { zgl->point_size }, pof = { zgl->po_factor }, pou = { zgl->po_units };
    /* the window's buffer is a scanout resource, which the host renders
     * y-flipped (y_0_top): that turns the winding around, so the front face
     * is the other one there (Mesa does the same); FBO textures are not flipped */
    int front_ccw = (zgl->front_face == GL_CCW) ^ (zgl->fbo == NULL);
    uint32_t dw[VIRGL_OBJ_RS_SIZE - 1] = { 0 };
    uint32_t cull = !zgl->cull ? PIPE_FACE_NONE : zgl->cull_mode == GL_FRONT ? PIPE_FACE_FRONT : zgl->cull_mode == GL_FRONT_AND_BACK ? PIPE_FACE_FRONT_AND_BACK : PIPE_FACE_BACK;
    uint32_t fill = zgl->polygon_mode == GL_LINE ? PIPE_POLYGON_MODE_LINE : zgl->polygon_mode == GL_POINT ? PIPE_POLYGON_MODE_POINT : PIPE_POLYGON_MODE_FILL;
    int program_ps = zgl->program && zgl->program_point_size;
    dw[0] = VIRGL_OBJ_RS_S0_DEPTH_CLIP(!zgl->depth_clamp) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) | VIRGL_OBJ_RS_S0_FRONT_CCW(front_ccw) |
            VIRGL_OBJ_RS_S0_CULL_FACE(cull) | VIRGL_OBJ_RS_S0_FILL_FRONT(fill) | VIRGL_OBJ_RS_S0_FILL_BACK(fill) |
            VIRGL_OBJ_RS_S0_SCISSOR(zgl->scissor) | VIRGL_OBJ_RS_S0_FLATSHADE(zgl->shade_model == GL_FLAT) |
            VIRGL_OBJ_RS_S0_FLATSHADE_FIRST(zgl->provoking_vertex == GL_FIRST_VERTEX_CONVENTION) |
            VIRGL_OBJ_RS_S0_RASTERIZER_DISCARD(zgl->rasterizer_discard) |
            VIRGL_OBJ_RS_S0_OFFSET_TRI(zgl->polygon_offset) | VIRGL_OBJ_RS_S0_OFFSET_LINE(zgl->polygon_offset_line) |
            VIRGL_OBJ_RS_S0_OFFSET_POINT(zgl->polygon_offset_point) | VIRGL_OBJ_RS_S0_POINT_SIZE_PER_VERTEX(program_ps) |
            VIRGL_OBJ_RS_S0_MULTISAMPLE(zgl->multisample) | VIRGL_OBJ_RS_S0_LINE_SMOOTH(zgl->line_smooth) |
            VIRGL_OBJ_RS_S0_POLY_SMOOTH(zgl->polygon_smooth) | VIRGL_OBJ_RS_S0_LINE_LAST_PIXEL(0) |
            VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(0);
    dw[1] = ps.u;                                       /* point size */
    dw[2] = 0;                                          /* sprite coord enable */
    dw[3] = VIRGL_OBJ_RS_S3_CLIP_PLANE_ENABLE(zgl->clip_mask);
    dw[4] = lw.u;                                       /* line width */
    dw[5] = pou.u;                                      /* offset units */
    dw[6] = pof.u;                                      /* offset scale */
    dw[7] = 0;                                          /* offset clamp */
    uint64_t key = fnv(dw, sizeof dw);
    if (key != zgl->rs_key || !zgl->h_rs) {
        if (zgl->h_rs) virgl_destroy_object(zgl->v, zgl->h_rs, VIRGL_OBJECT_RASTERIZER);
        zgl->h_rs = virgl_handle(zgl->v);
        virgl_create_rasterizer(zgl->v, zgl->h_rs, dw);
        virgl_bind_object(zgl->v, zgl->h_rs, VIRGL_OBJECT_RASTERIZER);
        zgl->rs_key = key;
    }
    uint32_t sm = zgl->sample_mask_enabled ? zgl->sample_mask : 0xFFFFFFFFu;
    if (!zgl->sample_mask_valid || sm != zgl->sample_mask_sent) { virgl_set_sample_mask(zgl->v, sm); zgl->sample_mask_sent = sm; zgl->sample_mask_valid = 1; }
}

static void apply_state_objects(void) { apply_blend(1); apply_rasterizer(); apply_dsa(); }

/* a clear follows the colour, depth and stencil write masks */
void zgl_apply_state_for_clear(void) { apply_blend(0); apply_dsa(); }

/* ---- the scratch buffer ---------------------------------------------------------- */

/* Copy `size` bytes into a scratch resource; returns the offset (or -1). */
static long scratch_put_in(virgl_res **res, size_t *used, uint32_t bind, const void *data, size_t size)
{
    if (!*res) {
        *res = virgl_res_create(zgl->v, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, bind, SCRATCH_SIZE, 1, 1, 0, SCRATCH_SIZE);
        if (!*res || !(*res)->map) return -1;
        zgl->scratch_size = SCRATCH_SIZE;
    }
    size_t aligned = (size + 15) & ~(size_t)15;
    if (aligned > zgl->scratch_size) return -1;
    if (*used + aligned > zgl->scratch_size) {
        /* wrap: everything queued must run before the memory is reused */
        virgl_flush(zgl->v);
        *used = 0;
    }
    size_t off = *used;
    memcpy((uint8_t *)(*res)->map + off, data, size);
    virgl_res_dirty(zgl->v, *res, (uint32_t)off, (uint32_t)aligned);   /* uploaded with the next submit */
    *used += aligned;
    return (long)off;
}
static long scratch_put(const void *data, size_t size) { return scratch_put_in(&zgl->scratch, &zgl->scratch_used, VIRGL_BIND_VERTEX_BUFFER, data, size); }
static long iscratch_put(const void *data, size_t size) { return scratch_put_in(&zgl->iscratch, &zgl->iscratch_used, VIRGL_BIND_INDEX_BUFFER, data, size); }

void zgl_flush_scratch(void) { zgl->scratch_used = 0; zgl->iscratch_used = 0; }

/* ---- vertex formats ------------------------------------------------------------- */

static uint32_t pick4(int size, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return size == 1 ? a : size == 2 ? b : size == 3 ? c : d; }

static uint32_t attrib_format(int size, GLenum type, int normalized, int integer, int *elem_size)
{
    if (size == GL_BGRA) { *elem_size = 4; return VIRGL_FORMAT_B8G8R8A8_UNORM; }
    switch (type) {
    case GL_FLOAT:
        *elem_size = 4 * size;
        return pick4(size, VIRGL_FORMAT_R32_FLOAT, VIRGL_FORMAT_R32G32_FLOAT, VIRGL_FORMAT_R32G32B32_FLOAT, VIRGL_FORMAT_R32G32B32A32_FLOAT);
    case GL_HALF_FLOAT:
        *elem_size = 2 * size;
        return pick4(size, VIRGL_FORMAT_R16_FLOAT, VIRGL_FORMAT_R16G16_FLOAT, VIRGL_FORMAT_R16G16B16_FLOAT, VIRGL_FORMAT_R16G16B16A16_FLOAT);
    case GL_UNSIGNED_BYTE:
        *elem_size = size;
        if (integer) return pick4(size, VIRGL_FORMAT_R8_UINT, VIRGL_FORMAT_R8G8_UINT, VIRGL_FORMAT_R8G8B8_UINT, VIRGL_FORMAT_R8G8B8A8_UINT);
        if (normalized) return pick4(size, VIRGL_FORMAT_R8_UNORM, VIRGL_FORMAT_R8G8_UNORM, VIRGL_FORMAT_R8G8B8_UNORM, VIRGL_FORMAT_R8G8B8A8_UNORM);
        return pick4(size, VIRGL_FORMAT_R8_USCALED, VIRGL_FORMAT_R8G8_USCALED, VIRGL_FORMAT_R8G8B8_USCALED, VIRGL_FORMAT_R8G8B8A8_USCALED);
    case GL_BYTE:
        *elem_size = size;
        if (integer) return pick4(size, VIRGL_FORMAT_R8_SINT, VIRGL_FORMAT_R8G8_SINT, VIRGL_FORMAT_R8G8B8_SINT, VIRGL_FORMAT_R8G8B8A8_SINT);
        if (normalized) return pick4(size, VIRGL_FORMAT_R8_SNORM, VIRGL_FORMAT_R8G8_SNORM, VIRGL_FORMAT_R8G8B8_SNORM, VIRGL_FORMAT_R8G8B8A8_SNORM);
        return pick4(size, VIRGL_FORMAT_R8_SSCALED, VIRGL_FORMAT_R8G8_SSCALED, VIRGL_FORMAT_R8G8B8_SSCALED, VIRGL_FORMAT_R8G8B8A8_SSCALED);
    case GL_SHORT:
        *elem_size = 2 * size;
        if (integer) return pick4(size, VIRGL_FORMAT_R16_SINT, VIRGL_FORMAT_R16G16_SINT, VIRGL_FORMAT_R16G16B16_SINT, VIRGL_FORMAT_R16G16B16A16_SINT);
        if (normalized) return pick4(size, VIRGL_FORMAT_R16_SNORM, VIRGL_FORMAT_R16G16_SNORM, VIRGL_FORMAT_R16G16B16_SNORM, VIRGL_FORMAT_R16G16B16A16_SNORM);
        return pick4(size, VIRGL_FORMAT_R16_SSCALED, VIRGL_FORMAT_R16G16_SSCALED, VIRGL_FORMAT_R16G16B16_SSCALED, VIRGL_FORMAT_R16G16B16A16_SSCALED);
    case GL_UNSIGNED_SHORT:
        *elem_size = 2 * size;
        if (integer) return pick4(size, VIRGL_FORMAT_R16_UINT, VIRGL_FORMAT_R16G16_UINT, VIRGL_FORMAT_R16G16B16_UINT, VIRGL_FORMAT_R16G16B16A16_UINT);
        if (normalized) return pick4(size, VIRGL_FORMAT_R16_UNORM, VIRGL_FORMAT_R16G16_UNORM, VIRGL_FORMAT_R16G16B16_UNORM, VIRGL_FORMAT_R16G16B16A16_UNORM);
        return pick4(size, VIRGL_FORMAT_R16_USCALED, VIRGL_FORMAT_R16G16_USCALED, VIRGL_FORMAT_R16G16B16_USCALED, VIRGL_FORMAT_R16G16B16A16_USCALED);
    case GL_INT:
        *elem_size = 4 * size;
        if (integer) return pick4(size, VIRGL_FORMAT_R32_SINT, VIRGL_FORMAT_R32G32_SINT, VIRGL_FORMAT_R32G32B32_SINT, VIRGL_FORMAT_R32G32B32A32_SINT);
        if (normalized) return pick4(size, VIRGL_FORMAT_R32_SNORM, VIRGL_FORMAT_R32G32_SNORM, VIRGL_FORMAT_R32G32B32_SNORM, VIRGL_FORMAT_R32G32B32A32_SNORM);
        return pick4(size, VIRGL_FORMAT_R32_SSCALED, VIRGL_FORMAT_R32G32_SSCALED, VIRGL_FORMAT_R32G32B32_SSCALED, VIRGL_FORMAT_R32G32B32A32_SSCALED);
    case GL_UNSIGNED_INT:
        *elem_size = 4 * size;
        if (integer) return pick4(size, VIRGL_FORMAT_R32_UINT, VIRGL_FORMAT_R32G32_UINT, VIRGL_FORMAT_R32G32B32_UINT, VIRGL_FORMAT_R32G32B32A32_UINT);
        if (normalized) return pick4(size, VIRGL_FORMAT_R32_UNORM, VIRGL_FORMAT_R32G32_UNORM, VIRGL_FORMAT_R32G32B32_UNORM, VIRGL_FORMAT_R32G32B32A32_UNORM);
        return pick4(size, VIRGL_FORMAT_R32_USCALED, VIRGL_FORMAT_R32G32_USCALED, VIRGL_FORMAT_R32G32B32_USCALED, VIRGL_FORMAT_R32G32B32A32_USCALED);
    case GL_INT_2_10_10_10_REV:
        *elem_size = 4;
        return normalized ? VIRGL_FORMAT_R10G10B10A2_SNORM : VIRGL_FORMAT_R10G10B10A2_SSCALED;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        *elem_size = 4;
        return normalized ? VIRGL_FORMAT_R10G10B10A2_UNORM : VIRGL_FORMAT_R10G10B10A2_USCALED;
    case GL_FIXED:
        *elem_size = 4 * size;
        return pick4(size, VIRGL_FORMAT_R32_FIXED, VIRGL_FORMAT_R32G32_FIXED, VIRGL_FORMAT_R32G32B32_FIXED, VIRGL_FORMAT_R32G32B32A32_FIXED);
    case GL_DOUBLE:
        *elem_size = 8 * size;
        return VIRGL_FORMAT_R64_FLOAT;              /* doubles are converted below */
    default:
        *elem_size = 0;
        return VIRGL_FORMAT_NONE;
    }
}

static uint32_t float_bits_format(int size)
{
    return pick4(size, VIRGL_FORMAT_R32_FLOAT, VIRGL_FORMAT_R32G32_FLOAT, VIRGL_FORMAT_R32G32B32_FLOAT, VIRGL_FORMAT_R32G32B32A32_FLOAT);
}

static uint32_t prim_mode(GLenum mode)
{
    switch (mode) {
    case GL_POINTS: return PIPE_PRIM_POINTS; case GL_LINES: return PIPE_PRIM_LINES; case GL_LINE_LOOP: return PIPE_PRIM_LINE_LOOP;
    case GL_LINE_STRIP: return PIPE_PRIM_LINE_STRIP; case GL_TRIANGLES: return PIPE_PRIM_TRIANGLES; case GL_TRIANGLE_STRIP: return PIPE_PRIM_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return PIPE_PRIM_TRIANGLE_FAN; case GL_QUADS: return PIPE_PRIM_QUADS; case GL_QUAD_STRIP: return PIPE_PRIM_QUAD_STRIP;
    case GL_POLYGON: return PIPE_PRIM_POLYGON;
    case GL_LINES_ADJACENCY: return PIPE_PRIM_LINES_ADJACENCY; case GL_LINE_STRIP_ADJACENCY: return PIPE_PRIM_LINE_STRIP_ADJACENCY;
    case GL_TRIANGLES_ADJACENCY: return PIPE_PRIM_TRIANGLES_ADJACENCY; case GL_TRIANGLE_STRIP_ADJACENCY: return PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY;
    default: return 0xFFFFFFFF;
    }
}

/* ---- the draw ---------------------------------------------------------------------- */

/* Fetch index `i` of an index array as uint32. */
static uint32_t index_at(const void *indices, GLenum type, size_t i)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: return ((const uint8_t *)indices)[i];
    case GL_UNSIGNED_SHORT: return ((const uint16_t *)indices)[i];
    default: return ((const uint32_t *)indices)[i];
    }
}

static size_t index_size(GLenum type) { return type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4; }

/* the constant for one attribute nobody feeds: 16 bytes every vertex and instance reads */
#define CONSTANT_DIVISOR 0x7FFFFFFFu

static void zgl_draw_inner(struct zgl_program *prog, const struct zgl_draw_args *a);

void zgl_draw_ex(const struct zgl_draw_args *a)
{
    struct zgl_program *prog = zgl->program ? zgl->program : zgl_ffp_program();
    if (a->count < 0 || a->instances < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (prim_mode(a->mode) == 0xFFFFFFFF) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (a->index_type && a->index_type != GL_UNSIGNED_BYTE && a->index_type != GL_UNSIGNED_SHORT && a->index_type != GL_UNSIGNED_INT) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (zgl->fbo) {
        /* an incomplete framebuffer can't be drawn to */
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { zgl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION); return; }
    }
    if (a->index_type && zgl->vao->element_buffer && zgl->vao->element_buffer->mapped) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (zgl->xfb.active && !zgl->xfb.paused && a->mode != zgl->xfb.mode &&
        !(zgl->xfb.mode == GL_LINES && (a->mode == GL_LINE_LOOP || a->mode == GL_LINE_STRIP)) &&
        !(zgl->xfb.mode == GL_TRIANGLES && (a->mode == GL_TRIANGLE_STRIP || a->mode == GL_TRIANGLE_FAN))) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (!prog || !prog->linked || a->count == 0 || a->instances == 0) return;
    int client_arrays = !zgl->program && !zgl->imm_draw;
    if (client_arrays) zgl_ffp_bind_client_arrays(1);
    zgl_draw_inner(prog, a);
    if (client_arrays) zgl_ffp_bind_client_arrays(0);
}

void zgl_draw(GLenum mode, GLint first, GLsizei count, GLenum index_type, const void *indices)
{
    struct zgl_draw_args a = { .mode = mode, .first = first, .count = count, .index_type = index_type, .indices = indices, .instances = 1 };
    zgl_draw_ex(&a);
}

/* the sampler state and view a SAMP register gets from its texture unit */
static enum zgl_type sampler_target_index(enum zgl_type t)
{
    switch (t) {
    case T_SAMPLER1D: case T_SAMPLER1DSHADOW: case T_ISAMPLER1D: case T_USAMPLER1D: return ZT_1D;
    case T_SAMPLER3D: case T_ISAMPLER3D: case T_USAMPLER3D: return ZT_3D;
    case T_SAMPLERCUBE: case T_SAMPLERCUBESHADOW: case T_ISAMPLERCUBE: case T_USAMPLERCUBE: return ZT_CUBE;
    case T_SAMPLER2DRECT: case T_SAMPLER2DRECTSHADOW: case T_ISAMPLER2DRECT: case T_USAMPLER2DRECT: return ZT_RECT;
    case T_SAMPLER1DARRAY: case T_SAMPLER1DARRAYSHADOW: case T_ISAMPLER1DARRAY: case T_USAMPLER1DARRAY: return ZT_1D_ARRAY;
    case T_SAMPLER2DARRAY: case T_SAMPLER2DARRAYSHADOW: case T_ISAMPLER2DARRAY: case T_USAMPLER2DARRAY: return ZT_2D_ARRAY;
    case T_SAMPLERBUFFER: case T_ISAMPLERBUFFER: case T_USAMPLERBUFFER: return ZT_BUFFER;
    case T_SAMPLER2DMS: case T_ISAMPLER2DMS: case T_USAMPLER2DMS: return ZT_2D_MS;
    case T_SAMPLER2DMSARRAY: case T_ISAMPLER2DMSARRAY: case T_USAMPLER2DMSARRAY: return ZT_2D_MS_ARRAY;
    case T_SAMPLERCUBEARRAY: case T_SAMPLERCUBEARRAYSHADOW: case T_ISAMPLERCUBEARRAY: case T_USAMPLERCUBEARRAY: return ZT_CUBE_ARRAY;
    default: return ZT_2D;
    }
}

static void bind_textures(struct zgl_program *prog)
{
    for (int stage = 0; stage < 2; stage++) {
        const struct zgl_compiled *sc = stage ? &prog->fs->c : &prog->vs->c;
        uint32_t views[ZGL_MAX_VARS], samplers[ZGL_MAX_VARS];
        int ns = 0;
        for (int i = 0; i < sc->nsamplers; i++) {
            const struct zgl_var *sv = &sc->samplers[i];
            const struct zgl_uniform *uu = NULL;
            for (int u = 0; u < prog->nuniforms; u++) if (strcmp(prog->uniforms[u].name, sv->name) == 0) { uu = &prog->uniforms[u]; break; }
            int k = sampler_target_index(sv->type);
            for (int e = 0; e < (sv->array ? sv->array : 1); e++) {
                int s = sv->slot + e;
                if (s >= ZGL_MAX_VARS) continue;
                int unit = uu ? (e < 16 ? uu->sampler_units[e] : 0) : 0;
                struct zgl_texture *t = unit >= 0 && unit < ZGL_MAX_UNITS ? zgl->unit_tex[unit][k] : NULL;
                if (t && zgl_texture_complete(t)) {
                    views[s] = zgl_texture_view(t);
                    samplers[s] = k == ZT_BUFFER ? zgl_sampler_state(t, NULL) : zgl_sampler_state(t, zgl->unit_sampler[unit]);
                } else { views[s] = 0; samplers[s] = 0; }
                if (s + 1 > ns) ns = s + 1;
            }
        }
        if (ns > 0) {
            virgl_set_sampler_views(zgl->v, stage ? PIPE_SHADER_FRAGMENT : PIPE_SHADER_VERTEX, 0, views, (uint32_t)ns);
            virgl_bind_sampler_states(zgl->v, stage ? PIPE_SHADER_FRAGMENT : PIPE_SHADER_VERTEX, 0, samplers, (uint32_t)ns);
        }
    }
}

/* uniform blocks: the bytes of each block's binding point's buffer range go
 * into the constants the compiler gave the block (std140 there too), when
 * the buffer, the range or its contents changed */
static void fill_uniform_blocks(struct zgl_program *prog)
{
    for (int i = 0; i < prog->nblocks; i++) {
        int bp = prog->blocks[i].binding;
        struct zgl_buffer *b = bp >= 0 && bp < ZGL_MAX_UBO_BINDINGS ? zgl->ubo[bp].buf : NULL;
        if (!b || !b->shadow) continue;
        size_t off = zgl->ubo[bp].off;
        if (b->host_newer) zgl_buffer_sync(b);
        if (prog->blocks[i].src == b && prog->blocks[i].src_off == off && prog->blocks[i].src_version == b->version) continue;
        size_t avail = off < b->size ? b->size - off : 0;
        if (zgl->ubo[bp].size && zgl->ubo[bp].size < avail) avail = zgl->ubo[bp].size;
        size_t n = (size_t)prog->blocks[i].size < avail ? (size_t)prog->blocks[i].size : avail;
        if (prog->blocks[i].vs_index >= 0) memcpy(&prog->vs_consts[prog->blocks[i].vs_index * 4], b->shadow + off, n);
        if (prog->blocks[i].fs_index >= 0) memcpy(&prog->fs_consts[prog->blocks[i].fs_index * 4], b->shadow + off, n);
        prog->blocks[i].src = b; prog->blocks[i].src_off = off; prog->blocks[i].src_version = b->version;
        prog->consts_dirty = 1;
    }
}

/* textureSize() of buffer samplers: their element counts, in the hidden uniforms */
static void fill_tbo_sizes(struct zgl_program *prog)
{
    size_t plen = strlen(ZGL_TBSIZE_PREFIX);
    for (int i = 0; i < prog->nuniforms; i++) {
        struct zgl_uniform *u = &prog->uniforms[i];
        if (strncmp(u->name, ZGL_TBSIZE_PREFIX, plen)) continue;
        int32_t n = 0;
        for (int k = 0; k < prog->nuniforms; k++)
            if (!strcmp(prog->uniforms[k].name, u->name + plen)) {
                int unit = prog->uniforms[k].sampler_units[0];
                struct zgl_texture *t = unit >= 0 && unit < ZGL_MAX_UNITS ? zgl->unit_tex[unit][ZT_BUFFER] : NULL;
                if (t) n = zgl_tbo_elements(t);
            }
        float *dst[2] = { u->vs_slot >= 0 ? &prog->vs_consts[u->vs_slot * 4] : NULL, u->fs_slot >= 0 ? &prog->fs_consts[u->fs_slot * 4] : NULL };
        for (int st = 0; st < 2; st++)
            if (dst[st] && memcmp(dst[st], &n, 4)) { memcpy(dst[st], &n, 4); prog->consts_dirty = 1; }
    }
}

static void zgl_draw_inner(struct zgl_program *prog, const struct zgl_draw_args *a)
{
    GLenum mode = a->mode;
    uint32_t pmode = prim_mode(mode);
    zgl_apply_framebuffer();
    apply_state_objects();
    virgl_bind_shader(zgl->v, prog->h_vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(zgl->v, prog->h_fs, PIPE_SHADER_FRAGMENT);

    struct zgl_buffer *eb = zgl->vao->element_buffer;
    int indexed = a->index_type != 0;
    GLsizei count = a->count;
    GLint first = a->first;

    /* does any attribute come from client memory? then the vertex range must be known */
    int need_range = 0;
    for (int i = 0; i < prog->nin && i < ZGL_MAX_ATTRIBS; i++) {
        int loc = prog->in_loc[i];
        if (i == prog->instance_in || loc < 0) continue;
        struct zgl_attrib *at = &ZGL_ATTRIBS[loc];
        int narrow_int = at->integer && at->type != GL_INT && at->type != GL_UNSIGNED_INT;
        if (at->enabled && at->size > 0 && !at->divisor && (!at->buffer || at->type == GL_DOUBLE || narrow_int) && (at->pointer || at->buffer)) need_range = 1;
    }

    /* the indices: an element buffer is used as it is when the host can draw
     * the primitive; client indices and the primitives core GL can't draw
     * (quads, polygons, line loops from client memory) go through scratch */
    uint32_t *idx32 = NULL;
    size_t nidx = (size_t)count;
    uint32_t min_index = 0, max_index = 0xFFFFFFFF;
    uint32_t ib_res = 0, ib_offset = 0, ib_size = 4;
    int rewrite = mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON || mode == GL_LINE_LOOP;
    int restart = zgl->primitive_restart && indexed;
    const void *isrc = a->indices;
    if (indexed && eb) {
        if (!eb->shadow) { zgl_set_error(GL_INVALID_OPERATION); return; }
        size_t off = (size_t)(uintptr_t)a->indices;
        if (off + index_size(a->index_type) * count > eb->size) { zgl_set_error(GL_INVALID_OPERATION); return; }
        isrc = NULL;
        if (rewrite || need_range) { zgl_buffer_sync(eb); isrc = eb->shadow + off; }
    } else if (indexed && !a->indices) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (rewrite || (indexed && !eb)) {
        size_t cap = (size_t)count * 3 + 8;
        idx32 = malloc(cap * 4);
        if (!idx32) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
        size_t n = 0;
#define IDX(k) (indexed ? index_at(isrc, a->index_type, (k)) : (uint32_t)(first + (k)))
        if (mode == GL_QUADS) {
            for (GLsizei q = 0; q + 3 < count; q += 4) {
                uint32_t i0 = IDX(q), i1 = IDX(q + 1), i2 = IDX(q + 2), i3 = IDX(q + 3);
                idx32[n++] = i0; idx32[n++] = i1; idx32[n++] = i2; idx32[n++] = i0; idx32[n++] = i2; idx32[n++] = i3;
            }
            pmode = PIPE_PRIM_TRIANGLES;
        } else if (mode == GL_QUAD_STRIP) {
            for (GLsizei q = 0; q + 3 < count; q += 2) {
                uint32_t i0 = IDX(q), i1 = IDX(q + 1), i2 = IDX(q + 2), i3 = IDX(q + 3);
                idx32[n++] = i0; idx32[n++] = i1; idx32[n++] = i3; idx32[n++] = i0; idx32[n++] = i3; idx32[n++] = i2;
            }
            pmode = PIPE_PRIM_TRIANGLES;
        } else if (mode == GL_POLYGON) {
            for (GLsizei i = 0; i < count; i++) idx32[n++] = IDX(i);
            pmode = PIPE_PRIM_TRIANGLE_FAN;
        } else if (mode == GL_LINE_LOOP) {
            for (GLsizei i = 0; i < count; i++) idx32[n++] = IDX(i);
            idx32[n++] = idx32[0];
            pmode = PIPE_PRIM_LINE_STRIP;
        } else {
            for (GLsizei i = 0; i < count; i++) idx32[n++] = IDX(i);
        }
#undef IDX
        nidx = n;
        min_index = 0xFFFFFFFF; max_index = 0;
        uint32_t ri = a->index_type == GL_UNSIGNED_BYTE ? (zgl->restart_index & 0xFF) : a->index_type == GL_UNSIGNED_SHORT ? (zgl->restart_index & 0xFFFF) : zgl->restart_index;
        for (size_t i = 0; i < n; i++) {
            if (restart && idx32[i] == ri) { idx32[i] = 0xFFFFFFFFu; continue; }
            if (idx32[i] < min_index) min_index = idx32[i];
            if (idx32[i] > max_index) max_index = idx32[i];
        }
        long off = iscratch_put(idx32, n * 4);
        free(idx32); idx32 = NULL;
        if (off < 0) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
        ib_res = zgl->iscratch->id; ib_offset = (uint32_t)off; ib_size = 4;
        indexed = 1;
    } else if (indexed) {
        ib_res = eb->res->id; ib_offset = (uint32_t)(uintptr_t)a->indices; ib_size = (uint32_t)index_size(a->index_type);
        if (need_range) {
            min_index = 0xFFFFFFFF; max_index = 0;
            uint32_t ri = a->index_type == GL_UNSIGNED_BYTE ? (zgl->restart_index & 0xFF) : a->index_type == GL_UNSIGNED_SHORT ? (zgl->restart_index & 0xFFFF) : zgl->restart_index;
            for (GLsizei i = 0; i < count; i++) {
                uint32_t x = index_at(isrc, a->index_type, (size_t)i);
                if (restart && x == ri) continue;
                if (x < min_index) min_index = x;
                if (x > max_index) max_index = x;
            }
        } else if (a->range_known) { min_index = a->range_start; max_index = a->range_end; }
    }
    if (indexed && max_index != 0xFFFFFFFF && min_index > max_index) return;     /* only restart indices */
    /* the range of vertices the attributes must cover (with the base vertex) */
    uint32_t vfirst, vcount;
    if (indexed) {
        int known = max_index != 0xFFFFFFFF;
        vfirst = known ? (uint32_t)((int64_t)min_index + a->base_vertex) : 0;
        vcount = known ? max_index - min_index + 1 : 0;
    } else { vfirst = (uint32_t)first; vcount = (uint32_t)count; }

    /* vertex elements: VS input register i comes from location in_loc[i] */
    int nel = prog->nin;
    uint32_t ve_off[ZGL_MAX_ATTRIBS], ve_buf[ZGL_MAX_ATTRIBS], ve_fmt[ZGL_MAX_ATTRIBS], ve_div[ZGL_MAX_ATTRIBS];
    uint32_t vb_stride[ZGL_MAX_ATTRIBS], vb_off[ZGL_MAX_ATTRIBS], vb_res[ZGL_MAX_ATTRIBS];
    int nvb = 0;
    for (int i = 0; i < nel && i < ZGL_MAX_ATTRIBS; i++) {
        ve_off[i] = 0; ve_div[i] = 0;
        if (i == prog->instance_in) {
            /* gl_InstanceID: a hidden per-instance attribute counting 0, 1, ... */
            uint32_t ni = (uint32_t)(a->instances > 0 ? a->instances : 1);
            uint32_t *ids = malloc(ni * 4);
            if (!ids) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
            for (uint32_t k = 0; k < ni; k++) ids[k] = k;
            long off = scratch_put(ids, ni * 4);
            free(ids);
            if (off < 0) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
            ve_buf[i] = (uint32_t)nvb; ve_fmt[i] = VIRGL_FORMAT_R32_FLOAT; ve_div[i] = 1;     /* int bits, as for integer attributes */
            vb_stride[nvb] = 4; vb_off[nvb] = (uint32_t)off; vb_res[nvb] = zgl->scratch->id;
            nvb++;
            continue;
        }
        int loc = prog->in_loc[i];
        struct zgl_attrib *at = loc >= 0 ? &ZGL_ATTRIBS[loc] : NULL;
        int elem_size = 0;
        if (at && at->enabled && (at->pointer != NULL || at->buffer) && at->size > 0) {
            uint32_t fmt = attrib_format(at->size, at->type, at->normalized, at->integer, &elem_size);
            /* integer attributes: virglrenderer (1.3.0) only declares integer shader
             * inputs on hosts with vertex attrib binding (GL 4.3), and macOS has
             * 4.1; so the 32-bit values go in float formats unchanged (the shader
             * reads its inputs as raw bits), narrower ones widened first */
            int widen = at->integer && at->type != GL_INT && at->type != GL_UNSIGNED_INT;
            if (at->integer) fmt = float_bits_format(at->size);
            int stride = at->stride ? at->stride : elem_size;
            ve_div[i] = (uint32_t)at->divisor;
            if (at->buffer && at->type != GL_DOUBLE && !widen) {
                if (!at->buffer->res) { zgl_set_error(GL_INVALID_OPERATION); return; }
                if (at->buffer->mapped) { zgl_set_error(GL_INVALID_OPERATION); return; }
                ve_buf[i] = (uint32_t)nvb; ve_fmt[i] = fmt;
                vb_stride[nvb] = (uint32_t)stride; vb_off[nvb] = (uint32_t)(uintptr_t)at->pointer; vb_res[nvb] = at->buffer->res->id;
                nvb++;
            } else {
                /* client memory (or doubles): copy the range the draw reads */
                uint32_t nv;
                if (at->divisor) nv = (uint32_t)((a->instances + at->divisor - 1) / at->divisor);
                else {
                    if (vcount == 0 && indexed) { zgl_set_error(GL_INVALID_OPERATION); return; }
                    nv = vfirst + vcount;
                }
                size_t bytes = (size_t)stride * (nv ? nv - 1 : 0) + (size_t)elem_size;
                const void *src = at->pointer;
                if (at->buffer) {
                    size_t boff = (size_t)(uintptr_t)at->pointer;
                    if (!at->buffer->shadow || boff + bytes > at->buffer->size) { zgl_set_error(GL_INVALID_OPERATION); return; }
                    zgl_buffer_sync(at->buffer);
                    src = at->buffer->shadow + boff;
                }
                long off;
                if (widen) {
                    int32_t *tmp = malloc((size_t)nv * at->size * 4 + 4);
                    if (!tmp) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
                    for (uint32_t k = 0; k < nv; k++) {
                        const uint8_t *e = (const uint8_t *)src + (size_t)k * stride;
                        for (int j = 0; j < at->size; j++)
                            tmp[k * at->size + j] = at->type == GL_BYTE ? ((const int8_t *)e)[j] : at->type == GL_UNSIGNED_BYTE ? e[j] :
                                                    at->type == GL_SHORT ? ((const int16_t *)e)[j] : ((const uint16_t *)e)[j];
                    }
                    off = scratch_put(tmp, (size_t)nv * at->size * 4);
                    free(tmp);
                    stride = 4 * at->size;
                } else if (at->type == GL_DOUBLE) {
                    float *tmp = malloc((size_t)nv * at->size * 4 + 4);
                    if (!tmp) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
                    for (uint32_t k = 0; k < nv; k++)
                        for (int j = 0; j < at->size; j++) tmp[k * at->size + j] = (float)((const double *)((const uint8_t *)src + (size_t)k * stride))[j];
                    off = scratch_put(tmp, (size_t)nv * at->size * 4);
                    free(tmp);
                    fmt = attrib_format(at->size, GL_FLOAT, 0, 0, &elem_size);
                    stride = elem_size;
                } else off = scratch_put(src, bytes);
                if (off < 0) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
                ve_buf[i] = (uint32_t)nvb; ve_fmt[i] = fmt;
                vb_stride[nvb] = (uint32_t)stride; vb_off[nvb] = (uint32_t)off; vb_res[nvb] = zgl->scratch->id;
                nvb++;
            }
        } else {
            /* the current value: one element that every vertex and instance reads */
            uint32_t v[4] = { 0, 0, 0, 0 };
            GLenum ty = GL_FLOAT;
            if (loc >= 0) { memcpy(v, zgl->cur_attrib[loc].v.u, 16); ty = zgl->cur_attrib[loc].type ? zgl->cur_attrib[loc].type : GL_FLOAT; }
            else { float one = 1.0f; memcpy(&v[3], &one, 4); }
            long off = scratch_put(v, 16);
            if (off < 0) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
            ve_buf[i] = (uint32_t)nvb;
            (void)ty;                                   /* integers too: their bits in a float format (see above) */
            ve_fmt[i] = VIRGL_FORMAT_R32G32B32A32_FLOAT;
            ve_div[i] = CONSTANT_DIVISOR;
            vb_stride[nvb] = 16; vb_off[nvb] = (uint32_t)off; vb_res[nvb] = zgl->scratch->id;
            nvb++;
        }
    }
    if (nel > 0) {
        uint64_t k1 = fnv(ve_fmt, sizeof ve_fmt[0] * nel) ^ (fnv(ve_div, sizeof ve_div[0] * nel) * 31) ^ (uint64_t)nel;
        if (k1 != zgl->ve_key || !zgl->h_ve) {
            if (zgl->h_ve) virgl_destroy_object(zgl->v, zgl->h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
            zgl->h_ve = virgl_handle(zgl->v);
            virgl_create_vertex_elements_ex(zgl->v, zgl->h_ve, (uint32_t)nel, ve_off, ve_div, ve_buf, ve_fmt);
            virgl_bind_object(zgl->v, zgl->h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
            zgl->ve_key = k1;
        }
        virgl_set_vertex_buffers(zgl->v, (uint32_t)nvb, vb_stride, vb_off, vb_res);
    }

    /* constants: the gl_* state, then the program's uniforms; uniform blocks */
    zgl_ffp_fill_state(prog);
    fill_uniform_blocks(prog);
    fill_tbo_sizes(prog);
    if (prog->consts_dirty) {
        if (prog->vs_nconsts) virgl_set_constants(zgl->v, PIPE_SHADER_VERTEX, prog->vs_consts, (uint32_t)prog->vs_nconsts * 4);
        if (prog->fs_nconsts) virgl_set_constants(zgl->v, PIPE_SHADER_FRAGMENT, prog->fs_consts, (uint32_t)prog->fs_nconsts * 4);
        prog->consts_dirty = 0;
    }
    bind_textures(prog);

    if (indexed) virgl_set_index_buffer(zgl->v, ib_res, ib_size, ib_offset);
    struct virgl_draw_info d = { .mode = pmode, .count = (uint32_t)nidx, .indexed = indexed, .instance_count = (uint32_t)a->instances };
    if (indexed) {
        d.start = 0;                                    /* the index buffer binding has the offset */
        d.index_bias = a->base_vertex;
        d.min_index = min_index == 0xFFFFFFFF ? 0 : min_index;
        d.max_index = max_index;
        d.primitive_restart = restart;
        /* indices rewritten into scratch marked restarts as ~0 */
        if (zgl->iscratch && ib_res == zgl->iscratch->id) d.restart_index = 0xFFFFFFFFu;
        else d.restart_index = ib_size == 1 ? (zgl->restart_index & 0xFF) : ib_size == 2 ? (zgl->restart_index & 0xFFFF) : zgl->restart_index;
    } else {
        d.start = (uint32_t)first;
        d.min_index = (uint32_t)first;
        d.max_index = (uint32_t)(first + count - 1);
    }
    virgl_draw_ex(zgl->v, &d);
    for (int i = 0; i < ZGL_MAX_XFB_BUFFERS; i++) if (zgl->xfb.active && zgl->xfb.buf[i]) zgl->xfb.buf[i]->host_newer = 1;
    zgl->stats_draws++;
}
