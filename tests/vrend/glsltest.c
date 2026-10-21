/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* glsltest: runs zgl's GLSL compiler end to end on the host.
 *
 * For each .test file: zgl compiles the vertex and fragment shader to TGSI,
 * virglrenderer's translator (vtrans.c) turns the TGSI into the GLSL a virgl
 * host would run, the host's OpenGL compiles, links and draws it over an 8x8
 * float render target, and the pixels are compared with the expected ones.
 * That is the path a sic program's shader takes, minus the guest kernel.
 *
 * A test file:
 *
 *   # anything after # is a comment
 *   [vertex]            optional; the default passes `uv` (0..1 over the target)
 *   ...GLSL...
 *   [fragment]
 *   ...GLSL...
 *   [uniforms]          name value...   (name[2], s.field, arr[1].field work)
 *   [attributes]        name x y z w    a constant attribute; `name quad` feeds it
 *                       the quad corners instead (default: zgl_pos, else the first)
 *   [texture N]         2x2 | grid W H | solid r g b a; bound to unit N
 *   [expect]
 *   all r g b a         every pixel (tolerance 1/512)
 *   pixel x y r g b a   one pixel
 *   fail text           compiling must fail with `text` in the log
 *
 * Output: one line per test, PASS / FAIL with the reason; with -v the TGSI
 * and host GLSL of failures. Exit status: the number of failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include "zgl.h"

#ifdef __APPLE__
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#endif

int vtrans_pair(const char *vs_tgsi, const char *fs_tgsi, int glsl_version, char **vs_glsl, char **fs_glsl, char *err, size_t errsz);

#define W 8
#define H 8

static int verbose;

static const char *default_vs =
    "#version 120\n"
    "attribute vec2 zgl_pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = zgl_pos * 0.5 + 0.5; gl_Position = vec4(zgl_pos, 0.0, 1.0); }\n";

struct test {
    char name[256];
    char *vs, *fs;
    char uniforms[64][256]; int nuniforms;
    char attrs[16][128]; int nattrs;
    char textures[8][128]; int has_tex[8];
    char expect[64][256]; int nexpect;
};

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    s[fread(s, 1, (size_t)n, f)] = 0;
    fclose(f);
    return s;
}

static char *dupn(const char *s, size_t n) { char *r = malloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }

static int parse_test(const char *path, struct test *t)
{
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", path);
    char *src = slurp(path);
    if (!src) return -1;
    enum { NONE, VS, FS, UNI, TEX, EXP, ATT } sec = NONE;
    int tex_unit = 0;
    const char *sec_start = NULL;
    char *p = src;
    while (*p) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (p[0] == '[') {
            if (sec == VS) t->vs = dupn(sec_start, (size_t)(p - sec_start));
            if (sec == FS) t->fs = dupn(sec_start, (size_t)(p - sec_start));
            if (!strncmp(p, "[vertex]", 8)) sec = VS;
            else if (!strncmp(p, "[fragment]", 10)) sec = FS;
            else if (!strncmp(p, "[uniforms]", 10)) sec = UNI;
            else if (!strncmp(p, "[attributes]", 12)) sec = ATT;
            else if (!strncmp(p, "[texture", 8)) { sec = TEX; tex_unit = atoi(p + 8); }
            else if (!strncmp(p, "[expect]", 8)) sec = EXP;
            else sec = NONE;
            sec_start = eol ? eol + 1 : p + len;
        } else if (sec == UNI || sec == TEX || sec == EXP || sec == ATT) {
            char line[256];
            snprintf(line, sizeof line, "%.*s", (int)len, p);
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            char *s = line;
            while (isspace((unsigned char)*s)) s++;
            if (*s) {
                if (sec == UNI && t->nuniforms < 64) snprintf(t->uniforms[t->nuniforms++], 256, "%s", s);
                if (sec == ATT && t->nattrs < 16) snprintf(t->attrs[t->nattrs++], 128, "%s", s);
                if (sec == TEX && tex_unit >= 0 && tex_unit < 8) { snprintf(t->textures[tex_unit], 128, "%s", s); t->has_tex[tex_unit] = 1; }
                if (sec == EXP && t->nexpect < 64) snprintf(t->expect[t->nexpect++], 256, "%s", s);
            }
        }
        if (!eol) { p += len; break; }
        p = eol + 1;
    }
    if (sec == VS) t->vs = dupn(sec_start, strlen(sec_start));
    if (sec == FS) t->fs = dupn(sec_start, strlen(sec_start));
    free(src);
    return t->fs ? 0 : -1;
}

