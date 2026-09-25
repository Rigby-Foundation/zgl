/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * libvirgl: the GPU from user space on sic. Talks to /dev/gpu0 (contexts,
 * resources, transfers, submissions) and encodes virglrenderer's command
 * stream — the Gallium-level protocol the host runs on its OpenGL. This is
 * what a GL implementation (libzgl) builds on; a program can use it
 * directly if it wants to drive the host GPU at the Gallium level.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "p_defines.h"
#include "virgl_protocol.h"
#include "virgl_hw.h"
#include <abi/gpu.h>

typedef struct virgl virgl;

/* A resource: a buffer or texture on the host, optionally with a guest copy
 * you can write and transfer. */
typedef struct {
    uint32_t id;
    uint32_t target, format, bind;
    uint32_t width, height, depth;
    size_t   size;              /* of the guest backing */
    void    *map;               /* the backing, mapped (NULL if size 0) */
    int      foreign;           /* another program's, attached: destroying detaches */
} virgl_res;

virgl *virgl_open(void);                    /* NULL if there is no /dev/gpu0 */
void   virgl_close(virgl *v);
int    virgl_fd(virgl *v);
const struct virgl_caps_v2 *virgl_caps(virgl *v);   /* the host's capabilities */

/* Resources */
virgl_res *virgl_res_create(virgl *v, uint32_t target, uint32_t format, uint32_t bind,
                            uint32_t width, uint32_t height, uint32_t depth, uint32_t flags, size_t backing);
virgl_res *virgl_res_create_ex(virgl *v, struct gpu_res_create *rc);    /* every parameter (mip levels, ...) */
void  virgl_res_destroy(virgl *v, virgl_res *r);
/* Another program's resource, by id, for use in this context (a compositor). */
virgl_res *virgl_res_attach(virgl *v, uint32_t id, uint32_t target, uint32_t format, uint32_t w, uint32_t h);
/* The display shows `r`; virgl_present puts a rectangle of it on screen. */
int   virgl_scanout(virgl *v, virgl_res *r);
int   virgl_present(virgl *v, virgl_res *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* Copy a box between the guest backing and the host: stride is bytes per
 * row in the backing, offset where the box's first byte sits in it. */
int   virgl_res_to_host(virgl *v, virgl_res *r, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t w, uint32_t h, uint32_t d, uint32_t stride, uint32_t layer_stride, uint64_t offset);
int   virgl_res_from_host(virgl *v, virgl_res *r, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                          uint32_t w, uint32_t h, uint32_t d, uint32_t stride, uint32_t layer_stride, uint64_t offset);

/* The command stream: commands accumulate and go to the host on
 * virgl_flush() (or when the buffer fills). Object handles are yours to
 * choose; virgl_handle() hands out unused ones. */
int      virgl_flush(virgl *v);   /* uploads the dirty buffer ranges, then submits */
/* ... and wait until the host has run it, with a fence in our context:
 * the host flushes the context for it, and only then do other GL
 * contexts (the display, a compositor sampling our buffer) see what we
 * drew (macOS GL, which QEMU's virgl uses there, is strict about that). */
int  virgl_flush_fenced(virgl *v);
/* A buffer range written through the mapping: uploaded (one transfer) with the next submit. */
void     virgl_res_dirty(virgl *v, virgl_res *r, uint32_t off, uint32_t len);
void     virgl_forget(virgl *v, virgl_res *r);
uint32_t virgl_handle(virgl *v);

/* Raw encoding: header (cmd, object type, payload dwords) then the dwords. */
void virgl_cmd(virgl *v, uint32_t cmd, uint32_t obj, uint32_t len);
void virgl_dw(virgl *v, uint32_t x);
void virgl_f(virgl *v, float x);
void virgl_dws(virgl *v, const void *p, size_t n);   /* n dwords */

/* Encoders for the state the GL front end needs. */
void virgl_create_shader(virgl *v, uint32_t handle, uint32_t type, const char *tgsi);
void virgl_bind_shader(virgl *v, uint32_t handle, uint32_t type);
void virgl_create_surface(virgl *v, uint32_t handle, uint32_t res, uint32_t format, uint32_t level, uint32_t layers);
void virgl_set_framebuffer(virgl *v, uint32_t zsurf, const uint32_t *cbufs, uint32_t nr);
void virgl_set_viewport(virgl *v, float x, float y, float w, float h, float near, float far, int y_flip);
void virgl_set_scissor(virgl *v, uint32_t minx, uint32_t miny, uint32_t maxx, uint32_t maxy);
void virgl_clear(virgl *v, uint32_t buffers, const float rgba[4], double depth, uint32_t stencil);
void virgl_create_vertex_elements(virgl *v, uint32_t handle, uint32_t n, const uint32_t *src_offset,
                                  const uint32_t *buffer_index, const uint32_t *format);
void virgl_set_vertex_buffers(virgl *v, uint32_t n, const uint32_t *stride, const uint32_t *offset, const uint32_t *res);
void virgl_set_index_buffer(virgl *v, uint32_t res, uint32_t index_size, uint32_t offset);
void virgl_draw(virgl *v, uint32_t mode, uint32_t start, uint32_t count, int indexed, uint32_t min_index, uint32_t max_index);
void virgl_bind_object(virgl *v, uint32_t handle, uint32_t obj);
void virgl_destroy_object(virgl *v, uint32_t handle, uint32_t obj);
void virgl_set_constants(virgl *v, uint32_t shader_type, const float *data, uint32_t n);   /* n floats, inline */
void virgl_create_sampler_view(virgl *v, uint32_t handle, uint32_t res, uint32_t target, uint32_t format, uint32_t first_level, uint32_t last_level, uint32_t swizzle);
#define VIRGL_SWIZZLE_IDENTITY (VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_R(0) | VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_G(1) | VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_B(2) | VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_A(3))
void virgl_set_sampler_views(virgl *v, uint32_t shader_type, uint32_t start, const uint32_t *handles, uint32_t n);
void virgl_create_sampler_state(virgl *v, uint32_t handle, uint32_t wrap_s, uint32_t wrap_t, uint32_t min_img, uint32_t min_mip, uint32_t mag_img);
void virgl_bind_sampler_states(virgl *v, uint32_t shader_type, uint32_t start, const uint32_t *handles, uint32_t n);
void virgl_set_blend_color(virgl *v, const float rgba[4]);
void virgl_set_stencil_ref(virgl *v, uint32_t front, uint32_t back);
void virgl_resource_copy_region(virgl *v, uint32_t dst, uint32_t dst_level, uint32_t dx, uint32_t dy, uint32_t dz,
                                uint32_t src, uint32_t src_level, uint32_t sx, uint32_t sy, uint32_t sz, uint32_t w, uint32_t h, uint32_t d);
void virgl_inline_write(virgl *v, uint32_t res, uint32_t level, uint32_t usage, uint32_t stride, uint32_t layer_stride,
                        uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, const void *data, size_t bytes);

/* Blend / rasterizer / depth-stencil objects: fill the dwords the protocol
 * defines and create with the matching size. */
void virgl_create_blend(virgl *v, uint32_t handle, const uint32_t dw[VIRGL_OBJ_BLEND_SIZE - 1]);
void virgl_create_rasterizer(virgl *v, uint32_t handle, const uint32_t dw[VIRGL_OBJ_RS_SIZE - 1]);
void virgl_create_dsa(virgl *v, uint32_t handle, const uint32_t dw[VIRGL_OBJ_DSA_SIZE - 1]);
