/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The fixed-function pipeline of OpenGL 1.x: matrix stacks, glBegin/glEnd,
 * lighting, materials, texturing. All of it is a GLSL program generated for
 * the state in use (lighting on/off, texture on/off, colour material) and
 * compiled like any other, with the gl_* state uniforms filled in per draw. */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- matrices (column-major, like GL) ------------------------------------------ */

static void mat_identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

static void mat_mul(float *r, const float *a, const float *b)       /* r = a * b */
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            t[c * 4 + rr] = a[rr] * b[c * 4] + a[4 + rr] * b[c * 4 + 1] + a[8 + rr] * b[c * 4 + 2] + a[12 + rr] * b[c * 4 + 3];
    memcpy(r, t, 64);
}

static float *cur_matrix(void)
{
    switch (zgl->matrix_mode) {
    case GL_PROJECTION: return zgl->proj[zgl->proj_top];
    case GL_TEXTURE: return zgl->texm[zgl->tex_top];
    default: return zgl->mv[zgl->mv_top];
    }
}

void glMatrixMode(GLenum mode) { if (zgl) zgl->matrix_mode = mode; }
void glLoadIdentity(void) { if (zgl) mat_identity(cur_matrix()); }
void glLoadMatrixf(const GLfloat *m) { if (zgl) memcpy(cur_matrix(), m, 64); }
void glLoadMatrixd(const GLdouble *m) { if (!zgl) return; float *c = cur_matrix(); for (int i = 0; i < 16; i++) c[i] = (float)m[i]; }
void glMultMatrixf(const GLfloat *m) { if (!zgl) return; float *c = cur_matrix(); mat_mul(c, c, m); }
void glMultMatrixd(const GLdouble *m) { float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)m[i]; glMultMatrixf(f); }

void glPushMatrix(void)
{
    if (!zgl) return;
    switch (zgl->matrix_mode) {
    case GL_PROJECTION: if (zgl->proj_top + 1 >= ZGL_PROJ_STACK) { zgl_set_error(GL_STACK_OVERFLOW); return; } memcpy(zgl->proj[zgl->proj_top + 1], zgl->proj[zgl->proj_top], 64); zgl->proj_top++; break;
    case GL_TEXTURE: if (zgl->tex_top + 1 >= ZGL_TEX_STACK) { zgl_set_error(GL_STACK_OVERFLOW); return; } memcpy(zgl->texm[zgl->tex_top + 1], zgl->texm[zgl->tex_top], 64); zgl->tex_top++; break;
    default: if (zgl->mv_top + 1 >= ZGL_MV_STACK) { zgl_set_error(GL_STACK_OVERFLOW); return; } memcpy(zgl->mv[zgl->mv_top + 1], zgl->mv[zgl->mv_top], 64); zgl->mv_top++; break;
    }
}

void glPopMatrix(void)
{
    if (!zgl) return;
    switch (zgl->matrix_mode) {
    case GL_PROJECTION: if (zgl->proj_top == 0) { zgl_set_error(GL_STACK_UNDERFLOW); return; } zgl->proj_top--; break;
    case GL_TEXTURE: if (zgl->tex_top == 0) { zgl_set_error(GL_STACK_UNDERFLOW); return; } zgl->tex_top--; break;
    default: if (zgl->mv_top == 0) { zgl_set_error(GL_STACK_UNDERFLOW); return; } zgl->mv_top--; break;
    }
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16]; mat_identity(m); m[12] = x; m[13] = y; m[14] = z;
    glMultMatrixf(m);
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16]; mat_identity(m); m[0] = x; m[5] = y; m[10] = z;
    glMultMatrixf(m);
}
void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }

