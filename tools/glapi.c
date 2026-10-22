/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* GL 3 API tests: each one draws (or copies, or reads) something through
 * the GL API and checks the pixels or the state it should give. Runs on
 * the desktop against virglrenderer (tests/vhost) and on sic itself.
 *
 *   glapi [name...]      the tests whose names contain one of the words (all without) */
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 32
#define H 32

static char why[512];

#define FAIL(...) do { snprintf(why, sizeof why, __VA_ARGS__); return 0; } while (0)
#define CHECK_GL(what) do { GLenum e_ = glGetError(); if (e_) FAIL("%s: GL error 0x%x", what, e_); } while (0)

/* ---- helpers ------------------------------------------------------------------------- */

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); }
    return s;
}

static GLuint program2(const char *vs, const char *fs, void (*before_link)(GLuint))
{
    GLuint p = glCreateProgram();
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    if (before_link) before_link(p);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "link: %s\n", log); glDeleteProgram(p); p = 0; }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
static GLuint program(const char *vs, const char *fs) { return program2(vs, fs, NULL); }

/* a full-target quad from a buffer in a VAO at location 0 */
static GLuint quad_vao, quad_vbo;
static void quad_setup(void)
{
    if (!quad_vao) {
        static const float v[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        glGenVertexArrays(1, &quad_vao);
        glBindVertexArray(quad_vao);
        glGenBuffers(1, &quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(0);
    }
}
static void quad(void)
{
    quad_setup();
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static const char *quad_vs =
    "#version 330\nlayout(location = 0) in vec2 pos;\nout vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static void rgba_at(int x, int y, uint8_t out[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out); }

static int near(const uint8_t *px, int r, int g, int b, int a)
{
    return abs(px[0] - r) <= 3 && abs(px[1] - g) <= 3 && abs(px[2] - b) <= 3 && (a < 0 || abs(px[3] - a) <= 3);
}

#define EXPECT_PX(x, y, r, g, b, a) do { uint8_t p_[4]; rgba_at(x, y, p_); \
    if (!near(p_, r, g, b, a)) FAIL("pixel (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d", x, y, p_[0], p_[1], p_[2], p_[3], r, g, b, a); } while (0)

static void reset(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    while (glGetError()) ;
}

/* ---- the tests ----------------------------------------------------------------------- */

static int t_version(void)
{
    GLint major = 0, minor = 0, n = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    CHECK_GL("gets");
    if (major != 3 || minor < 1) FAIL("version %d.%d", major, minor);
    int found = 0;
    for (int i = 0; i < n; i++) if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), "GL_ARB_instanced_arrays")) found = 1;
    if (!found) FAIL("no GL_ARB_instanced_arrays among %d extensions", n);
    if (strncmp((const char *)glGetString(GL_VERSION), "3.1", 3)) FAIL("GL_VERSION %s", glGetString(GL_VERSION));
    glGetStringi(GL_EXTENSIONS, (GLuint)n);
    if (glGetError() != GL_INVALID_VALUE) FAIL("glGetStringi past the end: no INVALID_VALUE");
    return 1;
}

static int t_vao_draw(void)
{
    GLuint p = program(quad_vs, "#version 330\nout vec4 c;\nvoid main() { c = vec4(1.0, 0.5, 0.25, 1.0); }\n");
    if (!p) FAIL("program");
    glUseProgram(p);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(5, 5, 255, 128, 64, 255);
    EXPECT_PX(30, 30, 255, 128, 64, 255);
    glDeleteProgram(p);
    return 1;
}

static int t_instancing(void)
{
    /* 4 instances, each a column: gl_InstanceID places it, a divisor-1 attribute colours it */
    GLuint p = program(
        "#version 330\nlayout(location = 0) in vec2 pos;\nlayout(location = 1) in vec4 col;\nout vec4 vc;\n"
        "void main() { vc = col; float x = (pos.x * 0.5 + 0.5) * 0.5 + float(gl_InstanceID) * 0.5 - 1.0; gl_Position = vec4(x, pos.y, 0.0, 1.0); }\n",
        "#version 330\nin vec4 vc;\nout vec4 c;\nvoid main() { c = vc; }\n");
    if (!p) FAIL("program");
    static const float cols[] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1 };
    static const float v[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vao, b[2];
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0); glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, b[1]); glBufferData(GL_ARRAY_BUFFER, sizeof cols, cols, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0); glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    glUseProgram(p);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 4);
    CHECK_GL("draw");
    EXPECT_PX(4, 16, 255, 0, 0, 255);
    EXPECT_PX(12, 16, 0, 255, 0, 255);
    EXPECT_PX(20, 16, 0, 0, 255, 255);
    EXPECT_PX(28, 16, 255, 255, 255, 255);
    glDeleteVertexArrays(1, &vao); glDeleteBuffers(2, b); glDeleteProgram(p);
    return 1;
}

