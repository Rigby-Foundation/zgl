/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The GLSL built-in functions, as TGSI.
 *
 * Most map to an opcode or two. TGSI has no inverse trigonometry, so atan
 * is a polynomial (max error about 1e-5 rad) that asin and acos build on;
 * inverse() and determinant() are written out by cofactors. Texture lookups
 * pack their coordinates the way TGSI wants them (shadow reference, layer,
 * lod or bias in the components GLSL also uses) and pick TEX, TXB, TXL, TXP,
 * TXD, TXF or TXQ. */
#include "glsl_gen.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define ERR(...) glc_error(g->c, __VA_ARGS__)

static int nc(struct val v) { return v.t.vec ? v.t.vec : 1; }
static struct gtype vf(int n) { return ty_vec(GK_FLOAT, n); }

static int is_vec_kind(struct val v, enum gk k) { return ty_is_vector(v.t) && v.t.k == k; }

/* an argument for a float function: ints are converted */
static struct val fl(struct gen *g, struct val v, const char *fn)
{
    if (!ty_is_vector(v.t)) ERR("%s(): wrong argument type", fn);
    if (v.t.k == GK_INT || v.t.k == GK_UINT) return v_convert(g, v, GK_FLOAT);
    if (v.t.k != GK_FLOAT) ERR("%s(): needs float arguments", fn);
    return v;
}

static struct val mulf(struct gen *g, struct val a, float k) { return v_binary(g, "*", a, v_fimm(g, k)); }
static struct val addf(struct gen *g, struct val a, float k) { return v_binary(g, "+", a, v_fimm(g, k)); }

static struct val dot(struct gen *g, struct val a, struct val b)
{
    static const char *const dp[] = { NULL, "MUL", "DP2", "DP3", "DP4" };
    if (nc(a) != nc(b)) ERR("dot(): sizes differ");
    return op2(g, dp[nc(a)], ty_scalar(GK_FLOAT), a, b);
}

static struct val absf(struct gen *g, struct val a)
{
    a = v_direct(g, a);
    a.abs = 1; a.neg = 0;
    return op1(g, "MOV", a.t, a);
}

/* atan(t) for t in [0, 1] (a minimax polynomial in t^2) */
static struct val atan01(struct gen *g, struct val t)
{
    struct val t2 = v_binary(g, "*", t, t);
    static const float k[] = { -0.0121323213f, 0.0536813784f, -0.1173503194f, 0.1938924977f, -0.3326756418f, 0.9999793128f };
    struct val p = v_bcast(v_fimm(g, k[0]), nc(t));
    for (int i = 1; i < 6; i++) p = op3(g, "MAD", t.t, p, t2, v_bcast(v_fimm(g, k[i]), nc(t)));
    return v_binary(g, "*", p, t);
}

/* atan2(y, x), component-wise */
static struct val atan2v(struct gen *g, struct val y, struct val x)
{
    int n = nc(y);
    struct gtype bt = ty_vec(GK_BOOL, n);
    struct val ay = absf(g, y), ax = absf(g, x);
    struct val mx = op2(g, "MAX", y.t, ax, ay), mn = op2(g, "MIN", y.t, ax, ay);
    struct val t = v_binary(g, "/", mn, mx);
    /* 0/0 at the origin: atan2(0, 0) = 0 */
    t = v_select(g, op2(g, "FSEQ", bt, mx, v_bcast(v_fimm(g, 0), n)), v_bcast(v_fimm(g, 0), n), t);
    struct val p = atan01(g, t);
    struct val hp = v_bcast(v_fimm(g, (float)M_PI_2), n), pi = v_bcast(v_fimm(g, (float)M_PI), n);
    p = v_select(g, op2(g, "FSLT", bt, ax, ay), v_binary(g, "-", hp, p), p);
    p = v_select(g, op2(g, "FSLT", bt, x, v_bcast(v_fimm(g, 0), n)), v_binary(g, "-", pi, p), p);
    struct val neg = p; neg = v_direct(g, neg); neg.neg = 1;
    return v_select(g, op2(g, "FSLT", bt, y, v_bcast(v_fimm(g, 0), n)), op1(g, "MOV", p.t, neg), p);
}

static struct val atan1(struct gen *g, struct val x)
{
    int n = nc(x);
    struct gtype bt = ty_vec(GK_BOOL, n);
    struct val ax = absf(g, x);
    struct val big = op2(g, "FSLT", bt, v_bcast(v_fimm(g, 1), n), ax);
    struct val t = v_select(g, big, scalar_op1(g, "RCP", ax), ax);
    struct val p = atan01(g, t);
    p = v_select(g, big, v_binary(g, "-", v_bcast(v_fimm(g, (float)M_PI_2), n), p), p);
    return v_binary(g, "*", p, op1(g, "SSG", x.t, x));
}

static struct val expv(struct gen *g, struct val x) { return scalar_op1(g, "EX2", mulf(g, x, 1.44269504f)); }
static struct val logv(struct gen *g, struct val x) { return mulf(g, scalar_op1(g, "LG2", x), 0.693147181f); }

/* ---- matrices ----------------------------------------------------------------------- */

static struct val m_el(struct gen *g, struct val m, int c, int r) { return v_comp(v_col(g, m, c), r); }

static struct val det2(struct gen *g, struct val a, struct val b, struct val c, struct val d)
{
    return v_binary(g, "-", v_binary(g, "*", a, d), v_binary(g, "*", b, c));
}

