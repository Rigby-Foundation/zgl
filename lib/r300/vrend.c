/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The R300 renderer: resources, objects and the command stream. The draw
 * itself (vertex fetch, the vertex shader on the CPU, clipping, the
 * hardware state) is in vdraw.c. */
#include "vrend.h"
#include <abi/gpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---- resources ------------------------------------------------------------------ */

struct vr_res *vr_res_get(struct r300_vrend *v, uint32_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < v->nres; i++) if (v->res[i]->id == id) return v->res[i];
    return NULL;
}

static int res_add(struct r300_vrend *v, struct vr_res *r)
{
    if (v->nres >= VR_MAX_RES) return -1;
    v->res[v->nres++] = r;
    return 0;
}

static uint32_t next_pow2(uint32_t x) { uint32_t p = 1; while (p < x) p <<= 1; return p; }

/* The linear layout the texture unit expects (Mesa's r300_texture_desc):
 * rows of 8 texels at least, heights rounded to powers of two when there
 * are mip levels, the levels one after the other. Render targets get rows
 * of 16 pixels so the 2D engine can reach them too. */
static void layout(struct vr_res *r)
{
    uint32_t off = 0;
    int npot = (r->w & (r->w - 1)) || (r->h & (r->h - 1));
    r->npot = npot;
    if (npot) r->last_level = 0;                    /* R300 does not mip non-power-of-two textures */
    for (uint32_t l = 0; l <= r->last_level && l < VR_MAX_LEVELS; l++) {
        uint32_t w = r->w >> l, h = r->h >> l;
        if (!w) w = 1;
        if (!h) h = 1;
        uint32_t align = l == 0 && !r->last_level ? 16 : 8;
        uint32_t rows = r->last_level ? next_pow2(h) : h;
        r->stride[l] = ((w + align - 1) & ~(align - 1)) * 4;
        r->lh[l] = rows;
        r->offset[l] = off;
        off += r->stride[l] * rows;
    }
    r->vram_size = off;
}

uint32_t r300_vrend_res_create(struct r300_vrend *v, const struct gpu_res_create *rc)
{
    struct vr_res *r = calloc(1, sizeof *r);
    if (!r) return 0;
    r->target = rc->target; r->format = rc->format; r->bind = rc->bind;
    r->w = rc->width ? rc->width : 1; r->h = rc->height ? rc->height : 1; r->last_level = rc->last_level;
    if (rc->target == PIPE_BUFFER) {
        r->size = rc->width;
        r->host = calloc(1, r->size + 16);          /* slack: vertex fetch reads whole vec4s */
        if (!r->host) { free(r); return 0; }
        r->id = 0x40000000u | (++v->next_buffer_id & 0x3FFFFFFF);
    } else {
        if (rc->format != VIRGL_FORMAT_B8G8R8A8_UNORM && rc->format != VIRGL_FORMAT_B8G8R8X8_UNORM &&
            rc->format != VIRGL_FORMAT_S8_UINT_Z24_UNORM && rc->format != VIRGL_FORMAT_Z24X8_UNORM) {
            fprintf(stderr, "r300: resource format %u not supported\n", rc->format);
            free(r);
            return 0;
        }
        layout(r);
        if (r300_bo_new(&v->hw, r->vram_size, &r->bo)) { fprintf(stderr, "r300: out of video memory (%u KiB)\n", r->vram_size >> 10); free(r); return 0; }
        r->id = r->bo.handle;                       /* global: other programs can open it */
    }
    if (res_add(v, r)) { if (r->bo.handle) r300_bo_free(&v->hw, &r->bo); free(r->host); free(r); return 0; }
    return r->id;
}

/* Another program's render target (a compositor sampling a client's frame). */
int r300_vrend_res_attach(struct r300_vrend *v, uint32_t id, uint32_t w, uint32_t h, uint32_t format)
{
    struct vr_res *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->target = PIPE_TEXTURE_2D; r->format = format; r->w = w ? w : 1; r->h = h ? h : 1;
    layout(r);
    if (r300_bo_open(&v->hw, id, &r->bo) || r->bo.size < r->vram_size) { r300_bo_free(&v->hw, &r->bo); free(r); return -1; }
    r->id = id;
    r->foreign = 1;
    if (res_add(v, r)) { r300_bo_free(&v->hw, &r->bo); free(r); return -1; }
    return 0;
}