static int t_int_attribs(void)
{
    GLuint p = program(
        "#version 330\nlayout(location = 0) in vec2 pos;\nlayout(location = 1) in ivec2 iv;\nlayout(location = 2) in uint uv;\nflat out ivec2 fi; flat out uint fu;\n"
        "void main() { fi = iv; fu = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n",
        "#version 330\nflat in ivec2 fi; flat in uint fu;\nout vec4 c;\n"
        "void main() { c = vec4(fi.x == -7 ? 1.0 : 0.0, fi.y == 100000 ? 1.0 : 0.0, fu == 4000000000u ? 1.0 : 0.0, 1.0); }\n");
    if (!p) FAIL("program");
    struct { float x, y; int32_t i[2]; uint32_t u; } v[4] = {
        { -1, -1, { -7, 100000 }, 4000000000u }, { 1, -1, { -7, 100000 }, 4000000000u },
        { -1, 1, { -7, 100000 }, 4000000000u }, { 1, 1, { -7, 100000 }, 4000000000u } };
    GLuint vao, b;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof v[0], 0); glEnableVertexAttribArray(0);
    glVertexAttribIPointer(1, 2, GL_INT, sizeof v[0], (void *)8); glEnableVertexAttribArray(1);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof v[0], (void *)16); glEnableVertexAttribArray(2);
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 255, 255, 255, 255);
    glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &b); glDeleteProgram(p);
    return 1;
}

static int t_const_attrib(void)
{
    /* an attribute with no array: its current value, int-typed through glVertexAttribI4i */
    GLuint p = program(
        "#version 330\nlayout(location = 0) in vec2 pos;\nlayout(location = 3) in vec4 k;\nlayout(location = 4) in ivec4 ki;\nout vec4 vc;\n"
        "void main() { vc = vec4(k.xy, ki.x == 9 ? 1.0 : 0.0, k.w); gl_Position = vec4(pos, 0.0, 1.0); }\n",
        "#version 330\nin vec4 vc;\nout vec4 c;\nvoid main() { c = vc; }\n");
    if (!p) FAIL("program");
    glUseProgram(p);
    glVertexAttrib4f(3, 0.25f, 0.75f, 0, 1);
    glVertexAttribI4i(4, 9, 0, 0, 0);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 64, 191, 255, 255);
    glDeleteProgram(p);
    return 1;
}

static int t_elements(void)
{
    /* an element buffer with a base vertex and primitive restart */
    GLuint p = program(quad_vs, "#version 330\nout vec4 c;\nvoid main() { c = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    if (!p) FAIL("program");
    /* vertices 0-3 are junk, 4-7 the left half, 8-11 the right half */
    static const float v[] = { 9, 9, 9, 9, 9, 9, 9, 9,
                               -1, -1, 0, -1, -1, 1, 0, 1,
                               0, -1, 1, -1, 0, 1, 1, 1 };
    static const uint16_t idx[] = { 0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7 };
    GLuint vao, b[2];
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(2, b);
    glBindBuffer(GL_ARRAY_BUFFER, b[0]); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0); glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b[1]); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glEnable(GL_PRIMITIVE_RESTART); glPrimitiveRestartIndex(0xFFFF);
    glUseProgram(p);
    glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, 9, GL_UNSIGNED_SHORT, 0, 4);
    glDisable(GL_PRIMITIVE_RESTART);
    CHECK_GL("draw");
    EXPECT_PX(4, 16, 0, 255, 0, 255);
    EXPECT_PX(28, 16, 0, 255, 0, 255);
    /* without restart the strip would join the halves through vertex 0xFFFF+4: still fine to check one region only */
    glDeleteVertexArrays(1, &vao); glDeleteBuffers(2, b); glDeleteProgram(p);
    return 1;
}

static const char *sample_fs =
    "#version 330\nuniform sampler2D t;\nin vec2 uv;\nout vec4 c;\nvoid main() { c = texture(t, uv); }\n";

