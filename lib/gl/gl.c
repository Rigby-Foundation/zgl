/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The OpenGL state machine on libvirgl: objects, state, and the draw path
 * that turns it all into Gallium state objects and a DRAW_VBO. */
#include "zgl.h"
#include <GL/zgl_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct zgl_ctx *zgl;

void zgl_set_error(GLenum e) { if (zgl && zgl->error == GL_NO_ERROR) zgl->error = e; }
#define CTX() do { if (!zgl) return; } while (0)
#define CTXV(v) do { if (!zgl) return (v); } while (0)

/* ---- objects ---------------------------------------------------------------- */

static GLuint new_name(void) { return zgl->next_name++; }

static struct zgl_buffer *buffer_get(GLuint name)
{
    for (int i = 0; i < zgl->nbuffers; i++) if (zgl->buffers[i]->name == name) return zgl->buffers[i];
    return NULL;
}
static struct zgl_texture *texture_get(GLuint name)
{
    for (int i = 0; i < zgl->ntextures; i++) if (zgl->textures[i]->name == name) return zgl->textures[i];
    return NULL;
}
static struct zgl_shader *shader_get(GLuint name)
{
    for (int i = 0; i < zgl->nshaders; i++) if (zgl->shaders[i]->name == name) return zgl->shaders[i];
    return NULL;
}
static struct zgl_program *program_get(GLuint name)
{
    for (int i = 0; i < zgl->nprograms; i++) if (zgl->programs[i]->name == name) return zgl->programs[i];
    return NULL;
}

#define LIST_ADD(list, n, item) do { (list) = realloc((list), sizeof *(list) * ((n) + 1)); (list)[(n)++] = (item); } while (0)
#define LIST_DEL(list, n, item) do { for (int _i = 0; _i < (n); _i++) if ((list)[_i] == (item)) { memmove(&(list)[_i], &(list)[_i + 1], sizeof *(list) * ((n) - _i - 1)); (n)--; break; } } while (0)

/* ---- errors, gets --------------------------------------------------------------- */

GLenum glGetError(void) { CTXV(GL_NO_ERROR); GLenum e = zgl->error; zgl->error = GL_NO_ERROR; return e; }

static char renderer_str[128];
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
    case GL_VERSION: return (const GLubyte *)"2.1 zgl";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"1.20";
    case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_vertex_buffer_object GL_ARB_shader_objects GL_ARB_vertex_shader GL_ARB_fragment_shader GL_ARB_texture_non_power_of_two GL_EXT_framebuffer_object";
    default: zgl_set_error(GL_INVALID_ENUM); return (const GLubyte *)"";
    }
}

void glGetIntegerv(GLenum pname, GLint *v)
{
    CTX();
    switch (pname) {
    case GL_VIEWPORT: v[0] = zgl->vp_x; v[1] = zgl->vp_y; v[2] = zgl->vp_w; v[3] = zgl->vp_h; break;
    case GL_MAX_TEXTURE_SIZE: { const struct virgl_caps_v2 *caps = virgl_caps(zgl->v); v[0] = caps ? (GLint)caps->max_texture_2d_size : 4096; break; }
    case GL_MAX_VERTEX_ATTRIBS: v[0] = ZGL_MAX_ATTRIBS; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_TEXTURE_UNITS: v[0] = ZGL_MAX_UNITS; break;
    case GL_FRAMEBUFFER_BINDING: v[0] = zgl->fbo ? (GLint)zgl->fbo->name : 0; break;
    case GL_PACK_ROW_LENGTH: v[0] = zgl->pack_row_length; break;
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: v[0] = ZGL_MAX_CONSTS * 4; break;
    case GL_MAX_VARYING_FLOATS: v[0] = 32; break;
    case GL_MAX_LIGHTS: v[0] = ZGL_MAX_LIGHTS; break;
    case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = ZGL_MV_STACK; break;
    case GL_MAX_PROJECTION_STACK_DEPTH: v[0] = ZGL_PROJ_STACK; break;
    case GL_MATRIX_MODE: v[0] = (GLint)zgl->matrix_mode; break;
    case GL_DEPTH_BITS: v[0] = 24; break;
    case GL_STENCIL_BITS: v[0] = 8; break;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: v[0] = 8; break;
    case GL_CURRENT_PROGRAM: v[0] = zgl->program ? (GLint)zgl->program->name : 0; break;
    case GL_ARRAY_BUFFER_BINDING: v[0] = zgl->array_buffer ? (GLint)zgl->array_buffer->name : 0; break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = zgl->element_buffer ? (GLint)zgl->element_buffer->name : 0; break;
    case GL_TEXTURE_BINDING_2D: v[0] = zgl->unit_tex[zgl->active_unit] ? (GLint)zgl->unit_tex[zgl->active_unit]->name : 0; break;
    case GL_ACTIVE_TEXTURE: v[0] = GL_TEXTURE0 + zgl->active_unit; break;
    case GL_UNPACK_ALIGNMENT: v[0] = zgl->unpack_alignment; break;
    case GL_PACK_ALIGNMENT: v[0] = zgl->pack_alignment; break;
    case GL_DEPTH_FUNC: v[0] = (GLint)zgl->depth_func; break;
    case GL_DEPTH_WRITEMASK: v[0] = zgl->depth_mask; break;
    case GL_CULL_FACE_MODE: v[0] = (GLint)zgl->cull_mode; break;
    case GL_FRONT_FACE: v[0] = (GLint)zgl->front_face; break;
    case GL_SHADE_MODEL: v[0] = (GLint)zgl->shade_model; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}

void glGetFloatv(GLenum pname, GLfloat *v)
{
    CTX();
    switch (pname) {
    case GL_MODELVIEW_MATRIX: memcpy(v, zgl->mv[zgl->mv_top], 64); break;
    case GL_PROJECTION_MATRIX: memcpy(v, zgl->proj[zgl->proj_top], 64); break;
    case GL_VIEWPORT: v[0] = (GLfloat)zgl->vp_x; v[1] = (GLfloat)zgl->vp_y; v[2] = (GLfloat)zgl->vp_w; v[3] = (GLfloat)zgl->vp_h; break;
    default: { GLint i[4] = { 0 }; glGetIntegerv(pname, i); v[0] = (GLfloat)i[0]; }
    }
}
void glGetDoublev(GLenum pname, GLdouble *v)
{
    GLfloat f[16] = { 0 };
    glGetFloatv(pname, f);
    int n = pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX ? 16 : pname == GL_VIEWPORT ? 4 : 1;
    for (int i = 0; i < n; i++) v[i] = f[i];
}
void glGetBooleanv(GLenum pname, GLboolean *v) { GLint i[4] = { 0 }; glGetIntegerv(pname, i); v[0] = i[0] != 0; }

/* ---- simple state --------------------------------------------------------------- */

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX(); zgl->clear_color[0] = r; zgl->clear_color[1] = g; zgl->clear_color[2] = b; zgl->clear_color[3] = a; }
void glClearDepth(GLclampd d) { CTX(); zgl->clear_depth = d; }
void glClearStencil(GLint s) { CTX(); zgl->clear_stencil = s; }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); zgl->vp_x = x; zgl->vp_y = y; zgl->vp_w = w; zgl->vp_h = h; zgl->vp_dirty = 1; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { CTX(); zgl->sc_x = x; zgl->sc_y = y; zgl->sc_w = w; zgl->sc_h = h; zgl->vp_dirty = 1; }
void glDepthRange(GLclampd n, GLclampd f) { CTX(); zgl->depth_near = (float)n; zgl->depth_far = (float)f; zgl->vp_dirty = 1; }
void glDepthFunc(GLenum f) { CTX(); zgl->depth_func = f; }
void glDepthMask(GLboolean m) { CTX(); zgl->depth_mask = m != 0; }
void glBlendFunc(GLenum s, GLenum d) { CTX(); zgl->blend_src = zgl->blend_src_a = s; zgl->blend_dst = zgl->blend_dst_a = d; }
void glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da) { CTX(); zgl->blend_src = s; zgl->blend_dst = d; zgl->blend_src_a = sa; zgl->blend_dst_a = da; }
void glBlendEquation(GLenum m) { CTX(); zgl->blend_eq = m; }
void glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX(); zgl->blend_color[0] = r; zgl->blend_color[1] = g; zgl->blend_color[2] = b; zgl->blend_color[3] = a; virgl_set_blend_color(zgl->v, zgl->blend_color); }
void glAlphaFunc(GLenum f, GLclampf ref) { CTX(); zgl->alpha_func = f; zgl->alpha_ref = ref; }
void glCullFace(GLenum m) { CTX(); zgl->cull_mode = m; }
void glFrontFace(GLenum m) { CTX(); zgl->front_face = m; }
void glPolygonMode(GLenum face, GLenum m) { CTX(); (void)face; zgl->polygon_mode = m; }
void glPolygonOffset(GLfloat f, GLfloat u) { CTX(); zgl->po_factor = f; zgl->po_units = u; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { CTX(); zgl->color_mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0); }
void glLineWidth(GLfloat w) { CTX(); zgl->line_width = w; }
void glPointSize(GLfloat s) { CTX(); zgl->point_size = s; }
void glShadeModel(GLenum m) { CTX(); zgl->shade_model = m; }
void glHint(GLenum t, GLenum m) { (void)t; (void)m; }
void glPixelStorei(GLenum p, GLint v)
{
    CTX();
    if (p == GL_UNPACK_ALIGNMENT) zgl->unpack_alignment = v;
    else if (p == GL_PACK_ALIGNMENT) zgl->pack_alignment = v;
    else if (p == GL_PACK_ROW_LENGTH) zgl->pack_row_length = v;
    else if (p != GL_UNPACK_ROW_LENGTH) zgl_set_error(GL_INVALID_ENUM);
}
void glFlush(void) { CTX(); virgl_flush(zgl->v); }
void glFinish(void) { CTX(); virgl_flush(zgl->v); }