/* The panel shows a copy of the render target, updated by present(): the
 * program keeps drawing into its own buffer, so half-drawn frames (areas
 * cleared and not yet drawn again) never reach the screen. */
static void blit_rect(struct r300_vrend *v, uint32_t dst, uint32_t src, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

int r300_vrend_scanout(struct r300_vrend *v, uint32_t id)
{
    struct vr_res *r = vr_res_get(v, id);
    if (id && (!r || !r->bo.handle)) return -1;
    if (v->front) { vr_sync(v); r300_scanout(&v->hw, NULL, 0); r300_vrend_res_destroy(v, v->front); v->front = 0; }
    v->scan_src = 0;
    if (!id) { vr_sync(v); return r300_scanout(&v->hw, NULL, 0); }
    struct gpu_res_create rc = { .target = PIPE_TEXTURE_2D, .format = r->format, .bind = VIRGL_BIND_RENDER_TARGET, .width = r->w, .height = r->h, .depth = 1 };
    uint32_t f = r300_vrend_res_create(v, &rc);
    struct vr_res *fr = vr_res_get(v, f);
    if (!fr) return -1;
    v->front = f;
    v->scan_src = id;
    blit_rect(v, f, id, 0, 0, r->w, r->h);
    vr_sync(v);
    return r300_scanout(&v->hw, &fr->bo, fr->stride[0]);
}

int r300_vrend_present(struct r300_vrend *v, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct vr_res *r = vr_res_get(v, v->scan_src);
    if (!r || !v->front) return -1;
    if (x >= r->w || y >= r->h) return 0;
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    blit_rect(v, v->front, v->scan_src, x, y, w, h);
    vr_commit(v);
    return 0;
}

void r300_vrend_res_destroy(struct r300_vrend *v, uint32_t id)
{
    for (int i = 0; i < v->nres; i++)
        if (v->res[i]->id == id) {
            struct vr_res *r = v->res[i];
            if (r->bo.handle) { vr_commit(v); r300_bo_free(&v->hw, &r->bo); }   /* the kernel frees it once the GPU is done */
            free(r->host);
            free(r);
            v->res[i] = v->res[--v->nres];
            return;
        }
}

void vr_commit(struct r300_vrend *v)
{
    if (!v->hw.used) return;
    v->fences[v->serial % 256] = r300_commit(&v->hw);
    v->serial++;
}

void vr_sync(struct r300_vrend *v)
{
    vr_commit(v);
    if (!v->busy) return;
    r300_wait(&v->hw, v->hw.fence, 2000);
    v->busy = 0;
}

/* Only the work that used this resource has to be done, not all of it. */
void vr_wait_res(struct r300_vrend *v, struct vr_res *r)
{
    if (!r->last_use) return;
    if (r->last_use >= v->serial) vr_commit(v);
    if (v->serial - r->last_use > 250) { vr_sync(v); return; }
    r300_wait(&v->hw, v->fences[r->last_use % 256], 2000);
}

int r300_vrend_finish(struct r300_vrend *v) { vr_sync(v); return 0; }

/* Between the guest backing and the resource. Textures live in VRAM,
 * which the CPU reaches uncached: fine for writes, slow for reads. */
int r300_vrend_transfer(struct r300_vrend *v, uint32_t id, int to_host, uint32_t level, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t stride, void *guest, uint64_t offset)
{
    struct vr_res *r = vr_res_get(v, id);
    if (!r || !guest) return -1;
    if (r->host) {
        if (x + w > r->size) return -1;
        if (to_host) memcpy(r->host + x, (uint8_t *)guest + offset, w);
        else memcpy((uint8_t *)guest + offset, r->host + x, w);
        return 0;
    }
    if (level > r->last_level || x + w > (r->w >> level ? r->w >> level : 1) + 0u || y + h > r->lh[level]) return -1;
    vr_wait_res(v, r);
    volatile uint8_t *base = r->bo.map + r->offset[level];
    for (uint32_t j = 0; j < h; j++) {
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)(y + j) * r->stride[level]) + x;
        uint32_t *g = (uint32_t *)((uint8_t *)guest + offset + (size_t)j * stride);
        if (to_host) for (uint32_t i = 0; i < w; i++) row[i] = g[i];
        else for (uint32_t i = 0; i < w; i++) g[i] = row[i];
    }
    return 0;
}

