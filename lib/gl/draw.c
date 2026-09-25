/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* From GL state to a draw: framebuffer and viewport, the blend/rasterizer/
 * depth-stencil objects (recreated when the state they encode changes),
 * vertex elements and buffers (client arrays and constant attributes go
 * through a scratch buffer), textures, constants, DRAW_VBO. */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH_SIZE (4u << 20)

/* ---- framebuffer, viewport ---------------------------------------------------- */

/* The size of what is being drawn on: the window or the bound FBO's texture. */
void zgl_rt_size(int *w, int *h)
{
    if (zgl->fbo && zgl->fbo->tex) { *w = zgl->fbo->tex->w >> zgl->fbo->level; *h = zgl->fbo->tex->h >> zgl->fbo->level; if (*w < 1) *w = 1; if (*h < 1) *h = 1; }
    else { *w = zgl->width; *h = zgl->height; }
}

void zgl_apply_framebuffer(void)
{
    if (zgl->fb_dirty) {
        struct zgl_fbo *f = zgl->fbo;
        if (f && f->tex && f->tex->res) {
            /* a surface on the texture level; remade if the texture was reallocated */
            if (f->h_surf && f->surf_res != f->tex->res) { virgl_destroy_object(zgl->v, f->h_surf, VIRGL_OBJECT_SURFACE); f->h_surf = 0; }
            if (!f->h_surf) {
                f->h_surf = virgl_handle(zgl->v);
                virgl_create_surface(zgl->v, f->h_surf, f->tex->res->id, f->tex->vformat, (uint32_t)f->level, 0);
                f->surf_res = f->tex->res;
            }
            virgl_set_framebuffer(zgl->v, 0, &f->h_surf, 1);        /* no depth on FBOs */
        } else {
            uint32_t cb = zgl->h_color_surf;
            virgl_set_framebuffer(zgl->v, zgl->h_depth_surf, &cb, 1);
        }
        zgl_rt_size(&zgl->rt_w, &zgl->rt_h);
        zgl->fb_dirty = 0;
        zgl->vp_dirty = 1;
    }
    if (zgl->vp_dirty) {
        virgl_set_viewport(zgl->v, (float)zgl->vp_x, (float)zgl->vp_y, (float)zgl->vp_w, (float)zgl->vp_h, zgl->depth_near, zgl->depth_far, 0);
        if (zgl->scissor) virgl_set_scissor(zgl->v, (uint32_t)zgl->sc_x, (uint32_t)zgl->sc_y, (uint32_t)(zgl->sc_x + zgl->sc_w), (uint32_t)(zgl->sc_y + zgl->sc_h));
        else virgl_set_scissor(zgl->v, 0, 0, (uint32_t)zgl->rt_w, (uint32_t)zgl->rt_h);
        zgl->vp_dirty = 0;
    }
}

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

