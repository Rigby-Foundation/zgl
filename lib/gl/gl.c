/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* Shaders, programs and uniforms (the default block and uniform blocks),
 * and the draw entry points. The state lives in state.c, objects in buf.c,
 * tex.c and fbo.c, the draw itself in draw.c. */
#include "zgl.h"
#include <GL/zgl_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct zgl_ctx *zgl;

#define CTX() do { if (!zgl) return; } while (0)
#define CTXV(v) do { if (!zgl) return (v); } while (0)

static GLuint new_name(void) { return zgl->next_name++; }

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
        if (strcmp(p->uniforms[i].name, v->name) == 0 && p->uniforms[i].block < 0) {
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
    u->block = -1;
    return u;
}

/* the program's uniform blocks: the stages' blocks merged by name */
static int program_blocks(struct zgl_program *p)
{
    p->nblocks = 0;
    for (int st = 0; st < 2; st++) {
        const struct zgl_compiled *c = st ? &p->fs->c : &p->vs->c;
        for (int i = 0; i < c->nblocks; i++) {
            const struct zgl_block *b = &c->blocks[i];
            int k;
            for (k = 0; k < p->nblocks; k++) if (!strcmp(p->blocks[k].name, b->name)) break;
            if (k == p->nblocks) {
                if (p->nblocks >= ZGL_MAX_STAGE_UBOS * 2) { snprintf(p->log, sizeof p->log, "too many uniform blocks\n"); return -1; }
                memset(&p->blocks[k], 0, sizeof p->blocks[k]);
                snprintf(p->blocks[k].name, sizeof p->blocks[k].name, "%s", b->name);
                p->blocks[k].vs_index = p->blocks[k].fs_index = -1;
                p->blocks[k].binding = b->binding >= 0 ? b->binding : 0;
                p->nblocks++;
                /* its members are active uniforms (with no location) */
                for (int m = 0; m < b->nmembers; m++) {
                    if (p->nuniforms >= ZGL_MAX_VARS * 2) break;
                    struct zgl_uniform *u = &p->uniforms[p->nuniforms++];
                    memset(u, 0, sizeof *u);
                    snprintf(u->name, sizeof u->name, "%s", b->members[m].name);
                    char *br = strchr(u->name, '[');
                    if (br && b->members[m].array && !strcmp(br, "[0]") && !strchr(br + 1, '[')) *br = 0;   /* arrays are listed as name[0]; stored by base name */
                    u->type = b->members[m].type; u->array = b->members[m].array;
                    u->vs_slot = u->fs_slot = -1;
                    u->block = k; u->offset = b->members[m].offset; u->array_stride = b->members[m].array_stride;
                    u->matrix_stride = b->members[m].matrix_stride; u->row_major = b->members[m].row_major;
                }
            } else if (p->blocks[k].size != b->size) { snprintf(p->log, sizeof p->log, "uniform block '%s' differs between the stages\n", b->name); return -1; }
            p->blocks[k].size = b->size;
            p->blocks[k].src = NULL;
            if (st) p->blocks[k].fs_index = b->cb; else p->blocks[k].vs_index = b->cb;
        }
    }
    return 0;
}