static int *enable_slot(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST: return &zgl->depth_test;
    case GL_BLEND: return &zgl->blend;
    case GL_CULL_FACE: return &zgl->cull;
    case GL_SCISSOR_TEST: return &zgl->scissor;
    case GL_ALPHA_TEST: return &zgl->alpha_test;
    case GL_TEXTURE_2D: return &zgl->texture_2d[zgl->active_unit];
    case GL_LIGHTING: return &zgl->lighting;
    case GL_COLOR_MATERIAL: return &zgl->color_material;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: return &zgl->normalize_n;
    case GL_STENCIL_TEST: return &zgl->stencil_test;
    case GL_POLYGON_OFFSET_FILL: return &zgl->polygon_offset;
    default: break;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + ZGL_MAX_LIGHTS) return &zgl->lights[cap - GL_LIGHT0].enabled;
    return NULL;
}
void glEnable(GLenum cap) { CTX(); int *s = enable_slot(cap); if (s) *s = 1; else if (cap != GL_DITHER && cap != GL_FOG && cap != GL_LINE_SMOOTH && cap != GL_POINT_SMOOTH && cap != GL_MULTISAMPLE) zgl_set_error(GL_INVALID_ENUM); }
void glDisable(GLenum cap) { CTX(); int *s = enable_slot(cap); if (s) *s = 0; }
GLboolean glIsEnabled(GLenum cap) { CTXV(0); int *s = enable_slot(cap); return s ? (*s != 0) : 0; }

/* ---- buffers ------------------------------------------------------------------- */

void glGenBuffers(GLsizei n, GLuint *b)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_buffer *buf = calloc(1, sizeof *buf);
        buf->name = new_name();
        LIST_ADD(zgl->buffers, zgl->nbuffers, buf);
        b[i] = buf->name;
    }
}

static void buffer_free(struct zgl_buffer *buf)
{
    if (zgl->array_buffer == buf) zgl->array_buffer = NULL;
    if (zgl->element_buffer == buf) zgl->element_buffer = NULL;
    for (int i = 0; i < ZGL_MAX_ATTRIBS; i++) if (zgl->attribs[i].buffer == buf) zgl->attribs[i].buffer = NULL;
    if (buf->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, buf->res); }
    LIST_DEL(zgl->buffers, zgl->nbuffers, buf);
    free(buf);
}

void glDeleteBuffers(GLsizei n, const GLuint *b)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) { struct zgl_buffer *buf = buffer_get(b[i]); if (buf) buffer_free(buf); }
}

void glBindBuffer(GLenum target, GLuint b)
{
    CTX();
    struct zgl_buffer *buf = b ? buffer_get(b) : NULL;
    if (b && !buf) {                            /* GL creates on bind */
        buf = calloc(1, sizeof *buf);
        buf->name = b;
        LIST_ADD(zgl->buffers, zgl->nbuffers, buf);
        if (b >= zgl->next_name) zgl->next_name = b + 1;
    }
    if (target == GL_ARRAY_BUFFER) zgl->array_buffer = buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) zgl->element_buffer = buf;
    else zgl_set_error(GL_INVALID_ENUM);
}

/* Write `size` bytes at `off` into a buffer resource through inline writes (chunked). */
/* Buffers are backed by guest memory (the map doubles as the CPU shadow):
 * write there, then transfer the range to the host. */
static void buffer_write(virgl_res *res, size_t off, const void *data, size_t size)
{
    memcpy((uint8_t *)res->map + off, data, size);
    virgl_res_to_host(zgl->v, res, 0, (uint32_t)off, 0, 0, (uint32_t)size, 1, 1, 0, 0, off);
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    CTX();
    (void)usage;
    struct zgl_buffer *buf = target == GL_ARRAY_BUFFER ? zgl->array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? zgl->element_buffer : NULL;
    if (!buf) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (buf->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, buf->res); buf->res = NULL; }
    buf->size = (size_t)size;
    buf->shadow = NULL;
    if (size <= 0) return;
    /* virgl wants one bind per buffer; a GL buffer bound as both later is
     * rare enough to live with the element-array copy path (shadow). */
    buf->res = virgl_res_create(zgl->v, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM,
                                target == GL_ARRAY_BUFFER ? VIRGL_BIND_VERTEX_BUFFER : VIRGL_BIND_INDEX_BUFFER,
                                (uint32_t)size, 1, 1, 0, (size_t)size);
    if (!buf->res || !buf->res->map) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
    buf->bind = target;
    buf->shadow = buf->res->map;
    if (data) buffer_write(buf->res, 0, data, (size_t)size);
    else memset(buf->res->map, 0, (size_t)size);
}

