/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Buffer objects on every binding point, mapping, copies; vertex array
 * objects and the vertex attribute state they hold. */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX() do { if (!zgl) return; } while (0)
#define CTXV(v) do { if (!zgl) return (v); } while (0)
#define LIST_ADD(list, n, item) do { (list) = realloc((list), sizeof *(list) * ((n) + 1)); (list)[(n)++] = (item); } while (0)
#define LIST_DEL(list, n, item) do { for (int _i = 0; _i < (n); _i++) if ((list)[_i] == (item)) { memmove(&(list)[_i], &(list)[_i + 1], sizeof *(list) * ((n) - _i - 1)); (n)--; break; } } while (0)

/* bind 0: the host keeps a plain GL buffer and binds it wherever it is used
 * (vertices, indices, stream output). Texture buffers are 2D textures filled
 * from the guest copy (tex.c), uniform blocks constants (draw.c). */
static uint32_t buffer_binds(const struct zgl_buffer *b) { (void)b; return 0; }

struct zgl_buffer *zgl_buffer_get(GLuint name)
{
    for (int i = 0; i < zgl->nbuffers; i++) if (zgl->buffers[i]->name == name) return zgl->buffers[i];
    return NULL;
}

struct zgl_buffer **zgl_buffer_slot(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return &zgl->array_buffer;
    case GL_ELEMENT_ARRAY_BUFFER: return &zgl->vao->element_buffer;
    case GL_COPY_READ_BUFFER: return &zgl->copy_read_buffer;
    case GL_COPY_WRITE_BUFFER: return &zgl->copy_write_buffer;
    case GL_PIXEL_PACK_BUFFER: return &zgl->pixel_pack_buffer;
    case GL_PIXEL_UNPACK_BUFFER: return &zgl->pixel_unpack_buffer;
    case GL_UNIFORM_BUFFER: return &zgl->uniform_buffer;
    case GL_TEXTURE_BUFFER: return &zgl->texture_buffer;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return &zgl->xfb_buffer;
    }
    return NULL;
}

static struct zgl_buffer *buffer_new(GLuint name)
{
    struct zgl_buffer *b = calloc(1, sizeof *b);
    b->name = name;
    b->usage = GL_STATIC_DRAW;
    LIST_ADD(zgl->buffers, zgl->nbuffers, b);
    return b;
}

void glGenBuffers(GLsizei n, GLuint *b)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) b[i] = buffer_new(zgl->next_name++)->name;
}

/* Is the buffer still referenced by any binding? (GL keeps deleted buffers alive while attached.) */
static void unbind_everywhere(struct zgl_buffer *b)
{
    struct zgl_buffer **slots[] = { &zgl->array_buffer, &zgl->copy_read_buffer, &zgl->copy_write_buffer, &zgl->pixel_pack_buffer,
                                    &zgl->pixel_unpack_buffer, &zgl->uniform_buffer, &zgl->texture_buffer, &zgl->xfb_buffer };
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) if (*slots[i] == b) *slots[i] = NULL;
    /* bound VAO and the default: GL detaches from the bound one only; others keep it (we drop it too, simpler and safe) */
    struct zgl_vao *vaos[64]; int nv = 0;
    vaos[nv++] = &zgl->vao0;
    for (int i = 0; i < zgl->nvaos && nv < 64; i++) vaos[nv++] = zgl->vaos[i];
    for (int v = 0; v < nv; v++) {
        if (vaos[v]->element_buffer == b) vaos[v]->element_buffer = NULL;
        for (int i = 0; i < ZGL_MAX_ATTRIBS; i++) if (vaos[v]->attribs[i].buffer == b) vaos[v]->attribs[i].buffer = NULL;
    }
    for (int i = 0; i < ZGL_MAX_UBO_BINDINGS; i++) if (zgl->ubo[i].buf == b) zgl->ubo[i].buf = NULL;
    for (int i = 0; i < ZGL_MAX_XFB_BUFFERS; i++) if (zgl->xfb.buf[i] == b) zgl->xfb.buf[i] = NULL;
    for (int i = 0; i < zgl->ntextures; i++) if (zgl->textures[i]->buffer == b) { zgl->textures[i]->buffer = NULL; zgl->textures[i]->h_view = 0; }
    if (zgl->cl_vertex.buffer == b) zgl->cl_vertex.buffer = NULL;
    if (zgl->cl_normal.buffer == b) zgl->cl_normal.buffer = NULL;
    if (zgl->cl_color.buffer == b) zgl->cl_color.buffer = NULL;
    if (zgl->cl_texcoord.buffer == b) zgl->cl_texcoord.buffer = NULL;
}