int zgl_link(struct zgl_program *p)
{
    p->linked = 0;
    p->log[0] = 0;
    if (!p->vs || !p->fs) { snprintf(p->log, sizeof p->log, "a vertex and a fragment shader are required\n"); return -1; }
    zgl_compiled_free(&p->vs->c); zgl_compiled_free(&p->fs->c);
    char log[512];
    if (zgl_compile(p->vs->source, GL_VERTEX_SHADER, &p->vs->c, log, sizeof log) != 0) { snprintf(p->log, sizeof p->log, "vertex shader: %s", log); return -1; }
    if (zgl_compile_linked_ex(p->fs->source, GL_FRAGMENT_SHADER, &p->vs->c, (const char (*)[48])p->bound_frag, p->bound_frag_index1[0] ? p->bound_frag_index1 : NULL,
                              &p->fs->c, log, sizeof log) != 0) { snprintf(p->log, sizeof p->log, "fragment shader: %s", log); return -1; }
    /* transform feedback: what the vertex shader writes, where */
    struct zgl_so_info so;
    if (zgl_xfb_layout(p, &so) != 0) return -1;
    /* host objects */
    if (p->h_vs) virgl_destroy_object(zgl->v, p->h_vs, VIRGL_OBJECT_SHADER);
    if (p->h_fs) virgl_destroy_object(zgl->v, p->h_fs, VIRGL_OBJECT_SHADER);
    p->h_vs = virgl_handle(zgl->v);
    p->h_fs = virgl_handle(zgl->v);
    if (so.n) virgl_create_shader_so(zgl->v, p->h_vs, PIPE_SHADER_VERTEX, p->vs->c.tgsi, so.n, so.stride, so.out);
    else virgl_create_shader(zgl->v, p->h_vs, PIPE_SHADER_VERTEX, p->vs->c.tgsi);
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
    for (int i = 0; i < p->vs->c.nsamplers; i++) { struct zgl_uniform *u = program_uniform_add(p, &p->vs->c.samplers[i], 1); if (u) { u->vs_slot = -1; u->slots = 0; } }
    if (program_blocks(p) != 0) return -1;
    p->vs_nconsts = p->vs->c.nconsts; p->fs_nconsts = p->fs->c.nconsts;
    memset(p->vs_consts, 0, sizeof p->vs_consts); memset(p->fs_consts, 0, sizeof p->fs_consts);
    p->consts_dirty = 1;
    /* attribute locations: layout(location) and gl_* built-ins are fixed,
     * glBindAttribLocation next, the rest in order; a matrix takes one
     * location per column. The last location is gl_InstanceID's. */
    uint32_t used = 1u << (ZGL_MAX_ATTRIBS - 1);
    p->attrib_mask = 0;
    p->instance_in = -1;
    for (int i = 0; i < p->vs->c.nattribs; i++) p->attrib_loc[i] = -1;
    for (int i = 0; i < p->vs->c.nattribs; i++)
        if (!strcmp(p->vs->c.attribs[i].name, "gl_InstanceID")) { p->attrib_loc[i] = ZGL_MAX_ATTRIBS - 1; p->instance_in = p->vs->c.attribs[i].slot; }
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < p->vs->c.nattribs; i++) {
            const struct zgl_var *a = &p->vs->c.attribs[i];
            int n = a->slots > 0 ? a->slots : 1;
            uint32_t span = ((1u << n) - 1);
            if (p->attrib_loc[i] >= 0) continue;
            int loc = -1;
            if (pass == 0) loc = a->location >= 0 ? a->location : a->builtin ? a->builtin - 1 : -1;
            else if (pass == 1) { for (int k = 0; k < ZGL_MAX_ATTRIBS; k++) if (p->bound_attr[k][0] && strcmp(p->bound_attr[k], a->name) == 0) loc = k; }
            else for (int k = 0; k + n <= ZGL_MAX_ATTRIBS - 1; k++) if (!(used & (span << k))) { loc = k; break; }
            if (loc < 0) { if (pass == 2) { snprintf(p->log, sizeof p->log, "too many attributes\n"); return -1; } continue; }
            if (loc + n > ZGL_MAX_ATTRIBS - 1) { snprintf(p->log, sizeof p->log, "attribute '%s': location %d out of range\n", a->name, loc); return -1; }
            p->attrib_loc[i] = loc;
            used |= span << loc;
        }
    p->nin = 0;
    for (int i = 0; i < p->vs->c.nattribs; i++) {
        const struct zgl_var *a = &p->vs->c.attribs[i];
        for (int c = 0; c < (a->slots > 0 ? a->slots : 1); c++)
            if (a->slot + c < ZGL_MAX_ATTRIBS) {
                p->in_loc[a->slot + c] = a->slot == p->instance_in ? -1 : p->attrib_loc[i] + c;
                if (a->slot + c + 1 > p->nin) p->nin = a->slot + c + 1;
            }
    }
    p->attrib_mask = used & ~(1u << (ZGL_MAX_ATTRIBS - 1));
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

static int is_hidden_attrib(const struct zgl_var *a) { return !strcmp(a->name, "gl_InstanceID"); }

static int name_len(const char *n) { return (int)strlen(n) + 1; }

/* GL's name for an active uniform: arrays are name[0] */
static void uniform_gl_name(const struct zgl_uniform *u, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s", u->name, u->array && !strchr(u->name, '[') ? "[0]" : "");
}

void glGetProgramiv(GLuint name, GLenum pname, GLint *v)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(shader_get(name) ? GL_INVALID_OPERATION : GL_INVALID_VALUE); return; }
    switch (pname) {
    case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *v = p->linked; break;
    case GL_INFO_LOG_LENGTH: *v = (GLint)strlen(p->log) + 1; break;
    case GL_ATTACHED_SHADERS: *v = (p->vs != NULL) + (p->fs != NULL); break;
    case GL_DELETE_STATUS: *v = p->deleted; break;
    case GL_ACTIVE_UNIFORMS: *v = p->linked ? p->nuniforms : 0; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: {
        int m = 0;
        for (int i = 0; p->linked && i < p->nuniforms; i++) { char nm[64]; uniform_gl_name(&p->uniforms[i], nm, sizeof nm); if (name_len(nm) > m) m = name_len(nm); }
        *v = m; break;
    }
    case GL_ACTIVE_ATTRIBUTES: {
        int n = 0;
        for (int i = 0; p->linked && i < p->vs->c.nattribs; i++) if (!is_hidden_attrib(&p->vs->c.attribs[i])) n++;
        *v = n; break;
    }
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: {
        int m = 0;
        for (int i = 0; p->linked && i < p->vs->c.nattribs; i++) if (!is_hidden_attrib(&p->vs->c.attribs[i]) && name_len(p->vs->c.attribs[i].name) > m) m = name_len(p->vs->c.attribs[i].name);
        *v = m; break;
    }
    case GL_ACTIVE_UNIFORM_BLOCKS: *v = p->linked ? p->nblocks : 0; break;
    case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH: {
        int m = 0;
        for (int i = 0; p->linked && i < p->nblocks; i++) if (name_len(p->blocks[i].name) > m) m = name_len(p->blocks[i].name);
        *v = m; break;
    }
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: *v = p->xfb_mode ? (GLint)p->xfb_mode : GL_INTERLEAVED_ATTRIBS; break;
    case GL_TRANSFORM_FEEDBACK_VARYINGS: *v = p->linked ? p->nxfb : 0; break;
    case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH: {
        int m = 0;
        for (int i = 0; p->linked && i < p->nxfb; i++) if (name_len(p->xfb_names[i]) > m) m = name_len(p->xfb_names[i]);
        *v = m; break;
    }
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

GLboolean glIsProgram(GLuint name) { CTXV(0); return name && program_get(name) != NULL; }
GLboolean glIsShader(GLuint name) { CTXV(0); return name && shader_get(name) != NULL; }

void glGetAttachedShaders(GLuint name, GLsizei max, GLsizei *count, GLuint *shaders)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    int n = 0;
    if (p->vs && n < max) shaders[n++] = p->vs->name;
    if (p->fs && n < max) shaders[n++] = p->fs->name;
    if (count) *count = n;
}

