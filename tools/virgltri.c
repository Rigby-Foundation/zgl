/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* virgltri: a triangle drawn by the host GPU through libvirgl, read back
 * and checked; shown in a zwm window if the server is running. */
#include <virgl.h>
#include <zwm.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 256
#define H 256

static const char vs[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: MOV OUT[1], IN[1]\n"
    "  2: END\n";
static const char fs[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: END\n";

int main(void)
{
    virgl *v = virgl_open();
    if (!v) { perror("virgltri: /dev/gpu0"); return 1; }
    const struct virgl_caps_v2 *caps = virgl_caps(v);
    if (caps) printf("virgltri: host renderer \"%.64s\", GLSL level %u, max texture %u\n", caps->renderer, caps->v1.glsl_level, caps->max_texture_2d_size);

    /* colour buffer: a 2D texture with a guest copy for the readback */
    virgl_res *color = virgl_res_create(v, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8X8_UNORM,
                                        VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, W, H, 1, 0, W * H * 4);
    /* (no Y_0_TOP: rows come back bottom-up, like GL; the window copy flips them) */
    if (!color || !color->map) { fprintf(stderr, "virgltri: colour buffer\n"); return 1; }
    /* vertex buffer: 3 x (x, y, z, w, r, g, b, a) */
    float verts[3][8] = {
        { -0.8f, -0.8f, 0, 1, 1, 0, 0, 1 },
        {  0.8f, -0.8f, 0, 1, 0, 1, 0, 1 },
        {  0.0f,  0.8f, 0, 1, 0, 0, 1, 1 },
    };
    virgl_res *vbo = virgl_res_create(v, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER, sizeof verts, 1, 1, 0, sizeof verts);
    if (!vbo || !vbo->map) { fprintf(stderr, "virgltri: vbo\n"); return 1; }
    memcpy(vbo->map, verts, sizeof verts);
    virgl_res_to_host(v, vbo, 0, 0, 0, 0, sizeof verts, 1, 1, 0, 0, 0);

    uint32_t h_vs = virgl_handle(v), h_fs = virgl_handle(v), h_surf = virgl_handle(v), h_ve = virgl_handle(v);
    uint32_t h_blend = virgl_handle(v), h_rs = virgl_handle(v), h_dsa = virgl_handle(v);
    virgl_create_shader(v, h_vs, PIPE_SHADER_VERTEX, vs);
    virgl_create_shader(v, h_fs, PIPE_SHADER_FRAGMENT, fs);
    virgl_bind_shader(v, h_vs, PIPE_SHADER_VERTEX);
    virgl_bind_shader(v, h_fs, PIPE_SHADER_FRAGMENT);
    virgl_create_surface(v, h_surf, color->id, VIRGL_FORMAT_B8G8R8X8_UNORM, 0, 0);
    virgl_set_framebuffer(v, 0, &h_surf, 1);
    virgl_set_viewport(v, 0, 0, W, H, 0.0f, 1.0f, 0);

    uint32_t blend[VIRGL_OBJ_BLEND_SIZE - 1] = { 0 };
    blend[2] = VIRGL_OBJ_BLEND_S2_RT_COLORMASK(0xF);          /* S2(0): no blending, write all */
    virgl_create_blend(v, h_blend, blend);
    virgl_bind_object(v, h_blend, VIRGL_OBJECT_BLEND);
    uint32_t rs[VIRGL_OBJ_RS_SIZE - 1] = { 0 };
    rs[0] = VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) | VIRGL_OBJ_RS_S0_FRONT_CCW(1);
    union { float f; uint32_t u; } one = { 1.0f };
    rs[1] = one.u;                                              /* point size */
    rs[4] = one.u;                                              /* line width */
    virgl_create_rasterizer(v, h_rs, rs);
    virgl_bind_object(v, h_rs, VIRGL_OBJECT_RASTERIZER);
    uint32_t dsa[VIRGL_OBJ_DSA_SIZE - 1] = { 0 };
    virgl_create_dsa(v, h_dsa, dsa);
    virgl_bind_object(v, h_dsa, VIRGL_OBJECT_DSA);

    uint32_t off[2] = { 0, 16 }, idx[2] = { 0, 0 }, fmt[2] = { VIRGL_FORMAT_R32G32B32A32_FLOAT, VIRGL_FORMAT_R32G32B32A32_FLOAT };
    virgl_create_vertex_elements(v, h_ve, 2, off, idx, fmt);
    virgl_bind_object(v, h_ve, VIRGL_OBJECT_VERTEX_ELEMENTS);
    uint32_t stride = 32, voff = 0, vres = vbo->id;
    virgl_set_vertex_buffers(v, 1, &stride, &voff, &vres);

    float clear[4] = { 0.1f, 0.1f, 0.2f, 1.0f };
    virgl_clear(v, PIPE_CLEAR_COLOR0, clear, 1.0, 0);
    virgl_draw(v, PIPE_PRIM_TRIANGLES, 0, 3, 0, 0, 2);
    if (virgl_flush(v) != 0) { perror("virgltri: submit"); return 1; }
    if (virgl_res_from_host(v, color, 0, 0, 0, 0, W, H, 1, W * 4, W * H * 4, 0) != 0) { perror("virgltri: readback"); return 1; }

    uint32_t *px = color->map;
    printf("virgltri: corner %08x, centre %08x, bottom-middle %08x, top-middle %08x\n",
           px[0], px[(H / 2) * W + W / 2], px[(H - 20) * W + W / 2], px[20 * W + W / 2]);
    /* a coarse picture on the console, rows top-down (GL readback is bottom-up) */
    for (int y = H - 8; y >= 0; y -= 16) {
        char line[W / 8 + 1];
        for (int x = 0; x < W; x += 8) {
            uint32_t p = px[y * W + x] & 0xFFFFFF;
            line[x / 8] = p == 0x1a1a33 ? '.' : (p >> 16) > 0x80 ? 'R' : ((p >> 8) & 0xFF) > 0x80 ? 'G' : (p & 0xFF) > 0x80 ? 'B' : '*';
        }
        line[W / 8] = 0;
        printf("  %s\n", line);
    }
    int ok = (px[0] & 0xFFFFFF) == 0x1a1a33 && (px[(H / 2) * W + W / 2] & 0xFFFFFF) != 0x1a1a33;
    printf("virgltri: %s\n", ok ? "the host GPU drew the triangle" : "unexpected pixels");

    zwm *c = zwm_connect();
    if (c) {
        struct zwm_m_geom g;
        int win = zwm_create(c, W, H, "virgl triangle", 0, &g);
        for (int y = 0; y < H; y++) zwm_blit(c, win, 0, y, W, 1, px + (H - 1 - y) * W, W);
        zwm_event ev;
        while (zwm_next_event(c, &ev, 1) > 0)
            if (ev.type == ZWM_S_CLOSE || (ev.type == ZWM_S_KEY && ev.key.down)) break;
        zwm_disconnect(c);
    }
    virgl_res_destroy(v, vbo);
    virgl_res_destroy(v, color);
    virgl_close(v);
    return ok ? 0 : 1;
}