/* ---- objects -------------------------------------------------------------------- */

static struct vr_obj *obj_get(struct r300_vrend *v, uint32_t handle, int type)
{
    for (int i = 0; i < v->nobj; i++)
        if (v->obj[i]->handle == handle && (!type || v->obj[i]->type == type)) return v->obj[i];
    return NULL;
}

static void obj_destroy(struct r300_vrend *v, uint32_t handle)
{
    for (int i = 0; i < v->nobj; i++)
        if (v->obj[i]->handle == handle) {
            struct vr_obj *o = v->obj[i];
            if (o->type == OBJ_SHADER) {
                if (v->vs == o->u.shader) v->vs = NULL;
                if (v->fs == o->u.shader) v->fs = NULL;
                tgsi_free(&o->u.shader->t);
                free(o->u.shader);
            }
            free(o);
            v->obj[i] = v->obj[--v->nobj];
            return;
        }
}

static struct vr_obj *obj_new(struct r300_vrend *v, uint32_t handle, int type)
{
    obj_destroy(v, handle);
    if (v->nobj >= VR_MAX_OBJ) return NULL;
    struct vr_obj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->handle = handle;
    o->type = type;
    v->obj[v->nobj++] = o;
    return o;
}

/* A fragment program that shows it is a stand-in: flat magenta. */
static const char *fallback_fs = "FRAG\nDCL OUT[0], COLOR\nIMM FLT32 {1, 0, 1, 1}\n  0: MOV OUT[0], IMM[0]\n  1: END\n";

struct vr_shader *vr_shader_from_text(const char *tgsi)
{
    struct vr_shader *s = calloc(1, sizeof *s);
    char err[256];
    if (!s) return NULL;
    if (tgsi_parse(tgsi, &s->t, err, sizeof err)) {
        fprintf(stderr, "r300: %s\n", err);
        free(s);
        return NULL;
    }
    s->frag = s->t.frag;
    s->ok = 1;
    if (s->frag && r300_fs_compile(&s->t, &s->fs, err, sizeof err)) {
        fprintf(stderr, "r300: %s; drawing it magenta\n", err);
        struct tgsi_shader fb;
        tgsi_parse(fallback_fs, &fb, err, sizeof err);
        r300_fs_compile(&fb, &s->fs, err, sizeof err);
        tgsi_free(&fb);
        s->fs.ninputs = 0;
        s->ok = 0;
    }
    return s;
}

/* Shaders arrive whole (zgl's fit one command) or in continuation pieces. */
static char *shader_text; static uint32_t shader_total, shader_have;