/* the determinant of the n x n minor of m without column `skipc` and row `skipr` (n = size - 1), or of m */
static struct val det_n(struct gen *g, struct val m, int size, const int *cols, const int *rows)
{
    if (size == 1) return m_el(g, m, cols[0], rows[0]);
    if (size == 2) return det2(g, m_el(g, m, cols[0], rows[0]), m_el(g, m, cols[1], rows[0]), m_el(g, m, cols[0], rows[1]), m_el(g, m, cols[1], rows[1]));
    struct val acc = v_fimm(g, 0);
    for (int i = 0; i < size; i++) {
        int sc[4], k = 0;
        for (int j = 0; j < size; j++) if (j != i) sc[k++] = cols[j];
        struct val minor = det_n(g, m, size - 1, sc, rows + 1);
        struct val term = v_binary(g, "*", m_el(g, m, cols[i], rows[0]), minor);
        acc = v_binary(g, (i & 1) ? "-" : "+", acc, term);
    }
    return acc;
}

static struct val determinant(struct gen *g, struct val m)
{
    int all[4] = { 0, 1, 2, 3 };
    return det_n(g, m, m.t.cols, all, all);
}

static struct val inverse(struct gen *g, struct val m)
{
    int n = m.t.cols;
    m = v_direct(g, m);
    struct val det = determinant(g, m);
    struct val inv_det = scalar_op1(g, "RCP", det);
    struct val r = v_temp(g, m.t);
    for (int c = 0; c < n; c++)
        for (int rr = 0; rr < n; rr++) {
            /* inverse[c][r] = cofactor(r, c) / det: the minor without column r and row c */
            int cols[4], rows[4], kc = 0, kr = 0;
            for (int j = 0; j < n; j++) { if (j != rr) cols[kc++] = j; if (j != c) rows[kr++] = j; }
            struct val minor = det_n(g, m, n - 1, cols, rows);
            if ((c + rr) & 1) minor = v_binary(g, "*", minor, v_fimm(g, -1));
            struct val d = v_comp(v_col(g, r, c), rr);
            d.lv = 1;
            v_store(g, d, v_binary(g, "*", minor, inv_det));
        }
    r.lv = 0;
    return r;
}

/* ---- textures -------------------------------------------------------------------------- */

struct texinfo { int dims; int layer; int shadow; int cube; int ms; int buffer; int rect; };

static struct texinfo tex_info(enum zgl_type st)
{
    struct texinfo t;
    memset(&t, 0, sizeof t);
    const char *tg = zgl_type_tgsi_target(st);
    t.shadow = !strncmp(tg, "SHADOW", 6);
    if (t.shadow) tg += 6;
    if (!strcmp(tg, "1D")) t.dims = 1;
    else if (!strcmp(tg, "2D") || !strcmp(tg, "RECT")) { t.dims = 2; t.rect = tg[0] == 'R'; }
    else if (!strcmp(tg, "3D")) t.dims = 3;
    else if (!strcmp(tg, "CUBE")) { t.dims = 3; t.cube = 1; }
    else if (!strcmp(tg, "1D_ARRAY")) { t.dims = 1; t.layer = 1; }
    else if (!strcmp(tg, "2D_ARRAY")) { t.dims = 2; t.layer = 1; }
    else if (!strcmp(tg, "CUBE_ARRAY")) { t.dims = 3; t.cube = 1; t.layer = 1; }
    else if (!strcmp(tg, "BUFFER")) { t.dims = 1; t.buffer = 1; }
    else if (!strcmp(tg, "2D_MSAA")) { t.dims = 2; t.ms = 1; }
    else if (!strcmp(tg, "2D_ARRAY_MSAA")) { t.dims = 2; t.ms = 1; t.layer = 1; }
    return t;
}

/* the result of a lookup into its GLSL type: a vec4/ivec4/uvec4, or the .x of a shadow lookup */
static struct val tex_result(struct gen *g, struct val r, enum zgl_type st, int legacy_shadow)
{
    (void)g;
    enum zgl_type b = zgl_type_base(st);
    r.t = ty_vec(b == T_INT ? GK_INT : b == T_UINT ? GK_UINT : GK_FLOAT, 4);
    if (tex_info(st).shadow && !legacy_shadow) return v_comp(r, 0);
    return r;
}

/* a vec4 temp with p's components first and `extra` at component `at` (if at >= 0) */
static struct val pack(struct gen *g, struct val p, int at, struct val extra, enum gk k)
{
    struct val r = v_temp(g, ty_vec(k, 4));
    int n = nc(p);
    struct val dst = r; dst.t = ty_vec(k, n);
    v_store(g, dst, v_convert(g, p, k));
    if (at >= 0) { struct val d = v_comp(r, at); d.lv = 1; v_store(g, d, v_convert(g, extra, k)); }
    return r;
}

/* the immediate for a constant offset argument, as the TGSI operand text */
static void offset_str(struct gen *g, struct val off, char *buf, size_t n, int dims)
{
    if (!off.is_const) ERR("texture offsets must be constant");
    if (off.t.k != GK_INT || nc(off) != dims) ERR("texture offset: wrong type");
    uint32_t cv[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < dims; i++) cv[i] = off.cv[i];
    struct val im = v_imm(g, ty_vec(GK_INT, 4), cv);
    snprintf(buf, n, "IMM[%d].xyz", im.index);
}