void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
    float len = sqrtf(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len; y /= len; z /= len;
    float r = a * 3.14159265f / 180.0f, c = cosf(r), s = sinf(r), t = 1 - c;
    float m[16] = {
        t * x * x + c,     t * x * y + s * z, t * x * z - s * y, 0,
        t * x * y - s * z, t * y * y + c,     t * y * z + s * x, 0,
        t * x * z + s * y, t * y * z - s * x, t * z * z + c,     0,
        0, 0, 0, 1,
    };
    glMultMatrixf(m);
}
void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    glMultMatrixf(m);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n)); m[15] = 1;
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    glMultMatrixf(m);
}

/* ---- lights, material ------------------------------------------------------------- */

static void transform_point(const float *m, const float *v, float *out)
{
    for (int i = 0; i < 4; i++) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *p)
{
    if (!zgl) return;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + ZGL_MAX_LIGHTS) { zgl_set_error(GL_INVALID_ENUM); return; }
    struct zgl_light *l = &zgl->lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(l->ambient, p, 16); break;
    case GL_DIFFUSE: memcpy(l->diffuse, p, 16); break;
    case GL_SPECULAR: memcpy(l->specular, p, 16); break;
    case GL_POSITION: transform_point(zgl->mv[zgl->mv_top], p, l->position); break;      /* stored in eye space, as GL does */
    case GL_SPOT_DIRECTION: {
        float d[4] = { p[0], p[1], p[2], 0 }, o[4];
        transform_point(zgl->mv[zgl->mv_top], d, o);
        memcpy(l->spot_dir, o, 16);
        break;
    }
    case GL_SPOT_EXPONENT: l->spot_exp = p[0]; break;
    case GL_SPOT_CUTOFF: l->spot_cutoff = p[0]; break;
    case GL_CONSTANT_ATTENUATION: l->att[0] = p[0]; break;
    case GL_LINEAR_ATTENUATION: l->att[1] = p[0]; break;
    case GL_QUADRATIC_ATTENUATION: l->att[2] = p[0]; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}
void glLightf(GLenum light, GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 0 }; glLightfv(light, pname, p); }

void glLightModelfv(GLenum pname, const GLfloat *p)
{
    if (!zgl) return;
    if (pname == GL_LIGHT_MODEL_AMBIENT) memcpy(zgl->lm_ambient, p, 16);
    else if (pname == GL_LIGHT_MODEL_TWO_SIDE) zgl->lm_two_side = p[0] != 0;
}
void glLightModeli(GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 0 }; glLightModelfv(pname, p); }

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p)
{
    if (!zgl) return;
    (void)face;
    switch (pname) {
    case GL_AMBIENT: memcpy(zgl->mat_ambient, p, 16); break;
    case GL_DIFFUSE: memcpy(zgl->mat_diffuse, p, 16); break;
    case GL_AMBIENT_AND_DIFFUSE: memcpy(zgl->mat_ambient, p, 16); memcpy(zgl->mat_diffuse, p, 16); break;
    case GL_SPECULAR: memcpy(zgl->mat_specular, p, 16); break;
    case GL_EMISSION: memcpy(zgl->mat_emission, p, 16); break;
    case GL_SHININESS: zgl->mat_shininess = p[0]; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}
void glMaterialf(GLenum face, GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 0 }; glMaterialfv(face, pname, p); }
void glColorMaterial(GLenum face, GLenum mode) { if (!zgl) return; (void)face; zgl->color_material_mode = mode; }
void glTexEnvi(GLenum target, GLenum pname, GLint param) { if (!zgl) return; if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) zgl->unit_env[zgl->active_unit] = (GLenum)param; }
void glTexEnvf(GLenum target, GLenum pname, GLfloat param) { glTexEnvi(target, pname, (GLint)param); }
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) { if (pname == GL_TEXTURE_ENV_COLOR) return; glTexEnvi(target, pname, (GLint)params[0]); }
void glClientActiveTexture(GLenum unit) { if (!zgl) return; zgl->client_unit = (int)(unit - GL_TEXTURE0); }

/* ---- immediate mode ---------------------------------------------------------------- */