/* ---- the host GL context ---------------------------------------------------- */

static int gl_init(void)
{
#ifdef __APPLE__
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated, (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pf; GLint npf;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf) return -1;
    CGLContextObj ctx;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError) return -1;
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(ctx);
    return 0;
#else
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) return -1;
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (c == EGL_NO_CONTEXT) return -1;
    return eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c) ? 0 : -1;
#endif
}

/* vrend asks for the lowest GLSL version and the extensions on top; macOS's
 * core profile offers no extensions at 1.40, but everything vrend asks for
 * here is core in 3.30: say 330 and drop those #extension lines */
static void host_adapt(char *s)
{
    char *v = strstr(s, "#version ");
    if (v && (v[9] == '1' || (v[9] == '3' && v[10] < '3'))) memcpy(v + 9, "330", 3);
    static const char *const core[] = {
        "GL_ARB_fragment_coord_conventions", "GL_ARB_explicit_attrib_location", "GL_ARB_shader_bit_encoding",
        "GL_ARB_uniform_buffer_object", "GL_ARB_texture_rectangle", "GL_ARB_draw_instanced", "GL_ARB_texture_multisample",
        "GL_ARB_sample_shading", NULL
    };
    for (int i = 0; core[i]; i++) {
        char *e;
        while ((e = strstr(s, core[i]))) {
            char *ls = e; while (ls > s && ls[-1] != '\n') ls--;
            char *le = strchr(e, '\n'); le = le ? le + 1 : e + strlen(e);
            memset(ls, ' ', (size_t)(le - ls - 1));
        }
    }
}

static GLuint gl_shader(GLenum type, const char *src, char *log, size_t logsz)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(s, (GLsizei)logsz, NULL, log); glDeleteShader(s); return 0; }
    return s;
}

/* ---- uniforms: test values -> the vec4 constant slots, as zgl stores them ---- */

static int is_int_type(enum zgl_type t) { return zgl_type_base(t) == T_INT || zgl_type_base(t) == T_UINT; }

/* `name` = "u", "u[2]", "s.f", "a[1].f": find the interface variable and the element */
static const struct zgl_var *find_uniform(const struct zgl_compiled *c, const char *name, int *elem)
{
    char base[96];
    snprintf(base, sizeof base, "%s", name);
    *elem = 0;
    for (int i = 0; i < c->nuniforms; i++) if (!strcmp(c->uniforms[i].name, base)) return &c->uniforms[i];
    /* a trailing [n] that is not part of a struct path selects an element */
    size_t n = strlen(base);
    if (n && base[n - 1] == ']') {
        char *br = strrchr(base, '[');
        if (br && !strchr(br, '.')) { *elem = atoi(br + 1); *br = 0; }
    }
    for (int i = 0; i < c->nuniforms; i++) if (!strcmp(c->uniforms[i].name, base)) return &c->uniforms[i];
    for (int i = 0; i < c->nsamplers; i++) if (!strcmp(c->samplers[i].name, base)) return &c->samplers[i];
    return NULL;
}

