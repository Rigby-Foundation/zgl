/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* libadreno: a Qualcomm Adreno 6xx through sic's /dev/adrenogpu: buffers
 * the GPU sees at fixed addresses, and PM4 command streams the kernel runs
 * as indirect buffers. The 2D engine (blits, fills) is here too, after
 * Mesa's turnip (r2d_*). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ADRENO_CMD_DWORDS  (64u * 1024)     /* per submission */

struct adreno_hw {
    int fd;
    uint32_t chip_id, gmem_size, width, height;
    uint64_t fb_gpuaddr;                    /* the screen: B8G8R8A8 */
    uint32_t fb_pitch;
    uint32_t *cmd;                          /* the stream being built */
    uint32_t used;
    uint32_t fence;                         /* of the last submission */
};

struct adreno_bo {
    uint32_t handle, size;
    uint64_t gpuaddr;
    volatile uint8_t *map;                  /* uncached */
};

int  adreno_open(struct adreno_hw *hw);     /* -1 if there is no GPU */
void adreno_close(struct adreno_hw *hw);

int  adreno_bo_new(struct adreno_hw *hw, uint32_t size, struct adreno_bo *bo);
int  adreno_bo_open(struct adreno_hw *hw, uint32_t handle, struct adreno_bo *bo);   /* another program's */
void adreno_bo_free(struct adreno_hw *hw, struct adreno_bo *bo);

/* PM4: type 4 writes registers, type 7 is a packet. */
uint32_t adreno_parity(uint32_t v);
static inline uint32_t adreno_pkt4(uint32_t reg, uint32_t n) { return 0x40000000u | n | adreno_parity(n) << 7 | (reg & 0x3ffff) << 8 | adreno_parity(reg) << 27; }
static inline uint32_t adreno_pkt7(uint32_t op, uint32_t n) { return 0x70000000u | n | adreno_parity(n) << 15 | (op & 0x7f) << 16 | adreno_parity(op) << 23; }
static inline uint32_t adreno_space(struct adreno_hw *hw) { return ADRENO_CMD_DWORDS - hw->used; }
static inline void adreno_emit(struct adreno_hw *hw, uint32_t v) { if (hw->used < ADRENO_CMD_DWORDS) hw->cmd[hw->used++] = v; }
static inline void adreno_reg(struct adreno_hw *hw, uint32_t reg, uint32_t v) { adreno_emit(hw, adreno_pkt4(reg, 1)); adreno_emit(hw, v); }
uint32_t adreno_commit(struct adreno_hw *hw);
int  adreno_wait(struct adreno_hw *hw, uint32_t fence, int timeout_ms);
static inline int adreno_finish(struct adreno_hw *hw) { return adreno_wait(hw, adreno_commit(hw), 2000); }

/* The 2D engine, 32-bit B8G8R8A8 surfaces (base and pitch 64-byte aligned;
 * the _screen forms take the screen's odd pitch). Colours are 0xAARRGGBB. */
void adreno_2d_fill(struct adreno_hw *hw, uint64_t dst, uint32_t pitch, int x, int y, int w, int h, uint32_t argb);
void adreno_2d_fill_screen(struct adreno_hw *hw, int x, int y, int w, int h, uint32_t argb);