static GLuint tex2d(GLenum internal, int w, int h, GLenum format, GLenum type, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static int t_formats(void)
{
    GLuint p = program(quad_vs, sample_fs);
    if (!p) FAIL("program");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t"), 0);
    struct { GLenum internal, format, type; const void *data; int r, g, b, a; const char *name; } cases[] = {
        { GL_R8, GL_RED, GL_UNSIGNED_BYTE, (const uint8_t[]){ 200 }, 200, 0, 0, 255, "R8" },
        { GL_RG8, GL_RG, GL_UNSIGNED_BYTE, (const uint8_t[]){ 10, 20 }, 10, 20, 0, 255, "RG8" },
        { GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, (const uint8_t[]){ 1, 2, 3 }, 1, 2, 3, 255, "RGB8" },
        { GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, (const uint8_t[]){ 30, 60, 90, 120 }, 90, 60, 30, 120, "RGBA8 from BGRA" },
        { GL_RGBA16F, GL_RGBA, GL_FLOAT, (const float[]){ 0.5f, 0.25f, 1.0f, 1.0f }, 128, 64, 255, 255, "RGBA16F" },
        { GL_RGBA32F, GL_RGBA, GL_FLOAT, (const float[]){ 0.1f, 0.2f, 0.3f, 0.4f }, 26, 51, 77, 102, "RGBA32F" },
        { GL_R16F, GL_RED, GL_HALF_FLOAT, (const uint16_t[]){ 0x3800 }, 128, 0, 0, 255, "R16F half" },
        { GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, (const uint32_t[]){ 1023u | (512u << 10) | (0u << 20) | (3u << 30) }, 255, 128, 0, 255, "RGB10_A2" },
        { GL_RGBA8, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, (const uint16_t[]){ 0xF84C }, 255, 136, 68, 204, "RGBA4 packed" },
        { GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, (const float[]){ 0.5f, 1.0f, 0.25f }, 128, 255, 64, 255, "R11G11B10F" },
        { GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, (const uint8_t[]){ 188, 188, 188, 255 }, 128, 128, 128, 255, "SRGB8_ALPHA8 decodes" },
        { GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, (const uint8_t[]){ 100, 50 }, 100, 100, 100, 50, "LUMINANCE_ALPHA" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        GLuint t = tex2d(cases[i].internal, 1, 1, cases[i].format, cases[i].type, cases[i].data);
        glClear(GL_COLOR_BUFFER_BIT);
        quad();
        GLenum e = glGetError();
        if (e) FAIL("%s: GL error 0x%x", cases[i].name, e);
        uint8_t px[4]; rgba_at(16, 16, px);
        if (!near(px, cases[i].r, cases[i].g, cases[i].b, cases[i].a))
            FAIL("%s: %d,%d,%d,%d, expected %d,%d,%d,%d", cases[i].name, px[0], px[1], px[2], px[3], cases[i].r, cases[i].g, cases[i].b, cases[i].a);
        glDeleteTextures(1, &t);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDeleteProgram(p);
    return 1;
}

static int t_int_texture(void)
{
    GLuint p = program(quad_vs, "#version 330\nuniform usampler2D t;\nuniform isampler2D s;\nout vec4 c;\n"
                                "void main() { uvec4 u = texelFetch(t, ivec2(0, 0), 0); ivec4 i = texelFetch(s, ivec2(1, 0), 0);\n"
                                "  c = vec4(u.r == 3000000000u ? 1.0 : 0.0, u.g == 7u ? 1.0 : 0.0, i.r == -5 ? 1.0 : 0.0, 1.0); }\n");
    if (!p) FAIL("program");
    static const uint32_t ud[] = { 3000000000u, 7, 0, 0 };
    static const int32_t sd[] = { 0, -5 };
    glActiveTexture(GL_TEXTURE0);
    GLuint t = tex2d(GL_RGBA32UI, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, ud);
    glActiveTexture(GL_TEXTURE1);
    GLuint s = tex2d(GL_R32I, 2, 1, GL_RED_INTEGER, GL_INT, sd);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t"), 0);
    glUniform1i(glGetUniformLocation(p, "s"), 1);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 255, 255, 255, 255);
    glActiveTexture(GL_TEXTURE0);
    glDeleteTextures(1, &t); glDeleteTextures(1, &s); glDeleteProgram(p);
    return 1;
}

static int t_texture_array(void)
{
    GLuint p = program(quad_vs, "#version 330\nuniform sampler2DArray t;\nuniform float layer;\nin vec2 uv;\nout vec4 c;\nvoid main() { c = texture(t, vec3(uv, layer)); }\n");
    if (!p) FAIL("program");
    uint8_t d[3][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 3);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 3, GL_RGBA, GL_UNSIGNED_BYTE, d);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    CHECK_GL("upload");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t"), 0);
    for (int l = 0; l < 3; l++) {
        glUniform1f(glGetUniformLocation(p, "layer"), (float)l);
        quad();
        EXPECT_PX(16, 16, d[l][0], d[l][1], d[l][2], 255);
    }
    glDeleteTextures(1, &t); glDeleteProgram(p);
    return 1;
}

static int t_texture_3d_cube(void)
{
    GLuint p = program(quad_vs, "#version 330\nuniform sampler3D t;\nuniform samplerCube cb;\nin vec2 uv;\nout vec4 c;\n"
                                "void main() { c = vec4(texture(t, vec3(0.5, 0.5, 0.75)).r, texture(cb, vec3(0.0, 0.0, -1.0)).g, texture(cb, vec3(1.0, 0.0, 0.0)).b, 1.0); }\n");
    if (!p) FAIL("program");
    uint8_t vol[2] = { 10, 250 };
    GLuint t3, tc;
    glGenTextures(1, &t3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, t3);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, 1, 1, 2, 0, GL_RED, GL_UNSIGNED_BYTE, vol);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &tc);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tc);
    for (int f = 0; f < 6; f++) {
        uint8_t px[4] = { 0, (uint8_t)(f == 5 ? 200 : 0), (uint8_t)(f == 0 ? 150 : 0), 255 };   /* -Z green, +X blue */
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    CHECK_GL("upload");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t"), 0);
    glUniform1i(glGetUniformLocation(p, "cb"), 1);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 250, 200, 150, 255);
    glActiveTexture(GL_TEXTURE0);
    glDeleteTextures(1, &t3); glDeleteTextures(1, &tc); glDeleteProgram(p);
    return 1;
}