void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data)
{
    CTX();
    struct zgl_buffer *buf = target == GL_ARRAY_BUFFER ? zgl->array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? zgl->element_buffer : NULL;
    if (!buf || !buf->res || off < 0 || (size_t)(off + size) > buf->size) { zgl_set_error(GL_INVALID_VALUE); return; }
    buffer_write(buf->res, (size_t)off, data, (size_t)size);
}

/* ---- textures ------------------------------------------------------------------- */

void glGenTextures(GLsizei n, GLuint *t)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_texture *tex = calloc(1, sizeof *tex);
        tex->name = new_name();
        tex->min_filter = GL_NEAREST_MIPMAP_LINEAR; tex->mag_filter = GL_LINEAR; tex->wrap_s = tex->wrap_t = GL_REPEAT;
        LIST_ADD(zgl->textures, zgl->ntextures, tex);
        t[i] = tex->name;
    }
}

static void texture_free(struct zgl_texture *tex)
{
    for (int i = 0; i < ZGL_MAX_UNITS; i++) if (zgl->unit_tex[i] == tex) zgl->unit_tex[i] = NULL;
    if (tex->h_view) virgl_destroy_object(zgl->v, tex->h_view, VIRGL_OBJECT_SAMPLER_VIEW);
    if (tex->h_sampler) virgl_destroy_object(zgl->v, tex->h_sampler, VIRGL_OBJECT_SAMPLER_STATE);
    for (int i = 0; i < zgl->nfbos; i++)
        if (zgl->fbos[i]->tex == tex) {
            if (zgl->fbos[i]->h_surf) { virgl_destroy_object(zgl->v, zgl->fbos[i]->h_surf, VIRGL_OBJECT_SURFACE); zgl->fbos[i]->h_surf = 0; }
            zgl->fbos[i]->tex = NULL;
            if (zgl->fbo == zgl->fbos[i]) zgl->fb_dirty = 1;
        }
    if (tex->res) virgl_res_destroy(zgl->v, tex->res);
    LIST_DEL(zgl->textures, zgl->ntextures, tex);
    free(tex);
}

void glDeleteTextures(GLsizei n, const GLuint *t)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) { struct zgl_texture *tex = texture_get(t[i]); if (tex) texture_free(tex); }
}

GLboolean glIsTexture(GLuint t) { CTXV(0); return texture_get(t) != NULL; }

void glBindTexture(GLenum target, GLuint t)
{
    CTX();
    if (target != GL_TEXTURE_2D) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *tex = t ? texture_get(t) : NULL;
    if (t && !tex) {
        tex = calloc(1, sizeof *tex);
        tex->name = t;
        tex->min_filter = GL_NEAREST_MIPMAP_LINEAR; tex->mag_filter = GL_LINEAR; tex->wrap_s = tex->wrap_t = GL_REPEAT;
        LIST_ADD(zgl->textures, zgl->ntextures, tex);
        if (t >= zgl->next_name) zgl->next_name = t + 1;
    }
    zgl->unit_tex[zgl->active_unit] = tex;
}

void glActiveTexture(GLenum unit)
{
    CTX();
    if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + ZGL_MAX_UNITS) { zgl_set_error(GL_INVALID_ENUM); return; }
    zgl->active_unit = (int)(unit - GL_TEXTURE0);
}

static int mip_levels(int w, int h)
{
    int n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

/* Convert a GL pixel rectangle to RGBA8 rows (tightly packed). */
static uint8_t *convert_pixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels, int align)
{
    int comps = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
    size_t row_in;
    if (type == GL_UNSIGNED_BYTE) row_in = (size_t)w * comps;
    else if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) row_in = (size_t)w * 2;
    else if (type == GL_UNSIGNED_INT_8_8_8_8_REV) row_in = (size_t)w * 4;
    else if (type == GL_FLOAT) row_in = (size_t)w * comps * 4;
    else return NULL;
    row_in = (row_in + align - 1) & ~(size_t)(align - 1);
    uint8_t *out = malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (GLsizei y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)pixels + y * row_in;
        uint8_t *dst = out + (size_t)y * w * 4;
        for (GLsizei x = 0; x < w; x++, dst += 4) {
            uint8_t r = 255, g = 255, b = 255, a = 255;
            if (type == GL_UNSIGNED_BYTE) {
                const uint8_t *s = src + x * comps;
                switch (format) {
                case GL_RGBA: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
                case GL_RGB: r = s[0]; g = s[1]; b = s[2]; break;
                case GL_BGRA: b = s[0]; g = s[1]; r = s[2]; a = s[3]; break;
                case GL_BGR: b = s[0]; g = s[1]; r = s[2]; break;
                case GL_LUMINANCE: r = g = b = s[0]; break;
                case GL_LUMINANCE_ALPHA: r = g = b = s[0]; a = s[1]; break;
                case GL_ALPHA: r = g = b = 0; a = s[0]; break;
                default: break;
                }
            } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
                uint16_t p = ((const uint16_t *)src)[x];
                r = (uint8_t)(((p >> 11) & 31) * 255 / 31); g = (uint8_t)(((p >> 5) & 63) * 255 / 63); b = (uint8_t)((p & 31) * 255 / 31);
            } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                uint16_t p = ((const uint16_t *)src)[x];
                r = (uint8_t)(((p >> 12) & 15) * 17); g = (uint8_t)(((p >> 8) & 15) * 17); b = (uint8_t)(((p >> 4) & 15) * 17); a = (uint8_t)((p & 15) * 17);
            } else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
                uint16_t p = ((const uint16_t *)src)[x];
                r = (uint8_t)(((p >> 11) & 31) * 255 / 31); g = (uint8_t)(((p >> 6) & 31) * 255 / 31); b = (uint8_t)(((p >> 1) & 31) * 255 / 31); a = (p & 1) ? 255 : 0;
            } else if (type == GL_UNSIGNED_INT_8_8_8_8_REV) {
                uint32_t p = ((const uint32_t *)src)[x];
                if (format == GL_BGRA) { b = (uint8_t)p; g = (uint8_t)(p >> 8); r = (uint8_t)(p >> 16); a = (uint8_t)(p >> 24); }
                else { r = (uint8_t)p; g = (uint8_t)(p >> 8); b = (uint8_t)(p >> 16); a = (uint8_t)(p >> 24); }
            } else if (type == GL_FLOAT) {
                const float *s = (const float *)src + x * comps;
                float fr = s[0], fg = comps > 1 ? s[1] : s[0], fb = comps > 2 ? s[2] : s[0], fa = comps > 3 ? s[3] : 1.0f;
                r = (uint8_t)(fr * 255); g = (uint8_t)(fg * 255); b = (uint8_t)(fb * 255); a = (uint8_t)(fa * 255);
            }
            dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = a;   /* B8G8R8A8: one format for textures and the window, so copies are plain blits */
        }
    }
    return out;
}

