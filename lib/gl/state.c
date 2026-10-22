/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Context state: enables, the simple setters, pixel storage, every glGet,
 * the version and extension strings; sync objects, queries, conditional
 * rendering and transform feedback. */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CTX() do { if (!zgl) return; } while (0)
#define CTXV(v) do { if (!zgl) return (v); } while (0)
#define LIST_ADD(list, n, item) do { (list) = realloc((list), sizeof *(list) * ((n) + 1)); (list)[(n)++] = (item); } while (0)
#define LIST_DEL(list, n, item) do { for (int _i = 0; _i < (n); _i++) if ((list)[_i] == (item)) { memmove(&(list)[_i], &(list)[_i + 1], sizeof *(list) * ((n) - _i - 1)); (n)--; break; } } while (0)

void zgl_set_error(GLenum e) { if (zgl && zgl->error == GL_NO_ERROR) zgl->error = e; }
GLenum glGetError(void) { CTXV(GL_NO_ERROR); GLenum e = zgl->error; zgl->error = GL_NO_ERROR; return e; }

/* ---- strings ------------------------------------------------------------------------- */

/* what zgl does beyond 3.1, said the way programs look for it */
static const char *const extensions[] = {
    "GL_ARB_vertex_buffer_object", "GL_ARB_shader_objects", "GL_ARB_vertex_shader", "GL_ARB_fragment_shader",
    "GL_ARB_texture_non_power_of_two", "GL_EXT_framebuffer_object", "GL_ARB_framebuffer_object", "GL_EXT_framebuffer_blit",
    "GL_EXT_framebuffer_multisample", "GL_EXT_packed_depth_stencil", "GL_ARB_depth_buffer_float", "GL_ARB_texture_float",
    "GL_ARB_half_float_pixel", "GL_ARB_half_float_vertex", "GL_ARB_texture_rg", "GL_EXT_texture_integer", "GL_EXT_texture_array",
    "GL_ARB_texture_rectangle", "GL_ARB_texture_buffer_object", "GL_ARB_uniform_buffer_object", "GL_ARB_vertex_array_object",
    "GL_ARB_map_buffer_range", "GL_ARB_copy_buffer", "GL_ARB_draw_instanced", "GL_ARB_instanced_arrays",
    "GL_ARB_draw_elements_base_vertex", "GL_ARB_provoking_vertex", "GL_ARB_sync", "GL_ARB_sampler_objects",
    "GL_ARB_explicit_attrib_location", "GL_ARB_texture_swizzle", "GL_ARB_timer_query", "GL_ARB_occlusion_query2",
    "GL_ARB_blend_func_extended", "GL_ARB_texture_storage", "GL_ARB_texture_rgb10_a2ui", "GL_ARB_depth_clamp",
    "GL_ARB_seamless_cube_map", "GL_ARB_texture_multisample", "GL_ARB_fragment_coord_conventions",
    "GL_EXT_texture_sRGB", "GL_EXT_framebuffer_sRGB", "GL_ARB_framebuffer_sRGB", "GL_EXT_texture_filter_anisotropic",
    "GL_EXT_texture_compression_s3tc", "GL_ARB_texture_compression_rgtc", "GL_EXT_bgra", "GL_EXT_texture_swizzle",
    "GL_EXT_transform_feedback", "GL_NV_conditional_render", "GL_ARB_shader_bit_encoding", "GL_EXT_draw_buffers2",
    "GL_ARB_draw_buffers", "GL_ARB_pixel_buffer_object", "GL_ARB_texture_cube_map", "GL_ARB_occlusion_query",
};
#define NEXT ((GLuint)(sizeof extensions / sizeof extensions[0]))

static char renderer_str[128], ext_str[2048];
const GLubyte *glGetString(GLenum name)
{
    CTXV((const GLubyte *)"");
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"Rigby Foundation";
    case GL_RENDERER: {
        const struct virgl_caps_v2 *caps = virgl_caps(zgl->v);
        snprintf(renderer_str, sizeof renderer_str, "zgl on virgl (%.64s)", caps ? caps->renderer : "unknown host GPU");
        return (const GLubyte *)renderer_str;
    }
    case GL_VERSION: return (const GLubyte *)"3.1 zgl";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"1.40";
    case GL_EXTENSIONS:
        if (!ext_str[0]) {
            size_t n = 0;
            for (GLuint i = 0; i < NEXT; i++) n += (size_t)snprintf(ext_str + n, sizeof ext_str - n, "%s%s", i ? " " : "", extensions[i]);
        }
        return (const GLubyte *)ext_str;
    default: zgl_set_error(GL_INVALID_ENUM); return NULL;
    }
}

const GLubyte *glGetStringi(GLenum name, GLuint index)
{
    CTXV(NULL);
    if (name != GL_EXTENSIONS) { zgl_set_error(GL_INVALID_ENUM); return NULL; }
    if (index >= NEXT) { zgl_set_error(GL_INVALID_VALUE); return NULL; }
    return (const GLubyte *)extensions[index];
}

/* ---- enables -------------------------------------------------------------------------- */

static int *enable_slot(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST: return &zgl->depth_test;
    case GL_BLEND: return &zgl->blend[0];
    case GL_CULL_FACE: return &zgl->cull;
    case GL_SCISSOR_TEST: return &zgl->scissor;
    case GL_ALPHA_TEST: return &zgl->alpha_test;
    case GL_TEXTURE_2D: return &zgl->texture_2d[zgl->active_unit];
    case GL_LIGHTING: return &zgl->lighting;
    case GL_COLOR_MATERIAL: return &zgl->color_material;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: return &zgl->normalize_n;
    case GL_STENCIL_TEST: return &zgl->stencil_test;
    case GL_POLYGON_OFFSET_FILL: return &zgl->polygon_offset;
    case GL_POLYGON_OFFSET_LINE: return &zgl->polygon_offset_line;
    case GL_POLYGON_OFFSET_POINT: return &zgl->polygon_offset_point;
    case GL_DEPTH_CLAMP: return &zgl->depth_clamp;
    case GL_RASTERIZER_DISCARD: return &zgl->rasterizer_discard;
    case GL_PRIMITIVE_RESTART: return &zgl->primitive_restart;
    case GL_PROGRAM_POINT_SIZE: return &zgl->program_point_size;
    case GL_FRAMEBUFFER_SRGB: return &zgl->framebuffer_srgb;
    case GL_MULTISAMPLE: return &zgl->multisample;
    case GL_SAMPLE_ALPHA_TO_COVERAGE: return &zgl->sample_alpha_to_coverage;
    case GL_SAMPLE_ALPHA_TO_ONE: return &zgl->sample_alpha_to_one;
    case GL_SAMPLE_COVERAGE: return &zgl->sample_coverage_enabled;
    case GL_SAMPLE_MASK: return &zgl->sample_mask_enabled;
    case GL_LINE_SMOOTH: return &zgl->line_smooth;
    case GL_POLYGON_SMOOTH: return &zgl->polygon_smooth;
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: return &zgl->cube_seamless;
    case GL_DITHER: return &zgl->dither;
    case GL_COLOR_LOGIC_OP: return &zgl->color_logic_op;
    default: break;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + ZGL_MAX_LIGHTS) return &zgl->lights[cap - GL_LIGHT0].enabled;
    return NULL;
}