static struct val tex_emit_n(struct gen *g, const char *op, enum zgl_type st, struct val samp, struct val *srcs, int nsrcs, const char *offset, int ndst)
{
    struct val r = v_temp(g, ty_vec(GK_FLOAT, 4));
    char d[96], s[3][96], smp[160];
    for (int i = 0; i < nsrcs; i++) { srcs[i] = v_direct(g, srcs[i]); src_str(g, s[i], sizeof s[i], srcs[i], 0); }
    dst_str(g, d, sizeof d, r, 0, ndst);
    snprintf(smp, sizeof smp, "SAMP[%d], %s%s%s", samp.index, zgl_type_tgsi_target(st), offset ? ", " : "", offset ? offset : "");
    emit(g, op, d, s[0], nsrcs > 1 ? s[1] : smp, nsrcs > 2 ? s[2] : nsrcs > 1 ? smp : NULL, nsrcs > 2 ? smp : NULL);
    r.lv = 0;
    return r;
}

static struct val tex_emit(struct gen *g, const char *op, enum zgl_type st, struct val samp, struct val *srcs, int nsrcs, const char *offset)
{
    return tex_emit_n(g, op, st, samp, srcs, nsrcs, offset, 4);
}

/* the coordinate size a lookup expects (without the shadow reference) */
static int coord_size(struct texinfo ti) { return ti.dims + ti.layer; }

/* texture(), textureLod(), textureProj(), textureGrad(), with or without Offset */
static struct val texture_call(struct gen *g, const char *fn, struct val *a, int n, int proj, int lod, int grad, int offset, int legacy)
{
    if (n < 2 || a[0].file != F_SAMP || a[0].t.k != GK_SAMPLER) ERR("%s(): the first argument must be a sampler", fn);
    enum zgl_type st = a[0].t.sampler;
    struct texinfo ti = tex_info(st);
    if (ti.ms || ti.buffer) ERR("%s(): not for this sampler (use texelFetch)", fn);
    struct val p = fl(g, a[1], fn);
    int need = coord_size(ti) + ti.shadow;
    if (ti.shadow && ti.dims == 1 && !ti.layer) need = 3;            /* sampler1DShadow: (s, unused, ref) */
    int k = 2;
    struct val lodv, dx, dy, bias;
    int has_bias = 0;
    if (lod) { if (k >= n) ERR("%s(): lod missing", fn); lodv = fl(g, a[k++], fn); }
    if (grad) { if (k + 1 >= n) ERR("%s(): gradients missing", fn); dx = fl(g, a[k++], fn); dy = fl(g, a[k++], fn); }
    char offs[64]; const char *offp = NULL;
    if (offset) { if (k >= n) ERR("%s(): offset missing", fn); offset_str(g, a[k++], offs, sizeof offs, ti.dims); offp = offs; }
    if (!lod && !grad && k < n) { bias = fl(g, a[k++], fn); has_bias = 1; }
    if (k != n) ERR("%s(): too many arguments", fn);
    if (proj) {
        /* the last component divides: TXP wants it in .w */
        int pn = nc(p);
        if (pn != need + 1 && !(pn == 4 && need < 4)) ERR("%s(): wrong coordinate size", fn);
        struct val q = v_comp(p, pn - 1);
        int idx[4] = { 0, 1, 2, 3 };
        struct val head = v_swizzle(p, idx, need);
        if (lod || grad || has_bias) {
            /* divide here and do an ordinary lookup */
            struct val pp = v_binary(g, "/", head, q);
            if (ti.shadow) { /* the reference is not divided by GLSL either... it is: keep it divided */ }
            p = pp;
            proj = 0;
        } else {
            struct val r = pack(g, head, 3, q, GK_FLOAT);
            struct val srcs[1] = { r };
            return tex_result(g, tex_emit(g, "TXP", st, a[0], srcs, 1, offp), st, legacy);
        }
    }
    if (nc(p) != need) ERR("%s(): coordinates need %d components, %d given", fn, need, nc(p));
    if (lod) {
        if (need == 4) ERR("%s(): explicit lod with this sampler is not supported", fn);
        struct val srcs[1] = { pack(g, p, 3, lodv, GK_FLOAT) };
        return tex_result(g, tex_emit(g, "TXL", st, a[0], srcs, 1, offp), st, legacy);
    }
    if (grad) {
        struct val srcs[3] = { pack(g, p, -1, p, GK_FLOAT), dx, dy };
        return tex_result(g, tex_emit(g, "TXD", st, a[0], srcs, 3, offp), st, legacy);
    }
    if (has_bias) {
        if (need == 4) ERR("%s(): a bias with this sampler is not supported", fn);
        struct val srcs[1] = { pack(g, p, 3, bias, GK_FLOAT) };
        return tex_result(g, tex_emit(g, "TXB", st, a[0], srcs, 1, offp), st, legacy);
    }
    struct val srcs[1] = { nc(p) == 4 ? p : pack(g, p, -1, p, GK_FLOAT) };
    return tex_result(g, tex_emit(g, "TEX", st, a[0], srcs, 1, offp), st, legacy);
}

