/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The context: a colour and a depth buffer on the host for a window, the
 * default state, and the frame readback for the window system (SDL). */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* front: what a compositor samples (sic_gl_buffer), a copy of the colour
 * buffer made at each sic_gl_flush, so it never sees a frame half drawn */
struct sic_gl_context { struct zgl_ctx c; virgl_res *front; };

static int make_targets(struct zgl_ctx *c, int w, int h)
{
    if (c->h_color_surf) virgl_destroy_object(c->v, c->h_color_surf, VIRGL_OBJECT_SURFACE);
    if (c->h_depth_surf) virgl_destroy_object(c->v, c->h_depth_surf, VIRGL_OBJECT_SURFACE);
    if (c->color) virgl_res_destroy(c->v, c->color);
    if (c->depth) virgl_res_destroy(c->v, c->depth);
    c->color = virgl_res_create(c->v, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_SCANOUT,
                                (uint32_t)w, (uint32_t)h, 1, 0, (size_t)w * h * 4);
    /* Depth: the packed 24+8 format is not always there (macOS GL lacks it);
     * ask the caps, prefer a format with stencil bits. */
    static const uint32_t depth_fmts[] = { VIRGL_FORMAT_Z24_UNORM_S8_UINT, VIRGL_FORMAT_S8_UINT_Z24_UNORM, VIRGL_FORMAT_Z32_FLOAT_S8X24_UINT,
                                           VIRGL_FORMAT_Z24X8_UNORM, VIRGL_FORMAT_Z32_FLOAT, VIRGL_FORMAT_Z16_UNORM };
    const struct virgl_caps_v2 *caps = virgl_caps(c->v);
    c->depth_format = VIRGL_FORMAT_Z24X8_UNORM;
    for (size_t i = 0; caps && i < sizeof depth_fmts / sizeof depth_fmts[0]; i++)
        if (caps->v1.depthstencil.bitmask[depth_fmts[i] / 32] & (1u << (depth_fmts[i] % 32))) { c->depth_format = depth_fmts[i]; break; }
    c->depth = virgl_res_create(c->v, PIPE_TEXTURE_2D, c->depth_format, VIRGL_BIND_DEPTH_STENCIL, (uint32_t)w, (uint32_t)h, 1, 0, 0);
    if (!c->color || !c->color->map || !c->depth) return -1;
    c->h_color_surf = virgl_handle(c->v);
    c->h_depth_surf = virgl_handle(c->v);
    virgl_create_surface(c->v, c->h_color_surf, c->color->id, VIRGL_FORMAT_B8G8R8A8_UNORM, 0, 0);
    virgl_create_surface(c->v, c->h_depth_surf, c->depth->id, c->depth_format, 0, 0);
    c->width = w; c->height = h;
    if (!c->fbo) { c->rt_w = w; c->rt_h = h; }
    c->frame = realloc(c->frame, (size_t)w * h * 4);
    c->fb_dirty = 1;
    return 0;
}

sic_gl_context *sic_gl_create(int width, int height)
{
    virgl *v = virgl_open();
    if (!v) return NULL;
    struct sic_gl_context *ctx = calloc(1, sizeof *ctx);
    struct zgl_ctx *c = &ctx->c;
    c->v = v;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (make_targets(c, width, height) != 0) { virgl_close(v); free(ctx); return NULL; }
    /* GL defaults */
    c->clear_depth = 1.0; c->depth_func = GL_LESS; c->depth_mask = 1; c->depth_near = 0; c->depth_far = 1;
    c->blend_src = c->blend_src_a = GL_ONE; c->blend_dst = c->blend_dst_a = GL_ZERO; c->blend_eq = GL_FUNC_ADD;
    c->cull_mode = GL_BACK; c->front_face = GL_CCW; c->polygon_mode = GL_FILL; c->line_width = 1; c->point_size = 1;
    c->color_mask = 0xF; c->alpha_func = GL_ALWAYS; c->shade_model = GL_SMOOTH;
    c->unpack_alignment = 4; c->pack_alignment = 4;
    c->vp_w = width; c->vp_h = height; c->sc_w = width; c->sc_h = height; c->vp_dirty = 1;
    c->next_name = 1;
    for (int i = 0; i < ZGL_MAX_ATTRIBS; i++) c->attribs[i].value[3] = 1;
    zgl_ffp_init(c);
    zgl = c;
    return ctx;
}

void sic_gl_make_current(sic_gl_context *ctx) { zgl = ctx ? &ctx->c : NULL; }