static int harmless_cap(GLenum cap)
{
    switch (cap) {
    case GL_FOG: case GL_POINT_SMOOTH: case GL_LINE_STIPPLE: case GL_POLYGON_STIPPLE: case GL_POINT_SPRITE: case GL_TEXTURE_1D:
    case GL_TEXTURE_3D: case GL_TEXTURE_CUBE_MAP: case GL_TEXTURE_RECTANGLE: case GL_CLIP_DISTANCE0: case GL_CLIP_DISTANCE1:
    case GL_CLIP_DISTANCE2: case GL_CLIP_DISTANCE3: case GL_CLIP_DISTANCE4: case GL_CLIP_DISTANCE5: case GL_CLIP_DISTANCE6:
    case GL_CLIP_DISTANCE7: case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q:
    case GL_AUTO_NORMAL: case GL_COLOR_SUM:
        return 1;
    }
    return 0;
}

static void set_cap(GLenum cap, int on)
{
    if (cap >= GL_CLIP_DISTANCE0 && cap <= GL_CLIP_DISTANCE7) {
        uint32_t bit = 1u << (cap - GL_CLIP_DISTANCE0);
        zgl->clip_mask = on ? zgl->clip_mask | bit : zgl->clip_mask & ~bit;
        return;
    }
    int *s = enable_slot(cap);
    if (!s) { if (!harmless_cap(cap)) zgl_set_error(GL_INVALID_ENUM); return; }
    if (cap == GL_BLEND) { for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS; i++) zgl->blend[i] = on; return; }
    if (*s == on) return;
    *s = on;
    if (cap == GL_SCISSOR_TEST) zgl->vp_dirty = 1;
    if (cap == GL_FRAMEBUFFER_SRGB) zgl->fb_dirty = 1;
    if (cap == GL_TEXTURE_CUBE_MAP_SEAMLESS) for (int i = 0; i < zgl->ntextures; i++) zgl->textures[i]->h_sampler = 0;
}

void glEnable(GLenum cap) { CTX(); set_cap(cap, 1); }
void glDisable(GLenum cap) { CTX(); set_cap(cap, 0); }
GLboolean glIsEnabled(GLenum cap) { CTXV(0); if (cap >= GL_CLIP_DISTANCE0 && cap <= GL_CLIP_DISTANCE7) return (zgl->clip_mask >> (cap - GL_CLIP_DISTANCE0)) & 1; int *s = enable_slot(cap); if (!s && !harmless_cap(cap)) zgl_set_error(GL_INVALID_ENUM); return s ? (*s != 0) : 0; }

void glEnablei(GLenum cap, GLuint i)
{
    CTX();
    if (cap != GL_BLEND) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (i >= ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return; }
    zgl->blend[i] = 1;
}
void glDisablei(GLenum cap, GLuint i)
{
    CTX();
    if (cap != GL_BLEND) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (i >= ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return; }
    zgl->blend[i] = 0;
}
GLboolean glIsEnabledi(GLenum cap, GLuint i)
{
    CTXV(0);
    if (cap != GL_BLEND) { zgl_set_error(GL_INVALID_ENUM); return 0; }
    if (i >= ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return 0; }
    return (GLboolean)zgl->blend[i];
}

/* ---- simple state ---------------------------------------------------------------------- */

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX(); zgl->clear_color[0] = r; zgl->clear_color[1] = g; zgl->clear_color[2] = b; zgl->clear_color[3] = a; }
void glClearDepth(GLclampd d) { CTX(); zgl->clear_depth = d < 0 ? 0 : d > 1 ? 1 : d; }
void glClearStencil(GLint s) { CTX(); zgl->clear_stencil = s; }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); if (w < 0 || h < 0) { zgl_set_error(GL_INVALID_VALUE); return; } zgl->vp_x = x; zgl->vp_y = y; zgl->vp_w = w; zgl->vp_h = h; zgl->vp_dirty = 1; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); if (w < 0 || h < 0) { zgl_set_error(GL_INVALID_VALUE); return; } zgl->sc_x = x; zgl->sc_y = y; zgl->sc_w = w; zgl->sc_h = h; zgl->vp_dirty = 1; }
void glDepthRange(GLclampd n, GLclampd f) { CTX(); zgl->depth_near = (float)(n < 0 ? 0 : n > 1 ? 1 : n); zgl->depth_far = (float)(f < 0 ? 0 : f > 1 ? 1 : f); zgl->vp_dirty = 1; }
static int is_compare(GLenum f) { return f >= GL_NEVER && f <= GL_ALWAYS; }
void glDepthFunc(GLenum f) { CTX(); if (!is_compare(f)) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->depth_func = f; }
void glDepthMask(GLboolean m) { CTX(); zgl->depth_mask = m != 0; }

static int blend_factor_ok(GLenum f)
{
    switch (f) {
    case GL_ZERO: case GL_ONE: case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR: case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
    case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA: case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA: case GL_CONSTANT_COLOR:
    case GL_ONE_MINUS_CONSTANT_COLOR: case GL_CONSTANT_ALPHA: case GL_ONE_MINUS_CONSTANT_ALPHA: case GL_SRC_ALPHA_SATURATE:
    case GL_SRC1_COLOR: case GL_ONE_MINUS_SRC1_COLOR: case GL_SRC1_ALPHA: case GL_ONE_MINUS_SRC1_ALPHA:
        return 1;
    }
    return 0;
}
static int blend_eq_ok(GLenum e) { return e == GL_FUNC_ADD || e == GL_FUNC_SUBTRACT || e == GL_FUNC_REVERSE_SUBTRACT || e == GL_MIN || e == GL_MAX; }

