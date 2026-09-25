/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/gpu0 and the virgl command stream encoder. */
#include "virgl.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define CMDBUF_DWORDS (GPU_SUBMIT_MAX / 4)

struct virgl {
    int fd;
    struct gpu_info info;
    struct virgl_caps_v2 caps;
    int have_caps;
    uint32_t *buf;
    uint32_t used;              /* dwords */
    uint32_t next_handle;
    /* buffer ranges written through their mapping since the last submit:
     * uploaded in one transfer each just before it, instead of one
     * synchronous round trip per draw */
    struct { virgl_res *r; uint32_t lo, hi; } dirty[4];
    int ndirty;
};

virgl *virgl_open(void)
{
    int fd = open("/dev/gpu0", O_RDWR);
    if (fd < 0) return NULL;
    virgl *v = calloc(1, sizeof *v);
    if (!v) { close(fd); return NULL; }
    v->fd = fd;
    if (ioctl(fd, GPU_IOC_INFO, &v->info) != 0) { close(fd); free(v); return NULL; }
    v->buf = malloc(GPU_SUBMIT_MAX);
    if (!v->buf) { close(fd); free(v); return NULL; }
    v->next_handle = 1;
    if (v->info.capset_size >= sizeof(struct virgl_caps_v1) && v->info.capset_size <= sizeof v->caps) {
        struct gpu_capset cs = { (uint64_t)(uintptr_t)&v->caps, v->info.capset_size, 0 };
        if (ioctl(fd, GPU_IOC_GET_CAPSET, &cs) == 0) v->have_caps = 1;
    }
    return v;
}

void virgl_close(virgl *v)
{
    if (!v) return;
    virgl_flush(v);
    close(v->fd);
    free(v->buf);
    free(v);
}

int virgl_fd(virgl *v) { return v->fd; }
const struct virgl_caps_v2 *virgl_caps(virgl *v) { return v->have_caps ? &v->caps : NULL; }
uint32_t virgl_handle(virgl *v) { return v->next_handle++; }

/* ---- resources ---------------------------------------------------------- */

virgl_res *virgl_res_create_ex(virgl *v, struct gpu_res_create *rc)
{
    if (ioctl(v->fd, GPU_IOC_CREATE_RES, rc) != 0) return NULL;
    virgl_res *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->id = rc->id; r->target = rc->target; r->format = rc->format; r->bind = rc->bind;
    r->width = rc->width; r->height = rc->height; r->depth = rc->depth; r->size = rc->size;
    if (rc->size) {
        r->map = mmap(NULL, rc->size, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, (off_t)rc->id * 4096);
        if (r->map == MAP_FAILED) { r->map = NULL; }
    }
    return r;
}

virgl_res *virgl_res_create(virgl *v, uint32_t target, uint32_t format, uint32_t bind,
                            uint32_t width, uint32_t height, uint32_t depth, uint32_t flags, size_t backing)
{
    struct gpu_res_create rc = {
        .target = target, .format = format, .bind = bind, .width = width, .height = height ? height : 1,
        .depth = depth ? depth : 1, .array_size = 1, .last_level = 0, .nr_samples = 0, .flags = flags,
        .size = (uint32_t)backing,
    };
    return virgl_res_create_ex(v, &rc);
}

void virgl_res_destroy(virgl *v, virgl_res *r)
{
    if (!r) return;
    virgl_flush(v);
    if (r->map) munmap(r->map, r->size);
    virgl_forget(v, r);
    ioctl(v->fd, r->foreign ? GPU_IOC_DETACH_RES : GPU_IOC_DESTROY_RES, &r->id);
    free(r);
}

virgl_res *virgl_res_attach(virgl *v, uint32_t id, uint32_t target, uint32_t format, uint32_t w, uint32_t h)
{
    virgl_flush(v);
    if (ioctl(v->fd, GPU_IOC_ATTACH_RES, &id) != 0) return NULL;
    virgl_res *r = calloc(1, sizeof *r);
    if (!r) { ioctl(v->fd, GPU_IOC_DETACH_RES, &id); return NULL; }
    r->id = id; r->target = target; r->format = format; r->width = w; r->height = h; r->depth = 1;
    r->foreign = 1;
    return r;
}

int virgl_scanout(virgl *v, virgl_res *r)
{
    virgl_flush(v);
    struct gpu_scanout so = { r->id, 0, 0, r->width, r->height };
    return ioctl(v->fd, GPU_IOC_SET_SCANOUT, &so);
}