static int t_mrt_fbo(void)
{
    /* two colour attachments and a depth renderbuffer; read each back */
    GLuint p = program(quad_vs, "#version 330\nlayout(location = 0) out vec4 a;\nlayout(location = 1) out vec4 b;\n"
                                "void main() { a = vec4(1.0, 0.0, 0.0, 1.0); b = vec4(0.0, 0.0, 1.0, 0.5); }\n");
    if (!p) FAIL("program");
    GLuint fbo, tex[2], rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(2, tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, tex[i], 0);
    }
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) FAIL("status 0x%x", st);
    GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, bufs);
    glViewport(0, 0, 16, 16);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(p);
    quad();
    CHECK_GL("draw");
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    EXPECT_PX(8, 8, 255, 0, 0, 255);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    EXPECT_PX(8, 8, 0, 0, 255, 128);
    CHECK_GL("read");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(2, tex); glDeleteRenderbuffers(1, &rb); glDeleteProgram(p);
    return 1;
}

static int t_blit_msaa(void)
{
    /* draw into a 4x multisampled renderbuffer, resolve into a texture, blit that to the window */
    GLuint p = program(quad_vs, "#version 330\nout vec4 c;\nvoid main() { c = vec4(0.0, 1.0, 1.0, 1.0); }\n");
    if (!p) FAIL("program");
    GLuint ms, rb, rs, t;
    glGenFramebuffers(1, &ms); glBindFramebuffer(GL_FRAMEBUFFER, ms);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) FAIL("ms fbo incomplete");
    glViewport(0, 0, 16, 16);
    glUseProgram(p);
    quad();
    glGenFramebuffers(1, &rs); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs);
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ms);
    glBlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    CHECK_GL("resolve");
    /* the resolved texture onto the window's right half, flipped upside down (the blit's reversed rectangle) */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 16, 16, 16, 32, 32, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    CHECK_GL("blit");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    EXPECT_PX(24, 16, 0, 255, 255, 255);
    EXPECT_PX(8, 16, 0, 0, 0, 0);
    glDeleteFramebuffers(1, &ms); glDeleteFramebuffers(1, &rs); glDeleteRenderbuffers(1, &rb); glDeleteTextures(1, &t); glDeleteProgram(p);
    return 1;
}

static int t_scissor_clear(void)
{
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, 20, 8, 6);
    glClearColor(1, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    CHECK_GL("clear");
    EXPECT_PX(6, 22, 255, 255, 0, 255);
    EXPECT_PX(6, 10, 0, 0, 255, 255);
    EXPECT_PX(20, 22, 0, 0, 255, 255);
    /* glClearBuffer on an FBO: integer colour, scissored too */
    GLuint fbo, t;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 8, 8, 0, GL_RGBA_INTEGER, GL_INT, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    GLint zero[4] = { 0, 0, 0, 0 }, val[4] = { -3, 70000, 5, -1 };
    glClearBufferiv(GL_COLOR, 0, zero);
    glEnable(GL_SCISSOR_TEST); glScissor(2, 2, 2, 2);
    glClearBufferiv(GL_COLOR, 0, val);
    glDisable(GL_SCISSOR_TEST);
    CHECK_GL("clear buffer");
    GLint got[4] = { 0 }, out[4] = { 9, 9, 9, 9 };
    glReadPixels(3, 3, 1, 1, GL_RGBA_INTEGER, GL_INT, got);
    glReadPixels(6, 6, 1, 1, GL_RGBA_INTEGER, GL_INT, out);
    CHECK_GL("read int");
    if (memcmp(got, val, sizeof val)) FAIL("integer clear: %d %d %d %d", got[0], got[1], got[2], got[3]);
    if (memcmp(out, zero, sizeof zero)) FAIL("outside the scissor: %d %d %d %d", out[0], out[1], out[2], out[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &t);
    return 1;
}

static int t_pbo(void)
{
    /* upload from a PIXEL_UNPACK buffer, read back into a PIXEL_PACK buffer, map it */
    uint8_t px[4 * 4];
    for (int i = 0; i < 16; i++) px[i] = (uint8_t)(i * 16 + 7);
    GLuint pb[2];
    glGenBuffers(2, pb);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pb[0]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, sizeof px + 16, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 16, sizeof px, px);
    GLuint t;
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, (void *)16);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    CHECK_GL("unpack");
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pb[1]);
    glBufferData(GL_PIXEL_PACK_BUFFER, sizeof px, NULL, GL_STREAM_READ);
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    const uint8_t *m = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sizeof px, GL_MAP_READ_BIT);
    CHECK_GL("pack");
    if (!m) FAIL("map");
    int bad = memcmp(m, px, sizeof px);
    uint8_t first[4]; memcpy(first, m, 4);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (bad) FAIL("read back %d,%d,%d,%d, expected %d,%d,%d,%d", first[0], first[1], first[2], first[3], px[0], px[1], px[2], px[3]);
    /* and glGetTexImage */
    uint8_t g[16];
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, g);
    CHECK_GL("get tex image");
    if (memcmp(g, px, sizeof px)) FAIL("glGetTexImage %d,%d,%d,%d", g[0], g[1], g[2], g[3]);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &t); glDeleteBuffers(2, pb);
    return 1;
}

