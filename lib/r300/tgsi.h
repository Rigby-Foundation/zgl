/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* TGSI text (what zgl's GLSL compiler emits) parsed into a small IR: the
 * input of the CPU vertex shader interpreter and of the R300 fragment
 * program compiler. */
#pragma once
#include <stdint.h>

enum tgsi_file { TF_NULL, TF_IN, TF_OUT, TF_TEMP, TF_CONST, TF_IMM, TF_SAMP };

enum tgsi_op {
    OP_NOP, OP_MOV, OP_ADD, OP_MUL, OP_MAD, OP_DP2, OP_DP3, OP_DP4, OP_MIN, OP_MAX,
    OP_SLT, OP_SGE, OP_SEQ, OP_SNE, OP_SGT, OP_SLE, OP_CMP, OP_LRP,
    OP_FRC, OP_FLR, OP_TRUNC, OP_CEIL, OP_RCP, OP_RSQ, OP_SQRT, OP_EX2, OP_LG2, OP_POW, OP_SIN, OP_COS,
    OP_DDX, OP_DDY, OP_TEX, OP_TXP, OP_TXL, OP_KILL_IF,
    OP_IF, OP_ELSE, OP_ENDIF, OP_BGNLOOP, OP_ENDLOOP, OP_BRK, OP_CONT, OP_END,
    OP_COUNT
};

enum tgsi_sem { SEM_NONE, SEM_POSITION, SEM_COLOR, SEM_BCOLOR, SEM_GENERIC, SEM_PSIZE, SEM_FACE, SEM_FOG };

struct tgsi_src {
    uint8_t file, neg, abs;
    uint8_t swz[4];                 /* 0..3 = x..w */
    uint16_t index;
};

struct tgsi_dst {
    uint8_t file, mask;             /* mask: bit 0 = x .. bit 3 = w */
    uint16_t index;
};

struct tgsi_inst {
    uint8_t op, sat, nsrc, tex_target;   /* tex_target: 0 = 2D, 1 = CUBE */
    struct tgsi_dst dst;
    struct tgsi_src src[3];
    uint16_t sampler;
    int16_t jump;                   /* IF/ELSE: where to go when not taken; BGNLOOP<->ENDLOOP: the partner */
};

struct tgsi_io {
    uint8_t sem, interp;            /* interp: 0 perspective, 1 linear, 2 constant */
    uint16_t sindex;
};

#define TGSI_MAX_IO   32
#define TGSI_MAX_IMM  64

struct tgsi_shader {
    int frag;                       /* 0 = vertex shader, 1 = fragment shader */
    struct tgsi_inst *inst;
    int ninst;
    float imm[TGSI_MAX_IMM][4];
    int nimm;
    int ntemps, nconsts, nsamp;
    struct tgsi_io in[TGSI_MAX_IO], out[TGSI_MAX_IO];
    int nin, nout;
};

/* 0 on success, -1 with a message in err. */
int  tgsi_parse(const char *text, struct tgsi_shader *s, char *err, int errsize);
void tgsi_free(struct tgsi_shader *s);
const char *tgsi_op_name(int op);