#define IMM_STRIDE 12           /* floats per vertex: xyzw rgba nx ny nz s t -> 4+4+3+2 = 13, rounded to 13 */
#undef IMM_STRIDE
#define IMM_STRIDE 13

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { if (!zgl) return; zgl->cur_color[0] = r; zgl->cur_color[1] = g; zgl->cur_color[2] = b; zgl->cur_color[3] = a; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1); }
void glColor3fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], 1); }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { if (!zgl) return; zgl->cur_normal[0] = x; zgl->cur_normal[1] = y; zgl->cur_normal[2] = z; }
void glNormal3fv(const GLfloat *v) { glNormal3f(v[0], v[1], v[2]); }
void glTexCoord2f(GLfloat s, GLfloat t) { if (!zgl) return; zgl->cur_texcoord[0] = s; zgl->cur_texcoord[1] = t; zgl->cur_texcoord[2] = 0; zgl->cur_texcoord[3] = 1; }
void glTexCoord2fv(const GLfloat *v) { glTexCoord2f(v[0], v[1]); }

void glBegin(GLenum mode)
{
    if (!zgl) return;
    if (zgl->in_begin) { zgl_set_error(GL_INVALID_OPERATION); return; }
    zgl->in_begin = 1;
    zgl->imm_mode = mode;
    zgl->imm_n = 0;
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (!zgl || !zgl->in_begin) return;
    if (zgl->imm_n + 1 > zgl->imm_cap) {
        int cap = zgl->imm_cap ? zgl->imm_cap * 2 : 1024;
        zgl->imm = realloc(zgl->imm, (size_t)cap * IMM_STRIDE * sizeof(float));
        zgl->imm_cap = cap;
    }
    float *v = zgl->imm + (size_t)zgl->imm_n * IMM_STRIDE;
    v[0] = x; v[1] = y; v[2] = z; v[3] = w;
    memcpy(v + 4, zgl->cur_color, 16);
    memcpy(v + 8, zgl->cur_normal, 12);
    v[11] = zgl->cur_texcoord[0]; v[12] = zgl->cur_texcoord[1];
    zgl->imm_n++;
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { glVertex4f(x, y, z, 1); }
void glVertex2f(GLfloat x, GLfloat y) { glVertex4f(x, y, 0, 1); }
void glVertex2i(GLint x, GLint y) { glVertex4f((float)x, (float)y, 0, 1); }
void glVertex3fv(const GLfloat *v) { glVertex4f(v[0], v[1], v[2], 1); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { glVertex4f((float)x, (float)y, (float)z, 1); }

void glEnd(void)
{
    if (!zgl || !zgl->in_begin) return;
    zgl->in_begin = 0;
    if (zgl->imm_n == 0) return;
    /* the collected vertices become the fixed-function attribute arrays for one draw */
    struct zgl_attrib saved[4];
    int locs[4] = { ZGL_ATTR_VERTEX, ZGL_ATTR_COLOR, ZGL_ATTR_NORMAL, ZGL_ATTR_TEXCOORD };
    int sizes[4] = { 4, 4, 3, 2 };
    int offs[4] = { 0, 4, 8, 11 };
    for (int i = 0; i < 4; i++) {
        saved[i] = zgl->attribs[locs[i]];
        struct zgl_attrib *a = &zgl->attribs[locs[i]];
        a->enabled = 1; a->size = sizes[i]; a->type = GL_FLOAT; a->normalized = 0;
        a->stride = IMM_STRIDE * 4; a->pointer = zgl->imm + offs[i]; a->buffer = NULL;
    }
    zgl->imm_draw = 1;
    zgl_draw(zgl->imm_mode, 0, zgl->imm_n, 0, NULL);
    zgl->imm_draw = 0;
    for (int i = 0; i < 4; i++) zgl->attribs[locs[i]] = saved[i];
}

/* ---- client arrays (compat) ------------------------------------------------------ */

void glEnableClientState(GLenum cap)
{
    if (!zgl) return;
    switch (cap) {
    case GL_VERTEX_ARRAY: zgl->cl_vertex.enabled = 1; break;
    case GL_NORMAL_ARRAY: zgl->cl_normal.enabled = 1; break;
    case GL_COLOR_ARRAY: zgl->cl_color.enabled = 1; break;
    case GL_TEXTURE_COORD_ARRAY: zgl->cl_texcoord.enabled = 1; break;
    default: zgl_set_error(GL_INVALID_ENUM);
    }
}
void glDisableClientState(GLenum cap)
{
    if (!zgl) return;
    switch (cap) {
    case GL_VERTEX_ARRAY: zgl->cl_vertex.enabled = 0; break;
    case GL_NORMAL_ARRAY: zgl->cl_normal.enabled = 0; break;
    case GL_COLOR_ARRAY: zgl->cl_color.enabled = 0; break;
    case GL_TEXTURE_COORD_ARRAY: zgl->cl_texcoord.enabled = 0; break;
    default: break;
    }
}
#define CL_SET(field, sz, ty, st, ptr) do { if (!zgl) return; zgl->field.size = (sz); zgl->field.type = (ty); zgl->field.stride = (st); zgl->field.p = (ptr); zgl->field.buffer = zgl->array_buffer; } while (0)
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { CL_SET(cl_vertex, size, type, stride, p); }
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) { CL_SET(cl_normal, 3, type, stride, p); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { CL_SET(cl_color, size, type, stride, p); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { if (zgl && zgl->client_unit) return; CL_SET(cl_texcoord, size, type, stride, p); }

/* Before a fixed-function draw: the client arrays feed the conventional
 * attribute locations (a user program with generic attributes leaves them). */
void zgl_ffp_bind_client_arrays(int enable)
{
    static struct zgl_attrib saved[4];
    int locs[4] = { ZGL_ATTR_VERTEX, ZGL_ATTR_NORMAL, ZGL_ATTR_COLOR, ZGL_ATTR_TEXCOORD };
    void *arrs[4] = { &zgl->cl_vertex, &zgl->cl_normal, &zgl->cl_color, &zgl->cl_texcoord };
    if (!enable) { for (int i = 0; i < 4; i++) zgl->attribs[locs[i]] = saved[i]; return; }
    for (int i = 0; i < 4; i++) {
        struct { int enabled; int size; GLenum type; int stride; const void *p; struct zgl_buffer *buffer; } *cl = arrs[i];
        saved[i] = zgl->attribs[locs[i]];
        struct zgl_attrib *a = &zgl->attribs[locs[i]];
        if (!cl->enabled) {
            a->enabled = 0;
            if (i == 1) memcpy(a->value, zgl->cur_normal, 12);
            else if (i == 2) memcpy(a->value, zgl->cur_color, 16);
            else if (i == 3) memcpy(a->value, zgl->cur_texcoord, 16);
            continue;
        }
        a->enabled = 1; a->size = cl->size; a->type = cl->type; a->stride = cl->stride; a->pointer = cl->p; a->buffer = cl->buffer;
        a->normalized = cl->type == GL_UNSIGNED_BYTE || cl->type == GL_UNSIGNED_SHORT;
    }
}

/* ---- the fixed-function program ---------------------------------------------------- */

static const char *ffp_vertex_source(int key)
{
    static char src[4096];
    int lighting = key & 1, texture = key & 2, color_material = key & 4, lights = (key >> 4) & 0xFF;
    char *p = src;
    p += sprintf(p, "varying vec4 vcol;\nvarying vec2 uv;\n");
    p += sprintf(p, "void main() {\n  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n");
    if (texture) p += sprintf(p, "  uv = (gl_TextureMatrix[0] * gl_MultiTexCoord0).st;\n"); else p += sprintf(p, "  uv = vec2(0.0);\n");
    if (!lighting) {
        p += sprintf(p, "  vcol = gl_Color;\n}\n");
        return src;
    }
    p += sprintf(p, "  vec3 n = normalize(gl_NormalMatrix * gl_Normal);\n  vec4 eye = gl_ModelViewMatrix * gl_Vertex;\n");
    p += sprintf(p, "  vec4 mamb = %s;\n  vec4 mdif = %s;\n", color_material ? "gl_Color" : "gl_FrontMaterial.ambient", color_material ? "gl_Color" : "gl_FrontMaterial.diffuse");
    p += sprintf(p, "  vec4 acc = gl_FrontMaterial.emission + gl_LightModel.ambient * mamb;\n");
    for (int i = 0; i < ZGL_MAX_LIGHTS; i++) {
        if (!(lights & (1 << i))) continue;
        p += sprintf(p,
            "  {\n"
            "    vec4 lp = gl_LightSource[%d].position;\n"
            "    vec3 L; float att = 1.0;\n"
            "    if (lp.w == 0.0) L = normalize(lp.xyz); else {\n"
            "      vec3 d = lp.xyz - eye.xyz; float dist = length(d); L = d / dist;\n"
            "      att = 1.0 / (gl_LightSource[%d].constantAttenuation + gl_LightSource[%d].linearAttenuation * dist + gl_LightSource[%d].quadraticAttenuation * dist * dist);\n"
            "    }\n"
            "    float nl = max(dot(n, L), 0.0);\n"
            "    vec4 c = gl_LightSource[%d].ambient * mamb + nl * gl_LightSource[%d].diffuse * mdif;\n"
            "    if (nl > 0.0) {\n"
            "      vec3 h = normalize(L + vec3(0.0, 0.0, 1.0));\n"
            "      c += pow(max(dot(n, h), 0.0), gl_FrontMaterial.shininess) * gl_LightSource[%d].specular * gl_FrontMaterial.specular;\n"
            "    }\n"
            "    acc += att * c;\n"
            "  }\n", i, i, i, i, i, i, i);
    }
    p += sprintf(p, "  vcol = vec4(clamp(acc.rgb, 0.0, 1.0), mdif.a);\n}\n");
    return src;
}

static const char *ffp_fragment_source(int key)
{
    static char src[1024];
    int texture = key & 2, env = (key >> 12) & 0xF;
    char *p = src;
    p += sprintf(p, "varying vec4 vcol;\nvarying vec2 uv;\n");
    if (texture) p += sprintf(p, "uniform sampler2D tex0;\n");
    p += sprintf(p, "void main() {\n");
    if (!texture) p += sprintf(p, "  gl_FragColor = vcol;\n");
    else if (env == 1) p += sprintf(p, "  gl_FragColor = texture2D(tex0, uv);\n");                                        /* REPLACE */
    else if (env == 2) p += sprintf(p, "  vec4 t = texture2D(tex0, uv);\n  gl_FragColor = vec4(mix(vcol.rgb, t.rgb, t.a), vcol.a);\n");   /* DECAL */
    else p += sprintf(p, "  gl_FragColor = vcol * texture2D(tex0, uv);\n");                                                /* MODULATE */
    p += sprintf(p, "}\n");
    return src;
}

struct ffp_cache { int key; struct zgl_program *prog; };
static struct ffp_cache ffp_cache[32];
static int nffp;

extern int zgl_link(struct zgl_program *p);

struct zgl_program *zgl_ffp_program(void)
{
    int lights = 0;
    for (int i = 0; i < ZGL_MAX_LIGHTS; i++) if (zgl->lights[i].enabled) lights |= 1 << i;
    int texture = zgl->texture_2d[0] && zgl->unit_tex[0] && zgl->unit_tex[0]->res;
    int env = zgl->unit_env[0] == GL_REPLACE ? 1 : zgl->unit_env[0] == GL_DECAL ? 2 : 0;
    int key = (zgl->lighting ? 1 : 0) | (texture ? 2 : 0) | (zgl->lighting && zgl->color_material ? 4 : 0) | (zgl->lighting ? lights << 4 : 0) | env << 12;
    for (int i = 0; i < nffp; i++) if (ffp_cache[i].key == key) return ffp_cache[i].prog;
    /* build it through the public API so it is an ordinary program */
    struct zgl_program *saved = zgl->program;
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
    const char *vsrc = ffp_vertex_source(key), *fsrc = ffp_fragment_source(key);
    glShaderSource(vs, 1, &vsrc, NULL);
    glShaderSource(fs, 1, &fsrc, NULL);
    glAttachShader(pr, vs); glAttachShader(pr, fs);
    glLinkProgram(pr);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    struct zgl_program *p = NULL;
    for (int i = 0; i < zgl->nprograms; i++) if (zgl->programs[i]->name == pr) p = zgl->programs[i];
    if (!ok || !p) {
        char log[512]; glGetProgramInfoLog(pr, sizeof log, NULL, log);
        fprintf(stderr, "zgl: fixed-function program failed: %s\n", log);
        zgl->program = saved;
        return NULL;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    zgl->program = saved;
    if (nffp < 32) { ffp_cache[nffp].key = key; ffp_cache[nffp].prog = p; nffp++; }
    return p;
}

/* ---- the gl_* state uniforms ------------------------------------------------------ */

static int invert3(const float *m, float *out)      /* m: 4x4 column-major; out: its 3x3 inverse transposed, as mat3 columns */
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-12f) { out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 0; out[4] = 1; out[5] = 0; out[6] = 0; out[7] = 0; out[8] = 1; return -1; }
    float id = 1.0f / det;
    /* inverse (row-major r[row][col]) */
    float r00 = (e * i - f * h) * id, r01 = (c * h - b * i) * id, r02 = (b * f - c * e) * id;
    float r10 = (f * g - d * i) * id, r11 = (a * i - c * g) * id, r12 = (c * d - a * f) * id;
    float r20 = (d * h - e * g) * id, r21 = (b * g - a * h) * id, r22 = (a * e - b * d) * id;
    /* transpose: the normal matrix N = (M^-1)^T; column-major columns of N are the rows of M^-1 */
    out[0] = r00; out[1] = r01; out[2] = r02;
    out[3] = r10; out[4] = r11; out[5] = r12;
    out[6] = r20; out[7] = r21; out[8] = r22;
    return 0;
}

static void put_slots(struct zgl_program *p, const struct zgl_uniform *u, const float *data, int slots)
{
    if (u->vs_slot >= 0) memcpy(&p->vs_consts[u->vs_slot * 4], data, (size_t)slots * 16);
    if (u->fs_slot >= 0) memcpy(&p->fs_consts[u->fs_slot * 4], data, (size_t)slots * 16);
}

void zgl_ffp_fill_state(struct zgl_program *p)
{
    float mvp[16], nm[16], buf[16 * 4];
    int any = 0;
    for (int i = 0; i < p->nuniforms; i++) {
        struct zgl_uniform *u = &p->uniforms[i];
        if (!u->builtin) continue;
        any = 1;
        switch (u->builtin) {
        case ZGL_STATE_MVP: mat_mul(mvp, zgl->proj[zgl->proj_top], zgl->mv[zgl->mv_top]); put_slots(p, u, mvp, 4); break;
        case ZGL_STATE_MV: put_slots(p, u, zgl->mv[zgl->mv_top], 4); break;
        case ZGL_STATE_PROJ: put_slots(p, u, zgl->proj[zgl->proj_top], 4); break;
        case ZGL_STATE_NORMAL: {
            float n3[9];
            invert3(zgl->mv[zgl->mv_top], n3);
            memset(nm, 0, sizeof nm);
            for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) nm[c * 4 + r] = n3[c * 3 + r];
            put_slots(p, u, nm, 3);
            break;
        }
        case ZGL_STATE_MV_INV_T: {
            float n3[9];
            invert3(zgl->mv[zgl->mv_top], n3);
            memset(nm, 0, sizeof nm);
            for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) nm[c * 4 + r] = n3[c * 3 + r];
            nm[15] = 1;
            put_slots(p, u, nm, 4);
            break;
        }
        case ZGL_STATE_TEXTURE0_MATRIX: put_slots(p, u, zgl->texm[zgl->tex_top], 4); break;
        case ZGL_STATE_LIGHTMODEL_AMBIENT: put_slots(p, u, zgl->lm_ambient, 1); break;
        case ZGL_STATE_MATERIAL_FRONT:
            memset(buf, 0, sizeof buf);
            memcpy(buf, zgl->mat_emission, 16); memcpy(buf + 4, zgl->mat_ambient, 16); memcpy(buf + 8, zgl->mat_diffuse, 16);
            memcpy(buf + 12, zgl->mat_specular, 16); buf[16] = zgl->mat_shininess;
            put_slots(p, u, buf, 5);
            break;
        default:
            if (u->builtin >= ZGL_STATE_LIGHT0 && u->builtin < ZGL_STATE_LIGHT0 + ZGL_MAX_LIGHTS) {
                struct zgl_light *l = &zgl->lights[u->builtin - ZGL_STATE_LIGHT0];
                memset(buf, 0, sizeof buf);
                memcpy(buf, l->ambient, 16); memcpy(buf + 4, l->diffuse, 16); memcpy(buf + 8, l->specular, 16); memcpy(buf + 12, l->position, 16);
                memcpy(buf + 16, l->spot_dir, 12);
                buf[20] = l->spot_exp; buf[24] = l->spot_cutoff; buf[28] = cosf(l->spot_cutoff * 3.14159265f / 180.0f);
                buf[32] = l->att[0]; buf[36] = l->att[1]; buf[40] = l->att[2];
                put_slots(p, u, buf, 11);
            }
        }
    }
    if (any) p->consts_dirty = 1;
}