void glBlendFunc(GLenum s, GLenum d) { CTX(); if (!blend_factor_ok(s) || !blend_factor_ok(d)) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->blend_src = zgl->blend_src_a = s; zgl->blend_dst = zgl->blend_dst_a = d; }
void glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da)
{
    CTX();
    if (!blend_factor_ok(s) || !blend_factor_ok(d) || !blend_factor_ok(sa) || !blend_factor_ok(da)) { zgl_set_error(GL_INVALID_ENUM); return; }
    zgl->blend_src = s; zgl->blend_dst = d; zgl->blend_src_a = sa; zgl->blend_dst_a = da;
}
void glBlendEquation(GLenum m) { CTX(); if (!blend_eq_ok(m)) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->blend_eq = zgl->blend_eq_a = m; }
void glBlendEquationSeparate(GLenum m, GLenum ma) { CTX(); if (!blend_eq_ok(m) || !blend_eq_ok(ma)) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->blend_eq = m; zgl->blend_eq_a = ma; }
void glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX(); zgl->blend_color[0] = r; zgl->blend_color[1] = g; zgl->blend_color[2] = b; zgl->blend_color[3] = a; virgl_set_blend_color(zgl->v, zgl->blend_color); }
void glAlphaFunc(GLenum f, GLclampf ref) { CTX(); zgl->alpha_func = f; zgl->alpha_ref = ref; }
void glCullFace(GLenum m) { CTX(); if (m != GL_FRONT && m != GL_BACK && m != GL_FRONT_AND_BACK) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->cull_mode = m; }
void glFrontFace(GLenum m) { CTX(); if (m != GL_CW && m != GL_CCW) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->front_face = m; }
void glPolygonMode(GLenum face, GLenum m) { CTX(); if (face != GL_FRONT_AND_BACK && face != GL_FRONT && face != GL_BACK) { zgl_set_error(GL_INVALID_ENUM); return; } if (m != GL_POINT && m != GL_LINE && m != GL_FILL) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->polygon_mode = m; }
void glPolygonOffset(GLfloat f, GLfloat u) { CTX(); zgl->po_factor = f; zgl->po_units = u; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    CTX();
    int m = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
    for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS; i++) zgl->color_masks[i] = m;
}
void glColorMaski(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    CTX();
    if (i >= ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return; }
    zgl->color_masks[i] = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
}
void glLineWidth(GLfloat w) { CTX(); if (w <= 0) { zgl_set_error(GL_INVALID_VALUE); return; } zgl->line_width = w; }
void glPointSize(GLfloat s) { CTX(); if (s <= 0) { zgl_set_error(GL_INVALID_VALUE); return; } zgl->point_size = s; }
void glShadeModel(GLenum m) { CTX(); zgl->shade_model = m; }
void glHint(GLenum t, GLenum m) { CTX(); (void)t; if (m != GL_FASTEST && m != GL_NICEST && m != GL_DONT_CARE) zgl_set_error(GL_INVALID_ENUM); }
void glLogicOp(GLenum op) { CTX(); if (op < GL_CLEAR || op > GL_SET) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->logic_op = op; }
void glProvokingVertex(GLenum m) { CTX(); if (m != GL_FIRST_VERTEX_CONVENTION && m != GL_LAST_VERTEX_CONVENTION) { zgl_set_error(GL_INVALID_ENUM); return; } zgl->provoking_vertex = m; }
void glPrimitiveRestartIndex(GLuint i) { CTX(); zgl->restart_index = i; }
void glClampColor(GLenum target, GLenum clamp)
{
    CTX();
    if (target != GL_CLAMP_READ_COLOR) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (clamp != GL_TRUE && clamp != GL_FALSE && clamp != GL_FIXED_ONLY) zgl_set_error(GL_INVALID_ENUM);
}
void glSampleCoverage(GLfloat value, GLboolean invert) { CTX(); zgl->sample_coverage = value < 0 ? 0 : value > 1 ? 1 : value; zgl->sample_coverage_invert = invert != 0; }
void glSampleMaski(GLuint index, GLbitfield mask) { CTX(); if (index != 0) { zgl_set_error(GL_INVALID_VALUE); return; } zgl->sample_mask = mask; }
void glPointParameterf(GLenum pname, GLfloat p) { glPointParameterfv(pname, &p); }
void glPointParameterfv(GLenum pname, const GLfloat *p)
{
    CTX();
    if (pname == GL_POINT_FADE_THRESHOLD_SIZE) { if (p[0] < 0) zgl_set_error(GL_INVALID_VALUE); return; }
    if (pname == GL_POINT_SPRITE_COORD_ORIGIN) { if (p[0] != GL_LOWER_LEFT && p[0] != GL_UPPER_LEFT) zgl_set_error(GL_INVALID_VALUE); return; }
    zgl_set_error(GL_INVALID_ENUM);
}
void glPointParameteri(GLenum pname, GLint p) { GLfloat f = (GLfloat)p; glPointParameterfv(pname, &f); }
void glPointParameteriv(GLenum pname, const GLint *p) { GLfloat f = (GLfloat)p[0]; glPointParameterfv(pname, &f); }

static int stencil_op_ok(GLenum op) { return op == GL_KEEP || op == GL_ZERO || op == GL_REPLACE || op == GL_INCR || op == GL_DECR || op == GL_INVERT || op == GL_INCR_WRAP || op == GL_DECR_WRAP; }
static int face_mask(GLenum face) { return face == GL_FRONT ? 1 : face == GL_BACK ? 2 : face == GL_FRONT_AND_BACK ? 3 : 0; }

void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    CTX();
    int fm = face_mask(face);
    if (!fm || !is_compare(func)) { zgl_set_error(GL_INVALID_ENUM); return; }
    for (int i = 0; i < 2; i++) if (fm & (1 << i)) { zgl->stencil_func[i] = func; zgl->stencil_ref[i] = ref; zgl->stencil_vmask[i] = mask; }
}
void glStencilFunc(GLenum func, GLint ref, GLuint mask) { glStencilFuncSeparate(GL_FRONT_AND_BACK, func, ref, mask); }
void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass)
{
    CTX();
    int fm = face_mask(face);
    if (!fm || !stencil_op_ok(sfail) || !stencil_op_ok(dpfail) || !stencil_op_ok(dppass)) { zgl_set_error(GL_INVALID_ENUM); return; }
    for (int i = 0; i < 2; i++) if (fm & (1 << i)) { zgl->stencil_fail[i] = sfail; zgl->stencil_zfail[i] = dpfail; zgl->stencil_zpass[i] = dppass; }
}
void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) { glStencilOpSeparate(GL_FRONT_AND_BACK, sfail, dpfail, dppass); }
void glStencilMaskSeparate(GLenum face, GLuint mask)
{
    CTX();
    int fm = face_mask(face);
    if (!fm) { zgl_set_error(GL_INVALID_ENUM); return; }
    for (int i = 0; i < 2; i++) if (fm & (1 << i)) zgl->stencil_wmask[i] = mask;
}
void glStencilMask(GLuint mask) { glStencilMaskSeparate(GL_FRONT_AND_BACK, mask); }

