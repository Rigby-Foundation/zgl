/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The 2D engine of an Adreno 6xx: solid fills, after Mesa's turnip
 * (tu_clear_blit.cc, r2d_*). Register offsets from Mesa's a6xx.xml. */
#include "adreno_hw.h"

#define GRAS_A2D_BLT_CNTL       0x8400
#define GRAS_A2D_DEST_TL        0x8405
#define RB_A2D_BLT_CNTL         0x8c00
#define RB_A2D_PIXEL_CNTL       0x8c01
#define RB_A2D_DEST_BUFFER_INFO 0x8c17  /* then BASE (64-bit) and PITCH (bytes >> 6) */
#define RB_A2D_CLEAR_COLOR_DW0  0x8c2c
#define SP_A2D_OUTPUT_INFO      0xacc0
#define FMT6_8_8_8_8_UNORM      0x30
#define SWAP_WXYZ               1       /* B8G8R8A8 in memory */
#define CP_WAIT_FOR_IDLE        0x26
#define CP_BLIT                 0x2c
#define BLIT_OP_SCALE           3

static void solid(struct adreno_hw *hw, uint64_t base, uint32_t pitch, int x, int y, int w, int h, uint32_t argb)
{
    uint32_t cntl = 1u << 7 | FMT6_8_8_8_8_UNORM << 8 | 0xfu << 20;   /* solid colour, RGBA8, all channels, UNORM8 */
    adreno_reg(hw, RB_A2D_PIXEL_CNTL, 0);
    adreno_reg(hw, RB_A2D_BLT_CNTL, cntl);
    adreno_reg(hw, GRAS_A2D_BLT_CNTL, cntl);
    adreno_reg(hw, SP_A2D_OUTPUT_INFO, FMT6_8_8_8_8_UNORM << 3 | 0xfu << 12);
    adreno_emit(hw, adreno_pkt4(RB_A2D_CLEAR_COLOR_DW0, 4));
    adreno_emit(hw, argb >> 16 & 0xff); adreno_emit(hw, argb >> 8 & 0xff); adreno_emit(hw, argb & 0xff); adreno_emit(hw, argb >> 24);
    adreno_emit(hw, adreno_pkt4(RB_A2D_DEST_BUFFER_INFO, 4));
    adreno_emit(hw, FMT6_8_8_8_8_UNORM | SWAP_WXYZ << 10); adreno_emit(hw, (uint32_t)base); adreno_emit(hw, (uint32_t)(base >> 32)); adreno_emit(hw, pitch >> 6);
    adreno_emit(hw, adreno_pkt4(GRAS_A2D_DEST_TL, 2));
    adreno_emit(hw, (uint32_t)x | (uint32_t)y << 16); adreno_emit(hw, (uint32_t)(x + w - 1) | (uint32_t)(y + h - 1) << 16);
    adreno_emit(hw, adreno_pkt7(CP_BLIT, 1)); adreno_emit(hw, BLIT_OP_SCALE);
}

void adreno_2d_fill(struct adreno_hw *hw, uint64_t dst, uint32_t pitch, int x, int y, int w, int h, uint32_t argb)
{
    if (w <= 0 || h <= 0 || adreno_space(hw) < 64) return;
    adreno_emit(hw, adreno_pkt7(CP_WAIT_FOR_IDLE, 0));
    solid(hw, dst, pitch, x, y, w, h, argb);
}

/* The screen's pitch (1080 * 4 = 4320) is not 64-byte aligned, twice it
 * is: the even rows are one surface at the base, the odd ones another,
 * starting (pitch & 63) bytes early, that many pixels to the right. */
void adreno_2d_fill_screen(struct adreno_hw *hw, int x, int y, int w, int h, uint32_t argb)
{
    uint64_t fb = hw->fb_gpuaddr;
    uint32_t pitch = hw->fb_pitch;
    if (w <= 0 || h <= 0 || adreno_space(hw) < 128) return;
    if (!(pitch & 63)) { adreno_2d_fill(hw, fb, pitch, x, y, w, h, argb); return; }
    adreno_emit(hw, adreno_pkt7(CP_WAIT_FOR_IDLE, 0));
    int shift = (int)(pitch & 63) / 4;
    int e0 = (y + 1) / 2, e1 = (y + h + 1) / 2, o0 = y / 2, o1 = (y + h) / 2;
    if (e1 > e0) solid(hw, fb, 2 * pitch, x, e0, w, e1 - e0, argb);
    if (o1 > o0) solid(hw, fb + (pitch & ~63u), 2 * pitch, x + shift, o0, w, o1 - o0, argb);
}
