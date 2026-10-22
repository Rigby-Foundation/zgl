/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* /dev/gpu0 on a desktop: what sic's virtio-gpu driver asks of the host,
 * done here by calling virglrenderer directly, the way QEMU's virtio-gpu
 * device does (same library, same calls), with macOS GL contexts (CGL)
 * standing in for QEMU's display.
 *
 * One process is one GPU context. A resource's guest backing is ordinary
 * memory attached to it as an iovec, so a transfer names an offset into it
 * exactly as on sic; a fenced submit polls until virglrenderer signals the
 * fence. Test tooling only: zgl on sic never sees this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/uio.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <virgl/virglrenderer.h>
#include <abi/gpu.h>
#include "virgl_hw.h"
#include "p_defines.h"
#include "r300/vrend.h"

#define CTX_ID 1

struct res { uint32_t id; void *backing; size_t size; struct iovec iov; struct res *next; };
static struct res *resources;
static uint32_t next_id = 1, last_fence;
static int initialized;
static CGLContextObj base_ctx;
static CGLContextObj ctxs[64]; static int nctxs;     /* every context virglrenderer has */

/* the host's glGetError: this program's own gl* names are zgl's */
static GLenum host_glGetError(void)
{
    static GLenum (*fn)(void);
    if (!fn) fn = (GLenum (*)(void))dlsym(dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY), "glGetError");
    return fn();
}

/* VHOST_DEBUG: a GL error left pending on the current context after `what` */
static void glcheck(const char *what)
{
    if (!getenv("VHOST_DEBUG")) return;
    for (GLenum e; (e = host_glGetError()) != GL_NO_ERROR; )
        fprintf(stderr, "vhost: GL error %#x pending after %s (context %p)\n", e, what, (void *)CGLGetCurrentContext());
}

static struct res *find(uint32_t id)
{
    for (struct res *r = resources; r; r = r->next) if (r->id == id) return r;
    return NULL;
}

/* ---- the GL contexts virglrenderer asks for ----------------------------------------- */

static void write_fence(void *cookie, uint32_t fence) { (void)cookie; last_fence = fence; }

static virgl_renderer_gl_context create_gl_context(void *cookie, int scanout, struct virgl_renderer_gl_ctx_param *param)
{
    (void)cookie; (void)scanout;
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated, (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pf; GLint n;
    if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) return NULL;
    CGLContextObj c = NULL;
    CGLCreateContext(pf, param->shared ? CGLGetCurrentContext() : NULL, &c);
    CGLDestroyPixelFormat(pf);
    if (c && nctxs < 64) ctxs[nctxs++] = c;
    return c;
}

static void destroy_gl_context(void *cookie, virgl_renderer_gl_context ctx)
{
    (void)cookie;
    for (int i = 0; i < nctxs; i++) if (ctxs[i] == ctx) ctxs[i] = ctxs[--nctxs];
    CGLDestroyContext(ctx);
}

/* drop GL errors left pending on any of the contexts */
static void drain_errors(void)
{
    CGLContextObj cur = CGLGetCurrentContext();
    for (int i = 0; i < nctxs; i++) {
        CGLSetCurrentContext(ctxs[i]);
        while (host_glGetError() != GL_NO_ERROR) ;
    }
    CGLSetCurrentContext(cur);
}
static int make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx) { (void)cookie; (void)scanout; return CGLSetCurrentContext(ctx) == kCGLNoError ? 0 : -1; }

static struct virgl_renderer_callbacks cbs = {
    .version = 1,
    .write_fence = write_fence,
    .create_gl_context = create_gl_context,
    .destroy_gl_context = destroy_gl_context,
    .make_current = make_current,
};