int virgl_present(virgl *v, virgl_res *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    virgl_flush_fenced(v);                  /* the frame must be visible outside our context */
    struct gpu_scanout so = { r->id, x, y, w, h };
    return ioctl(v->fd, GPU_IOC_FLUSH, &so);
}

static int transfer(virgl *v, virgl_res *r, int to_host, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                    uint32_t w, uint32_t h, uint32_t d, uint32_t stride, uint32_t layer_stride, uint64_t offset)
{
    virgl_flush(v);                     /* commands before the transfer must have run */
    struct gpu_transfer t = { r->id, level, x, y, z, w, h, d, stride, layer_stride, offset };
    return ioctl(v->fd, to_host ? GPU_IOC_TRANSFER_TO_HOST : GPU_IOC_TRANSFER_FROM_HOST, &t);
}

int virgl_res_to_host(virgl *v, virgl_res *r, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                      uint32_t w, uint32_t h, uint32_t d, uint32_t stride, uint32_t layer_stride, uint64_t offset)
{ return transfer(v, r, 1, level, x, y, z, w, h, d, stride, layer_stride, offset); }

int virgl_res_from_host(virgl *v, virgl_res *r, uint32_t level, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t w, uint32_t h, uint32_t d, uint32_t stride, uint32_t layer_stride, uint64_t offset)
{ return transfer(v, r, 0, level, x, y, z, w, h, d, stride, layer_stride, offset); }

/* ---- the command stream ------------------------------------------------- */

/* A buffer range straight to the host: no flush first (these are the
 * uploads the next submit's commands are waiting for). */
static void upload(virgl *v, virgl_res *r, uint32_t lo, uint32_t hi)
{
    struct gpu_transfer t = { r->id, 0, lo, 0, 0, hi - lo, 1, 1, 0, 0, lo };
    ioctl(v->fd, GPU_IOC_TRANSFER_TO_HOST, &t);
}

static void upload_dirty(virgl *v)
{
    int n = v->ndirty;
    v->ndirty = 0;
    for (int i = 0; i < n; i++) upload(v, v->dirty[i].r, v->dirty[i].lo, v->dirty[i].hi);
}

void virgl_res_dirty(virgl *v, virgl_res *r, uint32_t off, uint32_t len)
{
    for (int i = 0; i < v->ndirty; i++)
        if (v->dirty[i].r == r) {
            if (off < v->dirty[i].lo) v->dirty[i].lo = off;
            if (off + len > v->dirty[i].hi) v->dirty[i].hi = off + len;
            return;
        }
    if (v->ndirty == 4) upload_dirty(v);        /* full: send what we have */
    v->dirty[v->ndirty].r = r; v->dirty[v->ndirty].lo = off; v->dirty[v->ndirty].hi = off + len;
    v->ndirty++;
}

void virgl_forget(virgl *v, virgl_res *r)
{
    for (int i = 0; i < v->ndirty; i++)
        if (v->dirty[i].r == r) { v->dirty[i] = v->dirty[--v->ndirty]; i--; }
}

static int submit(virgl *v, uint32_t flags)
{
    struct gpu_submit sb = { (uint64_t)(uintptr_t)v->buf, v->used * 4, flags };
    int rc = ioctl(v->fd, GPU_IOC_SUBMIT, &sb);
    v->used = 0;
    return rc;
}

int virgl_flush(virgl *v)
{
    upload_dirty(v);
    if (!v->used) return 0;
    return submit(v, 0);
}

int virgl_flush_fenced(virgl *v)
{
    upload_dirty(v);
    if (!v->used) virgl_cmd(v, VIRGL_CCMD_NOP, 0, 0);     /* something to hang the fence on */
    return submit(v, GPU_SUBMIT_FENCE);
}

static void reserve(virgl *v, uint32_t dwords)
{
    if (v->used + dwords > CMDBUF_DWORDS)
        virgl_flush(v);
}

void virgl_cmd(virgl *v, uint32_t cmd, uint32_t obj, uint32_t len)
{
    reserve(v, len + 1);
    v->buf[v->used++] = VIRGL_CMD0(cmd, obj, len);
}
void virgl_dw(virgl *v, uint32_t x) { v->buf[v->used++] = x; }
void virgl_f(virgl *v, float x) { union { float f; uint32_t u; } c = { x }; v->buf[v->used++] = c.u; }
void virgl_dws(virgl *v, const void *p, size_t n) { memcpy(v->buf + v->used, p, n * 4); v->used += (uint32_t)n; }

