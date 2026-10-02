/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The R300 renderer behind libvirgl: it takes the virgl command stream
 * (Gallium state objects, TGSI shaders, draws) that would otherwise go to
 * the host, and runs it on an R300-class IGP. The vertex stage runs on the
 * CPU, everything from rasterization on runs on the GPU. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "r300_hw.h"
#include "r300_priv.h"
#include "r300_reg.h"
#include "p_defines.h"
#include "virgl_protocol.h"
#include "virgl_hw.h"

struct gpu_res_create;

/* ---- the interface libvirgl uses ------------------------------------------------ */
struct r300_vrend;
struct r300_vrend *r300_vrend_open(void);                    /* NULL if there is no usable R300 */
void r300_vrend_close(struct r300_vrend *v);
uint32_t r300_vrend_res_create(struct r300_vrend *v, const struct gpu_res_create *rc);   /* 0 on failure */
void r300_vrend_res_destroy(struct r300_vrend *v, uint32_t id);
int  r300_vrend_transfer(struct r300_vrend *v, uint32_t id, int to_host, uint32_t level, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t stride, void *guest, uint64_t offset);
int  r300_vrend_submit(struct r300_vrend *v, const uint32_t *buf, uint32_t dwords);
int  r300_vrend_res_attach(struct r300_vrend *v, uint32_t id, uint32_t w, uint32_t h, uint32_t format);
int  r300_vrend_scanout(struct r300_vrend *v, uint32_t id);  /* the panel shows this render target (0: the console) */
int  r300_vrend_present(struct r300_vrend *v, uint32_t x, uint32_t y, uint32_t w, uint32_t h);  /* a rectangle of it, finished */
int  r300_vrend_finish(struct r300_vrend *v);                /* wait until the GPU is done */
void r300_vrend_caps(struct r300_vrend *v, struct virgl_caps_v2 *caps);

/* ---- internals -------------------------------------------------------------------- */
#define VR_MAX_RES     1024
#define VR_MAX_OBJ     4096
#define VR_MAX_LEVELS  12
#define VR_MAX_UNITS   8
#define VR_MAX_VB      16
#define VR_MAX_VE      16

struct vr_res {
    uint32_t id, target, format, bind, w, h, last_level;
    uint8_t *host;                  /* buffers: the data, in RAM */
    uint32_t size;
    struct r300_bo bo;              /* textures and render targets: a buffer in VRAM (handle 0 = none) */
    uint32_t vram_size;
    int foreign;                    /* another program's (attached): not freed here */
    uint32_t last_use;              /* the submission (serial) that last had the GPU touch it */
    uint32_t stride[VR_MAX_LEVELS], offset[VR_MAX_LEVELS], lh[VR_MAX_LEVELS];   /* bytes per row, level offset, rows */
    int npot;
};

struct vr_shader {
    int frag;
    struct tgsi_shader t;
    struct r300_fs fs;              /* fragment: the compiled program */
    int ok;
};

struct vr_ve { int n; uint32_t offset[VR_MAX_VE], vb[VR_MAX_VE], format[VR_MAX_VE]; };
struct vr_view { uint32_t res, format, first_level, last_level; };
struct vr_sampler { uint32_t s0; float min_lod, max_lod; };
struct vr_surface { uint32_t res, format, level; };

enum { OBJ_NONE, OBJ_BLEND, OBJ_RS, OBJ_DSA, OBJ_SHADER, OBJ_VE, OBJ_VIEW, OBJ_SAMPLER, OBJ_SURFACE };
struct vr_obj {
    uint32_t handle;
    int type;
    union {
        uint32_t dw[12];            /* blend, rasterizer, dsa: the protocol dwords */
        struct vr_shader *shader;
        struct vr_ve ve;
        struct vr_view view;
        struct vr_sampler sampler;
        struct vr_surface surf;
    } u;
};

struct r300_vrend {
    struct r300_hw hw;
    struct vr_res *res[VR_MAX_RES]; int nres;
    uint32_t next_buffer_id;        /* buffers live in RAM: ids 0x40000000 + n, apart from the kernel's handles */
    struct vr_obj *obj[VR_MAX_OBJ]; int nobj;
    /* bound state */
    uint32_t blend[12], rs[12], dsa[12];
    int have_blend, have_rs, have_dsa;
    struct vr_shader *vs, *fs;
    struct vr_ve ve;
    struct { uint32_t stride, offset, res; } vb[VR_MAX_VB]; int nvb;
    struct { uint32_t res, size, offset; } ib;
    float vs_const[256][4], fs_const[32][4]; int nvs_const, nfs_const;
    struct vr_view views[VR_MAX_UNITS]; int nviews;
    struct vr_sampler samplers[VR_MAX_UNITS]; int nsamplers;
    struct vr_surface cbuf, zbuf; int have_cbuf, have_zbuf;
    float vp_scale[3], vp_trans[3];
    uint32_t sc_minx, sc_miny, sc_maxx, sc_maxy;
    float blend_color[4];
    uint32_t stencil_ref;
    int busy;                       /* GPU work queued since the last wait */
    struct vr_shader *clear_fs, *blit_fs, *blit_vs;   /* internal programs */
    int warned;
    uint32_t scan_src, front;       /* scanout: the render target, and the buffer the panel reads (a copy) */
    uint32_t serial;                /* the submission being built (counts from 1) */
    uint32_t fences[256];           /* the fence of each recent submission, by serial */
    uint32_t state[1700]; int state_len; uint32_t state_serial;   /* the last draw's state, to skip repeating it */
};

struct vr_res *vr_res_get(struct r300_vrend *v, uint32_t id);
void vr_sync(struct r300_vrend *v);                                  /* the GPU idle */
void vr_commit(struct r300_vrend *v);                                /* submit what is built */
void vr_wait_res(struct r300_vrend *v, struct vr_res *r);            /* before the CPU touches r's memory */
struct vr_shader *vr_shader_from_text(const char *tgsi);
/* vdraw.c */
void vr_draw_vbo(struct r300_vrend *v, const uint32_t *d);
/* A screen-aligned quad on the current framebuffer with the given fragment
 * program and constants, texcoords (s0,t0)-(s1,t1) in GENERIC[0]. */
void vr_draw_quad(struct r300_vrend *v, struct vr_shader *fs, float x0, float y0, float x1, float y1, float z,
                  float s0, float t0, float s1, float t1, int color_mask, int depth_write);