static void texture_invalidate_view(struct zgl_texture *tex)
{
    if (tex->h_view) { virgl_destroy_object(zgl->v, tex->h_view, VIRGL_OBJECT_SAMPLER_VIEW); tex->h_view = 0; }
}

/* upload RGBA8 rows into level `level` at (x, y) */
static void texture_upload(struct zgl_texture *tex, int level, int x, int y, int w, int h, const uint8_t *rgba)
{
    size_t row = (size_t)w * 4;
    int rows_per_chunk = (int)((48 * 1024) / row);
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    for (int yy = 0; yy < h; yy += rows_per_chunk) {
        int n = h - yy < rows_per_chunk ? h - yy : rows_per_chunk;
        virgl_inline_write(zgl->v, tex->res->id, (uint32_t)level, 0, (uint32_t)row, (uint32_t)(row * n),
                           (uint32_t)x, (uint32_t)(y + yy), 0, (uint32_t)w, (uint32_t)n, 1, rgba + (size_t)yy * row, row * n);
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    CTX();
    (void)internal; (void)border;
    if (target != GL_TEXTURE_2D) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *tex = zgl->unit_tex[zgl->active_unit];
    if (!tex) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (level == 0) {
        if (!tex->res || tex->w != w || tex->h != h) {
            if (tex->res) { texture_invalidate_view(tex); virgl_res_destroy(zgl->v, tex->res); }
            tex->w = w; tex->h = h;
            tex->levels = mip_levels(w, h);
            tex->vformat = VIRGL_FORMAT_B8G8R8A8_UNORM;
            tex->res = NULL;
            if (w > 0 && h > 0) {
                struct gpu_res_create rc = { .target = PIPE_TEXTURE_2D, .format = tex->vformat, .bind = VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET,
                                             .width = (uint32_t)w, .height = (uint32_t)h, .depth = 1, .array_size = 1, .last_level = (uint32_t)(tex->levels - 1) };
                tex->res = virgl_res_create_ex(zgl->v, &rc);
            }
            tex->has_level0 = 0;
        }
        tex->has_level0 = 1;
    }
    if (!tex->res || level >= tex->levels) return;
    if (!pixels) return;
    uint8_t *rgba = convert_pixels(w, h, format, type, pixels, zgl->unpack_alignment);
    if (!rgba) { zgl_set_error(GL_INVALID_ENUM); return; }
    texture_upload(tex, level, 0, 0, w, h, rgba);
    free(rgba);
    if (tex->generate_mipmap && level == 0) glGenerateMipmap(GL_TEXTURE_2D);
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
    CTX();
    if (target != GL_TEXTURE_2D) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *tex = zgl->unit_tex[zgl->active_unit];
    if (!tex || !tex->res || level >= tex->levels) { zgl_set_error(GL_INVALID_OPERATION); return; }
    uint8_t *rgba = convert_pixels(w, h, format, type, pixels, zgl->unpack_alignment);
    if (!rgba) { zgl_set_error(GL_INVALID_ENUM); return; }
    texture_upload(tex, level, x, y, w, h, rgba);
    free(rgba);
}

int zglTextureFromResource(GLuint name, uint32_t res, int w, int h)
{
    if (!zgl) return -1;
    struct zgl_texture *tex = texture_get(name);
    if (!tex) {
        glBindTexture(GL_TEXTURE_2D, name);                     /* creates it */
        tex = texture_get(name);
        if (!tex) return -1;
    }
    if (tex->res) { texture_invalidate_view(tex); virgl_res_destroy(zgl->v, tex->res); tex->res = NULL; }
    tex->res = virgl_res_attach(zgl->v, res, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, (uint32_t)w, (uint32_t)h);
    if (!tex->res) return -1;
    tex->w = w; tex->h = h; tex->levels = 1; tex->has_level0 = 1;
    tex->vformat = VIRGL_FORMAT_B8G8R8A8_UNORM;
    return 0;
}

void zglTexSubImageXRGB(GLint x, GLint y, GLsizei w, GLsizei h, const uint32_t *pixels, int stride)
{
    CTX();
    struct zgl_texture *tex = zgl->unit_tex[zgl->active_unit];
    if (!tex || !tex->res || w <= 0 || h <= 0) return;
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
        virgl_inline_write(zgl->v, tex->res->id, 0, 0, (uint32_t)row, (uint32_t)(row * n),
                           (uint32_t)x, (uint32_t)(y + yy), 0, (uint32_t)w, (uint32_t)n, 1, buf, row * n);
    }
    free(buf);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    CTX();
    if (target != GL_TEXTURE_2D) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *tex = zgl->unit_tex[zgl->active_unit];
    if (!tex) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: tex->min_filter = (GLenum)param; break;
    case GL_TEXTURE_MAG_FILTER: tex->mag_filter = (GLenum)param; break;
    case GL_TEXTURE_WRAP_S: tex->wrap_s = (GLenum)param; break;
    case GL_TEXTURE_WRAP_T: tex->wrap_t = (GLenum)param; break;
    case GL_GENERATE_MIPMAP: tex->generate_mipmap = param != 0; return;
    default: return;
    }
    if (tex->h_sampler) { virgl_destroy_object(zgl->v, tex->h_sampler, VIRGL_OBJECT_SAMPLER_STATE); tex->h_sampler = 0; }
    texture_invalidate_view(tex);
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { glTexParameteri(target, pname, (GLint)param); }

void glGenerateMipmap(GLenum target)
{
    CTX();
    if (target != GL_TEXTURE_2D) return;
    struct zgl_texture *tex = zgl->unit_tex[zgl->active_unit];
    if (!tex || !tex->res) return;
    /* each level is a filtered blit of the one above */
    int w = tex->w, h = tex->h;
    for (int l = 1; l < tex->levels; l++) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        virgl_cmd(zgl->v, VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE);
        virgl_dw(zgl->v, VIRGL_CMD_BLIT_S0_MASK(0xF) | VIRGL_CMD_BLIT_S0_FILTER(PIPE_TEX_FILTER_LINEAR));
        virgl_dw(zgl->v, 0); virgl_dw(zgl->v, 0);
        virgl_dw(zgl->v, tex->res->id); virgl_dw(zgl->v, (uint32_t)l); virgl_dw(zgl->v, tex->vformat);
        virgl_dw(zgl->v, 0); virgl_dw(zgl->v, 0); virgl_dw(zgl->v, 0); virgl_dw(zgl->v, (uint32_t)nw); virgl_dw(zgl->v, (uint32_t)nh); virgl_dw(zgl->v, 1);
        virgl_dw(zgl->v, tex->res->id); virgl_dw(zgl->v, (uint32_t)(l - 1)); virgl_dw(zgl->v, tex->vformat);
        virgl_dw(zgl->v, 0); virgl_dw(zgl->v, 0); virgl_dw(zgl->v, 0); virgl_dw(zgl->v, (uint32_t)w); virgl_dw(zgl->v, (uint32_t)h); virgl_dw(zgl->v, 1);
        w = nw; h = nh;
    }
}