static void create_object(struct r300_vrend *v, int type, const uint32_t *d, uint32_t len)
{
    uint32_t h = d[0];
    struct vr_obj *o;
    switch (type) {
    case VIRGL_OBJECT_BLEND:
    case VIRGL_OBJECT_RASTERIZER:
    case VIRGL_OBJECT_DSA:
        o = obj_new(v, h, type == VIRGL_OBJECT_BLEND ? OBJ_BLEND : type == VIRGL_OBJECT_RASTERIZER ? OBJ_RS : OBJ_DSA);
        if (o) memcpy(o->u.dw, d + 1, (len - 1 < 12 ? len - 1 : 12) * 4);
        break;
    case VIRGL_OBJECT_SHADER: {
        uint32_t off = d[2];
        const char *text = (const char *)(d + 5);
        size_t n = (len - 5) * 4;
        if (!(off & VIRGL_OBJ_SHADER_OFFSET_CONT)) {
            free(shader_text);
            shader_total = off;
            shader_text = calloc(1, shader_total + 4);
            shader_have = 0;
        }
        if (!shader_text) break;
        if (shader_have + n > shader_total) n = shader_total - shader_have;
        memcpy(shader_text + shader_have, text, n);
        shader_have += (uint32_t)n;
        if (shader_have < shader_total) break;
        o = obj_new(v, h, OBJ_SHADER);
        if (o) o->u.shader = vr_shader_from_text(shader_text);
        if (o && !o->u.shader) obj_destroy(v, h);
        free(shader_text);
        shader_text = NULL;
        break;
    }
    case VIRGL_OBJECT_VERTEX_ELEMENTS: {
        o = obj_new(v, h, OBJ_VE);
        if (!o) break;
        int n = (int)(len - 1) / 4;
        if (n > VR_MAX_VE) n = VR_MAX_VE;
        o->u.ve.n = n;
        for (int i = 0; i < n; i++) {
            o->u.ve.offset[i] = d[1 + i * 4];
            o->u.ve.vb[i] = d[3 + i * 4];
            o->u.ve.format[i] = d[4 + i * 4];
        }
        break;
    }
    case VIRGL_OBJECT_SAMPLER_VIEW:
        o = obj_new(v, h, OBJ_VIEW);
        if (!o) break;
        o->u.view.res = d[1];
        o->u.view.format = d[2] & 0xFFFFFF;
        o->u.view.first_level = d[4] & 0xFF;
        o->u.view.last_level = (d[4] >> 8) & 0xFF;
        break;
    case VIRGL_OBJECT_SAMPLER_STATE: {
        o = obj_new(v, h, OBJ_SAMPLER);
        if (!o) break;
        union { uint32_t u; float f; } a = { d[3] }, b = { d[4] };
        o->u.sampler.s0 = d[1];
        o->u.sampler.min_lod = a.f;
        o->u.sampler.max_lod = b.f;
        break;
    }
    case VIRGL_OBJECT_SURFACE:
        o = obj_new(v, h, OBJ_SURFACE);
        if (!o) break;
        o->u.surf.res = d[1];
        o->u.surf.format = d[2];
        o->u.surf.level = d[3];
        break;
    default:
        break;
    }
}

static void bind_object(struct r300_vrend *v, int type, uint32_t h)
{
    int t = type == VIRGL_OBJECT_BLEND ? OBJ_BLEND : type == VIRGL_OBJECT_RASTERIZER ? OBJ_RS : type == VIRGL_OBJECT_DSA ? OBJ_DSA :
            type == VIRGL_OBJECT_VERTEX_ELEMENTS ? OBJ_VE : 0;
    struct vr_obj *o = h ? obj_get(v, h, t) : NULL;
    switch (t) {
    case OBJ_BLEND: v->have_blend = o != NULL; if (o) memcpy(v->blend, o->u.dw, sizeof v->blend); break;
    case OBJ_RS: v->have_rs = o != NULL; if (o) memcpy(v->rs, o->u.dw, sizeof v->rs); break;
    case OBJ_DSA: v->have_dsa = o != NULL; if (o) memcpy(v->dsa, o->u.dw, sizeof v->dsa); break;
    case OBJ_VE: if (o) v->ve = o->u.ve; else v->ve.n = 0; break;
    }
}

/* ---- clears and blits: quads through the 3D pipe ------------------------------ */

static const char *clear_fs_text = "FRAG\nDCL OUT[0], COLOR\nDCL CONST[0]\n  0: MOV OUT[0], CONST[0]\n  1: END\n";
static const char *blit_fs_text =
    "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\n"
    "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n  1: END\n";

static void do_clear(struct r300_vrend *v, const uint32_t *d)
{
    uint32_t buffers = d[0];
    union { uint32_t u[2]; double dd; } depth = { { d[5], d[6] } };
    union { uint32_t u; float f; } c[4] = { { d[1] }, { d[2] }, { d[3] }, { d[4] } };
    if (!v->clear_fs) v->clear_fs = vr_shader_from_text(clear_fs_text);
    int color = (buffers & PIPE_CLEAR_COLOR0) && v->have_cbuf, zs = (buffers & PIPE_CLEAR_DEPTHSTENCIL) && v->have_zbuf;
    if (!color && !zs) return;
    for (int i = 0; i < 4; i++) v->fs_const[0][i] = c[i].f;
    v->nfs_const = 1;
    struct vr_res *r = vr_res_get(v, v->have_cbuf ? v->cbuf.res : v->zbuf.res);
    if (!r) return;
    uint32_t lv = v->have_cbuf ? v->cbuf.level : 0;
    float w = (float)(r->w >> lv ? r->w >> lv : 1), h = (float)(r->h >> lv ? r->h >> lv : 1);
    /* the scissor (when the rasterizer has it on) is applied by the hardware: GL clears honour it */
    vr_draw_quad(v, v->clear_fs, 0, 0, w, h, (float)depth.dd, 0, 0, 1, 1, color ? 0xF : 0, zs);
}

