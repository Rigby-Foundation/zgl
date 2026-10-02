/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* TGSI fragment shaders to R300 fragment programs.
 *
 * The R300 fragment unit runs up to 4 nodes, each a block of texture
 * instructions followed by a block of ALU instructions (64 ALU and 32 TEX
 * in all), on 32 temporaries and 32 constants, with no branches. An ALU
 * instruction is a pair: a vec3 operation on the colour channels and a
 * scalar one on alpha, each with three source registers and three
 * operands built from them with a limited set of swizzles (Mesa's r300
 * compiler documents the rules; the encodings follow its
 * r300_fragprog_emit.c).
 *
 * Three passes: lower TGSI into a few hardware-shaped operations on
 * virtual registers (flattening IF/ELSE into selects), allocate the
 * hardware temporaries, and encode, splitting what the swizzle rules do
 * not allow into per-channel instructions. */
#include "r300_priv.h"
#include "r300_reg.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---- the intermediate form ---------------------------------------------------- */

enum { VK_MAD, VK_DP3, VK_DP4, VK_MIN, VK_MAX, VK_CMP, VK_FRC, VK_RCP, VK_RSQ, VK_EX2, VK_LG2, VK_TEX, VK_TXP, VK_KIL };
enum { VF_NONE, VF_TEMP, VF_CONST, VF_ONE, VF_ZERO, VF_HALF };

struct vsrc { uint8_t file, neg, abs; uint8_t swz[4]; uint16_t idx; };
struct vop {
    uint8_t kind, sat, mask, sampler, out;          /* out: also write the colour output */
    uint16_t dst;
    struct vsrc s[3];
};

#define MAX_VOPS  512
#define MAX_VREGS 512

struct comp {
    const struct tgsi_shader *t;
    struct r300_fs *fs;
    char *err; int errsize;
    struct vop ops[MAX_VOPS]; int nops;
    int nv;                                         /* virtual registers */
    int vin0, vout;                                 /* first input register, the colour output */
    int cond[16], ncond;                            /* IF nesting: the condition registers (0/1 in .x) */
    int cond_else[16];                              /* the branch's condition before ELSE */
    int hw[MAX_VREGS];                              /* allocation */
    int failed;
};

static int error(struct comp *c, const char *msg)
{
    if (!c->failed) snprintf(c->err, (size_t)c->errsize, "r300 fs: %s", msg);
    c->failed = 1;
    return -1;
}

static int newreg(struct comp *c) { if (c->nv >= MAX_VREGS) { error(c, "too many registers"); return 0; } return c->nv++; }

static struct vsrc vreg(int r)
{
    struct vsrc s = { VF_TEMP, 0, 0, { 0, 1, 2, 3 }, (uint16_t)r };
    return s;
}
static struct vsrc vspecial(int file) { struct vsrc s = { (uint8_t)file, 0, 0, { 0, 1, 2, 3 }, 0 }; return s; }
static struct vsrc rep(struct vsrc s, int comp) { uint8_t v = s.swz[comp]; s.swz[0] = s.swz[1] = s.swz[2] = s.swz[3] = v; return s; }
static struct vsrc neg(struct vsrc s) { s.neg ^= 1; return s; }
static struct vsrc absv(struct vsrc s) { s.abs = 1; s.neg = 0; return s; }

/* A literal constant: a slot in the constant file (shared by equal values). */
static struct vsrc lit(struct comp *c, float v)
{
    if (v == 0.0f) return vspecial(VF_ZERO);
    if (v == 1.0f) return vspecial(VF_ONE);
    if (v == 0.5f) return vspecial(VF_HALF);
    struct r300_fs *fs = c->fs;
    for (int i = 0; i < fs->nconst; i++)
        if (fs->const_user[i] < 0 && fs->const_lit[i][0] == v) { struct vsrc s = { VF_CONST, 0, 0, { 0, 0, 0, 0 }, (uint16_t)i }; return s; }
    if (fs->nconst >= R300_FS_MAX_CONST) { error(c, "too many constants"); return vspecial(VF_ZERO); }
    int i = fs->nconst++;
    fs->const_user[i] = -1;
    fs->const_lit[i][0] = fs->const_lit[i][1] = fs->const_lit[i][2] = fs->const_lit[i][3] = v;
    struct vsrc s = { VF_CONST, 0, 0, { 0, 0, 0, 0 }, (uint16_t)i };
    return s;
}

static int const_slot_user(struct comp *c, int user)
{
    struct r300_fs *fs = c->fs;
    for (int i = 0; i < fs->nconst; i++) if (fs->const_user[i] == user) return i;
    if (fs->nconst >= R300_FS_MAX_CONST) { error(c, "too many constants"); return 0; }
    fs->const_user[fs->nconst] = user;
    return fs->nconst++;
}

static int const_slot_imm(struct comp *c, const float v[4])
{
    struct r300_fs *fs = c->fs;
    for (int i = 0; i < fs->nconst; i++)
        if (fs->const_user[i] < 0 && !memcmp(fs->const_lit[i], v, 16)) return i;
    if (fs->nconst >= R300_FS_MAX_CONST) { error(c, "too many constants"); return 0; }
    fs->const_user[fs->nconst] = -1;
    memcpy(fs->const_lit[fs->nconst], v, 16);
    return fs->nconst++;
}