void virgl_create_shader(virgl *v, uint32_t handle, uint32_t type, const char *tgsi)
{
    /* A shader longer than a command buffer goes in continuation packets
     * (offset = total length first, then sent-so-far | CONT). */
    uint32_t total = (uint32_t)strlen(tgsi) + 1, sent = 0;
    uint32_t num_tokens = 300 + total / 4;      /* an upper bound on TGSI tokens: the host sizes its parse buffer with it */
    while (sent < total) {
        uint32_t max_bytes = (CMDBUF_DWORDS - 8) * 4;
        uint32_t chunk = total - sent < max_bytes ? total - sent : max_bytes;
        uint32_t chunk_dw = (chunk + 3) / 4;
        reserve(v, VIRGL_OBJ_SHADER_HDR_SIZE(0) + chunk_dw + 1);
        virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, VIRGL_OBJ_SHADER_HDR_SIZE(0) + chunk_dw);
        virgl_dw(v, handle);
        virgl_dw(v, type);
        virgl_dw(v, sent == 0 ? total : (sent | VIRGL_OBJ_SHADER_OFFSET_CONT));
        virgl_dw(v, num_tokens);
        virgl_dw(v, 0);                                 /* no stream output */
        memset(v->buf + v->used, 0, chunk_dw * 4);
        memcpy(v->buf + v->used, tgsi + sent, chunk);
        v->used += chunk_dw;
        sent += chunk;
        if (sent < total) virgl_flush(v);
    }
}

void virgl_bind_shader(virgl *v, uint32_t handle, uint32_t type)
{
    virgl_cmd(v, VIRGL_CCMD_BIND_SHADER, 0, VIRGL_BIND_SHADER_SIZE);
    virgl_dw(v, handle);
    virgl_dw(v, type);
}

void virgl_create_surface(virgl *v, uint32_t handle, uint32_t res, uint32_t format, uint32_t level, uint32_t layers)
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE);
    virgl_dw(v, handle);
    virgl_dw(v, res);
    virgl_dw(v, format);
    virgl_dw(v, level);
    virgl_dw(v, layers);
}

void virgl_set_framebuffer(virgl *v, uint32_t zsurf, const uint32_t *cbufs, uint32_t nr)
{
    virgl_cmd(v, VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(nr));
    virgl_dw(v, nr);
    virgl_dw(v, zsurf);
    for (uint32_t i = 0; i < nr; i++) virgl_dw(v, cbufs[i]);
}

void virgl_set_viewport(virgl *v, float x, float y, float w, float h, float near, float far, int y_flip)
{
    /* gallium: scale and translate from NDC; y grows up unless flipped */
    float sy = (y_flip ? -1.0f : 1.0f) * h / 2.0f;
    virgl_cmd(v, VIRGL_CCMD_SET_VIEWPORT_STATE, 0, VIRGL_SET_VIEWPORT_STATE_SIZE(1));
    virgl_dw(v, 0);
    virgl_f(v, w / 2.0f);
    virgl_f(v, sy);
    virgl_f(v, (far - near) / 2.0f);
    virgl_f(v, x + w / 2.0f);
    virgl_f(v, y + h / 2.0f);
    virgl_f(v, (far + near) / 2.0f);
}

void virgl_set_scissor(virgl *v, uint32_t minx, uint32_t miny, uint32_t maxx, uint32_t maxy)
{
    virgl_cmd(v, VIRGL_CCMD_SET_SCISSOR_STATE, 0, VIRGL_SET_SCISSOR_STATE_SIZE(1));
    virgl_dw(v, 0);
    virgl_dw(v, (minx & 0xFFFF) | (miny << 16));
    virgl_dw(v, (maxx & 0xFFFF) | (maxy << 16));
}