static int t_buffers(void)
{
    GLuint b[2];
    glGenBuffers(2, b);
    glBindBuffer(GL_COPY_READ_BUFFER, b[0]);
    glBufferData(GL_COPY_READ_BUFFER, 64, NULL, GL_STATIC_DRAW);
    uint32_t *w = glMapBufferRange(GL_COPY_READ_BUFFER, 0, 64, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!w) FAIL("map write");
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)i * 0x01010101u;
    glUnmapBuffer(GL_COPY_READ_BUFFER);
    glBindBuffer(GL_COPY_WRITE_BUFFER, b[1]);
    glBufferData(GL_COPY_WRITE_BUFFER, 64, NULL, GL_STATIC_DRAW);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 16, 0, 32);
    uint32_t r[8];
    glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, 32, r);
    CHECK_GL("copy");
    for (int i = 0; i < 8; i++) if (r[i] != (uint32_t)(i + 4) * 0x01010101u) FAIL("copied word %d = %08x", i, r[i]);
    GLint64 size; glGetBufferParameteri64v(GL_COPY_WRITE_BUFFER, GL_BUFFER_SIZE, &size);
    if (size != 64) FAIL("size %lld", (long long)size);
    /* explicit flush */
    uint32_t *f = glMapBufferRange(GL_COPY_WRITE_BUFFER, 32, 32, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    f[0] = 0xDEADBEEF;
    glFlushMappedBufferRange(GL_COPY_WRITE_BUFFER, 0, 4);
    glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    glGetBufferSubData(GL_COPY_WRITE_BUFFER, 32, 4, r);
    if (r[0] != 0xDEADBEEF) FAIL("flushed word %08x", r[0]);
    CHECK_GL("flush");
    glDeleteBuffers(2, b);
    return 1;
}

static int t_ubo(void)
{
    /* std140 packing: scalars share registers, a vec3 then a float, a mat2, an array of structs */
    GLuint p = program(quad_vs,
        "#version 330\nstruct S { float f; vec2 v; };\n"
        "layout(std140) uniform Params { float a; float b; vec2 c; vec3 d; float e; mat2 m; S s[2]; ivec2 iv; } P;\n"
        "uniform Other { vec4 tint; };\n"
        "out vec4 o;\n"
        "void main() {\n"
        "  bool ok = P.a == 1.0 && P.b == 2.0 && P.c == vec2(3.0, 4.0) && P.d == vec3(5.0, 6.0, 7.0) && P.e == 8.0;\n"
        "  bool ok2 = P.m[0] == vec2(9.0, 10.0) && P.m[1] == vec2(11.0, 12.0) && P.s[1].f == 13.0 && P.s[1].v == vec2(14.0, 15.0) && P.iv == ivec2(-1, 77);\n"
        "  S cp = P.s[1];\n"
        "  bool ok3 = cp.v.y == 15.0;\n"
        "  o = vec4(ok ? 1.0 : 0.0, ok2 ? 1.0 : 0.0, ok3 ? 1.0 : 0.0, 1.0) * tint;\n"
        "}\n");
    if (!p) FAIL("program");
    GLuint bi = glGetUniformBlockIndex(p, "Params"), bo = glGetUniformBlockIndex(p, "Other");
    if (bi == GL_INVALID_INDEX || bo == GL_INVALID_INDEX) FAIL("block index %u %u", bi, bo);
    GLint size; glGetActiveUniformBlockiv(p, bi, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
    /* a@0 b@4 c@8 d@16 e@28 m@32(cols at 32, 48) s[0]@64 {f@64 v@72} s[1]@80 {f@80 v@88} iv@96 -> 112 */
    if (size != 112) FAIL("Params is %d bytes, expected 112", size);
    const char *names[] = { "Params.e", "Params.s[1].v", "Params.m" };
    GLuint idx[3]; GLint offs[3];
    glGetUniformIndices(p, 3, names, idx);
    if (idx[0] == GL_INVALID_INDEX || idx[1] == GL_INVALID_INDEX || idx[2] == GL_INVALID_INDEX) FAIL("uniform indices %u %u %u", idx[0], idx[1], idx[2]);
    glGetActiveUniformsiv(p, 3, idx, GL_UNIFORM_OFFSET, offs);
    if (offs[0] != 28 || offs[1] != 88 || offs[2] != 32) FAIL("offsets %d %d %d", offs[0], offs[1], offs[2]);
    float data[28] = { 0 };
    data[0] = 1; data[1] = 2; data[2] = 3; data[3] = 4; data[4] = 5; data[5] = 6; data[6] = 7; data[7] = 8;
    data[8] = 9; data[9] = 10; data[12] = 11; data[13] = 12;
    data[16] = 0; data[18] = 0; data[20] = 13; data[22] = 14; data[23] = 15;
    int32_t iv[2] = { -1, 77 }; memcpy(&data[24], iv, 8);
    float tint[4] = { 1, 1, 1, 1 };
    GLuint b[2];
    glGenBuffers(2, b);
    glBindBuffer(GL_UNIFORM_BUFFER, b[0]); glBufferData(GL_UNIFORM_BUFFER, 256 + sizeof data, NULL, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_UNIFORM_BUFFER, 256, sizeof data, data);       /* a range at an offset */
    glBindBuffer(GL_UNIFORM_BUFFER, b[1]); glBufferData(GL_UNIFORM_BUFFER, sizeof tint, tint, GL_DYNAMIC_DRAW);
    glUniformBlockBinding(p, bi, 3);
    glUniformBlockBinding(p, bo, 5);
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, b[0], 256, sizeof data);
    glBindBufferBase(GL_UNIFORM_BUFFER, 5, b[1]);
    glUseProgram(p);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 255, 255, 255, 255);
    glDeleteBuffers(2, b); glDeleteProgram(p);
    return 1;
}