static void set_uniform(const struct zgl_compiled *c, uint32_t *consts, const char *line, int *sampler_unit)
{
    char name[96];
    const char *p = line;
    int k = 0;
    while (*p && !isspace((unsigned char)*p) && k < 95) name[k++] = *p++;
    name[k] = 0;
    int elem;
    const struct zgl_var *v = find_uniform(c, name, &elem);
    if (!v) return;                                     /* not used by this stage */
    double vals[16]; int nv = 0;
    while (*p && nv < 16) { char *end; double d = strtod(p, &end); if (end == p) { p++; continue; } vals[nv++] = d; p = end; }
    if (zgl_type_is_sampler(v->type)) { if (nv) sampler_unit[v->slot] = (int)vals[0]; return; }
    int cols = zgl_type_cols(v->type), rows = zgl_type_vec(v->type);
    int per_elem = (cols ? cols : 1);
    int slot = v->slot + elem * per_elem;
    /* a list longer than one element fills the next elements of an array */
    int elems = v->array ? v->array - elem : 1;
    for (int col = 0, i = 0; col < per_elem * elems; col++)
        for (int r = 0; r < rows && i < nv; r++, i++) {
            uint32_t bits;
            if (zgl_type_base(v->type) == T_BOOL) bits = vals[i] != 0 ? 0xFFFFFFFFu : 0;
            else if (is_int_type(v->type)) bits = (uint32_t)(int64_t)vals[i];
            else { float f = (float)vals[i]; memcpy(&bits, &f, 4); }
            consts[(slot + col) * 4 + r] = bits;
        }
}

/* ---- textures ----------------------------------------------------------------- */

static GLuint make_texture(const char *spec)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    int w = 2, h = 2;
    float *px;
    if (!strncmp(spec, "grid", 4)) {
        sscanf(spec + 4, "%d %d", &w, &h);
        px = malloc((size_t)w * h * 16);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            float *q = &px[(y * w + x) * 4];
            q[0] = (x + 0.5f) / w; q[1] = (y + 0.5f) / h; q[2] = 0.25f; q[3] = 1;
        }
    } else if (!strncmp(spec, "solid", 5)) {
        float c[4] = { 0, 0, 0, 1 };
        sscanf(spec + 5, "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
        w = h = 1;
        px = malloc(16);
        memcpy(px, c, 16);
    } else {                                            /* 2x2: red green / blue white */
        static const float q[16] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1 };
        px = malloc(64);
        memcpy(px, q, 64);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    return t;
}

/* ---- one test --------------------------------------------------------------------- */

