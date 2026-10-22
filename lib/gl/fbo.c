/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Framebuffers: framebuffer objects with up to 8 colour attachments and a
 * depth/stencil one (textures of any target, layers, or renderbuffers),
 * renderbuffers (multisampled too), draw and read buffers, clears (whole
 * or scissored), blits, and reading pixels back. */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX() do { if (!zgl) return; } while (0)
#define CTXV(v) do { if (!zgl) return (v); } while (0)
#define LIST_ADD(list, n, item) do { (list) = realloc((list), sizeof *(list) * ((n) + 1)); (list)[(n)++] = (item); } while (0)
#define LIST_DEL(list, n, item) do { for (int _i = 0; _i < (n); _i++) if ((list)[_i] == (item)) { memmove(&(list)[_i], &(list)[_i + 1], sizeof *(list) * ((n) - _i - 1)); (n)--; break; } } while (0)

static int is_depth_kind(int k) { return k == ZF_DEPTH || k == ZF_DEPTH_STENCIL || k == ZF_STENCIL; }

/* the window's buffers, as formats */
static const struct zgl_format *window_color_format(void) { return zgl_format_of(GL_RGBA8, 0, 0); }
static struct zgl_format window_depth_fmt;
static const struct zgl_format *window_depth_format(void)
{
    window_depth_fmt = *zgl_format_of(GL_DEPTH24_STENCIL8, 0, 0);
    window_depth_fmt.vformat = zgl->depth_format;
    window_depth_fmt.kind = zgl->depth_format == VIRGL_FORMAT_Z24X8_UNORM || zgl->depth_format == VIRGL_FORMAT_Z32_FLOAT || zgl->depth_format == VIRGL_FORMAT_Z16_UNORM ? ZF_DEPTH : ZF_DEPTH_STENCIL;
    window_depth_fmt.bpp = zgl->depth_format == VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT ? 8 : zgl->depth_format == VIRGL_FORMAT_Z16_UNORM ? 2 : 4;
    return &window_depth_fmt;
}

/* ---- objects ----------------------------------------------------------------------- */

struct zgl_fbo *zgl_fbo_get(GLuint name)
{
    for (int i = 0; i < zgl->nfbos; i++) if (zgl->fbos[i]->name == name) return zgl->fbos[i];
    return NULL;
}

struct zgl_renderbuffer *zgl_rb_get(GLuint name)
{
    for (int i = 0; i < zgl->nrbs; i++) if (zgl->rbs[i]->name == name) return zgl->rbs[i];
    return NULL;
}

static void attachment_drop_surface(struct zgl_attachment *a)
{
    if (a->h_surf) { virgl_destroy_object(zgl->v, a->h_surf, VIRGL_OBJECT_SURFACE); a->h_surf = 0; a->surf_key = 0; }
}

static void attachment_clear(struct zgl_attachment *a)
{
    attachment_drop_surface(a);
    memset(a, 0, sizeof *a);
}

static void fbo_each_attachment(struct zgl_fbo *f, void (*fn)(struct zgl_attachment *, void *), void *arg)
{
    for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS; i++) fn(&f->color[i], arg);
    fn(&f->depth, arg);
    fn(&f->stencil, arg);
}

static void detach_tex_fn(struct zgl_attachment *a, void *arg)
{
    if (a->type == GL_TEXTURE && a->tex == arg) attachment_drop_surface(a);      /* remade on the new resource */
}

/* A texture's resource changed (or it is going away): surfaces made on it are stale. */
void zgl_detach_texture(struct zgl_texture *t)
{
    if (!zgl) return;
    for (int i = 0; i < zgl->nfbos; i++) fbo_each_attachment(zgl->fbos[i], detach_tex_fn, t);
    zgl->fb_dirty = 1;
}

static void forget_rb_fn(struct zgl_attachment *a, void *arg) { if (a->type == GL_RENDERBUFFER && a->rb == arg) attachment_clear(a); }

static struct zgl_fbo *fbo_new(GLuint name)
{
    struct zgl_fbo *f = calloc(1, sizeof *f);
    f->name = name;
    f->draw_buffers[0] = GL_COLOR_ATTACHMENT0; f->ndraw = 1;
    for (int i = 1; i < ZGL_MAX_DRAW_BUFFERS; i++) f->draw_buffers[i] = GL_NONE;
    f->read_buffer = GL_COLOR_ATTACHMENT0;
    LIST_ADD(zgl->fbos, zgl->nfbos, f);
    return f;
}

void glGenFramebuffers(GLsizei n, GLuint *ids)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) ids[i] = fbo_new(zgl->next_name++)->name;
}

GLboolean glIsFramebuffer(GLuint id) { CTXV(0); return id && zgl_fbo_get(id) != NULL; }

static void drop_fn(struct zgl_attachment *a, void *arg) { (void)arg; attachment_clear(a); }

void glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_fbo *f = ids[i] ? zgl_fbo_get(ids[i]) : NULL;
        if (!f) continue;
        if (zgl->fbo == f) { zgl->fbo = NULL; zgl->fb_dirty = 1; }
        if (zgl->read_fbo == f) zgl->read_fbo = NULL;
        fbo_each_attachment(f, drop_fn, NULL);
        LIST_DEL(zgl->fbos, zgl->nfbos, f);
        free(f);
    }
}

void glBindFramebuffer(GLenum target, GLuint id)
{
    CTX();
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_fbo *f = NULL;
    if (id) {
        f = zgl_fbo_get(id);
        if (!f) { f = fbo_new(id); if (id >= zgl->next_name) zgl->next_name = id + 1; }
    }
    if (target != GL_READ_FRAMEBUFFER && zgl->fbo != f) { zgl->fbo = f; zgl->fb_dirty = 1; }
    if (target != GL_DRAW_FRAMEBUFFER) zgl->read_fbo = f;
}

static struct zgl_fbo *target_fbo(GLenum target)
{
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) return zgl->fbo;
    if (target == GL_READ_FRAMEBUFFER) return zgl->read_fbo;
    zgl_set_error(GL_INVALID_ENUM);
    return NULL;
}

/* the attachment point an enum names; NULL if it is not one. DEPTH_STENCIL is depth. */
static struct zgl_attachment *attachment_of(struct zgl_fbo *f, GLenum att)
{
    if (att >= GL_COLOR_ATTACHMENT0 && att < GL_COLOR_ATTACHMENT0 + ZGL_MAX_DRAW_BUFFERS) return &f->color[att - GL_COLOR_ATTACHMENT0];
    if (att == GL_DEPTH_ATTACHMENT || att == GL_DEPTH_STENCIL_ATTACHMENT) return &f->depth;
    if (att == GL_STENCIL_ATTACHMENT) return &f->stencil;
    return NULL;
}

