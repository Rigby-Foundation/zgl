/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The sic GL context ABI: what the window system glue (SDL's zwm driver)
 * needs from an OpenGL implementation. One context per window, rendering
 * into an offscreen colour + depth buffer of the window's size;
 * sic_gl_swap() returns the frame, top row first, 0x00RRGGBB.
 * libzgl (hardware, virgl) and libTinyGL (software) both provide it, so a
 * program picks one at link time. */
#ifndef SIC_GL_H
#define SIC_GL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sic_gl_context sic_gl_context;
sic_gl_context *sic_gl_create(int width, int height);            /* NULL: no GPU */
void  sic_gl_destroy(sic_gl_context *c);
void  sic_gl_make_current(sic_gl_context *c);
int   sic_gl_resize(sic_gl_context *c, int width, int height);
const uint32_t *sic_gl_swap(sic_gl_context *c, int *stride);   /* pixels, stride in pixels */
/* Where swaps should put the frame (width*height pixels, rows `stride`
 * apart), e.g. a buffer the window server maps. A hint: sic_gl_swap()
 * still returns where the frame is; it is the target when it could be. */
void  sic_gl_set_target(sic_gl_context *c, uint32_t *pix, int stride);
void *sic_gl_proc(const char *name);                            /* for SDL_GL_GetProcAddress */

/* Zero-copy windows, where the window server composites on the GPU: a
 * host resource that holds the last finished frame (0 if there is none,
 * e.g. a software implementation; a new one after sic_gl_resize), and
 * "the frame is done", which puts it there without reading it back. The
 * frame's rows are bottom-up (GL's convention). */
uint32_t sic_gl_buffer(sic_gl_context *c);
int   sic_gl_flush(sic_gl_context *c);
/* For the window server itself: the display shows this context's buffer;
 * present puts a rectangle of it on screen. -1 where not supported. */
int   sic_gl_scanout(sic_gl_context *c);
int   sic_gl_present(sic_gl_context *c, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
#endif