/* A TGSI source as a virtual one. */
static struct vsrc from_tgsi(struct comp *c, const struct tgsi_src *t)
{
    struct vsrc s = { VF_TEMP, t->neg, t->abs, { t->swz[0], t->swz[1], t->swz[2], t->swz[3] }, 0 };
    switch (t->file) {
    case TF_TEMP: s.idx = t->index; break;
    case TF_IN: s.idx = (uint16_t)(c->vin0 + t->index); break;
    case TF_OUT: s.idx = (uint16_t)c->vout; break;
    case TF_CONST: s.file = VF_CONST; s.idx = (uint16_t)const_slot_user(c, t->index); break;
    case TF_IMM: s.file = VF_CONST; s.idx = (uint16_t)const_slot_imm(c, c->t->imm[t->index]); break;
    default: s.file = VF_ZERO; break;
    }
    return s;
}

static void emit(struct comp *c, int kind, int dst, int mask, int sat, struct vsrc a, struct vsrc b, struct vsrc d)
{
    if (c->nops >= MAX_VOPS) { error(c, "shader too long"); return; }
    /* the hardware reads at most two constants per instruction: a third goes through a temporary */
    struct vsrc *s[3] = { &a, &b, &d };
    int consts = 0;
    for (int i = 0; i < 3; i++) {
        if (s[i]->file != VF_CONST) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) if (s[j]->file == VF_CONST && s[j]->idx == s[i]->idx) dup = 1;
        if (dup) continue;
        if (++consts > 2) {
            int t = newreg(c);
            struct vop *m = &c->ops[c->nops++];
            memset(m, 0, sizeof *m);
            m->kind = VK_MAD; m->dst = (uint16_t)t; m->mask = 0xF;
            m->s[0] = *s[i]; m->s[0].neg = 0; m->s[0].abs = 0; m->s[1] = vspecial(VF_ONE); m->s[2] = vspecial(VF_ZERO);
            for (int k = 0; k < 4; k++) m->s[0].swz[k] = (uint8_t)k;
            struct vsrc r = vreg(t);
            memcpy(r.swz, s[i]->swz, 4);
            r.neg = s[i]->neg; r.abs = s[i]->abs;
            *s[i] = r;
        }
    }
    struct vop *o = &c->ops[c->nops++];
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind; o->dst = (uint16_t)dst; o->mask = (uint8_t)mask; o->sat = (uint8_t)sat;
    o->s[0] = a; o->s[1] = b; o->s[2] = d;
}

static void mad(struct comp *c, int dst, int mask, int sat, struct vsrc a, struct vsrc b, struct vsrc d) { emit(c, VK_MAD, dst, mask, sat, a, b, d); }
static void mov(struct comp *c, int dst, int mask, int sat, struct vsrc a) { mad(c, dst, mask, sat, a, vspecial(VF_ONE), vspecial(VF_ZERO)); }
static void scalar(struct comp *c, int kind, int dst, int mask, int sat, struct vsrc a) { emit(c, kind, dst, mask, sat, a, vspecial(VF_ZERO), vspecial(VF_ZERO)); }

/* sin(x): range-reduce to [-pi, pi], then a parabola with one refinement
 * (max error about 0.001). cos(x) = sin(x + pi/2). */
static void fs_sincos(struct comp *c, int dst, int mask, int sat, struct vsrc a, int is_cos)
{
    int t = newreg(c);
    struct vsrc x = rep(a, 0);
    mad(c, t, 1, 0, x, lit(c, 1.0f / (2.0f * (float)M_PI)), lit(c, is_cos ? 0.75f : 0.5f));
    emit(c, VK_FRC, t, 1, 0, rep(vreg(t), 0), vspecial(VF_ZERO), vspecial(VF_ZERO));
    mad(c, t, 1, 0, rep(vreg(t), 0), lit(c, 2.0f * (float)M_PI), lit(c, (float)-M_PI));      /* in [-pi, pi) */
    int y = newreg(c);
    /* y = 4/pi x - 4/pi^2 x |x| */
    mad(c, y, 1, 0, rep(vreg(t), 0), absv(rep(vreg(t), 0)), vspecial(VF_ZERO));
    mad(c, y, 1, 0, rep(vreg(y), 0), lit(c, (float)(-4.0 / (M_PI * M_PI))), vspecial(VF_ZERO));
    mad(c, y, 1, 0, rep(vreg(t), 0), lit(c, (float)(4.0 / M_PI)), rep(vreg(y), 0));
    /* y = 0.225 (y |y| - y) + y */
    mad(c, t, 1, 0, rep(vreg(y), 0), absv(rep(vreg(y), 0)), neg(rep(vreg(y), 0)));
    mad(c, dst, mask, sat, rep(vreg(t), 0), lit(c, 0.225f), rep(vreg(y), 0));
}