static void apply_state_objects(void)
{
    /* blend */
    uint64_t bkey = (uint64_t)zgl->blend << 40 | (uint64_t)zgl->color_mask << 36 | (uint64_t)blend_func(zgl->blend_eq) << 32 |
                    blend_factor(zgl->blend_src) << 24 | blend_factor(zgl->blend_dst) << 16 | blend_factor(zgl->blend_src_a) << 8 | blend_factor(zgl->blend_dst_a);
    if (bkey != zgl->blend_key || !zgl->h_blend) {
        uint32_t dw[VIRGL_OBJ_BLEND_SIZE - 1] = { 0 };
        dw[2] = VIRGL_OBJ_BLEND_S2_RT_COLORMASK((uint32_t)zgl->color_mask);
        if (zgl->blend)
            dw[2] |= VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(1) | VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(blend_func(zgl->blend_eq)) |
                     VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(blend_factor(zgl->blend_src)) | VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(blend_factor(zgl->blend_dst)) |
                     VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(blend_func(zgl->blend_eq)) |
                     VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(blend_factor(zgl->blend_src_a)) | VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(blend_factor(zgl->blend_dst_a));
        if (zgl->h_blend) virgl_destroy_object(zgl->v, zgl->h_blend, VIRGL_OBJECT_BLEND);
        zgl->h_blend = virgl_handle(zgl->v);
        virgl_create_blend(zgl->v, zgl->h_blend, dw);
        virgl_bind_object(zgl->v, zgl->h_blend, VIRGL_OBJECT_BLEND);
        zgl->blend_key = bkey;
    }
    /* rasterizer */
    union { float f; uint32_t u; } lw = { zgl->line_width }, ps = { zgl->point_size }, pof = { zgl->po_factor }, pou = { zgl->po_units };
    uint64_t rkey = (uint64_t)zgl->cull << 60 | (uint64_t)(zgl->cull_mode & 0xF) << 56 | (uint64_t)(zgl->front_face == GL_CCW) << 55 |
                    (uint64_t)(zgl->polygon_mode & 0xF) << 50 | (uint64_t)zgl->scissor << 49 | (uint64_t)(zgl->shade_model == GL_FLAT) << 48 |
                    (uint64_t)zgl->polygon_offset << 47 | (uint64_t)(lw.u >> 8) << 24 | (uint64_t)(ps.u >> 8) | (uint64_t)(pof.u ^ pou.u) << 8;
    if (rkey != zgl->rs_key || !zgl->h_rs) {
        uint32_t dw[VIRGL_OBJ_RS_SIZE - 1] = { 0 };
        uint32_t cull = !zgl->cull ? PIPE_FACE_NONE : zgl->cull_mode == GL_FRONT ? PIPE_FACE_FRONT : zgl->cull_mode == GL_FRONT_AND_BACK ? PIPE_FACE_FRONT_AND_BACK : PIPE_FACE_BACK;
        uint32_t fill = zgl->polygon_mode == GL_LINE ? PIPE_POLYGON_MODE_LINE : zgl->polygon_mode == GL_POINT ? PIPE_POLYGON_MODE_POINT : PIPE_POLYGON_MODE_FILL;
        dw[0] = VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) | VIRGL_OBJ_RS_S0_FRONT_CCW(zgl->front_face == GL_CCW) |
                VIRGL_OBJ_RS_S0_CULL_FACE(cull) | VIRGL_OBJ_RS_S0_FILL_FRONT(fill) | VIRGL_OBJ_RS_S0_FILL_BACK(fill) |
                VIRGL_OBJ_RS_S0_SCISSOR(zgl->scissor) | VIRGL_OBJ_RS_S0_FLATSHADE(zgl->shade_model == GL_FLAT) |
                VIRGL_OBJ_RS_S0_OFFSET_TRI(zgl->polygon_offset) | VIRGL_OBJ_RS_S0_POINT_SIZE_PER_VERTEX(0) | VIRGL_OBJ_RS_S0_LINE_LAST_PIXEL(0);
        dw[1] = ps.u;                                       /* point size */
        dw[2] = 0;                                          /* sprite coord enable */
        dw[3] = 0;                                          /* line stipple / clip planes */
        dw[4] = lw.u;                                       /* line width */
        dw[5] = pou.u;                                      /* offset units */
        dw[6] = pof.u;                                      /* offset scale */
        dw[7] = 0;                                          /* offset clamp */
        if (zgl->h_rs) virgl_destroy_object(zgl->v, zgl->h_rs, VIRGL_OBJECT_RASTERIZER);
        zgl->h_rs = virgl_handle(zgl->v);
        virgl_create_rasterizer(zgl->v, zgl->h_rs, dw);
        virgl_bind_object(zgl->v, zgl->h_rs, VIRGL_OBJECT_RASTERIZER);
        zgl->rs_key = rkey;
    }
    /* depth / stencil / alpha */
    union { float f; uint32_t u; } aref = { zgl->alpha_ref };
    uint64_t dkey = (uint64_t)zgl->depth_test << 40 | (uint64_t)zgl->depth_mask << 39 | (uint64_t)compare_func(zgl->depth_func) << 36 |
                    (uint64_t)zgl->alpha_test << 35 | (uint64_t)compare_func(zgl->alpha_func) << 32 | aref.u;
    if (dkey != zgl->dsa_key || !zgl->h_dsa) {
        uint32_t dw[VIRGL_OBJ_DSA_SIZE - 1] = { 0 };
        dw[0] = VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(zgl->depth_test) | VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(zgl->depth_mask && zgl->depth_test) |
                VIRGL_OBJ_DSA_S0_DEPTH_FUNC(compare_func(zgl->depth_func)) |
                VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(zgl->alpha_test) | VIRGL_OBJ_DSA_S0_ALPHA_FUNC(compare_func(zgl->alpha_func));
        dw[3] = aref.u;
        if (zgl->h_dsa) virgl_destroy_object(zgl->v, zgl->h_dsa, VIRGL_OBJECT_DSA);
        zgl->h_dsa = virgl_handle(zgl->v);
        virgl_create_dsa(zgl->v, zgl->h_dsa, dw);
        virgl_bind_object(zgl->v, zgl->h_dsa, VIRGL_OBJECT_DSA);
        zgl->dsa_key = dkey;
    }
}