static void buffer_free(struct zgl_buffer *b)
{
    unbind_everywhere(b);
    if (b->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, b->res); }
    LIST_DEL(zgl->buffers, zgl->nbuffers, b);
    free(b);
}

void glDeleteBuffers(GLsizei n, const GLuint *b)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) { if (!b[i]) continue; struct zgl_buffer *buf = zgl_buffer_get(b[i]); if (buf) buffer_free(buf); }
}

GLboolean glIsBuffer(GLuint b) { CTXV(0); return b && zgl_buffer_get(b) != NULL; }

void glBindBuffer(GLenum target, GLuint name)
{
    CTX();
    struct zgl_buffer **slot = zgl_buffer_slot(target);
    if (!slot) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_buffer *b = name ? zgl_buffer_get(name) : NULL;
    if (name && !b) { b = buffer_new(name); if (name >= zgl->next_name) zgl->next_name = name + 1; }   /* GL creates on bind */
    *slot = b;
}

int zgl_buffer_alloc(struct zgl_buffer *b, size_t size)
{
    if (b->res) { virgl_flush(zgl->v); virgl_res_destroy(zgl->v, b->res); b->res = NULL; }
    b->size = size; b->shadow = NULL; b->host_newer = 0; b->version++;
    for (int i = 0; i < zgl->ntextures; i++)            /* texture buffer views on the old resource */
        if (zgl->textures[i]->buffer == b && zgl->textures[i]->h_view) {
            virgl_destroy_object(zgl->v, zgl->textures[i]->h_view, VIRGL_OBJECT_SAMPLER_VIEW);
            zgl->textures[i]->h_view = 0;
            zgl->textures[i]->buf_size = size;
        }
    if (!size) return 0;
    b->res = virgl_res_create(zgl->v, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, buffer_binds(b), (uint32_t)size, 1, 1, 0, size);
    if (!b->res || !b->res->map) { b->size = 0; zgl_set_error(GL_OUT_OF_MEMORY); return -1; }
    b->shadow = b->res->map;
    return 0;
}

/* Buffers are backed by guest memory (the map doubles as the CPU copy):
 * write there, then transfer the range to the host. */
void zgl_buffer_write(struct zgl_buffer *b, size_t off, const void *data, size_t size)
{
    if (!size) return;
    b->version++;
    if (data != b->shadow + off) memcpy(b->shadow + off, data, size);
    virgl_res_to_host(zgl->v, b->res, 0, (uint32_t)off, 0, 0, (uint32_t)size, 1, 1, 0, 0, off);
}

/* The GPU wrote the buffer (a copy, stream output, a pack): bring the guest copy up to date. */
void zgl_buffer_sync(struct zgl_buffer *b)
{
    if (!b->host_newer || !b->res) return;
    virgl_flush(zgl->v);
    virgl_res_from_host(zgl->v, b->res, 0, 0, 0, 0, (uint32_t)b->size, 1, 1, 0, 0, 0);
    b->host_newer = 0;
    b->version++;
}

static struct zgl_buffer *target_buffer(GLenum target)
{
    struct zgl_buffer **slot = zgl_buffer_slot(target);
    if (!slot) { zgl_set_error(GL_INVALID_ENUM); return NULL; }
    if (!*slot) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    return *slot;
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    CTX();
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return;
    if (size < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (b->mapped) { b->mapped = 0; }
    b->usage = usage;
    if (zgl_buffer_alloc(b, (size_t)size) != 0 || !size) return;
    if (data) zgl_buffer_write(b, 0, data, (size_t)size);
    else memset(b->shadow, 0, (size_t)size);           /* GL leaves it undefined; zero is friendlier, and costs no transfer */
}

void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data)
{
    CTX();
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return;
    if (off < 0 || size < 0 || (size_t)(off + size) > b->size) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (b->mapped) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (!size) return;
    if (b->host_newer) zgl_buffer_sync(b);
    zgl_buffer_write(b, (size_t)off, data, (size_t)size);
}

void glGetBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, void *data)
{
    CTX();
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return;
    if (off < 0 || size < 0 || (size_t)(off + size) > b->size) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (b->mapped) { zgl_set_error(GL_INVALID_OPERATION); return; }
    zgl_buffer_sync(b);
    if (size) memcpy(data, b->shadow + off, (size_t)size);
}

void *glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    CTXV(NULL);
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return NULL;
    if (off < 0 || len < 0 || (size_t)(off + len) > b->size || !(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) ||
        (access & ~(GLbitfield)0xFF)) { zgl_set_error(GL_INVALID_VALUE); return NULL; }
    if (b->mapped) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    if ((access & GL_MAP_READ_BIT) && (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    if ((access & GL_MAP_FLUSH_EXPLICIT_BIT) && !(access & GL_MAP_WRITE_BIT)) { zgl_set_error(GL_INVALID_OPERATION); return NULL; }
    if (!b->shadow) { if (len == 0) { b->mapped = 1; b->map_access = access; b->map_off = 0; b->map_len = 0; static uint8_t dummy; return &dummy; } zgl_set_error(GL_OUT_OF_MEMORY); return NULL; }
    /* the guest copy is the mapping: current unless the GPU wrote the buffer */
    if (!(access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT))) zgl_buffer_sync(b);
    else if (b->host_newer) {
        /* invalidated: the old contents don't matter, but the rest of the buffer
         * outside the range must stay; sync only if part of it survives */
        if ((access & GL_MAP_INVALIDATE_BUFFER_BIT) && off == 0 && (size_t)len == b->size) b->host_newer = 0;
        else zgl_buffer_sync(b);
    }
    /* writes into memory the host may still be reading for queued draws: unless
     * the program promised not to (unsynchronized), let those finish first */
    if ((access & GL_MAP_WRITE_BIT) && !(access & GL_MAP_UNSYNCHRONIZED_BIT)) virgl_flush(zgl->v);
    b->mapped = 1; b->map_access = access; b->map_off = (size_t)off; b->map_len = (size_t)len;
    return b->shadow + off;
}

void *glMapBuffer(GLenum target, GLenum access)
{
    CTXV(NULL);
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return NULL;
    GLbitfield a = access == GL_READ_ONLY ? GL_MAP_READ_BIT : access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT : access == GL_READ_WRITE ? GL_MAP_READ_BIT | GL_MAP_WRITE_BIT : 0;
    if (!a) { zgl_set_error(GL_INVALID_ENUM); return NULL; }
    return glMapBufferRange(target, 0, (GLsizeiptr)b->size, a);
}

void glFlushMappedBufferRange(GLenum target, GLintptr off, GLsizeiptr len)
{
    CTX();
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return;
    if (!b->mapped || !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (off < 0 || len < 0 || (size_t)(off + len) > b->map_len) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (len) virgl_res_to_host(zgl->v, b->res, 0, (uint32_t)(b->map_off + off), 0, 0, (uint32_t)len, 1, 1, 0, 0, b->map_off + (size_t)off);
    b->version++;
}

GLboolean glUnmapBuffer(GLenum target)
{
    CTXV(GL_FALSE);
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return GL_FALSE;
    if (!b->mapped) { zgl_set_error(GL_INVALID_OPERATION); return GL_FALSE; }
    if ((b->map_access & GL_MAP_WRITE_BIT) && !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT) && b->map_len)
        virgl_res_to_host(zgl->v, b->res, 0, (uint32_t)b->map_off, 0, 0, (uint32_t)b->map_len, 1, 1, 0, 0, b->map_off);
    if (b->map_access & GL_MAP_WRITE_BIT) b->version++;
    b->mapped = 0; b->map_access = 0; b->map_off = b->map_len = 0;
    return GL_TRUE;
}

void glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64 *v)
{
    CTX();
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return;
    switch (pname) {
    case GL_BUFFER_SIZE: *v = (GLint64)b->size; break;
    case GL_BUFFER_USAGE: *v = b->usage; break;
    case GL_BUFFER_ACCESS: *v = (b->map_access & GL_MAP_READ_BIT) && (b->map_access & GL_MAP_WRITE_BIT) ? GL_READ_WRITE : (b->map_access & GL_MAP_READ_BIT) ? GL_READ_ONLY : GL_WRITE_ONLY; break;
    case GL_BUFFER_ACCESS_FLAGS: *v = b->map_access; break;
    case GL_BUFFER_MAPPED: *v = b->mapped; break;
    case GL_BUFFER_MAP_OFFSET: *v = (GLint64)b->map_off; break;
    case GL_BUFFER_MAP_LENGTH: *v = (GLint64)b->map_len; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *v)
{
    GLint64 x = 0;
    GLenum before = zgl ? zgl->error : 0;
    glGetBufferParameteri64v(target, pname, &x);
    if (!zgl || (zgl->error != before && before == GL_NO_ERROR)) return;
    *v = (GLint)x;
}

void glGetBufferPointerv(GLenum target, GLenum pname, void **p)
{
    CTX();
    struct zgl_buffer *b = target_buffer(target);
    if (!b) return;
    if (pname != GL_BUFFER_MAP_POINTER) { zgl_set_error(GL_INVALID_ENUM); return; }
    *p = b->mapped && b->shadow ? b->shadow + b->map_off : NULL;
}

void glCopyBufferSubData(GLenum rt, GLenum wt, GLintptr roff, GLintptr woff, GLsizeiptr size)
{
    CTX();
    struct zgl_buffer *r = target_buffer(rt), *w = r ? target_buffer(wt) : NULL;
    if (!r || !w) return;
    if (roff < 0 || woff < 0 || size < 0 || (size_t)(roff + size) > r->size || (size_t)(woff + size) > w->size) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (r->mapped || w->mapped) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (r == w && ((roff <= woff && woff < roff + size) || (woff <= roff && roff < woff + size))) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!size) return;
    /* through the guest copies (macOS GL can't copy between buffers on the host) */
    zgl_buffer_sync(r);
    zgl_buffer_sync(w);
    zgl_buffer_write(w, (size_t)woff, r->shadow + roff, (size_t)size);
}

/* ---- indexed binding points: uniform blocks and transform feedback ----------------------- */

static void bind_range(GLenum target, GLuint index, GLuint name, GLintptr off, GLsizeiptr size, int whole)
{
    struct zgl_buffer *b = name ? zgl_buffer_get(name) : NULL;
    if (name && !b) { b = buffer_new(name); if (name >= zgl->next_name) zgl->next_name = name + 1; }
    if (target == GL_UNIFORM_BUFFER) {
        if (index >= ZGL_MAX_UBO_BINDINGS) { zgl_set_error(GL_INVALID_VALUE); return; }
        if (!whole && b && (size <= 0 || off < 0 || (off & 15))) { zgl_set_error(GL_INVALID_VALUE); return; }   /* offset alignment 16 */
        zgl->ubo[index].buf = b; zgl->ubo[index].off = whole ? 0 : (size_t)off; zgl->ubo[index].size = whole ? 0 : (size_t)size;
        zgl->uniform_buffer = b;
    } else if (target == GL_TRANSFORM_FEEDBACK_BUFFER) {
        if (index >= ZGL_MAX_XFB_BUFFERS) { zgl_set_error(GL_INVALID_VALUE); return; }
        if (zgl->xfb.active) { zgl_set_error(GL_INVALID_OPERATION); return; }
        if (!whole && b && (size <= 0 || off < 0 || (off & 3) || (size & 3))) { zgl_set_error(GL_INVALID_VALUE); return; }
        if (zgl->xfb.h_target[index]) { virgl_destroy_object(zgl->v, zgl->xfb.h_target[index], VIRGL_OBJECT_STREAMOUT_TARGET); zgl->xfb.h_target[index] = 0; }
        zgl->xfb.buf[index] = b; zgl->xfb.off[index] = whole ? 0 : (size_t)off; zgl->xfb.size[index] = whole ? 0 : (size_t)size;
        zgl->xfb_buffer = b;
    } else zgl_set_error(GL_INVALID_ENUM);
}

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr off, GLsizeiptr size) { CTX(); bind_range(target, index, buffer, off, size, 0); }
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) { CTX(); bind_range(target, index, buffer, 0, 0, 1); }