/* One TGSI instruction computing into `dst` (a virtual register). */
static void lower(struct comp *c, const struct tgsi_inst *I, int dst, int mask)
{
    struct vsrc a = I->nsrc > 0 ? from_tgsi(c, &I->src[0]) : vspecial(VF_ZERO);
    struct vsrc b = I->nsrc > 1 ? from_tgsi(c, &I->src[1]) : vspecial(VF_ZERO);
    struct vsrc d = I->nsrc > 2 ? from_tgsi(c, &I->src[2]) : vspecial(VF_ZERO);
    int sat = I->sat, t;
    struct vsrc zero = vspecial(VF_ZERO), one = vspecial(VF_ONE);
    switch (I->op) {
    case OP_MOV: mov(c, dst, mask, sat, a); break;
    case OP_ADD: mad(c, dst, mask, sat, a, one, b); break;
    case OP_MUL: mad(c, dst, mask, sat, a, b, zero); break;
    case OP_MAD: mad(c, dst, mask, sat, a, b, d); break;
    case OP_DP3: emit(c, VK_DP3, dst, mask, sat, a, b, zero); break;
    case OP_DP4: emit(c, VK_DP4, dst, mask, sat, a, b, zero); break;
    case OP_DP2:
        t = newreg(c);
        mad(c, t, 3, 0, a, b, zero);
        mad(c, dst, mask, sat, rep(vreg(t), 0), one, rep(vreg(t), 1));
        break;
    case OP_MIN: emit(c, VK_MIN, dst, mask, sat, a, b, zero); break;
    case OP_MAX: emit(c, VK_MAX, dst, mask, sat, a, b, zero); break;
    case OP_CMP: emit(c, VK_CMP, dst, mask, sat, a, b, d); break;       /* a < 0 ? b : d */
    case OP_SLT: case OP_SGE: case OP_SGT: case OP_SLE:
        t = newreg(c);
        if (I->op == OP_SLT || I->op == OP_SGE) mad(c, t, mask, 0, a, one, neg(b));     /* a - b */
        else mad(c, t, mask, 0, b, one, neg(a));                                       /* b - a */
        if (I->op == OP_SLT || I->op == OP_SGT) emit(c, VK_CMP, dst, mask, sat, vreg(t), one, zero);
        else emit(c, VK_CMP, dst, mask, sat, vreg(t), zero, one);
        break;
    case OP_SEQ: case OP_SNE:
        t = newreg(c);
        mad(c, t, mask, 0, a, one, neg(b));
        /* -|a - b| < 0 when they differ */
        emit(c, VK_CMP, dst, mask, sat, neg(absv(vreg(t))), I->op == OP_SNE ? one : zero, I->op == OP_SNE ? zero : one);
        break;
    case OP_LRP:
        t = newreg(c);
        mad(c, t, mask, 0, b, one, neg(d));
        mad(c, dst, mask, sat, a, vreg(t), d);
        break;
    case OP_FRC: emit(c, VK_FRC, dst, mask, sat, a, zero, zero); break;
    case OP_FLR:
        t = newreg(c);
        emit(c, VK_FRC, t, mask, 0, a, zero, zero);
        mad(c, dst, mask, sat, a, one, neg(vreg(t)));
        break;
    case OP_CEIL:                                   /* x + frc(-x) */
        t = newreg(c);
        emit(c, VK_FRC, t, mask, 0, neg(a), zero, zero);
        mad(c, dst, mask, sat, a, one, vreg(t));
        break;
    case OP_TRUNC:                                  /* floor(|x|) with the sign of x */
        t = newreg(c);
        emit(c, VK_FRC, t, mask, 0, absv(a), zero, zero);
        mad(c, t, mask, 0, absv(a), one, neg(vreg(t)));
        emit(c, VK_CMP, dst, mask, sat, a, neg(vreg(t)), vreg(t));
        break;
    case OP_RCP: scalar(c, VK_RCP, dst, mask, sat, rep(a, 0)); break;
    case OP_RSQ: scalar(c, VK_RSQ, dst, mask, sat, absv(rep(a, 0))); break;
    case OP_SQRT:
        t = newreg(c);
        scalar(c, VK_RSQ, t, 1, 0, absv(rep(a, 0)));
        scalar(c, VK_RCP, dst, mask, sat, rep(vreg(t), 0));
        break;
    case OP_EX2: scalar(c, VK_EX2, dst, mask, sat, rep(a, 0)); break;
    case OP_LG2: scalar(c, VK_LG2, dst, mask, sat, rep(a, 0)); break;
    case OP_POW:
        t = newreg(c);
        scalar(c, VK_LG2, t, 1, 0, rep(a, 0));
        mad(c, t, 1, 0, rep(vreg(t), 0), rep(b, 0), zero);
        scalar(c, VK_EX2, dst, mask, sat, rep(vreg(t), 0));
        break;
    case OP_SIN: fs_sincos(c, dst, mask, sat, a, 0); break;
    case OP_COS: fs_sincos(c, dst, mask, sat, a, 1); break;
    case OP_DDX: case OP_DDY: mov(c, dst, mask, sat, zero); break;   /* no derivatives on R300 */
    case OP_TEX: case OP_TXL: case OP_TXP:
        emit(c, I->op == OP_TXP ? VK_TXP : VK_TEX, dst, mask, sat, a, zero, zero);
        c->ops[c->nops - 1].sampler = (uint8_t)I->sampler;
        break;
    default: {
        char m[48];
        snprintf(m, sizeof m, "%s is not supported", tgsi_op_name(I->op));
        error(c, m);
    }
    }
}

static int dst_reg(struct comp *c, const struct tgsi_dst *d)
{
    if (d->file == TF_TEMP) return d->index;
    if (d->file == TF_OUT) {
        const struct tgsi_io *io = &c->t->out[d->index];
        if (io->sem == SEM_COLOR && io->sindex == 0) return c->vout;
        return -1;                                  /* depth and extra colours: dropped */
    }
    return -1;
}

/* The current branch condition: 1.0 in .x when this code runs. */
static int active_cond(struct comp *c) { return c->ncond ? c->cond[c->ncond - 1] : -1; }

