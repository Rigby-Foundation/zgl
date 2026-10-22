/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Textures: how GL internal formats are stored on the host, pixels between
 * client memory and those formats, every texture target (1D, 2D, 3D, cube
 * maps, rectangles, arrays, buffers, multisample), their parameters, and
 * sampler objects. */
#include "zgl.h"
#include <GL/zgl_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CTX() do { if (!zgl) return; } while (0)
#define CTXV(v) do { if (!zgl) return (v); } while (0)
#define LIST_ADD(list, n, item) do { (list) = realloc((list), sizeof *(list) * ((n) + 1)); (list)[(n)++] = (item); } while (0)
#define LIST_DEL(list, n, item) do { for (int _i = 0; _i < (n); _i++) if ((list)[_i] == (item)) { memmove(&(list)[_i], &(list)[_i + 1], sizeof *(list) * ((n) - _i - 1)); (n)--; break; } } while (0)

/* ---- formats ---------------------------------------------------------------------- */

/* How a host format lays out a pixel: an array of channels (their order,
 * width and number type), or one of the packed layouts. */
enum { L_ARRAY, L_R11G11B10F, L_RGB9E5, L_RGB10A2, L_RGB10A2UI, L_Z16, L_Z24X8, L_Z24S8, L_S8Z24, L_Z32F, L_Z32FS8, L_S8, L_BLOCK };
enum { N_UNORM, N_SNORM, N_UINT, N_SINT, N_HALF, N_FLOAT };
struct layout {
    uint32_t vformat;
    uint8_t lay, n, bits, num;
    int8_t order[4];                        /* channel i holds RGBA component order[i] */
};

static const struct layout layouts[] = {
#define A(f, n, b, t, o0, o1, o2, o3) { VIRGL_FORMAT_##f, L_ARRAY, n, b, t, { o0, o1, o2, o3 } }
    A(B8G8R8A8_UNORM, 4, 8, N_UNORM, 2, 1, 0, 3), A(B8G8R8A8_SRGB, 4, 8, N_UNORM, 2, 1, 0, 3),
    A(B8G8R8X8_UNORM, 4, 8, N_UNORM, 2, 1, 0, 3), A(R8G8B8A8_UNORM, 4, 8, N_UNORM, 0, 1, 2, 3), A(R8G8B8A8_SRGB, 4, 8, N_UNORM, 0, 1, 2, 3),
    A(R8_UNORM, 1, 8, N_UNORM, 0, -1, -1, -1), A(R8G8_UNORM, 2, 8, N_UNORM, 0, 1, -1, -1),
    A(R16_UNORM, 1, 16, N_UNORM, 0, -1, -1, -1), A(R16G16_UNORM, 2, 16, N_UNORM, 0, 1, -1, -1), A(R16G16B16A16_UNORM, 4, 16, N_UNORM, 0, 1, 2, 3),
    A(R8_SNORM, 1, 8, N_SNORM, 0, -1, -1, -1), A(R8G8_SNORM, 2, 8, N_SNORM, 0, 1, -1, -1), A(R8G8B8A8_SNORM, 4, 8, N_SNORM, 0, 1, 2, 3),
    A(R16_SNORM, 1, 16, N_SNORM, 0, -1, -1, -1), A(R16G16_SNORM, 2, 16, N_SNORM, 0, 1, -1, -1), A(R16G16B16A16_SNORM, 4, 16, N_SNORM, 0, 1, 2, 3),
    A(R16_FLOAT, 1, 16, N_HALF, 0, -1, -1, -1), A(R16G16_FLOAT, 2, 16, N_HALF, 0, 1, -1, -1), A(R16G16B16A16_FLOAT, 4, 16, N_HALF, 0, 1, 2, 3),
    A(R32_FLOAT, 1, 32, N_FLOAT, 0, -1, -1, -1), A(R32G32_FLOAT, 2, 32, N_FLOAT, 0, 1, -1, -1), A(R32G32B32A32_FLOAT, 4, 32, N_FLOAT, 0, 1, 2, 3),
    A(R8_UINT, 1, 8, N_UINT, 0, -1, -1, -1), A(R8G8_UINT, 2, 8, N_UINT, 0, 1, -1, -1), A(R8G8B8A8_UINT, 4, 8, N_UINT, 0, 1, 2, 3),
    A(R8_SINT, 1, 8, N_SINT, 0, -1, -1, -1), A(R8G8_SINT, 2, 8, N_SINT, 0, 1, -1, -1), A(R8G8B8A8_SINT, 4, 8, N_SINT, 0, 1, 2, 3),
    A(R16_UINT, 1, 16, N_UINT, 0, -1, -1, -1), A(R16G16_UINT, 2, 16, N_UINT, 0, 1, -1, -1), A(R16G16B16A16_UINT, 4, 16, N_UINT, 0, 1, 2, 3),
    A(R16_SINT, 1, 16, N_SINT, 0, -1, -1, -1), A(R16G16_SINT, 2, 16, N_SINT, 0, 1, -1, -1), A(R16G16B16A16_SINT, 4, 16, N_SINT, 0, 1, 2, 3),
    A(R32_UINT, 1, 32, N_UINT, 0, -1, -1, -1), A(R32G32_UINT, 2, 32, N_UINT, 0, 1, -1, -1), A(R32G32B32A32_UINT, 4, 32, N_UINT, 0, 1, 2, 3),
    A(R32_SINT, 1, 32, N_SINT, 0, -1, -1, -1), A(R32G32_SINT, 2, 32, N_SINT, 0, 1, -1, -1), A(R32G32B32A32_SINT, 4, 32, N_SINT, 0, 1, 2, 3),
#undef A
    { VIRGL_FORMAT_R11G11B10_FLOAT, L_R11G11B10F, 3, 32, N_FLOAT, { 0, 1, 2, -1 } },
    { VIRGL_FORMAT_R9G9B9E5_FLOAT, L_RGB9E5, 3, 32, N_FLOAT, { 0, 1, 2, -1 } },
    { VIRGL_FORMAT_R10G10B10A2_UNORM, L_RGB10A2, 4, 32, N_UNORM, { 0, 1, 2, 3 } },
    { VIRGL_FORMAT_R10G10B10A2_UINT, L_RGB10A2UI, 4, 32, N_UINT, { 0, 1, 2, 3 } },
    { VIRGL_FORMAT_Z16_UNORM, L_Z16, 1, 16, N_UNORM, { 0, -1, -1, -1 } },
    { VIRGL_FORMAT_Z24X8_UNORM, L_Z24X8, 1, 32, N_UNORM, { 0, -1, -1, -1 } },
    { VIRGL_FORMAT_Z24_UNORM_S8_UINT, L_Z24S8, 2, 32, N_UNORM, { 0, 1, -1, -1 } },
    { VIRGL_FORMAT_S8_UINT_Z24_UNORM, L_S8Z24, 2, 32, N_UNORM, { 0, 1, -1, -1 } },
    { VIRGL_FORMAT_Z32_FLOAT, L_Z32F, 1, 32, N_FLOAT, { 0, -1, -1, -1 } },
    { VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT, L_Z32FS8, 2, 64, N_FLOAT, { 0, 1, -1, -1 } },
    { VIRGL_FORMAT_S8_UINT, L_S8, 1, 8, N_UINT, { 1, -1, -1, -1 } },
};

static const struct layout *layout_of(uint32_t vformat)
{
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) if (layouts[i].vformat == vformat) return &layouts[i];
    return NULL;
}

static int caps_has(const uint32_t *bitmask, uint32_t f) { return (bitmask[f / 32] >> (f % 32)) & 1; }
static int can_sample(uint32_t f) { const struct virgl_caps_v2 *c = virgl_caps(zgl->v); return !c || caps_has(c->v1.sampler.bitmask, f); }
static int can_render(uint32_t f) { const struct virgl_caps_v2 *c = virgl_caps(zgl->v); return !c || caps_has(c->v1.render.bitmask, f); }
/* hosts list depth formats in the depth-stencil set, or (virglrenderer on macOS) only among the sampled ones */
static int can_depth(uint32_t f) { const struct virgl_caps_v2 *c = virgl_caps(zgl->v); return !c || caps_has(c->v1.depthstencil.bitmask, f) || caps_has(c->v1.sampler.bitmask, f); }

/* The sized internal formats. RGB formats are kept in 4-channel host
 * formats (3-channel ones are rarely renderable) with alpha read as 1. */
static const struct zgl_format formats[] = {
#define F(i, f, bpp, c, k) { i, VIRGL_FORMAT_##f, bpp, c, k, 1 }
    F(GL_RGBA8, B8G8R8A8_UNORM, 4, 4, ZF_UNORM), F(GL_RGB8, B8G8R8A8_UNORM, 4, 3, ZF_UNORM),
    F(GL_RGBA4, B8G8R8A8_UNORM, 4, 4, ZF_UNORM), F(GL_RGB5_A1, B8G8R8A8_UNORM, 4, 4, ZF_UNORM), F(GL_RGB565, B8G8R8A8_UNORM, 4, 3, ZF_UNORM),
    F(GL_RGB5, B8G8R8A8_UNORM, 4, 3, ZF_UNORM), F(GL_RGB4, B8G8R8A8_UNORM, 4, 3, ZF_UNORM), F(GL_R3_G3_B2, B8G8R8A8_UNORM, 4, 3, ZF_UNORM),
    F(GL_RGBA2, B8G8R8A8_UNORM, 4, 4, ZF_UNORM), F(GL_RGB10, R16G16B16A16_UNORM, 8, 3, ZF_UNORM), F(GL_RGB12, R16G16B16A16_UNORM, 8, 3, ZF_UNORM),
    F(GL_RGBA12, R16G16B16A16_UNORM, 8, 4, ZF_UNORM),
    F(GL_SRGB8_ALPHA8, B8G8R8A8_SRGB, 4, 4, ZF_SRGB), F(GL_SRGB8, B8G8R8A8_SRGB, 4, 3, ZF_SRGB),
    F(GL_R8, R8_UNORM, 1, 1, ZF_UNORM), F(GL_RG8, R8G8_UNORM, 2, 2, ZF_UNORM),
    F(GL_R16, R16_UNORM, 2, 1, ZF_UNORM), F(GL_RG16, R16G16_UNORM, 4, 2, ZF_UNORM), F(GL_RGBA16, R16G16B16A16_UNORM, 8, 4, ZF_UNORM),
    F(GL_RGB16, R16G16B16A16_UNORM, 8, 3, ZF_UNORM),
    F(GL_R8_SNORM, R8_SNORM, 1, 1, ZF_SNORM), F(GL_RG8_SNORM, R8G8_SNORM, 2, 2, ZF_SNORM), F(GL_RGBA8_SNORM, R8G8B8A8_SNORM, 4, 4, ZF_SNORM),
    F(GL_RGB8_SNORM, R8G8B8A8_SNORM, 4, 3, ZF_SNORM),
    F(GL_R16_SNORM, R16_SNORM, 2, 1, ZF_SNORM), F(GL_RG16_SNORM, R16G16_SNORM, 4, 2, ZF_SNORM), F(GL_RGBA16_SNORM, R16G16B16A16_SNORM, 8, 4, ZF_SNORM),
    F(GL_RGB16_SNORM, R16G16B16A16_SNORM, 8, 3, ZF_SNORM),
    F(GL_R16F, R16_FLOAT, 2, 1, ZF_FLOAT), F(GL_RG16F, R16G16_FLOAT, 4, 2, ZF_FLOAT), F(GL_RGBA16F, R16G16B16A16_FLOAT, 8, 4, ZF_FLOAT),
    F(GL_RGB16F, R16G16B16A16_FLOAT, 8, 3, ZF_FLOAT),
    F(GL_R32F, R32_FLOAT, 4, 1, ZF_FLOAT), F(GL_RG32F, R32G32_FLOAT, 8, 2, ZF_FLOAT), F(GL_RGBA32F, R32G32B32A32_FLOAT, 16, 4, ZF_FLOAT),
    F(GL_RGB32F, R32G32B32A32_FLOAT, 16, 3, ZF_FLOAT),
    F(GL_R11F_G11F_B10F, R11G11B10_FLOAT, 4, 3, ZF_FLOAT), F(GL_RGB9_E5, R9G9B9E5_FLOAT, 4, 3, ZF_FLOAT),
    F(GL_RGB10_A2, R10G10B10A2_UNORM, 4, 4, ZF_UNORM), F(GL_RGB10_A2UI, R10G10B10A2_UINT, 4, 4, ZF_UINT),
    F(GL_R8UI, R8_UINT, 1, 1, ZF_UINT), F(GL_RG8UI, R8G8_UINT, 2, 2, ZF_UINT), F(GL_RGBA8UI, R8G8B8A8_UINT, 4, 4, ZF_UINT), F(GL_RGB8UI, R8G8B8A8_UINT, 4, 3, ZF_UINT),
    F(GL_R8I, R8_SINT, 1, 1, ZF_INT), F(GL_RG8I, R8G8_SINT, 2, 2, ZF_INT), F(GL_RGBA8I, R8G8B8A8_SINT, 4, 4, ZF_INT), F(GL_RGB8I, R8G8B8A8_SINT, 4, 3, ZF_INT),
    F(GL_R16UI, R16_UINT, 2, 1, ZF_UINT), F(GL_RG16UI, R16G16_UINT, 4, 2, ZF_UINT), F(GL_RGBA16UI, R16G16B16A16_UINT, 8, 4, ZF_UINT), F(GL_RGB16UI, R16G16B16A16_UINT, 8, 3, ZF_UINT),
    F(GL_R16I, R16_SINT, 2, 1, ZF_INT), F(GL_RG16I, R16G16_SINT, 4, 2, ZF_INT), F(GL_RGBA16I, R16G16B16A16_SINT, 8, 4, ZF_INT), F(GL_RGB16I, R16G16B16A16_SINT, 8, 3, ZF_INT),
    F(GL_R32UI, R32_UINT, 4, 1, ZF_UINT), F(GL_RG32UI, R32G32_UINT, 8, 2, ZF_UINT), F(GL_RGBA32UI, R32G32B32A32_UINT, 16, 4, ZF_UINT), F(GL_RGB32UI, R32G32B32A32_UINT, 16, 3, ZF_UINT),
    F(GL_R32I, R32_SINT, 4, 1, ZF_INT), F(GL_RG32I, R32G32_SINT, 8, 2, ZF_INT), F(GL_RGBA32I, R32G32B32A32_SINT, 16, 4, ZF_INT), F(GL_RGB32I, R32G32B32A32_SINT, 16, 3, ZF_INT),
    F(GL_DEPTH_COMPONENT16, Z16_UNORM, 2, 1, ZF_DEPTH), F(GL_DEPTH_COMPONENT24, Z24X8_UNORM, 4, 1, ZF_DEPTH),
    F(GL_DEPTH_COMPONENT32, Z24X8_UNORM, 4, 1, ZF_DEPTH), F(GL_DEPTH_COMPONENT32F, Z32_FLOAT, 4, 1, ZF_DEPTH),
    F(GL_DEPTH24_STENCIL8, Z24_UNORM_S8_UINT, 4, 2, ZF_DEPTH_STENCIL), F(GL_DEPTH32F_STENCIL8, Z32_FLOAT_S8X24_UINT, 8, 2, ZF_DEPTH_STENCIL),
    F(GL_STENCIL_INDEX8, S8_UINT, 1, 1, ZF_STENCIL),
#undef F
#define C(i, f, bytes, c) { i, VIRGL_FORMAT_##f, bytes, c, ZF_COMPRESSED, 4 }
    C(GL_COMPRESSED_RED_RGTC1, RGTC1_UNORM, 8, 1), C(GL_COMPRESSED_SIGNED_RED_RGTC1, RGTC1_SNORM, 8, 1),
    C(GL_COMPRESSED_RG_RGTC2, RGTC2_UNORM, 16, 2), C(GL_COMPRESSED_SIGNED_RG_RGTC2, RGTC2_SNORM, 16, 2),
    C(GL_COMPRESSED_RGB_S3TC_DXT1_EXT, DXT1_RGB, 8, 3), C(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, DXT1_RGBA, 8, 4),
    C(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, DXT3_RGBA, 16, 4), C(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, DXT5_RGBA, 16, 4),
#undef C
};