/* BLIT: dst res, level, format, box (x y z w h d), src res, level, format, box. */
static void do_blit(struct r300_vrend *v, const uint32_t *d)
{
    uint32_t filter = (d[0] >> 8) & 3;
    struct vr_res *dst = vr_res_get(v, d[3]), *src = vr_res_get(v, d[12]);
    if (!dst || !src || !dst->bo.handle || !src->bo.handle) return;
    if (!v->blit_fs) v->blit_fs = vr_shader_from_text(blit_fs_text);
    /* borrow the framebuffer, the sampler state and the scissor */
    struct vr_surface cb = v->cbuf, zb = v->zbuf;
    int hc = v->have_cbuf, hz = v->have_zbuf;
    struct vr_view view0 = v->views[0];
    struct vr_sampler samp0 = v->samplers[0];
    int nviews = v->nviews, nsamp = v->nsamplers;
    uint32_t sc[4] = { v->sc_minx, v->sc_miny, v->sc_maxx, v->sc_maxy };
    v->cbuf.res = dst->id; v->cbuf.level = d[4]; v->cbuf.format = d[5]; v->have_cbuf = 1; v->have_zbuf = 0;
    v->views[0].res = src->id; v->views[0].first_level = v->views[0].last_level = d[13]; v->views[0].format = d[14];
    v->samplers[0].s0 = VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(PIPE_TEX_WRAP_CLAMP_TO_EDGE) | VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(filter) | VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(filter) |
                        VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(PIPE_TEX_MIPFILTER_NONE);
    v->samplers[0].min_lod = v->samplers[0].max_lod = (float)d[13];
    v->nviews = v->nsamplers = 1;
    uint32_t sl = d[13], sw = src->w >> sl ? src->w >> sl : 1, sh = src->h >> sl ? src->h >> sl : 1;
    float dx = (float)d[6], dy = (float)d[7], dw = (float)d[9], dh = (float)d[10];
    float sx = (float)d[15], sy = (float)d[16], sw_ = (float)d[18], sh_ = (float)d[19];
    uint32_t dl = d[4], tw = dst->w >> dl ? dst->w >> dl : 1, th = dst->h >> dl ? dst->h >> dl : 1;
    v->sc_minx = 0; v->sc_miny = 0; v->sc_maxx = tw; v->sc_maxy = th;
    vr_draw_quad(v, v->blit_fs, dx, dy, dx + dw, dy + dh, 0, sx / (float)sw, sy / (float)sh, (sx + sw_) / (float)sw, (sy + sh_) / (float)sh, 0xF, 0);
    v->cbuf = cb; v->zbuf = zb; v->have_cbuf = hc; v->have_zbuf = hz;
    v->views[0] = view0; v->samplers[0] = samp0; v->nviews = nviews; v->nsamplers = nsamp;
    v->sc_minx = sc[0]; v->sc_miny = sc[1]; v->sc_maxx = sc[2]; v->sc_maxy = sc[3];
}

static void blit_rect(struct r300_vrend *v, uint32_t dst, uint32_t src, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct vr_res *d = vr_res_get(v, dst), *sr = vr_res_get(v, src);
    if (!d || !sr) return;
    uint32_t b[22] = { VIRGL_CMD_BLIT_S0_MASK(0xF), 0, 0, dst, 0, d->format, x, y, 0, w, h, 1, src, 0, sr->format, x, y, 0, w, h, 1, 0 };
    do_blit(v, b);
}

static void copy_region(struct r300_vrend *v, const uint32_t *d)
{
    struct vr_res *dst = vr_res_get(v, d[0]), *src = vr_res_get(v, d[5]);
    if (!dst || !src) return;
    if (dst->host && src->host) {                   /* buffers */
        uint32_t w = d[10];
        if (d[2] + w <= dst->size && d[7] + w <= src->size) memmove(dst->host + d[2], src->host + d[7], w);
        return;
    }
    /* textures: a nearest blit of the same size */
    uint32_t b[22] = { VIRGL_CMD_BLIT_S0_MASK(0xF), 0, 0,
                       d[0], d[1], dst->format, d[2], d[3], d[4], d[10], d[11], d[12],
                       d[5], d[6], src->format, d[7], d[8], d[9], d[10], d[11], d[12], 0 };
    do_blit(v, b);
}