int sic_gl_resize(sic_gl_context *ctx, int width, int height)
{
    struct zgl_ctx *c = &ctx->c;
    if (width == c->width && height == c->height) return 0;
    struct zgl_ctx *saved = zgl;
    zgl = c;
    virgl_flush(c->v);
    if (ctx->front) { virgl_res_destroy(c->v, ctx->front); ctx->front = NULL; }   /* the next sic_gl_buffer() makes one of the new size */
    int rc = make_targets(c, width < 1 ? 1 : width, height < 1 ? 1 : height);
    c->vp_dirty = 1;
    zgl = saved;
    return rc;
}

const uint32_t *sic_gl_swap(sic_gl_context *ctx, int *stride)
{
    struct zgl_ctx *c = &ctx->c;
    virgl_flush(c->v);
    if (virgl_res_from_host(c->v, c->color, 0, 0, 0, 0, (uint32_t)c->width, (uint32_t)c->height, 1,
                            (uint32_t)c->width * 4, (uint32_t)c->width * c->height * 4, 0) != 0)
        return NULL;
    /* the host's rows are bottom-up */
    const uint32_t *src = c->color->map;
    uint32_t *dst = c->target ? c->target : c->frame;
    int dstride = c->target ? c->target_stride : c->width;
    for (int y = 0; y < c->height; y++)
        memcpy(dst + (size_t)y * dstride, src + (size_t)(c->height - 1 - y) * c->width, (size_t)c->width * 4);
    *stride = dstride;
    return dst;
}

uint32_t sic_gl_buffer(sic_gl_context *ctx)
{
    struct zgl_ctx *c = &ctx->c;
    if (!ctx->front)
        ctx->front = virgl_res_create(c->v, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                      (uint32_t)c->width, (uint32_t)c->height, 1, 0, 0);
    return ctx->front ? ctx->front->id : 0;
}

int sic_gl_flush(sic_gl_context *ctx)
{
    struct zgl_ctx *c = &ctx->c;
    if (ctx->front)
        virgl_resource_copy_region(c->v, ctx->front->id, 0, 0, 0, 0, c->color->id, 0, 0, 0, 0, (uint32_t)c->width, (uint32_t)c->height, 1);
    return ctx->front ? virgl_flush_fenced(c->v) : virgl_flush(c->v);   /* a compositor samples the front from its context */
}
int sic_gl_scanout(sic_gl_context *ctx) { return virgl_scanout(ctx->c.v, ctx->c.color); }
int sic_gl_present(sic_gl_context *ctx, int x, int y, int w, int h)
{
    /* the buffer is bottom-up: the rectangle's rows count from the bottom */
    struct zgl_ctx *c = &ctx->c;
    return virgl_present(c->v, c->color, (uint32_t)x, (uint32_t)(c->height - y - h), (uint32_t)w, (uint32_t)h);
}

void sic_gl_set_target(sic_gl_context *ctx, uint32_t *pix, int stride)
{
    ctx->c.target = pix;
    ctx->c.target_stride = stride;
}

void sic_gl_destroy(sic_gl_context *ctx)
{
    if (!ctx) return;
    struct zgl_ctx *c = &ctx->c;
    if (zgl == c) zgl = NULL;
    virgl_flush(c->v);
    if (c->scratch) virgl_res_destroy(c->v, c->scratch);
    if (c->iscratch) virgl_res_destroy(c->v, c->iscratch);
    if (c->color) virgl_res_destroy(c->v, c->color);
    if (c->depth) virgl_res_destroy(c->v, c->depth);
    if (ctx->front) virgl_res_destroy(c->v, ctx->front);
    virgl_close(c->v);
    free(c->frame);
    free(c->imm);
    free(ctx);
}