/* the depth formats the host has, picked once: some (macOS) lack packed 24+8 */
static struct zgl_format depth_fmts[4];
static int depth_fmts_ready;

static const struct zgl_format *find_format(GLenum internal)
{
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) if (formats[i].internal == internal) return &formats[i];
    return NULL;
}

static const struct zgl_format *depth_format(GLenum internal)
{
    if (!depth_fmts_ready) {
        static const uint32_t ds_try[] = { VIRGL_FORMAT_Z24_UNORM_S8_UINT, VIRGL_FORMAT_S8_UINT_Z24_UNORM, VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT };
        static const uint32_t d24_try[] = { VIRGL_FORMAT_Z24X8_UNORM, VIRGL_FORMAT_Z24_UNORM_S8_UINT, VIRGL_FORMAT_Z32_FLOAT };
        depth_fmts[0] = *find_format(GL_DEPTH24_STENCIL8);
        for (size_t i = 0; i < 3; i++) if (can_depth(ds_try[i])) { depth_fmts[0].vformat = ds_try[i]; depth_fmts[0].bpp = ds_try[i] == VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT ? 8 : 4; break; }
        depth_fmts[1] = *find_format(GL_DEPTH_COMPONENT24);
        for (size_t i = 0; i < 3; i++) if (can_depth(d24_try[i])) { depth_fmts[1].vformat = d24_try[i]; break; }
        depth_fmts[2] = *find_format(GL_DEPTH_COMPONENT32);
        depth_fmts[2].vformat = depth_fmts[1].vformat;
        depth_fmts[3] = *find_format(GL_DEPTH_COMPONENT16);
        if (!can_depth(VIRGL_FORMAT_Z16_UNORM)) depth_fmts[3].vformat = depth_fmts[1].vformat, depth_fmts[3].bpp = 4;
        depth_fmts_ready = 1;
    }
    switch (internal) {
    case GL_DEPTH24_STENCIL8: return &depth_fmts[0];
    case GL_DEPTH_COMPONENT24: return &depth_fmts[1];
    case GL_DEPTH_COMPONENT32: return &depth_fmts[2];
    case GL_DEPTH_COMPONENT16: return &depth_fmts[3];
    }
    return find_format(internal);
}

/* the unsized and legacy internal formats: what they become */
static GLenum sized_internal(GLenum internal, GLenum format, GLenum type)
{
    (void)format;
    switch (internal) {
    case 4: case GL_RGBA: case GL_COMPRESSED_RGBA: return type == GL_FLOAT ? GL_RGBA8 : GL_RGBA8;
    case 3: case GL_RGB: case GL_COMPRESSED_RGB: return GL_RGB8;
    case GL_BGRA: return GL_RGBA8;                 /* not legal GL, used anyway (GLES habit) */
    case GL_RED: case GL_COMPRESSED_RED: return GL_R8;
    case GL_RG: case GL_COMPRESSED_RG: return GL_RG8;
    case GL_SRGB: case GL_COMPRESSED_SRGB: return GL_SRGB8;
    case GL_SRGB_ALPHA: case GL_COMPRESSED_SRGB_ALPHA: return GL_SRGB8_ALPHA8;
    case GL_DEPTH_COMPONENT: return type == GL_FLOAT ? GL_DEPTH_COMPONENT32F : type == GL_UNSIGNED_SHORT ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24;
    case GL_DEPTH_STENCIL: return type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    /* the compatibility profile's luminance and alpha formats: RGBA, expanded on upload */
    case 1: case 2: case GL_LUMINANCE: case GL_LUMINANCE_ALPHA: case GL_ALPHA: case GL_INTENSITY:
    case GL_LUMINANCE8: case GL_LUMINANCE8_ALPHA8: case GL_ALPHA8: case GL_INTENSITY8: return GL_RGBA8;
    }
    return internal;
}

/* whether the host can multisample a format: virglrenderer lists them (with
 * VIRGL_FORMAT_NONE set when the list is there at all). Asking for one it
 * can't fails on the host without telling the guest, and the first surface
 * on it then breaks the context for good, so this is checked first. */
int zgl_can_multisample(uint32_t vformat)
{
    const struct virgl_caps_v2 *c = virgl_caps(zgl->v);
    if (!c || !caps_has(c->supported_multisample_formats.bitmask, VIRGL_FORMAT_NONE)) return 1;
    return caps_has(c->supported_multisample_formats.bitmask, vformat);
}

/* the format to keep multisampled images in: the same, except BGRA, which
 * hosts can't multisample (macOS GL): RGBA instead (blits convert) */
const struct zgl_format *zgl_format_ms(const struct zgl_format *f)
{
    static struct zgl_format rgba8, srgb8a8, rgb8;
    if (f->vformat == VIRGL_FORMAT_B8G8R8A8_UNORM) {
        struct zgl_format *m = f->comps == 3 ? &rgb8 : &rgba8;
        *m = *f; m->vformat = VIRGL_FORMAT_R8G8B8A8_UNORM;
        return m;
    }
    if (f->vformat == VIRGL_FORMAT_B8G8R8A8_SRGB) { srgb8a8 = *f; srgb8a8.vformat = VIRGL_FORMAT_R8G8B8A8_SRGB; return &srgb8a8; }
    return f;
}

const struct zgl_format *zgl_format_of(GLenum internal, GLenum format, GLenum type)
{
    const struct zgl_format *f = depth_format(sized_internal(internal, format, type));
    if (!f) return NULL;
    /* a format the host can't sample: the nearest one it can */
    if (f->kind != ZF_DEPTH && f->kind != ZF_DEPTH_STENCIL && f->kind != ZF_STENCIL && f->kind != ZF_COMPRESSED && !can_sample(f->vformat)) {
        if (f->comps <= 2 && f->kind == ZF_UNORM) return find_format(GL_RGBA8);
        return f;                                   /* let the host say no */
    }
    return f;
}

/* ---- pixels: client memory <-> host formats ----------------------------------------- */

/* the client side of a transfer: which components a format names, in memory order */
static int client_comps(GLenum format, int8_t order[4], int *integer)
{
    *integer = 0;
    switch (format) {
    case GL_RED_INTEGER: *integer = 1; /* fall through */
    case GL_RED: order[0] = 0; return 1;
    case GL_GREEN_INTEGER: *integer = 1; /* fall through */
    case GL_GREEN: order[0] = 1; return 1;
    case GL_BLUE_INTEGER: *integer = 1; /* fall through */
    case GL_BLUE: order[0] = 2; return 1;
    case GL_ALPHA_INTEGER: *integer = 1; /* fall through */
    case GL_ALPHA: order[0] = 3; return 1;
    case GL_RG_INTEGER: *integer = 1; /* fall through */
    case GL_RG: order[0] = 0; order[1] = 1; return 2;
    case GL_RGB_INTEGER: *integer = 1; /* fall through */
    case GL_RGB: order[0] = 0; order[1] = 1; order[2] = 2; return 3;
    case GL_BGR_INTEGER: *integer = 1; /* fall through */
    case GL_BGR: order[0] = 2; order[1] = 1; order[2] = 0; return 3;
    case GL_RGBA_INTEGER: *integer = 1; /* fall through */
    case GL_RGBA: order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 3; return 4;
    case GL_BGRA_INTEGER: *integer = 1; /* fall through */
    case GL_BGRA: order[0] = 2; order[1] = 1; order[2] = 0; order[3] = 3; return 4;
    case GL_LUMINANCE: order[0] = 4; return 1;                          /* 4: luminance (R = G = B) */
    case GL_LUMINANCE_ALPHA: order[0] = 4; order[1] = 3; return 2;
    case GL_INTENSITY: order[0] = 5; return 1;                          /* 5: intensity (all four) */
    case GL_DEPTH_COMPONENT: order[0] = 0; return 1;
    case GL_STENCIL_INDEX: order[0] = 1; return 1;
    case GL_DEPTH_STENCIL: order[0] = 0; order[1] = 1; return 2;
    }
    return 0;
}

static int type_size(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
    default: return 0;
    }
}

/* packed types: one element per pixel of this size */
static int packed_size(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 0;
    }
}

static int client_pixel_size(GLenum format, GLenum type)
{
    int8_t o[4]; int integer;
    int n = client_comps(format, o, &integer);
    if (!n) return 0;
    int p = packed_size(type);
    if (p) return p;
    int s = type_size(type);
    return s ? s * n : 0;
}

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (e == 0) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | e << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7F800000u | m << 13;
    else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}

static uint16_t float_to_half(float f)
{
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 255) - 112; uint32_t m = u & 0x7FFFFF;
    if (((u >> 23) & 255) == 255) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    if (e <= 0) { if (e < -10) return (uint16_t)s; m |= 0x800000; uint32_t t = 14 - (uint32_t)e; uint32_t r = m >> t; if ((m >> (t - 1)) & 1) r++; return (uint16_t)(s | r); }
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    uint32_t r = s | (uint32_t)e << 10 | m >> 13;
    if (m & 0x1000) r++;                            /* round to nearest */
    return (uint16_t)r;
}

static float uf11(uint32_t v, int mbits)          /* the unsigned small floats of R11G11B10 */
{
    int e = (int)(v >> mbits) & 31; uint32_t m = v & ((1u << mbits) - 1);
    if (e == 31) return m ? NAN : INFINITY;
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), e - 15);
}
static uint32_t to_uf(float f, int mbits)
{
    if (!(f > 0)) return 0;
    if (isinf(f)) return 31u << mbits;
    int e; float m = frexpf(f, &e);               /* f = m * 2^e, m in [0.5, 1) */
    e += 14;                                        /* biased exponent of 1.m form: (e - 1) + 15 */
    if (e >= 31) return (30u << mbits) | ((1u << mbits) - 1);
    if (e <= 0) return (uint32_t)lrintf(ldexpf(f, 14 + mbits)) & ((1u << mbits) - 1);
    uint32_t mm = (uint32_t)lrintf((m * 2.0f - 1.0f) * (float)(1u << mbits));
    if (mm >> mbits) { mm = 0; e++; if (e >= 31) return (30u << mbits) | ((1u << mbits) - 1); }
    return (uint32_t)e << mbits | mm;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* read one client element of a plain type as a double: normalized types to [0,1] / [-1,1] unless integer */
static double read_elem(const uint8_t *p, GLenum type, int norm)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: return norm ? p[0] / 255.0 : p[0];
    case GL_BYTE: { int8_t v = (int8_t)p[0]; return norm ? fmax(v / 127.0, -1.0) : v; }
    case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, p, 2); return norm ? v / 65535.0 : v; }
    case GL_SHORT: { int16_t v; memcpy(&v, p, 2); return norm ? fmax(v / 32767.0, -1.0) : v; }
    case GL_UNSIGNED_INT: { uint32_t v; memcpy(&v, p, 4); return norm ? v / 4294967295.0 : v; }
    case GL_INT: { int32_t v; memcpy(&v, p, 4); return norm ? fmax(v / 2147483647.0, -1.0) : v; }
    case GL_HALF_FLOAT: { uint16_t v; memcpy(&v, p, 2); return half_to_float(v); }
    case GL_FLOAT: { float v; memcpy(&v, p, 4); return v; }
    }
    return 0;
}

static void write_elem(uint8_t *p, GLenum type, int norm, double x)
{
    switch (type) {
    case GL_UNSIGNED_BYTE: p[0] = (uint8_t)(norm ? lrint(clampf((float)x, 0, 1) * 255.0) : x < 0 ? 0 : x > 255 ? 255 : x); break;
    case GL_BYTE: p[0] = (uint8_t)(int8_t)(norm ? lrint(clampf((float)x, -1, 1) * 127.0) : x < -128 ? -128 : x > 127 ? 127 : x); break;
    case GL_UNSIGNED_SHORT: { uint16_t v = (uint16_t)(norm ? lrint(clampf((float)x, 0, 1) * 65535.0) : x < 0 ? 0 : x > 65535 ? 65535 : x); memcpy(p, &v, 2); break; }
    case GL_SHORT: { int16_t v = (int16_t)(norm ? lrint(clampf((float)x, -1, 1) * 32767.0) : x < -32768 ? -32768 : x > 32767 ? 32767 : x); memcpy(p, &v, 2); break; }
    case GL_UNSIGNED_INT: { uint32_t v = (uint32_t)(norm ? llrint(fmin(fmax(x, 0), 1) * 4294967295.0) : x < 0 ? 0 : x > 4294967295.0 ? 4294967295u : (uint32_t)x); memcpy(p, &v, 4); break; }
    case GL_INT: { int32_t v = (int32_t)(norm ? llrint(fmin(fmax(x, -1), 1) * 2147483647.0) : x < -2147483648.0 ? INT32_MIN : x > 2147483647.0 ? INT32_MAX : (int32_t)x); memcpy(p, &v, 4); break; }
    case GL_HALF_FLOAT: { uint16_t v = float_to_half((float)x); memcpy(p, &v, 2); break; }
    case GL_FLOAT: { float v = (float)x; memcpy(p, &v, 4); break; }
    }
}