static int lower_all(struct comp *c)
{
    const struct tgsi_shader *t = c->t;
    for (int pc = 0; pc < t->ninst && !c->failed; pc++) {
        const struct tgsi_inst *I = &t->inst[pc];
        switch (I->op) {
        case OP_END: return 0;
        case OP_NOP: continue;
        case OP_BGNLOOP: case OP_ENDLOOP: case OP_BRK: case OP_CONT:
            return error(c, "loops that were not unrolled are not supported");
        case OP_IF: {
            if (c->ncond == 16) return error(c, "IF nesting too deep");
            /* cond = (src.x != 0) [and the enclosing condition] */
            int r = newreg(c);
            struct vsrc x = rep(from_tgsi(c, &I->src[0]), 0);
            emit(c, VK_CMP, r, 1, 0, neg(absv(x)), vspecial(VF_ONE), vspecial(VF_ZERO));
            int outer = active_cond(c);
            if (outer >= 0) mad(c, r, 1, 0, rep(vreg(r), 0), rep(vreg(outer), 0), vspecial(VF_ZERO));
            c->cond_else[c->ncond] = outer;
            c->cond[c->ncond++] = r;
            continue;
        }
        case OP_ELSE: {
            if (!c->ncond) return error(c, "ELSE without IF");
            int r = newreg(c), cur = c->cond[c->ncond - 1], outer = c->cond_else[c->ncond - 1];
            /* else = outer * (1 - cur) */
            if (outer >= 0) mad(c, r, 1, 0, neg(rep(vreg(cur), 0)), rep(vreg(outer), 0), rep(vreg(outer), 0));
            else mad(c, r, 1, 0, neg(rep(vreg(cur), 0)), vspecial(VF_ONE), vspecial(VF_ONE));
            c->cond[c->ncond - 1] = r;
            continue;
        }
        case OP_ENDIF:
            if (!c->ncond) return error(c, "ENDIF without IF");
            c->ncond--;
            continue;
        case OP_KILL_IF: {
            struct vsrc s = from_tgsi(c, &I->src[0]);
            int cond = active_cond(c);
            if (cond >= 0) {                        /* only where the branch runs: elsewhere 0 (not negative) */
                int r = newreg(c);
                emit(c, VK_CMP, r, 0xF, 0, neg(rep(vreg(cond), 0)), s, vspecial(VF_ZERO));
                s = vreg(r);
            }
            emit(c, VK_KIL, 0, 0, 0, s, vspecial(VF_ZERO), vspecial(VF_ZERO));
            c->fs->uses_kill = 1;
            continue;
        }
        default: break;
        }
        int real = dst_reg(c, &I->dst);
        if (real < 0) continue;
        int cond = active_cond(c);
        if (cond < 0) { lower(c, I, real, I->dst.mask); continue; }
        /* in a branch: compute aside, then select into the real destination */
        int tmp = newreg(c);
        lower(c, I, tmp, I->dst.mask);
        emit(c, VK_CMP, real, I->dst.mask, 0, neg(rep(vreg(cond), 0)), vreg(tmp), vreg(real));
    }
    return c->failed ? -1 : 0;
}

/* ---- copies ------------------------------------------------------------------- */

static int is_copy(const struct vop *o)
{
    if (o->kind != VK_MAD || o->sat || o->out) return 0;
    const struct vsrc *a = &o->s[0];
    if (a->file != VF_TEMP || a->neg || a->abs || a->idx == o->dst) return 0;
    if (o->s[1].file != VF_ONE || o->s[1].neg || o->s[1].abs || o->s[2].file != VF_ZERO) return 0;
    for (int ch = 0; ch < 4; ch++) if ((o->mask & (1 << ch)) && a->swz[ch] != ch) return 0;
    return 1;
}

static int reads(const struct vop *o, int r)
{
    for (int k = 0; k < 3; k++) if (o->s[k].file == VF_TEMP && o->s[k].idx == r) return 1;
    return 0;
}

/* "t = f(...); d = t" with t used nowhere else becomes "d = f(...)": zgl's
 * GLSL compiler writes every result to a fresh temporary and moves it. */
static void coalesce(struct comp *c)
{
    for (int changed = 1; changed;) {
        changed = 0;
        /* d.x = t.x; d.y = t.y; ... in a row: one copy of d.xy.. */
        for (int i = 0; i + 1 < c->nops; i++) {
            struct vop *a = &c->ops[i], *b = &c->ops[i + 1];
            if (!is_copy(a) || !is_copy(b) || a->dst != b->dst || a->s[0].idx != b->s[0].idx || (a->mask & b->mask)) continue;
            a->mask |= b->mask;
            for (int ch = 0; ch < 4; ch++) if (b->mask & (1 << ch)) a->s[0].swz[ch] = (uint8_t)ch;
            memmove(b, b + 1, sizeof(struct vop) * (size_t)(c->nops - i - 2));
            c->nops--;
            changed = 1;
            i--;
        }
        for (int i = 0; i < c->nops; i++) {
            struct vop *m = &c->ops[i];
            if (!is_copy(m)) continue;
            int t = m->s[0].idx, d = m->dst, p = -1, writers = 0, users = 0;
            for (int j = 0; j < c->nops; j++) {
                if (c->ops[j].kind != VK_KIL && c->ops[j].dst == t) { writers++; if (j < i) p = j; }
                users += reads(&c->ops[j], t);
            }
            if (writers != 1 || users != 1 || p < 0 || c->ops[p].mask != m->mask || c->ops[p].kind == VK_KIL) continue;
            if ((c->ops[p].kind == VK_TEX || c->ops[p].kind == VK_TXP) && m->mask != 0xF) continue;
            int clash = 0;
            for (int j = p + 1; j < i && !clash; j++) clash = reads(&c->ops[j], d) || (c->ops[j].kind != VK_KIL && c->ops[j].dst == d);
            if (clash || reads(&c->ops[p], d)) continue;
            c->ops[p].dst = (uint16_t)d;
            memmove(&c->ops[i], &c->ops[i + 1], sizeof(struct vop) * (size_t)(c->nops - i - 1));
            c->nops--;
            changed = 1;
            i--;
        }
    }
}