void glGetShaderSource(GLuint name, GLsizei max, GLsizei *len, GLchar *src)
{
    CTX();
    struct zgl_shader *s = shader_get(name);
    if (!s) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (max <= 0) { if (len) *len = 0; return; }
    size_t n = s->source ? strlen(s->source) : 0;
    if (n >= (size_t)max) n = (size_t)max - 1;
    if (n) memcpy(src, s->source, n);
    src[n] = 0;
    if (len) *len = (GLsizei)n;
}

/* ---- attributes ------------------------------------------------------------------------------ */

GLint glGetAttribLocation(GLuint name, const GLchar *attr)
{
    CTXV(-1);
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    if (!strncmp(attr, "gl_", 3)) return -1;
    for (int i = 0; i < p->vs->c.nattribs; i++)
        if (strcmp(p->vs->c.attribs[i].name, attr) == 0) return p->attrib_loc[i];
    return -1;
}

static GLenum gl_type_of(enum zgl_type t);

void glGetActiveAttrib(GLuint name, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *out)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    GLuint k = 0;
    for (int i = 0; p->linked && i < p->vs->c.nattribs; i++) {
        const struct zgl_var *a = &p->vs->c.attribs[i];
        if (is_hidden_attrib(a)) continue;
        if (k++ != index) continue;
        if (size) *size = a->array ? a->array : 1;
        if (type) *type = gl_type_of(a->type);
        if (bufsize > 0) { snprintf(out, (size_t)bufsize, "%s", a->name); if (length) *length = (GLsizei)strlen(out); }
        else if (length) *length = 0;
        return;
    }
    zgl_set_error(GL_INVALID_VALUE);
}

/* ---- uniforms ---------------------------------------------------------------------------------- */

/* uniform locations: index into the program's table, with array element offsets in the high bits */
GLint glGetUniformLocation(GLuint name, const GLchar *uname)
{
    CTXV(-1);
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    char base[64]; int elem = 0;
    snprintf(base, sizeof base, "%s", uname);
    /* a trailing [n] is an element; inner brackets are part of the name (struct arrays) */
    size_t L = strlen(base);
    if (L && base[L - 1] == ']') {
        char *br = strrchr(base, '[');
        if (br) {
            char tmp[64]; snprintf(tmp, sizeof tmp, "%.*s", (int)(br - base), base);
            for (int i = 0; i < p->nuniforms; i++)
                if (p->uniforms[i].block < 0 && !strcmp(p->uniforms[i].name, base)) return i;   /* a leaf named with an index ("s[1].a" style) */
            elem = atoi(br + 1);
            *br = 0;
        }
    }
    for (int i = 0; i < p->nuniforms; i++)
        if (p->uniforms[i].block < 0 && strcmp(p->uniforms[i].name, base) == 0) {
            if (elem && !p->uniforms[i].array) return -1;
            if (p->uniforms[i].array && elem >= p->uniforms[i].array) return -1;
            return i | (elem << 16);
        }
    return -1;
}

void zgl_uniform_set(struct zgl_program *p, int loc, const void *data_, int count, int is_matrix_cols)
{
    const uint32_t *data = data_;
    int idx = loc & 0xFFFF, elem = loc >> 16;
    if (idx < 0 || idx >= p->nuniforms) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_uniform *u = &p->uniforms[idx];
    int per = zgl_type_slots(u->type);              /* vec4 slots (columns) per element */
    int rows = zgl_type_vec(u->type);               /* components per slot */
    int width = is_matrix_cols ? per * rows : rows; /* words per element in data */
    int max_elems = u->array ? u->array : 1;
    for (int e = 0; e < count && elem + e < max_elems; e++) {
        for (int col = 0; col < per; col++) {
            const uint32_t *src = data + (size_t)e * width + (is_matrix_cols ? col * rows : 0);
            int n = rows;
            int slot = (elem + e) * per + col;
            if (u->vs_slot >= 0 && u->vs_slot + slot < ZGL_MAX_CONSTS) memcpy(&p->vs_consts[(u->vs_slot + slot) * 4], src, (size_t)n * 4);
            if (u->fs_slot >= 0 && u->fs_slot + slot < ZGL_MAX_CONSTS) memcpy(&p->fs_consts[(u->fs_slot + slot) * 4], src, (size_t)n * 4);
        }
    }
    p->consts_dirty = 1;
}

static struct zgl_program *uniform_target(void) { return zgl->program; }

/* Values into the uniform's representation: floats stay floats, ints and
 * uints are stored as themselves, bools as ~0/0 (what the compiled code
 * compares against). kind: 0 float, 1 int, 2 uint. */