void glPixelStorei(GLenum p, GLint v)
{
    CTX();
    if ((p == GL_UNPACK_ALIGNMENT || p == GL_PACK_ALIGNMENT) && v != 1 && v != 2 && v != 4 && v != 8) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (v < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    switch (p) {
    case GL_UNPACK_ALIGNMENT: zgl->unpack_alignment = v; break;
    case GL_UNPACK_ROW_LENGTH: zgl->unpack_row_length = v; break;
    case GL_UNPACK_IMAGE_HEIGHT: zgl->unpack_image_height = v; break;
    case GL_UNPACK_SKIP_ROWS: zgl->unpack_skip_rows = v; break;
    case GL_UNPACK_SKIP_PIXELS: zgl->unpack_skip_pixels = v; break;
    case GL_UNPACK_SKIP_IMAGES: zgl->unpack_skip_images = v; break;
    case GL_UNPACK_SWAP_BYTES: zgl->unpack_swap = v != 0; break;
    case GL_UNPACK_LSB_FIRST: zgl->unpack_lsb = v != 0; break;
    case GL_PACK_ALIGNMENT: zgl->pack_alignment = v; break;
    case GL_PACK_ROW_LENGTH: zgl->pack_row_length = v; break;
    case GL_PACK_IMAGE_HEIGHT: zgl->pack_image_height = v; break;
    case GL_PACK_SKIP_ROWS: zgl->pack_skip_rows = v; break;
    case GL_PACK_SKIP_PIXELS: zgl->pack_skip_pixels = v; break;
    case GL_PACK_SKIP_IMAGES: zgl->pack_skip_images = v; break;
    case GL_PACK_SWAP_BYTES: zgl->pack_swap = v != 0; break;
    case GL_PACK_LSB_FIRST: zgl->pack_lsb = v != 0; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}
void glPixelStoref(GLenum p, GLfloat v) { glPixelStorei(p, (GLint)lrintf(v)); }

void glClipPlane(GLenum plane, const GLdouble *equation)
{
    CTX();
    (void)equation;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + 6) zgl_set_error(GL_INVALID_ENUM);
    /* accepted so fixed-function code runs; clipping itself is not implemented */
}

void glFlush(void) { CTX(); virgl_flush(zgl->v); }
void glFinish(void) { CTX(); virgl_flush_fenced(zgl->v); }

/* ---- glGet -------------------------------------------------------------------------------- */

/* One answer: up to 16 values, integers or floats; *n how many. Returns 0 for an unknown pname. */
struct val16 { int is_float, n; double v[16]; };

static GLint name_of_tex(int k) { struct zgl_texture *t = zgl->unit_tex[zgl->active_unit][k]; return t ? (GLint)t->name : 0; }
static GLint bufname(struct zgl_buffer *b) { return b ? (GLint)b->name : 0; }

static int caps_max(int v, int fallback) { return v > 0 ? v : fallback; }

static int get_state(GLenum pname, struct val16 *r)
{
    const struct virgl_caps_v2 *caps = virgl_caps(zgl->v);
    r->is_float = 0; r->n = 1;
    double *v = r->v;
#define I(x) do { v[0] = (double)(x); return 1; } while (0)
#define F(x) do { r->is_float = 1; v[0] = (double)(x); return 1; } while (0)
    switch (pname) {
    /* the implementation */
    case GL_MAJOR_VERSION: I(3);
    case GL_MINOR_VERSION: I(1);
    case GL_CONTEXT_FLAGS: I(0);
    case GL_CONTEXT_PROFILE_MASK: I(GL_CONTEXT_COMPATIBILITY_PROFILE_BIT);
    case GL_NUM_EXTENSIONS: I(NEXT);
    case GL_MAX_TEXTURE_SIZE: I(caps ? caps_max((int)caps->max_texture_2d_size, 4096) : 4096);
    case GL_MAX_3D_TEXTURE_SIZE: I(caps ? caps_max((int)caps->max_texture_3d_size, 256) : 256);
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: I(caps ? caps_max((int)caps->max_texture_cube_size, 4096) : 4096);
    case GL_MAX_ARRAY_TEXTURE_LAYERS: I(caps ? caps_max((int)caps->v1.max_texture_array_layers, 256) : 256);
    case GL_MAX_RECTANGLE_TEXTURE_SIZE: I(caps ? caps_max((int)caps->max_texture_2d_size, 4096) : 4096);
    case GL_MAX_RENDERBUFFER_SIZE: I(caps ? caps_max((int)caps->max_texture_2d_size, 4096) : 4096);
    case GL_MAX_TEXTURE_BUFFER_SIZE: I(caps ? caps_max((int)caps->v1.max_tbo_size, 65536) : 65536);
    case GL_MAX_VIEWPORT_DIMS: r->n = 2; v[0] = v[1] = caps ? caps_max((int)caps->max_texture_2d_size, 4096) : 4096; return 1;
    case GL_MAX_TEXTURE_LOD_BIAS: F(caps ? (caps->max_texture_lod_bias > 0 ? caps->max_texture_lod_bias : 15) : 15);
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: F(caps && caps->max_anisotropy > 0 ? caps->max_anisotropy : 16);
    case GL_MAX_SAMPLES: case GL_MAX_COLOR_TEXTURE_SAMPLES: case GL_MAX_DEPTH_TEXTURE_SAMPLES: case GL_MAX_INTEGER_SAMPLES:
        I(caps && caps->v1.max_samples ? caps->v1.max_samples : 4);
    case GL_MAX_SAMPLE_MASK_WORDS: I(1);
    case GL_MAX_VERTEX_ATTRIBS: I(ZGL_MAX_ATTRIBS - 1);              /* one is kept for gl_InstanceID */
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: case GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS: I(ZGL_MAX_STAGE_SAMPLERS);
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: I(ZGL_MAX_UNITS);
    case GL_MAX_TEXTURE_UNITS: I(8);
    case GL_MAX_TEXTURE_COORDS: I(8);
    case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: I(ZGL_MAX_DRAW_BUFFERS);
    case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS: I(1);
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: I(ZGL_MAX_CONSTS * 4);
    case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS: I(ZGL_MAX_CONSTS);
    case GL_MAX_VARYING_FLOATS: I(64);
    case GL_MAX_VARYING_VECTORS: I(16);
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS: case GL_MAX_FRAGMENT_INPUT_COMPONENTS: I(64);
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: I(ZGL_MAX_UBO_BINDINGS);
    case GL_MAX_VERTEX_UNIFORM_BLOCKS: case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: I(ZGL_MAX_STAGE_UBOS);
    case GL_MAX_GEOMETRY_UNIFORM_BLOCKS: I(0);
    case GL_MAX_COMBINED_UNIFORM_BLOCKS: I(ZGL_MAX_STAGE_UBOS * 2);
    case GL_MAX_UNIFORM_BLOCK_SIZE: I(16384);
    case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS: case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS: I(ZGL_MAX_CONSTS * 4 + 16384 / 4 * ZGL_MAX_STAGE_UBOS);
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: I(16);
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: case GL_MAX_TRANSFORM_FEEDBACK_BUFFERS: I(ZGL_MAX_XFB_BUFFERS);
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: I(4);
    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: I(64);
    case GL_MIN_PROGRAM_TEXEL_OFFSET: I(-8);
    case GL_MAX_PROGRAM_TEXEL_OFFSET: I(7);
    case GL_MAX_CLIP_DISTANCES: I(8);
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: I(1 << 20);
    case GL_MAX_SERVER_WAIT_TIMEOUT: I(1e15);
    case GL_MAX_GEOMETRY_OUTPUT_VERTICES: case GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS: case GL_MAX_GEOMETRY_INPUT_COMPONENTS:
    case GL_MAX_GEOMETRY_OUTPUT_COMPONENTS: case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS: I(0);
    case GL_MAX_LIGHTS: I(ZGL_MAX_LIGHTS);
    case GL_MAX_MODELVIEW_STACK_DEPTH: I(ZGL_MV_STACK);
    case GL_MAX_PROJECTION_STACK_DEPTH: I(ZGL_PROJ_STACK);
    case GL_MAX_TEXTURE_STACK_DEPTH: I(ZGL_TEX_STACK);
    case GL_ALIASED_LINE_WIDTH_RANGE: case GL_SMOOTH_LINE_WIDTH_RANGE: r->is_float = 1; r->n = 2; v[0] = 1; v[1] = caps && caps->max_aliased_line_width > 0 ? caps->max_aliased_line_width : 8; return 1;
    case GL_ALIASED_POINT_SIZE_RANGE: case GL_POINT_SIZE_RANGE: r->is_float = 1; r->n = 2; v[0] = 1; v[1] = caps && caps->max_aliased_point_size > 0 ? caps->max_aliased_point_size : 64; return 1;
    case GL_POINT_SIZE_GRANULARITY: case GL_LINE_WIDTH_GRANULARITY: F(0.125);
    case GL_SUBPIXEL_BITS: I(4);
    case GL_SAMPLE_BUFFERS: I(0);
    case GL_SAMPLES: I(0);
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: I(8);
    case GL_COMPRESSED_TEXTURE_FORMATS: {
        static const GLenum cf[] = { GL_COMPRESSED_RED_RGTC1, GL_COMPRESSED_SIGNED_RED_RGTC1, GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_SIGNED_RG_RGTC2,
                                     GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT };
        r->n = 8; for (int i = 0; i < 8; i++) v[i] = cf[i];
        return 1;
    }
    case GL_DOUBLEBUFFER: I(1);
    case GL_STEREO: I(0);
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: I(8);
    case GL_DEPTH_BITS: I(24);
    case GL_STENCIL_BITS: I(zgl->depth_format == VIRGL_FORMAT_Z24X8_UNORM || zgl->depth_format == VIRGL_FORMAT_Z32_FLOAT ? 0 : 8);
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: I(GL_BGRA);
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: I(GL_UNSIGNED_BYTE);
    case GL_TIMESTAMP: { struct zgl_query q = { 0 }; (void)q; I(0); }
    /* bindings */
    case GL_ARRAY_BUFFER_BINDING: I(bufname(zgl->array_buffer));
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: I(bufname(zgl->vao->element_buffer));
    case GL_COPY_READ_BUFFER: I(bufname(zgl->copy_read_buffer));
    case GL_COPY_WRITE_BUFFER: I(bufname(zgl->copy_write_buffer));
    case GL_PIXEL_PACK_BUFFER_BINDING: I(bufname(zgl->pixel_pack_buffer));
    case GL_PIXEL_UNPACK_BUFFER_BINDING: I(bufname(zgl->pixel_unpack_buffer));
    case GL_UNIFORM_BUFFER_BINDING: I(bufname(zgl->uniform_buffer));
    case GL_TEXTURE_BUFFER: I(bufname(zgl->texture_buffer));
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: I(bufname(zgl->xfb_buffer));
    case GL_VERTEX_ARRAY_BINDING: I(zgl->vao == &zgl->vao0 ? 0 : zgl->vao->name);
    case GL_CURRENT_PROGRAM: I(zgl->program ? zgl->program->name : 0);
    case GL_DRAW_FRAMEBUFFER_BINDING: I(zgl->fbo ? zgl->fbo->name : 0);
    case GL_READ_FRAMEBUFFER_BINDING: I(zgl->read_fbo ? zgl->read_fbo->name : 0);
    case GL_RENDERBUFFER_BINDING: I(zgl->rb ? zgl->rb->name : 0);
    case GL_ACTIVE_TEXTURE: I(GL_TEXTURE0 + zgl->active_unit);
    case GL_CLIENT_ACTIVE_TEXTURE: I(GL_TEXTURE0 + zgl->client_unit);
    case GL_TEXTURE_BINDING_1D: I(name_of_tex(ZT_1D));
    case GL_TEXTURE_BINDING_2D: I(name_of_tex(ZT_2D));
    case GL_TEXTURE_BINDING_3D: I(name_of_tex(ZT_3D));
    case GL_TEXTURE_BINDING_CUBE_MAP: I(name_of_tex(ZT_CUBE));
    case GL_TEXTURE_BINDING_RECTANGLE: I(name_of_tex(ZT_RECT));
    case GL_TEXTURE_BINDING_1D_ARRAY: I(name_of_tex(ZT_1D_ARRAY));
    case GL_TEXTURE_BINDING_2D_ARRAY: I(name_of_tex(ZT_2D_ARRAY));
    case GL_TEXTURE_BINDING_BUFFER: I(name_of_tex(ZT_BUFFER));
    case GL_TEXTURE_BINDING_2D_MULTISAMPLE: I(name_of_tex(ZT_2D_MS));
    case GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY: I(name_of_tex(ZT_2D_MS_ARRAY));
    case GL_SAMPLER_BINDING: I(zgl->unit_sampler[zgl->active_unit] ? zgl->unit_sampler[zgl->active_unit]->name : 0);
    case GL_DRAW_BUFFER: case GL_DRAW_BUFFER0: I(zgl->fbo ? zgl->fbo->draw_buffers[0] : zgl->win_draw_buffer);
    case GL_READ_BUFFER: I(zgl->read_fbo ? zgl->read_fbo->read_buffer : zgl->win_read_buffer);
    /* rasterization and fragment state */
    case GL_VIEWPORT: r->n = 4; v[0] = zgl->vp_x; v[1] = zgl->vp_y; v[2] = zgl->vp_w; v[3] = zgl->vp_h; return 1;
    case GL_SCISSOR_BOX: r->n = 4; v[0] = zgl->sc_x; v[1] = zgl->sc_y; v[2] = zgl->sc_w; v[3] = zgl->sc_h; return 1;
    case GL_DEPTH_RANGE: r->is_float = 1; r->n = 2; v[0] = zgl->depth_near; v[1] = zgl->depth_far; return 1;
    case GL_COLOR_CLEAR_VALUE: r->is_float = 1; r->n = 4; for (int i = 0; i < 4; i++) v[i] = zgl->clear_color[i]; return 1;
    case GL_DEPTH_CLEAR_VALUE: F(zgl->clear_depth);
    case GL_STENCIL_CLEAR_VALUE: I(zgl->clear_stencil);
    case GL_BLEND_COLOR: r->is_float = 1; r->n = 4; for (int i = 0; i < 4; i++) v[i] = zgl->blend_color[i]; return 1;
    case GL_BLEND_SRC_RGB: I(zgl->blend_src);
    case GL_BLEND_DST_RGB: I(zgl->blend_dst);
    case GL_BLEND_SRC_ALPHA: I(zgl->blend_src_a);
    case GL_BLEND_DST_ALPHA: I(zgl->blend_dst_a);
    case GL_BLEND_EQUATION_RGB: I(zgl->blend_eq);
    case GL_BLEND_EQUATION_ALPHA: I(zgl->blend_eq_a);
    case GL_COLOR_WRITEMASK: r->n = 4; for (int i = 0; i < 4; i++) v[i] = (zgl->color_masks[0] >> i) & 1; return 1;
    case GL_DEPTH_WRITEMASK: I(zgl->depth_mask);
    case GL_DEPTH_FUNC: I(zgl->depth_func);
    case GL_CULL_FACE_MODE: I(zgl->cull_mode);
    case GL_FRONT_FACE: I(zgl->front_face);
    case GL_POLYGON_MODE: r->n = 2; v[0] = v[1] = zgl->polygon_mode; return 1;
    case GL_POLYGON_OFFSET_FACTOR: F(zgl->po_factor);
    case GL_POLYGON_OFFSET_UNITS: F(zgl->po_units);
    case GL_LINE_WIDTH: F(zgl->line_width);
    case GL_POINT_SIZE: F(zgl->point_size);
    case GL_PROVOKING_VERTEX: I(zgl->provoking_vertex);
    case GL_PRIMITIVE_RESTART_INDEX: I(zgl->restart_index);
    case GL_LOGIC_OP_MODE: I(zgl->logic_op);
    case GL_SAMPLE_COVERAGE_VALUE: F(zgl->sample_coverage);
    case GL_SAMPLE_COVERAGE_INVERT: I(zgl->sample_coverage_invert);
    case GL_STENCIL_FUNC: I(zgl->stencil_func[0]);
    case GL_STENCIL_REF: I(zgl->stencil_ref[0]);
    case GL_STENCIL_VALUE_MASK: I(zgl->stencil_vmask[0]);
    case GL_STENCIL_WRITEMASK: I(zgl->stencil_wmask[0]);
    case GL_STENCIL_FAIL: I(zgl->stencil_fail[0]);
    case GL_STENCIL_PASS_DEPTH_FAIL: I(zgl->stencil_zfail[0]);
    case GL_STENCIL_PASS_DEPTH_PASS: I(zgl->stencil_zpass[0]);
    case GL_STENCIL_BACK_FUNC: I(zgl->stencil_func[1]);
    case GL_STENCIL_BACK_REF: I(zgl->stencil_ref[1]);
    case GL_STENCIL_BACK_VALUE_MASK: I(zgl->stencil_vmask[1]);
    case GL_STENCIL_BACK_WRITEMASK: I(zgl->stencil_wmask[1]);
    case GL_STENCIL_BACK_FAIL: I(zgl->stencil_fail[1]);
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL: I(zgl->stencil_zfail[1]);
    case GL_STENCIL_BACK_PASS_DEPTH_PASS: I(zgl->stencil_zpass[1]);
    case GL_SHADE_MODEL: I(zgl->shade_model);
    case GL_MATRIX_MODE: I(zgl->matrix_mode);
    case GL_MODELVIEW_MATRIX: r->is_float = 1; r->n = 16; for (int i = 0; i < 16; i++) v[i] = zgl->mv[zgl->mv_top][i]; return 1;
    case GL_PROJECTION_MATRIX: r->is_float = 1; r->n = 16; for (int i = 0; i < 16; i++) v[i] = zgl->proj[zgl->proj_top][i]; return 1;
    case GL_TEXTURE_MATRIX: r->is_float = 1; r->n = 16; for (int i = 0; i < 16; i++) v[i] = zgl->texm[zgl->tex_top][i]; return 1;
    case GL_MODELVIEW_STACK_DEPTH: I(zgl->mv_top + 1);
    case GL_PROJECTION_STACK_DEPTH: I(zgl->proj_top + 1);
    case GL_CURRENT_COLOR: r->is_float = 1; r->n = 4; for (int i = 0; i < 4; i++) v[i] = zgl->cur_color[i]; return 1;
    case GL_CURRENT_NORMAL: r->is_float = 1; r->n = 3; for (int i = 0; i < 3; i++) v[i] = zgl->cur_normal[i]; return 1;
    case GL_CURRENT_TEXTURE_COORDS: r->is_float = 1; r->n = 4; for (int i = 0; i < 4; i++) v[i] = zgl->cur_texcoord[i]; return 1;
    /* pixel storage */
    case GL_UNPACK_ALIGNMENT: I(zgl->unpack_alignment);
    case GL_UNPACK_ROW_LENGTH: I(zgl->unpack_row_length);
    case GL_UNPACK_IMAGE_HEIGHT: I(zgl->unpack_image_height);
    case GL_UNPACK_SKIP_ROWS: I(zgl->unpack_skip_rows);
    case GL_UNPACK_SKIP_PIXELS: I(zgl->unpack_skip_pixels);
    case GL_UNPACK_SKIP_IMAGES: I(zgl->unpack_skip_images);
    case GL_UNPACK_SWAP_BYTES: I(zgl->unpack_swap);
    case GL_UNPACK_LSB_FIRST: I(zgl->unpack_lsb);
    case GL_PACK_ALIGNMENT: I(zgl->pack_alignment);
    case GL_PACK_ROW_LENGTH: I(zgl->pack_row_length);
    case GL_PACK_IMAGE_HEIGHT: I(zgl->pack_image_height);
    case GL_PACK_SKIP_ROWS: I(zgl->pack_skip_rows);
    case GL_PACK_SKIP_PIXELS: I(zgl->pack_skip_pixels);
    case GL_PACK_SKIP_IMAGES: I(zgl->pack_skip_images);
    case GL_PACK_SWAP_BYTES: I(zgl->pack_swap);
    case GL_PACK_LSB_FIRST: I(zgl->pack_lsb);
    case GL_TRANSFORM_FEEDBACK_BUFFER_START: case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE: I(0);
    }
    if (pname >= GL_DRAW_BUFFER1 && pname <= GL_DRAW_BUFFER7) I(zgl->fbo ? zgl->fbo->draw_buffers[pname - GL_DRAW_BUFFER0] : GL_NONE);
    /* enables are gettable too */
    int *s = enable_slot(pname);
    if (s) I(*s);
#undef I
#undef F
    return 0;
}

static int get_or_error(GLenum pname, struct val16 *r)
{
    if (!get_state(pname, r)) { zgl_set_error(GL_INVALID_ENUM); return 0; }
    return 1;
}

void glGetIntegerv(GLenum pname, GLint *out)
{
    CTX();
    struct val16 r;
    if (!get_or_error(pname, &r)) return;
    for (int i = 0; i < r.n; i++) {
        double x = r.v[i];
        if (r.is_float && (pname == GL_COLOR_CLEAR_VALUE || pname == GL_BLEND_COLOR || pname == GL_DEPTH_RANGE || pname == GL_DEPTH_CLEAR_VALUE || pname == GL_CURRENT_COLOR))
            x = x * 2147483647.0;                      /* normalized state maps to the integer range */
        out[i] = x > 2147483647.0 ? 2147483647 : x < -2147483648.0 ? INT32_MIN : (GLint)llround(x);
    }
}
void glGetInteger64v(GLenum pname, GLint64 *out)
{
    CTX();
    struct val16 r;
    if (!get_or_error(pname, &r)) return;
    for (int i = 0; i < r.n; i++) out[i] = (GLint64)llround(r.v[i]);
}
void glGetFloatv(GLenum pname, GLfloat *out)
{
    CTX();
    struct val16 r;
    if (!get_or_error(pname, &r)) return;
    for (int i = 0; i < r.n; i++) out[i] = (GLfloat)r.v[i];
}
void glGetDoublev(GLenum pname, GLdouble *out)
{
    CTX();
    struct val16 r;
    if (!get_or_error(pname, &r)) return;
    for (int i = 0; i < r.n; i++) out[i] = r.v[i];
}
void glGetBooleanv(GLenum pname, GLboolean *out)
{
    CTX();
    struct val16 r;
    if (!get_or_error(pname, &r)) return;
    for (int i = 0; i < r.n; i++) out[i] = r.v[i] != 0;
}

static int get_indexed(GLenum target, GLuint index, GLint64 *out, int *n)
{
    *n = 1;
    switch (target) {
    case GL_UNIFORM_BUFFER_BINDING: if (index >= ZGL_MAX_UBO_BINDINGS) break; out[0] = bufname(zgl->ubo[index].buf); return 1;
    case GL_UNIFORM_BUFFER_START: if (index >= ZGL_MAX_UBO_BINDINGS) break; out[0] = (GLint64)zgl->ubo[index].off; return 1;
    case GL_UNIFORM_BUFFER_SIZE: if (index >= ZGL_MAX_UBO_BINDINGS) break; out[0] = (GLint64)zgl->ubo[index].size; return 1;
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: if (index >= ZGL_MAX_XFB_BUFFERS) break; out[0] = bufname(zgl->xfb.buf[index]); return 1;
    case GL_TRANSFORM_FEEDBACK_BUFFER_START: if (index >= ZGL_MAX_XFB_BUFFERS) break; out[0] = (GLint64)zgl->xfb.off[index]; return 1;
    case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE: if (index >= ZGL_MAX_XFB_BUFFERS) break; out[0] = (GLint64)zgl->xfb.size[index]; return 1;
    case GL_BLEND: if (index >= ZGL_MAX_DRAW_BUFFERS) break; out[0] = zgl->blend[index]; return 1;
    case GL_COLOR_WRITEMASK: if (index >= ZGL_MAX_DRAW_BUFFERS) break; *n = 4; for (int i = 0; i < 4; i++) out[i] = (zgl->color_masks[index] >> i) & 1; return 1;
    case GL_SAMPLE_MASK_VALUE: if (index != 0) break; out[0] = zgl->sample_mask; return 1;
    default: zgl_set_error(GL_INVALID_ENUM); return 0;
    }
    zgl_set_error(GL_INVALID_VALUE);
    return 0;
}

void glGetIntegeri_v(GLenum target, GLuint index, GLint *data)
{
    CTX();
    GLint64 v[4]; int n;
    if (!get_indexed(target, index, v, &n)) return;
    for (int i = 0; i < n; i++) data[i] = (GLint)v[i];
}
void glGetInteger64i_v(GLenum target, GLuint index, GLint64 *data)
{
    CTX();
    GLint64 v[4]; int n;
    if (!get_indexed(target, index, v, &n)) return;
    for (int i = 0; i < n; i++) data[i] = v[i];
}
void glGetBooleani_v(GLenum target, GLuint index, GLboolean *data)
{
    CTX();
    GLint64 v[4]; int n;
    if (!get_indexed(target, index, v, &n)) return;
    for (int i = 0; i < n; i++) data[i] = v[i] != 0;
}

void glGetPointerv(GLenum pname, void **p)
{
    CTX();
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: *p = (void *)zgl->cl_vertex.p; break;
    case GL_NORMAL_ARRAY_POINTER: *p = (void *)zgl->cl_normal.p; break;
    case GL_COLOR_ARRAY_POINTER: *p = (void *)zgl->cl_color.p; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: *p = (void *)zgl->cl_texcoord.p; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}

/* ---- sync objects: everything before the fence is done when the host has run the stream up to it ---- */

GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
    CTXV(NULL);
    if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) { zgl_set_error(GL_INVALID_ENUM); return NULL; }
    if (flags) { zgl_set_error(GL_INVALID_VALUE); return NULL; }
    struct zgl_sync *s = calloc(1, sizeof *s);
    s->status = GL_UNSIGNALED;
    virgl_flush(zgl->v);
    return (GLsync)s;
}