/* ---- register allocation ------------------------------------------------------ */

static int allocate(struct comp *c)
{
    int first[MAX_VREGS], last[MAX_VREGS];
    for (int i = 0; i < c->nv; i++) { first[i] = -1; last[i] = -1; c->hw[i] = -1; }
    for (int i = 0; i < c->nops; i++) {
        struct vop *o = &c->ops[i];
        for (int k = 0; k < 3; k++)
            if (o->s[k].file == VF_TEMP) {
                int r = o->s[k].idx;
                if (first[r] < 0) first[r] = i;     /* read before written: live from here */
                last[r] = i;
            }
        if (o->kind != VK_KIL) {
            int r = o->dst;
            if (first[r] < 0) first[r] = i;
            if (last[r] < i) last[r] = i;
        }
    }
    int busy_until[32];
    for (int h = 0; h < 32; h++) busy_until[h] = -1;
    /* inputs: where the rasterizer writes them, live from the start */
    for (int i = 0; i < c->t->nin; i++) {
        int r = c->vin0 + i;
        c->hw[r] = i;
        c->fs->input_temp[i] = first[r] >= 0 ? i : -1;
        busy_until[i] = last[r] >= 0 ? last[r] : -1;
        if (last[r] >= 0) first[r] = 0;
    }
    /* the rest in order of first use */
    for (int pos = 0; pos < c->nops; pos++)
        for (int r = 0; r < c->nv; r++) {
            if (first[r] != pos || c->hw[r] >= 0) continue;
            int h = 0;
            while (h < 32 && busy_until[h] >= pos) h++;
            /* a register freed at this very instruction may be read here: take a later one */
            if (h == 32) return error(c, "out of temporaries");
            c->hw[r] = h;
            busy_until[h] = last[r];
        }
    /* the temporaries' live ranges must not overlap within an instruction: a
     * register whose last use is instruction i is free only after i */
    return 0;
}

/* ---- encoding ------------------------------------------------------------------- */

struct slotset { int n; int file[3], idx[3]; };

static int slot_of(struct slotset *s, int file, int idx)
{
    for (int i = 0; i < s->n; i++) if (s->file[i] == file && s->idx[i] == idx) return i;
    if (s->n == 3) return -1;
    s->file[s->n] = file; s->idx[s->n] = idx;
    return s->n++;
}

static uint32_t slot_bits(struct comp *c, const struct slotset *s, int *pixsize)
{
    uint32_t v = 0;
    for (int i = 0; i < s->n; i++) {
        uint32_t a;
        if (s->file[i] == VF_CONST) a = (uint32_t)s->idx[i] | 1u << 5;
        else { a = (uint32_t)c->hw[s->idx[i]] & 0x1F; if (c->hw[s->idx[i]] > *pixsize) *pixsize = c->hw[s->idx[i]]; }
        v |= a << (6 * i);
    }
    return v;
}

/* The RGB operand code for `src` over the channels in `mask` (x,y,z), or -1
 * if no native swizzle fits. Takes a slot in rgb (or alpha, for .www). */
static int rgb_arg(struct vsrc *src, int mask, struct slotset *rs, struct slotset *as)
{
    int code;
    if (src->file == VF_ONE) code = R300_ALU_ARGC_ONE;
    else if (src->file == VF_ZERO) code = R300_ALU_ARGC_ZERO;
    else if (src->file == VF_HALF) code = R300_ALU_ARGC_HALF;
    else {
        static const struct { uint8_t s[3]; uint8_t base, stride, alpha; } nat[] = {
            { { 0, 1, 2 }, R300_ALU_ARGC_SRC0C_XYZ, 4, 0 }, { { 0, 0, 0 }, R300_ALU_ARGC_SRC0C_XXX, 4, 0 },
            { { 1, 1, 1 }, R300_ALU_ARGC_SRC0C_YYY, 4, 0 }, { { 2, 2, 2 }, R300_ALU_ARGC_SRC0C_ZZZ, 4, 0 },
            { { 3, 3, 3 }, R300_ALU_ARGC_SRC0A, 1, 1 }, { { 1, 2, 0 }, R300_ALU_ARGC_SRC0C_YZX, 1, 0 },
            { { 2, 0, 1 }, R300_ALU_ARGC_SRC0C_ZXY, 1, 0 },
        };
        int found = -1;
        for (unsigned n = 0; n < sizeof nat / sizeof nat[0] && found < 0; n++) {
            int ok = 1;
            for (int ch = 0; ch < 3; ch++) if ((mask & (1 << ch)) && src->swz[ch] != nat[n].s[ch]) ok = 0;
            if (ok) found = (int)n;
        }
        if (found < 0) return -1;
        int slot = slot_of(nat[found].alpha ? as : rs, src->file, src->idx);
        if (slot < 0) return -2;
        code = nat[found].base + nat[found].stride * slot;
    }
    return code | src->neg << 5 | src->abs << 6;
}