static struct val texel_fetch(struct gen *g, const char *fn, struct val *a, int n, int offset)
{
    if (n < 2 || a[0].file != F_SAMP || a[0].t.k != GK_SAMPLER) ERR("%s(): the first argument must be a sampler", fn);
    enum zgl_type st = a[0].t.sampler;
    struct texinfo ti = tex_info(st);
    if (ti.shadow || ti.cube) ERR("%s(): not for this sampler", fn);
    struct val p = a[1];
    if (!ty_is_vector(p.t) || (p.t.k != GK_INT)) ERR("%s(): coordinates must be integers", fn);
    if (nc(p) != coord_size(ti)) ERR("%s(): wrong coordinate size", fn);
    struct val extra = v_iimm(g, 0);
    int k = 2;
    if (!ti.buffer) { if (k >= n) ERR("%s(): %s missing", fn, ti.ms ? "sample" : "lod"); extra = a[k++]; }
    char offs[64]; const char *offp = NULL;
    if (offset) { if (k >= n) ERR("%s(): offset missing", fn); offset_str(g, a[k++], offs, sizeof offs, ti.dims); offp = offs; }
    if (k != n) ERR("%s(): too many arguments", fn);
    if (!ty_is_scalar(extra.t) || (extra.t.k != GK_INT && extra.t.k != GK_UINT)) ERR("%s(): lod must be an int", fn);
    struct val srcs[1] = { pack(g, p, 3, extra, GK_INT) };
    return tex_result(g, tex_emit(g, "TXF", st, a[0], srcs, 1, offp), st, 0);
}

static struct val texture_size(struct gen *g, struct val *a, int n)
{
    if (n < 1 || a[0].file != F_SAMP || a[0].t.k != GK_SAMPLER) ERR("textureSize(): the first argument must be a sampler");
    enum zgl_type st = a[0].t.sampler;
    struct texinfo ti = tex_info(st);
    struct val lodv = v_iimm(g, 0);
    if (!ti.buffer && !ti.ms && !ti.rect) { if (n != 2) ERR("textureSize(): needs a lod"); lodv = a[1]; }
    else if (n > 2) ERR("textureSize(): too many arguments");   /* a lod here is ignored, as drivers do */
    int dims = ti.cube ? 2 : ti.dims;
    dims += ti.layer;
    struct val srcs[1] = { pack(g, v_convert(g, lodv, GK_INT), -1, lodv, GK_INT) };
    /* only the size components: writing .w would ask the host for textureQueryLevels */
    struct val r = tex_emit_n(g, "TXQ", st, a[0], srcs, 1, NULL, dims);
    r.t = ty_vec(GK_INT, 4);
    int idx[4] = { 0, 1, 2, 3 };
    return v_swizzle(r, idx, dims);
}

/* ---- the table -------------------------------------------------------------------------- */

static const char *const names[] = {
    "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
    "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt",
    "abs", "sign", "floor", "trunc", "round", "roundEven", "ceil", "fract", "mod", "modf", "min", "max", "clamp", "mix",
    "step", "smoothstep", "isnan", "isinf", "floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat",
    "length", "distance", "dot", "cross", "normalize", "faceforward", "reflect", "refract",
    "matrixCompMult", "outerProduct", "transpose", "determinant", "inverse",
    "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual", "any", "all", "not",
    "texture", "textureProj", "textureLod", "textureOffset", "texelFetch", "texelFetchOffset", "textureProjOffset",
    "textureLodOffset", "textureProjLod", "textureProjLodOffset", "textureGrad", "textureGradOffset", "textureProjGrad",
    "textureProjGradOffset", "textureSize",
    "texture1D", "texture1DProj", "texture1DLod", "texture1DProjLod", "texture2D", "texture2DProj", "texture2DLod",
    "texture2DProjLod", "texture3D", "texture3DProj", "texture3DLod", "texture3DProjLod", "textureCube", "textureCubeLod",
    "texture2DRect", "texture2DRectProj", "shadow1D", "shadow2D", "shadow1DProj", "shadow2DProj", "shadow1DLod", "shadow2DLod",
    "shadow1DProjLod", "shadow2DProjLod", "texture2DLodEXT", "texture2DProjLodEXT", "textureCubeLodEXT", "texture2DGradEXT",
    "dFdx", "dFdy", "fwidth", "noise1", "noise2", "noise3", "noise4",
    NULL
};

int is_builtin_func(const char *name)
{
    for (int i = 0; names[i]; i++) if (!strcmp(names[i], name)) return 1;
    return 0;
}

#define NARGS(k) do { if (n != (k)) ERR("%s() takes %d argument(s), %d given", name, (k), n); } while (0)
#define IS(s) (!strcmp(name, s))

static struct val minmax(struct gen *g, const char *name, struct val a, struct val b, int is_max)
{
    if (!ty_is_vector(a.t) || !ty_is_vector(b.t)) ERR("%s(): wrong argument types", name);
    enum gk k = a.t.k == GK_FLOAT || b.t.k == GK_FLOAT ? GK_FLOAT : a.t.k == GK_UINT ? GK_UINT : GK_INT;
    if (a.t.k == GK_BOOL || b.t.k == GK_BOOL) ERR("%s(): not for bool", name);
    a = v_convert(g, a, k); b = v_convert(g, b, k);
    if (nc(b) == 1) b = v_bcast(b, nc(a));
    if (nc(a) != nc(b)) ERR("%s(): sizes differ", name);
    const char *op = k == GK_FLOAT ? (is_max ? "MAX" : "MIN") : k == GK_INT ? (is_max ? "IMAX" : "IMIN") : (is_max ? "UMAX" : "UMIN");
    if (a.is_const && b.is_const) {
        uint32_t cv[4];
        for (int i = 0; i < nc(a); i++) {
            int gt;
            if (k == GK_FLOAT) gt = cv_f(&a, i) > cv_f(&b, i);
            else if (k == GK_INT) gt = cv_i(&a, i) > cv_i(&b, i);
            else gt = a.cv[i] > b.cv[i];
            cv[i] = (gt == is_max) ? a.cv[i] : b.cv[i];
        }
        return v_imm(g, a.t, cv);
    }
    return op2(g, op, a.t, a, b);
}