static int run(const struct test *t, char *why, size_t whysz)
{
    struct zgl_compiled vs, fs;
    char log[2048] = "";
    const char *expect_fail = NULL;
    for (int i = 0; i < t->nexpect; i++) if (!strncmp(t->expect[i], "fail", 4)) expect_fail = t->expect[i] + 5;

    int rc = zgl_compile(t->vs ? t->vs : default_vs, GL_VERTEX_SHADER, &vs, log, sizeof log);
    if (rc == 0) rc = zgl_compile_linked(t->fs, GL_FRAGMENT_SHADER, &vs, &fs, log, sizeof log);
    if (expect_fail) {
        if (rc == 0) { snprintf(why, whysz, "compiled, expected an error containing \"%s\"", expect_fail); return -1; }
        if (!strstr(log, expect_fail)) { snprintf(why, whysz, "wrong error: %s", log); return -1; }
        return 0;
    }
    if (rc != 0) { snprintf(why, whysz, "zgl: %s", log); return -1; }

    char err[256];
    char *gvs, *gfs;
    if (vtrans_pair(vs.tgsi, fs.tgsi, 410, &gvs, &gfs, err, sizeof err) != 0) {
        snprintf(why, whysz, "virgl rejected the TGSI: %s", err);
        if (verbose) fprintf(stderr, "--- VS TGSI\n%s--- FS TGSI\n%s", vs.tgsi, fs.tgsi);
        return -1;
    }
#ifdef __APPLE__
    if (getenv("ZGL_HOST_ADAPT")) { host_adapt(gvs); host_adapt(gfs); }
#endif
    GLuint s1 = gl_shader(GL_VERTEX_SHADER, gvs, log, sizeof log);
    GLuint s2 = s1 ? gl_shader(GL_FRAGMENT_SHADER, gfs, log, sizeof log) : 0;
    if (!s1 || !s2) {
        snprintf(why, whysz, "host GLSL: %.300s", log);
        if (verbose) fprintf(stderr, "--- %s TGSI\n%s--- host GLSL\n%s", s1 ? "FS" : "VS", s1 ? fs.tgsi : vs.tgsi, s1 ? gfs : gvs);
        return -1;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, s1); glAttachShader(prog, s2);
    for (int i = 0; i < 8; i++) { char n[16]; snprintf(n, sizeof n, "fsout_c%d", i); glBindFragDataLocation(prog, (GLuint)i, n); }
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(prog, sizeof log, NULL, log); snprintf(why, whysz, "host link: %.300s", log); return -1; }
    glUseProgram(prog);

    /* constants */
    static uint32_t vc[ZGL_MAX_CONSTS * 4], fc[ZGL_MAX_CONSTS * 4];
    memset(vc, 0, sizeof vc); memset(fc, 0, sizeof fc);
    int vs_units[ZGL_MAX_VARS], fs_units[ZGL_MAX_VARS];
    for (int i = 0; i < ZGL_MAX_VARS; i++) vs_units[i] = fs_units[i] = i;
    for (int i = 0; i < t->nuniforms; i++) { set_uniform(&vs, vc, t->uniforms[i], vs_units); set_uniform(&fs, fc, t->uniforms[i], fs_units); }
    GLint l;
    if (vs.nconsts && (l = glGetUniformLocation(prog, "vsconst0")) >= 0) glUniform4uiv(l, vs.nconsts, vc);
    if (fs.nconsts && (l = glGetUniformLocation(prog, "fsconst0")) >= 0) glUniform4uiv(l, fs.nconsts, fc);
    GLuint texs[8] = { 0 };
    for (int u = 0; u < 8; u++) if (t->has_tex[u]) { glActiveTexture(GL_TEXTURE0 + u); texs[u] = make_texture(t->textures[u]); }
    for (int i = 0; i < fs.nsamplers; i++) {
        char n[32];
        for (int k = 0; k < (fs.samplers[i].array ? fs.samplers[i].array : 1); k++) {
            snprintf(n, sizeof n, "fssamp%d", fs.samplers[i].slot + k);
            if ((l = glGetUniformLocation(prog, n)) >= 0) glUniform1i(l, fs_units[fs.samplers[i].slot + k]);
        }
    }
    /* vrend's own uniform block: winsys_adjust_y flips y for window targets */
    GLuint ubo = 0;
    GLuint bi = glGetUniformBlockIndex(prog, "VirglBlock");
    if (bi != GL_INVALID_INDEX) {
        GLint size = 0; glGetActiveUniformBlockiv(prog, bi, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        uint8_t *blk = calloc(1, (size_t)size);
        const char *nm = "winsys_adjust_y"; GLuint ui; GLint off = -1;
        glGetUniformIndices(prog, 1, &nm, &ui);
        if (ui != GL_INVALID_INDEX) glGetActiveUniformsiv(prog, 1, &ui, GL_UNIFORM_OFFSET, &off);
        if (off >= 0) { float one = 1.0f; memcpy(blk + off, &one, 4); }
        glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, size, blk, GL_STATIC_DRAW);
        glUniformBlockBinding(prog, bi, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        free(blk);
    }

    /* the target and the quad (on a unit of its own, not over a test's textures) */
    glActiveTexture(GL_TEXTURE0 + 15);
    GLuint fbo, rt;
    glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, W, H, 0, GL_RGBA, GL_FLOAT, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, W, H);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    const char *quad_attr = NULL;
    for (int k = 0; k < t->nattrs; k++) if (strstr(t->attrs[k], " quad")) { static char qn[64]; sscanf(t->attrs[k], "%63s", qn); quad_attr = qn; }
    for (int i = 0; i < vs.nattribs; i++) {
        GLuint loc = (GLuint)vs.attribs[i].slot;
        int is_quad = quad_attr ? !strcmp(vs.attribs[i].name, quad_attr) : (!strcmp(vs.attribs[i].name, "zgl_pos") || i == 0);
        if (is_quad) { glEnableVertexAttribArray(loc); glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0); continue; }
        float v[4] = { 0, 0, 0, 1 };
        for (int k = 0; k < t->nattrs; k++) {
            char nm[64];
            if (sscanf(t->attrs[k], "%63s %f %f %f %f", nm, &v[0], &v[1], &v[2], &v[3]) >= 2 && !strcmp(nm, vs.attribs[i].name)) break;
            v[0] = v[1] = v[2] = 0; v[3] = 1;
        }
        if (zgl_type_cols(vs.attribs[i].type)) {
            /* a matrix: its columns at consecutive locations, the values column by column */
            float all[16] = { 0 }; int na = 0;
            for (int k = 0; k < t->nattrs; k++) {
                char nm[64]; float f[16]; int got;
                const char *q = t->attrs[k];
                if (sscanf(q, "%63s", nm) != 1 || strcmp(nm, vs.attribs[i].name)) continue;
                q += strlen(nm);
                for (got = 0; got < 16; got++) { char *end; f[got] = strtof(q, &end); if (end == q) break; q = end; }
                memcpy(all, f, sizeof f); na = got;
            }
            int rows = zgl_type_vec(vs.attribs[i].type);
            for (int c = 0; c < zgl_type_cols(vs.attribs[i].type); c++) {
                float col[4] = { 0, 0, 0, 1 };
                for (int r = 0; r < rows && c * rows + r < na; r++) col[r] = all[c * rows + r];
                glVertexAttrib4f(loc + (GLuint)c, col[0], col[1], col[2], col[3]);
            }
            continue;
        }
        if (zgl_type_base(vs.attribs[i].type) == T_INT) glVertexAttribI4i(loc, (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
        else if (zgl_type_base(vs.attribs[i].type) == T_UINT) glVertexAttribI4ui(loc, (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3]);
        else glVertexAttrib4f(loc, v[0], v[1], v[2], v[3]);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    float px[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, px);
    GLenum gerr = glGetError();

    int fail = 0;
    if (gerr) { snprintf(why, whysz, "GL error 0x%x", gerr); fail = 1; }
    for (int i = 0; i < t->nexpect && !fail; i++) {
        float e[4]; int x0 = 0, y0 = 0, x1 = W, y1 = H;
        if (!strncmp(t->expect[i], "all", 3)) sscanf(t->expect[i] + 3, "%f %f %f %f", &e[0], &e[1], &e[2], &e[3]);
        else if (!strncmp(t->expect[i], "pixel", 5)) { sscanf(t->expect[i] + 5, "%d %d %f %f %f %f", &x0, &y0, &e[0], &e[1], &e[2], &e[3]); x1 = x0 + 1; y1 = y0 + 1; }
        else continue;
        for (int y = y0; y < y1 && !fail; y++)
            for (int x = x0; x < x1 && !fail; x++) {
                const float *q = &px[(y * W + x) * 4];
                for (int k = 0; k < 4; k++)
                    if (!(fabsf(q[k] - e[k]) <= 1.0f / 512)) {
                        snprintf(why, whysz, "pixel %d,%d = %g %g %g %g, expected %g %g %g %g", x, y, q[0], q[1], q[2], q[3], e[0], e[1], e[2], e[3]);
                        fail = 1;
                        break;
                    }
            }
    }
    if (fail && verbose) fprintf(stderr, "--- VS TGSI\n%s--- FS TGSI\n%s--- host FS GLSL\n%s", vs.tgsi, fs.tgsi, gfs);

    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &rt);
    glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    if (ubo) glDeleteBuffers(1, &ubo);
    for (int u = 0; u < 8; u++) if (texs[u]) glDeleteTextures(1, &texs[u]);
    glDeleteProgram(prog); glDeleteShader(s1); glDeleteShader(s2);
    free(gvs); free(gfs);
    zgl_compiled_free(&vs); zgl_compiled_free(&fs);
    return fail ? -1 : 0;
}

