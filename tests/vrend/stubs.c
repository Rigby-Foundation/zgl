/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* virglrenderer's log sink, which vrend_shader.c's warnings and errors go
 * through (the rest of its logging layer is not built here). */
#include <stdio.h>
#include <stdarg.h>

void virgl_logv(int level, const char *fmt, va_list va)
{
    (void)level;
    vfprintf(stderr, fmt, va);
}