GLboolean glIsSync(GLsync s) { CTXV(0); return s != NULL; }
void glDeleteSync(GLsync s) { CTX(); free((struct zgl_sync *)s); }

GLenum glClientWaitSync(GLsync s_, GLbitfield flags, GLuint64 timeout)
{
    CTXV(GL_WAIT_FAILED);
    struct zgl_sync *s = (struct zgl_sync *)s_;
    if (!s) { zgl_set_error(GL_INVALID_VALUE); return GL_WAIT_FAILED; }
    if (flags & ~(GLbitfield)GL_SYNC_FLUSH_COMMANDS_BIT) { zgl_set_error(GL_INVALID_VALUE); return GL_WAIT_FAILED; }
    if (s->status == GL_SIGNALED) return GL_ALREADY_SIGNALED;
    if (timeout == 0) {
        /* polling: a fence the host has passed is signalled; without asking it, say not yet once, then wait */
        virgl_flush_fenced(zgl->v);
        s->status = GL_SIGNALED;
        return GL_ALREADY_SIGNALED;
    }
    virgl_flush_fenced(zgl->v);
    s->status = GL_SIGNALED;
    return GL_CONDITION_SATISFIED;
}

void glWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout)
{
    CTX();
    if (!s || flags || timeout != GL_TIMEOUT_IGNORED) { zgl_set_error(GL_INVALID_VALUE); return; }
    /* the host runs one stream in order: the server-side wait is a no-op */
}