void virgl_clear(virgl *v, uint32_t buffers, const float rgba[4], double depth, uint32_t stencil)
{
    union { double d; uint32_t u[2]; } dd = { depth };
    virgl_cmd(v, VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
    virgl_dw(v, buffers);
    for (int i = 0; i < 4; i++) virgl_f(v, rgba[i]);
    virgl_dw(v, dd.u[0]);
    virgl_dw(v, dd.u[1]);
    virgl_dw(v, stencil);
}

void virgl_create_vertex_elements(virgl *v, uint32_t handle, uint32_t n, const uint32_t *src_offset,
                                  const uint32_t *buffer_index, const uint32_t *format)
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(n));
    virgl_dw(v, handle);
    for (uint32_t i = 0; i < n; i++) {
        virgl_dw(v, src_offset[i]);
        virgl_dw(v, 0);                                 /* instance divisor */
        virgl_dw(v, buffer_index[i]);
        virgl_dw(v, format[i]);
    }
}

void virgl_set_vertex_buffers(virgl *v, uint32_t n, const uint32_t *stride, const uint32_t *offset, const uint32_t *res)
{
    virgl_cmd(v, VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, VIRGL_SET_VERTEX_BUFFERS_SIZE(n));
    for (uint32_t i = 0; i < n; i++) {
        virgl_dw(v, stride[i]);
        virgl_dw(v, offset[i]);
        virgl_dw(v, res[i]);
    }
}

void virgl_set_index_buffer(virgl *v, uint32_t res, uint32_t index_size, uint32_t offset)
{
    virgl_cmd(v, VIRGL_CCMD_SET_INDEX_BUFFER, 0, VIRGL_SET_INDEX_BUFFER_SIZE(res));
    virgl_dw(v, res);
    if (res) {
        virgl_dw(v, index_size);
        virgl_dw(v, offset);
    }
}

void virgl_draw(virgl *v, uint32_t mode, uint32_t start, uint32_t count, int indexed, uint32_t min_index, uint32_t max_index)
{
    virgl_cmd(v, VIRGL_CCMD_DRAW_VBO, 0, VIRGL_DRAW_VBO_SIZE);
    virgl_dw(v, start);
    virgl_dw(v, count);
    virgl_dw(v, mode);
    virgl_dw(v, indexed ? 1 : 0);
    virgl_dw(v, 1);                                     /* instance count */
    virgl_dw(v, 0);                                     /* index bias */
    virgl_dw(v, 0);                                     /* start instance */
    virgl_dw(v, 0);                                     /* primitive restart */
    virgl_dw(v, 0);                                     /* restart index */
    virgl_dw(v, min_index);
    virgl_dw(v, max_index);
    virgl_dw(v, 0);                                     /* count from streamout */
}

void virgl_bind_object(virgl *v, uint32_t handle, uint32_t obj)
{
    virgl_cmd(v, VIRGL_CCMD_BIND_OBJECT, obj, 1);
    virgl_dw(v, handle);
}

void virgl_destroy_object(virgl *v, uint32_t handle, uint32_t obj)
{
    virgl_cmd(v, VIRGL_CCMD_DESTROY_OBJECT, obj, 1);
    virgl_dw(v, handle);
}

void virgl_set_constants(virgl *v, uint32_t shader_type, const float *data, uint32_t n)
{
    virgl_cmd(v, VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, 2 + n);
    virgl_dw(v, shader_type);
    virgl_dw(v, 0);
    for (uint32_t i = 0; i < n; i++) virgl_f(v, data[i]);
}

void virgl_create_sampler_view(virgl *v, uint32_t handle, uint32_t res, uint32_t target, uint32_t format, uint32_t first_level, uint32_t last_level, uint32_t swizzle)
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, VIRGL_OBJ_SAMPLER_VIEW_SIZE);
    virgl_dw(v, handle);
    virgl_dw(v, res);
    virgl_dw(v, (format & 0xFFFFFF) | (target << 24));
    virgl_dw(v, 0);                                     /* first layer .. last layer (16 bits each) */
    virgl_dw(v, (first_level & 0xFF) | ((last_level & 0xFF) << 8));
    virgl_dw(v, swizzle);
}

void virgl_set_sampler_views(virgl *v, uint32_t shader_type, uint32_t start, const uint32_t *handles, uint32_t n)
{
    virgl_cmd(v, VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, VIRGL_SET_SAMPLER_VIEWS_SIZE(n));
    virgl_dw(v, shader_type);
    virgl_dw(v, start);
    for (uint32_t i = 0; i < n; i++) virgl_dw(v, handles[i]);
}