static void inline_write(struct r300_vrend *v, const uint32_t *d, uint32_t len)
{
    /* res, level, usage, stride, layer_stride, x, y, z, w, h, d, data */
    struct vr_res *r = vr_res_get(v, d[0]);
    if (!r) return;
    if (r->host) {
        uint32_t x = d[5], w = d[8];
        if (x + w <= r->size && w <= (len - 11) * 4) memcpy(r->host + x, d + 11, w);
        return;
    }
    r300_vrend_transfer(v, d[0], 1, d[1], d[5], d[6], d[8], d[9], d[3], (void *)(d + 11), 0);
}

/* ---- the command stream ----------------------------------------------------------- */

int r300_vrend_submit(struct r300_vrend *v, const uint32_t *buf, uint32_t dwords)
{
    for (uint32_t pos = 0; pos < dwords;) {
        uint32_t hdr = buf[pos];
        uint32_t cmd = hdr & 0xFF, obj = (hdr >> 8) & 0xFF, len = hdr >> 16;
        const uint32_t *d = buf + pos + 1;
        if (pos + 1 + len > dwords) break;
        pos += 1 + len;
        switch (cmd) {
        case VIRGL_CCMD_NOP: break;
        case VIRGL_CCMD_CREATE_OBJECT: create_object(v, (int)obj, d, len); break;
        case VIRGL_CCMD_BIND_OBJECT: bind_object(v, (int)obj, d[0]); break;
        case VIRGL_CCMD_DESTROY_OBJECT: obj_destroy(v, d[0]); break;
        case VIRGL_CCMD_BIND_SHADER: {
            struct vr_obj *o = d[0] ? obj_get(v, d[0], OBJ_SHADER) : NULL;
            if (d[1] == PIPE_SHADER_VERTEX) v->vs = o ? o->u.shader : NULL;
            else if (d[1] == PIPE_SHADER_FRAGMENT) v->fs = o ? o->u.shader : NULL;
            break;
        }
        case VIRGL_CCMD_SET_VIEWPORT_STATE: {
            union { uint32_t u; float f; } f[6];
            for (int i = 0; i < 6; i++) f[i].u = d[1 + i];
            for (int i = 0; i < 3; i++) { v->vp_scale[i] = f[i].f; v->vp_trans[i] = f[3 + i].f; }
            break;
        }
        case VIRGL_CCMD_SET_FRAMEBUFFER_STATE: {
            uint32_t nr = d[0], zs = d[1];
            struct vr_obj *z = zs ? obj_get(v, zs, OBJ_SURFACE) : NULL, *c = nr ? obj_get(v, d[2], OBJ_SURFACE) : NULL;
            v->have_zbuf = z != NULL; if (z) v->zbuf = z->u.surf;
            v->have_cbuf = c != NULL; if (c) v->cbuf = c->u.surf;
            break;
        }
        case VIRGL_CCMD_SET_SCISSOR_STATE:
            v->sc_minx = d[1] & 0xFFFF; v->sc_miny = d[1] >> 16; v->sc_maxx = d[2] & 0xFFFF; v->sc_maxy = d[2] >> 16;
            break;
        case VIRGL_CCMD_SET_VERTEX_BUFFERS:
            v->nvb = (int)(len / 3) < VR_MAX_VB ? (int)(len / 3) : VR_MAX_VB;
            for (int i = 0; i < v->nvb; i++) { v->vb[i].stride = d[i * 3]; v->vb[i].offset = d[i * 3 + 1]; v->vb[i].res = d[i * 3 + 2]; }
            break;
        case VIRGL_CCMD_SET_INDEX_BUFFER:
            v->ib.res = d[0];
            v->ib.size = len > 1 ? d[1] : 0;
            v->ib.offset = len > 2 ? d[2] : 0;
            break;
        case VIRGL_CCMD_SET_CONSTANT_BUFFER: {
            uint32_t n = len - 2;
            if (d[0] == PIPE_SHADER_VERTEX) {
                if (n > 256 * 4) n = 256 * 4;
                memcpy(v->vs_const, d + 2, n * 4);
                v->nvs_const = (int)(n + 3) / 4;
            } else if (d[0] == PIPE_SHADER_FRAGMENT) {
                if (n > 32 * 4) n = 32 * 4;
                memcpy(v->fs_const, d + 2, n * 4);
                v->nfs_const = (int)(n + 3) / 4;
            }
            break;
        }
        case VIRGL_CCMD_SET_SAMPLER_VIEWS:
            if (d[0] != PIPE_SHADER_FRAGMENT) break;
            for (uint32_t i = 0; i + 2 < len && d[1] + i < VR_MAX_UNITS; i++) {
                struct vr_obj *o = d[2 + i] ? obj_get(v, d[2 + i], OBJ_VIEW) : NULL;
                if (o) v->views[d[1] + i] = o->u.view; else memset(&v->views[d[1] + i], 0, sizeof v->views[0]);
                if ((int)(d[1] + i + 1) > v->nviews) v->nviews = (int)(d[1] + i + 1);
            }
            break;
        case VIRGL_CCMD_BIND_SAMPLER_STATES:
            if (d[0] != PIPE_SHADER_FRAGMENT) break;
            for (uint32_t i = 0; i + 2 < len && d[1] + i < VR_MAX_UNITS; i++) {
                struct vr_obj *o = d[2 + i] ? obj_get(v, d[2 + i], OBJ_SAMPLER) : NULL;
                if (o) v->samplers[d[1] + i] = o->u.sampler;
                if ((int)(d[1] + i + 1) > v->nsamplers) v->nsamplers = (int)(d[1] + i + 1);
            }
            break;
        case VIRGL_CCMD_SET_BLEND_COLOR:
            for (int i = 0; i < 4; i++) { union { uint32_t u; float f; } f = { d[i] }; v->blend_color[i] = f.f; }
            break;
        case VIRGL_CCMD_SET_STENCIL_REF: v->stencil_ref = d[0]; break;
        case VIRGL_CCMD_CLEAR: do_clear(v, d); break;
        case VIRGL_CCMD_DRAW_VBO: vr_draw_vbo(v, d); break;
        case VIRGL_CCMD_RESOURCE_INLINE_WRITE: inline_write(v, d, len); break;
        case VIRGL_CCMD_RESOURCE_COPY_REGION: copy_region(v, d); break;
        case VIRGL_CCMD_BLIT: do_blit(v, d); break;
        default:
            if (v->warned < 8) { fprintf(stderr, "r300: command %u not supported\n", cmd); v->warned++; }
            break;
        }
    }
    vr_commit(v);
    return 0;
}