/* a pixel in the middle: RGBA (or depth, stencil in [0], [1]) as doubles */
typedef struct { double c[4]; } pix;

static void client_read(const uint8_t *p, GLenum format, GLenum type, pix *out)
{
    int8_t o[4] = { -1, -1, -1, -1 }; int integer;
    int n = client_comps(format, o, &integer);
    int depthy = format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
    out->c[0] = out->c[1] = out->c[2] = 0; out->c[3] = integer ? 1 : 1.0;
    double v[4] = { 0, 0, 0, 0 };
    uint32_t w = 0;
    int ps = packed_size(type);
    if (ps == 1) w = p[0]; else if (ps == 2) { uint16_t s; memcpy(&s, p, 2); w = s; } else if (ps == 4) memcpy(&w, p, 4);
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: v[0] = (w >> 5) / 7.0; v[1] = ((w >> 2) & 7) / 7.0; v[2] = (w & 3) / 3.0; break;
    case GL_UNSIGNED_BYTE_2_3_3_REV: v[0] = (w & 7) / 7.0; v[1] = ((w >> 3) & 7) / 7.0; v[2] = (w >> 6) / 3.0; break;
    case GL_UNSIGNED_SHORT_5_6_5: v[0] = (w >> 11) / 31.0; v[1] = ((w >> 5) & 63) / 63.0; v[2] = (w & 31) / 31.0; break;
    case GL_UNSIGNED_SHORT_5_6_5_REV: v[0] = (w & 31) / 31.0; v[1] = ((w >> 5) & 63) / 63.0; v[2] = (w >> 11) / 31.0; break;
    case GL_UNSIGNED_SHORT_4_4_4_4: v[0] = (w >> 12) / 15.0; v[1] = ((w >> 8) & 15) / 15.0; v[2] = ((w >> 4) & 15) / 15.0; v[3] = (w & 15) / 15.0; break;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: v[0] = (w & 15) / 15.0; v[1] = ((w >> 4) & 15) / 15.0; v[2] = ((w >> 8) & 15) / 15.0; v[3] = (w >> 12) / 15.0; break;
    case GL_UNSIGNED_SHORT_5_5_5_1: v[0] = (w >> 11) / 31.0; v[1] = ((w >> 6) & 31) / 31.0; v[2] = ((w >> 1) & 31) / 31.0; v[3] = w & 1; break;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: v[0] = (w & 31) / 31.0; v[1] = ((w >> 5) & 31) / 31.0; v[2] = ((w >> 10) & 31) / 31.0; v[3] = w >> 15; break;
    case GL_UNSIGNED_INT_8_8_8_8: v[0] = (w >> 24) / 255.0; v[1] = ((w >> 16) & 255) / 255.0; v[2] = ((w >> 8) & 255) / 255.0; v[3] = (w & 255) / 255.0; break;
    case GL_UNSIGNED_INT_8_8_8_8_REV: v[0] = (w & 255) / 255.0; v[1] = ((w >> 8) & 255) / 255.0; v[2] = ((w >> 16) & 255) / 255.0; v[3] = (w >> 24) / 255.0; break;
    case GL_UNSIGNED_INT_10_10_10_2: v[0] = (w >> 22) / 1023.0; v[1] = ((w >> 12) & 1023) / 1023.0; v[2] = ((w >> 2) & 1023) / 1023.0; v[3] = (w & 3) / 3.0; break;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        if (integer) { v[0] = w & 1023; v[1] = (w >> 10) & 1023; v[2] = (w >> 20) & 1023; v[3] = w >> 30; }
        else { v[0] = (w & 1023) / 1023.0; v[1] = ((w >> 10) & 1023) / 1023.0; v[2] = ((w >> 20) & 1023) / 1023.0; v[3] = (w >> 30) / 3.0; }
        break;
    case GL_UNSIGNED_INT_10F_11F_11F_REV: out->c[0] = uf11(w & 2047, 6); out->c[1] = uf11((w >> 11) & 2047, 6); out->c[2] = uf11(w >> 22, 5); return;
    case GL_UNSIGNED_INT_5_9_9_9_REV: {
        double sc = ldexp(1.0, (int)(w >> 27) - 15 - 9);
        out->c[0] = (w & 511) * sc; out->c[1] = ((w >> 9) & 511) * sc; out->c[2] = ((w >> 18) & 511) * sc; return;
    }
    case GL_UNSIGNED_INT_24_8: out->c[0] = (w >> 8) / 16777215.0; out->c[1] = w & 255; return;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: { float d; memcpy(&d, p, 4); uint32_t s; memcpy(&s, p + 4, 4); out->c[0] = d; out->c[1] = s & 255; return; }
    default: {
        int sz = type_size(type);
        for (int i = 0; i < n; i++) {
            int norm = !integer && !(depthy && o[i] == 1);
            v[i] = read_elem(p + i * sz, type, norm);
        }
        /* the components are in the format's order: put them in place */
        double r[4] = { 0, 0, 0, integer ? 1 : 1.0 };
        for (int i = 0; i < n; i++) {
            if (o[i] == 4) r[0] = r[1] = r[2] = v[i];
            else if (o[i] == 5) r[0] = r[1] = r[2] = r[3] = v[i];
            else if (o[i] >= 0) r[o[i]] = v[i];
        }
        memcpy(out->c, r, sizeof r);
        return;
    }
    }
    /* packed colour types name the components in RGBA order; BGRA swaps R and B */
    if (format == GL_BGRA || format == GL_BGR) { double t = v[0]; v[0] = v[2]; v[2] = t; }
    if (n < 4 && type != GL_UNSIGNED_INT_2_10_10_10_REV) v[3] = 1.0;
    memcpy(out->c, v, sizeof v);
}

static void client_write(uint8_t *p, GLenum format, GLenum type, const pix *in)
{
    int8_t o[4] = { -1, -1, -1, -1 }; int integer;
    int n = client_comps(format, o, &integer);
    int depthy = format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
    double v[4];
    memcpy(v, in->c, sizeof v);
    int ps = packed_size(type);
    if (ps) {
        double r = v[0], g = v[1], b = v[2], a = v[3];
        if (format == GL_BGRA || format == GL_BGR) { r = v[2]; b = v[0]; }
#define Q(x, m) ((uint32_t)lrint(clampf((float)(x), 0, 1) * (m)))
        uint32_t w = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE_3_3_2: w = Q(r, 7) << 5 | Q(g, 7) << 2 | Q(b, 3); break;
        case GL_UNSIGNED_BYTE_2_3_3_REV: w = Q(r, 7) | Q(g, 7) << 3 | Q(b, 3) << 6; break;
        case GL_UNSIGNED_SHORT_5_6_5: w = Q(r, 31) << 11 | Q(g, 63) << 5 | Q(b, 31); break;
        case GL_UNSIGNED_SHORT_5_6_5_REV: w = Q(r, 31) | Q(g, 63) << 5 | Q(b, 31) << 11; break;
        case GL_UNSIGNED_SHORT_4_4_4_4: w = Q(r, 15) << 12 | Q(g, 15) << 8 | Q(b, 15) << 4 | Q(a, 15); break;
        case GL_UNSIGNED_SHORT_4_4_4_4_REV: w = Q(r, 15) | Q(g, 15) << 4 | Q(b, 15) << 8 | Q(a, 15) << 12; break;
        case GL_UNSIGNED_SHORT_5_5_5_1: w = Q(r, 31) << 11 | Q(g, 31) << 6 | Q(b, 31) << 1 | Q(a, 1); break;
        case GL_UNSIGNED_SHORT_1_5_5_5_REV: w = Q(r, 31) | Q(g, 31) << 5 | Q(b, 31) << 10 | Q(a, 1) << 15; break;
        case GL_UNSIGNED_INT_8_8_8_8: w = Q(r, 255) << 24 | Q(g, 255) << 16 | Q(b, 255) << 8 | Q(a, 255); break;
        case GL_UNSIGNED_INT_8_8_8_8_REV: w = Q(r, 255) | Q(g, 255) << 8 | Q(b, 255) << 16 | Q(a, 255) << 24; break;
        case GL_UNSIGNED_INT_10_10_10_2: w = Q(r, 1023) << 22 | Q(g, 1023) << 12 | Q(b, 1023) << 2 | Q(a, 3); break;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            if (integer) w = ((uint32_t)r & 1023) | ((uint32_t)g & 1023) << 10 | ((uint32_t)b & 1023) << 20 | ((uint32_t)a & 3) << 30;
            else w = Q(r, 1023) | Q(g, 1023) << 10 | Q(b, 1023) << 20 | Q(a, 3) << 30;
            break;
        case GL_UNSIGNED_INT_10F_11F_11F_REV: w = to_uf((float)v[0], 6) | to_uf((float)v[1], 6) << 11 | to_uf((float)v[2], 5) << 22; break;
        case GL_UNSIGNED_INT_5_9_9_9_REV: {
            double mx = fmax(fmax(v[0], v[1]), v[2]);
            int e = mx > 0 ? (int)floor(log2(mx)) + 1 + 15 : 0; if (e < 0) e = 0; if (e > 31) e = 31;
            double sc = ldexp(1.0, e - 15 - 9);
            uint32_t R = (uint32_t)lrint(fmin(fmax(v[0], 0) / sc, 511)), G = (uint32_t)lrint(fmin(fmax(v[1], 0) / sc, 511)), B = (uint32_t)lrint(fmin(fmax(v[2], 0) / sc, 511));
            w = R | G << 9 | B << 18 | (uint32_t)e << 27; break;
        }
        case GL_UNSIGNED_INT_24_8: w = (uint32_t)lrint(clampf((float)v[0], 0, 1) * 16777215.0) << 8 | ((uint32_t)v[1] & 255); break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: { float d = (float)v[0]; uint32_t s = (uint32_t)v[1] & 255; memcpy(p, &d, 4); memcpy(p + 4, &s, 4); return; }
        }
#undef Q
        if (ps == 1) p[0] = (uint8_t)w; else if (ps == 2) { uint16_t s = (uint16_t)w; memcpy(p, &s, 2); } else memcpy(p, &w, 4);
        return;
    }
    int sz = type_size(type);
    for (int i = 0; i < n; i++) {
        double x = o[i] == 4 ? v[0] : o[i] == 5 ? v[0] : o[i] >= 0 ? v[o[i]] : 0;
        if (o[i] == 4 && format == GL_LUMINANCE) x = v[0];          /* GL's luminance readback is R (no sum) */
        int norm = !integer && !(depthy && o[i] == 1);
        write_elem(p + i * sz, type, norm, x);
    }
}

/* host format pixels */
static void host_read(const uint8_t *p, const struct layout *l, pix *out)
{
    out->c[0] = out->c[1] = out->c[2] = 0; out->c[3] = 1;
    uint32_t w; memcpy(&w, p, 4);
    switch (l->lay) {
    case L_ARRAY:
        for (int i = 0; i < l->n; i++) {
            const uint8_t *q = p + i * (l->bits / 8);
            double x = 0;
            switch (l->num) {
            case N_UNORM: x = l->bits == 8 ? q[0] / 255.0 : l->bits == 16 ? read_elem(q, GL_UNSIGNED_SHORT, 1) : read_elem(q, GL_UNSIGNED_INT, 1); break;
            case N_SNORM: x = l->bits == 8 ? read_elem(q, GL_BYTE, 1) : read_elem(q, GL_SHORT, 1); break;
            case N_UINT: x = l->bits == 8 ? q[0] : l->bits == 16 ? read_elem(q, GL_UNSIGNED_SHORT, 0) : read_elem(q, GL_UNSIGNED_INT, 0); break;
            case N_SINT: x = l->bits == 8 ? read_elem(q, GL_BYTE, 0) : l->bits == 16 ? read_elem(q, GL_SHORT, 0) : read_elem(q, GL_INT, 0); break;
            case N_HALF: x = read_elem(q, GL_HALF_FLOAT, 0); break;
            case N_FLOAT: x = read_elem(q, GL_FLOAT, 0); break;
            }
            out->c[l->order[i]] = x;
        }
        break;
    case L_R11G11B10F: out->c[0] = uf11(w & 2047, 6); out->c[1] = uf11((w >> 11) & 2047, 6); out->c[2] = uf11(w >> 22, 5); break;
    case L_RGB9E5: { double sc = ldexp(1.0, (int)(w >> 27) - 24); out->c[0] = (w & 511) * sc; out->c[1] = ((w >> 9) & 511) * sc; out->c[2] = ((w >> 18) & 511) * sc; break; }
    case L_RGB10A2: out->c[0] = (w & 1023) / 1023.0; out->c[1] = ((w >> 10) & 1023) / 1023.0; out->c[2] = ((w >> 20) & 1023) / 1023.0; out->c[3] = (w >> 30) / 3.0; break;
    case L_RGB10A2UI: out->c[0] = w & 1023; out->c[1] = (w >> 10) & 1023; out->c[2] = (w >> 20) & 1023; out->c[3] = w >> 30; break;
    case L_Z16: out->c[0] = read_elem(p, GL_UNSIGNED_SHORT, 1); break;
    case L_Z24X8: out->c[0] = (w & 0xFFFFFF) / 16777215.0; break;
    case L_Z24S8: out->c[0] = (w & 0xFFFFFF) / 16777215.0; out->c[1] = w >> 24; break;
    case L_S8Z24: out->c[0] = (w >> 8) / 16777215.0; out->c[1] = w & 255; break;
    case L_Z32F: out->c[0] = read_elem(p, GL_FLOAT, 0); break;
    case L_Z32FS8: out->c[0] = read_elem(p, GL_FLOAT, 0); out->c[1] = p[4]; break;
    case L_S8: out->c[1] = p[0]; break;
    }
}