static struct val relational(struct gen *g, const char *name, struct val a, struct val b, const char *op)
{
    if (!ty_is_vector(a.t) || !ty_is_vector(b.t) || nc(a) != nc(b) || nc(a) < 2) ERR("%s(): needs two vectors of the same size", name);
    if (a.t.k != b.t.k) { if (a.t.k == GK_FLOAT || b.t.k == GK_FLOAT) { a = v_convert(g, a, GK_FLOAT); b = v_convert(g, b, GK_FLOAT); } else ERR("%s(): types differ", name); }
    enum gk k = a.t.k;
    struct gtype bt = ty_vec(GK_BOOL, nc(a));
    int f = k == GK_FLOAT, i = k == GK_INT;
    if (k == GK_BOOL && strcmp(op, "==") && strcmp(op, "!=")) ERR("%s(): not for bool vectors", name);
    if (!strcmp(op, "<")) return op2(g, f ? "FSLT" : i ? "ISLT" : "USLT", bt, a, b);
    if (!strcmp(op, ">=")) return op2(g, f ? "FSGE" : i ? "ISGE" : "USGE", bt, a, b);
    if (!strcmp(op, ">")) return op2(g, f ? "FSLT" : i ? "ISLT" : "USLT", bt, b, a);
    if (!strcmp(op, "<=")) return op2(g, f ? "FSGE" : i ? "ISGE" : "USGE", bt, b, a);
    if (!strcmp(op, "==")) return op2(g, f ? "FSEQ" : "USEQ", bt, a, b);
    return op2(g, f ? "FSNE" : "USNE", bt, a, b);
}