static void uniform_store(GLint loc, const void *vals, int kind, int width, int count)
{
    struct zgl_program *p = uniform_target();
    if (!p) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (loc == -1) return;
    if (count < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    int idx = loc & 0xFFFF, elem = loc >> 16;
    if (loc < 0 || idx >= p->nuniforms || p->uniforms[idx].block >= 0) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_uniform *u = &p->uniforms[idx];
    if (count > 1 && !u->array) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (zgl_type_is_sampler(u->type)) {
        if (kind != 1 || width != 1) { zgl_set_error(GL_INVALID_OPERATION); return; }
        const GLint *iv = vals;
        for (int e = 0; e < count && elem + e < 16; e++) {
            if (iv[e] < 0 || iv[e] >= ZGL_MAX_UNITS) { zgl_set_error(GL_INVALID_VALUE); return; }
            u->sampler_units[elem + e] = iv[e];
        }
        u->sampler_unit = u->sampler_units[0];
        return;
    }
    if (zgl_type_vec(u->type) != width || zgl_type_cols(u->type)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    enum zgl_type base = zgl_type_base(u->type);
    if (base != T_BOOL && ((base == T_FLOAT) != (kind == 0) || (base == T_INT && kind != 1) || (base == T_UINT && kind != 2))) { zgl_set_error(GL_INVALID_OPERATION); return; }
    uint32_t w[64 * 4];
    for (int i = 0; i < count * width && i < 256; i++) {
        uint32_t x; memcpy(&x, (const uint32_t *)vals + i, 4);
        if (base == T_BOOL) {
            float f; memcpy(&f, &x, 4);
            w[i] = (kind ? x != 0 : f != 0.0f) ? 0xFFFFFFFFu : 0;
        } else w[i] = x;
    }
    zgl_uniform_set(p, loc, w, count, 0);
}

void glUniform1f(GLint loc, GLfloat x) { CTX(); float v[1] = { x }; uniform_store(loc, v, 0, 1, 1); }
void glUniform2f(GLint loc, GLfloat x, GLfloat y) { CTX(); float v[2] = { x, y }; uniform_store(loc, v, 0, 2, 1); }
void glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) { CTX(); float v[3] = { x, y, z }; uniform_store(loc, v, 0, 3, 1); }
void glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { CTX(); float v[4] = { x, y, z, w }; uniform_store(loc, v, 0, 4, 1); }
void glUniform1fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); uniform_store(loc, v, 0, 1, n); }
void glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); uniform_store(loc, v, 0, 2, n); }
void glUniform3fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); uniform_store(loc, v, 0, 3, n); }
void glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) { CTX(); uniform_store(loc, v, 0, 4, n); }

void glUniform1i(GLint loc, GLint x) { CTX(); uniform_store(loc, &x, 1, 1, 1); }
void glUniform2i(GLint loc, GLint x, GLint y) { CTX(); GLint v[2] = { x, y }; uniform_store(loc, v, 1, 2, 1); }
void glUniform3i(GLint loc, GLint x, GLint y, GLint z) { CTX(); GLint v[3] = { x, y, z }; uniform_store(loc, v, 1, 3, 1); }
void glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w) { CTX(); GLint v[4] = { x, y, z, w }; uniform_store(loc, v, 1, 4, 1); }
void glUniform1iv(GLint loc, GLsizei n, const GLint *v) { CTX(); uniform_store(loc, v, 1, 1, n); }
void glUniform2iv(GLint loc, GLsizei n, const GLint *v) { CTX(); uniform_store(loc, v, 1, 2, n); }
void glUniform3iv(GLint loc, GLsizei n, const GLint *v) { CTX(); uniform_store(loc, v, 1, 3, n); }
void glUniform4iv(GLint loc, GLsizei n, const GLint *v) { CTX(); uniform_store(loc, v, 1, 4, n); }

void glUniform1ui(GLint loc, GLuint x) { CTX(); uniform_store(loc, &x, 2, 1, 1); }
void glUniform2ui(GLint loc, GLuint x, GLuint y) { CTX(); GLuint v[2] = { x, y }; uniform_store(loc, v, 2, 2, 1); }
void glUniform3ui(GLint loc, GLuint x, GLuint y, GLuint z) { CTX(); GLuint v[3] = { x, y, z }; uniform_store(loc, v, 2, 3, 1); }
void glUniform4ui(GLint loc, GLuint x, GLuint y, GLuint z, GLuint w) { CTX(); GLuint v[4] = { x, y, z, w }; uniform_store(loc, v, 2, 4, 1); }
void glUniform1uiv(GLint loc, GLsizei n, const GLuint *v) { CTX(); uniform_store(loc, v, 2, 1, n); }
void glUniform2uiv(GLint loc, GLsizei n, const GLuint *v) { CTX(); uniform_store(loc, v, 2, 2, n); }
void glUniform3uiv(GLint loc, GLsizei n, const GLuint *v) { CTX(); uniform_store(loc, v, 2, 3, n); }
void glUniform4uiv(GLint loc, GLsizei n, const GLuint *v) { CTX(); uniform_store(loc, v, 2, 4, n); }

/* cols x rows matrices; the data is column-major unless transposed */
static void uniform_matrix(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v, int cols, int rows)
{
    struct zgl_program *p = uniform_target();
    if (!p) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (loc == -1) return;
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    int idx = loc & 0xFFFF;
    if (loc < 0 || idx >= p->nuniforms || p->uniforms[idx].block >= 0) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_uniform *u = &p->uniforms[idx];
    if (zgl_type_cols(u->type) != cols || zgl_type_vec(u->type) != rows) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (n > 1 && !u->array) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (!transpose) { zgl_uniform_set(p, loc, v, n, 1); return; }
    int sz = cols * rows;
    float *t = malloc((size_t)(n ? n : 1) * sz * sizeof *t);
    if (!t) { zgl_set_error(GL_OUT_OF_MEMORY); return; }
    for (GLsizei e = 0; e < n; e++)
        for (int c = 0; c < cols; c++)
            for (int r = 0; r < rows; r++)
                t[e * sz + c * rows + r] = v[e * sz + r * cols + c];
    zgl_uniform_set(p, loc, t, n, 1);
    free(t);
}
void glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 2, 2); }
void glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 3, 3); }
void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 4, 4); }
void glUniformMatrix2x3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 2, 3); }
void glUniformMatrix3x2fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 3, 2); }
void glUniformMatrix2x4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 2, 4); }
void glUniformMatrix4x2fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 4, 2); }
void glUniformMatrix3x4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 3, 4); }
void glUniformMatrix4x3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { CTX(); uniform_matrix(loc, n, tr, v, 4, 3); }