static uint32_t gl_wrap(GLenum w)
{
    switch (w) { case GL_CLAMP: case GL_CLAMP_TO_EDGE: return PIPE_TEX_WRAP_CLAMP_TO_EDGE; case GL_MIRRORED_REPEAT: return PIPE_TEX_WRAP_MIRROR_REPEAT; default: return PIPE_TEX_WRAP_REPEAT; }
}

uint32_t zgl_texture_sampler(struct zgl_texture *t)
{
    if (t->h_sampler) return t->h_sampler;
    int mip = t->min_filter == GL_NEAREST_MIPMAP_NEAREST || t->min_filter == GL_LINEAR_MIPMAP_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    if (mip && t->levels < 2) mip = 0;
    uint32_t min_img = (t->min_filter == GL_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_LINEAR) ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
    uint32_t min_mip = !mip ? PIPE_TEX_MIPFILTER_NONE : (t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR) ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;
    uint32_t mag = t->mag_filter == GL_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
    t->h_sampler = virgl_handle(zgl->v);
    virgl_create_sampler_state(zgl->v, t->h_sampler, gl_wrap(t->wrap_s), gl_wrap(t->wrap_t), min_img, min_mip, mag);
    return t->h_sampler;
}

uint32_t zgl_texture_view(struct zgl_texture *t)
{
    if (t->h_view) return t->h_view;
    int mip = t->min_filter == GL_NEAREST_MIPMAP_NEAREST || t->min_filter == GL_LINEAR_MIPMAP_NEAREST || t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    t->h_view = virgl_handle(zgl->v);
    virgl_create_sampler_view(zgl->v, t->h_view, t->res->id, PIPE_TEXTURE_2D, t->vformat, 0, mip ? (uint32_t)(t->levels - 1) : 0, VIRGL_SWIZZLE_IDENTITY);
    return t->h_view;
}

/* ---- shaders and programs ------------------------------------------------------ */

GLuint glCreateShader(GLenum type)
{
    CTXV(0);
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) { zgl_set_error(GL_INVALID_ENUM); return 0; }
    struct zgl_shader *s = calloc(1, sizeof *s);
    s->name = new_name(); s->type = type; s->refs = 1;
    LIST_ADD(zgl->shaders, zgl->nshaders, s);
    return s->name;
}

static void shader_unref(struct zgl_shader *s)
{
    if (--s->refs > 0) return;
    LIST_DEL(zgl->shaders, zgl->nshaders, s);
    free(s->source);
    zgl_compiled_free(&s->c);
    free(s);
}

void glDeleteShader(GLuint name) { CTX(); struct zgl_shader *s = shader_get(name); if (!s) return; s->deleted = 1; shader_unref(s); }

void glShaderSource(GLuint name, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    CTX();
    struct zgl_shader *s = shader_get(name);
    if (!s) { zgl_set_error(GL_INVALID_VALUE); return; }
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *src = malloc(total + 1);
    size_t off = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(src + off, strings[i], n);
        off += n;
    }
    src[off] = 0;
    free(s->source);
    s->source = src;
    s->compiled = 0;
}

void glCompileShader(GLuint name)
{
    CTX();
    struct zgl_shader *s = shader_get(name);
    if (!s || !s->source) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_compiled trial;
    s->log[0] = 0;
    /* a trial compile reports errors now; the real one happens at link, with the
     * vertex shader's varying numbering */
    if (zgl_compile(s->source, s->type, &trial, s->log, sizeof s->log) == 0) { zgl_compiled_free(&trial); s->compiled = 1; }
    else s->compiled = 0;
}