static void host_write(uint8_t *p, const struct layout *l, const pix *in)
{
    uint32_t w = 0;
    const double *c = in->c;
    switch (l->lay) {
    case L_ARRAY:
        for (int i = 0; i < l->n; i++) {
            uint8_t *q = p + i * (l->bits / 8);
            double x = c[l->order[i]];
            switch (l->num) {
            case N_UNORM: write_elem(q, l->bits == 8 ? GL_UNSIGNED_BYTE : l->bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 1, x); break;
            case N_SNORM: write_elem(q, l->bits == 8 ? GL_BYTE : GL_SHORT, 1, x); break;
            case N_UINT: write_elem(q, l->bits == 8 ? GL_UNSIGNED_BYTE : l->bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 0, x); break;
            case N_SINT: write_elem(q, l->bits == 8 ? GL_BYTE : l->bits == 16 ? GL_SHORT : GL_INT, 0, x); break;
            case N_HALF: write_elem(q, GL_HALF_FLOAT, 0, x); break;
            case N_FLOAT: write_elem(q, GL_FLOAT, 0, x); break;
            }
        }
        return;
    case L_R11G11B10F: w = to_uf((float)c[0], 6) | to_uf((float)c[1], 6) << 11 | to_uf((float)c[2], 5) << 22; break;
    case L_RGB9E5: { pix t = *in; client_write(p, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, &t); return; }
    case L_RGB10A2: { pix t = *in; client_write(p, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &t); return; }
    case L_RGB10A2UI: w = ((uint32_t)c[0] & 1023) | ((uint32_t)c[1] & 1023) << 10 | ((uint32_t)c[2] & 1023) << 20 | ((uint32_t)c[3] & 3) << 30; break;
    case L_Z16: write_elem(p, GL_UNSIGNED_SHORT, 1, c[0]); return;
    case L_Z24X8: w = (uint32_t)lrint(clampf((float)c[0], 0, 1) * 16777215.0); break;
    case L_Z24S8: w = (uint32_t)lrint(clampf((float)c[0], 0, 1) * 16777215.0) | ((uint32_t)c[1] & 255) << 24; break;
    case L_S8Z24: w = (uint32_t)lrint(clampf((float)c[0], 0, 1) * 16777215.0) << 8 | ((uint32_t)c[1] & 255); break;
    case L_Z32F: write_elem(p, GL_FLOAT, 0, c[0]); return;
    case L_Z32FS8: write_elem(p, GL_FLOAT, 0, c[0]); memset(p + 4, 0, 4); p[4] = (uint8_t)c[1]; return;
    case L_S8: p[0] = (uint8_t)c[1]; return;
    }
    memcpy(p, &w, 4);
}

void zgl_unpack_store(struct zgl_pixel_store *ps)
{
    ps->alignment = zgl->unpack_alignment; ps->row_length = zgl->unpack_row_length; ps->image_height = zgl->unpack_image_height;
    ps->skip_rows = zgl->unpack_skip_rows; ps->skip_pixels = zgl->unpack_skip_pixels; ps->skip_images = zgl->unpack_skip_images; ps->swap = zgl->unpack_swap;
}
void zgl_pack_store(struct zgl_pixel_store *ps)
{
    ps->alignment = zgl->pack_alignment; ps->row_length = zgl->pack_row_length; ps->image_height = zgl->pack_image_height;
    ps->skip_rows = zgl->pack_skip_rows; ps->skip_pixels = zgl->pack_skip_pixels; ps->skip_images = zgl->pack_skip_images; ps->swap = zgl->pack_swap;
}

/* the client layout of a w x h x d block: bytes per row and per image, and where it starts */
static int client_geometry(int w, int h, GLenum format, GLenum type, const struct zgl_pixel_store *ps, size_t *row, size_t *image, size_t *start)
{
    int px = client_pixel_size(format, type);
    if (!px) return -1;
    size_t rl = (size_t)(ps->row_length > 0 ? ps->row_length : w);
    size_t a = (size_t)(ps->alignment > 0 ? ps->alignment : 1);
    *row = (rl * px + a - 1) / a * a;
    size_t ih = (size_t)(ps->image_height > 0 ? ps->image_height : h);
    *image = *row * ih;
    *start = (size_t)ps->skip_images * *image + (size_t)ps->skip_rows * *row + (size_t)ps->skip_pixels * px;
    return px;
}

size_t zgl_pixels_size(int w, int h, int d, GLenum format, GLenum type, const struct zgl_pixel_store *ps)
{
    size_t row, image, start;
    int px = client_geometry(w, h, format, type, ps, &row, &image, &start);
    if (px < 0 || w <= 0 || h <= 0 || d <= 0) return 0;
    return start + (size_t)(d - 1) * image + (size_t)(h - 1) * row + (size_t)w * px;
}

/* memory-identical client and host pixels: rows can be copied */
static int same_layout(const struct layout *l, GLenum format, GLenum type)
{
    if (!l || l->lay != L_ARRAY) {
        if (!l) return 0;
        if (l->lay == L_Z32F) return format == GL_DEPTH_COMPONENT && type == GL_FLOAT;
        if (l->lay == L_Z16) return format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT;
        if (l->lay == L_S8Z24) return format == GL_DEPTH_STENCIL && type == GL_UNSIGNED_INT_24_8;
        if (l->lay == L_Z32FS8) return format == GL_DEPTH_STENCIL && type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        if (l->lay == L_RGB10A2) return format == GL_RGBA && type == GL_UNSIGNED_INT_2_10_10_10_REV;
        if (l->lay == L_RGB10A2UI) return format == GL_RGBA_INTEGER && type == GL_UNSIGNED_INT_2_10_10_10_REV;
        if (l->lay == L_R11G11B10F) return format == GL_RGB && type == GL_UNSIGNED_INT_10F_11F_11F_REV;
        if (l->lay == L_RGB9E5) return format == GL_RGB && type == GL_UNSIGNED_INT_5_9_9_9_REV;
        return 0;
    }
    int8_t o[4] = { -1, -1, -1, -1 }; int integer;
    int n = client_comps(format, o, &integer);
    if (n != l->n) return 0;
    for (int i = 0; i < n; i++) if (o[i] != l->order[i]) return 0;
    if (integer != (l->num == N_UINT || l->num == N_SINT)) return 0;
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV && l->bits == 8 && l->num == N_UNORM && n == 4) return 1;
    GLenum want = 0;
    switch (l->num) {
    case N_UNORM: case N_UINT: want = l->bits == 8 ? GL_UNSIGNED_BYTE : l->bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT; break;
    case N_SNORM: case N_SINT: want = l->bits == 8 ? GL_BYTE : l->bits == 16 ? GL_SHORT : GL_INT; break;
    case N_HALF: want = GL_HALF_FLOAT; break;
    case N_FLOAT: want = GL_FLOAT; break;
    }
    return type == want;
}

/* the logical components a format has beyond what its host format stores
 * are its defaults; and an RGB format stored as RGBA reads alpha as one */
static void fix_logical(const struct zgl_format *f, pix *p)
{
    if (f->comps == 3 && f->kind != ZF_DEPTH && f->kind != ZF_DEPTH_STENCIL) p->c[3] = 1;
}

int zgl_pixels_to_format(const struct zgl_format *f, int w, int h, int d, GLenum format, GLenum type, const void *src_,
                         const struct zgl_pixel_store *ps, uint8_t *dst)
{
    const uint8_t *src = src_;
    const struct layout *l = layout_of(f->vformat);
    if (!l) return -1;
    size_t row, image, start;
    int px = client_geometry(w, h, format, type, ps, &row, &image, &start);
    if (px < 0) return -1;
    size_t drow = (size_t)w * f->bpp;
    int fast = same_layout(l, format, type) && f->comps != 3;
    for (int z = 0; z < d; z++)
        for (int y = 0; y < h; y++) {
            const uint8_t *s = src + start + (size_t)z * image + (size_t)y * row;
            uint8_t *o = dst + ((size_t)z * h + y) * drow;
            if (fast) { memcpy(o, s, drow); continue; }
            /* the common 8-bit RGBA <-> BGRA case without the general path */
            if (l->lay == L_ARRAY && l->bits == 8 && l->n == 4 && l->num == N_UNORM && l->order[0] == 2 && type == GL_UNSIGNED_BYTE && (format == GL_RGBA || format == GL_RGB)) {
                int n = format == GL_RGBA ? 4 : 3;
                for (int x = 0; x < w; x++, s += n, o += 4) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = n == 4 && f->comps == 4 ? s[3] : 255; }
                continue;
            }
            for (int x = 0; x < w; x++, s += px, o += f->bpp) {
                pix p;
                client_read(s, format, type, &p);
                fix_logical(f, &p);
                host_write(o, l, &p);
            }
        }
    return 0;
}

int zgl_pixels_from_format(const struct zgl_format *f, int w, int h, int d, const uint8_t *src, GLenum format, GLenum type,
                           const struct zgl_pixel_store *ps, void *dst_)
{
    uint8_t *dst = dst_;
    const struct layout *l = layout_of(f->vformat);
    if (!l) return -1;
    size_t row, image, start;
    int px = client_geometry(w, h, format, type, ps, &row, &image, &start);
    if (px < 0) return -1;
    size_t srow = (size_t)w * f->bpp;
    int fast = same_layout(l, format, type) && f->comps != 3;
    for (int z = 0; z < d; z++)
        for (int y = 0; y < h; y++) {
            const uint8_t *s = src + ((size_t)z * h + y) * srow;
            uint8_t *o = dst + start + (size_t)z * image + (size_t)y * row;
            if (fast) { memcpy(o, s, srow); continue; }
            if (l->lay == L_ARRAY && l->bits == 8 && l->n == 4 && l->num == N_UNORM && l->order[0] == 2 && type == GL_UNSIGNED_BYTE && (format == GL_RGBA || format == GL_RGB)) {
                int n = format == GL_RGBA ? 4 : 3;
                for (int x = 0; x < w; x++, s += 4, o += n) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; if (n == 4) o[3] = f->comps == 4 ? s[3] : 255; }
                continue;
            }
            for (int x = 0; x < w; x++, s += f->bpp, o += px) {
                pix p;
                host_read(s, l, &p);
                fix_logical(f, &p);
                client_write(o, format, type, &p);
            }
        }
    return 0;
}

const void *zgl_unpack_pointer(const void *pixels, size_t need)
{
    struct zgl_buffer *b = zgl->pixel_unpack_buffer;
    if (!b) return pixels;
    size_t off = (size_t)(uintptr_t)pixels;
    if (!b->shadow || b->mapped || off + need > b->size) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    zgl_buffer_sync(b);
    return b->shadow + off;
}

/* ---- texture objects ---------------------------------------------------------------- */

int zgl_target_index(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_1D: return ZT_1D;
    case GL_TEXTURE_2D: return ZT_2D;
    case GL_TEXTURE_3D: return ZT_3D;
    case GL_TEXTURE_CUBE_MAP: case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: return ZT_CUBE;
    case GL_TEXTURE_RECTANGLE: return ZT_RECT;
    case GL_TEXTURE_1D_ARRAY: return ZT_1D_ARRAY;
    case GL_TEXTURE_2D_ARRAY: return ZT_2D_ARRAY;
    case GL_TEXTURE_BUFFER: return ZT_BUFFER;
    case GL_TEXTURE_2D_MULTISAMPLE: return ZT_2D_MS;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return ZT_2D_MS_ARRAY;
    case 0x9009 /* GL_TEXTURE_CUBE_MAP_ARRAY */: return ZT_CUBE_ARRAY;
    }
    return -1;
}

static GLenum bind_target(GLenum target)          /* cube faces bind as the cube map */
{
    return zgl_target_index(target) == ZT_CUBE ? GL_TEXTURE_CUBE_MAP : target;
}

static uint32_t pipe_target(GLenum target, int samples)
{
    switch (bind_target(target)) {
    case GL_TEXTURE_1D: return PIPE_TEXTURE_1D;
    case GL_TEXTURE_2D: return PIPE_TEXTURE_2D;
    case GL_TEXTURE_3D: return PIPE_TEXTURE_3D;
    case GL_TEXTURE_CUBE_MAP: return PIPE_TEXTURE_CUBE;
    case GL_TEXTURE_RECTANGLE: return PIPE_TEXTURE_RECT;
    case GL_TEXTURE_1D_ARRAY: return PIPE_TEXTURE_1D_ARRAY;
    case GL_TEXTURE_2D_ARRAY: case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return PIPE_TEXTURE_2D_ARRAY;
    case GL_TEXTURE_2D_MULTISAMPLE: return PIPE_TEXTURE_2D;
    case 0x9009: return PIPE_TEXTURE_CUBE_ARRAY;
    case GL_TEXTURE_BUFFER: return PIPE_BUFFER;
    }
    (void)samples;
    return PIPE_TEXTURE_2D;
}

static void default_sampler_params(struct zgl_sampler_params *sp, GLenum target)
{
    memset(sp, 0, sizeof *sp);
    int rect = target == GL_TEXTURE_RECTANGLE;
    sp->min_filter = rect ? GL_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    sp->mag_filter = GL_LINEAR;
    sp->wrap_s = sp->wrap_t = sp->wrap_r = rect ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    sp->min_lod = -1000; sp->max_lod = 1000; sp->max_aniso = 1;
    sp->compare_mode = GL_NONE; sp->compare_func = GL_LEQUAL;
}

static struct zgl_texture *texture_new(GLuint name)
{
    struct zgl_texture *t = calloc(1, sizeof *t);
    t->name = name;
    default_sampler_params(&t->sp, 0);
    t->base_level = 0; t->max_level = 1000;
    t->swizzle[0] = GL_RED; t->swizzle[1] = GL_GREEN; t->swizzle[2] = GL_BLUE; t->swizzle[3] = GL_ALPHA;
    t->depth_mode = GL_DEPTH_COMPONENT;
    LIST_ADD(zgl->textures, zgl->ntextures, t);
    return t;
}

struct zgl_texture *zgl_texture_get(GLuint name)
{
    for (int i = 0; i < zgl->ntextures; i++) if (zgl->textures[i]->name == name) return zgl->textures[i];
    return NULL;
}

void glGenTextures(GLsizei n, GLuint *t)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) t[i] = texture_new(zgl->next_name++)->name;
}

static void texture_drop_objects(struct zgl_texture *t)
{
    if (t->h_view) { virgl_destroy_object(zgl->v, t->h_view, VIRGL_OBJECT_SAMPLER_VIEW); t->h_view = 0; }
    t->h_sampler = 0;                                   /* sampler states are shared (the cache owns them) */
}

void zgl_texture_free(struct zgl_texture *t)
{
    for (int u = 0; u < ZGL_MAX_UNITS; u++) for (int k = 0; k < ZT_COUNT; k++) if (zgl->unit_tex[u][k] == t) zgl->unit_tex[u][k] = NULL;
    zgl_detach_texture(t);
    texture_drop_objects(t);
    if (t->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, t->res); }
    LIST_DEL(zgl->textures, zgl->ntextures, t);
    free(t);
}