void glGetSynciv(GLsync s_, GLenum pname, GLsizei bufsize, GLsizei *length, GLint *values)
{
    CTX();
    struct zgl_sync *s = (struct zgl_sync *)s_;
    if (!s) { zgl_set_error(GL_INVALID_VALUE); return; }
    GLint v;
    switch (pname) {
    case GL_OBJECT_TYPE: v = GL_SYNC_FENCE; break;
    case GL_SYNC_STATUS:
        if (s->status != GL_SIGNALED) { virgl_flush_fenced(zgl->v); s->status = GL_SIGNALED; }
        v = (GLint)s->status; break;
    case GL_SYNC_CONDITION: v = GL_SYNC_GPU_COMMANDS_COMPLETE; break;
    case GL_SYNC_FLAGS: v = 0; break;
    default: zgl_set_error(GL_INVALID_ENUM); return;
    }
    if (bufsize > 0) values[0] = v;
    if (length) *length = bufsize > 0 ? 1 : 0;
}

/* ---- queries ------------------------------------------------------------------------------ */

enum { ZQ_SAMPLES, ZQ_ANY_SAMPLES, ZQ_TIME, ZQ_PRIMS_GENERATED, ZQ_XFB_WRITTEN, ZQ_COUNT };

static int query_kind(GLenum target)
{
    switch (target) {
    case GL_SAMPLES_PASSED: return ZQ_SAMPLES;
    case GL_ANY_SAMPLES_PASSED: return ZQ_ANY_SAMPLES;
    case GL_TIME_ELAPSED: return ZQ_TIME;
    case GL_PRIMITIVES_GENERATED: return ZQ_PRIMS_GENERATED;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: return ZQ_XFB_WRITTEN;
    }
    return -1;
}