static void set_attachment(GLenum target, GLenum att, GLenum type, struct zgl_texture *t, struct zgl_renderbuffer *rb, int level, int layer)
{
    struct zgl_fbo *f = target_fbo(target);
    if (!f) { if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_attachment *a = attachment_of(f, att);
    if (!a) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_attachment n = { 0 };
    if (t || rb) { n.type = type; n.tex = t; n.rb = rb; n.level = level; n.layer = layer; }
    attachment_clear(a);
    *a = n;
    if (att == GL_DEPTH_STENCIL_ATTACHMENT) { attachment_clear(&f->stencil); f->stencil = n; f->stencil.h_surf = 0; }
    if (t) t->has_level0 = 1;                         /* rendered into: sampleable even if never uploaded */
    if (f == zgl->fbo) zgl->fb_dirty = 1;
}

static struct zgl_texture *attach_texture_obj(GLuint texture, GLenum textarget, int need_target)
{
    if (!texture) return NULL;
    struct zgl_texture *t = zgl_texture_get(texture);
    if (!t || !t->target) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    if (need_target && zgl_target_index(textarget) != zgl_target_index(t->target)) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    return t;
}

void glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint texture, GLint level)
{
    CTX();
    struct zgl_texture *t = attach_texture_obj(texture, textarget, 1);
    if (texture && !t) return;
    int layer = 0;
    if (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) layer = (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    else if (texture && textarget != GL_TEXTURE_2D && textarget != GL_TEXTURE_RECTANGLE && textarget != GL_TEXTURE_2D_MULTISAMPLE) { zgl_set_error(GL_INVALID_OPERATION); return; }
    set_attachment(target, att, GL_TEXTURE, t, NULL, level, layer);
}

void glFramebufferTexture1D(GLenum target, GLenum att, GLenum textarget, GLuint texture, GLint level)
{
    CTX();
    struct zgl_texture *t = attach_texture_obj(texture, textarget, 1);
    if (texture && (!t || textarget != GL_TEXTURE_1D)) { if (t) zgl_set_error(GL_INVALID_OPERATION); return; }
    set_attachment(target, att, GL_TEXTURE, t, NULL, level, 0);
}

void glFramebufferTexture3D(GLenum target, GLenum att, GLenum textarget, GLuint texture, GLint level, GLint zoff)
{
    CTX();
    struct zgl_texture *t = attach_texture_obj(texture, textarget, 1);
    if (texture && (!t || textarget != GL_TEXTURE_3D)) { if (t) zgl_set_error(GL_INVALID_OPERATION); return; }
    set_attachment(target, att, GL_TEXTURE, t, NULL, level, zoff);
}

void glFramebufferTextureLayer(GLenum target, GLenum att, GLuint texture, GLint level, GLint layer)
{
    CTX();
    struct zgl_texture *t = attach_texture_obj(texture, 0, 0);
    if (texture && !t) return;
    if (t && t->target != GL_TEXTURE_3D && t->target != GL_TEXTURE_2D_ARRAY && t->target != GL_TEXTURE_1D_ARRAY &&
        t->target != GL_TEXTURE_2D_MULTISAMPLE_ARRAY && t->target != GL_TEXTURE_CUBE_MAP && t->target != 0x9009) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (layer < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    set_attachment(target, att, GL_TEXTURE, t, NULL, level, layer);
}

void glFramebufferTexture(GLenum target, GLenum att, GLuint texture, GLint level)
{
    CTX();
    struct zgl_texture *t = attach_texture_obj(texture, 0, 0);
    if (texture && !t) return;
    int layered = t && (t->target == GL_TEXTURE_3D || t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_CUBE_MAP ||
                        t->target == GL_TEXTURE_1D_ARRAY || t->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY || t->target == 0x9009);
    set_attachment(target, att, GL_TEXTURE, t, NULL, level, layered ? -1 : 0);
}

void glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbtarget, GLuint rb)
{
    CTX();
    if (rbtarget != GL_RENDERBUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_renderbuffer *r = rb ? zgl_rb_get(rb) : NULL;
    if (rb && !r) { zgl_set_error(GL_INVALID_OPERATION); return; }
    set_attachment(target, att, GL_RENDERBUFFER, NULL, r, 0, 0);
}

/* ---- attachment properties ------------------------------------------------------------ */

static virgl_res *attachment_res(const struct zgl_attachment *a)
{
    if (a->type == GL_TEXTURE && a->tex) return a->tex->res;
    if (a->type == GL_RENDERBUFFER && a->rb) return a->rb->res;
    return NULL;
}

static const struct zgl_format *attachment_format(const struct zgl_attachment *a)
{
    if (a->type == GL_TEXTURE && a->tex) return a->tex->fmt;
    if (a->type == GL_RENDERBUFFER && a->rb) return a->rb->fmt;
    return NULL;
}

static void attachment_size(const struct zgl_attachment *a, int *w, int *h)
{
    *w = *h = 0;
    if (a->type == GL_TEXTURE && a->tex && a->tex->res) {
        *w = a->tex->w >> a->level; *h = a->tex->target == GL_TEXTURE_1D || a->tex->target == GL_TEXTURE_1D_ARRAY ? 1 : a->tex->h >> a->level;
        if (*w < 1) *w = 1;
        if (*h < 1) *h = 1;
    } else if (a->type == GL_RENDERBUFFER && a->rb && a->rb->res) { *w = a->rb->w; *h = a->rb->h; }
}

static int attachment_samples(const struct zgl_attachment *a)
{
    if (a->type == GL_TEXTURE && a->tex) return a->tex->samples;
    if (a->type == GL_RENDERBUFFER && a->rb) return a->rb->samples;
    return 0;
}

static uint32_t linear_of(uint32_t vf)
{
    switch (vf) {
    case VIRGL_FORMAT_B8G8R8A8_SRGB: return VIRGL_FORMAT_B8G8R8A8_UNORM;
    case VIRGL_FORMAT_R8G8B8A8_SRGB: return VIRGL_FORMAT_R8G8B8A8_UNORM;
    }
    return vf;
}

/* The host surface for an attachment, made (again) when the resource, level,
 * layer, or sRGB writing changed. */
uint32_t zgl_attachment_surface(struct zgl_attachment *a)
{
    virgl_res *res = attachment_res(a);
    const struct zgl_format *f = attachment_format(a);
    if (!res || !f) return 0;
    uint32_t vf = f->vformat;
    if (f->kind == ZF_SRGB && !zgl->framebuffer_srgb) vf = linear_of(vf);
    uint32_t layers;
    if (a->layer < 0) {
        int n = a->tex->target == GL_TEXTURE_CUBE_MAP ? 6 : a->tex->target == GL_TEXTURE_1D_ARRAY ? a->tex->h :
                a->tex->target == GL_TEXTURE_3D ? (a->tex->d >> a->level ? a->tex->d >> a->level : 1) : a->tex->d;
        layers = (uint32_t)(n - 1) << 16;
    } else layers = (uint32_t)a->layer | (uint32_t)a->layer << 16;
    uint64_t key = (uint64_t)res->id << 40 ^ (uint64_t)vf << 32 ^ (uint64_t)a->level << 24 ^ layers;
    if (a->h_surf && a->surf_key == key) return a->h_surf;
    attachment_drop_surface(a);
    a->h_surf = virgl_handle(zgl->v);
    virgl_create_surface(zgl->v, a->h_surf, res->id, vf, (uint32_t)a->level, layers);
    a->surf_key = key;
    return a->h_surf;
}

static GLenum fbo_status(struct zgl_fbo *f)
{
    if (!f) return GL_FRAMEBUFFER_COMPLETE;
    int any = 0, samples = -1;     /* attachments may differ in size (GL 3): the draw covers what they share */
    for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS + 2; i++) {
        struct zgl_attachment *a = i < ZGL_MAX_DRAW_BUFFERS ? &f->color[i] : i == ZGL_MAX_DRAW_BUFFERS ? &f->depth : &f->stencil;
        if (a->type == GL_NONE) continue;
        virgl_res *res = attachment_res(a);
        const struct zgl_format *fm = attachment_format(a);
        if (!res || !fm) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (a->type == GL_TEXTURE && a->tex->target != GL_TEXTURE_BUFFER && (a->level < 0 || a->level >= a->tex->levels)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (i < ZGL_MAX_DRAW_BUFFERS && (is_depth_kind(fm->kind) || fm->kind == ZF_COMPRESSED)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (i == ZGL_MAX_DRAW_BUFFERS && fm->kind != ZF_DEPTH && fm->kind != ZF_DEPTH_STENCIL) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        if (i == ZGL_MAX_DRAW_BUFFERS + 1 && fm->kind != ZF_STENCIL && fm->kind != ZF_DEPTH_STENCIL) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        int s = attachment_samples(a);
        if (samples < 0) samples = s;
        else if (samples != s) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
        any = 1;
    }
    if (!any) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if (f->depth.type && f->stencil.type && (attachment_res(&f->depth) != attachment_res(&f->stencil))) return GL_FRAMEBUFFER_UNSUPPORTED;
    return GL_FRAMEBUFFER_COMPLETE;
}

GLenum glCheckFramebufferStatus(GLenum target)
{
    CTXV(0);
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { zgl_set_error(GL_INVALID_ENUM); return 0; }
    return fbo_status(target == GL_READ_FRAMEBUFFER ? zgl->read_fbo : zgl->fbo);
}

void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum att, GLenum pname, GLint *v)
{
    CTX();
    struct zgl_fbo *f = target_fbo(target);
    if (zgl->error == GL_INVALID_ENUM && !f && target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) return;
    const struct zgl_format *fm = NULL;
    struct zgl_attachment *a = NULL;
    GLenum type;
    if (!f) {                                           /* the window */
        if (att == GL_BACK_LEFT || att == GL_FRONT_LEFT || att == GL_BACK || att == GL_FRONT) fm = window_color_format();
        else if (att == GL_DEPTH || att == GL_STENCIL) fm = window_depth_format();
        else { zgl_set_error(GL_INVALID_ENUM); return; }
        type = GL_FRAMEBUFFER_DEFAULT;
    } else {
        a = attachment_of(f, att);
        if (!a) { zgl_set_error(GL_INVALID_ENUM); return; }
        type = a->type;
        fm = attachment_format(a);
    }
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) { *v = (GLint)type; return; }
    if (type == GL_NONE) {
        if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) *v = 0; else zgl_set_error(GL_INVALID_OPERATION);
        return;
    }
    switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: *v = a ? (GLint)(a->tex ? a->tex->name : a->rb->name) : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL: if (!a || a->type != GL_TEXTURE) break; *v = a->level; return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
        if (!a || a->type != GL_TEXTURE) break;
        *v = a->tex->target == GL_TEXTURE_CUBE_MAP && a->layer >= 0 ? (GLint)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + a->layer) : 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER: if (!a || a->type != GL_TEXTURE) break; *v = a->layer < 0 ? 0 : a->layer; return;
    case GL_FRAMEBUFFER_ATTACHMENT_LAYERED: if (!a || a->type != GL_TEXTURE) break; *v = a->layer < 0; return;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: *v = fm && fm->kind == ZF_SRGB ? GL_SRGB : GL_LINEAR; return;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
        if (!fm) { *v = GL_NONE; return; }
        if (att == GL_DEPTH_STENCIL_ATTACHMENT) { zgl_set_error(GL_INVALID_OPERATION); return; }
        *v = fm->kind == ZF_FLOAT ? GL_FLOAT : fm->kind == ZF_INT ? GL_INT : fm->kind == ZF_UINT ? GL_UNSIGNED_INT : fm->kind == ZF_SNORM ? GL_SIGNED_NORMALIZED :
             (fm->internal == GL_DEPTH_COMPONENT32F || fm->internal == GL_DEPTH32F_STENCIL8) && att != GL_STENCIL_ATTACHMENT ? GL_FLOAT :
             att == GL_STENCIL_ATTACHMENT || att == GL_STENCIL ? GL_UNSIGNED_INT : GL_UNSIGNED_NORMALIZED;
        return;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: {
        /* the sizes the texture level would report */
        if (!fm) { *v = 0; return; }
        int k = (int)(pname - GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE);
        if (k < 4) {
            if (is_depth_kind(fm->kind)) { *v = 0; return; }
            int bits = fm->bpp * 8 / (fm->comps == 3 ? 4 : fm->comps);
            if (fm->internal == GL_RGB10_A2 || fm->internal == GL_RGB10_A2UI) bits = k == 3 ? 2 : 10;
            if (fm->internal == GL_R11F_G11F_B10F) bits = k == 2 ? 10 : k == 3 ? 0 : 11;
            *v = k < fm->comps || (k == 3 && fm->comps == 4) ? bits : 0;
            if (k == 3 && fm->comps == 3) *v = 0;
        } else if (k == 4) *v = fm->kind == ZF_DEPTH || fm->kind == ZF_DEPTH_STENCIL ? (fm->internal == GL_DEPTH_COMPONENT16 ? 16 : fm->internal == GL_DEPTH_COMPONENT32F || fm->internal == GL_DEPTH32F_STENCIL8 ? 32 : 24) : 0;
        else *v = fm->kind == ZF_DEPTH_STENCIL || fm->kind == ZF_STENCIL ? 8 : 0;
        return;
    }
    }
    zgl_set_error(GL_INVALID_ENUM);
}