/* The alpha operand code for channel `ch` of `src`. */
static int alpha_arg(struct vsrc *src, int ch, struct slotset *rs, struct slotset *as)
{
    int code;
    if (src->file == VF_ONE) code = R300_ALU_ARGA_ONE;
    else if (src->file == VF_ZERO) code = R300_ALU_ARGA_ZERO;
    else if (src->file == VF_HALF) code = R300_ALU_ARGA_HALF;
    else {
        int comp = src->swz[ch];
        int slot = slot_of(comp == 3 ? as : rs, src->file, src->idx);
        if (slot < 0) return -2;
        code = comp == 3 ? R300_ALU_ARGA_SRC0A + slot : comp + 3 * slot;
    }
    return code | src->neg << 5 | src->abs << 6;
}

struct node { int first_alu, first_tex; uint32_t flags; };
struct enc {
    struct comp *c;
    struct node node[4]; int cur;
    int pixsize;
    uint32_t tex_written;                           /* hw temps written by the current node's TEX block */
};

static int push_alu(struct enc *e, uint32_t rgb_inst, uint32_t rgb_addr, uint32_t a_inst, uint32_t a_addr)
{
    struct r300_fs *fs = e->c->fs;
    if (fs->nalu >= R300_FS_MAX_ALU) return error(e->c, "more than 64 ALU instructions");
    uint32_t *a = fs->alu[fs->nalu++];
    a[0] = rgb_inst; a[1] = rgb_addr; a[2] = a_inst; a[3] = a_addr;
    return 0;
}

static int rgb_opcode(int kind)
{
    switch (kind) {
    case VK_DP3: return R300_ALU_OUTC_DP3;
    case VK_DP4: return R300_ALU_OUTC_DP4;
    case VK_MIN: return R300_ALU_OUTC_MIN;
    case VK_MAX: return R300_ALU_OUTC_MAX;
    case VK_CMP: return R300_ALU_OUTC_CMP;
    case VK_FRC: return R300_ALU_OUTC_FRC;
    case VK_RCP: case VK_RSQ: case VK_EX2: case VK_LG2: return R300_ALU_OUTC_REPL_ALPHA;
    default: return R300_ALU_OUTC_MAD;
    }
}
static int alpha_opcode(int kind)
{
    switch (kind) {
    case VK_DP3: case VK_DP4: return R300_ALU_OUTA_DP4;
    case VK_MIN: return R300_ALU_OUTA_MIN;
    case VK_MAX: return R300_ALU_OUTA_MAX;
    case VK_CMP: return R300_ALU_OUTA_CMP;
    case VK_FRC: return R300_ALU_OUTA_FRC;
    case VK_RCP: return R300_ALU_OUTA_RCP;
    case VK_RSQ: return R300_ALU_OUTA_RSQ;
    case VK_EX2: return R300_ALU_OUTA_EX2;
    case VK_LG2: return R300_ALU_OUTA_LG2;
    default: return R300_ALU_OUTA_MAD;
    }
}

/* One hardware instruction: the colour part over `rgb_mask`, the alpha part
 * if `do_alpha`. Returns 1 if the swizzles did not fit (nothing emitted). */
static int encode_one(struct enc *e, struct vop *o, int rgb_mask, int do_alpha, int write_alpha)
{
    struct comp *c = e->c;
    struct slotset rs = { 0 }, as = { 0 };
    uint32_t ri = (uint32_t)rgb_opcode(o->kind), ai = (uint32_t)alpha_opcode(o->kind);
    int scalar_op = o->kind >= VK_RCP && o->kind <= VK_LG2;
    int dp = o->kind == VK_DP3 || o->kind == VK_DP4;
    int nargs = o->kind == VK_FRC || scalar_op ? 1 : 3;
    /* colour operands: scalar ops have none (REPL_ALPHA); DP reads all of xyz */
    if (!scalar_op && (rgb_mask || dp)) {
        int m = dp ? 7 : rgb_mask;
        for (int k = 0; k < nargs; k++) {
            int a = rgb_arg(&o->s[k], m, &rs, &as);
            if (a < 0) return 1;
            ri |= (uint32_t)a << (7 * k);
        }
        for (int k = nargs; k < 3; k++) ri |= (uint32_t)R300_ALU_ARGC_ZERO << (7 * k);
    } else {
        for (int k = 0; k < 3; k++) ri |= (uint32_t)R300_ALU_ARGC_ZERO << (7 * k);
    }
    /* alpha operands: the w lane, or channel x for scalar ops; zeros for DP3 */
    if (do_alpha || scalar_op || dp) {
        for (int k = 0; k < 3; k++) {
            int a;
            if (k >= nargs || o->kind == VK_DP3 || (dp && k == 2)) a = R300_ALU_ARGA_ZERO;
            else a = alpha_arg(&o->s[k], scalar_op ? 0 : 3, &rs, &as);
            if (a < 0) return 1;
            ai |= (uint32_t)a << (7 * k);
        }
    } else {
        for (int k = 0; k < 3; k++) ai |= (uint32_t)R300_ALU_ARGA_ZERO << (7 * k);
    }
    int consts = 0;
    for (int i = 0; i < rs.n; i++) consts += rs.file[i] == VF_CONST;
    for (int i = 0; i < as.n; i++) {
        int dup = 0;
        for (int j = 0; j < rs.n; j++) if (rs.file[j] == VF_CONST && as.file[i] == VF_CONST && rs.idx[j] == as.idx[i]) dup = 1;
        if (as.file[i] == VF_CONST && !dup) consts++;
    }
    if (consts > 2) return error(c, "more than two constants in one instruction"), 0;
    uint32_t ra = slot_bits(c, &rs, &e->pixsize), aa = slot_bits(c, &as, &e->pixsize);
    int d = c->hw[o->dst];
    if (d > e->pixsize) e->pixsize = d;
    if (o->sat) { ri |= R300_ALU_OUTC_CLAMP; ai |= R300_ALU_OUTA_CLAMP; }
    if (!o->out && rgb_mask) ra |= (uint32_t)(d & 0x1F) << R300_ALU_DSTC_SHIFT | (uint32_t)rgb_mask << R300_ALU_DSTC_REG_MASK_SHIFT;
    if (!o->out && write_alpha) aa |= (uint32_t)(d & 0x1F) << R300_ALU_DSTA_SHIFT | R300_ALU_DSTA_REG;
    if (o->out) {                                   /* the final write of the colour */
        if (rgb_mask) { ra |= (uint32_t)rgb_mask << R300_ALU_DSTC_OUTPUT_MASK_SHIFT; e->node[e->cur].flags |= R300_RGBA_OUT; }
        if (write_alpha) { aa |= R300_ALU_DSTA_OUTPUT; e->node[e->cur].flags |= R300_RGBA_OUT; }
    }
    push_alu(e, ri, ra, ai, aa);
    return 0;
}