static int t_texture_buffer(void)
{
    GLuint p = program(quad_vs, "#version 330\nuniform samplerBuffer tb;\nuniform usamplerBuffer ub;\nout vec4 c;\n"
                                "void main() { vec4 v = texelFetch(tb, 2); uint u = texelFetch(ub, 1).r; c = vec4(v.rg, u == 123456u ? 1.0 : 0.0, 1.0); }\n");
    if (!p) FAIL("program");
    float f[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0.25f, 0.75f, 0, 0 };
    uint32_t u[] = { 0, 123456 };
    GLuint b[2], t[2];
    glGenBuffers(2, b);
    glBindBuffer(GL_TEXTURE_BUFFER, b[0]); glBufferData(GL_TEXTURE_BUFFER, sizeof f, f, GL_STATIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, b[1]); glBufferData(GL_TEXTURE_BUFFER, sizeof u, u, GL_STATIC_DRAW);
    glGenTextures(2, t);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_BUFFER, t[0]); glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, b[0]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_BUFFER, t[1]); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, b[1]);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tb"), 0);
    glUniform1i(glGetUniformLocation(p, "ub"), 1);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 64, 191, 255, 255);
    glDeleteTextures(2, t); glDeleteBuffers(2, b); glDeleteProgram(p);
    return 1;
}

static int t_sync_query(void)
{
    GLuint p = program(quad_vs, "#version 330\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n");
    if (!p) FAIL("program");
    GLuint q[2];
    glGenQueries(2, q);
    glUseProgram(p);
    glBeginQuery(GL_SAMPLES_PASSED, q[0]);
    glBeginQuery(GL_TIME_ELAPSED, q[1]);
    quad();
    glEndQuery(GL_TIME_ELAPSED);
    glEndQuery(GL_SAMPLES_PASSED);
    GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLenum r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    if (r != GL_CONDITION_SATISFIED && r != GL_ALREADY_SIGNALED) FAIL("wait 0x%x", r);
    GLint status; glGetSynciv(s, GL_SYNC_STATUS, 1, NULL, &status);
    if (status != GL_SIGNALED) FAIL("sync status 0x%x", status);
    glDeleteSync(s);
    GLuint samples = 0; GLuint64 ns = 0;
    glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &samples);
    glGetQueryObjectui64v(q[1], GL_QUERY_RESULT, &ns);
    CHECK_GL("queries");
    if (samples < W * H / 2) FAIL("samples passed %u", samples);
    GLuint avail = 0; glGetQueryObjectuiv(q[1], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) FAIL("timer result not available");
    glDeleteQueries(2, q); glDeleteProgram(p);
    return 1;
}

static void xfb_setup(GLuint prog)
{
    const char *names[] = { "outv", "gl_Position" };
    glTransformFeedbackVaryings(prog, 2, names, GL_INTERLEAVED_ATTRIBS);
}