/* reading uniforms back: the words in the stage that has it */
static void get_uniform(GLuint name, GLint loc, void *out, int as)    /* as: 0 float, 1 int, 2 uint */
{
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return; }
    int idx = loc & 0xFFFF, elem = loc >> 16;
    if (loc < 0 || idx >= p->nuniforms || p->uniforms[idx].block >= 0) { zgl_set_error(GL_INVALID_OPERATION); return; }
    struct zgl_uniform *u = &p->uniforms[idx];
    if (zgl_type_is_sampler(u->type)) { if (as == 0) ((float *)out)[0] = (float)u->sampler_units[elem & 15]; else ((GLint *)out)[0] = u->sampler_units[elem & 15]; return; }
    int per = zgl_type_slots(u->type), rows = zgl_type_vec(u->type);
    const float *src = u->vs_slot >= 0 ? &p->vs_consts[(u->vs_slot + elem * per) * 4] : u->fs_slot >= 0 ? &p->fs_consts[(u->fs_slot + elem * per) * 4] : NULL;
    if (!src) return;
    enum zgl_type base = zgl_type_base(u->type);
    for (int c = 0; c < per; c++)
        for (int r = 0; r < rows; r++) {
            uint32_t w; memcpy(&w, &src[c * 4 + r], 4);
            double x = base == T_FLOAT ? (double)src[c * 4 + r] : base == T_INT ? (double)(int32_t)w : base == T_UINT ? (double)w : (w ? 1.0 : 0.0);
            int k = c * rows + r;
            if (as == 0) ((float *)out)[k] = (float)x;
            else if (as == 1) ((GLint *)out)[k] = base == T_FLOAT ? (GLint)x : (GLint)w * (base == T_BOOL ? -1 : 1);
            else ((GLuint *)out)[k] = base == T_FLOAT ? (GLuint)x : (base == T_BOOL ? (w != 0) : w);
            if (as == 1 && base == T_BOOL) ((GLint *)out)[k] = w != 0;
        }
}
void glGetUniformfv(GLuint p, GLint loc, GLfloat *v) { CTX(); get_uniform(p, loc, v, 0); }
void glGetUniformiv(GLuint p, GLint loc, GLint *v) { CTX(); get_uniform(p, loc, v, 1); }
void glGetUniformuiv(GLuint p, GLint loc, GLuint *v) { CTX(); get_uniform(p, loc, v, 2); }

static GLenum gl_type_of(enum zgl_type t)
{
    static const struct { enum zgl_type z; GLenum g; } map[] = {
        { T_FLOAT, GL_FLOAT }, { T_INT, GL_INT }, { T_BOOL, GL_BOOL }, { T_VEC2, GL_FLOAT_VEC2 }, { T_VEC3, GL_FLOAT_VEC3 }, { T_VEC4, GL_FLOAT_VEC4 },
        { T_IVEC2, GL_INT_VEC2 }, { T_IVEC3, GL_INT_VEC3 }, { T_IVEC4, GL_INT_VEC4 }, { T_BVEC2, GL_BOOL_VEC2 }, { T_BVEC3, GL_BOOL_VEC3 }, { T_BVEC4, GL_BOOL_VEC4 },
        { T_MAT2, GL_FLOAT_MAT2 }, { T_MAT3, GL_FLOAT_MAT3 }, { T_MAT4, GL_FLOAT_MAT4 }, { T_SAMPLER2D, GL_SAMPLER_2D }, { T_SAMPLERCUBE, GL_SAMPLER_CUBE },
        { T_UINT, GL_UNSIGNED_INT }, { T_UVEC2, GL_UNSIGNED_INT_VEC2 }, { T_UVEC3, GL_UNSIGNED_INT_VEC3 }, { T_UVEC4, GL_UNSIGNED_INT_VEC4 },
        { T_MAT2X3, GL_FLOAT_MAT2x3 }, { T_MAT2X4, GL_FLOAT_MAT2x4 }, { T_MAT3X2, GL_FLOAT_MAT3x2 }, { T_MAT3X4, GL_FLOAT_MAT3x4 }, { T_MAT4X2, GL_FLOAT_MAT4x2 }, { T_MAT4X3, GL_FLOAT_MAT4x3 },
        { T_SAMPLER1D, GL_SAMPLER_1D }, { T_SAMPLER3D, GL_SAMPLER_3D }, { T_SAMPLER2DRECT, GL_SAMPLER_2D_RECT }, { T_SAMPLER1DARRAY, GL_SAMPLER_1D_ARRAY },
        { T_SAMPLER2DARRAY, GL_SAMPLER_2D_ARRAY }, { T_SAMPLERBUFFER, GL_SAMPLER_BUFFER }, { T_SAMPLER2DMS, GL_SAMPLER_2D_MULTISAMPLE },
        { T_SAMPLER2DMSARRAY, GL_SAMPLER_2D_MULTISAMPLE_ARRAY }, { T_SAMPLER1DSHADOW, GL_SAMPLER_1D_SHADOW }, { T_SAMPLER2DSHADOW, GL_SAMPLER_2D_SHADOW },
        { T_SAMPLERCUBESHADOW, GL_SAMPLER_CUBE_SHADOW }, { T_SAMPLER2DRECTSHADOW, GL_SAMPLER_2D_RECT_SHADOW }, { T_SAMPLER1DARRAYSHADOW, GL_SAMPLER_1D_ARRAY_SHADOW },
        { T_SAMPLER2DARRAYSHADOW, GL_SAMPLER_2D_ARRAY_SHADOW },
        { T_ISAMPLER1D, GL_INT_SAMPLER_1D }, { T_ISAMPLER2D, GL_INT_SAMPLER_2D }, { T_ISAMPLER3D, GL_INT_SAMPLER_3D }, { T_ISAMPLERCUBE, GL_INT_SAMPLER_CUBE },
        { T_ISAMPLER2DRECT, GL_INT_SAMPLER_2D_RECT }, { T_ISAMPLER1DARRAY, GL_INT_SAMPLER_1D_ARRAY }, { T_ISAMPLER2DARRAY, GL_INT_SAMPLER_2D_ARRAY },
        { T_ISAMPLERBUFFER, GL_INT_SAMPLER_BUFFER }, { T_ISAMPLER2DMS, GL_INT_SAMPLER_2D_MULTISAMPLE }, { T_ISAMPLER2DMSARRAY, GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY },
        { T_USAMPLER1D, GL_UNSIGNED_INT_SAMPLER_1D }, { T_USAMPLER2D, GL_UNSIGNED_INT_SAMPLER_2D }, { T_USAMPLER3D, GL_UNSIGNED_INT_SAMPLER_3D },
        { T_USAMPLERCUBE, GL_UNSIGNED_INT_SAMPLER_CUBE }, { T_USAMPLER2DRECT, GL_UNSIGNED_INT_SAMPLER_2D_RECT }, { T_USAMPLER1DARRAY, GL_UNSIGNED_INT_SAMPLER_1D_ARRAY },
        { T_USAMPLER2DARRAY, GL_UNSIGNED_INT_SAMPLER_2D_ARRAY }, { T_USAMPLERBUFFER, GL_UNSIGNED_INT_SAMPLER_BUFFER },
        { T_USAMPLER2DMS, GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE }, { T_USAMPLER2DMSARRAY, GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].z == t) return map[i].g;
    return GL_FLOAT;
}