/* ---- the scratch buffer ---------------------------------------------------------- */

/* Copy `size` bytes into a scratch resource (vertex or index: virgl wants
 * one bind per buffer); returns the offset (or -1). */
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
static long scratch_put(const void *data, size_t size)
{
    return scratch_put_in(&zgl->scratch, &zgl->scratch_used, VIRGL_BIND_VERTEX_BUFFER, data, size);
}
static long iscratch_put(const void *data, size_t size)
{
    return scratch_put_in(&zgl->iscratch, &zgl->iscratch_used, VIRGL_BIND_INDEX_BUFFER, data, size);
}

void zgl_flush_scratch(void) { zgl->scratch_used = 0; zgl->iscratch_used = 0; }

/* ---- vertex formats ------------------------------------------------------------- */

static uint32_t attrib_format(int size, GLenum type, int normalized, int *elem_size)
{
    switch (type) {
    case GL_FLOAT:
        *elem_size = 4 * size;
        return size == 1 ? VIRGL_FORMAT_R32_FLOAT : size == 2 ? VIRGL_FORMAT_R32G32_FLOAT : size == 3 ? VIRGL_FORMAT_R32G32B32_FLOAT : VIRGL_FORMAT_R32G32B32A32_FLOAT;
    case GL_UNSIGNED_BYTE:
        *elem_size = size;
        if (normalized) return size == 1 ? VIRGL_FORMAT_R8_UNORM : size == 2 ? VIRGL_FORMAT_R8G8_UNORM : size == 3 ? VIRGL_FORMAT_R8G8B8_UNORM : VIRGL_FORMAT_R8G8B8A8_UNORM;
        return size == 1 ? VIRGL_FORMAT_R8_USCALED : size == 2 ? VIRGL_FORMAT_R8G8_USCALED : size == 3 ? VIRGL_FORMAT_R8G8B8_USCALED : VIRGL_FORMAT_R8G8B8A8_USCALED;
    case GL_BYTE:
        *elem_size = size;
        if (normalized) return size == 1 ? VIRGL_FORMAT_R8_SNORM : size == 2 ? VIRGL_FORMAT_R8G8_SNORM : size == 3 ? VIRGL_FORMAT_R8G8B8_SNORM : VIRGL_FORMAT_R8G8B8A8_SNORM;
        return size == 1 ? VIRGL_FORMAT_R8_SSCALED : size == 2 ? VIRGL_FORMAT_R8G8_SSCALED : size == 3 ? VIRGL_FORMAT_R8G8B8_SSCALED : VIRGL_FORMAT_R8G8B8A8_SSCALED;
    case GL_SHORT:
        *elem_size = 2 * size;
        if (normalized) return size == 1 ? VIRGL_FORMAT_R16_SNORM : size == 2 ? VIRGL_FORMAT_R16G16_SNORM : size == 3 ? VIRGL_FORMAT_R16G16B16_SNORM : VIRGL_FORMAT_R16G16B16A16_SNORM;
        return size == 1 ? VIRGL_FORMAT_R16_SSCALED : size == 2 ? VIRGL_FORMAT_R16G16_SSCALED : size == 3 ? VIRGL_FORMAT_R16G16B16_SSCALED : VIRGL_FORMAT_R16G16B16A16_SSCALED;
    case GL_UNSIGNED_SHORT:
        *elem_size = 2 * size;
        if (normalized) return size == 1 ? VIRGL_FORMAT_R16_UNORM : size == 2 ? VIRGL_FORMAT_R16G16_UNORM : size == 3 ? VIRGL_FORMAT_R16G16B16_UNORM : VIRGL_FORMAT_R16G16B16A16_UNORM;
        return size == 1 ? VIRGL_FORMAT_R16_USCALED : size == 2 ? VIRGL_FORMAT_R16G16_USCALED : size == 3 ? VIRGL_FORMAT_R16G16B16_USCALED : VIRGL_FORMAT_R16G16B16A16_USCALED;
    case GL_INT:
        *elem_size = 4 * size;
        return size == 1 ? VIRGL_FORMAT_R32_SSCALED : size == 2 ? VIRGL_FORMAT_R32G32_SSCALED : size == 3 ? VIRGL_FORMAT_R32G32B32_SSCALED : VIRGL_FORMAT_R32G32B32A32_SSCALED;
    case GL_UNSIGNED_INT:
        *elem_size = 4 * size;
        return size == 1 ? VIRGL_FORMAT_R32_USCALED : size == 2 ? VIRGL_FORMAT_R32G32_USCALED : size == 3 ? VIRGL_FORMAT_R32G32B32_USCALED : VIRGL_FORMAT_R32G32B32A32_USCALED;
    case GL_DOUBLE:
        *elem_size = 8 * size;
        return VIRGL_FORMAT_R64_FLOAT;              /* doubles are converted below */
    default:
        *elem_size = 0;
        return VIRGL_FORMAT_NONE;
    }
}

