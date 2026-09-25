/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* zgl extensions for a compositor: textures that are another program's
 * render target (zero copy), and fast uploads of 0x00RRGGBB pixels (what
 * the window system's software surfaces hold) into the bound texture. */
#pragma once
#include <GL/gl.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Texture `tex` becomes host resource `res` (another program's, w x h,
 * B8G8R8A8, rows bottom-up). Returns 0, or -1 if it could not be attached. */
int  zglTextureFromResource(GLuint tex, uint32_t res, int w, int h);
/* Rows of 0x00RRGGBB (alpha forced opaque) into level 0 of the bound
 * texture at (x, y); `stride` in pixels. */
void zglTexSubImageXRGB(GLint x, GLint y, GLsizei w, GLsizei h, const uint32_t *pixels, int stride);

#ifdef __cplusplus
}
#endif