void glGetShaderiv(GLuint name, GLenum pname, GLint *v)
{
    CTX();
    struct zgl_shader *s = shader_get(name);
    if (!s) { zgl_set_error(GL_INVALID_VALUE); return; }
    switch (pname) {
    case GL_COMPILE_STATUS: *v = s->compiled; break;
    case GL_INFO_LOG_LENGTH: *v = (GLint)strlen(s->log) + 1; break;
    case GL_SHADER_TYPE: *v = (GLint)s->type; break;
    case GL_DELETE_STATUS: *v = s->deleted; break;
    case GL_SHADER_SOURCE_LENGTH: *v = s->source ? (GLint)strlen(s->source) + 1 : 0; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}

void glGetShaderInfoLog(GLuint name, GLsizei max, GLsizei *len, GLchar *log)
{
    CTX();
    struct zgl_shader *s = shader_get(name);
    if (!s || max <= 0) { if (len) *len = 0; return; }
    size_t n = strlen(s->log);
    if (n >= (size_t)max) n = (size_t)max - 1;
    memcpy(log, s->log, n); log[n] = 0;
    if (len) *len = (GLsizei)n;
}

GLuint glCreateProgram(void)
{
    CTXV(0);
    struct zgl_program *p = calloc(1, sizeof *p);
    p->name = new_name();
    for (int i = 0; i < ZGL_MAX_VARS; i++) p->attrib_loc[i] = -1;
    LIST_ADD(zgl->programs, zgl->nprograms, p);
    return p->name;
}

static void program_free(struct zgl_program *p)
{
    if (zgl->program == p) zgl->program = NULL;
    if (zgl->ffp_program == p) zgl->ffp_program = NULL;
    if (p->h_vs) virgl_destroy_object(zgl->v, p->h_vs, VIRGL_OBJECT_SHADER);
    if (p->h_fs) virgl_destroy_object(zgl->v, p->h_fs, VIRGL_OBJECT_SHADER);
    if (p->vs) shader_unref(p->vs);
    if (p->fs) shader_unref(p->fs);
    LIST_DEL(zgl->programs, zgl->nprograms, p);
    free(p);
}

void glDeleteProgram(GLuint name) { CTX(); struct zgl_program *p = program_get(name); if (!p) return; if (zgl->program == p) { p->deleted = 1; return; } program_free(p); }

void glAttachShader(GLuint pn, GLuint sn)
{
    CTX();
    struct zgl_program *p = program_get(pn);
    struct zgl_shader *s = shader_get(sn);
    if (!p || !s) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_shader **slot = s->type == GL_VERTEX_SHADER ? &p->vs : &p->fs;
    if (*slot) shader_unref(*slot);
    *slot = s;
    s->refs++;
}

void glDetachShader(GLuint pn, GLuint sn)
{
    CTX();
    struct zgl_program *p = program_get(pn);
    struct zgl_shader *s = shader_get(sn);
    if (!p || !s) return;
    if (p->vs == s) { p->vs = NULL; shader_unref(s); }
    else if (p->fs == s) { p->fs = NULL; shader_unref(s); }
}

void glBindAttribLocation(GLuint pn, GLuint index, const GLchar *name)
{
    CTX();
    struct zgl_program *p = program_get(pn);
    if (!p || index >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; }
    strncpy(p->bound_attr[index], name, 47);
    p->bound_attr[index][47] = 0;
}

static struct zgl_uniform *program_uniform_add(struct zgl_program *p, const struct zgl_var *v, int is_vs)
{
    for (int i = 0; i < p->nuniforms; i++)
        if (strcmp(p->uniforms[i].name, v->name) == 0) {
            if (is_vs) p->uniforms[i].vs_slot = v->slot; else p->uniforms[i].fs_slot = v->slot;
            return &p->uniforms[i];
        }
    if (p->nuniforms >= ZGL_MAX_VARS * 2) return NULL;
    struct zgl_uniform *u = &p->uniforms[p->nuniforms++];
    memset(u, 0, sizeof *u);
    strncpy(u->name, v->name, 47);
    u->type = v->type; u->array = v->array; u->slots = v->slots; u->builtin = v->builtin;
    u->vs_slot = is_vs ? v->slot : -1;
    u->fs_slot = is_vs ? -1 : v->slot;
    return u;
}

int zgl_link(struct zgl_program *p)
{
    p->linked = 0;
    p->log[0] = 0;
    if (!p->vs || !p->fs) { snprintf(p->log, sizeof p->log, "a vertex and a fragment shader are required\n"); return -1; }
    zgl_compiled_free(&p->vs->c); zgl_compiled_free(&p->fs->c);
    char log[512];
    if (zgl_compile(p->vs->source, GL_VERTEX_SHADER, &p->vs->c, log, sizeof log) != 0) { snprintf(p->log, sizeof p->log, "vertex shader: %s", log); return -1; }
    if (zgl_compile_linked(p->fs->source, GL_FRAGMENT_SHADER, &p->vs->c, &p->fs->c, log, sizeof log) != 0) { snprintf(p->log, sizeof p->log, "fragment shader: %s", log); return -1; }
    /* host objects */
    if (p->h_vs) virgl_destroy_object(zgl->v, p->h_vs, VIRGL_OBJECT_SHADER);
    if (p->h_fs) virgl_destroy_object(zgl->v, p->h_fs, VIRGL_OBJECT_SHADER);
    p->h_vs = virgl_handle(zgl->v);
    p->h_fs = virgl_handle(zgl->v);
    virgl_create_shader(zgl->v, p->h_vs, PIPE_SHADER_VERTEX, p->vs->c.tgsi);
    virgl_create_shader(zgl->v, p->h_fs, PIPE_SHADER_FRAGMENT, p->fs->c.tgsi);
    /* uniforms */
    p->nuniforms = 0;
    for (int i = 0; i < p->vs->c.nuniforms; i++) program_uniform_add(p, &p->vs->c.uniforms[i], 1);
    for (int i = 0; i < p->fs->c.nuniforms; i++) program_uniform_add(p, &p->fs->c.uniforms[i], 0);
    for (int i = 0; i < p->fs->c.nsamplers; i++) {
        const struct zgl_var *v = &p->fs->c.samplers[i];
        struct zgl_uniform *u = program_uniform_add(p, v, 0);
        if (u) { u->fs_slot = -1; u->sampler_unit = 0; u->slots = 0; }
    }
    for (int i = 0; i < p->vs->c.nsamplers; i++) program_uniform_add(p, &p->vs->c.samplers[i], 1);
    p->vs_nconsts = p->vs->c.nconsts; p->fs_nconsts = p->fs->c.nconsts;
    memset(p->vs_consts, 0, sizeof p->vs_consts); memset(p->fs_consts, 0, sizeof p->fs_consts);
    p->consts_dirty = 1;
    /* attribute locations: bound names first, gl_* built-ins fixed, the rest in order */
    uint32_t used = 0;
    p->attrib_mask = 0;
    for (int i = 0; i < p->vs->c.nattribs; i++) p->attrib_loc[i] = -1;
    for (int i = 0; i < p->vs->c.nattribs; i++) {
        const struct zgl_var *a = &p->vs->c.attribs[i];
        if (a->builtin) { p->attrib_loc[i] = a->builtin - 1; used |= 1u << (a->builtin - 1); continue; }
        for (int k = 0; k < ZGL_MAX_ATTRIBS; k++)
            if (p->bound_attr[k][0] && strcmp(p->bound_attr[k], a->name) == 0) { p->attrib_loc[i] = k; used |= 1u << k; break; }
    }
    for (int i = 0; i < p->vs->c.nattribs; i++) {
        if (p->attrib_loc[i] >= 0) continue;
        for (int k = 0; k < ZGL_MAX_ATTRIBS; k++)
            if (!(used & (1u << k))) { p->attrib_loc[i] = k; used |= 1u << k; break; }
        if (p->attrib_loc[i] < 0) { snprintf(p->log, sizeof p->log, "too many attributes\n"); return -1; }
    }
    p->attrib_mask = used;
    p->linked = 1;
    return 0;
}

void glLinkProgram(GLuint name) { CTX(); struct zgl_program *p = program_get(name); if (!p) { zgl_set_error(GL_INVALID_VALUE); return; } zgl_link(p); }
void glValidateProgram(GLuint name) { (void)name; }

void glUseProgram(GLuint name)
{
    CTX();
    struct zgl_program *old = zgl->program;
    struct zgl_program *p = name ? program_get(name) : NULL;
    if (name && !p) { zgl_set_error(GL_INVALID_VALUE); return; }
    zgl->program = p;
    if (old && old != p && old->deleted) program_free(old);
}

void glGetProgramiv(GLuint name, GLenum pname, GLint *v)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    switch (pname) {
    case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *v = p->linked; break;
    case GL_INFO_LOG_LENGTH: *v = (GLint)strlen(p->log) + 1; break;
    case GL_ATTACHED_SHADERS: *v = (p->vs != NULL) + (p->fs != NULL); break;
    case GL_ACTIVE_UNIFORMS: *v = p->nuniforms; break;
    case GL_ACTIVE_ATTRIBUTES: *v = p->vs ? p->vs->c.nattribs : 0; break;
    case GL_DELETE_STATUS: *v = p->deleted; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}

void glGetProgramInfoLog(GLuint name, GLsizei max, GLsizei *len, GLchar *log)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p || max <= 0) { if (len) *len = 0; return; }
    size_t n = strlen(p->log);
    if (n >= (size_t)max) n = (size_t)max - 1;
    memcpy(log, p->log, n); log[n] = 0;
    if (len) *len = (GLsizei)n;
}

GLint glGetAttribLocation(GLuint name, const GLchar *attr)
{
    CTXV(-1);
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    for (int i = 0; i < p->vs->c.nattribs; i++)
        if (strcmp(p->vs->c.attribs[i].name, attr) == 0) return p->attrib_loc[i];
    return -1;
}

/* uniform locations: index into the program's table, with array element offsets in the high bits */
GLint glGetUniformLocation(GLuint name, const GLchar *uname)
{
    CTXV(-1);
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    char base[48]; int elem = 0;
    strncpy(base, uname, 47); base[47] = 0;
    char *br = strchr(base, '[');
    if (br) { elem = atoi(br + 1); *br = 0; }
    for (int i = 0; i < p->nuniforms; i++)
        if (strcmp(p->uniforms[i].name, base) == 0) {
            if (p->uniforms[i].array && elem >= p->uniforms[i].array) return -1;
            return i | (elem << 16);
        }
    return -1;
}

