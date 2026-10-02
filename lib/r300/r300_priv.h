/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* libr300 internals: the shader compilers and the renderer. */
#pragma once
#include <stdint.h>
#include "tgsi.h"

/* vs.c: run a vertex shader on one vertex. temp: ntemps vec4 of scratch. */
void r300_vs_run(const struct tgsi_shader *s, const float (*consts)[4], const float (*in)[4], float (*out)[4], float (*temp)[4]);

/* fs.c: a TGSI fragment shader as an R300 fragment program. */
#define R300_FS_MAX_ALU   64
#define R300_FS_MAX_TEX   32
#define R300_FS_MAX_CONST 32

struct r300_fs {
    uint32_t alu[R300_FS_MAX_ALU][4];       /* rgb_inst, rgb_addr, alpha_inst, alpha_addr */
    int nalu;
    uint32_t tex[R300_FS_MAX_TEX];
    int ntex;
    uint32_t config, pixsize, code_offset, code_addr[4];
    /* the constant file: each slot is a user constant or a literal */
    int nconst;
    int const_user[R300_FS_MAX_CONST];      /* TGSI CONST index, or -1 for a literal */
    float const_lit[R300_FS_MAX_CONST][4];
    /* the inputs: TGSI IN[i] arrives in hardware temp input_temp[i] (-1: unused) */
    int ninputs;
    int input_temp[TGSI_MAX_IO];
    struct tgsi_io input[TGSI_MAX_IO];
    int uses_kill;
};

/* 0, or -1 with a message in err. */
int r300_fs_compile(const struct tgsi_shader *s, struct r300_fs *fs, char *err, int errsize);
void r300_fs_dump(const struct r300_fs *fs);

uint32_t r300_float24(float f);