void glDeleteTextures(GLsizei n, const GLuint *t)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) { if (!t[i]) continue; struct zgl_texture *tex = zgl_texture_get(t[i]); if (tex) zgl_texture_free(tex); }
}

GLboolean glIsTexture(GLuint t) { CTXV(0); struct zgl_texture *tex = t ? zgl_texture_get(t) : NULL; return tex && tex->target; }

void glBindTexture(GLenum target, GLuint name)
{
    CTX();
    int k = zgl_target_index(target);
    if (k < 0 || target != bind_target(target)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = NULL;
    if (name) {
        t = zgl_texture_get(name);
        if (!t) { t = texture_new(name); if (name >= zgl->next_name) zgl->next_name = name + 1; }
        if (!t->target) { t->target = target; default_sampler_params(&t->sp, target); }
        else if (t->target != target) { zgl_set_error(GL_INVALID_OPERATION); return; }
    }
    zgl->unit_tex[zgl->active_unit][k] = t;
}

void glActiveTexture(GLenum unit)
{
    CTX();
    if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + ZGL_MAX_UNITS) { zgl_set_error(GL_INVALID_ENUM); return; }
    zgl->active_unit = (int)(unit - GL_TEXTURE0);
}

struct zgl_texture *zgl_bound_texture(GLenum target)
{
    int k = zgl_target_index(target);
    if (k < 0) { zgl_set_error(GL_INVALID_ENUM); return NULL; }
    struct zgl_texture *t = zgl->unit_tex[zgl->active_unit][k];
    if (!t) zgl_set_error(GL_INVALID_OPERATION);
    return t;
}

static int max_levels(int w, int h, int d)
{
    int m = w > h ? w : h;
    if (d > m) m = d;
    int n = 1;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

static int level_dim(int x, int level) { x >>= level; return x < 1 ? 1 : x; }

/* the dimensions of a level: arrays keep their layer count, cube maps their 6 faces */
static void level_size(const struct zgl_texture *t, int level, int *w, int *h, int *d)
{
    *w = level_dim(t->w, level);
    *h = (t->target == GL_TEXTURE_1D || t->target == GL_TEXTURE_1D_ARRAY) ? t->h : level_dim(t->h, level);
    if (t->target == GL_TEXTURE_1D_ARRAY) *h = t->h;
    *d = t->target == GL_TEXTURE_3D ? level_dim(t->d, level) : t->d;
}

static int is_depth_kind(int k) { return k == ZF_DEPTH || k == ZF_DEPTH_STENCIL || k == ZF_STENCIL; }

/* Make the host resource for a texture of this size and format with `levels` mip levels. */
static int texture_alloc(struct zgl_texture *t, const struct zgl_format *f, int w, int h, int d, int levels, int samples)
{
    texture_drop_objects(t);
    if (t->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, t->res); t->res = NULL; }
    t->fmt = f; t->w = w; t->h = h; t->d = d; t->levels = levels; t->samples = samples; t->defined = 0;
    t->has_level0 = 0;
    zgl_detach_texture(t);                              /* framebuffer surfaces on the old resource are remade */
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    uint32_t bind = VIRGL_BIND_SAMPLER_VIEW;
    if (is_depth_kind(f->kind)) bind |= VIRGL_BIND_DEPTH_STENCIL;
    else if (f->kind != ZF_COMPRESSED && can_render(f->vformat)) bind |= VIRGL_BIND_RENDER_TARGET;
    uint32_t target = pipe_target(t->target, samples);
    int layers = 1, depth = 1;
    switch (t->target) {
    case GL_TEXTURE_3D: depth = d; break;
    case GL_TEXTURE_1D_ARRAY: layers = h; h = 1; break;
    case GL_TEXTURE_2D_ARRAY: case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: case 0x9009: layers = d; break;
    case GL_TEXTURE_CUBE_MAP: layers = 6; break;
    }
    if (samples > 1 && !zgl_can_multisample(f->vformat)) samples = t->samples = 0;     /* one sample: GL_TEXTURE_SAMPLES says so */
    struct gpu_res_create rc = { .target = target, .format = f->vformat, .bind = bind, .width = (uint32_t)w, .height = (uint32_t)h,
                                 .depth = (uint32_t)depth, .array_size = (uint32_t)layers, .last_level = (uint32_t)(levels - 1),
                                 .nr_samples = (uint32_t)(samples > 1 ? samples : 0) };
    t->res = virgl_res_create_ex(zgl->v, &rc);
    if (!t->res && samples > 1) {
        /* no multisampling for this format on the host: one sample */
        t->samples = 0; rc.nr_samples = 0;
        t->res = virgl_res_create_ex(zgl->v, &rc);
    }
    if (!t->res) { zgl_set_error(GL_OUT_OF_MEMORY); return -1; }
    return 0;
}

/* Upload a box of host-format pixels (rows of w * bpp bytes, tightly packed) into a level. */
static void texture_write(struct zgl_texture *t, int level, int x, int y, int z, int w, int h, int d, const uint8_t *data)
{
    const struct zgl_format *f = t->fmt;
    int bw = f->block > 1 ? (w + f->block - 1) / f->block : w;      /* compressed: in blocks */
    int bh = f->block > 1 ? (h + f->block - 1) / f->block : h;
    size_t row = (size_t)bw * f->bpp;
    size_t image = row * bh;
    /* 1D arrays keep their layers in y */
    int is1da = t->target == GL_TEXTURE_1D_ARRAY;
    for (int zz = 0; zz < d; zz++) {
        int rows_per_chunk = (int)((48 * 1024) / row);
        if (rows_per_chunk < 1) rows_per_chunk = 1;
        if (f->block > 1) rows_per_chunk = rows_per_chunk < 1 ? 1 : rows_per_chunk;
        for (int yy = 0; yy < bh; yy += rows_per_chunk) {
            int n = bh - yy < rows_per_chunk ? bh - yy : rows_per_chunk;
            int py = f->block > 1 ? yy * f->block : yy, ph = f->block > 1 ? n * f->block : n;
            if (f->block > 1 && py + ph > h) ph = h - py;
            if (is1da)
                virgl_inline_write(zgl->v, t->res->id, (uint32_t)level, 0, (uint32_t)row, (uint32_t)(row * n),
                                   (uint32_t)x, 0, (uint32_t)(y + py), (uint32_t)w, 1, (uint32_t)ph, data + zz * image + (size_t)yy * row, row * n);
            else
                virgl_inline_write(zgl->v, t->res->id, (uint32_t)level, 0, (uint32_t)row, (uint32_t)(row * n),
                                   (uint32_t)x, (uint32_t)(y + py), (uint32_t)(z + zz), (uint32_t)w, (uint32_t)ph, 1,
                                   data + zz * image + (size_t)yy * row, row * n);
        }
    }
}

static int face_index(GLenum target)
{
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) return (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    return 0;
}

/* glTexImage1D/2D/3D: the image target and the level's box */
static int image_target_ok(GLenum target, int dims)
{
    switch (dims) {
    case 1: return target == GL_TEXTURE_1D;
    case 2: return target == GL_TEXTURE_2D || target == GL_TEXTURE_RECTANGLE || target == GL_TEXTURE_1D_ARRAY ||
                   (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z);
    case 3: return target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY || target == 0x9009;
    }
    return 0;
}

/* convert client pixels to the texture's format and upload them */
static void upload_pixels(struct zgl_texture *t, int level, int x, int y, int z, int w, int h, int d, GLenum format, GLenum type, const void *pixels)
{
    struct zgl_pixel_store ps;
    zgl_unpack_store(&ps);
    size_t need = zgl_pixels_size(w, h, d, format, type, &ps);
    if (!need) { zgl_set_error(GL_INVALID_ENUM); return; }
    const void *src = zgl_unpack_pointer(pixels, need);
    if (!src) return;
    size_t bytes = (size_t)w * h * d * t->fmt->bpp;
    uint8_t *buf = malloc(bytes ? bytes : 1);
    if (!buf) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
    if (zgl_pixels_to_format(t->fmt, w, h, d, format, type, src, &ps, buf) != 0) { free(buf); zgl_set_error(GL_INVALID_OPERATION); return; }
    texture_write(t, level, x, y, z, w, h, d, buf);
    free(buf);
}

static int format_type_ok(GLenum format, GLenum type)
{
    return client_pixel_size(format, type) > 0;
}

static void tex_image(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void *pixels, int dims)
{
    if (!image_target_ok(target, dims)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (t->immutable) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (level < 0 || w < 0 || h < 0 || d < 0 || border != 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!format_type_ok(format, type)) { zgl_set_error(GL_INVALID_ENUM); return; }
    const struct zgl_format *f = zgl_format_of((GLenum)internal, format, type);
    if (!f || f->kind == ZF_COMPRESSED) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (target == GL_TEXTURE_RECTANGLE && level != 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    int face = face_index(target);
    /* the level 0 size this level implies */
    int w0 = w << level, h0 = (target == GL_TEXTURE_1D_ARRAY || dims == 1) ? h : h << level;
    int d0 = (t->target == GL_TEXTURE_3D) ? d << level : d;
    if (dims == 1) { h0 = 1; d0 = 1; }
    if (dims == 2 && t->target != GL_TEXTURE_3D) d0 = t->target == GL_TEXTURE_CUBE_MAP ? 1 : 1;
    if (t->target == GL_TEXTURE_CUBE_MAP) d0 = 6;
    int lw, lh, ld;
    int fits = 0;
    if (t->res && t->fmt == f && level < t->levels) { level_size(t, level, &lw, &lh, &ld); fits = lw == w && (dims < 2 || lh == h) && (dims < 3 || ld == d); }
    if (!fits) {
        if (level == 0 || !t->res || t->fmt != f) {
            int mipmapped = t->target != GL_TEXTURE_RECTANGLE;
            if (texture_alloc(t, f, w0, h0, d0, mipmapped ? max_levels(w0, t->target == GL_TEXTURE_1D_ARRAY ? 1 : h0, t->target == GL_TEXTURE_3D ? d0 : 1) : 1, 0) != 0) return;
        } else return;                                  /* a level that doesn't fit the texture: ignored (GL: incomplete) */
    }
    t->defined |= 1u << level;
    if (level == 0) t->has_level0 = 1;
    if (!pixels && !zgl->pixel_unpack_buffer) return;
    if (!t->res || w == 0 || h == 0 || d == 0) return;
    int z = 0, hh = h, dd = d;
    if (t->target == GL_TEXTURE_CUBE_MAP) { z = face; dd = 1; }
    if (dims == 1) { hh = 1; dd = 1; }
    upload_pixels(t, level, 0, 0, z, w, hh, dd, format, type, pixels);
    if (t->generate_mipmap && level == 0) glGenerateMipmap(bind_target(target));
}

void glTexImage1D(GLenum target, GLint level, GLint internal, GLsizei w, GLint border, GLenum format, GLenum type, const void *pixels)
{ CTX(); tex_image(target, level, internal, w, 1, 1, border, format, type, pixels, 1); }
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{ CTX(); tex_image(target, level, internal, w, h, 1, border, format, type, pixels, 2); }
void glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void *pixels)
{ CTX(); tex_image(target, level, internal, w, h, d, border, format, type, pixels, 3); }

static void tex_sub_image(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void *pixels, int dims)
{
    if (!image_target_ok(target, dims)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (!t->res || level < 0 || level >= t->levels) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (!format_type_ok(format, type)) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (t->fmt->kind == ZF_COMPRESSED) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int lw, lh, ld;
    level_size(t, level, &lw, &lh, &ld);
    if (t->target == GL_TEXTURE_CUBE_MAP) { z = face_index(target); d = 1; ld = 6; }
    if (dims == 1) { y = 0; h = 1; lh = 1; }
    if (x < 0 || y < 0 || z < 0 || w < 0 || h < 0 || d < 0 || x + w > lw || y + h > lh || z + d > ld) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!w || !h || !d) return;
    upload_pixels(t, level, x, y, z, w, h, d, format, type, pixels);
}

void glTexSubImage1D(GLenum target, GLint level, GLint x, GLsizei w, GLenum format, GLenum type, const void *pixels)
{ CTX(); tex_sub_image(target, level, x, 0, 0, w, 1, 1, format, type, pixels, 1); }
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{ CTX(); tex_sub_image(target, level, x, y, 0, w, h, 1, format, type, pixels, 2); }
void glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void *pixels)
{ CTX(); tex_sub_image(target, level, x, y, z, w, h, d, format, type, pixels, 3); }

/* ---- compressed images ------------------------------------------------------------- */

static void compressed_image(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLsizei d, GLint border, GLsizei size, const void *data, int dims)
{
    if (!image_target_ok(target, dims)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    const struct zgl_format *f = find_format(internal);
    if (!f || f->kind != ZF_COMPRESSED || border || level < 0 || w < 0 || h < 0) { zgl_set_error(GL_INVALID_ENUM); return; }
    size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * f->bpp * d;
    if ((size_t)size < need) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (level == 0 || !t->res || t->fmt != f) {
        int w0 = w << level, h0 = h << level;
        if (texture_alloc(t, f, w0, h0, t->target == GL_TEXTURE_CUBE_MAP ? 6 : d, max_levels(w0, h0, 1), 0) != 0) return;
    }
    t->defined |= 1u << level;
    if (level == 0) t->has_level0 = 1;
    const void *src = zgl_unpack_pointer(data, need);
    if (!src || !w || !h) return;
    texture_write(t, level, 0, 0, t->target == GL_TEXTURE_CUBE_MAP ? face_index(target) : 0, w, h, t->target == GL_TEXTURE_CUBE_MAP ? 1 : d, src);
}

void glCompressedTexImage1D(GLenum target, GLint level, GLenum internal, GLsizei w, GLint border, GLsizei size, const void *data)
{ CTX(); compressed_image(target, level, internal, w, 1, 1, border, size, data, 1); }
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data)
{ CTX(); compressed_image(target, level, internal, w, h, 1, border, size, data, 2); }
void glCompressedTexImage3D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLsizei d, GLint border, GLsizei size, const void *data)
{ CTX(); compressed_image(target, level, internal, w, h, d, border, size, data, 3); }