void glGetActiveUniform(GLuint name, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size, GLenum *type, GLchar *out)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!p->linked || index >= (GLuint)p->nuniforms) { zgl_set_error(GL_INVALID_VALUE); return; }
    const struct zgl_uniform *u = &p->uniforms[index];
    if (size) *size = u->array ? u->array : 1;
    if (type) *type = gl_type_of(u->type);
    char nm[64];
    uniform_gl_name(u, nm, sizeof nm);
    if (bufsize > 0) { snprintf(out, (size_t)bufsize, "%s", nm); if (length) *length = (GLsizei)strlen(out); }
    else if (length) *length = 0;
}

void glGetActiveUniformName(GLuint name, GLuint index, GLsizei bufsize, GLsizei *length, GLchar *out)
{
    GLint size; GLenum type;
    glGetActiveUniform(name, index, bufsize, length, &size, &type, out);
}

void glGetUniformIndices(GLuint name, GLsizei count, const GLchar *const *names, GLuint *indices)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei k = 0; k < count; k++) {
        indices[k] = GL_INVALID_INDEX;
        char nm[64];
        for (int i = 0; p->linked && i < p->nuniforms; i++) {
            uniform_gl_name(&p->uniforms[i], nm, sizeof nm);
            if (!strcmp(nm, names[k]) || !strcmp(p->uniforms[i].name, names[k])) { indices[k] = (GLuint)i; break; }
        }
    }
}

void glGetActiveUniformsiv(GLuint name, GLsizei count, const GLuint *indices, GLenum pname, GLint *params)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei k = 0; k < count; k++) if (indices[k] >= (GLuint)p->nuniforms) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei k = 0; k < count; k++) {
        const struct zgl_uniform *u = &p->uniforms[indices[k]];
        char nm[64];
        switch (pname) {
        case GL_UNIFORM_TYPE: params[k] = (GLint)gl_type_of(u->type); break;
        case GL_UNIFORM_SIZE: params[k] = u->array ? u->array : 1; break;
        case GL_UNIFORM_NAME_LENGTH: uniform_gl_name(u, nm, sizeof nm); params[k] = name_len(nm); break;
        case GL_UNIFORM_BLOCK_INDEX: params[k] = u->block; break;
        case GL_UNIFORM_OFFSET: params[k] = u->block >= 0 ? u->offset : -1; break;
        case GL_UNIFORM_ARRAY_STRIDE: params[k] = u->block >= 0 ? u->array_stride : -1; break;
        case GL_UNIFORM_MATRIX_STRIDE: params[k] = u->block >= 0 ? u->matrix_stride : -1; break;
        case GL_UNIFORM_IS_ROW_MAJOR: params[k] = u->block >= 0 ? u->row_major : 0; break;
        default: zgl_set_error(GL_INVALID_ENUM); return;
        }
    }
}

/* ---- uniform blocks ---------------------------------------------------------------------------- */

GLuint glGetUniformBlockIndex(GLuint name, const GLchar *block)
{
    CTXV(GL_INVALID_INDEX);
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return GL_INVALID_INDEX; }
    for (int i = 0; p->linked && i < p->nblocks; i++) if (!strcmp(p->blocks[i].name, block)) return (GLuint)i;
    return GL_INVALID_INDEX;
}