/* ---- renderbuffers -------------------------------------------------------------------------- */

void glGenRenderbuffers(GLsizei n, GLuint *ids)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_renderbuffer *r = calloc(1, sizeof *r);
        r->name = zgl->next_name++;
        LIST_ADD(zgl->rbs, zgl->nrbs, r);
        ids[i] = r->name;
    }
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *ids)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_renderbuffer *r = ids[i] ? zgl_rb_get(ids[i]) : NULL;
        if (!r) continue;
        if (zgl->rb == r) zgl->rb = NULL;
        for (int k = 0; k < zgl->nfbos; k++) fbo_each_attachment(zgl->fbos[k], forget_rb_fn, r);
        zgl->fb_dirty = 1;
        if (r->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, r->res); }
        LIST_DEL(zgl->rbs, zgl->nrbs, r);
        free(r);
    }
}

GLboolean glIsRenderbuffer(GLuint id) { CTXV(0); return id && zgl_rb_get(id) != NULL; }

void glBindRenderbuffer(GLenum target, GLuint id)
{
    CTX();
    if (target != GL_RENDERBUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_renderbuffer *r = NULL;
    if (id) {
        r = zgl_rb_get(id);
        if (!r) {
            r = calloc(1, sizeof *r);
            r->name = id;
            LIST_ADD(zgl->rbs, zgl->nrbs, r);
            if (id >= zgl->next_name) zgl->next_name = id + 1;
        }
    }
    zgl->rb = r;
}

static void rb_detach_fn(struct zgl_attachment *a, void *arg) { if (a->type == GL_RENDERBUFFER && a->rb == arg) attachment_drop_surface(a); }

void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h)
{
    CTX();
    if (target != GL_RENDERBUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_renderbuffer *r = zgl->rb;
    if (!r) { zgl_set_error(GL_INVALID_OPERATION); return; }
    const struct zgl_format *f = zgl_format_of(internal, 0, 0);
    if (!f || f->kind == ZF_COMPRESSED) { zgl_set_error(GL_INVALID_ENUM); return; }
    const struct virgl_caps_v2 *caps = virgl_caps(zgl->v);
    int maxs = caps ? (int)caps->v1.max_samples : 4;
    if (w < 0 || h < 0 || samples < 0 || samples > (maxs > 0 ? maxs : 4)) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (r->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, r->res); r->res = NULL; }
    for (int k = 0; k < zgl->nfbos; k++) fbo_each_attachment(zgl->fbos[k], rb_detach_fn, r);
    zgl->fb_dirty = 1;
    /* a stencil-only renderbuffer becomes a depth-stencil one: hosts rarely render to S8 alone */
    if (f->kind == ZF_STENCIL) f = zgl_format_of(GL_DEPTH24_STENCIL8, 0, 0);
    if (samples > 1) f = zgl_format_ms(f);
    if (samples > 1 && !zgl_can_multisample(f->vformat)) { samples = 0; f = zgl_format_of(internal, 0, 0); }   /* GL_RENDERBUFFER_SAMPLES reports it */
    r->fmt = f; r->w = w; r->h = h; r->samples = samples > 1 ? samples : 0;
    if (!w || !h) return;
    struct gpu_res_create rc = { .target = PIPE_TEXTURE_2D, .format = f->vformat,
                                 .bind = is_depth_kind(f->kind) ? VIRGL_BIND_DEPTH_STENCIL : VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                 .width = (uint32_t)w, .height = (uint32_t)h, .depth = 1, .array_size = 1, .nr_samples = (uint32_t)r->samples };
    r->res = virgl_res_create_ex(zgl->v, &rc);
    if (!r->res && r->samples) {
        /* the host can't multisample this format (macOS GL through virglrenderer
         * can't at all): single-sampled, which GL_RENDERBUFFER_SAMPLES reports */
        r->samples = 0; rc.nr_samples = 0;
        r->fmt = zgl_format_of(internal, 0, 0);
        rc.format = r->fmt->vformat;
        r->res = virgl_res_create_ex(zgl->v, &rc);
    }
    if (!r->res) zgl_set_error(GL_OUT_OF_MEMORY);
}

void glRenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h) { glRenderbufferStorageMultisample(target, 0, internal, w, h); }

void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *v)
{
    CTX();
    if (target != GL_RENDERBUFFER) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_renderbuffer *r = zgl->rb;
    if (!r) { zgl_set_error(GL_INVALID_OPERATION); return; }
    const struct zgl_format *f = r->fmt;
    switch (pname) {
    case GL_RENDERBUFFER_WIDTH: *v = r->w; break;
    case GL_RENDERBUFFER_HEIGHT: *v = r->h; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: *v = f ? (GLint)f->internal : GL_RGBA4; break;
    case GL_RENDERBUFFER_SAMPLES: *v = r->samples; break;
    case GL_RENDERBUFFER_RED_SIZE: case GL_RENDERBUFFER_GREEN_SIZE: case GL_RENDERBUFFER_BLUE_SIZE: case GL_RENDERBUFFER_ALPHA_SIZE: {
        int k = (int)(pname - GL_RENDERBUFFER_RED_SIZE);
        *v = f && !is_depth_kind(f->kind) && (k < f->comps) ? f->bpp * 8 / (f->comps == 3 ? 4 : f->comps) : 0;
        break;
    }
    case GL_RENDERBUFFER_DEPTH_SIZE: *v = f && (f->kind == ZF_DEPTH || f->kind == ZF_DEPTH_STENCIL) ? (f->internal == GL_DEPTH_COMPONENT16 ? 16 : f->internal == GL_DEPTH_COMPONENT32F || f->internal == GL_DEPTH32F_STENCIL8 ? 32 : 24) : 0; break;
    case GL_RENDERBUFFER_STENCIL_SIZE: *v = f && f->kind == ZF_DEPTH_STENCIL ? 8 : 0; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}

/* ---- draw and read buffers ------------------------------------------------------------------ */

void glDrawBuffers(GLsizei n, const GLenum *bufs)
{
    CTX();
    if (n < 0 || n > ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_fbo *f = zgl->fbo;
    if (!f) {
        /* the window has one: GL_BACK (or GL_NONE) */
        if (n != 1 || (bufs[0] != GL_BACK && bufs[0] != GL_NONE && bufs[0] != GL_BACK_LEFT && bufs[0] != GL_FRONT_LEFT && bufs[0] != GL_FRONT)) { zgl_set_error(GL_INVALID_OPERATION); return; }
        zgl->win_draw_buffer = bufs[0] == GL_NONE ? GL_NONE : GL_BACK;
        zgl->fb_dirty = 1;
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        if (bufs[i] == GL_NONE) continue;
        if (bufs[i] < GL_COLOR_ATTACHMENT0 || bufs[i] >= GL_COLOR_ATTACHMENT0 + ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(bufs[i] == GL_BACK ? GL_INVALID_OPERATION : GL_INVALID_ENUM); return; }
        for (GLsizei j = 0; j < i; j++) if (bufs[j] == bufs[i]) { zgl_set_error(GL_INVALID_OPERATION); return; }
    }
    for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS; i++) f->draw_buffers[i] = i < n ? bufs[i] : GL_NONE;
    f->ndraw = n;
    zgl->fb_dirty = 1;
}

void glDrawBuffer(GLenum buf)
{
    CTX();
    if (!zgl->fbo) {
        if (buf != GL_NONE && buf != GL_BACK && buf != GL_FRONT && buf != GL_BACK_LEFT && buf != GL_FRONT_LEFT && buf != GL_FRONT_AND_BACK && buf != GL_LEFT) { zgl_set_error(GL_INVALID_ENUM); return; }
        zgl->win_draw_buffer = buf == GL_NONE ? GL_NONE : GL_BACK;
        zgl->fb_dirty = 1;
        return;
    }
    glDrawBuffers(1, &buf);
}

void glReadBuffer(GLenum buf)
{
    CTX();
    struct zgl_fbo *f = zgl->read_fbo;
    if (!f) {
        if (buf != GL_NONE && buf != GL_BACK && buf != GL_FRONT && buf != GL_BACK_LEFT && buf != GL_FRONT_LEFT && buf != GL_LEFT) { zgl_set_error(GL_INVALID_ENUM); return; }
        zgl->win_read_buffer = buf == GL_NONE ? GL_NONE : GL_BACK;
        return;
    }
    if (buf != GL_NONE && (buf < GL_COLOR_ATTACHMENT0 || buf >= GL_COLOR_ATTACHMENT0 + ZGL_MAX_DRAW_BUFFERS)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    f->read_buffer = buf;
}

/* ---- the framebuffer at draw time ------------------------------------------------------------ */

void zgl_rt_size(int *w, int *h)
{
    struct zgl_fbo *f = zgl->fbo;
    if (!f) { *w = zgl->width; *h = zgl->height; return; }
    int mw = 0, mh = 0, any = 0;
    for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS + 2; i++) {
        struct zgl_attachment *a = i < ZGL_MAX_DRAW_BUFFERS ? &f->color[i] : i == ZGL_MAX_DRAW_BUFFERS ? &f->depth : &f->stencil;
        int aw, ah;
        attachment_size(a, &aw, &ah);
        if (!aw) continue;
        if (!any || aw < mw) mw = aw;
        if (!any || ah < mh) mh = ah;
        any = 1;
    }
    *w = any ? mw : 1; *h = any ? mh : 1;
}

/* the surfaces the current draw framebuffer sends: colour buffers in draw-buffer order, depth/stencil */
static int fb_surfaces(uint32_t *cbufs, uint32_t *zsurf)
{
    struct zgl_fbo *f = zgl->fbo;
    if (!f) {
        cbufs[0] = zgl->win_draw_buffer == GL_NONE ? 0 : zgl->h_color_surf;
        *zsurf = zgl->h_depth_surf;
        return 1;
    }
    int nr = 0;
    for (int i = 0; i < f->ndraw && i < ZGL_MAX_DRAW_BUFFERS; i++) {
        GLenum b = f->draw_buffers[i];
        cbufs[i] = b == GL_NONE ? 0 : zgl_attachment_surface(&f->color[b - GL_COLOR_ATTACHMENT0]);
        if (cbufs[i]) nr = i + 1;
    }
    *zsurf = f->depth.type ? zgl_attachment_surface(&f->depth) : f->stencil.type ? zgl_attachment_surface(&f->stencil) : 0;
    return nr;
}

void zgl_apply_framebuffer(void)
{
    if (zgl->fb_dirty) {
        uint32_t cbufs[ZGL_MAX_DRAW_BUFFERS] = { 0 }, zsurf = 0;
        int nr = fb_surfaces(cbufs, &zsurf);
        virgl_set_framebuffer(zgl->v, zsurf, cbufs, (uint32_t)nr);
        zgl_rt_size(&zgl->rt_w, &zgl->rt_h);
        zgl->fb_dirty = 0;
        zgl->vp_dirty = 1;
    }
    if (zgl->vp_dirty) {
        virgl_set_viewport(zgl->v, (float)zgl->vp_x, (float)zgl->vp_y, (float)zgl->vp_w, (float)zgl->vp_h, zgl->depth_near, zgl->depth_far, 0);
        if (zgl->scissor) {
            int x0 = zgl->sc_x < 0 ? 0 : zgl->sc_x, y0 = zgl->sc_y < 0 ? 0 : zgl->sc_y;
            int x1 = zgl->sc_x + zgl->sc_w, y1 = zgl->sc_y + zgl->sc_h;
            if (x1 < x0) x1 = x0;
            if (y1 < y0) y1 = y0;
            virgl_set_scissor(zgl->v, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
        } else virgl_set_scissor(zgl->v, 0, 0, (uint32_t)zgl->rt_w, (uint32_t)zgl->rt_h);
        zgl->vp_dirty = 0;
    }
}

/* ---- clears ------------------------------------------------------------------------------------- */

/* the scissor box clipped to the target, or all of it; returns 0 if nothing is left */
static int clear_rect(int *x, int *y, int *w, int *h, int *partial)
{
    int tw, th;
    zgl_rt_size(&tw, &th);
    *x = 0; *y = 0; *w = tw; *h = th; *partial = 0;
    if (!zgl->scissor) return 1;
    int x0 = zgl->sc_x > 0 ? zgl->sc_x : 0, y0 = zgl->sc_y > 0 ? zgl->sc_y : 0;
    int x1 = zgl->sc_x + zgl->sc_w < tw ? zgl->sc_x + zgl->sc_w : tw, y1 = zgl->sc_y + zgl->sc_h < th ? zgl->sc_y + zgl->sc_h : th;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    *partial = x0 > 0 || y0 > 0 || x1 < tw || y1 < th;
    return 1;
}

/* Clear a rectangle of one surface (the host's whole-framebuffer clear ignores the scissor). */
static void clear_surface(uint32_t surf, uint32_t buffers, const uint32_t color[4], int x, int y, int w, int h, int window)
{
    if (!surf) return;
    (void)window;                                       /* the host flips the window's rectangle itself */
    virgl_cmd(zgl->v, VIRGL_CCMD_CLEAR_SURFACE, 0, VIRGL_CLEAR_SURFACE_SIZE);
    virgl_dw(zgl->v, VIRGL_CLEAR_SURFACE_S0_BUFFERS(buffers) | VIRGL_CLEAR_SURFACE_S0_RENDER_CONDITION(1));
    virgl_dw(zgl->v, surf);
    for (int i = 0; i < 4; i++) virgl_dw(zgl->v, color[i]);
    virgl_dw(zgl->v, (uint32_t)x); virgl_dw(zgl->v, (uint32_t)y); virgl_dw(zgl->v, (uint32_t)w); virgl_dw(zgl->v, (uint32_t)h);
}

static void ds_words(double depth, uint32_t stencil, uint32_t out[4])
{
    union { double d; uint32_t u[2]; } dd = { depth };
    out[0] = dd.u[0]; out[1] = dd.u[1]; out[2] = 0; out[3] = stencil;
}

void zgl_apply_state_for_clear(void);               /* draw.c: blend (colour masks), depth-stencil state */

/* A clear the write masks cut down: a quad drawn through the ordinary
 * pipeline with the masks as they are (the host's clear ignores them). */
static void clear_with_quad(int color, int depth, int stencil)
{
    if (!zgl->clear_program) {
        static const char *vs = "#version 330\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
        static const char *fs =
            "#version 330\nuniform vec4 c;\nuniform float d;\n"
            "layout(location = 0) out vec4 o0; layout(location = 1) out vec4 o1; layout(location = 2) out vec4 o2; layout(location = 3) out vec4 o3;\n"
            "layout(location = 4) out vec4 o4; layout(location = 5) out vec4 o5; layout(location = 6) out vec4 o6; layout(location = 7) out vec4 o7;\n"
            "void main() { o0 = c; o1 = c; o2 = c; o3 = c; o4 = c; o5 = c; o6 = c; o7 = c; gl_FragDepth = d; }\n";
        struct zgl_program *saved = zgl->program;
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
        glShaderSource(v, 1, &vs, NULL); glShaderSource(f, 1, &fs, NULL);
        glAttachShader(pr, v); glAttachShader(pr, f);
        glLinkProgram(pr);
        glDeleteShader(v); glDeleteShader(f);
        for (int i = 0; i < zgl->nprograms; i++) if (zgl->programs[i]->name == pr) zgl->clear_program = zgl->programs[i];
        zgl->program = saved;
        if (!zgl->clear_program || !zgl->clear_program->linked) { zgl->clear_program = NULL; return; }
    }
    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    struct zgl_program *p = zgl->clear_program;
    float cv[4]; memcpy(cv, zgl->clear_color, 16);
    float dv = (float)zgl->clear_depth;
    for (int i = 0; i < p->nuniforms; i++) {
        if (!strcmp(p->uniforms[i].name, "c")) zgl_uniform_set(p, i, cv, 1, 0);
        if (!strcmp(p->uniforms[i].name, "d")) zgl_uniform_set(p, i, &dv, 1, 0);
    }
    /* the state the quad needs, then everything back */
    struct zgl_ctx saved = *zgl;
    struct zgl_vao vao;
    memset(&vao, 0, sizeof vao);
    vao.attribs[0].enabled = 1; vao.attribs[0].size = 2; vao.attribs[0].type = GL_FLOAT; vao.attribs[0].pointer = quad;
    zgl->vao = &vao;
    zgl->program = p;
    zgl->vp_x = 0; zgl->vp_y = 0; zgl->rt_w = 0; zgl_rt_size(&zgl->vp_w, &zgl->vp_h); zgl->vp_dirty = 1;
    zgl->depth_near = 0; zgl->depth_far = 1;
    for (int i = 0; i < ZGL_MAX_DRAW_BUFFERS; i++) { zgl->blend[i] = 0; if (!color) zgl->color_masks[i] = 0; }
    zgl->color_logic_op = 0; zgl->alpha_test = 0; zgl->cull = 0; zgl->polygon_mode = GL_FILL; zgl->polygon_offset = 0;
    zgl->depth_test = depth; zgl->depth_func = GL_ALWAYS; zgl->depth_clamp = 0;
    zgl->stencil_test = stencil;
    for (int f = 0; f < 2; f++) {
        zgl->stencil_func[f] = GL_ALWAYS; zgl->stencil_ref[f] = zgl->clear_stencil; zgl->stencil_vmask[f] = 0xFF;
        zgl->stencil_fail[f] = zgl->stencil_zfail[f] = zgl->stencil_zpass[f] = GL_REPLACE;
        zgl->stencil_wmask[f] = saved.stencil_wmask[0];
    }
    zgl->sample_alpha_to_coverage = 0; zgl->rasterizer_discard = 0;
    zgl->xfb.active = 0;
    struct zgl_draw_args a = { .mode = GL_TRIANGLE_STRIP, .first = 0, .count = 4, .instances = 1 };
    zgl->imm_draw = 1;                                  /* not a fixed-function draw: no client array binding */
    zgl_draw_ex(&a);
    /* restore: the copy holds every field, the counters and objects created since must stay */
    struct zgl_program *cp = zgl->clear_program;
    virgl_res *sc = zgl->scratch, *isc = zgl->iscratch; size_t su = zgl->scratch_used, isu = zgl->iscratch_used, ss = zgl->scratch_size;
    uint32_t hb = zgl->h_blend, hr = zgl->h_rs, hd = zgl->h_dsa, hv = zgl->h_ve; uint64_t bk = zgl->blend_key, rk = zgl->rs_key, dk = zgl->dsa_key, vk = zgl->ve_key;
    uint32_t srs = zgl->stencil_ref_sent; int srv = zgl->stencil_ref_valid; int hds = zgl->host_dual_src;
    int nsc = zgl->nsampler_cache; __typeof__(zgl->sampler_cache) scache; memcpy(scache, zgl->sampler_cache, sizeof scache);
    GLenum err = zgl->error; uint32_t draws = zgl->stats_draws;
    *zgl = saved;
    zgl->clear_program = cp;
    zgl->scratch = sc; zgl->iscratch = isc; zgl->scratch_used = su; zgl->iscratch_used = isu; zgl->scratch_size = ss;
    zgl->h_blend = hb; zgl->h_rs = hr; zgl->h_dsa = hd; zgl->h_ve = hv; zgl->blend_key = bk; zgl->rs_key = rk; zgl->dsa_key = dk; zgl->ve_key = vk;
    zgl->stencil_ref_sent = srs; zgl->stencil_ref_valid = srv; zgl->host_dual_src = hds;
    zgl->nsampler_cache = nsc; memcpy(zgl->sampler_cache, scache, sizeof scache);
    zgl->error = err; zgl->stats_draws = draws;
    zgl->vp_dirty = 1;
}

void glClear(GLbitfield mask)
{
    CTX();
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (zgl->rasterizer_discard) return;
    zgl_apply_framebuffer();
    zgl_apply_state_for_clear();
    int x, y, w, h, partial;
    if (!clear_rect(&x, &y, &w, &h, &partial)) return;
    uint32_t buffers = 0;
    uint32_t cbufs[ZGL_MAX_DRAW_BUFFERS] = { 0 }, zsurf = 0;
    int nr = fb_surfaces(cbufs, &zsurf);
    /* the write masks apply: a fully masked buffer isn't cleared, a partly masked one needs a quad */
    int partial_mask = 0, int_target = 0;
    if (mask & GL_COLOR_BUFFER_BIT)
        for (int i = 0; i < nr; i++) {
            if (!cbufs[i] || !zgl->color_masks[i]) continue;
            buffers |= PIPE_CLEAR_COLOR0 << i;
            if (zgl->color_masks[i] != 0xF) partial_mask = 1;
            if (zgl->fbo) {
                const struct zgl_format *cf = attachment_format(&zgl->fbo->color[zgl->fbo->draw_buffers[i] - GL_COLOR_ATTACHMENT0]);
                if (cf && (cf->kind == ZF_INT || cf->kind == ZF_UINT)) int_target = 1;
            }
        }
    if ((mask & GL_DEPTH_BUFFER_BIT) && zsurf && zgl->depth_mask) buffers |= PIPE_CLEAR_DEPTH;
    if ((mask & GL_STENCIL_BUFFER_BIT) && zsurf && (zgl->stencil_wmask[0] & 0xFF)) {
        buffers |= PIPE_CLEAR_STENCIL;
        if ((zgl->stencil_wmask[0] & 0xFF) != 0xFF) partial_mask = 1;
    }
    if (!buffers) return;
    if (partial_mask && !int_target) {
        clear_with_quad(!!(buffers & PIPE_CLEAR_COLOR), !!(buffers & PIPE_CLEAR_DEPTH), !!(buffers & PIPE_CLEAR_STENCIL));
        return;
    }
    if (!partial) { virgl_clear(zgl->v, buffers, zgl->clear_color, zgl->clear_depth, (uint32_t)zgl->clear_stencil); return; }
    int window = zgl->fbo == NULL;
    uint32_t c[4];
    memcpy(c, zgl->clear_color, 16);
    for (int i = 0; i < nr; i++) if (buffers & (PIPE_CLEAR_COLOR0 << i)) clear_surface(cbufs[i], PIPE_CLEAR_COLOR0, c, x, y, w, h, window);
    if (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
        uint32_t ds[4];
        ds_words(zgl->clear_depth, (uint32_t)zgl->clear_stencil, ds);
        clear_surface(zsurf, buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL), ds, x, y, w, h, window);
    }
}

static void clear_buffer(GLenum buffer, GLint drawbuffer, const uint32_t value[4], double depth, int stencil, int which)
{
    if (zgl->rasterizer_discard) return;
    zgl_apply_framebuffer();
    zgl_apply_state_for_clear();
    int x, y, w, h, partial;
    if (!clear_rect(&x, &y, &w, &h, &partial)) return;
    uint32_t cbufs[ZGL_MAX_DRAW_BUFFERS] = { 0 }, zsurf = 0;
    int nr = fb_surfaces(cbufs, &zsurf);
    int window = zgl->fbo == NULL;
    if (buffer == GL_COLOR) {
        if (drawbuffer < 0 || drawbuffer >= ZGL_MAX_DRAW_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return; }
        if (drawbuffer >= nr || !cbufs[drawbuffer]) return;
        if (!partial) {
            float f[4]; memcpy(f, value, 16);
            virgl_clear(zgl->v, PIPE_CLEAR_COLOR0 << drawbuffer, f, 0, 0);
        } else clear_surface(cbufs[drawbuffer], PIPE_CLEAR_COLOR0, value, x, y, w, h, window);
        return;
    }
    if (drawbuffer != 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!zsurf) return;
    uint32_t b = which;
    if (!partial) { float none[4] = { 0 }; virgl_clear(zgl->v, b, none, depth, (uint32_t)stencil); return; }
    uint32_t ds[4];
    ds_words(depth, (uint32_t)stencil, ds);
    clear_surface(zsurf, b, ds, x, y, w, h, window);
}

void glClearBufferfv(GLenum buffer, GLint db, const GLfloat *v)
{
    CTX();
    if (buffer == GL_COLOR) { uint32_t u[4]; memcpy(u, v, 16); clear_buffer(buffer, db, u, 0, 0, 0); }
    else if (buffer == GL_DEPTH) clear_buffer(buffer, db, NULL, v[0], 0, PIPE_CLEAR_DEPTH);
    else zgl_set_error(GL_INVALID_ENUM);
}
void glClearBufferiv(GLenum buffer, GLint db, const GLint *v)
{
    CTX();
    if (buffer == GL_COLOR) { uint32_t u[4]; memcpy(u, v, 16); clear_buffer(buffer, db, u, 0, 0, 0); }
    else if (buffer == GL_STENCIL) clear_buffer(buffer, db, NULL, 0, v[0], PIPE_CLEAR_STENCIL);
    else zgl_set_error(GL_INVALID_ENUM);
}
void glClearBufferuiv(GLenum buffer, GLint db, const GLuint *v)
{
    CTX();
    if (buffer == GL_COLOR) clear_buffer(buffer, db, v, 0, 0, 0);
    else zgl_set_error(GL_INVALID_ENUM);
}
void glClearBufferfi(GLenum buffer, GLint db, GLfloat depth, GLint stencil)
{
    CTX();
    if (buffer != GL_DEPTH_STENCIL) { zgl_set_error(GL_INVALID_ENUM); return; }
    clear_buffer(buffer, db, NULL, depth, stencil, PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL);
}

/* ---- reading -------------------------------------------------------------------------------------- */

/* the attachment the read framebuffer reads colour from; or the window */
static struct zgl_attachment *read_color_attachment(void)
{
    struct zgl_fbo *f = zgl->read_fbo;
    if (!f || f->read_buffer == GL_NONE) return NULL;
    return &f->color[f->read_buffer - GL_COLOR_ATTACHMENT0];
}

int zgl_read_source(virgl_res **res, int *level, int *layer, const struct zgl_format **f)
{
    zgl_apply_framebuffer();
    if (!zgl->read_fbo) {
        if (zgl->win_read_buffer == GL_NONE) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
        *res = zgl->color; *level = 0; *layer = 0; *f = window_color_format();
        return 0;
    }
    if (fbo_status(zgl->read_fbo) != GL_FRAMEBUFFER_COMPLETE) { zgl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION); return -1; }
    struct zgl_attachment *a = read_color_attachment();
    if (!a || !attachment_res(a)) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    *res = attachment_res(a); *level = a->level; *layer = a->layer < 0 ? 0 : a->layer; *f = attachment_format(a);
    return 0;
}