static int type_slots(enum zgl_type t) { return t == T_MAT2 ? 2 : t == T_MAT3 ? 3 : t == T_MAT4 ? 4 : 1; }

/* Store `count` values (each `stride_floats` wide, one per vec4 slot) into the uniform's slots. */
void zgl_uniform_set(struct zgl_program *p, int loc, const float *data, int count, int is_matrix_cols)
{
    int idx = loc & 0xFFFF, elem = loc >> 16;
    if (idx < 0 || idx >= p->nuniforms) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_uniform *u = &p->uniforms[idx];
    int per = type_slots(u->type);                  /* vec4 slots per element */
    int width = is_matrix_cols ? per : (u->type == T_FLOAT || u->type == T_INT || u->type == T_BOOL ? 1 : u->type == T_VEC2 || u->type == T_IVEC2 || u->type == T_BVEC2 ? 2 : u->type == T_VEC3 || u->type == T_IVEC3 || u->type == T_BVEC3 ? 3 : 4);
    int max_elems = u->array ? u->array : 1;
    for (int e = 0; e < count && elem + e < max_elems; e++) {
        for (int col = 0; col < per; col++) {
            const float *src = data + (size_t)e * (is_matrix_cols ? per * per : width) + (is_matrix_cols ? col * per : 0);
            int n = is_matrix_cols ? per : width;
            int slot = (elem + e) * per + col;
            if (u->vs_slot >= 0 && u->vs_slot + slot < ZGL_MAX_CONSTS) memcpy(&p->vs_consts[(u->vs_slot + slot) * 4], src, (size_t)n * 4);
            if (u->fs_slot >= 0 && u->fs_slot + slot < ZGL_MAX_CONSTS) memcpy(&p->fs_consts[(u->fs_slot + slot) * 4], src, (size_t)n * 4);
        }
    }
    p->consts_dirty = 1;
}

static struct zgl_program *uniform_target(void) { return zgl->program; }

void glUniform1f(GLint loc, GLfloat x) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; float v[4] = { x, 0, 0, 0 }; zgl_uniform_set(p, loc, v, 1, 0); }
void glUniform2f(GLint loc, GLfloat x, GLfloat y) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; float v[4] = { x, y, 0, 0 }; zgl_uniform_set(p, loc, v, 1, 0); }
void glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; float v[4] = { x, y, z, 0 }; zgl_uniform_set(p, loc, v, 1, 0); }
void glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; float v[4] = { x, y, z, w }; zgl_uniform_set(p, loc, v, 1, 0); }
void glUniform1fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; zgl_uniform_set(p, loc, v, n, 0); }
void glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; zgl_uniform_set(p, loc, v, n, 0); }
void glUniform3fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; zgl_uniform_set(p, loc, v, n, 0); }
void glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); struct zgl_program *p = uniform_target(); if (!p || loc < 0) return; zgl_uniform_set(p, loc, v, n, 0); }

static void uniform_int(GLint loc, const GLint *iv, int width, int count)
{
    struct zgl_program *p = uniform_target();
    if (!p || loc < 0) return;
    int idx = loc & 0xFFFF;
    if (idx < p->nuniforms && (p->uniforms[idx].type == T_SAMPLER2D || p->uniforms[idx].type == T_SAMPLERCUBE)) {
        p->uniforms[idx].sampler_unit = iv[0];
        return;
    }
    float f[64 * 4];
    for (int i = 0; i < count * width && i < 256; i++) f[i] = (float)iv[i];
    zgl_uniform_set(p, loc, f, count, 0);
}
void glUniform1i(GLint loc, GLint x) { CTX(); uniform_int(loc, &x, 1, 1); }
void glUniform2i(GLint loc, GLint x, GLint y) { CTX(); GLint v[2] = { x, y }; uniform_int(loc, v, 2, 1); }
void glUniform3i(GLint loc, GLint x, GLint y, GLint z) { CTX(); GLint v[3] = { x, y, z }; uniform_int(loc, v, 3, 1); }
void glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w) { CTX(); GLint v[4] = { x, y, z, w }; uniform_int(loc, v, 4, 1); }
void glUniform1iv(GLint loc, GLsizei n, const GLint *v) { CTX(); uniform_int(loc, v, 1, n); }

static void uniform_matrix(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v, int dim)
{
    struct zgl_program *p = uniform_target();
    if (!p || loc < 0) return;
    if (!transpose) { zgl_uniform_set(p, loc, v, n, 1); return; }
    float t[16 * 8];
    for (GLsizei e = 0; e < n && e < 8; e++)
        for (int i = 0; i < dim; i++)
            for (int j = 0; j < dim; j++)
                t[e * dim * dim + i * dim + j] = v[e * dim * dim + j * dim + i];
    zgl_uniform_set(p, loc, t, n, 1);
}
void glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 2); }
void glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 3); }
void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 4); }

/* ---- vertex attributes -------------------------------------------------------- */

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *p)
{
    CTX();
    if (index >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_attrib *a = &zgl->attribs[index];
    a->size = size; a->type = type; a->normalized = normalized; a->stride = stride; a->pointer = p;
    a->buffer = zgl->array_buffer;
}
void glEnableVertexAttribArray(GLuint index) { CTX(); if (index < ZGL_MAX_ATTRIBS) zgl->attribs[index].enabled = 1; }
void glDisableVertexAttribArray(GLuint index) { CTX(); if (index < ZGL_MAX_ATTRIBS) zgl->attribs[index].enabled = 0; }
void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { CTX(); if (index >= ZGL_MAX_ATTRIBS) return; float *v = zgl->attribs[index].value; v[0] = x; v[1] = y; v[2] = z; v[3] = w; }
void glVertexAttrib1f(GLuint i, GLfloat x) { glVertexAttrib4f(i, x, 0, 0, 1); }
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { glVertexAttrib4f(i, x, y, 0, 1); }
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { glVertexAttrib4f(i, x, y, z, 1); }
void glVertexAttrib4fv(GLuint i, const GLfloat *v) { glVertexAttrib4f(i, v[0], v[1], v[2], v[3]); }

void glDrawArrays(GLenum mode, GLint first, GLsizei count) { CTX(); zgl_draw(mode, first, count, 0, NULL); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) { CTX(); zgl_draw(mode, 0, count, type, indices); }

/* ---- clear, readback ------------------------------------------------------------ */

void zgl_apply_framebuffer(void);           /* draw.c */

void glClear(GLbitfield mask)
{
    CTX();
    zgl_apply_framebuffer();
    uint32_t buffers = 0;
    if (mask & GL_COLOR_BUFFER_BIT) buffers |= PIPE_CLEAR_COLOR0;
    if (mask & GL_DEPTH_BUFFER_BIT) buffers |= PIPE_CLEAR_DEPTH;
    if (mask & GL_STENCIL_BUFFER_BIT) buffers |= PIPE_CLEAR_STENCIL;
    if (!buffers) return;
    virgl_clear(zgl->v, buffers, zgl->clear_color, zgl->clear_depth, (uint32_t)zgl->clear_stencil);
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    CTX();
    if (type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_RGB && format != GL_BGRA)) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (zgl->fbo) { zgl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION); return; }     /* textures have no guest copy to read */
    if (x < 0 || y < 0 || x + w > zgl->width || y + h > zgl->height) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (virgl_res_from_host(zgl->v, zgl->color, 0, (uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1,
                            (uint32_t)zgl->width * 4, (uint32_t)zgl->width * zgl->height * 4, (uint64_t)(y * zgl->width + x) * 4) != 0) return;
    const uint32_t *src = zgl->color->map;
    uint8_t *dst = pixels;
    int comps = format == GL_RGB ? 3 : 4;
    size_t row = ((size_t)(zgl->pack_row_length > 0 ? zgl->pack_row_length : w) * comps + zgl->pack_alignment - 1) & ~(size_t)(zgl->pack_alignment - 1);
    for (GLsizei j = 0; j < h; j++) {
        const uint32_t *s = src + (size_t)(y + j) * zgl->width + x;
        uint8_t *d = dst + j * row;
        for (GLsizei i = 0; i < w; i++, d += comps) {
            uint32_t p = s[i];
            if (format == GL_BGRA) { d[0] = (uint8_t)p; d[1] = (uint8_t)(p >> 8); d[2] = (uint8_t)(p >> 16); d[3] = 255; }
            else { d[0] = (uint8_t)(p >> 16); d[1] = (uint8_t)(p >> 8); d[2] = (uint8_t)p; if (comps == 4) d[3] = 255; }
        }
    }
}