static uint32_t pipe_query(GLenum target)
{
    switch (target) {
    case GL_SAMPLES_PASSED: return PIPE_QUERY_OCCLUSION_COUNTER;
    case GL_ANY_SAMPLES_PASSED: return PIPE_QUERY_OCCLUSION_PREDICATE;
    case GL_TIME_ELAPSED: return PIPE_QUERY_TIME_ELAPSED;
    case GL_TIMESTAMP: return PIPE_QUERY_TIMESTAMP;
    case GL_PRIMITIVES_GENERATED: return PIPE_QUERY_PRIMITIVES_GENERATED;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: return PIPE_QUERY_PRIMITIVES_EMITTED;
    }
    return 0;
}

static struct zgl_query *query_get(GLuint name)
{
    for (int i = 0; i < zgl->nqueries; i++) if (zgl->queries[i]->name == name) return zgl->queries[i];
    return NULL;
}

void glGenQueries(GLsizei n, GLuint *ids)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_query *q = calloc(1, sizeof *q);
        q->name = zgl->next_name++;
        LIST_ADD(zgl->queries, zgl->nqueries, q);
        ids[i] = q->name;
    }
}

static void query_free(struct zgl_query *q)
{
    for (int k = 0; k < ZQ_COUNT; k++) if (zgl->active_query[k] == q) zgl->active_query[k] = NULL;
    if (zgl->cond_query == q) zgl->cond_query = NULL;
    if (q->handle) virgl_destroy_object(zgl->v, q->handle, VIRGL_OBJECT_QUERY);
    if (q->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, q->res); }
    LIST_DEL(zgl->queries, zgl->nqueries, q);
    free(q);
}

void glDeleteQueries(GLsizei n, const GLuint *ids)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) { struct zgl_query *q = ids[i] ? query_get(ids[i]) : NULL; if (q) query_free(q); }
}

GLboolean glIsQuery(GLuint id) { CTXV(0); struct zgl_query *q = id ? query_get(id) : NULL; return q && q->target; }

/* the host object and the guest page it writes the result to */
static int query_make(struct zgl_query *q, GLenum target)
{
    if (q->handle && q->target == target) return 0;
    if (q->handle) { virgl_destroy_object(zgl->v, q->handle, VIRGL_OBJECT_QUERY); q->handle = 0; }
    if (!q->res) {
        q->res = virgl_res_create(zgl->v, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_CUSTOM, 4096, 1, 1, 0, 4096);
        if (!q->res || !q->res->map) { zgl_set_error(GL_OUT_OF_MEMORY); return -1; }
    }
    q->target = target;
    q->handle = virgl_handle(zgl->v);
    virgl_create_query(zgl->v, q->handle, pipe_query(target), 0, q->res->id, 0);
    return 0;
}