static int init(void)
{
    if (initialized) return 0;
    struct virgl_renderer_gl_ctx_param p = { .version = 1, .shared = false, .major_ver = 4, .minor_ver = 1 };
    base_ctx = create_gl_context(NULL, 0, &p);
    if (!base_ctx) { fprintf(stderr, "vhost: no OpenGL 4.1 core context\n"); return -1; }
    CGLSetCurrentContext(base_ctx);
    static int cookie;
    if (virgl_renderer_init(&cookie, 0, &cbs) != 0) { fprintf(stderr, "vhost: virgl_renderer_init failed\n"); return -1; }
    glcheck("virgl_renderer_init");
    static const char name[] = "zgl";
    if (virgl_renderer_context_create(CTX_ID, sizeof name - 1, name) != 0) { fprintf(stderr, "vhost: no context\n"); return -1; }
    glcheck("context_create");
    initialized = 1;
    return 0;
}

/* ---- the device file ---------------------------------------------------------------- */

int vt_open(const char *path, int flags)
{
    (void)path; (void)flags;
    return init() == 0 ? 3 : -1;
}

int vt_close(int fd) { (void)fd; return 0; }

void *vt_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd;
    struct res *r = find((uint32_t)(off / 4096));
    return r && r->backing ? r->backing : (void *)-1;
}

int vt_munmap(void *p, size_t len) { (void)p; (void)len; return 0; }      /* freed with the resource */

static int fence(void)
{
    static uint32_t id;
    id++;
    virgl_renderer_create_fence((int)id, CTX_ID);
    for (int spins = 0; last_fence < id; spins++) {
        virgl_renderer_poll();
        if (spins > 1000000) { fprintf(stderr, "vhost: fence %u never signalled\n", id); return -1; }
    }
    return 0;
}

int vt_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    switch (req) {
    case GPU_IOC_INFO: {
        struct gpu_info *i = arg;
        uint32_t ver, size;
        virgl_renderer_get_cap_set(2, &ver, &size);
        i->ctx_id = CTX_ID; i->capset_id = 2; i->capset_version = ver; i->capset_size = size;
        return 0;
    }
    case GPU_IOC_GET_CAPSET: {
        struct gpu_capset *cs = arg;
        uint32_t ver, size;
        virgl_renderer_get_cap_set(2, &ver, &size);
        char *buf = calloc(1, size > cs->size ? size : cs->size);
        virgl_renderer_fill_caps(2, ver, buf);
        /* fill_caps asks a core profile for GL_ALIASED_POINT_SIZE_RANGE, which
         * leaves GL_INVALID_ENUM pending; the next command would find it and
         * mark the guest's context lost for good. It runs on whichever
         * context vrend makes current, so clear them all */
        drain_errors();
        memcpy((void *)(uintptr_t)cs->buf, buf, cs->size);
        free(buf);
        return 0;
    }
    case GPU_IOC_CREATE_RES: {
        struct gpu_res_create *rc = arg;
        struct res *r = calloc(1, sizeof *r);
        r->id = next_id++;
        r->size = rc->size;
        struct virgl_renderer_resource_create_args a = {
            .handle = r->id, .target = rc->target, .format = rc->format, .bind = rc->bind,
            .width = rc->width, .height = rc->height, .depth = rc->depth, .array_size = rc->array_size ? rc->array_size : 1,
            .last_level = rc->last_level, .nr_samples = rc->nr_samples, .flags = rc->flags,
        };
        if (rc->size) { r->backing = calloc(1, rc->size); r->iov.iov_base = r->backing; r->iov.iov_len = rc->size; }
        if (virgl_renderer_resource_create(&a, NULL, 0) != 0) { free(r->backing); free(r); errno = EINVAL; return -1; }
        if (rc->size) virgl_renderer_resource_attach_iov((int)r->id, &r->iov, 1);    /* as QEMU does on ATTACH_BACKING */
        virgl_renderer_ctx_attach_resource(CTX_ID, (int)r->id);
        glcheck("resource_create");
        if (getenv("VHOST_DEBUG")) fprintf(stderr, "vhost: res %u target %u fmt %u bind %#x %ux%ux%u backing %u\n", r->id, rc->target, rc->format, rc->bind, rc->width, rc->height, rc->depth, rc->size);
        r->next = resources; resources = r;
        rc->id = r->id;
        return 0;
    }
    case GPU_IOC_DESTROY_RES: {
        uint32_t id = *(uint32_t *)arg;
        virgl_renderer_resource_unref(id);
        for (struct res **p = &resources; *p; p = &(*p)->next)
            if ((*p)->id == id) { struct res *r = *p; *p = r->next; free(r->backing); free(r); break; }
        return 0;
    }
    case GPU_IOC_TRANSFER_TO_HOST:
    case GPU_IOC_TRANSFER_FROM_HOST: {
        struct gpu_transfer *t = arg;
        struct virgl_box box = { t->x, t->y, t->z, t->w, t->h, t->d };
        if (getenv("VHOST_DEBUG")) fprintf(stderr, "vhost: %s res %u lvl %u box %u,%u,%u %ux%ux%u stride %u off %llu\n", req == GPU_IOC_TRANSFER_TO_HOST ? "put" : "get", t->res, t->level, t->x, t->y, t->z, t->w, t->h, t->d, t->stride, (unsigned long long)t->offset);
        int rc;
        glcheck("before transfer");
        if (req == GPU_IOC_TRANSFER_TO_HOST)
            rc = virgl_renderer_transfer_write_iov(t->res, CTX_ID, (int)t->level, t->stride, t->layer_stride, &box, t->offset, NULL, 0);
        else {
            fence();                                        /* what was drawn is in the host's memory */
            rc = virgl_renderer_transfer_read_iov(t->res, CTX_ID, t->level, t->stride, t->layer_stride, &box, t->offset, NULL, 0);
        }
        if (rc) { errno = EINVAL; return -1; }
        return 0;
    }
    case GPU_IOC_SUBMIT: {
        struct gpu_submit *sb = arg;
        glcheck("before submit");
        int rc = virgl_renderer_submit_cmd((void *)(uintptr_t)sb->buf, CTX_ID, (int)(sb->size / 4));
        glcheck("submit");
        if (sb->flags & GPU_SUBMIT_FENCE) fence();
        return rc ? -1 : 0;
    }
    case GPU_IOC_SET_SCANOUT: case GPU_IOC_FLUSH: return 0;          /* no display */
    case GPU_IOC_ATTACH_RES: case GPU_IOC_DETACH_RES: errno = ENOSYS; return -1;
    }
    errno = EINVAL;
    return -1;
}