int call_builtin(struct gen *g, const char *name, struct val *a, int n, struct val *out)
{
    if (!is_builtin_func(name)) return -1;
    struct val r;
    /* ---- angles and trigonometry */
    if (IS("radians")) { NARGS(1); r = mulf(g, fl(g, a[0], name), 0.0174532925f); goto done; }
    if (IS("degrees")) { NARGS(1); r = mulf(g, fl(g, a[0], name), 57.2957795f); goto done; }
    if (IS("sin")) { NARGS(1); r = scalar_op1(g, "SIN", fl(g, a[0], name)); goto done; }
    if (IS("cos")) { NARGS(1); r = scalar_op1(g, "COS", fl(g, a[0], name)); goto done; }
    if (IS("tan")) { NARGS(1); struct val x = fl(g, a[0], name); r = v_binary(g, "/", scalar_op1(g, "SIN", x), scalar_op1(g, "COS", x)); goto done; }
    if (IS("atan")) {
        if (n == 2) { struct val y = fl(g, a[0], name), x = fl(g, a[1], name); if (nc(y) != nc(x)) ERR("atan(): sizes differ"); r = atan2v(g, y, x); goto done; }
        NARGS(1); r = atan1(g, fl(g, a[0], name)); goto done;
    }
    if (IS("asin") || IS("acos")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        struct val c = scalar_op1(g, "SQRT", v_binary(g, "-", v_bcast(v_fimm(g, 1), nc(x)), v_binary(g, "*", x, x)));
        r = IS("asin") ? atan2v(g, x, c) : atan2v(g, c, x);
        goto done;
    }
    if (IS("sinh") || IS("cosh")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        struct val e = expv(g, x), ei = scalar_op1(g, "RCP", e);
        r = mulf(g, v_binary(g, IS("sinh") ? "-" : "+", e, ei), 0.5f);
        goto done;
    }
    if (IS("tanh")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        /* clamp so e^2x stays finite: tanh is +-1 there anyway */
        x = op2(g, "MAX", x.t, op2(g, "MIN", x.t, x, v_bcast(v_fimm(g, 20), nc(x))), v_bcast(v_fimm(g, -20), nc(x)));
        struct val e = expv(g, mulf(g, x, 2));
        r = v_binary(g, "/", addf(g, e, -1), addf(g, e, 1));
        goto done;
    }
    if (IS("asinh")) { NARGS(1); struct val x = fl(g, a[0], name); r = logv(g, v_binary(g, "+", x, scalar_op1(g, "SQRT", addf(g, v_binary(g, "*", x, x), 1)))); goto done; }
    if (IS("acosh")) { NARGS(1); struct val x = fl(g, a[0], name); r = logv(g, v_binary(g, "+", x, scalar_op1(g, "SQRT", addf(g, v_binary(g, "*", x, x), -1)))); goto done; }
    if (IS("atanh")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        struct val one = v_bcast(v_fimm(g, 1), nc(x));
        r = mulf(g, logv(g, v_binary(g, "/", v_binary(g, "+", one, x), v_binary(g, "-", one, x))), 0.5f);
        goto done;
    }
    /* ---- exponentials */
    if (IS("pow")) { NARGS(2); r = scalar_op2(g, "POW", fl(g, a[0], name), fl(g, a[1], name)); goto done; }
    if (IS("exp")) { NARGS(1); r = expv(g, fl(g, a[0], name)); goto done; }
    if (IS("log")) { NARGS(1); r = logv(g, fl(g, a[0], name)); goto done; }
    if (IS("exp2")) { NARGS(1); r = scalar_op1(g, "EX2", fl(g, a[0], name)); goto done; }
    if (IS("log2")) { NARGS(1); r = scalar_op1(g, "LG2", fl(g, a[0], name)); goto done; }
    if (IS("sqrt")) { NARGS(1); r = scalar_op1(g, "SQRT", fl(g, a[0], name)); goto done; }
    if (IS("inversesqrt")) { NARGS(1); r = scalar_op1(g, "RSQ", fl(g, a[0], name)); goto done; }
    /* ---- common */
    if (IS("abs")) {
        NARGS(1);
        if (is_vec_kind(a[0], GK_INT)) { r = op1(g, "IABS", a[0].t, a[0]); goto done; }
        r = absf(g, fl(g, a[0], name));
        goto done;
    }
    if (IS("sign")) {
        NARGS(1);
        if (is_vec_kind(a[0], GK_INT)) { r = op1(g, "ISSG", a[0].t, a[0]); goto done; }
        struct val x = fl(g, a[0], name);
        r = op1(g, "SSG", x.t, x);
        goto done;
    }
    if (IS("floor")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "FLR", x.t, x); goto done; }
    if (IS("trunc")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "TRUNC", x.t, x); goto done; }
    if (IS("round") || IS("roundEven")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "ROUND", x.t, x); goto done; }
    if (IS("ceil")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "CEIL", x.t, x); goto done; }
    if (IS("fract")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "FRC", x.t, x); goto done; }
    if (IS("mod")) {
        NARGS(2);
        struct val x = fl(g, a[0], name), y = fl(g, a[1], name);
        y = v_bcast(y, nc(x));
        struct val q = op1(g, "FLR", x.t, v_binary(g, "/", x, y));
        r = v_binary(g, "-", x, v_binary(g, "*", y, q));
        goto done;
    }
    if (IS("modf")) {
        NARGS(2);
        struct val x = fl(g, a[0], name);
        struct val i = op1(g, "TRUNC", x.t, x);
        if (!a[1].lv || !ty_eq(a[1].t, x.t)) ERR("modf(): the second argument must be a variable of the same type");
        v_store(g, a[1], i);
        r = v_binary(g, "-", x, i);
        goto done;
    }
    if (IS("min")) { NARGS(2); r = minmax(g, name, a[0], a[1], 0); goto done; }
    if (IS("max")) { NARGS(2); r = minmax(g, name, a[0], a[1], 1); goto done; }
    if (IS("clamp")) { NARGS(3); r = minmax(g, name, minmax(g, name, a[0], a[1], 1), a[2], 0); goto done; }
    if (IS("mix")) {
        NARGS(3);
        if (ty_is_vector(a[2].t) && a[2].t.k == GK_BOOL) {
            if (!ty_eq(a[0].t, a[1].t) || nc(a[2]) != nc(a[0])) ERR("mix(): types differ");
            r = op3(g, "UCMP", a[0].t, a[2], a[1], a[0]);
            goto done;
        }
        struct val x = fl(g, a[0], name), y = fl(g, a[1], name), t = fl(g, a[2], name);
        if (nc(x) != nc(y)) ERR("mix(): sizes differ");
        r = op3(g, "LRP", x.t, v_bcast(t, nc(x)), y, x);
        goto done;
    }
    if (IS("step")) {
        NARGS(2);
        struct val e = fl(g, a[0], name), x = fl(g, a[1], name);
        e = v_bcast(e, nc(x));
        r = op2(g, "SGE", x.t, x, e);
        goto done;
    }
    if (IS("smoothstep")) {
        NARGS(3);
        struct val e0 = fl(g, a[0], name), e1 = fl(g, a[1], name), x = fl(g, a[2], name);
        int m = nc(x);
        e0 = v_bcast(e0, m); e1 = v_bcast(e1, m);
        struct val t = v_binary(g, "/", v_binary(g, "-", x, e0), v_binary(g, "-", e1, e0));
        t = op2(g, "MIN", t.t, op2(g, "MAX", t.t, t, v_bcast(v_fimm(g, 0), m)), v_bcast(v_fimm(g, 1), m));
        struct val q = op3(g, "MAD", t.t, t, v_bcast(v_fimm(g, -2), m), v_bcast(v_fimm(g, 3), m));
        r = v_binary(g, "*", v_binary(g, "*", t, t), q);
        goto done;
    }
    if (IS("isnan") || IS("isinf")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        int m = nc(x);
        struct val bits = x; bits.t = ty_vec(GK_UINT, m);
        struct val mag = op2(g, "AND", bits.t, bits, v_bcast(v_uimm(g, 0x7FFFFFFFu), m));
        r = IS("isnan") ? op2(g, "USLT", ty_vec(GK_BOOL, m), v_bcast(v_uimm(g, 0x7F800000u), m), mag)
                        : op2(g, "USEQ", ty_vec(GK_BOOL, m), mag, v_bcast(v_uimm(g, 0x7F800000u), m));
        goto done;
    }
    if (IS("floatBitsToInt") || IS("floatBitsToUint")) {
        NARGS(1);
        if (!is_vec_kind(a[0], GK_FLOAT)) ERR("%s(): needs a float", name);
        r = a[0]; r.t.k = IS("floatBitsToInt") ? GK_INT : GK_UINT; r.lv = 0;
        if (r.is_const) r = v_imm(g, r.t, r.cv);
        goto done;
    }
    if (IS("intBitsToFloat") || IS("uintBitsToFloat")) {
        NARGS(1);
        if (!is_vec_kind(a[0], IS("intBitsToFloat") ? GK_INT : GK_UINT)) ERR("%s(): wrong argument type", name);
        r = a[0]; r.t.k = GK_FLOAT; r.lv = 0;
        if (r.is_const) r = v_imm(g, r.t, r.cv);
        goto done;
    }
    /* ---- geometry */
    if (IS("dot")) { NARGS(2); r = dot(g, fl(g, a[0], name), fl(g, a[1], name)); goto done; }
    if (IS("length")) { NARGS(1); struct val x = fl(g, a[0], name); r = nc(x) == 1 ? absf(g, x) : scalar_op1(g, "SQRT", dot(g, x, x)); goto done; }
    if (IS("distance")) { NARGS(2); struct val d = v_binary(g, "-", fl(g, a[0], name), fl(g, a[1], name)); r = nc(d) == 1 ? absf(g, d) : scalar_op1(g, "SQRT", dot(g, d, d)); goto done; }
    if (IS("normalize")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        if (nc(x) == 1) { r = op1(g, "SSG", x.t, x); goto done; }
        r = v_binary(g, "*", x, v_bcast(scalar_op1(g, "RSQ", dot(g, x, x)), nc(x)));
        goto done;
    }
    if (IS("cross")) {
        NARGS(2);
        struct val x = fl(g, a[0], name), y = fl(g, a[1], name);
        if (nc(x) != 3 || nc(y) != 3) ERR("cross(): needs vec3s");
        static const int yzx[3] = { 1, 2, 0 }, zxy[3] = { 2, 0, 1 };
        struct val t = v_binary(g, "*", v_swizzle(x, zxy, 3), v_swizzle(y, yzx, 3));
        t = v_direct(g, t); t.neg = 1;
        r = op3(g, "MAD", vf(3), v_swizzle(x, yzx, 3), v_swizzle(y, zxy, 3), t);
        goto done;
    }
    if (IS("faceforward")) {
        NARGS(3);
        struct val nn = fl(g, a[0], name), i = fl(g, a[1], name), nr = fl(g, a[2], name);
        struct val d = dot(g, nr, i);
        struct val neg = v_direct(g, nn); neg.neg = 1;
        r = v_select(g, op2(g, "FSLT", ty_scalar(GK_BOOL), d, v_fimm(g, 0)), nn, op1(g, "MOV", nn.t, neg));
        goto done;
    }
    if (IS("reflect")) {
        NARGS(2);
        struct val i = fl(g, a[0], name), nn = fl(g, a[1], name);
        struct val k = mulf(g, dot(g, nn, i), -2);
        r = op3(g, "MAD", i.t, nn, v_bcast(k, nc(i)), i);
        goto done;
    }
    if (IS("refract")) {
        NARGS(3);
        struct val i = fl(g, a[0], name), nn = fl(g, a[1], name), eta = fl(g, a[2], name);
        struct val d = dot(g, nn, i);
        struct val k = v_binary(g, "-", v_fimm(g, 1), v_binary(g, "*", v_binary(g, "*", eta, eta), v_binary(g, "-", v_fimm(g, 1), v_binary(g, "*", d, d))));
        struct val s = v_binary(g, "+", v_binary(g, "*", eta, d), scalar_op1(g, "SQRT", op2(g, "MAX", k.t, k, v_fimm(g, 0))));
        struct val v = v_binary(g, "-", v_binary(g, "*", v_bcast(eta, nc(i)), i), v_binary(g, "*", v_bcast(s, nc(i)), nn));
        r = v_select(g, op2(g, "FSLT", ty_scalar(GK_BOOL), k, v_fimm(g, 0)), v_bcast(v_fimm(g, 0), nc(i)), v);
        goto done;
    }
    /* ---- matrices */
    if (IS("matrixCompMult")) {
        NARGS(2);
        if (!ty_is_matrix(a[0].t) || !ty_eq(a[0].t, a[1].t)) ERR("matrixCompMult(): needs two matrices of the same size");
        struct val m0 = v_direct(g, a[0]), m1 = v_direct(g, a[1]);
        r = v_temp(g, m0.t);
        for (int c = 0; c < m0.t.cols; c++) v_store(g, v_col(g, r, c), op2(g, "MUL", vf(m0.t.vec), v_col(g, m0, c), v_col(g, m1, c)));
        r.lv = 0;
        goto done;
    }
    if (IS("outerProduct")) {
        NARGS(2);
        struct val c = fl(g, a[0], name), rr = fl(g, a[1], name);
        if (nc(c) < 2 || nc(rr) < 2) ERR("outerProduct(): needs vectors");
        r = v_temp(g, ty_mat(nc(rr), nc(c)));
        for (int j = 0; j < nc(rr); j++) v_store(g, v_col(g, r, j), op2(g, "MUL", vf(nc(c)), c, v_bcast(v_comp(rr, j), nc(c))));
        r.lv = 0;
        goto done;
    }
    if (IS("transpose")) {
        NARGS(1);
        if (!ty_is_matrix(a[0].t)) ERR("transpose(): needs a matrix");
        struct val m = v_direct(g, a[0]);
        r = v_temp(g, ty_mat(m.t.vec, m.t.cols));
        for (int c = 0; c < m.t.vec; c++)
            for (int k = 0; k < m.t.cols; k++) { struct val d = v_comp(v_col(g, r, c), k); d.lv = 1; v_store(g, d, m_el(g, m, k, c)); }
        r.lv = 0;
        goto done;
    }
    if (IS("determinant")) { NARGS(1); if (!ty_is_matrix(a[0].t) || a[0].t.cols != a[0].t.vec) ERR("determinant(): needs a square matrix"); r = determinant(g, v_direct(g, a[0])); goto done; }
    if (IS("inverse")) { NARGS(1); if (!ty_is_matrix(a[0].t) || a[0].t.cols != a[0].t.vec) ERR("inverse(): needs a square matrix"); r = inverse(g, a[0]); goto done; }
    /* ---- vector relations */
    if (IS("lessThan")) { NARGS(2); r = relational(g, name, a[0], a[1], "<"); goto done; }
    if (IS("lessThanEqual")) { NARGS(2); r = relational(g, name, a[0], a[1], "<="); goto done; }
    if (IS("greaterThan")) { NARGS(2); r = relational(g, name, a[0], a[1], ">"); goto done; }
    if (IS("greaterThanEqual")) { NARGS(2); r = relational(g, name, a[0], a[1], ">="); goto done; }
    if (IS("equal")) { NARGS(2); r = relational(g, name, a[0], a[1], "=="); goto done; }
    if (IS("notEqual")) { NARGS(2); r = relational(g, name, a[0], a[1], "!="); goto done; }
    if (IS("any") || IS("all")) {
        NARGS(1);
        if (!is_vec_kind(a[0], GK_BOOL) || nc(a[0]) < 2) ERR("%s(): needs a bool vector", name);
        r = v_comp(a[0], 0);
        for (int i = 1; i < nc(a[0]); i++) r = op2(g, IS("any") ? "OR" : "AND", ty_scalar(GK_BOOL), r, v_comp(a[0], i));
        goto done;
    }
    if (IS("not")) { NARGS(1); if (!is_vec_kind(a[0], GK_BOOL) || nc(a[0]) < 2) ERR("not(): needs a bool vector"); r = v_not(g, a[0]); goto done; }
    /* ---- textures */
    if (IS("textureSize")) { r = texture_size(g, a, n); goto done; }
    if (IS("texelFetch")) { r = texel_fetch(g, name, a, n, 0); goto done; }
    if (IS("texelFetchOffset")) { r = texel_fetch(g, name, a, n, 1); goto done; }
    if (IS("texture")) { r = texture_call(g, name, a, n, 0, 0, 0, 0, 0); goto done; }
    if (IS("textureProj")) { r = texture_call(g, name, a, n, 1, 0, 0, 0, 0); goto done; }
    if (IS("textureLod")) { r = texture_call(g, name, a, n, 0, 1, 0, 0, 0); goto done; }
    if (IS("textureOffset")) { r = texture_call(g, name, a, n, 0, 0, 0, 1, 0); goto done; }
    if (IS("textureProjOffset")) { r = texture_call(g, name, a, n, 1, 0, 0, 1, 0); goto done; }
    if (IS("textureLodOffset")) { r = texture_call(g, name, a, n, 0, 1, 0, 1, 0); goto done; }
    if (IS("textureProjLod")) { r = texture_call(g, name, a, n, 1, 1, 0, 0, 0); goto done; }
    if (IS("textureProjLodOffset")) { r = texture_call(g, name, a, n, 1, 1, 0, 1, 0); goto done; }
    if (IS("textureGrad") || IS("texture2DGradEXT")) { r = texture_call(g, name, a, n, 0, 0, 1, 0, 0); goto done; }
    if (IS("textureGradOffset")) { r = texture_call(g, name, a, n, 0, 0, 1, 1, 0); goto done; }
    if (IS("textureProjGrad")) { r = texture_call(g, name, a, n, 1, 0, 1, 0, 0); goto done; }
    if (IS("textureProjGradOffset")) { r = texture_call(g, name, a, n, 1, 0, 1, 1, 0); goto done; }
    if (!strncmp(name, "texture", 7) || !strncmp(name, "shadow", 6)) {
        /* the GLSL 1.x names: the sampler type must match the name; shadow* returns a vec4 */
        int proj = strstr(name, "Proj") != NULL, lod = strstr(name, "Lod") != NULL;
        int legacy_shadow = !strncmp(name, "shadow", 6);
        if (n < 1 || a[0].t.k != GK_SAMPLER) ERR("%s(): the first argument must be a sampler", name);
        r = texture_call(g, name, a, n, proj, lod, 0, 0, legacy_shadow);
        goto done;
    }
    /* ---- derivatives and noise */
    if (IS("dFdx")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "DDX", x.t, x); goto done; }
    if (IS("dFdy")) { NARGS(1); struct val x = fl(g, a[0], name); r = op1(g, "DDY", x.t, x); goto done; }
    if (IS("fwidth")) {
        NARGS(1);
        struct val x = fl(g, a[0], name);
        r = v_binary(g, "+", absf(g, op1(g, "DDX", x.t, x)), absf(g, op1(g, "DDY", x.t, x)));
        goto done;
    }
    if (!strncmp(name, "noise", 5)) { NARGS(1); r = v_bcast(v_fimm(g, 0), name[5] - '0'); goto done; }
    return -1;
done:
    r.lv = 0;
    *out = r;
    return 0;
}
