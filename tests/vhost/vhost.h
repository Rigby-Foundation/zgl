/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The host build of libvirgl (zgl/tests/vhost): /dev/gpu0's operations done
 * by virglrenderer linked into the program, as QEMU's virtio-gpu does them,
 * so the unchanged virgl.c and libzgl run on a desktop. Included by virgl.c
 * after its system headers when ZGL_VHOST is defined. */
#pragma once
#include <stddef.h>
#include <sys/types.h>

int   vt_open(const char *path, int flags);
int   vt_close(int fd);
int   vt_ioctl(int fd, unsigned long req, void *arg);
void *vt_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   vt_munmap(void *p, size_t len);

#define open(p, f) vt_open(p, f)
#define close(fd) vt_close(fd)
#define ioctl(fd, req, arg) vt_ioctl(fd, req, arg)
#define mmap(a, l, p, f, fd, o) vt_mmap(a, l, p, f, fd, o)
#define munmap(p, l) vt_munmap(p, l)