void virgl_create_sampler_state(virgl *v, uint32_t handle, uint32_t wrap_s, uint32_t wrap_t, uint32_t min_img, uint32_t min_mip, uint32_t mag_img)
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, VIRGL_OBJ_SAMPLER_STATE_SIZE);
    virgl_dw(v, handle);
    virgl_dw(v, VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(wrap_s) | VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(wrap_t) |
                VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_R(wrap_s) | VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(min_img) |
                VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(min_mip) | VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(mag_img));
    virgl_f(v, 0.0f);                                   /* lod bias */
    virgl_f(v, 0.0f);                                   /* min lod */
    virgl_f(v, min_mip == PIPE_TEX_MIPFILTER_NONE ? 0.0f : 1000.0f);   /* max lod */
    for (int i = 0; i < 4; i++) virgl_f(v, 0.0f);       /* border colour */
}

void virgl_bind_sampler_states(virgl *v, uint32_t shader_type, uint32_t start, const uint32_t *handles, uint32_t n)
{
    virgl_cmd(v, VIRGL_CCMD_BIND_SAMPLER_STATES, 0, VIRGL_BIND_SAMPLER_STATES(n));
    virgl_dw(v, shader_type);
    virgl_dw(v, start);
    for (uint32_t i = 0; i < n; i++) virgl_dw(v, handles[i]);
}

void virgl_set_blend_color(virgl *v, const float rgba[4])
{
    virgl_cmd(v, VIRGL_CCMD_SET_BLEND_COLOR, 0, VIRGL_SET_BLEND_COLOR_SIZE);
    for (int i = 0; i < 4; i++) virgl_f(v, rgba[i]);
}

void virgl_set_stencil_ref(virgl *v, uint32_t front, uint32_t back)
{
    virgl_cmd(v, VIRGL_CCMD_SET_STENCIL_REF, 0, VIRGL_SET_STENCIL_REF_SIZE);
    virgl_dw(v, VIRGL_STENCIL_REF_VAL(front, back));
}

void virgl_resource_copy_region(virgl *v, uint32_t dst, uint32_t dst_level, uint32_t dx, uint32_t dy, uint32_t dz,
                                uint32_t src, uint32_t src_level, uint32_t sx, uint32_t sy, uint32_t sz, uint32_t w, uint32_t h, uint32_t d)
{
    virgl_cmd(v, VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    virgl_dw(v, dst); virgl_dw(v, dst_level); virgl_dw(v, dx); virgl_dw(v, dy); virgl_dw(v, dz);
    virgl_dw(v, src); virgl_dw(v, src_level); virgl_dw(v, sx); virgl_dw(v, sy); virgl_dw(v, sz);
    virgl_dw(v, w); virgl_dw(v, h); virgl_dw(v, d);
}

void virgl_inline_write(virgl *v, uint32_t res, uint32_t level, uint32_t usage, uint32_t stride, uint32_t layer_stride,
                        uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, const void *data, size_t bytes)
{
    uint32_t dw = (uint32_t)((bytes + 3) / 4);
    virgl_cmd(v, VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 11 + dw);
    virgl_dw(v, res); virgl_dw(v, level); virgl_dw(v, usage); virgl_dw(v, stride); virgl_dw(v, layer_stride);
    virgl_dw(v, x); virgl_dw(v, y); virgl_dw(v, z); virgl_dw(v, w); virgl_dw(v, h); virgl_dw(v, d);
    memset(v->buf + v->used, 0, dw * 4);
    memcpy(v->buf + v->used, data, bytes);
    v->used += dw;
}

void virgl_create_blend(virgl *v, uint32_t handle, const uint32_t dw[VIRGL_OBJ_BLEND_SIZE - 1])
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, VIRGL_OBJ_BLEND_SIZE);
    virgl_dw(v, handle);
    virgl_dws(v, dw, VIRGL_OBJ_BLEND_SIZE - 1);
}

void virgl_create_rasterizer(virgl *v, uint32_t handle, const uint32_t dw[VIRGL_OBJ_RS_SIZE - 1])
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, VIRGL_OBJ_RS_SIZE);
    virgl_dw(v, handle);
    virgl_dws(v, dw, VIRGL_OBJ_RS_SIZE - 1);
}

void virgl_create_dsa(virgl *v, uint32_t handle, const uint32_t dw[VIRGL_OBJ_DSA_SIZE - 1])
{
    virgl_cmd(v, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, VIRGL_OBJ_DSA_SIZE);
    virgl_dw(v, handle);
    virgl_dws(v, dw, VIRGL_OBJ_DSA_SIZE - 1);
}