/* ---- setup ------------------------------------------------------------------------ */

struct r300_vrend *r300_vrend_open(void)
{
    struct r300_vrend *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    if (r300_open(&v->hw)) { free(v); return NULL; }
    v->serial = 1;
    return v;
}

void r300_vrend_close(struct r300_vrend *v)
{
    if (!v) return;
    vr_sync(v);
    while (v->nres) r300_vrend_res_destroy(v, v->res[0]->id);
    while (v->nobj) obj_destroy(v, v->obj[0]->handle);
    r300_close(&v->hw);
    free(v);
}

void r300_vrend_caps(struct r300_vrend *v, struct virgl_caps_v2 *caps)
{
    memset(caps, 0, sizeof *caps);
    caps->v1.max_version = 2;
    caps->v1.glsl_level = 120;
    caps->v1.max_render_targets = 1;
    uint32_t fmts[] = { VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_FORMAT_B8G8R8X8_UNORM };
    for (unsigned i = 0; i < 2; i++) {
        caps->v1.sampler.bitmask[fmts[i] / 32] |= 1u << (fmts[i] % 32);
        caps->v1.render.bitmask[fmts[i] / 32] |= 1u << (fmts[i] % 32);
    }
    caps->v1.depthstencil.bitmask[VIRGL_FORMAT_S8_UINT_Z24_UNORM / 32] |= 1u << (VIRGL_FORMAT_S8_UINT_Z24_UNORM % 32);
    caps->max_texture_2d_size = 2048;
    snprintf(caps->renderer, sizeof caps->renderer, "ATI Radeon %04x (R300 IGP, %u MiB)", v->hw.chip, v->hw.vram_size >> 20);
}