void glBeginQuery(GLenum target, GLuint id)
{
    CTX();
    int k = query_kind(target);
    if (k < 0) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_query *q = id ? query_get(id) : NULL;
    if (!q || zgl->active_query[k] || q->active || (q->target && q->target != target)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (query_make(q, target) != 0) return;
    memset(q->res->map, 0, sizeof(struct virgl_host_query_state));
    q->active = 1; q->pending = 0;
    zgl->active_query[k] = q;
    virgl_begin_query(zgl->v, q->handle);
}

void glEndQuery(GLenum target)
{
    CTX();
    int k = query_kind(target);
    if (k < 0) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_query *q = zgl->active_query[k];
    if (!q) { zgl_set_error(GL_INVALID_OPERATION); return; }
    virgl_end_query(zgl->v, q->handle);
    q->active = 0; q->pending = 1;
    zgl->active_query[k] = NULL;
}

void glQueryCounter(GLuint id, GLenum target)
{
    CTX();
    if (target != GL_TIMESTAMP) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_query *q = id ? query_get(id) : NULL;
    if (!q || q->active) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (query_make(q, target) != 0) return;
    memset(q->res->map, 0, sizeof(struct virgl_host_query_state));
    virgl_end_query(zgl->v, q->handle);
    q->pending = 1;
}

/* the host writes the result into the query's page when asked; wait: until it has */
void zgl_query_result(struct zgl_query *q, int wait)
{
    if (!q->pending) return;
    volatile struct virgl_host_query_state *st = q->res->map;
    if (st->query_state != VIRGL_QUERY_STATE_DONE) {
        virgl_get_query_result(zgl->v, q->handle, wait);
        if (wait) virgl_flush_fenced(zgl->v); else virgl_flush(zgl->v);
    }
    if (st->query_state == VIRGL_QUERY_STATE_DONE) {
        q->result = st->result;
        if (q->target == GL_ANY_SAMPLES_PASSED) q->result = q->result != 0;
        q->pending = 0;
    }
}

void glGetQueryiv(GLenum target, GLenum pname, GLint *v)
{
    CTX();
    int k = target == GL_TIMESTAMP ? -2 : query_kind(target);
    if (k == -1) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (pname == GL_CURRENT_QUERY) *v = k >= 0 && zgl->active_query[k] ? (GLint)zgl->active_query[k]->name : 0;
    else if (pname == GL_QUERY_COUNTER_BITS) *v = target == GL_TIME_ELAPSED || target == GL_TIMESTAMP ? 64 : target == GL_ANY_SAMPLES_PASSED ? 1 : 32;
    else zgl_set_error(GL_INVALID_ENUM);
}

static int query_object(GLuint id, GLenum pname, uint64_t *out)
{
    struct zgl_query *q = id ? query_get(id) : NULL;
    if (!q || q->active || !q->target) { zgl_set_error(GL_INVALID_OPERATION); return 0; }
    switch (pname) {
    case GL_QUERY_RESULT: zgl_query_result(q, 1); *out = q->result; return 1;
    case GL_QUERY_RESULT_AVAILABLE: zgl_query_result(q, 0); *out = !q->pending; return 1;
    }
    zgl_set_error(GL_INVALID_ENUM);
    return 0;
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *v) { CTX(); uint64_t r; if (query_object(id, pname, &r)) *v = r > INT32_MAX ? INT32_MAX : (GLint)r; }
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *v) { CTX(); uint64_t r; if (query_object(id, pname, &r)) *v = r > UINT32_MAX ? UINT32_MAX : (GLuint)r; }
void glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64 *v) { CTX(); uint64_t r; if (query_object(id, pname, &r)) *v = (GLint64)r; }
void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *v) { CTX(); uint64_t r; if (query_object(id, pname, &r)) *v = r; }

void glBeginConditionalRender(GLuint id, GLenum mode)
{
    CTX();
    struct zgl_query *q = id ? query_get(id) : NULL;
    if (!q || q->active || (q->target != GL_SAMPLES_PASSED && q->target != GL_ANY_SAMPLES_PASSED) || zgl->cond_query) { zgl_set_error(GL_INVALID_OPERATION); return; }
    uint32_t m;
    switch (mode) {
    case GL_QUERY_WAIT: m = PIPE_RENDER_COND_WAIT; break;
    case GL_QUERY_NO_WAIT: m = PIPE_RENDER_COND_NO_WAIT; break;
    case GL_QUERY_BY_REGION_WAIT: m = PIPE_RENDER_COND_BY_REGION_WAIT; break;
    case GL_QUERY_BY_REGION_NO_WAIT: m = PIPE_RENDER_COND_BY_REGION_NO_WAIT; break;
    default: zgl_set_error(GL_INVALID_ENUM); return;
    }
    zgl->cond_query = q; zgl->cond_mode = mode;
    virgl_set_render_condition(zgl->v, q->handle, 0, m);
}

void glEndConditionalRender(void)
{
    CTX();
    if (!zgl->cond_query) { zgl_set_error(GL_INVALID_OPERATION); return; }
    zgl->cond_query = NULL;
    virgl_set_render_condition(zgl->v, 0, 0, 0);
}

/* ---- transform feedback ------------------------------------------------------------------- */

void glBeginTransformFeedback(GLenum mode)
{
    CTX();
    if (mode != GL_POINTS && mode != GL_LINES && mode != GL_TRIANGLES) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_program *p = zgl->program;
    if (zgl->xfb.active || !p || !p->linked || !p->nxfb) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int nbuf = p->xfb_mode == GL_SEPARATE_ATTRIBS ? p->nxfb : 1;
    for (int i = 0; i < nbuf; i++) if (!zgl->xfb.buf[i] || !zgl->xfb.buf[i]->res) { zgl_set_error(GL_INVALID_OPERATION); return; }
    zgl->xfb.active = 1; zgl->xfb.paused = 0; zgl->xfb.mode = mode; zgl->xfb.program = p; zgl->xfb.append = 0;
    uint32_t h[ZGL_MAX_XFB_BUFFERS] = { 0 };
    for (int i = 0; i < nbuf; i++) {
        struct zgl_buffer *b = zgl->xfb.buf[i];
        if (zgl->xfb.h_target[i]) virgl_destroy_object(zgl->v, zgl->xfb.h_target[i], VIRGL_OBJECT_STREAMOUT_TARGET);
        zgl->xfb.h_target[i] = virgl_handle(zgl->v);
        size_t size = zgl->xfb.size[i] ? zgl->xfb.size[i] : b->size - zgl->xfb.off[i];
        virgl_create_so_target(zgl->v, zgl->xfb.h_target[i], b->res->id, (uint32_t)zgl->xfb.off[i], (uint32_t)size);
        h[i] = zgl->xfb.h_target[i];
        b->host_newer = 1;
    }
    virgl_set_so_targets(zgl->v, 0, h, (uint32_t)nbuf);
}

void glEndTransformFeedback(void)
{
    CTX();
    if (!zgl->xfb.active) { zgl_set_error(GL_INVALID_OPERATION); return; }
    virgl_set_so_targets(zgl->v, 0, NULL, 0);
    zgl->xfb.active = 0; zgl->xfb.paused = 0; zgl->xfb.program = NULL;
}
