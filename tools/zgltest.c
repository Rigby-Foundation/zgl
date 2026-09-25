/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* zgltest: libzgl on the host GPU without a window system: a coloured
 * triangle (immediate mode), a lit cube (fixed function), a textured quad
 * (GLSL), each read back and sketched on the console. */
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 96

static void sketch(const uint32_t *px, int stride)
{
    for (int y = 0; y < H; y += 8) {
        char line[W / 4 + 1];
        for (int x = 0; x < W; x += 4) {
            uint32_t p = px[y * stride + x] & 0xFFFFFF;
            int r = p >> 16, g = (p >> 8) & 255, b = p & 255;
            line[x / 4] = p == 0x1a1a33 ? '.' : (r > g && r > b) ? 'R' : (g > r && g > b) ? 'G' : (b > r && b > g) ? 'B' : r > 200 ? '#' : r > 100 ? '+' : ':';
        }
        line[W / 4] = 0;
        printf("  %s\n", line);
    }
}

static int check(const char *what)
{
    GLenum e = glGetError();
    if (e) printf("zgltest: GL error 0x%x after %s\n", e, what);
    return e == 0;
}

int main(void)
{
    sic_gl_context *ctx = sic_gl_create(W, H);
    if (!ctx) { fprintf(stderr, "zgltest: no GPU context (is this a virgl VM?)\n"); return 1; }
    printf("zgltest: %s / %s / GLSL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    int stride, ok = 1;

    /* 1: immediate mode triangle */
    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
    glColor3f(0, 1, 0); glVertex2f(0.8f, -0.8f);
    glColor3f(0, 0, 1); glVertex2f(0, 0.8f);
    glEnd();
    const uint32_t *px = sic_gl_swap(ctx, &stride);
    ok &= check("triangle");
    printf("zgltest: immediate-mode triangle\n");
    sketch(px, stride);
    ok &= (px[(H / 2) * stride + W / 2] & 0xFFFFFF) != 0x1a1a33;

    /* 2: lit cube through the fixed-function pipeline */
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING); glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    float lp[4] = { 1, 2, 3, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, lp);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-1, 1, -0.75, 0.75, 1, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0, 0, -4); glRotatef(30, 1, 0, 0); glRotatef(40, 0, 1, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    static const float v[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
    static const int f[6][4] = { {0,1,2,3},{4,7,6,5},{0,4,5,1},{3,2,6,7},{0,3,7,4},{1,5,6,2} };
    static const float n[6][3] = { {0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0} };
    glBegin(GL_QUADS);
    for (int i = 0; i < 6; i++) { glNormal3fv(n[i]); glColor3f(0.9f, 0.6f, 0.2f); for (int k = 0; k < 4; k++) glVertex3fv(v[f[i][k]]); }
    glEnd();
    px = sic_gl_swap(ctx, &stride);
    ok &= check("cube");
    printf("zgltest: lit cube (fixed function, quads)\n");
    sketch(px, stride);
    glDisable(GL_LIGHTING);

    /* 3: a GLSL program with a texture */
    const char *vs = "attribute vec2 pos; attribute vec2 tc; varying vec2 uv; uniform float scale;\nvoid main() { gl_Position = vec4(pos * scale, 0.0, 1.0); uv = tc; }";
    const char *fs = "uniform sampler2D tex; varying vec2 uv; uniform vec4 tint;\nvoid main() { gl_FragColor = texture2D(tex, uv) * tint; }";
    GLuint sv = glCreateShader(GL_VERTEX_SHADER), sf = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
    glShaderSource(sv, 1, &vs, NULL); glCompileShader(sv);
    glShaderSource(sf, 1, &fs, NULL); glCompileShader(sf);
    GLint st;
    glGetShaderiv(sv, GL_COMPILE_STATUS, &st); if (!st) { char log[512]; glGetShaderInfoLog(sv, 512, NULL, log); printf("vs: %s\n", log); }
    glGetShaderiv(sf, GL_COMPILE_STATUS, &st); if (!st) { char log[512]; glGetShaderInfoLog(sf, 512, NULL, log); printf("fs: %s\n", log); }
    glAttachShader(pr, sv); glAttachShader(pr, sf); glLinkProgram(pr);
    glGetProgramiv(pr, GL_LINK_STATUS, &st); if (!st) { char log[512]; glGetProgramInfoLog(pr, 512, NULL, log); printf("link: %s\n", log); }
    glUseProgram(pr);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    uint8_t img[8 * 8 * 4];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t *p = img + (y * 8 + x) * 4; int c = (x + y) & 1; p[0] = c ? 255 : 30; p[1] = c ? 255 : 30; p[2] = c ? 255 : 30; p[3] = 255; }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(glGetUniformLocation(pr, "tex"), 0);
    glUniform1f(glGetUniformLocation(pr, "scale"), 0.9f);
    glUniform4f(glGetUniformLocation(pr, "tint"), 1.0f, 0.3f, 0.3f, 1.0f);
    float quad[4][4] = { { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { 1, 1, 1, 1 }, { -1, 1, 0, 1 } };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    GLint apos = glGetAttribLocation(pr, "pos"), atc = glGetAttribLocation(pr, "tc");
    glVertexAttribPointer((GLuint)apos, 2, GL_FLOAT, 0, 16, (void *)0); glEnableVertexAttribArray((GLuint)apos);
    glVertexAttribPointer((GLuint)atc, 2, GL_FLOAT, 0, 16, (void *)8); glEnableVertexAttribArray((GLuint)atc);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    px = sic_gl_swap(ctx, &stride);
    ok &= check("textured quad");
    printf("zgltest: textured quad (GLSL, VBO)\n");
    sketch(px, stride);
    /* 4: render to a texture through an FBO, then show it; then copy the window into a texture */
    glUseProgram(0);
    glDisableVertexAttribArray((GLuint)apos); glDisableVertexAttribArray((GLuint)atc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLuint rt; glGenTextures(1, &rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    ok &= glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glViewport(0, 0, 64, 64);
    glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBegin(GL_TRIANGLES); glColor3f(1, 0, 0); glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(0, 1); glEnd();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, rt);
    glBegin(GL_QUADS); glColor3f(1, 1, 1);
    glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f); glTexCoord2f(1, 0); glVertex2f(0.9f, -0.9f);
    glTexCoord2f(1, 1); glVertex2f(0.9f, 0.9f); glTexCoord2f(0, 1); glVertex2f(-0.9f, 0.9f); glEnd();
    px = sic_gl_swap(ctx, &stride);
    ok &= check("fbo");
    printf("zgltest: render to texture (FBO)\n");
    sketch(px, stride);
    uint32_t mid = px[(H / 2) * stride + W / 2] & 0xFFFFFF, corner = px[(H / 8) * stride + W / 8] & 0xFFFFFF;
    ok &= (mid >> 16) > 150 && ((mid >> 8) & 255) < 100;            /* red triangle in the middle */
    ok &= ((corner >> 8) & 255) > 150 && (corner >> 16) < 100;      /* green background at the corner */
    /* copy: the window into a texture, draw it small */
    GLuint cp; glGenTextures(1, &cp); glBindTexture(GL_TEXTURE_2D, cp);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glColor3f(1, 1, 1);
    glTexCoord2f(0, 0); glVertex2f(-1, -1); glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(-1, 1); glEnd();
    px = sic_gl_swap(ctx, &stride);
    ok &= check("copytex");
    mid = px[(H / 2) * stride + W / 2] & 0xFFFFFF;
    ok &= (mid >> 16) > 150;                                        /* the copy shows the red triangle too */
    printf("zgltest: glCopyTexSubImage2D %s\n", (mid >> 16) > 150 ? "ok" : "wrong");

    printf("zgltest: %s\n", ok ? "ok" : "FAILED");
    sic_gl_destroy(ctx);
    return ok ? 0 : 1;
}