static int encode_alu(struct enc *e, struct vop *o)
{
    int rgb = o->mask & 7, alpha = (o->mask & 8) != 0;
    if (o->kind == VK_DP3 || o->kind == VK_DP4) {
        /* the whole xyz of both operands takes part: make them native first */
        if (encode_one(e, o, rgb, 1, alpha) == 0) return 0;
        return error(e->c, "unsupported swizzle in a dot product");
    }
    if (encode_one(e, o, rgb, alpha, alpha) == 0) return 0;
    /* split the colour channels one by one; alpha goes with the first */
    int first = 1;
    for (int ch = 0; ch < 3; ch++) {
        if (!(rgb & (1 << ch))) continue;
        if (encode_one(e, o, 1 << ch, first && alpha, first && alpha)) return error(e->c, "unencodable operands");
        first = 0;
    }
    if (first && alpha && encode_one(e, o, 0, 1, 1)) return error(e->c, "unencodable operands");
    return 0;
}

static int finish_node(struct enc *e)
{
    struct r300_fs *fs = e->c->fs;
    struct node *n = &e->node[e->cur];
    if (fs->nalu == n->first_alu)                   /* every node needs an ALU instruction */
        push_alu(e, (uint32_t)R300_ALU_ARGC_ZERO | (uint32_t)R300_ALU_ARGC_ZERO << 7 | (uint32_t)R300_ALU_ARGC_ZERO << 14, 0,
                 (uint32_t)R300_ALU_ARGA_ZERO | (uint32_t)R300_ALU_ARGA_ZERO << 7 | (uint32_t)R300_ALU_ARGA_ZERO << 14, 0);
    uint32_t alu_end = (uint32_t)(fs->nalu - n->first_alu - 1);
    uint32_t tex_end = fs->ntex == n->first_tex ? 0 : (uint32_t)(fs->ntex - n->first_tex - 1);
    if (fs->ntex != n->first_tex && e->cur == 0) fs->config |= R300_PFS_CNTL_FIRST_NODE_HAS_TEX;
    fs->code_addr[e->cur] = ((uint32_t)n->first_alu << R300_ALU_START_SHIFT) | (alu_end << R300_ALU_SIZE_SHIFT) |
                            ((uint32_t)n->first_tex << R300_TEX_START_SHIFT) | (tex_end << R300_TEX_SIZE_SHIFT) | n->flags;
    return 0;
}

static int encode_tex(struct enc *e, struct vop *o)
{
    struct comp *c = e->c;
    struct r300_fs *fs = c->fs;
    int src = c->hw[o->s[0].idx];
    struct node *n = &e->node[e->cur];
    /* a TEX after ALU work, or reading what this TEX block wrote: a new node (an indirection) */
    if (fs->nalu != n->first_alu || (fs->ntex != n->first_tex && (e->tex_written & (1u << src)))) {
        if (e->cur == 3) return error(c, "more than 4 texture indirections");
        finish_node(e);
        e->cur++;
        e->node[e->cur].first_alu = fs->nalu;
        e->node[e->cur].first_tex = fs->ntex;
        e->node[e->cur].flags = 0;
        e->tex_written = 0;
    }
    if (fs->ntex >= R300_FS_MAX_TEX) return error(c, "more than 32 texture instructions");
    int dst = o->kind == VK_KIL ? 0 : c->hw[o->dst];
    uint32_t op = o->kind == VK_KIL ? R300_TEX_OP_KIL : o->kind == VK_TXP ? R300_TEX_OP_TXP : R300_TEX_OP_LD;
    fs->tex[fs->ntex++] = (uint32_t)src << R300_SRC_ADDR_SHIFT | (uint32_t)dst << R300_DST_ADDR_SHIFT |
                          (uint32_t)(o->kind == VK_KIL ? 0 : o->sampler) << R300_TEX_ID_SHIFT | op << R300_TEX_INST_SHIFT;
    if (o->kind != VK_KIL) e->tex_written |= 1u << dst;
    if (src > e->pixsize) e->pixsize = src;
    if (dst > e->pixsize) e->pixsize = dst;
    return 0;
}

