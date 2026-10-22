/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* glslrun: zgl's shader tests (zgl/tests/glsl, *.test) on sic, through the
 * real thing: the GL calls a program makes, libzgl, the kernel's virtio-gpu
 * driver and the host's virglrenderer. The host-side runner
 * (tests/vrend/glsltest) checks the compiler against virglrenderer alone;
 * this checks everything between an application and the screen.
 *
 *   glslrun [-v] file.test...
 *
 * The target is an 8x8 RGBA8 surface, so expected values are clamped to
 * [0, 1] and compared to within 2.5/255. The gl_* state uniforms of the
 * fixed-function tests are set the way a program sets them: glMaterial,
 * glLight, glLightModel (the matrices are the identity already). */
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "../lib/gl/zgl.h"

#define W 8
#define H 8

static int verbose;

static const char *default_vs =
    "#version 120\n"
    "attribute vec2 zgl_pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = zgl_pos * 0.5 + 0.5; gl_Position = vec4(zgl_pos, 0.0, 1.0); }\n";

struct test {
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
    char *src = slurp(path);
    if (!src) return -1;
    enum { NONE, VS, FS, UNI, TEX, EXP, ATT } sec = NONE;
    int unit = 0;
    const char *start = NULL;
    char *p = src;
    while (*p) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (p[0] == '[') {
            if (sec == VS) t->vs = dupn(start, (size_t)(p - start));
            if (sec == FS) t->fs = dupn(start, (size_t)(p - start));
            sec = !strncmp(p, "[vertex]", 8) ? VS : !strncmp(p, "[fragment]", 10) ? FS : !strncmp(p, "[uniforms]", 10) ? UNI :
                  !strncmp(p, "[attributes]", 12) ? ATT : !strncmp(p, "[texture", 8) ? TEX : !strncmp(p, "[expect]", 8) ? EXP : NONE;
            if (sec == TEX) unit = atoi(p + 8);
            start = eol ? eol + 1 : p + len;
        } else if (sec == UNI || sec == TEX || sec == EXP || sec == ATT) {
            char line[256];
            snprintf(line, sizeof line, "%.*s", (int)len, p);
            char *h = strchr(line, '#');
            if (h) *h = 0;
            char *s = line;
            while (isspace((unsigned char)*s)) s++;
            if (*s) {
                if (sec == UNI && t->nuniforms < 64) snprintf(t->uniforms[t->nuniforms++], 256, "%s", s);
                if (sec == ATT && t->nattrs < 16) snprintf(t->attrs[t->nattrs++], 128, "%s", s);
                if (sec == TEX && unit >= 0 && unit < 8) { snprintf(t->textures[unit], 128, "%s", s); t->has_tex[unit] = 1; }
                if (sec == EXP && t->nexpect < 64) snprintf(t->expect[t->nexpect++], 256, "%s", s);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    if (sec == VS) t->vs = dupn(start, strlen(start));
    if (sec == FS) t->fs = dupn(start, strlen(start));
    free(src);
    return t->fs ? 0 : -1;
}

static int numbers(const char *s, float *v, int max)
{
    int n = 0;
    while (*s && n < max) { char *end; float f = strtof(s, &end); if (end == s) { s++; continue; } v[n++] = f; s = end; }
    return n;
}

/* the fixed-function state behind a gl_* uniform line; 1 if it was one */
static int set_state(const char *name, const float *v, int n)
{
    int a, b;
    static const GLenum mat[5] = { GL_EMISSION, GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_SHININESS };
    static const GLenum light[11] = { GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_POSITION, GL_SPOT_DIRECTION, GL_SPOT_EXPONENT,
                                      GL_SPOT_CUTOFF, 0, GL_CONSTANT_ATTENUATION, GL_LINEAR_ATTENUATION, GL_QUADRATIC_ATTENUATION };
    if (sscanf(name, "gl_FrontMaterial[%d]", &a) == 1 && a >= 0 && a < 5) {
        if (a == 4) glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, v[0]); else glMaterialfv(GL_FRONT_AND_BACK, mat[a], v);
        return 1;
    }
    if (sscanf(name, "gl_LightSource[%d][%d]", &a, &b) == 2 && b >= 0 && b < 11 && light[b]) {
        if (b >= 5) glLightf(GL_LIGHT0 + a, light[b], v[0]); else glLightfv(GL_LIGHT0 + a, light[b], v);
        return 1;
    }
    if (!strcmp(name, "gl_LightModelAmbient")) { glLightModelfv(GL_LIGHT_MODEL_AMBIENT, v); return 1; }
    (void)n;
    return !strncmp(name, "gl_", 3);          /* the matrices: identity, as the state already is */
}

static void set_uniform(GLuint prog, const char *line)
{
    char name[96];
    int k = 0;
    const char *p = line;
    while (*p && !isspace((unsigned char)*p) && k < 95) name[k++] = *p++;
    name[k] = 0;
    float v[64];
    int n = numbers(p, v, 64);
    if (set_state(name, v, n)) return;
    /* find it: the exact name, else name[i] */
    struct zgl_program *zp = zgl->program;
    int idx = -1, elem = 0;
    for (int i = 0; i < zp->nuniforms; i++) if (!strcmp(zp->uniforms[i].name, name)) idx = i;
    if (idx < 0) {
        char base[96];
        snprintf(base, sizeof base, "%s", name);
        char *br = strrchr(base, '[');
        if (br) { elem = atoi(br + 1); *br = 0; }
        for (int i = 0; i < zp->nuniforms; i++) if (!strcmp(zp->uniforms[i].name, base)) idx = i;
    }
    if (idx < 0) return;                              /* optimized away or unused */
    (void)prog;
    GLint loc = idx | (elem << 16);
    enum zgl_type t = zp->uniforms[idx].type;
    int rows = zgl_type_vec(t), cols = zgl_type_cols(t);
    /* integers straight from the text (a float would round 4294967295 to 2^32) */
    GLint iv[64];
    {
        const char *q = p; int k2 = 0;
        while (*q && k2 < 64) { char *end; long long x = strtoll(q, &end, 10); if (end == q) { q++; continue; } iv[k2++] = (GLint)(uint32_t)x; q = end; if (*q == '.') { while (*q && !isspace((unsigned char)*q)) q++; } }
    }
    if (zgl_type_is_sampler(t)) { glUniform1iv(loc, n, iv); return; }
    if (cols) {
        int per = cols * rows;
        if (cols == 2 && rows == 2) glUniformMatrix2fv(loc, n / per, GL_FALSE, v);
        else if (cols == 3 && rows == 3) glUniformMatrix3fv(loc, n / per, GL_FALSE, v);
        else if (cols == 4 && rows == 4) glUniformMatrix4fv(loc, n / per, GL_FALSE, v);
        return;
    }
    int count = n / rows;
    enum zgl_type b = zgl_type_base(t);
    if (b == T_FLOAT) {
        if (rows == 1) glUniform1fv(loc, count, v); else if (rows == 2) glUniform2fv(loc, count, v);
        else if (rows == 3) glUniform3fv(loc, count, v); else glUniform4fv(loc, count, v);
    } else {
        for (int e = 0; e < count; e++) {
            GLint *q = iv + e * rows;
            GLint el = idx | ((elem + e) << 16);
            if (rows == 1) glUniform1i(el, q[0]); else if (rows == 2) glUniform2i(el, q[0], q[1]);
            else if (rows == 3) glUniform3i(el, q[0], q[1], q[2]); else glUniform4i(el, q[0], q[1], q[2], q[3]);
        }
    }
}

static GLuint make_texture(const char *spec)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    int w = 2, h = 2;
    unsigned char *px;
    if (!strncmp(spec, "grid", 4)) {
        sscanf(spec + 4, "%d %d", &w, &h);
        px = malloc((size_t)w * h * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            unsigned char *q = &px[(y * w + x) * 4];
            q[0] = (unsigned char)((x + 0.5f) / w * 255 + 0.5f); q[1] = (unsigned char)((y + 0.5f) / h * 255 + 0.5f); q[2] = 64; q[3] = 255;
        }
    } else if (!strncmp(spec, "solid", 5)) {
        float c[4] = { 0, 0, 0, 1 };
        sscanf(spec + 5, "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
        w = h = 1;
        px = malloc(4);
        for (int i = 0; i < 4; i++) px[i] = (unsigned char)(c[i] * 255 + 0.5f);
    } else {
        static const unsigned char q[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
        px = malloc(16);
        memcpy(px, q, 16);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    return t;
}

static GLuint shader(GLenum type, const char *src, char *log, size_t logsz)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(s, (GLsizei)logsz, NULL, log); return 0; }
    return s;
}

static int run(sic_gl_context *ctx, const struct test *t, char *why, size_t whysz)
{
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    char log[1024] = "";
    const char *expect_fail = NULL;
    for (int i = 0; i < t->nexpect; i++) if (!strncmp(t->expect[i], "fail", 4)) expect_fail = t->expect[i] + 5;
    GLuint vs = shader(GL_VERTEX_SHADER, t->vs ? t->vs : default_vs, log, sizeof log);
    GLuint fs = vs ? shader(GL_FRAGMENT_SHADER, t->fs, log, sizeof log) : 0;
    GLuint prog = 0;
    GLint linked = 0;
    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) glGetProgramInfoLog(prog, sizeof log, NULL, log);
    }
    if (expect_fail) {
        if (linked) { snprintf(why, whysz, "compiled, expected an error with \"%s\"", expect_fail); return -1; }
        if (!strstr(log, expect_fail)) { snprintf(why, whysz, "wrong error: %s", log); return -1; }
        return 0;
    }
    if (!linked) { snprintf(why, whysz, "%s", log); return -1; }
    glUseProgram(prog);
    for (int i = 0; i < t->nuniforms; i++) set_uniform(prog, t->uniforms[i]);
    GLuint texs[8] = { 0 };
    for (int u = 0; u < 8; u++) if (t->has_tex[u]) { glActiveTexture(GL_TEXTURE0 + u); texs[u] = make_texture(t->textures[u]); }
    glActiveTexture(GL_TEXTURE0);
    /* attributes: the quad to `zgl_pos` (or the one marked `quad`, or the first), constants to the rest */
    const char *quad_name = "zgl_pos";
    for (int k = 0; k < t->nattrs; k++) if (strstr(t->attrs[k], " quad")) { static char qn[64]; sscanf(t->attrs[k], "%63s", qn); quad_name = qn; }
    struct zgl_program *zp = zgl->program;
    for (int i = 0; i < zp->vs->c.nattribs; i++) {
        const struct zgl_var *a = &zp->vs->c.attribs[i];
        GLint loc = zp->attrib_loc[i];
        if (!strcmp(a->name, quad_name) || (i == 0 && !strcmp(quad_name, "zgl_pos") && glGetAttribLocation(prog, "zgl_pos") < 0)) {
            glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 0, quad);
            glEnableVertexAttribArray((GLuint)loc);
            continue;
        }
        float v[16] = { 0, 0, 0, 1 };
        int nv = 0;
        for (int k = 0; k < t->nattrs; k++) {
            char nm[64];
            if (sscanf(t->attrs[k], "%63s", nm) == 1 && !strcmp(nm, a->name)) nv = numbers(t->attrs[k] + strlen(nm), v, 16);
        }
        int cols = zgl_type_cols(a->type), rows = zgl_type_vec(a->type);
        if (cols) {
            for (int c = 0; c < cols; c++) {
                float col[4] = { 0, 0, 0, 1 };
                for (int r = 0; r < rows && c * rows + r < nv; r++) col[r] = v[c * rows + r];
                glVertexAttrib4f((GLuint)(loc + c), col[0], col[1], col[2], col[3]);
            }
        } else glVertexAttrib4f((GLuint)loc, v[0], v[1], v[2], v[3]);
        glDisableVertexAttribArray((GLuint)loc);
    }
    glViewport(0, 0, W, H);
    glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    int stride;
    const uint32_t *px = sic_gl_swap(ctx, &stride);
    GLenum err = glGetError();
    int fail = 0;
    if (err) { snprintf(why, whysz, "GL error 0x%x", err); fail = 1; }
    if (!px && !fail) { snprintf(why, whysz, "no frame (the host failed the draw)"); fail = 1; }
    for (int i = 0; i < t->nexpect && !fail; i++) {
        float e[4]; int x0 = 0, y0 = 0, x1 = W, y1 = H;
        if (!strncmp(t->expect[i], "all", 3)) sscanf(t->expect[i] + 3, "%f %f %f %f", &e[0], &e[1], &e[2], &e[3]);
        else if (!strncmp(t->expect[i], "pixel", 5)) { sscanf(t->expect[i] + 5, "%d %d %f %f %f %f", &x0, &y0, &e[0], &e[1], &e[2], &e[3]); x1 = x0 + 1; y1 = y0 + 1; }
        else continue;
        for (int y = y0; y < y1 && !fail; y++)
            for (int x = x0; x < x1 && !fail; x++) {
                uint32_t q = px[(H - 1 - y) * stride + x];       /* the frame is top-down */
                float got[4] = { ((q >> 16) & 255) / 255.0f, ((q >> 8) & 255) / 255.0f, (q & 255) / 255.0f, (q >> 24) / 255.0f };
                for (int k = 0; k < 4; k++) {
                    float want = e[k] < 0 ? 0 : e[k] > 1 ? 1 : e[k];
                    if (fabsf(got[k] - want) > 2.5f / 255) {
                        snprintf(why, whysz, "pixel %d,%d = %.3f %.3f %.3f %.3f, expected %.3f %.3f %.3f %.3f",
                                 x, y, got[0], got[1], got[2], got[3], e[0], e[1], e[2], e[3]);
                        fail = 1;
                        break;
                    }
                }
            }
    }
    if (fail && verbose) fprintf(stderr, "--- VS TGSI\n%s--- FS TGSI\n%s", zp->vs->c.tgsi, zp->fs->c.tgsi);
    for (int u = 0; u < 8; u++) if (texs[u]) glDeleteTextures(1, &texs[u]);
    glUseProgram(0);
    glDeleteProgram(prog); glDeleteShader(vs); glDeleteShader(fs);
    return fail ? -1 : 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-v")) { verbose = 1; first = 2; }
    sic_gl_context *ctx = sic_gl_create(W, H);
    if (!ctx) { fprintf(stderr, "glslrun: no GPU context (is this a virgl VM?)\n"); return 1; }
    int failed = 0, total = 0;
    for (int i = first; i < argc; i++) {
        struct test t;
        char why[512] = "";
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        total++;
        if (parse_test(argv[i], &t) != 0) { printf("FAIL %s: unreadable\n", base); failed++; continue; }
        if (run(ctx, &t, why, sizeof why) == 0) printf("PASS %s\n", base);
        else { printf("FAIL %s: %s\n", base, why); failed++; }
        fflush(stdout);
        free(t.vs); free(t.fs);
    }
    printf("%d/%d passed\n", total - failed, total);
    return failed;
}