void glGetActiveUniformBlockName(GLuint name, GLuint index, GLsizei bufsize, GLsizei *length, GLchar *out)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!p->linked || index >= (GLuint)p->nblocks) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (bufsize > 0) { snprintf(out, (size_t)bufsize, "%s", p->blocks[index].name); if (length) *length = (GLsizei)strlen(out); }
    else if (length) *length = 0;
}

void glGetActiveUniformBlockiv(GLuint name, GLuint index, GLenum pname, GLint *params)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!p->linked || index >= (GLuint)p->nblocks) { zgl_set_error(GL_INVALID_VALUE); return; }
    int n = 0;
    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING: params[0] = p->blocks[index].binding; return;
    case GL_UNIFORM_BLOCK_DATA_SIZE: params[0] = p->blocks[index].size; return;
    case GL_UNIFORM_BLOCK_NAME_LENGTH: params[0] = name_len(p->blocks[index].name); return;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: for (int i = 0; i < p->nuniforms; i++) if (p->uniforms[i].block == (int)index) n++; params[0] = n; return;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: for (int i = 0; i < p->nuniforms; i++) if (p->uniforms[i].block == (int)index) params[n++] = i; return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: params[0] = p->blocks[index].vs_index >= 0; return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: params[0] = p->blocks[index].fs_index >= 0; return;
    case GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER: params[0] = 0; return;
    }
    zgl_set_error(GL_INVALID_ENUM);
}

void glUniformBlockBinding(GLuint name, GLuint index, GLuint binding)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!p->linked || index >= (GLuint)p->nblocks || binding >= ZGL_MAX_UBO_BINDINGS) { zgl_set_error(GL_INVALID_VALUE); return; }
    p->blocks[index].binding = (int)binding;
}

/* ---- fragment outputs --------------------------------------------------------------------------- */

void glBindFragDataLocationIndexed(GLuint name, GLuint color, GLuint index, const GLchar *out)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!strncmp(out, "gl_", 3)) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (index > 1 || color >= (index ? 1u : (GLuint)ZGL_MAX_DRAW_BUFFERS)) { zgl_set_error(GL_INVALID_VALUE); return; }
    /* a name is bound to one place: forget the others */
    for (int k = 0; k < ZGL_MAX_DRAW_BUFFERS; k++) if (!strcmp(p->bound_frag[k], out)) p->bound_frag[k][0] = 0;
    if (!strcmp(p->bound_frag_index1, out)) p->bound_frag_index1[0] = 0;
    if (index) snprintf(p->bound_frag_index1, sizeof p->bound_frag_index1, "%s", out);
    else snprintf(p->bound_frag[color], sizeof p->bound_frag[color], "%s", out);
}
void glBindFragDataLocation(GLuint name, GLuint color, const GLchar *out) { glBindFragDataLocationIndexed(name, color, 0, out); }

static const struct zgl_var *frag_output(struct zgl_program *p, const char *name, int *elem)
{
    char base[48]; *elem = 0;
    snprintf(base, sizeof base, "%s", name);
    char *br = strchr(base, '[');
    if (br) { *elem = atoi(br + 1); *br = 0; }
    for (int i = 0; i < p->fs->c.noutputs; i++) if (!strcmp(p->fs->c.outputs[i].name, base)) return &p->fs->c.outputs[i];
    return NULL;
}

GLint glGetFragDataLocation(GLuint name, const GLchar *out)
{
    CTXV(-1);
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    int e;
    const struct zgl_var *v = frag_output(p, out, &e);
    if (!v || (v->array && e >= v->array)) return -1;
    return v->slot + e;
}

GLint glGetFragDataIndex(GLuint name, const GLchar *out)
{
    CTXV(-1);
    struct zgl_program *p = program_get(name);
    if (!p || !p->linked) { zgl_set_error(GL_INVALID_OPERATION); return -1; }
    int e;
    const struct zgl_var *v = frag_output(p, out, &e);
    if (!v) return -1;
    return v->builtin == 1 ? 1 : 0;
}

/* ---- transform feedback varyings --------------------------------------------------------------- */

void glTransformFeedbackVaryings(GLuint name, GLsizei count, const GLchar *const *varyings, GLenum mode)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (mode != GL_INTERLEAVED_ATTRIBS && mode != GL_SEPARATE_ATTRIBS) { zgl_set_error(GL_INVALID_ENUM); return; }
    if (count < 0 || (mode == GL_SEPARATE_ATTRIBS && count > ZGL_MAX_XFB_BUFFERS)) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (int i = 0; i < p->nxfb; i++) free(p->xfb_names[i]);
    free(p->xfb_names);
    p->xfb_names = count ? calloc((size_t)count, sizeof *p->xfb_names) : NULL;
    p->nxfb = count;
    for (GLsizei i = 0; i < count; i++) p->xfb_names[i] = strdup(varyings[i]);
    p->xfb_mode = mode;
}