void zgl_ffp_init(struct zgl_ctx *c)
{
    c->matrix_mode = GL_MODELVIEW;
    mat_identity(c->mv[0]); mat_identity(c->proj[0]); mat_identity(c->texm[0]);
    c->cur_color[0] = c->cur_color[1] = c->cur_color[2] = c->cur_color[3] = 1;
    c->cur_normal[2] = 1;
    c->cur_texcoord[3] = 1;
    for (int i = 0; i < ZGL_MAX_LIGHTS; i++) {
        struct zgl_light *l = &c->lights[i];
        l->ambient[3] = 1;
        if (i == 0) { l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = 1; l->specular[0] = l->specular[1] = l->specular[2] = 1; }
        l->diffuse[3] = l->specular[3] = 1;
        l->position[2] = 1; l->position[3] = 0;
        l->spot_dir[2] = -1; l->spot_cutoff = 180; l->att[0] = 1;
    }
    c->mat_ambient[0] = c->mat_ambient[1] = c->mat_ambient[2] = 0.2f; c->mat_ambient[3] = 1;
    c->mat_diffuse[0] = c->mat_diffuse[1] = c->mat_diffuse[2] = 0.8f; c->mat_diffuse[3] = 1;
    c->mat_specular[3] = 1; c->mat_emission[3] = 1;
    c->lm_ambient[0] = c->lm_ambient[1] = c->lm_ambient[2] = 0.2f; c->lm_ambient[3] = 1;
    c->color_material_mode = GL_AMBIENT_AND_DIFFUSE;
    for (int i = 0; i < ZGL_MAX_UNITS; i++) c->unit_env[i] = GL_MODULATE;
}
