/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* adrenotest: /dev/adrenogpu from user space: a buffer the GPU fills and we
 * read back, then a few rectangles on the screen. */
#include "adreno_hw.h"
#include <stdio.h>

int main(void)
{
    struct adreno_hw hw;
    if (adreno_open(&hw) != 0) { perror("adrenotest: /dev/adrenogpu"); return 1; }
    printf("adreno %x, gmem %u KiB, screen %ux%u (pitch %u) at %llx\n", hw.chip_id, hw.gmem_size / 1024, hw.width, hw.height,
           hw.fb_pitch, (unsigned long long)hw.fb_gpuaddr);

    struct adreno_bo bo;
    if (adreno_bo_new(&hw, 64 * 64 * 4, &bo) != 0) { perror("adrenotest: buffer"); return 1; }
    volatile uint32_t *px = (volatile uint32_t *)bo.map;
    adreno_2d_fill(&hw, bo.gpuaddr, 256, 0, 0, 64, 64, 0xff000000);
    adreno_2d_fill(&hw, bo.gpuaddr, 256, 16, 8, 32, 16, 0xffcc8844);
    if (adreno_finish(&hw) != 0) { printf("adrenotest: the GPU did not finish\n"); return 1; }
    int ok = px[0] == 0xff000000 && px[8 * 64 + 16] == 0xffcc8844 && px[23 * 64 + 47] == 0xffcc8844 && px[24 * 64 + 48] == 0xff000000;
    printf("buffer at %llx: (0,0) %08x (16,8) %08x (47,23) %08x (48,24) %08x: %s\n", (unsigned long long)bo.gpuaddr,
           px[0], px[8 * 64 + 16], px[23 * 64 + 47], px[24 * 64 + 48], ok ? "right" : "WRONG");

    /* stripes on the screen, below the bars the kernel drew */
    static const uint32_t col[] = { 0xffff0000, 0xffffff00, 0xff00ff00, 0xff00ffff, 0xff0000ff, 0xffff00ff };
    int sw = (int)hw.width / 6;
    for (int i = 0; i < 6; i++) adreno_2d_fill_screen(&hw, i * sw, 1300, sw, 300, col[i]);
    if (adreno_finish(&hw) != 0) { printf("adrenotest: the GPU did not finish the stripes\n"); return 1; }
    printf("stripes drawn (fence %u)\n", hw.fence);
    adreno_bo_free(&hw, &bo);
    adreno_close(&hw);
    return ok ? 0 : 1;
}