/* ---- framebuffer objects, copies ------------------------------------------------- */

static struct zgl_fbo *fbo_get(GLuint name)
{
    for (int i = 0; i < zgl->nfbos; i++) if (zgl->fbos[i]->name == name) return zgl->fbos[i];
    return NULL;
}

void glGenFramebuffers(GLsizei n, GLuint *ids) { CTX(); for (GLsizei i = 0; i < n; i++) ids[i] = new_name(); }
GLboolean glIsFramebuffer(GLuint id) { CTXV(0); return fbo_get(id) != NULL; }

void glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_fbo *f = fbo_get(ids[i]);
        if (!f) continue;
        if (zgl->fbo == f) { zgl->fbo = NULL; zgl->fb_dirty = 1; }
        if (f->h_surf) virgl_destroy_object(zgl->v, f->h_surf, VIRGL_OBJECT_SURFACE);
        LIST_DEL(zgl->fbos, zgl->nfbos, f);
        free(f);
    }
}

void glBindFramebuffer(GLenum target, GLuint id)
{
    CTX();
    if (target != GL_FRAMEBUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_fbo *f = NULL;
    if (id) {
        f = fbo_get(id);
        if (!f) {
            f = calloc(1, sizeof *f);
            if (!f) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
            f->name = id;
            LIST_ADD(zgl->fbos, zgl->nfbos, f);
        }
    }
    if (zgl->fbo != f) { zgl->fbo = f; zgl->fb_dirty = 1; }
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    CTX();
    if (target != GL_FRAMEBUFFER || attachment != GL_COLOR_ATTACHMENT0 || (texture && textarget != GL_TEXTURE_2D)) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_fbo *f = zgl->fbo;
    if (!f) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_texture *tex = texture ? texture_get(texture) : NULL;
    if (texture && !tex) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (f->h_surf) { virgl_destroy_object(zgl->v, f->h_surf, VIRGL_OBJECT_SURFACE); f->h_surf = 0; }
    f->tex = tex; f->level = level;
    if (tex) tex->has_level0 = 1;           /* rendered into: sampleable even if never uploaded */
    zgl->fb_dirty = 1;
}

GLenum glCheckFramebufferStatus(GLenum target)
{
    CTXV(0);
    if (target != GL_FRAMEBUFFER) { zgl_set_error(GL_INVALID_ENUM); return 0; }
    if (!zgl->fbo) return GL_FRAMEBUFFER_COMPLETE;
    return zgl->fbo->tex && zgl->fbo->tex->res ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_UNSUPPORTED;
}

/* Copy from the current colour buffer into the bound texture: a host blit. */
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei w, GLsizei h)
{
    CTX();
    if (target != GL_TEXTURE_2D) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_texture *tex = zgl->unit_tex[zgl->active_unit];
    if (!tex || !tex->res || level >= tex->levels) { zgl_set_error(GL_INVALID_OPERATION); return; }
    zgl_apply_framebuffer();
    uint32_t src_res = zgl->fbo && zgl->fbo->tex && zgl->fbo->tex->res ? zgl->fbo->tex->res->id : zgl->color->id;
    uint32_t src_level = zgl->fbo ? (uint32_t)zgl->fbo->level : 0;
    uint32_t src_format = zgl->fbo && zgl->fbo->tex ? zgl->fbo->tex->vformat : VIRGL_FORMAT_B8G8R8A8_UNORM;
    virgl_cmd(zgl->v, VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE);
    virgl_dw(zgl->v, VIRGL_CMD_BLIT_S0_MASK(0xF) | VIRGL_CMD_BLIT_S0_FILTER(PIPE_TEX_FILTER_NEAREST));
    virgl_dw(zgl->v, 0); virgl_dw(zgl->v, 0);
    virgl_dw(zgl->v, tex->res->id); virgl_dw(zgl->v, (uint32_t)level); virgl_dw(zgl->v, tex->vformat);
    virgl_dw(zgl->v, (uint32_t)xoffset); virgl_dw(zgl->v, (uint32_t)yoffset); virgl_dw(zgl->v, 0); virgl_dw(zgl->v, (uint32_t)w); virgl_dw(zgl->v, (uint32_t)h); virgl_dw(zgl->v, 1);
    virgl_dw(zgl->v, src_res); virgl_dw(zgl->v, src_level); virgl_dw(zgl->v, src_format);
    virgl_dw(zgl->v, (uint32_t)x); virgl_dw(zgl->v, (uint32_t)y); virgl_dw(zgl->v, 0); virgl_dw(zgl->v, (uint32_t)w); virgl_dw(zgl->v, (uint32_t)h); virgl_dw(zgl->v, 1);
    tex->has_level0 = 1;
}

void glClipPlane(GLenum plane, const GLdouble *equation)
{
    CTX();
    (void)equation;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + 6) zgl_set_error(GL_INVALID_ENUM);
    /* accepted so fixed-function code runs; clipping itself is not implemented */
}

/* ---- GL_ARB_shader_objects: the untyped "object" calls (GL/glew.h maps them here) ---- */

void zgl_GetInfoLogARB(GLuint obj, GLsizei maxlen, GLsizei *len, char *log)
{
    CTX();
    if (shader_get(obj)) glGetShaderInfoLog(obj, maxlen, len, log);
    else glGetProgramInfoLog(obj, maxlen, len, log);
}

void zgl_GetObjectParameterivARB(GLuint obj, GLenum pname, GLint *v)
{
    CTX();
    if (shader_get(obj)) glGetShaderiv(obj, pname, v);
    else glGetProgramiv(obj, pname, v);
}

void zgl_DeleteObjectARB(GLuint obj)
{
    CTX();
    if (shader_get(obj)) glDeleteShader(obj);
    else glDeleteProgram(obj);
}
