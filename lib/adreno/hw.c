/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* libadreno: /dev/adrenogpu (sic's include/abi/adreno.h). */
#include "adreno_hw.h"
#include <abi/adreno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

uint32_t adreno_parity(uint32_t v)
{
    return (0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28))) & 1;
}

int adreno_open(struct adreno_hw *hw)
{
    memset(hw, 0, sizeof *hw);
    hw->fd = open("/dev/adrenogpu", O_RDWR);
    if (hw->fd < 0) return -1;
    struct adreno_info in;
    if (ioctl(hw->fd, ADRENO_IOC_INFO, &in) != 0) { close(hw->fd); return -1; }
    hw->chip_id = in.chip_id; hw->gmem_size = in.gmem_size;
    hw->width = in.width; hw->height = in.height;
    hw->fb_gpuaddr = in.fb_gpuaddr; hw->fb_pitch = in.fb_pitch;
    hw->fence = in.fence;
    hw->cmd = malloc(ADRENO_CMD_DWORDS * 4);
    if (!hw->cmd) { close(hw->fd); return -1; }
    return 0;
}

void adreno_close(struct adreno_hw *hw)
{
    if (hw->fd < 0) return;
    adreno_finish(hw);
    close(hw->fd);
    free(hw->cmd);
    hw->fd = -1;
}

static int bo_map(struct adreno_hw *hw, struct adreno_bo *bo, uint64_t offset)
{
    void *p = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, hw->fd, (off_t)offset);
    if (p == MAP_FAILED) { ioctl(hw->fd, ADRENO_IOC_BO_FREE, (unsigned long)bo->handle); return -1; }
    bo->map = p;
    return 0;
}

int adreno_bo_new(struct adreno_hw *hw, uint32_t size, struct adreno_bo *bo)
{
    struct adreno_bo_info b = { 0, size, 0, 0 };
    if (ioctl(hw->fd, ADRENO_IOC_BO_NEW, &b) != 0) return -1;
    bo->handle = b.handle; bo->size = b.size; bo->gpuaddr = b.gpuaddr;
    return bo_map(hw, bo, b.offset);
}

int adreno_bo_open(struct adreno_hw *hw, uint32_t handle, struct adreno_bo *bo)
{
    struct adreno_bo_info b = { handle, 0, 0, 0 };
    if (ioctl(hw->fd, ADRENO_IOC_BO_OPEN, &b) != 0) return -1;
    bo->handle = handle; bo->size = b.size; bo->gpuaddr = b.gpuaddr;
    return bo_map(hw, bo, b.offset);
}

void adreno_bo_free(struct adreno_hw *hw, struct adreno_bo *bo)
{
    if (!bo->handle) return;
    if (bo->map) munmap((void *)bo->map, bo->size);
    ioctl(hw->fd, ADRENO_IOC_BO_FREE, (unsigned long)bo->handle);
    memset(bo, 0, sizeof *bo);
}

uint32_t adreno_commit(struct adreno_hw *hw)
{
    if (!hw->used) return hw->fence;
    struct adreno_submit s = { (uint64_t)(uintptr_t)hw->cmd, hw->used, 0 };
    if (ioctl(hw->fd, ADRENO_IOC_SUBMIT, &s) != 0) fprintf(stderr, "adreno: the kernel refused a submission (%u dwords)\n", hw->used);
    else hw->fence = s.fence;
    hw->used = 0;
    return hw->fence;
}

int adreno_wait(struct adreno_hw *hw, uint32_t fence, int timeout_ms)
{
    struct adreno_wait w = { fence, (uint32_t)timeout_ms };
    return ioctl(hw->fd, ADRENO_IOC_WAIT, &w) == 0 ? 0 : -1;
}