/* a varying's register, components and GL type, by its GL name */
static int xfb_find(struct zgl_program *p, const char *name, int *reg, int *regs, int *comps, enum zgl_type *type, int *size)
{
    const struct zgl_compiled *c = &p->vs->c;
    if (!strcmp(name, "gl_Position") && c->position_reg >= 0) { *reg = c->position_reg; *regs = 1; *comps = 4; *type = T_VEC4; *size = 1; return 0; }
    if (!strcmp(name, "gl_PointSize") && c->pointsize_reg >= 0) { *reg = c->pointsize_reg; *regs = 1; *comps = 1; *type = T_FLOAT; *size = 1; return 0; }
    char base[48]; int elem = -1;
    snprintf(base, sizeof base, "%s", name);
    char *br = strchr(base, '[');
    if (br) { elem = atoi(br + 1); *br = 0; }
    for (int i = 0; i < c->nvaryings; i++) {
        const struct zgl_var *v = &c->varyings[i];
        if (strcmp(v->name, base)) continue;
        int per = zgl_type_slots(v->type);
        *type = v->type; *comps = zgl_type_vec(v->type);
        if (elem >= 0) {
            if (!v->array || elem >= v->array) return -1;
            *reg = v->reg + elem * per; *regs = per; *size = 1;
        } else { *reg = v->reg; *regs = v->slots; *size = v->array ? v->array : 1; }
        return 0;
    }
    return -1;
}

int zgl_xfb_layout(struct zgl_program *p, struct zgl_so_info *so)
{
    memset(so, 0, sizeof *so);
    if (!p->nxfb) return 0;
    uint32_t off[4] = { 0 };
    for (int i = 0; i < p->nxfb; i++) {
        int reg, regs, comps, size; enum zgl_type t;
        if (xfb_find(p, p->xfb_names[i], &reg, &regs, &comps, &t, &size) != 0) {
            snprintf(p->log, sizeof p->log, "transform feedback varying '%s' is not written by the vertex shader\n", p->xfb_names[i]);
            return -1;
        }
        int buf = p->xfb_mode == GL_SEPARATE_ATTRIBS ? i : 0;
        for (int r = 0; r < regs; r++) {
            if (so->n >= 64) { snprintf(p->log, sizeof p->log, "too many transform feedback outputs\n"); return -1; }
            so->out[so->n++] = VIRGL_OBJ_SHADER_SO_OUTPUT_REGISTER_INDEX((uint32_t)(reg + r)) | VIRGL_OBJ_SHADER_SO_OUTPUT_START_COMPONENT(0) |
                               VIRGL_OBJ_SHADER_SO_OUTPUT_NUM_COMPONENTS((uint32_t)comps) | VIRGL_OBJ_SHADER_SO_OUTPUT_BUFFER((uint32_t)buf) |
                               VIRGL_OBJ_SHADER_SO_OUTPUT_DST_OFFSET(off[buf]);
            off[buf] += (uint32_t)comps;
        }
    }
    for (int b = 0; b < 4; b++) so->stride[b] = off[b];
    if (p->xfb_mode == GL_INTERLEAVED_ATTRIBS && off[0] > 64) { snprintf(p->log, sizeof p->log, "too many interleaved transform feedback components\n"); return -1; }
    return 0;
}

void glGetTransformFeedbackVarying(GLuint name, GLuint index, GLsizei bufsize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *out)
{
    CTX();
    struct zgl_program *p = program_get(name);
    if (!p) { zgl_set_error(GL_INVALID_VALUE); return; }
    if (!p->linked || index >= (GLuint)p->nxfb) { zgl_set_error(GL_INVALID_VALUE); return; }
    int reg, regs, comps, sz; enum zgl_type t;
    if (xfb_find(p, p->xfb_names[index], &reg, &regs, &comps, &t, &sz) != 0) { zgl_set_error(GL_INVALID_OPERATION); return; }
    if (size) *size = sz;
    if (type) *type = gl_type_of(t);
    if (bufsize > 0) { snprintf(out, (size_t)bufsize, "%s", p->xfb_names[index]); if (length) *length = (GLsizei)strlen(out); }
    else if (length) *length = 0;
}

/* ---- draws ------------------------------------------------------------------------------------- */

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    CTX();
    if (first < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_draw_args a = { .mode = mode, .first = first, .count = count, .instances = 1 };
    zgl_draw_ex(&a);
}
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    CTX();
    if (first < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_draw_args a = { .mode = mode, .first = first, .count = count, .instances = instances };
    zgl_draw_ex(&a);
}
static void draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances, GLint base, int range, GLuint start, GLuint end)
{
    if (range && end < start) { zgl_set_error(GL_INVALID_VALUE); return; }
    struct zgl_draw_args a = { .mode = mode, .count = count, .index_type = type ? type : GL_UNSIGNED_INT, .indices = indices, .instances = instances,
                               .base_vertex = base, .range_known = range, .range_start = start, .range_end = end };
    if (!type) { zgl_set_error(GL_INVALID_ENUM); return; }
    zgl_draw_ex(&a);
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) { CTX(); draw_elements(mode, count, type, indices, 1, 0, 0, 0, 0); }
void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei n) { CTX(); draw_elements(mode, count, type, indices, n, 0, 0, 0, 0); }
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) { CTX(); draw_elements(mode, count, type, indices, 1, 0, 1, start, end); }
void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base) { CTX(); draw_elements(mode, count, type, indices, 1, base, 0, 0, 0); }
void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint base) { CTX(); draw_elements(mode, count, type, indices, 1, base, 1, start, end); }
void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei n, GLint base) { CTX(); draw_elements(mode, count, type, indices, n, base, 0, 0, 0); }
void glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) glDrawArrays(mode, first[i], count[i]);
}
void glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices, GLsizei n)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) draw_elements(mode, count[i], type, indices[i], 1, 0, 0, 0, 0);
}
void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices, GLsizei n, const GLint *base)
{
    CTX();
    if (n < 0) { zgl_set_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) draw_elements(mode, count[i], type, indices[i], 1, base[i], 0, 0, 0);
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