static int t_transform_feedback(void)
{
    GLuint p = program2("#version 330\nlayout(location = 0) in vec2 pos;\nout vec3 outv;\n"
                        "void main() { outv = vec3(pos * 2.0, float(gl_VertexID)); gl_Position = vec4(pos, 0.0, 1.0); }\n",
                        "#version 330\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n", xfb_setup);
    if (!p) FAIL("program");
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, b);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 4 * 7 * 4, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
    GLuint q; glGenQueries(1, &q);
    glUseProgram(p);
    quad_setup();
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
    glBeginTransformFeedback(GL_POINTS);
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_POINTS, 0, 4);
    glEndTransformFeedback();
    glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    glDisable(GL_RASTERIZER_DISCARD);
    CHECK_GL("capture");
    float out[28];
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof out, out);
    GLuint prims = 0; glGetQueryObjectuiv(q, GL_QUERY_RESULT, &prims);
    CHECK_GL("read");
    if (prims != 4) FAIL("%u primitives written", prims);
    /* vertex 3 is (1, 1): outv = (2, 2, 3), then gl_Position (1, 1, 0, 1) */
    const float *v3 = out + 3 * 7;
    if (v3[0] != 2 || v3[1] != 2 || v3[2] != 3 || v3[3] != 1 || v3[6] != 1) FAIL("vertex 3: %g %g %g | %g %g %g %g", v3[0], v3[1], v3[2], v3[3], v3[4], v3[5], v3[6]);
    glDeleteQueries(1, &q); glDeleteBuffers(1, &b); glDeleteProgram(p);
    return 1;
}

static int t_stencil(void)
{
    GLuint p = program(quad_vs, "#version 330\nuniform vec4 col;\nout vec4 c;\nvoid main() { c = col; }\n");
    if (!p) FAIL("program");
    glUseProgram(p);
    glEnable(GL_STENCIL_TEST);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    /* write 1 into the left half (scissored draw), nothing to colour */
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 16, 32);
    quad();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* draw where stencil == 1 */
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glUniform4f(glGetUniformLocation(p, "col"), 1, 0, 1, 1);
    quad();
    glDisable(GL_STENCIL_TEST);
    CHECK_GL("draw");
    GLint bits; glGetIntegerv(GL_STENCIL_BITS, &bits);
    if (!bits) { glDeleteProgram(p); return 1; }      /* a window without stencil: nothing to check */
    EXPECT_PX(8, 16, 255, 0, 255, 255);
    EXPECT_PX(24, 16, 0, 0, 0, 0);
    glDeleteProgram(p);
    return 1;
}

static int t_dual_source(void)
{
    GLuint p = program(quad_vs, "#version 330\nlayout(location = 0, index = 0) out vec4 c;\nlayout(location = 0, index = 1) out vec4 f;\n"
                                "void main() { c = vec4(0.5, 0.5, 0.5, 1.0); f = vec4(1.0, 0.0, 0.5, 1.0); }\n");
    if (!p) FAIL("program");
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC1_COLOR);         /* src + dst * src1 */
    glUseProgram(p);
    quad();
    glDisable(GL_BLEND);
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 255, 128, 255, -1);
    glDeleteProgram(p);
    return 1;
}

static int t_sampler_object(void)
{
    /* a 2x1 texture black|white sampled in the middle: linear blends, a nearest sampler object doesn't */
    GLuint p = program(quad_vs, "#version 330\nuniform sampler2D t;\nout vec4 c;\nvoid main() { c = texture(t, vec2(0.5, 0.5)); }\n");
    if (!p) FAIL("program");
    uint8_t px[8] = { 0, 0, 0, 255, 255, 255, 255, 255 };
    GLuint t = tex2d(GL_RGBA8, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUseProgram(p);
    quad();
    EXPECT_PX(16, 16, 128, 128, 128, 255);
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindSampler(0, s);
    quad();
    glBindSampler(0, 0);
    CHECK_GL("draw");
    uint8_t got[4]; rgba_at(16, 16, got);
    if (!(near(got, 255, 255, 255, 255) || near(got, 0, 0, 0, 255))) FAIL("nearest sampler gave %d,%d,%d", got[0], got[1], got[2]);
    glDeleteSamplers(1, &s); glDeleteTextures(1, &t); glDeleteProgram(p);
    return 1;
}

static int t_shadow(void)
{
    /* a depth texture with compare mode: texture() returns the comparison */
    GLuint p = program(quad_vs, "#version 330\nuniform sampler2DShadow t;\nout vec4 c;\nvoid main() { c = vec4(texture(t, vec3(0.5, 0.5, 0.4)), texture(t, vec3(0.5, 0.5, 0.6)), 0.0, 1.0); }\n");
    if (!p) FAIL("program");
    float d = 0.5f;
    GLuint t;
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glUseProgram(p);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 255, 0, 0, 255);              /* 0.4 <= 0.5 passes, 0.6 <= 0.5 fails */
    glDeleteTextures(1, &t); glDeleteProgram(p);
    return 1;
}

static int t_layer_render(void)
{
    /* render into layer 1 of an array texture through glFramebufferTextureLayer, then sample it */
    GLuint pw = program(quad_vs, "#version 330\nout vec4 c;\nvoid main() { c = vec4(0.0, 0.0, 1.0, 1.0); }\n");
    GLuint pr = program(quad_vs, "#version 330\nuniform sampler2DArray t;\nout vec4 c;\nvoid main() { c = texture(t, vec3(0.5, 0.5, 1.0)) + texture(t, vec3(0.5, 0.5, 0.0)); }\n");
    if (!pw || !pr) FAIL("program");
    GLuint t, fbo;
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 4, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t, 0, 0);
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t, 0, 1);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) FAIL("incomplete");
    glViewport(0, 0, 4, 4);
    glUseProgram(pw);
    quad();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(pr);
    quad();
    CHECK_GL("draw");
    EXPECT_PX(16, 16, 255, 0, 255, 255);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &t); glDeleteProgram(pw); glDeleteProgram(pr);
    return 1;
}