/* --link vs fs: compile and link only (zgl, virgl translation, the host) */
static int link_only(const char *vsp, const char *fsp)
{
    char *vsrc = slurp(vsp), *fsrc = slurp(fsp);
    if (!vsrc || !fsrc) { printf("FAIL: unreadable\n"); return 1; }
    struct zgl_compiled vs, fs;
    static char log[4096];
    if (zgl_compile(vsrc, GL_VERTEX_SHADER, &vs, log, sizeof log) != 0) { printf("FAIL zgl VS: %s", log); return 1; }
    if (zgl_compile_linked(fsrc, GL_FRAGMENT_SHADER, &vs, &fs, log, sizeof log) != 0) { printf("FAIL zgl FS: %s", log); return 1; }
    char err[256], *gvs, *gfs;
    if (vtrans_pair(vs.tgsi, fs.tgsi, 410, &gvs, &gfs, err, sizeof err) != 0) {
        printf("FAIL virgl: %s\n", err);
        if (verbose) fprintf(stderr, "--- VS TGSI\n%s--- FS TGSI\n%s", vs.tgsi, fs.tgsi);
        return 1;
    }
#ifdef __APPLE__
    if (getenv("ZGL_HOST_ADAPT")) { host_adapt(gvs); host_adapt(gfs); }
#endif
    GLuint s1 = gl_shader(GL_VERTEX_SHADER, gvs, log, sizeof log);
    GLuint s2 = s1 ? gl_shader(GL_FRAGMENT_SHADER, gfs, log, sizeof log) : 0;
    if (!s1 || !s2) {
        printf("FAIL host %s: %.400s\n", s1 ? "FS" : "VS", log);
        if (verbose) fprintf(stderr, "--- TGSI\n%s--- host GLSL\n%s", s1 ? fs.tgsi : vs.tgsi, s1 ? gfs : gvs);
        return 1;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, s1); glAttachShader(prog, s2);
    for (int i = 0; i < 8; i++) { char n[16]; snprintf(n, sizeof n, "fsout_c%d", i); glBindFragDataLocation(prog, (GLuint)i, n); }
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(prog, sizeof log, NULL, log); printf("FAIL host link: %.400s\n", log); return 1; }
    int vi = 0, fi = 0;
    for (const char *q = vs.tgsi; (q = strchr(q, '\n')); q++) vi++;
    for (const char *q = fs.tgsi; (q = strchr(q, '\n')); q++) fi++;
    printf("OK (TGSI %d + %d lines)\n", vi, fi);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc == 4 && !strcmp(argv[1], "--link")) { if (gl_init() != 0) return 1; return link_only(argv[2], argv[3]); }
    if (argc == 5 && !strcmp(argv[1], "-v") && !strcmp(argv[2], "--link")) { verbose = 1; if (gl_init() != 0) return 1; return link_only(argv[3], argv[4]); }
    if (argc > 1 && !strcmp(argv[1], "-v")) { verbose = 1; first = 2; }
    if (gl_init() != 0) { fprintf(stderr, "glsltest: no OpenGL 4.1 core context\n"); return 1; }
    int failed = 0, total = 0;
    for (int i = first; i < argc; i++) {
        struct test t;
        char why[512] = "";
        total++;
        if (parse_test(argv[i], &t) != 0) { printf("FAIL %s: unreadable test file\n", argv[i]); failed++; continue; }
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        if (run(&t, why, sizeof why) == 0) printf("PASS %s\n", base);
        else { printf("FAIL %s: %s\n", base, why); failed++; }
        free(t.vs); free(t.fs);
    }
    printf("%d/%d passed\n", total - failed, total);
    return failed;
}