/* Texture sources must be plain temporaries read .xyzw; partial texture
 * destinations go through a temporary. Output: the last write of the
 * colour marked `out` (a final MOV if needed). */
static void legalize(struct comp *c)
{
    struct vop ops[MAX_VOPS];
    int n = c->nops;
    memcpy(ops, c->ops, sizeof(struct vop) * (size_t)n);
    c->nops = 0;
    for (int i = 0; i < n; i++) {
        struct vop o = ops[i];
        if (o.kind == VK_TEX || o.kind == VK_TXP || o.kind == VK_KIL) {
            struct vsrc *s = &o.s[0];
            /* 2D lookups read .xy, projective ones .xyw, KIL all four */
            int need = o.kind == VK_TEX ? 3 : o.kind == VK_TXP ? 0xB : 0xF, plain = s->file == VF_TEMP && !s->neg && !s->abs;
            for (int k = 0; k < 4; k++) if ((need & (1 << k)) && s->swz[k] != k) plain = 0;
            if (!plain) { int t = newreg(c); mov(c, t, 0xF, 0, *s); *s = vreg(t); }
            if (o.kind != VK_KIL && (o.mask != 0xF || o.sat || o.dst == c->vout)) {
                int t = newreg(c), real = o.dst, mask = o.mask, sat = o.sat;
                o.dst = (uint16_t)t; o.mask = 0xF; o.sat = 0;
                c->ops[c->nops++] = o;
                mov(c, real, mask, sat, vreg(t));
                continue;
            }
        }
        if (c->nops < MAX_VOPS) c->ops[c->nops++] = o;
    }
    /* the colour output: a final instruction writes the whole of it. The
     * unit truncates to 8 bits where GL rounds: half a step makes it round. */
    mad(c, c->vout, 0xF, 1, vreg(c->vout), vspecial(VF_ONE), lit(c, 0.5f / 255.0f));
    c->ops[c->nops - 1].out = 1;
}

int r300_fs_compile(const struct tgsi_shader *t, struct r300_fs *fs, char *err, int errsize)
{
    static struct comp cs;                          /* big: keep it off the stack */
    struct comp *c = &cs;
    memset(c, 0, sizeof *c);
    memset(fs, 0, sizeof *fs);
    c->t = t; c->fs = fs; c->err = err; c->errsize = errsize;
    c->nv = t->ntemps;
    c->vin0 = c->nv; c->nv += t->nin;
    c->vout = c->nv++;
    fs->ninputs = t->nin;
    memcpy(fs->input, t->in, sizeof fs->input);
    if (t->nin > 8) return error(c, "more than 8 inputs");
    if (lower_all(c)) return -1;
    coalesce(c);
    legalize(c);
    if (c->failed || allocate(c)) return -1;
    struct enc e;
    memset(&e, 0, sizeof e);
    e.c = c;
    for (int i = 0; i < c->nops && !c->failed; i++) {
        struct vop *o = &c->ops[i];
        if (o->kind == VK_TEX || o->kind == VK_TXP || o->kind == VK_KIL) encode_tex(&e, o);
        else encode_alu(&e, o);
    }
    if (c->failed) return -1;
    finish_node(&e);
    fs->config |= (uint32_t)e.cur;
    uint32_t tex_end = fs->ntex ? (uint32_t)fs->ntex - 1 : 0;
    fs->code_offset = (0u << R300_PFS_CNTL_ALU_OFFSET_SHIFT) | ((uint32_t)(fs->nalu - 1) << R300_PFS_CNTL_ALU_END_SHIFT) |
                      (0u << R300_PFS_CNTL_TEX_OFFSET_SHIFT) | (tex_end << R300_PFS_CNTL_TEX_END_SHIFT);
    /* the nodes are stored backwards: the last one in CODE_ADDR_3 */
    int shift = 3 - e.cur;
    for (int i = e.cur; i >= 0; i--) fs->code_addr[shift + i] = fs->code_addr[i];
    for (int i = 0; i < shift; i++) fs->code_addr[i] = 0;
    fs->pixsize = (uint32_t)e.pixsize;
    if (e.pixsize >= 32) return error(c, "out of temporaries");
    return 0;
}

void r300_fs_dump(const struct r300_fs *fs)
{
    printf("fs: %d alu, %d tex, config %x pixsize %u offset %08x addr %08x %08x %08x %08x, %d consts\n", fs->nalu, fs->ntex,
           fs->config, fs->pixsize, fs->code_offset, fs->code_addr[0], fs->code_addr[1], fs->code_addr[2], fs->code_addr[3], fs->nconst);
    for (int i = 0; i < fs->ntex; i++) printf("  tex %2d: %08x\n", i, fs->tex[i]);
    for (int i = 0; i < fs->nalu; i++)
        printf("  alu %2d: rgb %08x %08x  alpha %08x %08x\n", i, fs->alu[i][0], fs->alu[i][1], fs->alu[i][2], fs->alu[i][3]);
}

uint32_t r300_float24(float f)
{
    union { float f; uint32_t u; } u = { f };
    if (f == 0.0f) return 0;
    int exponent;
    float mantissa = frexpf(f, &exponent);
    uint32_t v = 0;
    if (mantissa < 0) v |= 1u << 23;
    /* 7 exponent bits (bias 63), 16 mantissa bits: round, don't truncate */
    uint32_t m = ((u.u & 0x7FFFFF) + 0x40) >> 7;
    if (m & 0x10000) { m = 0; exponent++; }
    v |= (uint32_t)(exponent + 62) << 16;
    v |= m;
    return v;
}