static void compressed_sub(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLsizei size, const void *data, int dims)
{
    if (!image_target_ok(target, dims)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (!t->res || t->fmt->kind != ZF_COMPRESSED || t->fmt->internal != format || level >= t->levels) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if ((x & 3) || (y & 3)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * t->fmt->bpp * d;
    if ((size_t)size < need) { zgl_set_error(GL_INVALID_VALUE); return; }
    const void *src = zgl_unpack_pointer(data, need);
    if (!src || !w || !h) return;
    if (t->target == GL_TEXTURE_CUBE_MAP) z = face_index(target);
    texture_write(t, level, x, y, z, w, h, d, src);
}

void glCompressedTexSubImage1D(GLenum target, GLint level, GLint x, GLsizei w, GLenum format, GLsizei size, const void *data)
{ CTX(); compressed_sub(target, level, x, 0, 0, w, 1, 1, format, size, data, 1); }
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size, const void *data)
{ CTX(); compressed_sub(target, level, x, y, 0, w, h, 1, format, size, data, 2); }
void glCompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLsizei size, const void *data)
{ CTX(); compressed_sub(target, level, x, y, z, w, h, d, format, size, data, 3); }

/* ---- immutable storage, multisample ------------------------------------------------- */

static void tex_storage(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d, int dims)
{
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (t->immutable) { zgl_set_error(GL_INVALID_OPERATION); return; }
    const struct zgl_format *f = zgl_format_of(internal, 0, 0);
    if (!f || internal == GL_RGBA || internal == GL_RGB || internal == GL_RED || internal == GL_RG) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (levels < 1 || w < 1 || h < 1 || d < 1) { zgl_set_error(GL_INVALID_VALUE); return; }
    int maxl = max_levels(w, dims >= 2 && target != GL_TEXTURE_1D_ARRAY ? h : 1, target == GL_TEXTURE_3D ? d : 1);
    if (levels > maxl) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (t->target == GL_TEXTURE_CUBE_MAP) d = 6;
    if (texture_alloc(t, f, w, h, d, levels, 0) != 0) return;
    t->immutable = 1;
    t->defined = (levels >= 32) ? 0xFFFFFFFFu : (1u << levels) - 1;
    t->has_level0 = 1;
}

void glTexStorage1D(GLenum target, GLsizei levels, GLenum internal, GLsizei w) { CTX(); tex_storage(target, levels, internal, w, 1, 1, 1); }
void glTexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h) { CTX(); tex_storage(target, levels, internal, w, h, 1, 2); }
void glTexStorage3D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d) { CTX(); tex_storage(target, levels, internal, w, h, d, 3); }

void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h, GLboolean fixed)
{
    CTX();
    if (target != GL_TEXTURE_2D_MULTISAMPLE) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    const struct zgl_format *f = zgl_format_of(internal, 0, 0);
    if (!f) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (samples < 1 || w < 1 || h < 1) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (texture_alloc(t, zgl_format_ms(f), w, h, 1, 1, samples) != 0) return;
    t->fixed_locations = fixed; t->defined = 1; t->has_level0 = 1;
}

void glTexImage3DMultisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h, GLsizei d, GLboolean fixed)
{
    CTX();
    if (target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    const struct zgl_format *f = zgl_format_of(internal, 0, 0);
    if (!f) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (samples < 1 || w < 1 || h < 1 || d < 1) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (texture_alloc(t, zgl_format_ms(f), w, h, d, 1, samples) != 0) return;
    t->fixed_locations = fixed; t->defined = 1; t->has_level0 = 1;
}

void glGetMultisamplefv(GLenum pname, GLuint index, GLfloat *val)
{
    CTX();
    if (pname != GL_SAMPLE_POSITION) { zgl_set_error(GL_INVALID_ENUM); return; }
    /* the standard 4x pattern; the host does its own */
    static const float pos[4][2] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.125f, 0.625f }, { 0.625f, 0.875f } };
    val[0] = pos[index & 3][0]; val[1] = pos[index & 3][1];
}

/* ---- texture buffers ----------------------------------------------------------------- */

void glTexBuffer(GLenum target, GLenum internal, GLuint buffer)
{
    CTX();
    if (target != GL_TEXTURE_BUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    const struct zgl_format *f = find_format(internal);
    if (buffer && (!f || f->comps == 3 || is_depth_kind(f->kind) || f->kind == ZF_COMPRESSED || f->kind == ZF_SRGB)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_buffer *b = buffer ? zgl_buffer_get(buffer) : NULL;
    if (buffer && !b) { zgl_set_error(GL_INVALID_OPERATION); return; }
    texture_drop_objects(t);
    t->buffer = b; t->fmt = f; t->buf_off = 0; t->buf_size = b ? b->size : 0;
    t->tbo_version = ~0u;                               /* refresh at the next draw */
    t->has_level0 = b != NULL;
}

/* A texture buffer is a 2D texture ZGL_TBO_WIDTH texels wide on the host,
 * filled from the buffer when the buffer changed: macOS GL through
 * virglrenderer crashes with real GL_TEXTURE_BUFFER objects. The bytes are
 * what the program wrote, so RGBA8 is R8G8B8A8 here (not the BGRA the
 * other textures use). */
static uint32_t tbo_vformat(const struct zgl_format *f)
{
    return f->vformat == VIRGL_FORMAT_B8G8R8A8_UNORM ? VIRGL_FORMAT_R8G8B8A8_UNORM : f->vformat;
}

int zgl_tbo_elements(const struct zgl_texture *t)
{
    if (!t->buffer || !t->fmt) return 0;
    size_t bytes = t->buffer->size > t->buf_off ? t->buffer->size - t->buf_off : 0;
    if (t->buf_size && t->buf_size < bytes) bytes = t->buf_size;
    return (int)(bytes / t->fmt->bpp);
}

int zgl_tbo_update(struct zgl_texture *t)
{
    struct zgl_buffer *b = t->buffer;
    if (!b || !b->shadow || !t->fmt) return -1;
    int n = zgl_tbo_elements(t);
    if (n <= 0) return -1;
    if (t->res && t->tbo_src == b && t->tbo_version == b->version && t->tbo_n == n) return 0;
    zgl_buffer_sync(b);
    int w = n < ZGL_TBO_WIDTH ? n : ZGL_TBO_WIDTH, h = (n + ZGL_TBO_WIDTH - 1) / ZGL_TBO_WIDTH;
    uint32_t vf = tbo_vformat(t->fmt);
    if (!t->res || t->w != w || t->h != h || t->res->format != vf) {
        texture_drop_objects(t);
        if (t->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, t->res); }
        struct gpu_res_create rc = { .target = PIPE_TEXTURE_2D, .format = vf, .bind = VIRGL_BIND_SAMPLER_VIEW,
                                     .width = (uint32_t)w, .height = (uint32_t)h, .depth = 1, .array_size = 1 };
        t->res = virgl_res_create_ex(zgl->v, &rc);
        if (!t->res) { zgl_set_error(GL_OUT_OF_MEMORY); return -1; }
        t->w = w; t->h = h; t->d = 1; t->levels = 1;
    }
    const uint8_t *src = b->shadow + t->buf_off;
    size_t bpp = t->fmt->bpp, row = (size_t)w * bpp;
    int full = n / w, rows_per_chunk = (int)((48 * 1024) / row);
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    for (int y = 0; y < full; y += rows_per_chunk) {
        int k = full - y < rows_per_chunk ? full - y : rows_per_chunk;
        virgl_inline_write(zgl->v, t->res->id, 0, 0, (uint32_t)row, (uint32_t)(row * k), 0, (uint32_t)y, 0, (uint32_t)w, (uint32_t)k, 1, src + (size_t)y * row, row * k);
    }
    int rest = n - full * w;
    if (rest) virgl_inline_write(zgl->v, t->res->id, 0, 0, (uint32_t)(rest * bpp), (uint32_t)(rest * bpp), 0, (uint32_t)full, 0, (uint32_t)rest, 1, 1, src + (size_t)full * row, (size_t)rest * bpp);
    t->tbo_src = b; t->tbo_version = b->version; t->tbo_n = n;
    return 0;
}

/* ---- parameters ------------------------------------------------------------------------ */

static void sampler_param(struct zgl_sampler_params *sp, GLenum pname, const GLint *iv, const GLfloat *fv, int is_int_border, int *ok)
{
    *ok = 1;
    GLint i = iv ? iv[0] : (GLint)fv[0];
    GLfloat f = fv ? fv[0] : (GLfloat)iv[0];
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (i != GL_NEAREST && i != GL_LINEAR && i != GL_NEAREST_MIPMAP_NEAREST && i != GL_LINEAR_MIPMAP_NEAREST &&
            i != GL_NEAREST_MIPMAP_LINEAR && i != GL_LINEAR_MIPMAP_LINEAR) { zgl_set_error(GL_INVALID_ENUM); return; }
        sp->min_filter = (GLenum)i; break;
    case GL_TEXTURE_MAG_FILTER:
        if (i != GL_NEAREST && i != GL_LINEAR) { zgl_set_error(GL_INVALID_ENUM); return; }
        sp->mag_filter = (GLenum)i; break;
    case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: case GL_TEXTURE_WRAP_R:
        if (i != GL_REPEAT && i != GL_CLAMP_TO_EDGE && i != GL_CLAMP_TO_BORDER && i != GL_MIRRORED_REPEAT && i != GL_CLAMP) { zgl_set_error(GL_INVALID_ENUM); return; }
        if (pname == GL_TEXTURE_WRAP_S) sp->wrap_s = (GLenum)i; else if (pname == GL_TEXTURE_WRAP_T) sp->wrap_t = (GLenum)i; else sp->wrap_r = (GLenum)i;
        break;
    case GL_TEXTURE_MIN_LOD: sp->min_lod = f; break;
    case GL_TEXTURE_MAX_LOD: sp->max_lod = f; break;
    case GL_TEXTURE_LOD_BIAS: sp->lod_bias = f; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: if (f < 1) { zgl_set_error(GL_INVALID_VALUE); return; } sp->max_aniso = f; break;
    case GL_TEXTURE_COMPARE_MODE:
        if (i != GL_NONE && i != GL_COMPARE_REF_TO_TEXTURE) { zgl_set_error(GL_INVALID_ENUM); return; }
        sp->compare_mode = (GLenum)i; break;
    case GL_TEXTURE_COMPARE_FUNC:
        if (i < GL_NEVER || i > GL_ALWAYS) { zgl_set_error(GL_INVALID_ENUM); return; }
        sp->compare_func = (GLenum)i; break;
    case GL_TEXTURE_BORDER_COLOR:
        for (int k = 0; k < 4; k++) {
            if (is_int_border) sp->border.i[k] = iv[k];
            else if (fv) sp->border.f[k] = fv[k];
            else sp->border.f[k] = (float)iv[k] / 2147483647.0f;
        }
        break;
    default: *ok = 0;
    }
}

static void tex_param(GLenum target, GLenum pname, const GLint *iv, const GLfloat *fv, int int_border)
{
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (target != bind_target(target)) { zgl_set_error(GL_INVALID_ENUM); return; }
    int ok;
    GLenum err0 = zgl->error;
    sampler_param(&t->sp, pname, iv, fv, int_border, &ok);
    if (zgl->error != err0 && err0 == GL_NO_ERROR) return;
    if (!ok) {
        GLint i = iv ? iv[0] : (GLint)fv[0];
        switch (pname) {
        case GL_TEXTURE_BASE_LEVEL: if (i < 0) { zgl_set_error(GL_INVALID_VALUE); return; } t->base_level = i; break;
        case GL_TEXTURE_MAX_LEVEL: if (i < 0) { zgl_set_error(GL_INVALID_VALUE); return; } t->max_level = i; break;
        case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
            if (i != GL_RED && i != GL_GREEN && i != GL_BLUE && i != GL_ALPHA && i != GL_ZERO && i != GL_ONE) { zgl_set_error(GL_INVALID_ENUM); return; }
            t->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = i; break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            for (int k = 0; k < 4; k++) t->swizzle[k] = iv ? iv[k] : (GLint)fv[k];
            break;
        case GL_GENERATE_MIPMAP: t->generate_mipmap = i != 0; return;
        case 0x90EA /* GL_DEPTH_STENCIL_TEXTURE_MODE */: t->depth_mode = (GLenum)i; break;
        case GL_TEXTURE_PRIORITY: return;
        default: zgl_set_error(GL_INVALID_ENUM); return;
        }
    }
    texture_drop_objects(t);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) { CTX(); tex_param(target, pname, &param, NULL, 0); }
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { CTX(); tex_param(target, pname, NULL, &param, 0); }
void glTexParameteriv(GLenum target, GLenum pname, const GLint *v) { CTX(); tex_param(target, pname, v, NULL, 0); }
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *v) { CTX(); tex_param(target, pname, NULL, v, 0); }
void glTexParameterIiv(GLenum target, GLenum pname, const GLint *v) { CTX(); tex_param(target, pname, v, NULL, 1); }
void glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *v) { CTX(); tex_param(target, pname, (const GLint *)v, NULL, 1); }

static int get_sampler_param(const struct zgl_sampler_params *sp, GLenum pname, GLint *iv, GLfloat *fv, int int_border)
{
#define PUT(x) do { if (iv) iv[0] = (GLint)(x); else fv[0] = (GLfloat)(x); } while (0)
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: PUT(sp->min_filter); return 1;
    case GL_TEXTURE_MAG_FILTER: PUT(sp->mag_filter); return 1;
    case GL_TEXTURE_WRAP_S: PUT(sp->wrap_s); return 1;
    case GL_TEXTURE_WRAP_T: PUT(sp->wrap_t); return 1;
    case GL_TEXTURE_WRAP_R: PUT(sp->wrap_r); return 1;
    case GL_TEXTURE_MIN_LOD: PUT(sp->min_lod); return 1;
    case GL_TEXTURE_MAX_LOD: PUT(sp->max_lod); return 1;
    case GL_TEXTURE_LOD_BIAS: PUT(sp->lod_bias); return 1;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: PUT(sp->max_aniso); return 1;
    case GL_TEXTURE_COMPARE_MODE: PUT(sp->compare_mode); return 1;
    case GL_TEXTURE_COMPARE_FUNC: PUT(sp->compare_func); return 1;
    case GL_TEXTURE_BORDER_COLOR:
        for (int k = 0; k < 4; k++) {
            if (iv && int_border) iv[k] = sp->border.i[k];
            else if (iv) iv[k] = (GLint)(sp->border.f[k] * 2147483647.0f);
            else fv[k] = sp->border.f[k];
        }
        return 1;
    }
#undef PUT
    return 0;
}