static uint32_t prim_mode(GLenum mode)
{
    switch (mode) {
    case GL_POINTS: return PIPE_PRIM_POINTS; case GL_LINES: return PIPE_PRIM_LINES; case GL_LINE_LOOP: return PIPE_PRIM_LINE_LOOP;
    case GL_LINE_STRIP: return PIPE_PRIM_LINE_STRIP; case GL_TRIANGLES: return PIPE_PRIM_TRIANGLES; case GL_TRIANGLE_STRIP: return PIPE_PRIM_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return PIPE_PRIM_TRIANGLE_FAN; case GL_QUADS: return PIPE_PRIM_QUADS; case GL_QUAD_STRIP: return PIPE_PRIM_QUAD_STRIP;
    case GL_POLYGON: return PIPE_PRIM_POLYGON; default: return 0xFFFFFFFF;
    }
}

/* ---- the draw ---------------------------------------------------------------------- */

struct attrib_src {                          /* what feeds VS input i */
    int loc;
    struct zgl_attrib *a;
};

/* Fetch index `i` of a client index array as uint32. */
static uint32_t index_at(const void *indices, GLenum type, size_t i)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: return ((const uint8_t *)indices)[i];
    case GL_UNSIGNED_SHORT: return ((const uint16_t *)indices)[i];
    default: return ((const uint32_t *)indices)[i];
    }
}

static void zgl_draw_inner(struct zgl_program *prog, uint32_t pmode, GLenum mode, GLint first, GLsizei count, GLenum index_type, const void *indices);

void zgl_draw(GLenum mode, GLint first, GLsizei count, GLenum index_type, const void *indices)
{
    struct zgl_program *prog = zgl->program ? zgl->program : zgl_ffp_program();
    if (!prog || !prog->linked || count <= 0) return;
    uint32_t pmode = prim_mode(mode);
    if (pmode == 0xFFFFFFFF) { zgl_set_error(GL_INVALID_ENUM); return; }
    int client_arrays = !zgl->program && !zgl->imm_draw;
    if (client_arrays) zgl_ffp_bind_client_arrays(1);
    zgl_draw_inner(prog, pmode, mode, first, count, index_type, indices);
    if (client_arrays) zgl_ffp_bind_client_arrays(0);
}