/* no R300 on a desktop */
struct r300_vrend *r300_vrend_open(void) { return NULL; }
void r300_vrend_close(struct r300_vrend *r) { (void)r; }
void r300_vrend_caps(struct r300_vrend *r, struct virgl_caps_v2 *c) { (void)r; (void)c; }
uint32_t r300_vrend_res_create(struct r300_vrend *r, const struct gpu_res_create *rc) { (void)r; (void)rc; return 0; }
void r300_vrend_res_destroy(struct r300_vrend *r, uint32_t id) { (void)r; (void)id; }
int r300_vrend_res_attach(struct r300_vrend *r, uint32_t id, uint32_t w, uint32_t h, uint32_t f) { (void)r; (void)id; (void)w; (void)h; (void)f; return -1; }
int r300_vrend_scanout(struct r300_vrend *r, uint32_t id) { (void)r; (void)id; return 0; }
int r300_vrend_present(struct r300_vrend *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)r; (void)x; (void)y; (void)w; (void)h; return 0; }
int r300_vrend_transfer(struct r300_vrend *r, uint32_t id, int to_host, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t stride, void *map, uint64_t offset)
{ (void)r; (void)id; (void)to_host; (void)level; (void)x; (void)y; (void)w; (void)h; (void)stride; (void)map; (void)offset; return -1; }
int r300_vrend_submit(struct r300_vrend *r, const uint32_t *buf, uint32_t n) { (void)r; (void)buf; (void)n; return -1; }
int r300_vrend_finish(struct r300_vrend *r) { (void)r; return 0; }