static const struct { const char *name; void *fn; } procs[] = {
    { "glClearColor", (void *)glClearColor },
    { "glClearDepth", (void *)glClearDepth },
    { "glClearStencil", (void *)glClearStencil },
    { "glClear", (void *)glClear },
    { "glViewport", (void *)glViewport },
    { "glScissor", (void *)glScissor },
    { "glEnable", (void *)glEnable },
    { "glDisable", (void *)glDisable },
    { "glIsEnabled", (void *)glIsEnabled },
    { "glDepthFunc", (void *)glDepthFunc },
    { "glDepthMask", (void *)glDepthMask },
    { "glDepthRange", (void *)glDepthRange },
    { "glBlendFunc", (void *)glBlendFunc },
    { "glBlendFuncSeparate", (void *)glBlendFuncSeparate },
    { "glBlendEquation", (void *)glBlendEquation },
    { "glBlendColor", (void *)glBlendColor },
    { "glAlphaFunc", (void *)glAlphaFunc },
    { "glCullFace", (void *)glCullFace },
    { "glFrontFace", (void *)glFrontFace },
    { "glPolygonMode", (void *)glPolygonMode },
    { "glPolygonOffset", (void *)glPolygonOffset },
    { "glColorMask", (void *)glColorMask },
    { "glLineWidth", (void *)glLineWidth },
    { "glPointSize", (void *)glPointSize },
    { "glShadeModel", (void *)glShadeModel },
    { "glHint", (void *)glHint },
    { "glPixelStorei", (void *)glPixelStorei },
    { "glFlush", (void *)glFlush },
    { "glFinish", (void *)glFinish },
    { "glGetError", (void *)glGetError },
    { "glGetString", (void *)glGetString },
    { "glGetIntegerv", (void *)glGetIntegerv },
    { "glGetFloatv", (void *)glGetFloatv },
    { "glGetDoublev", (void *)glGetDoublev },
    { "glGetBooleanv", (void *)glGetBooleanv },
    { "glReadPixels", (void *)glReadPixels },
    { "glMatrixMode", (void *)glMatrixMode },
    { "glLoadIdentity", (void *)glLoadIdentity },
    { "glLoadMatrixf", (void *)glLoadMatrixf },
    { "glLoadMatrixd", (void *)glLoadMatrixd },
    { "glMultMatrixf", (void *)glMultMatrixf },
    { "glMultMatrixd", (void *)glMultMatrixd },
    { "glPushMatrix", (void *)glPushMatrix },
    { "glPopMatrix", (void *)glPopMatrix },
    { "glTranslatef", (void *)glTranslatef },
    { "glTranslated", (void *)glTranslated },
    { "glRotatef", (void *)glRotatef },
    { "glRotated", (void *)glRotated },
    { "glScalef", (void *)glScalef },
    { "glScaled", (void *)glScaled },
    { "glFrustum", (void *)glFrustum },
    { "glOrtho", (void *)glOrtho },
    { "glBegin", (void *)glBegin },
    { "glEnd", (void *)glEnd },
    { "glVertex2f", (void *)glVertex2f },
    { "glVertex2i", (void *)glVertex2i },
    { "glVertex3f", (void *)glVertex3f },
    { "glVertex3fv", (void *)glVertex3fv },
    { "glVertex3d", (void *)glVertex3d },
    { "glVertex4f", (void *)glVertex4f },
    { "glColor3f", (void *)glColor3f },
    { "glColor3fv", (void *)glColor3fv },
    { "glColor4f", (void *)glColor4f },
    { "glColor4fv", (void *)glColor4fv },
    { "glColor3ub", (void *)glColor3ub },
    { "glColor4ub", (void *)glColor4ub },
    { "glNormal3f", (void *)glNormal3f },
    { "glNormal3fv", (void *)glNormal3fv },
    { "glTexCoord2f", (void *)glTexCoord2f },
    { "glTexCoord2fv", (void *)glTexCoord2fv },
    { "glLightfv", (void *)glLightfv },
    { "glLightf", (void *)glLightf },
    { "glLightModelfv", (void *)glLightModelfv },
    { "glLightModeli", (void *)glLightModeli },
    { "glMaterialfv", (void *)glMaterialfv },
    { "glMaterialf", (void *)glMaterialf },
    { "glColorMaterial", (void *)glColorMaterial },
    { "glTexEnvi", (void *)glTexEnvi },
    { "glTexEnvf", (void *)glTexEnvf },
    { "glEnableClientState", (void *)glEnableClientState },
    { "glDisableClientState", (void *)glDisableClientState },
    { "glVertexPointer", (void *)glVertexPointer },
    { "glNormalPointer", (void *)glNormalPointer },
    { "glColorPointer", (void *)glColorPointer },
    { "glTexCoordPointer", (void *)glTexCoordPointer },
    { "glGenTextures", (void *)glGenTextures },
    { "glDeleteTextures", (void *)glDeleteTextures },
    { "glBindTexture", (void *)glBindTexture },
    { "glIsTexture", (void *)glIsTexture },
    { "glActiveTexture", (void *)glActiveTexture },
    { "glTexImage2D", (void *)glTexImage2D },
    { "glTexSubImage2D", (void *)glTexSubImage2D },
    { "glTexParameteri", (void *)glTexParameteri },
    { "glTexParameterf", (void *)glTexParameterf },
    { "glGenerateMipmap", (void *)glGenerateMipmap },
    { "glCopyTexSubImage2D", (void *)glCopyTexSubImage2D },
    { "glGenFramebuffers", (void *)glGenFramebuffers },
    { "glDeleteFramebuffers", (void *)glDeleteFramebuffers },
    { "glBindFramebuffer", (void *)glBindFramebuffer },
    { "glFramebufferTexture2D", (void *)glFramebufferTexture2D },
    { "glCheckFramebufferStatus", (void *)glCheckFramebufferStatus },
    { "glIsFramebuffer", (void *)glIsFramebuffer },
    { "glGenFramebuffersEXT", (void *)glGenFramebuffers },
    { "glDeleteFramebuffersEXT", (void *)glDeleteFramebuffers },
    { "glBindFramebufferEXT", (void *)glBindFramebuffer },
    { "glFramebufferTexture2DEXT", (void *)glFramebufferTexture2D },
    { "glCheckFramebufferStatusEXT", (void *)glCheckFramebufferStatus },
    { "glClipPlane", (void *)glClipPlane },
    { "glTexEnvfv", (void *)glTexEnvfv },
    { "glClientActiveTexture", (void *)glClientActiveTexture },
    { "glGenBuffers", (void *)glGenBuffers },
    { "glDeleteBuffers", (void *)glDeleteBuffers },
    { "glBindBuffer", (void *)glBindBuffer },
    { "glBufferData", (void *)glBufferData },
    { "glBufferSubData", (void *)glBufferSubData },
    { "glCreateShader", (void *)glCreateShader },
    { "glDeleteShader", (void *)glDeleteShader },
    { "glShaderSource", (void *)glShaderSource },
    { "glCompileShader", (void *)glCompileShader },
    { "glGetShaderiv", (void *)glGetShaderiv },
    { "glGetShaderInfoLog", (void *)glGetShaderInfoLog },
    { "glCreateProgram", (void *)glCreateProgram },
    { "glDeleteProgram", (void *)glDeleteProgram },
    { "glAttachShader", (void *)glAttachShader },
    { "glDetachShader", (void *)glDetachShader },
    { "glLinkProgram", (void *)glLinkProgram },
    { "glUseProgram", (void *)glUseProgram },
    { "glGetProgramiv", (void *)glGetProgramiv },
    { "glGetProgramInfoLog", (void *)glGetProgramInfoLog },
    { "glValidateProgram", (void *)glValidateProgram },
    { "glGetAttribLocation", (void *)glGetAttribLocation },
    { "glBindAttribLocation", (void *)glBindAttribLocation },
    { "glGetUniformLocation", (void *)glGetUniformLocation },
    { "glUniform1f", (void *)glUniform1f },
    { "glUniform2f", (void *)glUniform2f },
    { "glUniform3f", (void *)glUniform3f },
    { "glUniform4f", (void *)glUniform4f },
    { "glUniform1i", (void *)glUniform1i },
    { "glUniform2i", (void *)glUniform2i },
    { "glUniform3i", (void *)glUniform3i },
    { "glUniform4i", (void *)glUniform4i },
    { "glUniform1fv", (void *)glUniform1fv },
    { "glUniform2fv", (void *)glUniform2fv },
    { "glUniform3fv", (void *)glUniform3fv },
    { "glUniform4fv", (void *)glUniform4fv },
    { "glUniform1iv", (void *)glUniform1iv },
    { "glUniformMatrix2fv", (void *)glUniformMatrix2fv },
    { "glUniformMatrix3fv", (void *)glUniformMatrix3fv },
    { "glUniformMatrix4fv", (void *)glUniformMatrix4fv },
    { "glVertexAttribPointer", (void *)glVertexAttribPointer },
    { "glEnableVertexAttribArray", (void *)glEnableVertexAttribArray },
    { "glDisableVertexAttribArray", (void *)glDisableVertexAttribArray },
    { "glVertexAttrib1f", (void *)glVertexAttrib1f },
    { "glVertexAttrib2f", (void *)glVertexAttrib2f },
    { "glVertexAttrib3f", (void *)glVertexAttrib3f },
    { "glVertexAttrib4f", (void *)glVertexAttrib4f },
    { "glVertexAttrib4fv", (void *)glVertexAttrib4fv },
    { "glDrawArrays", (void *)glDrawArrays },
    { "glDrawElements", (void *)glDrawElements },
};

void *sic_gl_proc(const char *name)
{
    for (size_t i = 0; i < sizeof procs / sizeof procs[0]; i++)
        if (strcmp(procs[i].name, name) == 0) return procs[i].fn;
    return NULL;
}