/* ---- vertex array objects ---------------------------------------------------------------- */

static struct zgl_vao *vao_get(GLuint name)
{
    for (int i = 0; i < zgl->nvaos; i++) if (zgl->vaos[i]->name == name) return zgl->vaos[i];
    return NULL;
}

void glGenVertexArrays(GLsizei n, GLuint *a)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_vao *v = calloc(1, sizeof *v);
        v->name = zgl->next_name++;
        LIST_ADD(zgl->vaos, zgl->nvaos, v);
        a[i] = v->name;
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint *a)
{
    CTX();
    for (GLsizei i = 0; i < n; i++) {
        struct zgl_vao *v = a[i] ? vao_get(a[i]) : NULL;
        if (!v) continue;
        if (zgl->vao == v) zgl->vao = &zgl->vao0;
        LIST_DEL(zgl->vaos, zgl->nvaos, v);
        free(v);
    }
}

void glBindVertexArray(GLuint name)
{
    CTX();
    struct zgl_vao *v = name ? vao_get(name) : &zgl->vao0;
    if (!v) { zgl_set_error(GL_INVALID_OPERATION); return; }
    v->bound_once = 1;
    zgl->vao = v;
}

GLboolean glIsVertexArray(GLuint name) { CTXV(0); struct zgl_vao *v = name ? vao_get(name) : NULL; return v && v->bound_once; }

/* ---- vertex attributes --------------------------------------------------------------------- */

static int attrib_type_ok(GLenum type, int integer)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_INT: case GL_UNSIGNED_INT: return 1;
    case GL_FLOAT: case GL_HALF_FLOAT: case GL_DOUBLE: case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return !integer;
    case GL_FIXED: return !integer;
    }
    return 0;
}

static void attrib_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *p, int integer)
{
    if (index >= ZGL_MAX_ATTRIBS || stride < 0 || ((size < 1 || size > 4) && !(size == GL_BGRA && !integer))) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!attrib_type_ok(type, integer)) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (size == GL_BGRA && (type != GL_UNSIGNED_BYTE || !normalized)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    /* a non-zero pointer with no buffer: client memory, allowed in the compatibility profile with VAO 0 */
    if (!zgl->array_buffer && p && zgl->vao != &zgl->vao0) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_attrib *a = &ZGL_ATTRIBS[index];
    a->size = size; a->type = type; a->normalized = normalized != 0; a->stride = stride; a->pointer = p;
    a->integer = integer;
    a->buffer = zgl->array_buffer;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *p) { CTX(); attrib_pointer(index, size, type, normalized, stride, p, 0); }
void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *p) { CTX(); attrib_pointer(index, size, type, 0, stride, p, 1); }
void glEnableVertexAttribArray(GLuint index) { CTX(); if (index >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; } ZGL_ATTRIBS[index].enabled = 1; }
void glDisableVertexAttribArray(GLuint index) { CTX(); if (index >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; } ZGL_ATTRIBS[index].enabled = 0; }
void glVertexAttribDivisor(GLuint index, GLuint divisor) { CTX(); if (index >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; } ZGL_ATTRIBS[index].divisor = (int)divisor; }

/* current values: context state, typed */
static void cur_f(GLuint i, float x, float y, float z, float w)
{
    if (!zgl) return;
    if (i >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; }
    zgl->cur_attrib[i].v.f[0] = x; zgl->cur_attrib[i].v.f[1] = y; zgl->cur_attrib[i].v.f[2] = z; zgl->cur_attrib[i].v.f[3] = w;
    zgl->cur_attrib[i].type = GL_FLOAT;
}
static void cur_i(GLuint i, int32_t x, int32_t y, int32_t z, int32_t w, GLenum type)
{
    if (!zgl) return;
    if (i >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; }
    zgl->cur_attrib[i].v.i[0] = x; zgl->cur_attrib[i].v.i[1] = y; zgl->cur_attrib[i].v.i[2] = z; zgl->cur_attrib[i].v.i[3] = w;
    zgl->cur_attrib[i].type = type;
}