static void get_tex_param(GLenum target, GLenum pname, GLint *iv, GLfloat *fv, int int_border)
{
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (get_sampler_param(&t->sp, pname, iv, fv, int_border)) return;
    GLint x;
    switch (pname) {
    case GL_TEXTURE_BASE_LEVEL: x = t->base_level; break;
    case GL_TEXTURE_MAX_LEVEL: x = t->max_level; break;
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A: x = t->swizzle[pname - GL_TEXTURE_SWIZZLE_R]; break;
    case GL_TEXTURE_SWIZZLE_RGBA: for (int k = 0; k < 4; k++) { if (iv) iv[k] = t->swizzle[k]; else fv[k] = (GLfloat)t->swizzle[k]; } return;
    case GL_TEXTURE_IMMUTABLE_FORMAT: x = t->immutable; break;
    case 0x82DF /* GL_TEXTURE_IMMUTABLE_LEVELS */: x = t->immutable ? t->levels : 0; break;
    case GL_GENERATE_MIPMAP: x = t->generate_mipmap; break;
    case GL_TEXTURE_PRIORITY: x = 1; break;
    case GL_TEXTURE_RESIDENT: x = 1; break;
    default: zgl_set_error(GL_INVALID_ENUM); return;
    }
    if (iv) iv[0] = x; else fv[0] = (GLfloat)x;
}

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *v) { CTX(); get_tex_param(target, pname, v, NULL, 0); }
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *v) { CTX(); get_tex_param(target, pname, NULL, v, 0); }
void glGetTexParameterIiv(GLenum target, GLenum pname, GLint *v) { CTX(); get_tex_param(target, pname, v, NULL, 1); }
void glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint *v) { CTX(); get_tex_param(target, pname, (GLint *)v, NULL, 1); }

static GLint channel_bits(const struct zgl_format *f, int comp)
{
    if (!f) return 0;
    const struct layout *l = layout_of(f->vformat);
    if (!l) return 8;
    if (comp >= f->comps && comp < 4) return 0;
    switch (l->lay) {
    case L_ARRAY: for (int i = 0; i < l->n; i++) if (l->order[i] == comp) return l->bits; return 0;
    case L_R11G11B10F: return comp < 2 ? 11 : comp == 2 ? 10 : 0;
    case L_RGB9E5: return comp < 3 ? 9 : 0;
    case L_RGB10A2: case L_RGB10A2UI: return comp < 3 ? 10 : 2;
    }
    return 0;
}

static void get_level_param(GLenum target, GLint level, GLenum pname, GLint *iv, GLfloat *fv)
{
    int k = zgl_target_index(target);
    if (k < 0) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl->unit_tex[zgl->active_unit][k];
    GLint x = 0;
    int w = 0, h = 0, d = 0;
    if (t && t->res && level >= 0 && level < t->levels && (t->defined & (1u << level))) {
        level_size(t, level, &w, &h, &d);
        if (t->target == GL_TEXTURE_CUBE_MAP) d = 1;
        if (t->target == GL_TEXTURE_1D || t->target == GL_TEXTURE_1D_ARRAY) { if (t->target == GL_TEXTURE_1D) h = 1; d = 1; }
        if (t->target != GL_TEXTURE_3D && t->target != GL_TEXTURE_2D_ARRAY && t->target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY) d = 1;
    } else if (t && t->target == GL_TEXTURE_BUFFER && t->buffer && t->fmt) {
        w = (int)(t->buf_size / t->fmt->bpp); h = 1; d = 1;
    }
    const struct zgl_format *f = w ? t->fmt : NULL;
    switch (pname) {
    case GL_TEXTURE_WIDTH: x = w; break;
    case GL_TEXTURE_HEIGHT: x = h; break;
    case GL_TEXTURE_DEPTH: x = d; break;
    case GL_TEXTURE_INTERNAL_FORMAT: x = f ? (GLint)f->internal : 1; break;
    case GL_TEXTURE_RED_SIZE: x = f && !is_depth_kind(f->kind) ? channel_bits(f, 0) : 0; break;
    case GL_TEXTURE_GREEN_SIZE: x = f && !is_depth_kind(f->kind) ? channel_bits(f, 1) : 0; break;
    case GL_TEXTURE_BLUE_SIZE: x = f && !is_depth_kind(f->kind) ? channel_bits(f, 2) : 0; break;
    case GL_TEXTURE_ALPHA_SIZE: x = f && !is_depth_kind(f->kind) ? channel_bits(f, 3) : 0; break;
    case GL_TEXTURE_DEPTH_SIZE: x = f && (f->kind == ZF_DEPTH || f->kind == ZF_DEPTH_STENCIL) ? (f->internal == GL_DEPTH_COMPONENT16 ? 16 : f->internal == GL_DEPTH_COMPONENT32F || f->internal == GL_DEPTH32F_STENCIL8 ? 32 : 24) : 0; break;
    case GL_TEXTURE_STENCIL_SIZE: x = f && (f->kind == ZF_DEPTH_STENCIL || f->kind == ZF_STENCIL) ? 8 : 0; break;
    case GL_TEXTURE_SHARED_SIZE: x = f && f->internal == GL_RGB9_E5 ? 5 : 0; break;
    case GL_TEXTURE_COMPRESSED: x = f && f->kind == ZF_COMPRESSED; break;
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
        if (!f || f->kind != ZF_COMPRESSED) { zgl_set_error(GL_INVALID_OPERATION); return; }
        x = ((w + 3) / 4) * ((h + 3) / 4) * f->bpp * d; break;
    case GL_TEXTURE_SAMPLES: x = t ? t->samples : 0; break;
    case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS: x = t ? t->fixed_locations : 1; break;
    case GL_TEXTURE_RED_TYPE: case GL_TEXTURE_GREEN_TYPE: case GL_TEXTURE_BLUE_TYPE: case GL_TEXTURE_ALPHA_TYPE: case GL_TEXTURE_DEPTH_TYPE: {
        int comp = pname == GL_TEXTURE_DEPTH_TYPE ? -1 : (int)(pname - GL_TEXTURE_RED_TYPE);
        if (!f || (comp >= 0 && (is_depth_kind(f->kind) || !channel_bits(f, comp))) || (comp < 0 && !is_depth_kind(f->kind))) { x = GL_NONE; break; }
        switch (f->kind) {
        case ZF_UNORM: case ZF_SRGB: case ZF_COMPRESSED: x = GL_UNSIGNED_NORMALIZED; break;
        case ZF_SNORM: x = GL_SIGNED_NORMALIZED; break;
        case ZF_FLOAT: x = GL_FLOAT; break;
        case ZF_INT: x = GL_INT; break;
        case ZF_UINT: x = GL_UNSIGNED_INT; break;
        default: x = f->internal == GL_DEPTH_COMPONENT32F || f->internal == GL_DEPTH32F_STENCIL8 ? GL_FLOAT : GL_UNSIGNED_NORMALIZED;
        }
        break;
    }
    case GL_TEXTURE_BUFFER_DATA_STORE_BINDING: x = t && t->buffer ? (GLint)t->buffer->name : 0; break;
    case GL_TEXTURE_BORDER: x = 0; break;
    default: zgl_set_error(GL_INVALID_ENUM); return;
    }
    if (iv) iv[0] = x; else fv[0] = (GLfloat)x;
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *v) { CTX(); get_level_param(target, level, pname, v, NULL); }
void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *v) { CTX(); get_level_param(target, level, pname, NULL, v); }

/* ---- mipmaps, copies, readback ------------------------------------------------------------ */

void glGenerateMipmap(GLenum target)
{
    CTX();
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D && target != GL_TEXTURE_3D && target != GL_TEXTURE_CUBE_MAP &&
        target != GL_TEXTURE_1D_ARRAY && target != GL_TEXTURE_2D_ARRAY) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t || !t->res) return;
    if (t->fmt->kind == ZF_COMPRESSED || t->fmt->kind == ZF_INT || t->fmt->kind == ZF_UINT || is_depth_kind(t->fmt->kind)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int base = t->base_level < t->levels ? t->base_level : 0;
    int last = t->max_level < t->levels - 1 ? t->max_level : t->levels - 1;
    /* each level is a filtered blit of the one above (all layers or faces at once) */
    for (int l = base + 1; l <= last; l++) {
        int w, h, d, pw, ph, pd;
        level_size(t, l, &w, &h, &d);
        level_size(t, l - 1, &pw, &ph, &pd);
        struct virgl_blit_info b = { .mask = PIPE_MASK_RGBA, .filter = PIPE_TEX_FILTER_LINEAR };
        int layers = t->target == GL_TEXTURE_CUBE_MAP ? 6 : (t->target == GL_TEXTURE_3D ? 1 : d);
        if (t->target == GL_TEXTURE_1D_ARRAY) { layers = h; h = ph = 1; }
        b.dst = (struct virgl_blit_box){ t->res->id, (uint32_t)l, t->fmt->vformat, 0, 0, 0, w, h, t->target == GL_TEXTURE_3D ? d : layers };
        b.src = (struct virgl_blit_box){ t->res->id, (uint32_t)(l - 1), t->fmt->vformat, 0, 0, 0, pw, ph, t->target == GL_TEXTURE_3D ? pd : layers };
        virgl_blit(zgl->v, &b);
        t->defined |= 1u << l;
    }
}

/* the colour buffer being read: resource, level, layer, format, and whether it is the window (y-flipped on the host) */
int zgl_read_source(virgl_res **res, int *level, int *layer, const struct zgl_format **f);   /* fbo.c */

static void copy_tex(GLenum target, GLint level, int x0, int y0, int z0, GLint x, GLint y, GLsizei w, GLsizei h)
{
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (!t->res || level < 0 || level >= t->levels) { zgl_set_error(GL_INVALID_OPERATION); return; }
    virgl_res *src; int slevel, slayer; const struct zgl_format *sf;
    if (zgl_read_source(&src, &slevel, &slayer, &sf) != 0) return;
    if (t->target == GL_TEXTURE_CUBE_MAP) z0 = face_index(target);
    struct virgl_blit_info b = { .mask = is_depth_kind(t->fmt->kind) ? PIPE_MASK_Z : PIPE_MASK_RGBA, .filter = PIPE_TEX_FILTER_NEAREST };
    if (t->target == GL_TEXTURE_1D_ARRAY) { b.dst = (struct virgl_blit_box){ t->res->id, (uint32_t)level, t->fmt->vformat, x0, 0, y0, w, 1, h }; }
    else b.dst = (struct virgl_blit_box){ t->res->id, (uint32_t)level, t->fmt->vformat, x0, y0, z0, w, h, 1 };
    b.src = (struct virgl_blit_box){ src->id, (uint32_t)slevel, sf->vformat, x, y, slayer, w, h, 1 };
    zgl_apply_framebuffer();
    virgl_blit(zgl->v, &b);
}

void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoff, GLint x, GLint y, GLsizei w) { CTX(); copy_tex(target, level, xoff, 0, 0, x, y, w, 1); }
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); copy_tex(target, level, xoff, yoff, 0, x, y, w, h); }
void glCopyTexSubImage3D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint zoff, GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); copy_tex(target, level, xoff, yoff, zoff, x, y, w, h); }

static void copy_tex_image(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border, int dims)
{
    if (border) { zgl_set_error(GL_INVALID_VALUE); return; }
    virgl_res *src; int slevel, slayer; const struct zgl_format *sf;
    if (zgl_read_source(&src, &slevel, &slayer, &sf) != 0) return;
    GLenum fmt = is_depth_kind(sf->kind) ? GL_DEPTH_COMPONENT : GL_RGBA, type = is_depth_kind(sf->kind) ? GL_FLOAT : GL_UNSIGNED_BYTE;
    tex_image(target, level, (GLint)internal, w, h, 1, 0, fmt, type, NULL, dims);
    if (zgl->error == GL_NO_ERROR || 1) copy_tex(target, level, 0, 0, 0, x, y, w, h);
}

void glCopyTexImage1D(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLint border) { CTX(); copy_tex_image(target, level, internal, x, y, w, 1, border, 1); }
void glCopyTexImage2D(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) { CTX(); copy_tex_image(target, level, internal, x, y, w, h, border, 2); }

