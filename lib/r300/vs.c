/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The vertex shader on the CPU: these IGPs have no vertex engine, so every
 * vertex runs through this interpreter of the parsed TGSI. */
#include "r300_priv.h"
#include <math.h>
#include <string.h>

static inline void fetch(const struct tgsi_src *s, const float (*in)[4], const float (*temp)[4],
                         const float (*consts)[4], const float (*imm)[4], float out[4])
{
    static const float zero[4];
    const float *r;
    switch (s->file) {
    case TF_IN:    r = in[s->index]; break;
    case TF_TEMP:  r = temp[s->index]; break;
    case TF_CONST: r = consts[s->index]; break;
    case TF_IMM:   r = imm[s->index]; break;
    default:       r = zero; break;
    }
    for (int i = 0; i < 4; i++) {
        float v = r[s->swz[i]];
        if (s->abs) v = fabsf(v);
        if (s->neg) v = -v;
        out[i] = v;
    }
}

void r300_vs_run(const struct tgsi_shader *s, const float (*consts)[4], const float (*in)[4], float (*out)[4], float (*temp)[4])
{
    int loops[16], nloops = 0;
    for (int pc = 0; pc < s->ninst;) {
        const struct tgsi_inst *I = &s->inst[pc];
        float a[4] = {0}, b[4] = {0}, c[4] = {0}, r[4] = {0};
        if (I->nsrc > 0) fetch(&I->src[0], in, (const float (*)[4])temp, consts, (const float (*)[4])s->imm, a);
        if (I->nsrc > 1) fetch(&I->src[1], in, (const float (*)[4])temp, consts, (const float (*)[4])s->imm, b);
        if (I->nsrc > 2) fetch(&I->src[2], in, (const float (*)[4])temp, consts, (const float (*)[4])s->imm, c);
        int write = 1;
        switch (I->op) {
        case OP_MOV: memcpy(r, a, 16); break;
        case OP_ADD: for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;
        case OP_MUL: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; break;
        case OP_MAD: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i] + c[i]; break;
        case OP_DP2: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1]; break;
        case OP_DP3: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
        case OP_DP4: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
        case OP_MIN: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case OP_MAX: for (int i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; break;
        case OP_SLT: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i]; break;
        case OP_SGE: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i]; break;
        case OP_SGT: for (int i = 0; i < 4; i++) r[i] = a[i] > b[i]; break;
        case OP_SLE: for (int i = 0; i < 4; i++) r[i] = a[i] <= b[i]; break;
        case OP_SEQ: for (int i = 0; i < 4; i++) r[i] = a[i] == b[i]; break;
        case OP_SNE: for (int i = 0; i < 4; i++) r[i] = a[i] != b[i]; break;
        case OP_CMP: for (int i = 0; i < 4; i++) r[i] = a[i] < 0 ? b[i] : c[i]; break;
        case OP_LRP: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i] + (1 - a[i]) * c[i]; break;
        case OP_FRC: for (int i = 0; i < 4; i++) r[i] = a[i] - floorf(a[i]); break;
        case OP_FLR: for (int i = 0; i < 4; i++) r[i] = floorf(a[i]); break;
        case OP_TRUNC: for (int i = 0; i < 4; i++) r[i] = truncf(a[i]); break;
        case OP_CEIL: for (int i = 0; i < 4; i++) r[i] = ceilf(a[i]); break;
        case OP_RCP: r[0] = r[1] = r[2] = r[3] = 1.0f / a[0]; break;
        case OP_RSQ: r[0] = r[1] = r[2] = r[3] = 1.0f / sqrtf(fabsf(a[0])); break;
        case OP_SQRT: r[0] = r[1] = r[2] = r[3] = sqrtf(a[0]); break;
        case OP_EX2: r[0] = r[1] = r[2] = r[3] = exp2f(a[0]); break;
        case OP_LG2: r[0] = r[1] = r[2] = r[3] = log2f(a[0]); break;
        case OP_POW: r[0] = r[1] = r[2] = r[3] = powf(a[0], b[0]); break;
        case OP_SIN: r[0] = r[1] = r[2] = r[3] = sinf(a[0]); break;
        case OP_COS: r[0] = r[1] = r[2] = r[3] = cosf(a[0]); break;
        case OP_DDX: case OP_DDY: case OP_TEX: case OP_TXP: case OP_TXL: break;   /* no derivatives or textures in a VS */
        case OP_IF:
            write = 0;
            if (a[0] == 0.0f) { pc = I->jump; continue; }
            break;
        case OP_ELSE: pc = I->jump; continue;   /* the IF branch ran: skip the ELSE branch */
        case OP_BGNLOOP: write = 0; if (nloops < 16) loops[nloops++] = pc; break;
        case OP_ENDLOOP: pc = I->jump; continue;
        case OP_BRK: if (nloops) { pc = s->inst[loops[--nloops]].jump; continue; } write = 0; break;
        case OP_CONT: if (nloops) { pc = loops[nloops - 1] + 1; continue; } write = 0; break;
        case OP_END: return;
        default: write = 0; break;
        }
        if (write && I->dst.file) {
            float *d = I->dst.file == TF_OUT ? out[I->dst.index] : temp[I->dst.index];
            for (int i = 0; i < 4; i++)
                if (I->dst.mask & (1 << i)) {
                    float v = r[i];
                    if (I->sat) v = v < 0 ? 0 : v > 1 ? 1 : v;
                    d[i] = v;
                }
        }
        pc++;
    }
}