void glVertexAttrib1f(GLuint i, GLfloat x) { cur_f(i, x, 0, 0, 1); }
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { cur_f(i, x, y, 0, 1); }
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { cur_f(i, x, y, z, 1); }
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { cur_f(i, x, y, z, w); }
void glVertexAttrib1fv(GLuint i, const GLfloat *v) { cur_f(i, v[0], 0, 0, 1); }
void glVertexAttrib2fv(GLuint i, const GLfloat *v) { cur_f(i, v[0], v[1], 0, 1); }
void glVertexAttrib3fv(GLuint i, const GLfloat *v) { cur_f(i, v[0], v[1], v[2], 1); }
void glVertexAttrib4fv(GLuint i, const GLfloat *v) { cur_f(i, v[0], v[1], v[2], v[3]); }
void glVertexAttrib1d(GLuint i, GLdouble x) { cur_f(i, (float)x, 0, 0, 1); }
void glVertexAttrib2d(GLuint i, GLdouble x, GLdouble y) { cur_f(i, (float)x, (float)y, 0, 1); }
void glVertexAttrib3d(GLuint i, GLdouble x, GLdouble y, GLdouble z) { cur_f(i, (float)x, (float)y, (float)z, 1); }
void glVertexAttrib4d(GLuint i, GLdouble x, GLdouble y, GLdouble z, GLdouble w) { cur_f(i, (float)x, (float)y, (float)z, (float)w); }
void glVertexAttrib1s(GLuint i, GLshort x) { cur_f(i, x, 0, 0, 1); }
void glVertexAttrib2s(GLuint i, GLshort x, GLshort y) { cur_f(i, x, y, 0, 1); }
void glVertexAttrib3s(GLuint i, GLshort x, GLshort y, GLshort z) { cur_f(i, x, y, z, 1); }
void glVertexAttrib4s(GLuint i, GLshort x, GLshort y, GLshort z, GLshort w) { cur_f(i, x, y, z, w); }
void glVertexAttrib4Nub(GLuint i, GLubyte x, GLubyte y, GLubyte z, GLubyte w) { cur_f(i, x / 255.0f, y / 255.0f, z / 255.0f, w / 255.0f); }
void glVertexAttrib4Nubv(GLuint i, const GLubyte *v) { glVertexAttrib4Nub(i, v[0], v[1], v[2], v[3]); }
void glVertexAttribI1i(GLuint i, GLint x) { cur_i(i, x, 0, 0, 1, GL_INT); }
void glVertexAttribI2i(GLuint i, GLint x, GLint y) { cur_i(i, x, y, 0, 1, GL_INT); }
void glVertexAttribI3i(GLuint i, GLint x, GLint y, GLint z) { cur_i(i, x, y, z, 1, GL_INT); }
void glVertexAttribI4i(GLuint i, GLint x, GLint y, GLint z, GLint w) { cur_i(i, x, y, z, w, GL_INT); }
void glVertexAttribI1ui(GLuint i, GLuint x) { cur_i(i, (int32_t)x, 0, 0, 1, GL_UNSIGNED_INT); }
void glVertexAttribI2ui(GLuint i, GLuint x, GLuint y) { cur_i(i, (int32_t)x, (int32_t)y, 0, 1, GL_UNSIGNED_INT); }
void glVertexAttribI3ui(GLuint i, GLuint x, GLuint y, GLuint z) { cur_i(i, (int32_t)x, (int32_t)y, (int32_t)z, 1, GL_UNSIGNED_INT); }
void glVertexAttribI4ui(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w) { cur_i(i, (int32_t)x, (int32_t)y, (int32_t)z, (int32_t)w, GL_UNSIGNED_INT); }
void glVertexAttribI1iv(GLuint i, const GLint *v) { cur_i(i, v[0], 0, 0, 1, GL_INT); }
void glVertexAttribI2iv(GLuint i, const GLint *v) { cur_i(i, v[0], v[1], 0, 1, GL_INT); }
void glVertexAttribI3iv(GLuint i, const GLint *v) { cur_i(i, v[0], v[1], v[2], 1, GL_INT); }
void glVertexAttribI4iv(GLuint i, const GLint *v) { cur_i(i, v[0], v[1], v[2], v[3], GL_INT); }
void glVertexAttribI1uiv(GLuint i, const GLuint *v) { cur_i(i, (int32_t)v[0], 0, 0, 1, GL_UNSIGNED_INT); }
void glVertexAttribI2uiv(GLuint i, const GLuint *v) { cur_i(i, (int32_t)v[0], (int32_t)v[1], 0, 1, GL_UNSIGNED_INT); }
void glVertexAttribI3uiv(GLuint i, const GLuint *v) { cur_i(i, (int32_t)v[0], (int32_t)v[1], (int32_t)v[2], 1, GL_UNSIGNED_INT); }
void glVertexAttribI4uiv(GLuint i, const GLuint *v) { cur_i(i, (int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3], GL_UNSIGNED_INT); }
void glVertexAttribI4bv(GLuint i, const GLbyte *v) { cur_i(i, v[0], v[1], v[2], v[3], GL_INT); }
void glVertexAttribI4sv(GLuint i, const GLshort *v) { cur_i(i, v[0], v[1], v[2], v[3], GL_INT); }
void glVertexAttribI4ubv(GLuint i, const GLubyte *v) { cur_i(i, v[0], v[1], v[2], v[3], GL_UNSIGNED_INT); }
void glVertexAttribI4usv(GLuint i, const GLushort *v) { cur_i(i, v[0], v[1], v[2], v[3], GL_UNSIGNED_INT); }

