/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* libr300: the kernel's GPU interface, /dev/radeongpu (sic's
 * include/abi/radeon.h). The kernel runs the command processor; here are
 * the buffers and the command stream. */
#include "r300_hw.h"
#include <abi/radeon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

uint64_t r300_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int r300_open(struct r300_hw *hw)
{
    memset(hw, 0, sizeof *hw);
    hw->fd = open("/dev/radeongpu", O_RDWR);
    if (hw->fd < 0) return -1;
    struct radeon_info in;
    if (ioctl(hw->fd, RADEON_IOC_INFO, &in) != 0) { close(hw->fd); return -1; }
    hw->vram_size = in.vram_size; hw->vram_mc = in.vram_mc;
    hw->width = in.width; hw->height = in.height; hw->chip = in.chip;
    hw->fence = in.fence;
    hw->cmd = malloc(R300_CMD_DWORDS * 4);
    if (!hw->cmd) { close(hw->fd); return -1; }
    return 0;
}

void r300_close(struct r300_hw *hw)
{
    if (hw->fd < 0) return;
    r300_finish(hw);
    close(hw->fd);
    free(hw->cmd);
    hw->fd = -1;
}

static int bo_map(struct r300_hw *hw, struct r300_bo *bo)
{
    void *p = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, hw->fd, bo->offset);
    if (p == MAP_FAILED) { ioctl(hw->fd, RADEON_IOC_BO_FREE, (unsigned long)bo->handle); return -1; }
    bo->map = p;
    return 0;
}

int r300_bo_new(struct r300_hw *hw, uint32_t size, struct r300_bo *bo)
{
    struct radeon_bo b = { 0, size, 0, 0 };
    if (ioctl(hw->fd, RADEON_IOC_BO_NEW, &b) != 0) return -1;
    bo->handle = b.handle; bo->size = b.size; bo->offset = b.offset;
    return bo_map(hw, bo);
}

int r300_bo_open(struct r300_hw *hw, uint32_t handle, struct r300_bo *bo)
{
    struct radeon_bo b = { handle, 0, 0, 0 };
    if (ioctl(hw->fd, RADEON_IOC_BO_OPEN, &b) != 0) return -1;
    bo->handle = handle; bo->size = b.size; bo->offset = b.offset;
    return bo_map(hw, bo);
}

void r300_bo_free(struct r300_hw *hw, struct r300_bo *bo)
{
    if (!bo->handle) return;
    if (bo->map) munmap((void *)bo->map, bo->size);
    ioctl(hw->fd, RADEON_IOC_BO_FREE, (unsigned long)bo->handle);
    memset(bo, 0, sizeof *bo);
}

int r300_scanout(struct r300_hw *hw, const struct r300_bo *bo, uint32_t pitch)
{
    struct radeon_scanout s = { bo ? bo->handle : 0, pitch };
    return ioctl(hw->fd, RADEON_IOC_SCANOUT, &s);
}

void r300_regs(struct r300_hw *hw, uint32_t reg, const uint32_t *v, unsigned n)
{
    r300_emit(hw, R300_PACKET0(reg, n - 1));
    for (unsigned i = 0; i < n; i++) r300_emit(hw, v[i]);
}

uint32_t r300_commit(struct r300_hw *hw)
{
    if (!hw->used) return hw->fence;
    struct radeon_submit s = { (uint64_t)(uintptr_t)hw->cmd, hw->used, 0 };
    if (ioctl(hw->fd, RADEON_IOC_SUBMIT, &s) != 0) fprintf(stderr, "r300: the kernel refused a submission (%u dwords)\n", hw->used);
    else hw->fence = s.fence;
    hw->used = 0;
    return hw->fence;
}

int r300_wait(struct r300_hw *hw, uint32_t fence, int timeout_ms)
{
    struct radeon_wait w = { fence, (uint32_t)timeout_ms };
    return ioctl(hw->fd, RADEON_IOC_WAIT, &w) == 0 ? 0 : -1;
}