static int t_frag_data_location(void)
{
    /* glBindFragDataLocation: the second output goes to colour 0 */
    GLuint fbo, tex[2];
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(2, tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, tex[i], 0);
    }
    GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, bufs);
    GLuint p = glCreateProgram();
    GLuint v = shader(GL_VERTEX_SHADER, quad_vs);
    GLuint f = shader(GL_FRAGMENT_SHADER, "#version 150\nout vec4 first;\nout vec4 second;\nvoid main() { first = vec4(1.0, 0.0, 0.0, 1.0); second = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    glAttachShader(p, v); glAttachShader(p, f);
    glBindFragDataLocation(p, 0, "second");
    glBindFragDataLocation(p, 1, "first");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) FAIL("link");
    if (glGetFragDataLocation(p, "first") != 1) FAIL("first at %d", glGetFragDataLocation(p, "first"));
    glViewport(0, 0, 4, 4);
    glUseProgram(p);
    quad();
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    EXPECT_PX(2, 2, 0, 255, 0, 255);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    EXPECT_PX(2, 2, 255, 0, 0, 255);
    CHECK_GL("draw");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(2, tex); glDeleteProgram(p); glDeleteShader(v); glDeleteShader(f);
    return 1;
}

static int t_clear_masks(void)
{
    /* glClear follows the colour and depth write masks */
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    glClearColor(1, 0, 1, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    CHECK_GL("masked clear");
    EXPECT_PX(16, 16, 255, 255, 0, 255);
    /* depth: cleared to 0.25, then a clear with the depth mask off leaves it */
    GLuint p = program(quad_vs, "#version 330\nout vec4 c;\nvoid main() { c = vec4(0.0, 0.0, 1.0, 1.0); gl_FragDepth = 0.5; }\n");
    if (!p) FAIL("program");
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_FALSE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glUseProgram(p);
    quad();                                        /* 0.5 < 0.25 fails: nothing drawn */
    glDisable(GL_DEPTH_TEST);
    CHECK_GL("depth");
    EXPECT_PX(16, 16, 255, 255, 0, 255);
    glDeleteProgram(p);
    return 1;
}

static int t_errors(void)
{
    /* a few of the errors programs rely on */
    glBindBuffer(0x1234, 0);
    if (glGetError() != GL_INVALID_ENUM) FAIL("bad buffer target");
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void *)16);
    if (glGetError() != GL_INVALID_OPERATION) FAIL("client pointer in a VAO");
    glBindVertexArray(0); glDeleteVertexArrays(1, &vao);
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT) FAIL("empty fbo status");
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
    while (glGetError()) ;
    return 1;
}

/* ---- the runner ---------------------------------------------------------------------- */

static const struct { const char *name; int (*fn)(void); } tests[] = {
    { "version", t_version }, { "vao-draw", t_vao_draw }, { "instancing", t_instancing }, { "int-attribs", t_int_attribs },
    { "const-attrib", t_const_attrib }, { "elements", t_elements }, { "formats", t_formats }, { "int-texture", t_int_texture },
    { "texture-array", t_texture_array }, { "texture-3d-cube", t_texture_3d_cube }, { "mrt-fbo", t_mrt_fbo }, { "blit-msaa", t_blit_msaa },
    { "scissor-clear", t_scissor_clear }, { "pbo", t_pbo }, { "buffers", t_buffers }, { "ubo", t_ubo }, { "texture-buffer", t_texture_buffer },
    { "sync-query", t_sync_query }, { "transform-feedback", t_transform_feedback }, { "stencil", t_stencil }, { "dual-source", t_dual_source },
    { "sampler-object", t_sampler_object }, { "shadow", t_shadow }, { "layer-render", t_layer_render },
    { "frag-data-location", t_frag_data_location }, { "clear-masks", t_clear_masks }, { "errors", t_errors },
};

int main(int argc, char **argv)
{
    sic_gl_context *ctx = sic_gl_create(W, H);
    if (!ctx) { fprintf(stderr, "glapi: no GL context\n"); return 2; }
    sic_gl_make_current(ctx);
    int run = 0, passed = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int want = argc < 2;
        for (int a = 1; a < argc; a++) if (strstr(tests[i].name, argv[a])) want = 1;
        if (!want) continue;
        reset();
        why[0] = 0;
        run++;
        if (tests[i].fn()) { passed++; printf("PASS %s\n", tests[i].name); }
        else { printf("FAIL %s: %s\n", tests[i].name, why); }
        fflush(stdout);
    }
    printf("%d/%d passed\n", passed, run);
    sic_gl_destroy(ctx);
    return passed == run ? 0 : 1;
}
