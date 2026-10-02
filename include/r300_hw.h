/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* libr300: an R300-class Radeon IGP (Xpress 200M/1100/1150, RS400/RS480)
 * through sic's /dev/radeongpu: buffers in the IGP's memory, and command
 * streams the kernel checks and runs on the chip's command processor.
 * The 3D driver of zgl (lib/r300) sits on top of this. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define R300_PACKET0(reg, n)   (((uint32_t)(n) << 16) | ((reg) >> 2))   /* n+1 registers from reg */
#define R300_PACKET3(op, n)    (0xC0000000u | ((uint32_t)(n) << 16) | (op))  /* op already << 8 */
#define R300_PACKET2           0x80000000u

#define R300_CMD_DWORDS  (64u * 1024)       /* per submission */

struct r300_hw {
    int fd;
    uint32_t vram_size, vram_mc, width, height, chip;
    uint32_t *cmd;                          /* the stream being built */
    uint32_t used;
    uint32_t fence;                         /* of the last submission */
};

struct r300_bo {
    uint32_t handle, size, offset;          /* offset in VRAM: the GPU sees vram_mc + offset */
    volatile uint8_t *map;                  /* write-combining */
};

int  r300_open(struct r300_hw *hw);         /* -1 (and why, on stderr) if there is no GPU */
void r300_close(struct r300_hw *hw);

int  r300_bo_new(struct r300_hw *hw, uint32_t size, struct r300_bo *bo);
int  r300_bo_open(struct r300_hw *hw, uint32_t handle, struct r300_bo *bo);   /* another program's */
void r300_bo_free(struct r300_hw *hw, struct r300_bo *bo);
int  r300_scanout(struct r300_hw *hw, const struct r300_bo *bo, uint32_t pitch);   /* NULL: the console */

/* The stream: dwords, whole packets. r300_space() says how much fits
 * before a submission must be made; r300_commit() submits (and returns
 * its fence); r300_wait() waits for a fence. */
static inline uint32_t r300_space(struct r300_hw *hw) { return R300_CMD_DWORDS - hw->used; }
static inline void r300_emit(struct r300_hw *hw, uint32_t v) { if (hw->used < R300_CMD_DWORDS) hw->cmd[hw->used++] = v; }
static inline void r300_emit_f(struct r300_hw *hw, float f) { union { float f; uint32_t u; } c = { f }; r300_emit(hw, c.u); }
static inline void r300_reg(struct r300_hw *hw, uint32_t reg, uint32_t v) { r300_emit(hw, R300_PACKET0(reg, 0)); r300_emit(hw, v); }
void r300_regs(struct r300_hw *hw, uint32_t reg, const uint32_t *v, unsigned n);
uint32_t r300_commit(struct r300_hw *hw);
int  r300_wait(struct r300_hw *hw, uint32_t fence, int timeout_ms);
static inline int r300_finish(struct r300_hw *hw) { return r300_wait(hw, r300_commit(hw), 2000); }

uint64_t r300_now_us(void);