static void zgl_draw_inner(struct zgl_program *prog, uint32_t pmode, GLenum mode, GLint first, GLsizei count, GLenum index_type, const void *indices)
{

    zgl_apply_framebuffer();
    apply_state_objects();
    virgl_bind_shader(zgl->v, prog->h_vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(zgl->v, prog->h_fs, PIPE_SHADER_FRAGMENT);

    /* the indices: client arrays go through scratch; quads and friends that
     * core GL can't draw become triangle lists */
    uint32_t *idx32 = NULL;
    size_t nidx = (size_t)count;
    int indexed = index_type != 0;
    uint32_t min_index = 0, max_index = 0;
    uint32_t ib_res = 0, ib_offset = 0, ib_size = 4;
    {
        /* the index list as uint32: from the client array or the element buffer's shadow */
        const void *src = indices;
        if (indexed && zgl->element_buffer) {
            if (!zgl->element_buffer->shadow) { zgl_set_error(GL_INVALID_OPERATION); return; }
            size_t esz = index_type == GL_UNSIGNED_BYTE ? 1 : index_type == GL_UNSIGNED_SHORT ? 2 : 4;
            size_t off = (size_t)(uintptr_t)indices;
            if (off + esz * count > zgl->element_buffer->size) { zgl_set_error(GL_INVALID_OPERATION); return; }
            src = zgl->element_buffer->shadow + off;
        }
        size_t cap = (size_t)count * 3 + 8;
        idx32 = malloc(cap * 4);
        size_t n = 0;
        if (mode == GL_QUADS) {
            for (GLsizei q = 0; q + 3 < count; q += 4) {
                uint32_t a = indexed ? index_at(src, index_type, q) : (uint32_t)(first + q);
                uint32_t b = indexed ? index_at(src, index_type, q + 1) : (uint32_t)(first + q + 1);
                uint32_t c = indexed ? index_at(src, index_type, q + 2) : (uint32_t)(first + q + 2);
                uint32_t d = indexed ? index_at(src, index_type, q + 3) : (uint32_t)(first + q + 3);
                idx32[n++] = a; idx32[n++] = b; idx32[n++] = c; idx32[n++] = a; idx32[n++] = c; idx32[n++] = d;
            }
            pmode = PIPE_PRIM_TRIANGLES;
        } else if (mode == GL_QUAD_STRIP) {
            for (GLsizei q = 0; q + 3 < count; q += 2) {
                uint32_t a = indexed ? index_at(src, index_type, q) : (uint32_t)(first + q);
                uint32_t b = indexed ? index_at(src, index_type, q + 1) : (uint32_t)(first + q + 1);
                uint32_t c = indexed ? index_at(src, index_type, q + 2) : (uint32_t)(first + q + 2);
                uint32_t d = indexed ? index_at(src, index_type, q + 3) : (uint32_t)(first + q + 3);
                idx32[n++] = a; idx32[n++] = b; idx32[n++] = d; idx32[n++] = a; idx32[n++] = d; idx32[n++] = c;
            }
            pmode = PIPE_PRIM_TRIANGLES;
        } else if (mode == GL_POLYGON) {
            for (GLsizei i = 0; i < count; i++) idx32[n++] = indexed ? index_at(src, index_type, i) : (uint32_t)(first + i);
            pmode = PIPE_PRIM_TRIANGLE_FAN;
        } else if (mode == GL_LINE_LOOP) {
            for (GLsizei i = 0; i < count; i++) idx32[n++] = indexed ? index_at(src, index_type, i) : (uint32_t)(first + i);
            idx32[n++] = idx32[0];
            pmode = PIPE_PRIM_LINE_STRIP;
        } else if (indexed) {
            for (GLsizei i = 0; i < count; i++) idx32[n++] = index_at(src, index_type, i);
        } else {
            free(idx32); idx32 = NULL;
        }
        if (idx32) {
            nidx = n;
            min_index = 0xFFFFFFFF; max_index = 0;
            for (size_t i = 0; i < n; i++) { if (idx32[i] < min_index) min_index = idx32[i]; if (idx32[i] > max_index) max_index = idx32[i]; }
            long off = iscratch_put(idx32, n * 4);
            if (off < 0) { free(idx32); zgl_set_error(GL_OUT_OF_MEMORY); return; }
            ib_res = zgl->iscratch->id; ib_offset = (uint32_t)off; ib_size = 4;
            indexed = 1;
        }
    }
    /* the range of vertices the attributes must cover */
    uint32_t vfirst = indexed ? (max_index == 0xFFFFFFFF ? 0 : min_index) : (uint32_t)first;
    uint32_t vcount = indexed ? (max_index == 0xFFFFFFFF ? 0 : max_index - min_index + 1) : (uint32_t)count;

    /* vertex elements: VS input i comes from attribute location attrib_loc[i] */
    int nel = prog->vs->c.nattribs;
    uint32_t ve_off[ZGL_MAX_ATTRIBS], ve_buf[ZGL_MAX_ATTRIBS], ve_fmt[ZGL_MAX_ATTRIBS];
    uint32_t vb_stride[ZGL_MAX_ATTRIBS], vb_off[ZGL_MAX_ATTRIBS], vb_res[ZGL_MAX_ATTRIBS];
    uint64_t ve_key = 0;
    int nvb = 0;
    for (int i = 0; i < nel && i < ZGL_MAX_ATTRIBS; i++) {
        int loc = prog->attrib_loc[i];
        struct zgl_attrib *a = loc >= 0 ? &zgl->attribs[loc] : NULL;
        int elem_size = 0;
        if (a && a->enabled && (a->pointer != NULL || a->buffer) && a->size > 0) {
            uint32_t fmt = attrib_format(a->size, a->type, a->normalized, &elem_size);
            int stride = a->stride ? a->stride : elem_size;
            if (a->buffer && a->buffer->bind == GL_ARRAY_BUFFER) {
                if (!a->buffer->res) { zgl_set_error(GL_INVALID_OPERATION); free(idx32); return; }
                ve_off[i] = 0; ve_buf[i] = (uint32_t)nvb; ve_fmt[i] = fmt;
                vb_stride[nvb] = (uint32_t)stride; vb_off[nvb] = (uint32_t)(uintptr_t)a->pointer; vb_res[nvb] = a->buffer->res->id;
                nvb++;
            } else {
                /* client memory: copy the used range (max_index may be unknown for buffer indices: not here) */
                if (vcount == 0 && indexed) { zgl_set_error(GL_INVALID_OPERATION); free(idx32); return; }
                size_t bytes = (size_t)stride * (vfirst + vcount);
                const void *src = a->pointer;
                if (a->buffer) {                /* an element-array buffer used for vertices: copy its shadow */
                    size_t boff = (size_t)(uintptr_t)a->pointer;
                    if (!a->buffer->shadow || boff + bytes > a->buffer->size) { zgl_set_error(GL_INVALID_OPERATION); free(idx32); return; }
                    src = a->buffer->shadow + boff;
                }
                long off;
                if (a->type == GL_DOUBLE) {
                    /* doubles: convert to floats */
                    float *tmp = malloc((size_t)(vfirst + vcount) * a->size * 4);
                    for (uint32_t k = 0; k < vfirst + vcount; k++)
                        for (int j = 0; j < a->size; j++) tmp[k * a->size + j] = (float)((const double *)((const uint8_t *)src + (size_t)k * stride))[j];
                    off = scratch_put(tmp, (size_t)(vfirst + vcount) * a->size * 4);
                    free(tmp);
                    fmt = attrib_format(a->size, GL_FLOAT, 0, &elem_size);
                    stride = elem_size;
                } else {
                    off = scratch_put(src, bytes);
                }
                if (off < 0) { zgl_set_error(GL_OUT_OF_MEMORY); free(idx32); return; }
                ve_off[i] = 0; ve_buf[i] = (uint32_t)nvb; ve_fmt[i] = fmt;
                vb_stride[nvb] = (uint32_t)stride; vb_off[nvb] = (uint32_t)off; vb_res[nvb] = zgl->scratch->id;
                nvb++;
            }
        } else {
            /* a constant attribute: the current value, repeated for every vertex */
            float v[4] = { 0, 0, 0, 1 };
            if (a) memcpy(v, a->value, sizeof v);
            size_t nv = vfirst + vcount;
            float *tmp = malloc(nv * 16 + 16);
            for (size_t k = 0; k < nv; k++) memcpy(tmp + k * 4, v, 16);
            long off = scratch_put(tmp, nv * 16);
            free(tmp);
            if (off < 0) { zgl_set_error(GL_OUT_OF_MEMORY); free(idx32); return; }
            ve_off[i] = 0; ve_buf[i] = (uint32_t)nvb; ve_fmt[i] = VIRGL_FORMAT_R32G32B32A32_FLOAT;
            vb_stride[nvb] = 16; vb_off[nvb] = (uint32_t)off; vb_res[nvb] = zgl->scratch->id;
            nvb++;
        }
        ve_key = ve_key * 1000003u + ve_fmt[i] * 31 + (uint32_t)i;
    }
    free(idx32);
    if (nel > 0) {
        if (ve_key != zgl->ve_key || !zgl->h_ve) {
            if (zgl->h_ve) virgl_destroy_object(zgl->v, zgl->h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
            zgl->h_ve = virgl_handle(zgl->v);
            virgl_create_vertex_elements(zgl->v, zgl->h_ve, (uint32_t)nel, ve_off, ve_buf, ve_fmt);
            virgl_bind_object(zgl->v, zgl->h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
            zgl->ve_key = ve_key;
        }
        virgl_set_vertex_buffers(zgl->v, (uint32_t)nvb, vb_stride, vb_off, vb_res);
    }

    /* constants: the gl_* state, then the program's uniforms */
    zgl_ffp_fill_state(prog);
    if (prog->consts_dirty) {
        if (prog->vs_nconsts) virgl_set_constants(zgl->v, PIPE_SHADER_VERTEX, prog->vs_consts, (uint32_t)prog->vs_nconsts * 4);
        if (prog->fs_nconsts) virgl_set_constants(zgl->v, PIPE_SHADER_FRAGMENT, prog->fs_consts, (uint32_t)prog->fs_nconsts * 4);
        prog->consts_dirty = 0;
    }

    /* textures: SAMP[i] of the fragment shader reads the unit its uniform names */
    int ns = prog->fs->c.nsamplers;
    if (ns > 0) {
        uint32_t views[ZGL_MAX_VARS], samplers[ZGL_MAX_VARS];
        for (int i = 0; i < ns; i++) {
            int unit = 0;
            for (int u = 0; u < prog->nuniforms; u++)
                if (strcmp(prog->uniforms[u].name, prog->fs->c.samplers[i].name) == 0) { unit = prog->uniforms[u].sampler_unit; break; }
            struct zgl_texture *t = unit >= 0 && unit < ZGL_MAX_UNITS ? zgl->unit_tex[unit] : NULL;
            if (t && t->res && t->has_level0) { views[i] = zgl_texture_view(t); samplers[i] = zgl_texture_sampler(t); }
            else { views[i] = 0; samplers[i] = 0; }
        }
        virgl_set_sampler_views(zgl->v, PIPE_SHADER_FRAGMENT, 0, views, (uint32_t)ns);
        virgl_bind_sampler_states(zgl->v, PIPE_SHADER_FRAGMENT, 0, samplers, (uint32_t)ns);
    }

    if (indexed) virgl_set_index_buffer(zgl->v, ib_res, ib_size, ib_offset);
    virgl_draw(zgl->v, pmode, indexed ? 0 : (uint32_t)first, (uint32_t)nidx, indexed, indexed ? min_index : (uint32_t)first,
               indexed ? (max_index == 0xFFFFFFFF ? 0xFFFFFFFF : max_index) : (uint32_t)(first + count - 1));
    zgl->stats_draws++;
}