uint8_t *zgl_read_resource(virgl_res *res, const struct zgl_format *f, int level, int x, int y, int z, int w, int h)
{
    size_t row = (size_t)(f->block > 1 ? (w + 3) / 4 : w) * f->bpp, size = row * (size_t)(f->block > 1 ? (h + 3) / 4 : h);
    uint8_t *out = malloc(size ? size : 1);
    if (!out) { zgl_set_error(GL_OUT_OF_MEMORY); return NULL; }
    /* the window's buffer has guest memory: read it directly */
    if (res == zgl->color) {
        virgl_flush(zgl->v);
        if (virgl_res_from_host(zgl->v, res, 0, (uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1,
                                (uint32_t)zgl->width * 4, (uint32_t)zgl->width * zgl->height * 4, (uint64_t)((size_t)y * zgl->width + x) * 4) != 0) { free(out); zgl_set_error(GL_OUT_OF_MEMORY); return NULL; }
        for (int j = 0; j < h; j++) memcpy(out + (size_t)j * row, (const uint8_t *)res->map + ((size_t)(y + j) * zgl->width + x) * 4, row);
        return out;
    }
    /* everything else through a staging texture of the rectangle's size: a
     * blit (which also resolves multisampling) then a transfer */
    int depthy = is_depth_kind(f->kind);
    struct gpu_res_create rc = { .target = PIPE_TEXTURE_2D, .format = f->vformat, .bind = depthy ? VIRGL_BIND_DEPTH_STENCIL : VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET,
                                 .width = (uint32_t)w, .height = (uint32_t)h, .depth = 1, .array_size = 1, .size = (uint32_t)size };
    if (f->kind == ZF_COMPRESSED) rc.bind = VIRGL_BIND_SAMPLER_VIEW;
    virgl_res *st = virgl_res_create_ex(zgl->v, &rc);
    if (!st || !st->map) { if (st) virgl_res_destroy(zgl->v, st); free(out); zgl_set_error(GL_OUT_OF_MEMORY); return NULL; }
    if (f->kind == ZF_COMPRESSED || res->bind == 0) {
        virgl_resource_copy_region(zgl->v, st->id, 0, 0, 0, 0, res->id, (uint32_t)level, (uint32_t)x, (uint32_t)y, (uint32_t)z, (uint32_t)w, (uint32_t)h, 1);
    } else {
        struct virgl_blit_info b = { .mask = depthy ? (f->kind == ZF_DEPTH ? PIPE_MASK_Z : f->kind == ZF_STENCIL ? PIPE_MASK_S : PIPE_MASK_ZS) : PIPE_MASK_RGBA,
                                     .filter = PIPE_TEX_FILTER_NEAREST };
        b.dst = (struct virgl_blit_box){ st->id, 0, f->vformat, 0, 0, 0, w, h, 1 };
        b.src = (struct virgl_blit_box){ res->id, (uint32_t)level, f->vformat, x, y, z, w, h, 1 };
        virgl_blit(zgl->v, &b);
    }
    virgl_flush(zgl->v);
    int rc2 = virgl_res_from_host(zgl->v, st, 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, 1, (uint32_t)row, (uint32_t)size, 0);
    if (rc2 == 0) memcpy(out, st->map, size);
    virgl_res_destroy(zgl->v, st);
    if (rc2 != 0) { free(out); zgl_set_error(GL_OUT_OF_MEMORY); return NULL; }
    return out;
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    CTX();
    if (w < 0 || h < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_pixel_store ps;
    zgl_pack_store(&ps);
    size_t need = zgl_pixels_size(w ? w : 1, h ? h : 1, 1, format, type, &ps);
    if (!need) { zgl_set_error(GL_INVALID_ENUM); return; }
    virgl_res *res; int level, layer; const struct zgl_format *f;
    int depthy = format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
    if (depthy) {
        /* depth and stencil come from the depth-stencil attachment */
        zgl_apply_framebuffer();
        if (!zgl->read_fbo) { res = zgl->depth; f = window_depth_format(); level = 0; layer = 0; }
        else {
            struct zgl_attachment *a = format == GL_STENCIL_INDEX && zgl->read_fbo->stencil.type ? &zgl->read_fbo->stencil : &zgl->read_fbo->depth;
            res = attachment_res(a); f = attachment_format(a); level = a->level; layer = a->layer < 0 ? 0 : a->layer;
            if (!res) { zgl_set_error(GL_INVALID_OPERATION); return; }
        }
    } else if (zgl_read_source(&res, &level, &layer, &f) != 0) return;
    if (!depthy && is_depth_kind(f->kind)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int integer = format == GL_RED_INTEGER || format == GL_RG_INTEGER || format == GL_RGB_INTEGER || format == GL_RGBA_INTEGER || format == GL_BGRA_INTEGER ||
                  format == GL_GREEN_INTEGER || format == GL_BLUE_INTEGER || format == GL_ALPHA_INTEGER || format == GL_BGR_INTEGER;
    if (!depthy && integer != (f->kind == ZF_INT || f->kind == ZF_UINT)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (!w || !h) return;
    uint8_t *dst = pixels;
    struct zgl_buffer *pb = zgl->pixel_pack_buffer;
    if (pb) {
        size_t off = (size_t)(uintptr_t)pixels;
        if (!pb->shadow || pb->mapped || off + need > pb->size) { zgl_set_error(GL_INVALID_OPERATION); return; }
        zgl_buffer_sync(pb);
        dst = pb->shadow + off;
    }
    /* clip to the source; pixels outside are left alone */
    int sw, sh;
    if (!zgl->read_fbo) { sw = zgl->width; sh = zgl->height; }
    else {
        struct zgl_attachment *a = depthy ? &zgl->read_fbo->depth : read_color_attachment();
        attachment_size(a, &sw, &sh);
    }
    int cx0 = x < 0 ? 0 : x, cy0 = y < 0 ? 0 : y, cx1 = x + w > sw ? sw : x + w, cy1 = y + h > sh ? sh : y + h;
    if (cx1 <= cx0 || cy1 <= cy0) return;
    uint8_t *rows = zgl_read_resource(res, f, level, cx0, cy0, layer, cx1 - cx0, cy1 - cy0);
    if (!rows) return;
    struct zgl_pixel_store ps2 = ps;
    if (ps2.row_length <= 0) ps2.row_length = w;
    ps2.skip_pixels += cx0 - x; ps2.skip_rows += cy0 - y;
    zgl_pixels_from_format(f, cx1 - cx0, cy1 - cy0, 1, rows, format, type, &ps2, dst);
    free(rows);
    if (pb) zgl_buffer_write(pb, (size_t)(uintptr_t)pixels, dst, need);
}

/* ---- blits ------------------------------------------------------------------------------------------ */

void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    CTX();
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (filter != GL_NEAREST && filter != GL_LINEAR) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (filter == GL_LINEAR && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (fbo_status(zgl->read_fbo) != GL_FRAMEBUFFER_COMPLETE || fbo_status(zgl->fbo) != GL_FRAMEBUFFER_COMPLETE) { zgl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION); return; }
    zgl_apply_framebuffer();
    struct virgl_blit_info b = { .filter = filter == GL_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST };
    if (zgl->scissor) {
        b.scissor_enable = 1;
        int x0 = zgl->sc_x < 0 ? 0 : zgl->sc_x, y0 = zgl->sc_y < 0 ? 0 : zgl->sc_y;
        b.scissor[0] = (uint32_t)x0; b.scissor[1] = (uint32_t)y0;
        b.scissor[2] = (uint32_t)(zgl->sc_x + zgl->sc_w > x0 ? zgl->sc_x + zgl->sc_w : x0);
        b.scissor[3] = (uint32_t)(zgl->sc_y + zgl->sc_h > y0 ? zgl->sc_y + zgl->sc_h : y0);
    }
    b.render_condition_enable = 1;
    if (mask & GL_COLOR_BUFFER_BIT) {
        virgl_res *src; int slevel, slayer; const struct zgl_format *sf;
        if (zgl_read_source(&src, &slevel, &slayer, &sf) == 0) {
            b.mask = PIPE_MASK_RGBA;
            b.src = (struct virgl_blit_box){ src->id, (uint32_t)slevel, sf->vformat, sx0, sy0, slayer, sx1 - sx0, sy1 - sy0, 1 };
            if (sf->kind == ZF_SRGB && !zgl->framebuffer_srgb) b.src.format = linear_of(sf->vformat);
            if (!zgl->fbo) {
                if (zgl->win_draw_buffer != GL_NONE) {
                    b.dst = (struct virgl_blit_box){ zgl->color->id, 0, VIRGL_FORMAT_B8G8R8A8_UNORM, dx0, dy0, 0, dx1 - dx0, dy1 - dy0, 1 };
                    virgl_blit(zgl->v, &b);
                }
            } else {
                struct zgl_fbo *f = zgl->fbo;
                for (int i = 0; i < f->ndraw; i++) {
                    if (f->draw_buffers[i] == GL_NONE) continue;
                    struct zgl_attachment *a = &f->color[f->draw_buffers[i] - GL_COLOR_ATTACHMENT0];
                    virgl_res *dr = attachment_res(a);
                    const struct zgl_format *df = attachment_format(a);
                    if (!dr) continue;
                    if ((df->kind == ZF_INT || df->kind == ZF_UINT) != (sf->kind == ZF_INT || sf->kind == ZF_UINT)) { zgl_set_error(GL_INVALID_OPERATION); return; }
                    b.dst = (struct virgl_blit_box){ dr->id, (uint32_t)a->level, df->kind == ZF_SRGB && !zgl->framebuffer_srgb ? linear_of(df->vformat) : df->vformat,
                                                     dx0, dy0, a->layer < 0 ? 0 : a->layer, dx1 - dx0, dy1 - dy0, 1 };
                    virgl_blit(zgl->v, &b);
                }
            }
        }
    }
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        virgl_res *sr, *dr; const struct zgl_format *sf, *df; int sl = 0, dl = 0, slay = 0, dlay = 0;
        if (!zgl->read_fbo) { sr = zgl->depth; sf = window_depth_format(); }
        else { struct zgl_attachment *a = zgl->read_fbo->depth.type ? &zgl->read_fbo->depth : &zgl->read_fbo->stencil; sr = attachment_res(a); sf = attachment_format(a); sl = a->level; slay = a->layer < 0 ? 0 : a->layer; }
        if (!zgl->fbo) { dr = zgl->depth; df = window_depth_format(); }
        else { struct zgl_attachment *a = zgl->fbo->depth.type ? &zgl->fbo->depth : &zgl->fbo->stencil; dr = attachment_res(a); df = attachment_format(a); dl = a->level; dlay = a->layer < 0 ? 0 : a->layer; }
        if (sr && dr) {
            if (sf->vformat != df->vformat) { zgl_set_error(GL_INVALID_OPERATION); return; }
            b.mask = ((mask & GL_DEPTH_BUFFER_BIT) ? PIPE_MASK_Z : 0) | ((mask & GL_STENCIL_BUFFER_BIT) ? PIPE_MASK_S : 0);
            b.filter = PIPE_TEX_FILTER_NEAREST;
            b.src = (struct virgl_blit_box){ sr->id, (uint32_t)sl, sf->vformat, sx0, sy0, slay, sx1 - sx0, sy1 - sy0, 1 };
            b.dst = (struct virgl_blit_box){ dr->id, (uint32_t)dl, df->vformat, dx0, dy0, dlay, dx1 - dx0, dy1 - dy0, 1 };
            virgl_blit(zgl->v, &b);
        }
    }
}