void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels)
{
    CTX();
    int k = zgl_target_index(target);
    if (k < 0 || k == ZT_BUFFER || k == ZT_2D_MS || k == ZT_2D_MS_ARRAY) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *t = zgl->unit_tex[zgl->active_unit][k];
    if (!t || !t->res || level < 0 || level >= t->levels) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (t->fmt->kind == ZF_COMPRESSED || !format_type_ok(format, type)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int w, h, d;
    level_size(t, level, &w, &h, &d);
    int z0 = 0;
    if (t->target == GL_TEXTURE_CUBE_MAP) { z0 = face_index(target); d = 1; }
    if (t->target != GL_TEXTURE_3D && t->target != GL_TEXTURE_2D_ARRAY && t->target != GL_TEXTURE_CUBE_MAP && t->target != GL_TEXTURE_1D_ARRAY) d = 1;
    if (t->target == GL_TEXTURE_1D) h = 1;
    struct zgl_pixel_store ps;
    zgl_pack_store(&ps);
    size_t need = zgl_pixels_size(w, h, d, format, type, &ps);
    uint8_t *dst = pixels;
    struct zgl_buffer *pb = zgl->pixel_pack_buffer;
    if (pb) {
        size_t off = (size_t)(uintptr_t)pixels;
        if (!pb->shadow || pb->mapped || off + need > pb->size) { zgl_set_error(GL_INVALID_OPERATION); return; }
        zgl_buffer_sync(pb);
        dst = pb->shadow + off;
    }
    size_t img = (size_t)w * h * t->fmt->bpp;
    uint8_t *all = malloc(img * d);
    if (!all) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
    for (int z = 0; z < d; z++) {
        uint8_t *rows = t->target == GL_TEXTURE_1D_ARRAY ? zgl_read_resource(t->res, t->fmt, level, 0, 0, z, w, 1)
                                                          : zgl_read_resource(t->res, t->fmt, level, 0, 0, z0 + z, w, h);
        if (!rows) { free(all); return; }
        memcpy(all + img * z, rows, t->target == GL_TEXTURE_1D_ARRAY ? (size_t)w * t->fmt->bpp : img);
        free(rows);
    }
    if (t->target == GL_TEXTURE_1D_ARRAY) { /* layers came one row each: they are the rows */ }
    zgl_pixels_from_format(t->fmt, w, t->target == GL_TEXTURE_1D_ARRAY ? h : h, t->target == GL_TEXTURE_1D_ARRAY ? 1 : d, all, format, type, &ps, dst);
    free(all);
    if (pb) zgl_buffer_write(pb, (size_t)(uintptr_t)pixels, dst, need);
}

void glGetCompressedTexImage(GLenum target, GLint level, void *img)
{
    CTX();
    struct zgl_texture *t = zgl_bound_texture(target);
    if (!t) return;
    if (!t->res || t->fmt->kind != ZF_COMPRESSED || level >= t->levels) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int w, h, d;
    level_size(t, level, &w, &h, &d);
    int z = t->target == GL_TEXTURE_CUBE_MAP ? face_index(target) : 0;
    uint8_t *rows = zgl_read_resource(t->res, t->fmt, level, 0, 0, z, w, h);
    if (!rows) return;
    memcpy(img, rows, (size_t)((w + 3) / 4) * ((h + 3) / 4) * t->fmt->bpp);
    free(rows);
}

/* ---- host objects for sampling ------------------------------------------------------------- */

static uint32_t gl_wrap(GLenum w)
{
    switch (w) {
    case GL_CLAMP_TO_EDGE: return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    case GL_CLAMP: return PIPE_TEX_WRAP_CLAMP;
    case GL_CLAMP_TO_BORDER: return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
    case GL_MIRRORED_REPEAT: return PIPE_TEX_WRAP_MIRROR_REPEAT;
    default: return PIPE_TEX_WRAP_REPEAT;
    }
}

static uint32_t pipe_compare(GLenum f)
{
    switch (f) {
    case GL_NEVER: return PIPE_FUNC_NEVER; case GL_LESS: return PIPE_FUNC_LESS; case GL_EQUAL: return PIPE_FUNC_EQUAL;
    case GL_LEQUAL: return PIPE_FUNC_LEQUAL; case GL_GREATER: return PIPE_FUNC_GREATER; case GL_NOTEQUAL: return PIPE_FUNC_NOTEQUAL;
    case GL_GEQUAL: return PIPE_FUNC_GEQUAL; default: return PIPE_FUNC_ALWAYS;
    }
}

static int is_mip_filter(GLenum f) { return f == GL_NEAREST_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR || f == GL_LINEAR_MIPMAP_LINEAR; }

/* Sampler states are shared: one per distinct set of parameters. */
uint32_t zgl_sampler_state(struct zgl_texture *t, struct zgl_sampler *s)
{
    const struct zgl_sampler_params *sp = s ? &s->sp : &t->sp;
    int mip = is_mip_filter(sp->min_filter) && t->levels > 1 && t->target != GL_TEXTURE_RECTANGLE;
    struct virgl_sampler_info si = { 0 };
    si.wrap_s = gl_wrap(sp->wrap_s); si.wrap_t = gl_wrap(sp->wrap_t); si.wrap_r = gl_wrap(sp->wrap_r);
    si.min_img = (sp->min_filter == GL_NEAREST || sp->min_filter == GL_NEAREST_MIPMAP_NEAREST || sp->min_filter == GL_NEAREST_MIPMAP_LINEAR) ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
    si.min_mip = !mip ? PIPE_TEX_MIPFILTER_NONE : (sp->min_filter == GL_NEAREST_MIPMAP_LINEAR || sp->min_filter == GL_LINEAR_MIPMAP_LINEAR) ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;
    si.mag_img = sp->mag_filter == GL_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
    /* integer textures can't be filtered */
    if (t->fmt && (t->fmt->kind == ZF_INT || t->fmt->kind == ZF_UINT)) { si.min_img = si.mag_img = PIPE_TEX_FILTER_NEAREST; if (mip) si.min_mip = PIPE_TEX_MIPFILTER_NEAREST; }
    si.compare_mode = sp->compare_mode == GL_COMPARE_REF_TO_TEXTURE ? 1 : 0;
    si.compare_func = pipe_compare(sp->compare_func);
    si.seamless_cube_map = zgl->cube_seamless;
    si.max_anisotropy = sp->max_aniso > 1 ? (uint32_t)(sp->max_aniso > 16 ? 16 : sp->max_aniso) : 0;
    si.lod_bias = sp->lod_bias;
    si.min_lod = sp->min_lod < 0 ? 0 : sp->min_lod;
    si.max_lod = mip ? sp->max_lod : 0;
    memcpy(si.border, sp->border.i, 16);
    /* the cache key: everything above */
    uint64_t key = 1469598103934665603ull;
    const uint8_t *kb = (const uint8_t *)&si;
    for (size_t i = 0; i < sizeof si; i++) key = (key ^ kb[i]) * 1099511628211ull;
    for (int i = 0; i < zgl->nsampler_cache; i++) if (zgl->sampler_cache[i].key == key) return zgl->sampler_cache[i].h;
    uint32_t h = virgl_handle(zgl->v);
    virgl_create_sampler_state_ex(zgl->v, h, &si);
    if (zgl->nsampler_cache < 64) { zgl->sampler_cache[zgl->nsampler_cache].key = key; zgl->sampler_cache[zgl->nsampler_cache].h = h; zgl->nsampler_cache++; }
    else {                                              /* full: replace the oldest */
        virgl_destroy_object(zgl->v, zgl->sampler_cache[0].h, VIRGL_OBJECT_SAMPLER_STATE);
        memmove(&zgl->sampler_cache[0], &zgl->sampler_cache[1], sizeof zgl->sampler_cache[0] * 63);
        zgl->sampler_cache[63].key = key; zgl->sampler_cache[63].h = h;
    }
    return h;
}

static uint32_t pipe_swizzle(GLint s, int comp)
{
    switch (s) {
    case GL_RED: return PIPE_SWIZZLE_X; case GL_GREEN: return PIPE_SWIZZLE_Y; case GL_BLUE: return PIPE_SWIZZLE_Z; case GL_ALPHA: return PIPE_SWIZZLE_W;
    case GL_ZERO: return PIPE_SWIZZLE_0; case GL_ONE: return PIPE_SWIZZLE_1;
    }
    return (uint32_t)comp;
}

uint32_t zgl_texture_view(struct zgl_texture *t)
{
    int srgb = !zgl->framebuffer_srgb;                  /* sRGB decode on sampling is always on in GL; the flag is for writes */
    (void)srgb;
    if (t->h_view) return t->h_view;
    uint32_t sw[4];
    for (int i = 0; i < 4; i++) {
        sw[i] = pipe_swizzle(t->swizzle[i], i);
        /* an RGB format kept in RGBA: alpha reads as one */
        if (t->fmt && t->fmt->comps == 3 && sw[i] == PIPE_SWIZZLE_W) sw[i] = PIPE_SWIZZLE_1;
        /* missing channels of R and RG formats read as 0 (and alpha as 1): what the host does already */
    }
    uint32_t swizzle = VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_R(sw[0]) | VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_G(sw[1]) |
                       VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_B(sw[2]) | VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_A(sw[3]);
    t->h_view = virgl_handle(zgl->v);
    if (t->target == GL_TEXTURE_BUFFER) {
        virgl_create_sampler_view_ex(zgl->v, t->h_view, t->res->id, PIPE_TEXTURE_2D, tbo_vformat(t->fmt), 0, 0, 0, 0, VIRGL_SWIZZLE_IDENTITY);
        return t->h_view;
    }
    int mip = is_mip_filter(t->sp.min_filter) && t->target != GL_TEXTURE_RECTANGLE;
    uint32_t first = (uint32_t)(t->base_level < t->levels ? t->base_level : 0);
    uint32_t last = mip ? (uint32_t)(t->max_level < t->levels - 1 ? t->max_level : t->levels - 1) : first;
    if (last < first) last = first;
    uint32_t layers = 0;
    if (t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY || t->target == 0x9009) layers = (uint32_t)t->d - 1;
    else if (t->target == GL_TEXTURE_1D_ARRAY) layers = (uint32_t)t->h - 1;
    else if (t->target == GL_TEXTURE_CUBE_MAP) layers = 5;
    uint32_t vf = t->fmt->vformat;
    /* depth-stencil textures sample their depth */
    virgl_create_sampler_view_ex(zgl->v, t->h_view, t->res->id, pipe_target(t->target, t->samples), vf, 0, layers, first, last, swizzle);
    return t->h_view;
}

/* complete enough to sample: the base level exists (and the levels the filter needs, if they were never made, are read as whatever the host has) */
int zgl_texture_complete(struct zgl_texture *t)
{
    if (t->target == GL_TEXTURE_BUFFER) return zgl_tbo_update(t) == 0;
    return t->res && t->has_level0;
}

/* ---- sampler objects (3.3) ----------------------------------------------------------------- */

static struct zgl_sampler *sampler_get(GLuint name)
{
    for (int i = 0; i < zgl->nsamplers; i++) if (zgl->samplers[i]->name == name) return zgl->samplers[i];
    return NULL;
}

void glGenSamplers(GLsizei n, GLuint *s)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_sampler *o = calloc(1, sizeof *o);
        o->name = zgl->next_name++;
        default_sampler_params(&o->sp, 0);
        LIST_ADD(zgl->samplers, zgl->nsamplers, o);
        s[i] = o->name;
    }
}

void glDeleteSamplers(GLsizei n, const GLuint *s)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_sampler *o = s[i] ? sampler_get(s[i]) : NULL;
        if (!o) continue;
        for (int u = 0; u < ZGL_MAX_UNITS; u++) if (zgl->unit_sampler[u] == o) zgl->unit_sampler[u] = NULL;
        LIST_DEL(zgl->samplers, zgl->nsamplers, o);
        free(o);
    }
}

GLboolean glIsSampler(GLuint s) { CTXV(0); return s && sampler_get(s) != NULL; }

void glBindSampler(GLuint unit, GLuint s)
{
    CTX();
    if (unit >= ZGL_MAX_UNITS) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_sampler *o = s ? sampler_get(s) : NULL;
    if (s && !o) { zgl_set_error(GL_INVALID_OPERATION); return; }
    zgl->unit_sampler[unit] = o;
}

static void samp_param(GLuint s, GLenum pname, const GLint *iv, const GLfloat *fv, int int_border)
{
    struct zgl_sampler *o = sampler_get(s);
    if (!o) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int ok;
    sampler_param(&o->sp, pname, iv, fv, int_border, &ok);
    if (!ok) zgl_set_error(GL_INVALID_ENUM);
}

void glSamplerParameteri(GLuint s, GLenum pname, GLint p) { CTX(); samp_param(s, pname, &p, NULL, 0); }
void glSamplerParameteriv(GLuint s, GLenum pname, const GLint *p) { CTX(); samp_param(s, pname, p, NULL, 0); }
void glSamplerParameterf(GLuint s, GLenum pname, GLfloat p) { CTX(); samp_param(s, pname, NULL, &p, 0); }
void glSamplerParameterfv(GLuint s, GLenum pname, const GLfloat *p) { CTX(); samp_param(s, pname, NULL, p, 0); }
void glSamplerParameterIiv(GLuint s, GLenum pname, const GLint *p) { CTX(); samp_param(s, pname, p, NULL, 1); }
void glSamplerParameterIuiv(GLuint s, GLenum pname, const GLuint *p) { CTX(); samp_param(s, pname, (const GLint *)p, NULL, 1); }

static void get_samp_param(GLuint s, GLenum pname, GLint *iv, GLfloat *fv, int int_border)
{
    struct zgl_sampler *o = sampler_get(s);
    if (!o) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (!get_sampler_param(&o->sp, pname, iv, fv, int_border)) zgl_set_error(GL_INVALID_ENUM);
}

void glGetSamplerParameteriv(GLuint s, GLenum pname, GLint *p) { CTX(); get_samp_param(s, pname, p, NULL, 0); }
void glGetSamplerParameterIiv(GLuint s, GLenum pname, GLint *p) { CTX(); get_samp_param(s, pname, p, NULL, 1); }
void glGetSamplerParameterfv(GLuint s, GLenum pname, GLfloat *p) { CTX(); get_samp_param(s, pname, NULL, p, 0); }
void glGetSamplerParameterIuiv(GLuint s, GLenum pname, GLuint *p) { CTX(); get_samp_param(s, pname, (GLint *)p, NULL, 1); }

/* ---- zgl's own: textures on another program's resource, XRGB uploads ---------------------- */

int zglTextureFromResource(GLuint name, uint32_t res, int w, int h)
{
    if (!zgl) return -1;
    struct zgl_texture *t = zgl_texture_get(name);
    if (!t) { t = texture_new(name); if (name >= zgl->next_name) zgl->next_name = name + 1; }
    if (!t->target) { t->target = GL_TEXTURE_2D; default_sampler_params(&t->sp, GL_TEXTURE_2D); }
    texture_drop_objects(t);
    if (t->res) { virgl_res_destroy(zgl->v, t->res); t->res = NULL; }
    t->res = virgl_res_attach(zgl->v, res, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, (uint32_t)w, (uint32_t)h);
    if (!t->res) return -1;
    t->fmt = find_format(GL_RGBA8);
    t->w = w; t->h = h; t->d = 1; t->levels = 1; t->has_level0 = 1; t->defined = 1;
    return 0;
}

void zglTexSubImageXRGB(GLint x, GLint y, GLsizei w, GLsizei h, const uint32_t *pixels, int stride)
{
    CTX();
    struct zgl_texture *t = zgl->unit_tex[zgl->active_unit][ZT_2D];
    if (!t || !t->res || w <= 0 || h <= 0) return;
    /* B8G8R8A8 in memory is 0xAARRGGBB little-endian: the pixels as they
     * are, with the alpha byte set; in chunks the command stream can take */
    size_t row = (size_t)w * 4;
    int rows = (int)((48 * 1024) / row);
    if (rows < 1) rows = 1;
    uint32_t *buf = malloc(row * (size_t)(h < rows ? h : rows));
    if (!buf) return;
    for (int yy = 0; yy < h; yy += rows) {
        int n = h - yy < rows ? h - yy : rows;
        for (int j = 0; j < n; j++) {
            const uint32_t *src = pixels + (size_t)(yy + j) * stride;
            uint32_t *dst = buf + (size_t)j * w;
            for (int i = 0; i < w; i++) dst[i] = src[i] | 0xFF000000u;
        }
        virgl_inline_write(zgl->v, t->res->id, 0, 0, (uint32_t)row, (uint32_t)(row * n),
                           (uint32_t)x, (uint32_t)(y + yy), 0, (uint32_t)w, (uint32_t)n, 1, buf, row * n);
    }
    free(buf);
}