static int get_attrib(GLuint index, GLenum pname, GLint *iv, GLfloat *fv, GLdouble *dv, int as_int)
{
    if (index >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return 0; }
    struct zgl_attrib *a = &ZGL_ATTRIBS[index];
    GLint x;
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: x = a->enabled; break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: x = a->size ? a->size : 4; break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: x = a->stride; break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: x = a->type ? (GLint)a->type : GL_FLOAT; break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: x = a->normalized; break;
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: x = a->integer; break;
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: x = a->divisor; break;
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: x = a->buffer ? (GLint)a->buffer->name : 0; break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (int k = 0; k < 4; k++) {
            if (as_int) iv[k] = zgl->cur_attrib[index].v.i[k];
            else {
                float f = zgl->cur_attrib[index].type == GL_FLOAT ? zgl->cur_attrib[index].v.f[k] :
                          zgl->cur_attrib[index].type == GL_INT ? (float)zgl->cur_attrib[index].v.i[k] : (float)zgl->cur_attrib[index].v.u[k];
                if (iv) iv[k] = (GLint)f; else if (fv) fv[k] = f; else dv[k] = f;
            }
        }
        return 1;
    default: zgl_set_error(GL_INVALID_ENUM); return 0;
    }
    if (iv) iv[0] = x; else if (fv) fv[0] = (GLfloat)x; else dv[0] = x;
    return 1;
}

void glGetVertexAttribiv(GLuint i, GLenum pname, GLint *v) { CTX(); get_attrib(i, pname, v, NULL, NULL, 0); }
void glGetVertexAttribfv(GLuint i, GLenum pname, GLfloat *v) { CTX(); get_attrib(i, pname, NULL, v, NULL, 0); }
void glGetVertexAttribdv(GLuint i, GLenum pname, GLdouble *v) { CTX(); get_attrib(i, pname, NULL, NULL, v, 0); }
void glGetVertexAttribIiv(GLuint i, GLenum pname, GLint *v) { CTX(); get_attrib(i, pname, v, NULL, NULL, 1); }
void glGetVertexAttribIuiv(GLuint i, GLenum pname, GLuint *v) { CTX(); get_attrib(i, pname, (GLint *)v, NULL, NULL, 1); }

void glGetVertexAttribPointerv(GLuint i, GLenum pname, void **p)
{
    CTX();
    if (i >= ZGL_MAX_ATTRIBS) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) { zgl_set_error(GL_INVALID_ENUM); return; }
    *p = (void *)ZGL_ATTRIBS[i].pointer;
}
